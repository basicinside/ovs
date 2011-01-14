/*
 * Copyright (c) 2011 Nicira Networks.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "route-table.h"

#include <assert.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/rtnetlink.h>
#include <net/if.h>

#include "hash.h"
#include "hmap.h"
#include "netlink.h"
#include "netlink-socket.h"
#include "ofpbuf.h"
#include "rtnetlink.h"
#include "vlog.h"

VLOG_DEFINE_THIS_MODULE(route_table);

struct route_data {
    /* Copied from struct rtmsg. */
    unsigned char rtm_dst_len;

    /* Extracted from Netlink attributes. */
    uint32_t rta_dst; /* Destination in host byte order. 0 if missing. */
    int rta_oif;      /* Output interface index. */
};

/* A digested version of a route message sent down by the kernel to indicate
 * that a route has changed. */
struct route_table_msg {
    bool relevant;        /* Should this message be processed? */
    int nlmsg_type;       /* e.g. RTM_NEWROUTE, RTM_DELROUTE. */
    struct route_data rd; /* Data parsed from this message. */
};

struct route_node {
    struct hmap_node node; /* Node in route_map. */
    struct route_data rd;  /* Data associated with this node. */
};

static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 20);

static unsigned int register_count = 0;
static struct rtnetlink *rtn = NULL;
static struct route_table_msg rtmsg;
static struct rtnetlink_notifier notifier;
static struct hmap route_map;

static int route_table_reset(void);
static bool route_table_parse(struct ofpbuf *, struct route_table_msg *);
static void route_table_change(const struct route_table_msg *, void *);
static struct route_node *route_node_lookup(const struct route_data *);
static struct route_node *route_node_lookup_by_ip(uint32_t ip);
static void route_map_clear(void);
static uint32_t hash_route_data(const struct route_data *);

/* Populates 'ifindex' with the interface index traffic destined for 'ip' is
 * likely to egress.  There is no hard guarantee that traffic destined for 'ip'
 * will egress out the specified interface.  'ifindex' may refer to an
 * interface which is not physical (such as a bridge port).
 *
 * Returns true if successful, otherwise false. */
bool
route_table_get_ifindex(ovs_be32 ip_, int *ifindex)
{
    struct route_node *rn;
    uint32_t ip = ntohl(ip_);

    *ifindex = 0;

    rn = route_node_lookup_by_ip(ip);

    if (rn) {
        *ifindex = rn->rd.rta_oif;
        return true;
    }

    /* Choose a default route. */
    HMAP_FOR_EACH(rn, node, &route_map) {
        if (rn->rd.rta_dst == 0 && rn->rd.rtm_dst_len == 0) {
            *ifindex = rn->rd.rta_oif;
            return true;
        }
    }

    return false;
}

/* Users of the route_table module should register themselves with this
 * function before making any other route_table function calls. */
void
route_table_register(void)
{
    if (!register_count) {
        rtnetlink_parse_func *pf;
        rtnetlink_notify_func *nf;

        assert(!rtn);

        pf = (rtnetlink_parse_func *)  route_table_parse;
        nf = (rtnetlink_notify_func *) route_table_change;

        rtn = rtnetlink_create(RTNLGRP_IPV4_ROUTE, pf, &rtmsg);
        rtnetlink_notifier_register(rtn, &notifier, nf, NULL);

        hmap_init(&route_map);
        route_table_reset();
    }

    register_count++;
}

/* Users of the route_table module should unregister themselves with this
 * function when they will no longer be making any more route_table fuction
 * calls. */
void
route_table_unregister(void)
{
    register_count--;

    if (!register_count) {
        rtnetlink_destroy(rtn);
        rtn = NULL;

        route_map_clear();
        hmap_destroy(&route_map);
    }
}

/* Run periodically to update the locally maintained routing table. */
void
route_table_run(void)
{
    if (rtn) {
        rtnetlink_notifier_run(rtn);
    }
}

/* Causes poll_block() to wake up when route_table updates are required. */
void
route_table_wait(void)
{
    if (rtn) {
        rtnetlink_notifier_wait(rtn);
    }
}

static int
route_table_reset(void)
{
    int error;
    struct nl_dump dump;
    struct rtgenmsg *rtmsg;
    struct ofpbuf request, reply;
    static struct nl_sock *rtnl_sock;

    route_map_clear();

    error = nl_sock_create(NETLINK_ROUTE, 0, 0, 0, &rtnl_sock);
    if (error) {
        VLOG_WARN_RL(&rl, "failed to reset routing table, "
                     "cannot create RTNETLINK_ROUTE socket");
        return error;
    }

    ofpbuf_init(&request, 0);

    nl_msg_put_nlmsghdr(&request, sizeof *rtmsg, RTM_GETROUTE, NLM_F_REQUEST);

    rtmsg = ofpbuf_put_zeros(&request, sizeof *rtmsg);
    rtmsg->rtgen_family = AF_INET;

    nl_dump_start(&dump, rtnl_sock, &request);

    while (nl_dump_next(&dump, &reply)) {
        struct route_table_msg msg;

        if (route_table_parse(&reply, &msg)) {
            route_table_change(&msg, NULL);
        }
    }

    error = nl_dump_done(&dump);
    nl_sock_destroy(rtnl_sock);

    return error;
}


