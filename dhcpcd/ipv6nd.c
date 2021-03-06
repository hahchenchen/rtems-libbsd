/*
 * dhcpcd - DHCP client daemon
 * Copyright (c) 2006-2014 Roy Marples <roy@marples.name>
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>

#ifdef __linux__
#  define _LINUX_IN6_H
#  include <linux/ipv6.h>
#endif

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define ELOOP_QUEUE 2
#include "common.h"
#include "dhcpcd.h"
#include "dhcp6.h"
#include "eloop.h"
#include "ipv6.h"
#include "ipv6nd.h"
#include "script.h"

#if defined(LISTEN_DAD) && defined(INET6)
#  warning kernel does not report DAD results to userland
#  warning listening to duplicated addresses on the wire
#endif

/* Debugging Router Solicitations is a lot of spam, so disable it */
//#define DEBUG_RS

#define RTR_SOLICITATION_INTERVAL       4 /* seconds */
#define MAX_RTR_SOLICITATIONS           3 /* times */

#ifndef ND_OPT_RDNSS
#define ND_OPT_RDNSS			25
struct nd_opt_rdnss {           /* RDNSS option RFC 6106 */
	uint8_t		nd_opt_rdnss_type;
	uint8_t		nd_opt_rdnss_len;
	uint16_t	nd_opt_rdnss_reserved;
	uint32_t	nd_opt_rdnss_lifetime;
        /* followed by list of IP prefixes */
} __packed;
#endif

#ifndef ND_OPT_DNSSL
#define ND_OPT_DNSSL			31
struct nd_opt_dnssl {		/* DNSSL option RFC 6106 */
	uint8_t		nd_opt_dnssl_type;
	uint8_t		nd_opt_dnssl_len;
	uint16_t	nd_opt_dnssl_reserved;
	uint32_t	nd_opt_dnssl_lifetime;
	/* followed by list of DNS servers */
} __packed;
#endif

/* Minimal IPv6 MTU */
#ifndef IPV6_MMTU
#define IPV6_MMTU 1280
#endif

#ifndef ND_RA_FLAG_RTPREF_HIGH
#define ND_RA_FLAG_RTPREF_MASK		0x18
#define ND_RA_FLAG_RTPREF_HIGH		0x08
#define ND_RA_FLAG_RTPREF_MEDIUM	0x00
#define ND_RA_FLAG_RTPREF_LOW		0x18
#define ND_RA_FLAG_RTPREF_RSV		0x10
#endif

/* RTPREF_MEDIUM has to be 0! */
#define RTPREF_HIGH	1
#define RTPREF_MEDIUM	0
#define RTPREF_LOW	(-1)
#define RTPREF_RESERVED	(-2)
#define RTPREF_INVALID	(-3)	/* internal */

#define MIN_RANDOM_FACTOR	500				/* millisecs */
#define MAX_RANDOM_FACTOR	1500				/* millisecs */
#define MIN_RANDOM_FACTOR_U	MIN_RANDOM_FACTOR * 1000	/* usecs */
#define MAX_RANDOM_FACTOR_U	MAX_RANDOM_FACTOR * 1000	/* usecs */

#if BYTE_ORDER == BIG_ENDIAN
#define IPV6_ADDR_INT32_ONE     1
#define IPV6_ADDR_INT16_MLL     0xff02
#elif BYTE_ORDER == LITTLE_ENDIAN
#define IPV6_ADDR_INT32_ONE     0x01000000
#define IPV6_ADDR_INT16_MLL     0x02ff
#endif

/* Debugging Neighbor Solicitations is a lot of spam, so disable it */
//#define DEBUG_NS
//

/* Currently, no known kernel allows us to send from the unspecified address
 * which is required for DAD to work. This isn't that much of a problem as
 * the kernel will do DAD for us correctly, however we don't know the exact
 * randomness the kernel applies to the timeouts. So we just follow the same
 * logic and have a little faith.
 * This define is purely for completeness */
// #define IPV6_SEND_DAD

static int sock = -1;
#ifdef IPV6_SEND_DAD
static int unspec_sock = -1;
#endif
static struct sockaddr_in6 allrouters, from;
static struct msghdr sndhdr;
static struct iovec sndiov[2];
static unsigned char *sndbuf;
static struct msghdr rcvhdr;
static struct iovec rcviov[2];
static unsigned char *rcvbuf;
static unsigned char ansbuf[1500];
static char ntopbuf[INET6_ADDRSTRLEN];
static const char *sfrom;
static struct icmp6_filter filt;

struct rahead ipv6_routers = TAILQ_HEAD_INITIALIZER(ipv6_routers);

static void ipv6nd_handledata(void *arg);

/*
 * Android ships buggy ICMP6 filter headers.
 * Supply our own until they fix their shit.
 * References:
 *     https://android-review.googlesource.com/#/c/58438/
 *     http://code.google.com/p/android/issues/original?id=32621&seq=24
 */
#ifdef __ANDROID__
#undef ICMP6_FILTER_WILLPASS
#undef ICMP6_FILTER_WILLBLOCK
#undef ICMP6_FILTER_SETPASS
#undef ICMP6_FILTER_SETBLOCK
#undef ICMP6_FILTER_SETPASSALL
#undef ICMP6_FILTER_SETBLOCKALL
#define ICMP6_FILTER_WILLPASS(type, filterp) \
	((((filterp)->icmp6_filt[(type) >> 5]) & (1 << ((type) & 31))) == 0)
#define ICMP6_FILTER_WILLBLOCK(type, filterp) \
	((((filterp)->icmp6_filt[(type) >> 5]) & (1 << ((type) & 31))) != 0)
#define ICMP6_FILTER_SETPASS(type, filterp) \
	((((filterp)->icmp6_filt[(type) >> 5]) &= ~(1 << ((type) & 31))))
#define ICMP6_FILTER_SETBLOCK(type, filterp) \
	((((filterp)->icmp6_filt[(type) >> 5]) |=  (1 << ((type) & 31))))
#define ICMP6_FILTER_SETPASSALL(filterp) \
	memset(filterp, 0, sizeof(struct icmp6_filter));
#define ICMP6_FILTER_SETBLOCKALL(filterp) \
	memset(filterp, 0xff, sizeof(struct icmp6_filter));
#endif

#if DEBUG_MEMORY
static void
ipv6nd_cleanup(void)
{

	free(sndbuf);
	free(rcvbuf);
}
#endif

static int
ipv6nd_open(void)
{
	int on;
	int len;
#ifdef IPV6_SEND_DAD
	union {
		struct sockaddr sa;
		struct sockaddr_in6 sin;
	} su;
#endif

	sock = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (sock == -1)
		return -1;

	memset(&allrouters, 0, sizeof(allrouters));
	allrouters.sin6_family = AF_INET6;
#ifdef SIN6_LEN
	allrouters.sin6_len = sizeof(allrouters);
#endif
	if (inet_pton(AF_INET6, ALLROUTERS, &allrouters.sin6_addr.s6_addr) != 1)
		goto eexit;
	on = 1;
	if (setsockopt(sock, IPPROTO_IPV6, IPV6_RECVPKTINFO,
		&on, sizeof(on)) == -1)
		goto eexit;

	on = 1;
	if (setsockopt(sock, IPPROTO_IPV6, IPV6_RECVHOPLIMIT,
		&on, sizeof(on)) == -1)
		goto eexit;

	ICMP6_FILTER_SETBLOCKALL(&filt);
	ICMP6_FILTER_SETPASS(ND_ROUTER_ADVERT, &filt);
	if (setsockopt(sock, IPPROTO_ICMPV6, ICMP6_FILTER,
		&filt, sizeof(filt)) == -1)
		goto eexit;

	set_cloexec(sock);
#if DEBUG_MEMORY
	atexit(ipv6nd_cleanup);
#endif

	len = CMSG_SPACE(sizeof(struct in6_pktinfo)) + CMSG_SPACE(sizeof(int));
	sndbuf = calloc(1, len);
	if (sndbuf == NULL)
		goto eexit;
	sndhdr.msg_namelen = sizeof(struct sockaddr_in6);
	sndhdr.msg_iov = sndiov;
	sndhdr.msg_iovlen = 1;
	sndhdr.msg_control = sndbuf;
	sndhdr.msg_controllen = len;
	rcvbuf = calloc(1, len);
	if (rcvbuf == NULL)
		goto eexit;
	rcvhdr.msg_name = &from;
	rcvhdr.msg_namelen = sizeof(from);
	rcvhdr.msg_iov = rcviov;
	rcvhdr.msg_iovlen = 1;
	rcvhdr.msg_control = rcvbuf;
	rcvhdr.msg_controllen = len;
	rcviov[0].iov_base = ansbuf;
	rcviov[0].iov_len = sizeof(ansbuf);
	return sock;

eexit:
	close(sock);
	sock = -1;
	free(sndbuf);
	sndbuf = NULL;
	free(rcvbuf);
	rcvbuf = NULL;
	return -1;
}

