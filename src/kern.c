/* Kernel API for join/leave multicast groups and add/del routes
 *
 * Copyright (c) 2011-2020  Joachim Wiberg <troglobit@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <string.h>

#include "iface.h"
#include "kern.h"
#include "log.h"
#include "mrdisc.h"
#include "util.h"


/*
 * This function handles both ASM and SSM join/leave for IPv4 and IPv6
 * using the RFC 3678 API available on Linux, FreeBSD, and a few other
 * operating systems.
 *
 * On Linux this makes it possible to join a group on an interface that
 * is down and/or has no IP address assigned to it yet.  The latter is
 * one of the most common causes of malfunction on Linux and IPv4 with
 * the old struct ip_mreq API.
 */
#ifdef HAVE_STRUCT_GROUP_REQ	/* Prefer RFC 3678 */
static int group_req(int sd, int cmd, struct mcgroup *mcg)
{
	char source[INET_ADDRSTR_LEN], group[INET_ADDRSTR_LEN];
	struct group_source_req gsr;
	struct group_req gr;
	size_t len;
	void *arg;
	int op, proto;

#ifdef HAVE_IPV6_MULTICAST_HOST
	if (mcg->group.ss_family == AF_INET6)
		proto = IPPROTO_IPV6;
	else
#endif
		proto = IPPROTO_IP;

	if (is_anyaddr(&mcg->source)) {
		if (cmd == 'j')	op = MCAST_JOIN_GROUP;
		else		op = MCAST_LEAVE_GROUP;

		gr.gr_interface    = mcg->iface->ifindex;
		gr.gr_group        = mcg->group;

		strncpy(source, "*", sizeof(source));
		inet_addr2str(&gr.gr_group, group, sizeof(group));

		arg                = &gr;
		len                = sizeof(gr);
	} else {
		if (cmd == 'j')	op = MCAST_JOIN_SOURCE_GROUP;
		else		op = MCAST_LEAVE_SOURCE_GROUP;

		gsr.gsr_interface  = mcg->iface->ifindex;
		gsr.gsr_source     = mcg->source;
		gsr.gsr_group      = mcg->group;

		inet_addr2str(&gsr.gsr_source, source, sizeof(source));
		inet_addr2str(&gsr.gsr_group, group, sizeof(group));

		arg                = &gsr;
		len                = sizeof(gsr);
	}

	smclog(LOG_DEBUG, "%s group (%s,%s) on ifindex %d and socket %d ...",
	       cmd == 'j' ? "Join" : "Leave", source, group, mcg->iface->ifindex, sd);

	return setsockopt(sd, proto, op, arg, len);
}

#else  /* Assume we have old style struct ip_mreq */

static int group_req(int sd, int cmd, struct mcgroup *mcg)
{
	char group[INET_ADDRSTR_LEN];
#ifdef HAVE_IPV6_MULTICAST_HOST
	struct ipv6_mreq ipv6mr;
#endif
#ifdef HAVE_STRUCT_IP_MREQN
	struct ip_mreqn ipmr;
#else
	struct ip_mreq ipmr;
#endif
	int op, proto;
	size_t len;
	void *arg;

#ifdef HAVE_IPV6_MULTICAST_HOST
	if (mcg->group.ss_family == AF_INET6) {
		struct sockaddr_in6 *sin6;

		sin6 = inet_addr6_get(&mcg->group);
		ipv6mr.ipv6mr_multiaddr = sin6->sin6_addr;
		ipv6mr.ipv6mr_interface = mcg->iface->ifindex;

		proto = IPPROTO_IPV6;
		op    = cmd == 'j' ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP;
		arg   = &ipv6mr;
		len   = sizeof(ipv6mr);
	} else
#endif
	{
		ipmr.imr_multiaddr = *inet_addr_get(&mcg->group);
#ifdef HAVE_STRUCT_IP_MREQN
		ipmr.imr_ifindex   = mcg->iface->ifindex;
#else
		ipmr.imr_interface = mcg->iface->inaddr;
#endif

		proto = IPPROTO_IP;
		op    = cmd == 'j' ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP;
		arg   = &ipmr;
		len   = sizeof(ipmr);
	}

	smclog(LOG_DEBUG, "%s group (*,%s) on ifindex %d ...", cmd == 'j' ? "Join" : "Leave",
	       inet_addr2str(&mcg->group, group, sizeof(group)), mcg->iface->ifindex);

	return setsockopt(sd, proto, op, arg, len);
}
#endif