static bool
route_table_parse(struct ofpbuf *buf, struct route_table_msg *change)
{
    bool parsed;

    static const struct nl_policy policy[] = {
        [RTA_DST] = { .type = NL_A_U32, .optional = true  },
        [RTA_OIF] = { .type = NL_A_U32, .optional = false },
    };

    static struct nlattr *attrs[ARRAY_SIZE(policy)];

    parsed = nl_policy_parse(buf, NLMSG_HDRLEN + sizeof(struct rtmsg),
                             policy, attrs, ARRAY_SIZE(policy));

    if (parsed) {
        const struct rtmsg *rtm;
        const struct nlmsghdr *nlmsg;

        nlmsg = buf->data;
        rtm = (const struct rtmsg *) ((const char *) buf->data + NLMSG_HDRLEN);

        if (rtm->rtm_family != AF_INET) {
            VLOG_DBG_RL(&rl, "received non AF_INET rtnetlink route message");
            return false;
        }

        memset(change, 0, sizeof *change);
        change->relevant = true;

        if (rtm->rtm_scope == RT_SCOPE_NOWHERE) {
            change->relevant = false;
        }

        if (rtm->rtm_type != RTN_UNICAST &&
            rtm->rtm_type != RTN_LOCAL) {
            change->relevant = false;
        }

        change->nlmsg_type     = nlmsg->nlmsg_type;
        change->rd.rtm_dst_len = rtm->rtm_dst_len;
        change->rd.rta_oif     = nl_attr_get_u32(attrs[RTA_OIF]);

        if (attrs[RTA_DST]) {
            change->rd.rta_dst = ntohl(nl_attr_get_be32(attrs[RTA_DST]));
        }

    } else {
        VLOG_DBG_RL(&rl, "received unparseable rtnetlink route message");
    }

    return parsed;
}

static void
route_table_change(const struct route_table_msg *change, void *aux OVS_UNUSED)
{
    if (!change) {
        VLOG_DBG_RL(&rl, "received NULL change message");
        route_table_reset();
    } else if (!change->relevant) {
        VLOG_DBG_RL(&rl, "ignoring irrelevant change message");
    } else if (change->nlmsg_type == RTM_NEWROUTE) {
        if (!route_node_lookup(&change->rd)) {
            struct route_node *rn;

            rn = xzalloc(sizeof *rn);
            memcpy(&rn->rd, &change->rd, sizeof change->rd);

            hmap_insert(&route_map, &rn->node, hash_route_data(&rn->rd));
        } else {
            VLOG_DBG_RL(&rl, "skipping insertion of duplicate route entry");
        }
    } else if (change->nlmsg_type == RTM_DELROUTE) {
        struct route_node *rn;

        rn = route_node_lookup(&change->rd);

        if (rn) {
            hmap_remove(&route_map, &rn->node);
            free(rn);
        } else {
            VLOG_DBG_RL(&rl, "skipping deletion of non-existent route entry");
        }
    }
}

static struct route_node *
route_node_lookup(const struct route_data *rd)
{
    struct route_node *rn;

    HMAP_FOR_EACH_WITH_HASH(rn, node, hash_route_data(rd), &route_map) {
        if (!memcmp(&rn->rd, rd, sizeof *rd)) {
            return rn;
        }
    }

    return NULL;
}

static struct route_node *
route_node_lookup_by_ip(uint32_t ip)
{
    int dst_len;
    struct route_node *rn, *rn_ret;

    dst_len = -1;
    rn_ret  = NULL;

    HMAP_FOR_EACH(rn, node, &route_map) {
        uint32_t mask = 0xffffffff << (32 - rn->rd.rtm_dst_len);

        if (rn->rd.rta_dst == 0 && rn->rd.rtm_dst_len == 0) {
            /* Default route. */
            continue;
        }

        if (rn->rd.rtm_dst_len > dst_len &&
            (ip & mask) == (rn->rd.rta_dst & mask)) {
            rn_ret  = rn;
            dst_len = rn->rd.rtm_dst_len;
        }
    }

    return rn_ret;
}

static void
route_map_clear(void)
{
    struct route_node *rn, *rn_next;

    HMAP_FOR_EACH_SAFE(rn, rn_next, node, &route_map) {
        hmap_remove(&route_map, &rn->node);
        free(rn);
    }
}

static uint32_t
hash_route_data(const struct route_data *rd)
{
    return hash_bytes(rd, sizeof *rd, 0);
}