static int
ipv6nd_naopen(void)
{
	static int naopen = 0;
	struct icmp6_filter unspec_filt;
#ifdef IPV6_SEND_DAD
	union {
		struct sockaddr sa;
		struct sockaddr_in6 sin;
	} su;
#endif

	if (naopen)
		return sock;

	ICMP6_FILTER_SETBLOCKALL(&unspec_filt);

#ifdef IPV6_SEND_DAD
	/* We send DAD requests from the unspecified address. */
	unspec_sock = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (unspec_sock == -1)
		return -1;
	if (setsockopt(unspec_sock, IPPROTO_ICMPV6, ICMP6_FILTER,
	    &unspec_filt, sizeof(unspec_filt)) == -1)
		goto eexit;
	memset(&su, 0, sizeof(su));
	su.sin.sin6_family = AF_INET6;
#ifdef SIN6_LEN
	su.sin.sin6_len = sizeof(su.sin);
#endif
	if (bind(unspec_sock, &su.sa, sizeof(su.sin)) == -1)
		goto eexit;
#endif

	if (sock == -1) {
		if (ipv6nd_open() == -1)
			goto eexit;
		eloop_event_add(sock, ipv6nd_handledata, NULL);
	}

	ICMP6_FILTER_SETPASS(ND_NEIGHBOR_ADVERT, &filt);
	if (setsockopt(sock, IPPROTO_ICMPV6, ICMP6_FILTER,
	    &filt, sizeof(filt)) == -1)
		goto eexit;

#ifdef LISTEN_DAD
	syslog(LOG_WARNING, "kernel does not report DAD results to userland");
	syslog(LOG_WARNING,
	    "warning listening to duplicated addresses on the wire");
#endif

	naopen = sock;
	return sock;

eexit:
	syslog(LOG_ERR, "%s: %m", __func__);
#ifdef IPV6_SEND_DAD
	close(unspec_sock);
	unspec_sock = -1;
#endif
	return -1;
}

static int
ipv6nd_makersprobe(struct interface *ifp)
{
	struct rs_state *state;
	struct nd_router_solicit *rs;
	struct nd_opt_hdr *nd;

	state = RS_STATE(ifp);
	free(state->rs);
	state->rslen = sizeof(*rs) + ROUNDUP8(ifp->hwlen + 2);
	state->rs = calloc(1, state->rslen);
	if (state->rs == NULL)
		return -1;
	rs = (struct nd_router_solicit *)(void *)state->rs;
	rs->nd_rs_type = ND_ROUTER_SOLICIT;
	rs->nd_rs_code = 0;
	rs->nd_rs_cksum = 0;
	rs->nd_rs_reserved = 0;
	nd = (struct nd_opt_hdr *)(state->rs + sizeof(*rs));
	nd->nd_opt_type = ND_OPT_SOURCE_LINKADDR;
	nd->nd_opt_len = (ROUNDUP8(ifp->hwlen + 2)) >> 3;
	memcpy(nd + 1, ifp->hwaddr, ifp->hwlen);
	return 0;
}

static void
ipv6nd_sendrsprobe(void *arg)
{
	struct interface *ifp = arg;
	struct rs_state *state;
	struct sockaddr_in6 dst;
	struct cmsghdr *cm;
	struct in6_pktinfo pi;
	int hoplimit = HOPLIMIT;

	if (ipv6_linklocal(ifp) == NULL) {
		syslog(LOG_DEBUG,
		    "%s: delaying Router Solicitation for LL address",
		    ifp->name);
		ipv6_addlinklocalcallback(ifp, ipv6nd_sendrsprobe, ifp);
		return;
	}

	dst = allrouters;
	dst.sin6_scope_id = ifp->index;

	state = RS_STATE(ifp);
	sndhdr.msg_name = (caddr_t)&dst;
	sndhdr.msg_iov[0].iov_base = state->rs;
	sndhdr.msg_iov[0].iov_len = state->rslen;

	/* Set the outbound interface */
	cm = CMSG_FIRSTHDR(&sndhdr);
	cm->cmsg_level = IPPROTO_IPV6;
	cm->cmsg_type = IPV6_PKTINFO;
	cm->cmsg_len = CMSG_LEN(sizeof(pi));
	memset(&pi, 0, sizeof(pi));
	pi.ipi6_ifindex = ifp->index;
	memcpy(CMSG_DATA(cm), &pi, sizeof(pi));

	/* Hop limit */
	cm = CMSG_NXTHDR(&sndhdr, cm);
	cm->cmsg_level = IPPROTO_IPV6;
	cm->cmsg_type = IPV6_HOPLIMIT;
	cm->cmsg_len = CMSG_LEN(sizeof(hoplimit));
	memcpy(CMSG_DATA(cm), &hoplimit, sizeof(hoplimit));

	syslog(LOG_DEBUG, "%s: sending Router Solicitation", ifp->name);
	if (sendmsg(sock, &sndhdr, 0) == -1) {
		syslog(LOG_ERR, "%s: %s: sendmsg: %m", ifp->name, __func__);
		ipv6nd_drop(ifp);
		ifp->options->options &= ~(DHCPCD_IPV6 | DHCPCD_IPV6RS);
		return;
	}

	if (state->rsprobes++ < MAX_RTR_SOLICITATIONS)
		eloop_timeout_add_sec(RTR_SOLICITATION_INTERVAL,
		    ipv6nd_sendrsprobe, ifp);
	else
		syslog(LOG_WARNING, "%s: no IPv6 Routers available", ifp->name);
}

static void
ipv6nd_free_opts(struct ra *rap)
{
	struct ra_opt *rao;

	while ((rao = TAILQ_FIRST(&rap->options))) {
		TAILQ_REMOVE(&rap->options, rao, next);
		free(rao->option);
		free(rao);
	}
}

int
ipv6nd_addrexists(const struct ipv6_addr *addr)
{
	struct ra *rap;
	struct ipv6_addr *ap;

	TAILQ_FOREACH(rap, &ipv6_routers, next) {
		TAILQ_FOREACH(ap, &rap->addrs, next) {
			if (addr == NULL) {
				if ((ap->flags &
				    (IPV6_AF_ADDED | IPV6_AF_DADCOMPLETED)) ==
				    (IPV6_AF_ADDED | IPV6_AF_DADCOMPLETED))
					return 1;
			} else if (IN6_ARE_ADDR_EQUAL(&ap->addr, &addr->addr))
				return 1;
		}
	}
	return 0;
}

void ipv6nd_freedrop_ra(struct ra *rap, int drop)
{

	eloop_timeout_delete(NULL, rap->iface);
	eloop_timeout_delete(NULL, rap);
	if (!drop)
		TAILQ_REMOVE(&ipv6_routers, rap, next);
	ipv6_freedrop_addrs(&rap->addrs, drop, NULL);
	ipv6nd_free_opts(rap);
	free(rap->data);
	free(rap->ns);
	free(rap);
}