int kern_join_leave(int sd, int cmd, struct mcgroup *mcg)
{
	int err;

	if (!cmd)
		cmd = 'j';

	err = group_req(sd, cmd, mcg);
	if (err) {
		char source[INET_ADDRSTR_LEN] = "*";
		char group[INET_ADDRSTR_LEN];
		int len;

		if (!is_anyaddr(&mcg->source))
			inet_addr2str(&mcg->source, source, sizeof(source));
		inet_addr2str(&mcg->group, group, sizeof(group));
		len = mcg->len == 0 ? 32 : mcg->len;

		smclog(LOG_ERR, "Failed %s group (%s,%s/%d) on sd %d ... %d: %s",
		       cmd == 'j' ? "joining" : "leaving",
		       source, group, len, sd,
		       errno, strerror(errno));
		return 1;
	}

	return 0;
}

int kern_mroute4(int sd, int cmd, struct mroute *route, int active)
{
	char origin[INET_ADDRSTRLEN], group[INET_ADDRSTRLEN];
	int op = cmd == 'a' ? MRT_ADD_MFC : MRT_DEL_MFC;
	struct mfcctl mc;
	size_t i;

	if (sd == -1) {
		smclog(LOG_DEBUG, "No IPv4 multicast socket");
		return -1;
	}

	memset(&mc, 0, sizeof(mc));
	mc.mfcc_origin   = *inet_addr_get(&route->source);
	mc.mfcc_mcastgrp = *inet_addr_get(&route->group);
	mc.mfcc_parent   = route->inbound;

	inet_addr2str(&route->source, origin, sizeof(origin));
	inet_addr2str(&route->group, group, sizeof(group));

	/* copy the TTL vector, as many as the kernel supports */
	for (i = 0; i < NELEMS(mc.mfcc_ttls); i++)
		mc.mfcc_ttls[i] = route->ttl[i];

	if (setsockopt(sd, IPPROTO_IP, op, &mc, sizeof(mc))) {
		if (ENOENT == errno)
			smclog(LOG_DEBUG, "failed removing multicast route (%s,%s), does not exist.",
				origin, group);
		else
			smclog(LOG_DEBUG, "failed %s IPv4 multicast route (%s,%s): %s",
			       cmd == 'a' ? "adding" : "removing", origin, group, strerror(errno));
		return 1;
	}

	if (active) {
		smclog(LOG_DEBUG, "%s %s -> %s from VIF %d", cmd == 'a' ? "Add" : "Del",
		       origin, group, route->inbound);

		/* Only enable/disable mrdisc for active routes, i.e. with outbound */
		if (cmd == 'a')
			mrdisc_enable(route->inbound);
		else
			mrdisc_disable(route->inbound);
	}

	return 0;
}

int kern_mroute6(int sd, int cmd, struct mroute *route)
{
	char origin[INET_ADDRSTR_LEN], group[INET_ADDRSTR_LEN];
	int op = cmd == 'a' ? MRT6_ADD_MFC : MRT6_DEL_MFC;
	struct mf6cctl mc;
	size_t i;

	if (sd < 0) {
		smclog(LOG_DEBUG, "No IPv6 multicast socket");
		return -1;
	}

	memset(&mc, 0, sizeof(mc));
	mc.mf6cc_origin   = *inet_addr6_get(&route->source);
	mc.mf6cc_mcastgrp = *inet_addr6_get(&route->group);
	mc.mf6cc_parent   = route->inbound;

	inet_addr2str(&route->source, origin, INET_ADDRSTR_LEN);
	inet_addr2str(&route->group, group, INET_ADDRSTR_LEN);

	IF_ZERO(&mc.mf6cc_ifset);
	for (i = 0; i < NELEMS(route->ttl); i++) {
		if (route->ttl[i]) {
			IF_SET(i, &mc.mf6cc_ifset);
		}
	}

	if (setsockopt(sd, IPPROTO_IPV6, op, &mc, sizeof(mc))) {
		if (ENOENT == errno)
			smclog(LOG_DEBUG, "failed removing IPv6 multicast route (%s,%s), does not exist.",
				origin, group);
		else
			smclog(LOG_WARNING, "failed %s IPv6 multicast route (%s,%s): %s",
			       cmd == 'a' ? "adding" : "removing", origin, group, strerror(errno));
		return 1;
	}

	return 0;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