ssize_t
ipv6nd_free(struct interface *ifp)
{
	struct rs_state *state;
	struct ra *rap, *ran;
	ssize_t n;

	state = RS_STATE(ifp);
	if (state) {
		free(state->rs);
		free(state);
		ifp->if_data[IF_DATA_IPV6ND] = NULL;
	}
	n = 0;
	TAILQ_FOREACH_SAFE(rap, &ipv6_routers, next, ran) {
		if (rap->iface == ifp) {
			ipv6nd_free_ra(rap);
			n++;
		}
	}
	return n;
}

static int
rtpref(struct ra *rap)
{

	switch (rap->flags & ND_RA_FLAG_RTPREF_MASK) {
	case ND_RA_FLAG_RTPREF_HIGH:
		return (RTPREF_HIGH);
	case ND_RA_FLAG_RTPREF_MEDIUM:
	case ND_RA_FLAG_RTPREF_RSV:
		return (RTPREF_MEDIUM);
	case ND_RA_FLAG_RTPREF_LOW:
		return (RTPREF_LOW);
	default:
		syslog(LOG_ERR, "rtpref: impossible RA flag %x", rap->flags);
		return (RTPREF_INVALID);
	}
	/* NOTREACHED */
}

static void
add_router(struct ra *router)
{
	struct ra *rap;

	TAILQ_FOREACH(rap, &ipv6_routers, next) {
		if (router->iface->metric < rap->iface->metric ||
		    (router->iface->metric == rap->iface->metric &&
		    rtpref(router) > rtpref(rap)))
		{
			TAILQ_INSERT_BEFORE(rap, router, next);
			return;
		}
	}
	TAILQ_INSERT_TAIL(&ipv6_routers, router, next);
}

static void
ipv6nd_scriptrun(struct ra *rap)
{
	int hasdns;
	struct ipv6_addr *ap;
	const struct ra_opt *rao;

	/* If all addresses have completed DAD run the script */
	TAILQ_FOREACH(ap, &rap->addrs, next) {
		if ((ap->flags & (IPV6_AF_ONLINK | IPV6_AF_AUTOCONF)) ==
		    (IPV6_AF_ONLINK | IPV6_AF_AUTOCONF))
		{
			if (!(ap->flags & IPV6_AF_DADCOMPLETED) &&
			    ipv6_findaddr(ap->iface, &ap->addr))
				ap->flags |= IPV6_AF_DADCOMPLETED;
			if ((ap->flags & IPV6_AF_DADCOMPLETED) == 0) {
				syslog(LOG_DEBUG,
				    "%s: waiting for Router Advertisement"
				    " DAD to complete",
				    rap->iface->name);
				return;
			}
		}
	}

	/* If we don't require RDNSS then set hasdns = 1 so we fork */
	if (!(rap->iface->options->options & DHCPCD_IPV6RA_REQRDNSS))
		hasdns = 1;
	else {
		hasdns = 0;
		TAILQ_FOREACH(rao, &rap->options, next) {
			if (rao->type == ND_OPT_RDNSS &&
			    rao->option &&
			    timerisset(&rao->expire))
			{
				hasdns = 1;
				break;
			}
		}
	}

	script_runreason(rap->iface, "ROUTERADVERT");
	if (hasdns)
		daemonise();
#if 0
	else if (options & DHCPCD_DAEMONISE &&
	    !(options & DHCPCD_DAEMONISED) && new_data)
		syslog(LOG_WARNING,
		    "%s: did not fork due to an absent"
		    " RDNSS option in the RA",
		    ifp->name);
}
#endif
}

static void
ipv6nd_dadcallback(void *arg)
{
	struct ipv6_addr *ap = arg, *rapap;
	struct interface *ifp;
	struct ra *rap;
	int wascompleted, found;

	wascompleted = (ap->flags & IPV6_AF_DADCOMPLETED);
	ipv6nd_cancelprobeaddr(ap);
	ap->flags |= IPV6_AF_DADCOMPLETED;
	if (ap->flags & IPV6_AF_DUPLICATED)
		/* No idea what how to try and make another address :( */
		syslog(LOG_WARNING, "%s: DAD detected %s",
		    ap->iface->name, ap->saddr);
#ifdef IPV6_SEND_DAD
	else
		ipv6_addaddr(ap);
#endif

	if (!wascompleted) {
		ifp = ap->iface;

		TAILQ_FOREACH(rap, &ipv6_routers, next) {
			if (rap->iface != ifp)
				continue;
			wascompleted = 1;
			found = 0;
			TAILQ_FOREACH(rapap, &rap->addrs, next) {
				if (rapap->flags & IPV6_AF_AUTOCONF &&
				    (rapap->flags & IPV6_AF_DADCOMPLETED) == 0)
				{
					wascompleted = 0;
					break;
				}
				if (rapap == ap)
					found = 1;
			}

			if (wascompleted && found && rap->lifetime) {
				syslog(LOG_DEBUG,
				    "%s: Router Advertisement DAD completed",
				    rap->iface->name);
				ipv6nd_scriptrun(rap);
			}
		}
	}
}

static void
ipv6nd_handlera(struct interface *ifp, struct icmp6_hdr *icp, ssize_t len)
{
	ssize_t l, m, n, olen;
	struct nd_router_advert *nd_ra;
	struct nd_opt_prefix_info *pi;
	struct nd_opt_mtu *mtu;
	struct nd_opt_rdnss *rdnss;
	struct nd_opt_dnssl *dnssl;
	uint32_t lifetime, mtuv;
	uint8_t *p, *op;
	struct in6_addr addr;
	char buf[INET6_ADDRSTRLEN];
	const char *cbp;
	struct ra *rap;
	struct nd_opt_hdr *ndo;
	struct ra_opt *rao;
	struct ipv6_addr *ap;
	char *opt, *tmp;
	struct timeval expire;
	uint8_t new_rap, new_data;

	if ((size_t)len < sizeof(struct nd_router_advert)) {
		syslog(LOG_ERR, "IPv6 RA packet too short from %s", sfrom);
		return;
	}

	if (!IN6_IS_ADDR_LINKLOCAL(&from.sin6_addr)) {
		syslog(LOG_ERR, "RA from non local address %s", sfrom);
		return;
	}

	if (ifp == NULL) {
#ifdef DEBUG_RS
		syslog(LOG_DEBUG, "RA for unexpected interface from %s", sfrom);
#endif
		return;
	}
	if (!(ifp->options->options & DHCPCD_IPV6RS)) {
#ifdef DEBUG_RS
		syslog(LOG_DEBUG, "%s: unexpected RA from %s",
		    ifp->name, sfrom);
#endif
		return;
	}

	/* We could receive a RA before we sent a RS*/
	if (ipv6_linklocal(ifp) == NULL) {
#ifdef DEBUG_RS
		syslog(LOG_DEBUG, "%s: received RA from %s (no link-local)",
		    ifp->name, sfrom);
#endif
		return;
	}

	TAILQ_FOREACH(rap, &ipv6_routers, next) {
		if (ifp == rap->iface &&
		    memcmp(rap->from.s6_addr, from.sin6_addr.s6_addr,
		    sizeof(rap->from.s6_addr)) == 0)
			break;
	}

	nd_ra = (struct nd_router_advert *)icp;
	/* Don't bother doing anything if we don't know about a router
	 * expiring */
	if ((rap == NULL || rap->lifetime == 0)
	    && nd_ra->nd_ra_router_lifetime == 0)
		return;

	/* We don't want to spam the log with the fact we got an RA every
	 * 30 seconds or so, so only spam the log if it's different. */
	if (rap == NULL || (rap->data_len != len ||
	     memcmp(rap->data, (unsigned char *)icp, rap->data_len) != 0))
	{
		if (rap) {
			free(rap->data);
			rap->data_len = 0;
			free(rap->ns);
			rap->ns = NULL;
			rap->nslen = 0;
		}
		new_data = 1;
	} else
		new_data = 0;
	if (new_data || ifp->options->options & DHCPCD_DEBUG)
		syslog(LOG_INFO, "%s: Router Advertisement from %s",
		    ifp->name, sfrom);

	if (rap == NULL) {
		rap = calloc(1, sizeof(*rap));
		if (rap == NULL) {
			syslog(LOG_ERR, "%s: %m", __func__);
			return;
		}
		rap->iface = ifp;
		memcpy(rap->from.s6_addr, from.sin6_addr.s6_addr,
		    sizeof(rap->from.s6_addr));
		strlcpy(rap->sfrom, sfrom, sizeof(rap->sfrom));
		TAILQ_INIT(&rap->addrs);
		TAILQ_INIT(&rap->options);
		new_rap = 1;
	} else
		new_rap = 0;
	if (rap->data_len == 0) {
		rap->data = malloc(len);
		if (rap->data == NULL) {
			syslog(LOG_ERR, "%s: %m", __func__);
			if (new_rap)
				free(rap);
			return;
		}
		memcpy(rap->data, icp, len);
		rap->data_len = len;
	}

	get_monotonic(&rap->received);
	rap->flags = nd_ra->nd_ra_flags_reserved;
	if (new_rap == 0 && rap->lifetime == 0)
		syslog(LOG_WARNING, "%s: %s router available",
		   ifp->name, rap->sfrom);
	rap->lifetime = ntohs(nd_ra->nd_ra_router_lifetime);
	if (nd_ra->nd_ra_reachable) {
		rap->reachable = ntohl(nd_ra->nd_ra_reachable);
		if (rap->reachable > MAX_REACHABLE_TIME)
			rap->reachable = 0;
	}
	if (nd_ra->nd_ra_retransmit)
		rap->retrans = ntohl(nd_ra->nd_ra_retransmit);
	if (rap->lifetime)
		rap->expired = 0;

	len -= sizeof(struct nd_router_advert);
	p = ((uint8_t *)icp) + sizeof(struct nd_router_advert);
	olen = 0;
	lifetime = ~0U;
	for (olen = 0; len > 0; p += olen, len -= olen) {
		if ((size_t)len < sizeof(struct nd_opt_hdr)) {
			syslog(LOG_ERR, "%s: Short option", ifp->name);
			break;
		}
		ndo = (struct nd_opt_hdr *)p;
		olen = ndo->nd_opt_len * 8 ;
		if (olen == 0) {
			syslog(LOG_ERR, "%s: zero length option", ifp->name);
			break;
		}
		if (olen > len) {
			syslog(LOG_ERR,
			    "%s: Option length exceeds message", ifp->name);
			break;
		}

		opt = NULL;
		switch (ndo->nd_opt_type) {
		case ND_OPT_PREFIX_INFORMATION:
			pi = (struct nd_opt_prefix_info *)(void *)ndo;
			if (pi->nd_opt_pi_len != 4) {
				syslog(LOG_ERR,
				    "%s: invalid option len for prefix",
				    ifp->name);
				break;
			}
			if (pi->nd_opt_pi_prefix_len > 128) {
				syslog(LOG_ERR, "%s: invalid prefix len",
				    ifp->name);
				break;
			}
			if (IN6_IS_ADDR_MULTICAST(&pi->nd_opt_pi_prefix) ||
			    IN6_IS_ADDR_LINKLOCAL(&pi->nd_opt_pi_prefix))
			{
				syslog(LOG_ERR,
				    "%s: invalid prefix in RA", ifp->name);
				break;
			}
			if (ntohl(pi->nd_opt_pi_preferred_time) >
			    ntohl(pi->nd_opt_pi_valid_time))
			{
				syslog(LOG_ERR,
				    "%s: pltime > vltime", ifp->name);
				break;
			}
			TAILQ_FOREACH(ap, &rap->addrs, next)
				if (ap->prefix_len ==pi->nd_opt_pi_prefix_len &&
				    memcmp(ap->prefix.s6_addr,
				    pi->nd_opt_pi_prefix.s6_addr,
				    sizeof(ap->prefix.s6_addr)) == 0)
					break;
			if (ap == NULL) {
				if (!(pi->nd_opt_pi_flags_reserved &
				    ND_OPT_PI_FLAG_AUTO) &&
				    !(pi->nd_opt_pi_flags_reserved &
				    ND_OPT_PI_FLAG_ONLINK))
					break;
				ap = calloc(1, sizeof(*ap));
				if (ap == NULL) {
					syslog(LOG_ERR, "%s: %m", __func__);
					break;
				}
				ap->iface = rap->iface;
				ap->flags = IPV6_AF_NEW;
				ap->prefix_len = pi->nd_opt_pi_prefix_len;
				memcpy(ap->prefix.s6_addr,
				   pi->nd_opt_pi_prefix.s6_addr,
				   sizeof(ap->prefix.s6_addr));
				if (pi->nd_opt_pi_flags_reserved &
				    ND_OPT_PI_FLAG_AUTO)
				{
					ap->flags |= IPV6_AF_AUTOCONF;
					ipv6_makeaddr(&ap->addr, ifp,
					    &ap->prefix,
					    pi->nd_opt_pi_prefix_len);
					cbp = inet_ntop(AF_INET6,
					    ap->addr.s6_addr,
					    ntopbuf, INET6_ADDRSTRLEN);
					if (cbp)
						snprintf(ap->saddr,
						    sizeof(ap->saddr),
						    "%s/%d",
						    cbp, ap->prefix_len);
					else
						ap->saddr[0] = '\0';
				} else {
					memset(&ap->addr, 0, sizeof(ap->addr));
					ap->saddr[0] = '\0';
				}
				ap->dadcallback = ipv6nd_dadcallback;
				TAILQ_INSERT_TAIL(&rap->addrs, ap, next);
			}
			if (pi->nd_opt_pi_flags_reserved &
			    ND_OPT_PI_FLAG_ONLINK)
				ap->flags |= IPV6_AF_ONLINK;
			ap->prefix_vltime =
			    ntohl(pi->nd_opt_pi_valid_time);
			ap->prefix_pltime =
			    ntohl(pi->nd_opt_pi_preferred_time);
			ap->nsprobes = 0;
			if (opt) {
				l = strlen(opt);
				tmp = realloc(opt,
					l + strlen(ap->saddr) + 2);
				if (tmp) {
					opt = tmp;
					opt[l] = ' ';
					strcpy(opt + l + 1, ap->saddr);
				}
			} else
				opt = strdup(ap->saddr);
			lifetime = ap->prefix_vltime;
			break;

		case ND_OPT_MTU:
			mtu = (struct nd_opt_mtu *)(void *)p;
			mtuv = ntohl(mtu->nd_opt_mtu_mtu);
			if (mtuv < IPV6_MMTU) {
				syslog(LOG_ERR, "%s: invalid MTU %d",
				    ifp->name, mtuv);
				break;
			}
			rap->mtu = mtuv;
			snprintf(buf, sizeof(buf), "%d", mtuv);
			opt = strdup(buf);
			break;

		case ND_OPT_RDNSS:
			rdnss = (struct nd_opt_rdnss *)p;
			lifetime = ntohl(rdnss->nd_opt_rdnss_lifetime);
			op = (uint8_t *)ndo;
			op += offsetof(struct nd_opt_rdnss,
			    nd_opt_rdnss_lifetime);
			op += sizeof(rdnss->nd_opt_rdnss_lifetime);
			l = 0;
			for (n = ndo->nd_opt_len - 1; n > 1; n -= 2,
			    op += sizeof(addr.s6_addr))
			{
				m = ipv6_printaddr(NULL, 0, op, ifp->name);
				if (m != -1)
					l += m + 1;
			}
			op = (uint8_t *)ndo;
			op += offsetof(struct nd_opt_rdnss,
			    nd_opt_rdnss_lifetime);
			op += sizeof(rdnss->nd_opt_rdnss_lifetime);
			tmp = opt = malloc(l);
			if (opt) {
				for (n = ndo->nd_opt_len - 1; n > 1; n -= 2,
				    op += sizeof(addr.s6_addr))
				{
					m = ipv6_printaddr(tmp, l, op,
					    ifp->name);
					if (m != -1) {
						l -= (m + 1);
						tmp += m;
						*tmp++ = ' ';
					}
				}
				if (tmp != opt)
					(*--tmp) = '\0';
				else
					*opt = '\0';
			}
			break;

		case ND_OPT_DNSSL:
			dnssl = (struct nd_opt_dnssl *)p;
			lifetime = ntohl(dnssl->nd_opt_dnssl_lifetime);
			op = p + offsetof(struct nd_opt_dnssl,
			    nd_opt_dnssl_lifetime);
			op += sizeof(dnssl->nd_opt_dnssl_lifetime);
			n = (dnssl->nd_opt_dnssl_len - 1) * 8;
			l = decode_rfc3397(NULL, 0, n, op);
			if (l < 1) {
				syslog(LOG_ERR, "%s: invalid DNSSL option",
				    ifp->name);
			} else {
				tmp = malloc(l);
				if (tmp) {
					decode_rfc3397(tmp, l, n, op);
					n = print_string(NULL, 0,
					    l - 1, (const uint8_t *)tmp);
					opt = malloc(n);
					if (opt)
						print_string(opt, n,
						    l - 1,
						    (const uint8_t *)tmp);
					free(tmp);
				}
			}
			break;

		default:
			continue;
		}

		if (opt == NULL) {
			syslog(LOG_ERR, "%s: %m", __func__);
			continue;
		}
		TAILQ_FOREACH(rao, &rap->options, next) {
			if (rao->type == ndo->nd_opt_type &&
			    strcmp(rao->option, opt) == 0)
				break;
		}
		if (lifetime == 0) {
			if (rao) {
				TAILQ_REMOVE(&rap->options, rao, next);
				free(rao->option);
				free(rao);
			}
			free(opt);
			continue;
		}

		if (rao == NULL) {
			rao = malloc(sizeof(*rao));
			if (rao == NULL) {
				syslog(LOG_ERR, "%s: %m", __func__);
				continue;
			}
			rao->type = ndo->nd_opt_type;
			rao->option = opt;
			TAILQ_INSERT_TAIL(&rap->options, rao, next);
		} else
			free(opt);
		if (lifetime == ~0U)
			timerclear(&rao->expire);
		else {
			expire.tv_sec = lifetime;
			expire.tv_usec = 0;
			timeradd(&rap->received, &expire, &rao->expire);
		}
	}

	if (new_rap)
		add_router(rap);
	if (options & DHCPCD_TEST) {
		script_runreason(ifp, "TEST");
		goto handle_flag;
	}
	ipv6nd_probeaddrs(&rap->addrs);
	ipv6_buildroutes();

	/* We will get run by the expire function */
	if (rap->lifetime)
		ipv6nd_scriptrun(rap);

	eloop_timeout_delete(NULL, ifp);
	eloop_timeout_delete(NULL, rap); /* reachable timer */

	/* If we're owning the RA then we need to try and ensure the
	 * router is actually reachable */
	if (ifp->options->options & DHCPCD_IPV6RA_OWN ||
	    ifp->options->options & DHCPCD_IPV6RA_OWN_DEFAULT)
	{
		rap->nsprobes = 0;
		if (rap->lifetime)
			ipv6nd_proberouter(rap);
	}

handle_flag:
	if (rap->flags & ND_RA_FLAG_MANAGED) {
		if (rap->lifetime && new_data &&
		    dhcp6_start(ifp, DH6S_INIT) == -1)
			syslog(LOG_ERR, "dhcp6_start: %s: %m", ifp->name);
	} else if (rap->flags & ND_RA_FLAG_OTHER) {
		if (rap->lifetime && new_data &&
		    dhcp6_start(ifp, DH6S_INFORM) == -1)
			syslog(LOG_ERR, "dhcp6_start: %s: %m", ifp->name);
	} else {
		if (rap->lifetime && new_data)
			syslog(LOG_DEBUG, "%s: No DHCPv6 instruction in RA",
			    ifp->name);
		if (options & DHCPCD_TEST)
			exit(EXIT_SUCCESS);
	}

	/* Expire should be called last as the rap object could be destroyed */
	ipv6nd_expirera(ifp);
}

int
ipv6nd_has_ra(const struct interface *ifp)
{
	const struct ra *rap;

	TAILQ_FOREACH(rap, &ipv6_routers, next)
		if (rap->iface == ifp)
			return 1;
	return 0;
}

ssize_t
ipv6nd_env(char **env, const char *prefix, const struct interface *ifp)
{
	ssize_t l;
	size_t len;
	struct timeval now;
	const struct ra *rap;
	const struct ra_opt *rao;
	int i;
	char buffer[32];
	const char *optn;
	char **pref, **mtu, **rdnss, **dnssl, ***var, *new;

	i = 0;
	l = 0;
	get_monotonic(&now);
	TAILQ_FOREACH(rap, &ipv6_routers, next) {
		i++;
		if (rap->iface != ifp)
			continue;
		if (env) {
			snprintf(buffer, sizeof(buffer),
			    "ra%d_from", i);
			if (setvar(&env, prefix, buffer, rap->sfrom) == -1)
				return -1;
		}
		l++;

		pref = mtu = rdnss = dnssl = NULL;
		TAILQ_FOREACH(rao, &rap->options, next) {
			if (rao->option == NULL)
				continue;
			var = NULL;
			switch(rao->type) {
			case ND_OPT_PREFIX_INFORMATION:
				optn = "prefix";
				var = &pref;
				break;
			case ND_OPT_MTU:
				optn = "mtu";
				var = &mtu;
				break;
			case ND_OPT_RDNSS:
				optn = "rdnss";
				var = &rdnss;
				break;
			case ND_OPT_DNSSL:
				optn = "dnssl";
				var = &dnssl;
				break;
			default:
				continue;
			}
			if (*var == NULL) {
				*var = env ? env : &new;
				l++;
			} else if (env) {
				/* With single only options, last one takes
				 * precedence */
				if (rao->type == ND_OPT_MTU) {
					new = strchr(**var, '=');
					if (new == NULL) {
						syslog(LOG_ERR, "new is null");
						continue;
					} else
						new++;
					len = (new - **var) +
					    strlen(rao->option) + 1;
					if (len > strlen(**var))
						new = realloc(**var, len);
					else
						new = **var;
					if (new) {
						**var = new;
						new = strchr(**var, '=');
						if (new)
							strcpy(new + 1,
							    rao->option);
						else
							syslog(LOG_ERR,
							    "new is null");
					}
					continue;
				}
				new = realloc(**var,
				    strlen(**var) + 1 +
				    strlen(rao->option) + 1);
				if (new == NULL)
					return -1;
				**var = new;
				new += strlen(new);
				*new++ = ' ';
				strcpy(new, rao->option);
				continue;
			}
			if (env) {
				snprintf(buffer, sizeof(buffer),
				    "ra%d_%s", i, optn);
				if (setvar(&env, prefix, buffer, rao->option)
				    == -1)
					return -1;
			}
		}
	}

	if (env) {
		if (setvard(&env, prefix, "ra_count", i) == -1)
			return -1;
	}
	l++;
	return l;
}

void
ipv6nd_handleifa(int cmd, const char *ifname,
    const struct in6_addr *addr, int flags)
{
	struct ra *rap;

	TAILQ_FOREACH(rap, &ipv6_routers, next) {
		if (strcmp(rap->iface->name, ifname))
			continue;
		ipv6_handleifa_addrs(cmd, &rap->addrs, addr, flags);
	}
}

void
ipv6nd_expirera(void *arg)
{
	struct interface *ifp;
	struct ra *rap, *ran;
	struct ra_opt *rao, *raon;
	struct timeval now, lt, expire, next;
	int expired, valid;

	ifp = arg;
	get_monotonic(&now);
	expired = 0;
	timerclear(&next);

	TAILQ_FOREACH_SAFE(rap, &ipv6_routers, next, ran) {
		if (rap->iface != ifp)
			continue;
		lt.tv_sec = rap->lifetime;
		lt.tv_usec = 0;
		timeradd(&rap->received, &lt, &expire);
		if (rap->lifetime == 0 || timercmp(&now, &expire, >)) {
			valid = 0;
			if (!rap->expired) {
				syslog(LOG_WARNING,
				    "%s: %s: router expired",
				    ifp->name, rap->sfrom);
				rap->expired = expired = 1;
				ipv6nd_cancelproberouter(rap);
			}
		} else {
			valid = 1;
			timersub(&expire, &now, &lt);
			if (!timerisset(&next) || timercmp(&next, &lt, >))
				next = lt;
		}

		/* Addresses are expired in ipv6ns_probeaddrs
		 * so that DHCPv6 addresses can be removed also. */
		TAILQ_FOREACH_SAFE(rao, &rap->options, next, raon) {
			if (rap->expired) {
				switch(rao->type) {
				case ND_OPT_RDNSS: /* FALLTHROUGH */
				case ND_OPT_DNSSL:
					/* RFC6018 end of section 5.2 states
					 * that if tha RA has a lifetime of 0
					 * then we should expire these
					 * options */
					TAILQ_REMOVE(&rap->options, rao, next);
					expired = 1;
					free(rao->option);
					free(rao);
					continue;
				}
			}
			if (!timerisset(&rao->expire))
				continue;
			if (timercmp(&now, &rao->expire, >)) {
				/* Expired prefixes are logged above */
				if (rao->type != ND_OPT_PREFIX_INFORMATION)
					syslog(LOG_WARNING,
					    "%s: %s: expired option %d",
					    ifp->name, rap->sfrom, rao->type);
				TAILQ_REMOVE(&rap->options, rao, next);
				expired = 1;
				free(rao->option);
				free(rao);
				continue;
			}
			valid = 1;
			timersub(&rao->expire, &now, &lt);
			if (!timerisset(&next) || timercmp(&next, &lt, >))
				next = lt;
		}

		/* No valid lifetimes are left on the RA, so we might
		 * as well punt it. */
		if (!valid && TAILQ_FIRST(&rap->addrs) == NULL)
			ipv6nd_free_ra(rap);
	}

	if (timerisset(&next))
		eloop_timeout_add_tv(&next, ipv6nd_expirera, ifp);
	if (expired) {
		ipv6_buildroutes();
		script_runreason(ifp, "ROUTERADVERT");
	}
}

void
ipv6nd_drop(struct interface *ifp)
{
	struct ra *rap;
	int expired = 0;
	TAILQ_HEAD(rahead, ra) rtrs;

	eloop_timeout_delete(NULL, ifp);
	TAILQ_INIT(&rtrs);
	TAILQ_FOREACH(rap, &ipv6_routers, next) {
		if (rap->iface == ifp) {
			rap->expired = expired = 1;
			TAILQ_REMOVE(&ipv6_routers, rap, next);
			TAILQ_INSERT_TAIL(&rtrs, rap, next);
		}
	}
	if (expired) {
		while ((rap = TAILQ_FIRST(&rtrs))) {
			TAILQ_REMOVE(&rtrs, rap, next);
			ipv6nd_drop_ra(rap);
		}
		ipv6_buildroutes();
		if ((ifp->options->options &
		    (DHCPCD_EXITING | DHCPCD_PERSISTENT)) !=
		    (DHCPCD_EXITING | DHCPCD_PERSISTENT))
			script_runreason(ifp, "ROUTERADVERT");
	}
}
static void
ipv6nd_unreachable(void *arg)
{
	struct ra *rap = arg;
	struct timeval tv;

	/* We could add an unreachable flag and persist the information,
	 * but that is more effort than it's probably worth. */
	syslog(LOG_WARNING, "%s: %s is unreachable, expiring it",
	    rap->iface->name, rap->sfrom);
	rap->expired = 1;
	ipv6_buildroutes();
	script_runreason(rap->iface, "ROUTERADVERT"); /* XXX not RA */

	/* We should still test if it's reachable or not so
	 * incase it comes back to life and it's preferable. */
	if (rap->reachable) {
		ms_to_tv(&tv, rap->reachable);
	} else {
		tv.tv_sec = REACHABLE_TIME;
		tv.tv_usec = 0;
	}
	eloop_timeout_add_tv(&tv, ipv6nd_proberouter, rap);
}

#ifdef LISTEN_DAD
void
ipv6nd_cancelprobeaddr(struct ipv6_addr *ap)
{

	eloop_timeout_delete(ipv6nd_probeaddr, ap);
	if (ap->dadcallback)
		eloop_timeout_delete(ap->dadcallback, ap);
}
#endif

void
ipv6nd_probeaddr(void *arg)
{
	struct ipv6_addr *ap = arg;
#ifdef IPV6_SEND_DAD
	struct nd_neighbor_solicit *ns;
	struct nd_opt_hdr *nd;
	struct sockaddr_in6 dst;
	struct cmsghdr *cm;
	struct in6_pktinfo pi;
	int hoplimit = HOPLIMIT;
#else
#ifdef LISTEN_DAD
	struct timeval tv, rtv;
	struct timeval mtv;
	int i;
#endif
#endif

	if (ap->dadcallback &&
	    ((ap->flags & IPV6_AF_NEW) == 0 ||
	    ap->nsprobes >= ap->iface->options->dadtransmits))
	{
#ifdef IPV6_SEND_DAD
		ap->dadcallback(ap);
#else
		if (!(ap->flags & IPV6_AF_AUTOCONF) ||
		    ap->iface->options->options & DHCPCD_IPV6RA_OWN)
			ipv6_addaddr(ap);
#endif
		return;
	}

	if (ipv6nd_naopen() == -1)
		return;

	ap->flags &= ~IPV6_AF_DADCOMPLETED;

#ifdef IPV6_SEND_DAD
	if (!ap->ns) {
	        ap->nslen = sizeof(*ns) + ROUNDUP8(ap->iface->hwlen + 2);
		ap->ns = calloc(1, ap->nslen);
		if (ap->ns == NULL) {
			syslog(LOG_ERR, "%s: %m", __func__);
			return;
		}
		ns = (struct nd_neighbor_solicit *)(void *)ap->ns;
		ns->nd_ns_type = ND_NEIGHBOR_SOLICIT;
		//ns->nd_ns_cksum = 0;
		//ns->nd_ns_code = 0;
		//ns->nd_ns_reserved = 0;
		ns->nd_ns_target = ap->addr;
		nd = (struct nd_opt_hdr *)(ap->ns + sizeof(*ns));
		nd->nd_opt_type = ND_OPT_SOURCE_LINKADDR;
		nd->nd_opt_len = (ROUNDUP8(ap->iface->hwlen + 2)) >> 3;
		memcpy(nd + 1, ap->iface->hwaddr, ap->iface->hwlen);
	}

	memset(&dst, 0, sizeof(dst));
	dst.sin6_family = AF_INET6;
#ifdef SIN6_LEN
	dst.sin6_len = sizeof(dst);
#endif
	dst.sin6_addr.s6_addr16[0] = IPV6_ADDR_INT16_MLL;
	dst.sin6_addr.s6_addr16[1] = 0;
	dst.sin6_addr.s6_addr32[1] = 0;
	dst.sin6_addr.s6_addr32[2] = IPV6_ADDR_INT32_ONE;
	dst.sin6_addr.s6_addr32[3] = ap->addr.s6_addr32[3];
	dst.sin6_addr.s6_addr[12] = 0xff;

	//memcpy(&dst.sin6_addr, &ap->addr, sizeof(dst.sin6_addr));
	dst.sin6_scope_id = ap->iface->index;

	sndhdr.msg_name = (caddr_t)&dst;
	sndhdr.msg_iov[0].iov_base = ap->ns;
	sndhdr.msg_iov[0].iov_len = ap->nslen;

	/* Set the outbound interface */
	cm = CMSG_FIRSTHDR(&sndhdr);
	cm->cmsg_level = IPPROTO_IPV6;
	cm->cmsg_type = IPV6_PKTINFO;
	cm->cmsg_len = CMSG_LEN(sizeof(pi));
	memset(&pi, 0, sizeof(pi));
	pi.ipi6_ifindex = ap->iface->index;
	memcpy(CMSG_DATA(cm), &pi, sizeof(pi));

	/* Hop limit */
	cm = CMSG_NXTHDR(&sndhdr, cm);
	cm->cmsg_level = IPPROTO_IPV6;
	cm->cmsg_type = IPV6_HOPLIMIT;
	cm->cmsg_len = CMSG_LEN(sizeof(hoplimit));
	memcpy(CMSG_DATA(cm), &hoplimit, sizeof(hoplimit));

#ifdef DEBUG_NS
	syslog(LOG_INFO, "%s: sending IPv6 NS for %s",
	    ap->iface->name, ap->saddr);
	if (ap->dadcallback == NULL)
		syslog(LOG_WARNING, "%s: no callback!", ap->iface->name);
#endif
	if (sendmsg(unspec_sock, &sndhdr, 0) == -1) {
		syslog(LOG_ERR, "%s: %s: sendmsg: %m",
		    ap->iface->name, __func__);
		return;
	}

	if (ap->dadcallback) {
		ms_to_tv(&tv, RETRANS_TIMER);
		ms_to_tv(&rtv, MIN_RANDOM_FACTOR);
		timeradd(&tv, &rtv, &tv);
		rtv.tv_sec = 0;
		rtv.tv_usec = arc4random() %
		    (MAX_RANDOM_FACTOR_U - MIN_RANDOM_FACTOR_U);
		timeradd(&tv, &rtv, &tv);

		eloop_timeout_add_tv(&tv,
		    ++(ap->nsprobes) < ap->iface->options->dadtransmits ?
		    ipv6nd_probeaddr : ap->dadcallback,
		    ap);
	}
#else /* IPV6_SEND_DAD */

	if (!(ap->flags & IPV6_AF_AUTOCONF) ||
	    ap->iface->options->options & DHCPCD_IPV6RA_OWN)
		ipv6_addaddr(ap);

#ifdef LISTEN_DAD
	/* Let the kernel handle DAD.
	 * We don't know the timings, so just wait for the max */
	if (ap->dadcallback) {
		mtv.tv_sec = 0;
		mtv.tv_usec = 0;
		for (i = 0; i < ap->iface->options->dadtransmits; i++) {
			ms_to_tv(&tv, RETRANS_TIMER);
			ms_to_tv(&rtv, MAX_RANDOM_FACTOR);
			timeradd(&tv, &rtv, &tv);
			timeradd(&mtv, &tv, &mtv);
		}
		eloop_timeout_add_tv(&mtv, ap->dadcallback, ap);
	}
#endif
#endif /* IPV6_SEND_DAD */
}

ssize_t
ipv6nd_probeaddrs(struct ipv6_addrhead *addrs)
{
	struct ipv6_addr *ap, *apn;
	ssize_t i;

	i = 0;
	TAILQ_FOREACH_SAFE(ap, addrs, next, apn) {
		if (ap->prefix_vltime == 0) {
			TAILQ_REMOVE(addrs, ap, next);
			if (ap->flags & IPV6_AF_ADDED) {
				syslog(LOG_INFO, "%s: deleting address %s",
				    ap->iface->name, ap->saddr);
				i++;
				if (!IN6_IS_ADDR_UNSPECIFIED(&ap->addr) &&
				    del_address6(ap) == -1 &&
				    errno != EADDRNOTAVAIL && errno != ENXIO)
					syslog(LOG_ERR, "del_address6 %m");
			}
			if (ap->dadcallback)
				eloop_q_timeout_delete(0, NULL,
				    ap->dadcallback);
			free(ap);
		} else if (!IN6_IS_ADDR_UNSPECIFIED(&ap->addr)) {
			ipv6nd_probeaddr(ap);
			if (ap->flags & IPV6_AF_NEW)
				i++;
		}
	}

	return i;
}

void
ipv6nd_proberouter(void *arg)
{
	struct ra *rap = arg;
	struct nd_neighbor_solicit *ns;
	struct nd_opt_hdr *nd;
	struct sockaddr_in6 dst;
	struct cmsghdr *cm;
	struct in6_pktinfo pi;
	int hoplimit = HOPLIMIT;
	struct timeval tv, rtv;

	if (ipv6nd_naopen() == -1)
		return;

	if (!rap->ns) {
	        rap->nslen = sizeof(*ns) + ROUNDUP8(rap->iface->hwlen + 2);
		rap->ns = calloc(1, rap->nslen);
		if (rap->ns == NULL) {
			syslog(LOG_ERR, "%s: %m", __func__);
			return;
		}
		ns = (struct nd_neighbor_solicit *)(void *)rap->ns;
		ns->nd_ns_type = ND_NEIGHBOR_SOLICIT;
		//ns->nd_ns_cksum = 0;
		//ns->nd_ns_code = 0;
		//ns->nd_ns_reserved = 0;
		ns->nd_ns_target = rap->from;
		nd = (struct nd_opt_hdr *)(rap->ns + sizeof(*ns));
		nd->nd_opt_type = ND_OPT_SOURCE_LINKADDR;
		nd->nd_opt_len = (ROUNDUP8(rap->iface->hwlen + 2)) >> 3;
		memcpy(nd + 1, rap->iface->hwaddr, rap->iface->hwlen);
	}

	memset(&dst, 0, sizeof(dst));
	dst.sin6_family = AF_INET6;
#ifdef SIN6_LEN
	dst.sin6_len = sizeof(dst);
#endif
	memcpy(&dst.sin6_addr, &rap->from, sizeof(dst.sin6_addr));
	dst.sin6_scope_id = rap->iface->index;

	sndhdr.msg_name = (caddr_t)&dst;
	sndhdr.msg_iov[0].iov_base = rap->ns;
	sndhdr.msg_iov[0].iov_len = rap->nslen;

	/* Set the outbound interface */
	cm = CMSG_FIRSTHDR(&sndhdr);
	cm->cmsg_level = IPPROTO_IPV6;
	cm->cmsg_type = IPV6_PKTINFO;
	cm->cmsg_len = CMSG_LEN(sizeof(pi));
	memset(&pi, 0, sizeof(pi));
	pi.ipi6_ifindex = rap->iface->index;
	memcpy(CMSG_DATA(cm), &pi, sizeof(pi));

	/* Hop limit */
	cm = CMSG_NXTHDR(&sndhdr, cm);
	cm->cmsg_level = IPPROTO_IPV6;
	cm->cmsg_type = IPV6_HOPLIMIT;
	cm->cmsg_len = CMSG_LEN(sizeof(hoplimit));
	memcpy(CMSG_DATA(cm), &hoplimit, sizeof(hoplimit));

#ifdef DEBUG_NS
	syslog(LOG_INFO, "%s: sending IPv6 NS for %s",
	    rap->iface->name, rap->sfrom);
#endif
	if (sendmsg(sock, &sndhdr, 0) == -1) {
		syslog(LOG_ERR, "%s: %s: sendmsg: %m",
		    rap->iface->name, __func__);
		return;
	}

	ms_to_tv(&tv, rap->retrans == 0 ? RETRANS_TIMER : rap->retrans);
	ms_to_tv(&rtv, MIN_RANDOM_FACTOR);
	timeradd(&tv, &rtv, &tv);
	rtv.tv_sec = 0;
	rtv.tv_usec = arc4random() % (MAX_RANDOM_FACTOR_U -MIN_RANDOM_FACTOR_U);
	timeradd(&tv, &rtv, &tv);
	eloop_timeout_add_tv(&tv, ipv6nd_proberouter, rap);

	if (rap->nsprobes++ == 0)
		eloop_timeout_add_sec(DELAY_FIRST_PROBE_TIME,
		    ipv6nd_unreachable, rap);
}

void
ipv6nd_cancelproberouter(struct ra *rap)
{

	eloop_timeout_delete(ipv6nd_proberouter, rap);
	eloop_timeout_delete(ipv6nd_unreachable, rap);
}

/* ARGSUSED */
static void
ipv6nd_handlena(struct interface *ifp, struct icmp6_hdr *icp, ssize_t len)
{
	struct nd_neighbor_advert *nd_na;
	struct ra *rap;
	int is_router, is_solicited;
#ifdef DEBUG_NS
	int found;
#endif
	struct timeval tv;

#ifdef LISTEN_DAD
	struct dhcp6_state *d6state;
	struct ipv6_addr *ap;
#endif

	if ((size_t)len < sizeof(struct nd_neighbor_advert)) {
		syslog(LOG_ERR, "IPv6 NA packet too short from %s", sfrom);
		return;
	}

	if (ifp == NULL) {
#ifdef DEBUG_NS
		syslog(LOG_DEBUG, "NA for unexpected interface from %s", sfrom);
#endif
		return;
	}

	nd_na = (struct nd_neighbor_advert *)icp;
	is_router = nd_na->nd_na_flags_reserved & ND_NA_FLAG_ROUTER;
	is_solicited = nd_na->nd_na_flags_reserved & ND_NA_FLAG_SOLICITED;

	if (IN6_IS_ADDR_MULTICAST(&nd_na->nd_na_target)) {
		syslog(LOG_ERR, "%s: NA for multicast address from %s",
		    ifp->name, sfrom);
		return;
	}

#ifdef DEBUG_NS
	found = 0;
#endif
	TAILQ_FOREACH(rap, &ipv6_routers, next) {
		if (rap->iface != ifp)
			continue;
		if (memcmp(rap->from.s6_addr, nd_na->nd_na_target.s6_addr,
		    sizeof(rap->from.s6_addr)) == 0)
			break;
#ifdef LISTEN_DAD
		TAILQ_FOREACH(ap, &rap->addrs, next) {
			if (memcmp(ap->addr.s6_addr,
			    nd_na->nd_na_target.s6_addr,
			    sizeof(ap->addr.s6_addr)) == 0)
			{
				ap->flags |= IPV6_AF_DUPLICATED;
				if (ap->dadcallback)
					ap->dadcallback(ap);
#ifdef DEBUG_NS
				found++;
#endif
			}
		}
#endif
	}
	if (rap == NULL) {
#ifdef LISTEN_DAD
		d6state = D6_STATE(ifp);
		if (d6state) {
			TAILQ_FOREACH(ap, &d6state->addrs, next) {
				if (memcmp(ap->addr.s6_addr,
				    nd_na->nd_na_target.s6_addr,
				    sizeof(ap->addr.s6_addr)) == 0)
				{
					ap->flags |= IPV6_AF_DUPLICATED;
					if (ap->dadcallback)
						ap->dadcallback(ap);
#ifdef DEBUG_NS
					found++;
#endif
				}
			}
		}
#endif

#ifdef DEBUG_NS
		if (found == 0)
			syslog(LOG_DEBUG, "%s: unexpected NA from %s",
			    ifp->name, sfrom);
#endif
		return;
	}

#ifdef DEBUG_NS
	syslog(LOG_DEBUG, "%s: %sNA from %s",
	    ifp->name, is_solicited ? "solicited " : "",  sfrom);
#endif

	/* Node is no longer a router, so remove it from consideration */
	if (!is_router && !rap->expired) {
		syslog(LOG_INFO, "%s: %s is no longer a router",
		    ifp->name, sfrom);
		rap->expired = 1;
		ipv6nd_cancelproberouter(rap);
		ipv6_buildroutes();
		script_runreason(ifp, "ROUTERADVERT");
		return;
	}

	if (is_solicited && is_router && rap->lifetime) {
		if (rap->expired) {
			rap->expired = 0;
			syslog(LOG_INFO, "%s: %s is reachable again",
				ifp->name, sfrom);
			ipv6_buildroutes();
			script_runreason(rap->iface, "ROUTERADVERT"); /* XXX */
		}
		rap->nsprobes = 0;
		if (rap->reachable) {
			ms_to_tv(&tv, rap->reachable);
		} else {
			tv.tv_sec = REACHABLE_TIME;
			tv.tv_usec = 0;
		}
		eloop_timeout_add_tv(&tv, ipv6nd_proberouter, rap);
		eloop_timeout_delete(ipv6nd_unreachable, rap);
	}
}

/* ARGSUSED */
static void
ipv6nd_handledata(__unused void *arg)
{
	ssize_t len;
	struct cmsghdr *cm;
	int hoplimit;
	struct in6_pktinfo pkt;
	struct icmp6_hdr *icp;
	struct interface *ifp;

	len = recvmsg(sock, &rcvhdr, 0);
	if (len == -1) {
		syslog(LOG_ERR, "recvmsg: %m");
		return;
	}
	sfrom = inet_ntop(AF_INET6, &from.sin6_addr,
	    ntopbuf, INET6_ADDRSTRLEN);
	if ((size_t)len < sizeof(struct icmp6_hdr)) {
		syslog(LOG_ERR, "IPv6 ICMP packet too short from %s", sfrom);
		return;
	}

	pkt.ipi6_ifindex = hoplimit = 0;
	for (cm = (struct cmsghdr *)CMSG_FIRSTHDR(&rcvhdr);
	     cm;
	     cm = (struct cmsghdr *)CMSG_NXTHDR(&rcvhdr, cm))
	{
		if (cm->cmsg_level != IPPROTO_IPV6)
			continue;
		switch(cm->cmsg_type) {
		case IPV6_PKTINFO:
			if (cm->cmsg_len == CMSG_LEN(sizeof(pkt)))
				memcpy(&pkt, CMSG_DATA(cm), sizeof(pkt));
			break;
		case IPV6_HOPLIMIT:
			if (cm->cmsg_len == CMSG_LEN(sizeof(int)))
				memcpy(&hoplimit, CMSG_DATA(cm), sizeof(int));
			break;
		}
	}

	if (pkt.ipi6_ifindex == 0 || hoplimit == 0) {
		syslog(LOG_ERR,
		    "IPv6 RA did not contain index or hop limit from %s",
		    sfrom);
		return;
	}

	TAILQ_FOREACH(ifp, ifaces, next) {
		if (ifp->index == (unsigned int)pkt.ipi6_ifindex)
			break;
	}

	icp = (struct icmp6_hdr *)rcvhdr.msg_iov[0].iov_base;
	if (icp->icmp6_code == 0) {
		switch(icp->icmp6_type) {
			case ND_NEIGHBOR_ADVERT:
				ipv6nd_handlena(ifp, icp, len);
				return;
			case ND_ROUTER_ADVERT:
				ipv6nd_handlera(ifp, icp, len);
				return;
		}
	}

	syslog(LOG_ERR, "invalid IPv6 type %d or code %d from %s",
	    icp->icmp6_type, icp->icmp6_code, sfrom);
}

int
ipv6nd_startrs(struct interface *ifp)
{
	struct rs_state *state;

	syslog(LOG_INFO, "%s: soliciting an IPv6 router", ifp->name);
	if (sock == -1) {
		if (ipv6nd_open() == -1) {
			syslog(LOG_ERR, "%s: ipv6nd_open: %m", __func__);
			return -1;
		}
		eloop_event_add(sock, ipv6nd_handledata, NULL);
	}

	eloop_timeout_delete(NULL, ifp);

	state = RS_STATE(ifp);
	if (state == NULL) {
		ifp->if_data[IF_DATA_IPV6ND] = calloc(1, sizeof(*state));
		state = RS_STATE(ifp);
		if (state == NULL) {
			syslog(LOG_ERR, "%s: %m", __func__);
			return -1;
		}
	}

	/* Always make a new probe as the underlying hardware
	 * address could have changed. */
	ipv6nd_makersprobe(ifp);
	if (state->rs == NULL) {
		syslog(LOG_ERR, "%s: ipv6ns_makersprobe: %m", __func__);
		return -1;
	}

	state->rsprobes = 0;
	ipv6nd_sendrsprobe(ifp);
	return 0;
}
