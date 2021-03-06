/*
 * Packet matching code.
 *
 * Copyright (C) 1999 Paul `Rusty' Russell & Michael J. Neuling
 * Copyright (C) 2009-2002 Netfilter core team <coreteam@netfilter.org>
 *
 * 19 Jan 2002 Harald Welte <laforge@gnumonks.org>
 * 	- increase module usage count as soon as we have rules inside
 * 	  a table
 */
#include <linux/cache.h>
#include <linux/skbuff.h>
#include <linux/kmod.h>
#include <linux/vmalloc.h>
#include <linux/netdevice.h>
#include <linux/module.h>
#include <linux/ip.h>
#include <net/route.h>
#include <net/ip.h>
#include <net/icmp.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <net/netfilter/nf_nat_core.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/nf_conntrack_acct.h>
#include <linux/netfilter/nf_conntrack_common.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/rcupdate.h>
#include <net/fast_vpn.h>
#include <linux/ntc_shaper_hooks.h>

#if IS_ENABLED(CONFIG_RA_HW_NAT)
#include <../ndm/hw_nat/ra_nat.h>
#endif

extern int ipv4_fastnat_conntrack;

extern int (*fast_nat_hit_hook_func)(struct sk_buff *skb);
extern int (*fast_nat_bind_hook_func)(struct nf_conn *ct,
	enum ip_conntrack_info ctinfo, 
	struct sk_buff *skb,
	struct nf_conntrack_l3proto *l3proto,
	struct nf_conntrack_l4proto *l4proto);

extern int
manip_pkt(u_int16_t proto,
	struct sk_buff *skb,
	unsigned int iphdroff,
	const struct nf_conntrack_tuple *target,
	enum nf_nat_manip_type maniptype);

extern int (*fast_nat_bind_hook_ingress)(struct sk_buff * skb);

/*
 * check NAT session initialized and ready
 */
static inline int nat_is_ready(struct nf_conn *ct)
{
	/* If NAT initialized is finished may be offload */
	if ((ct->status & IPS_NAT_DONE_MASK) == IPS_NAT_DONE_MASK)
		return 1;
	return 0;
}

/*
 * check SKB really accesseble
 */
static inline int skb_is_ready(struct sk_buff *skb)
{
	if( skb_cloned(skb) && !skb->sk )
		return 0;
	return 1;
}

/*
 * Direct send packets to output.
 * Stolen from ip_finish_output2.
 */
static inline int fast_nat_path_output(struct sk_buff *skb)
{
	struct dst_entry *dst = skb_dst(skb);
	struct net_device *dev = dst->dev;
	unsigned int hh_len = LL_RESERVED_SPACE(dev);
	struct neighbour *neigh;

	/* Be paranoid, rather than too clever. */
	if (unlikely(skb_headroom(skb) < hh_len && dev->header_ops)) {
		struct sk_buff *skb2;

		skb2 = skb_realloc_headroom(skb, LL_RESERVED_SPACE(dev));
		if (skb2 == NULL) {
			kfree_skb(skb);
			return -ENOMEM;
		}
		if (skb->sk)
			skb_set_owner_w(skb2, skb->sk);
		consume_skb(skb);
		skb = skb2;
	}

	rcu_read_lock();
	neigh = dst_get_neighbour_noref(dst);
	if (neigh) {
		int res = neigh_output(neigh, skb);

		rcu_read_unlock();

		/* Don't return 1 */
		return (res == 1) ? 0 : res;
	}
	rcu_read_unlock();

	net_dbg_ratelimited("%s: No header cache and no neighbour!\n",
			    __func__);
	kfree_skb(skb);
	return -EINVAL;
}

static inline int ip_skb_dst_mtu(struct sk_buff *skb)
{
	struct inet_sock *inet = skb->sk ? inet_sk(skb->sk) : NULL;

	return (inet && inet->pmtudisc == IP_PMTUDISC_PROBE) ?
		skb_dst(skb)->dev->mtu : dst_mtu(skb_dst(skb));
}

static int fast_nat_bind_hook_egress(struct sk_buff * skb)
{
	struct iphdr *iph = ip_hdr(skb);

	if (iph->ttl <= 1) {
		icmp_send(skb, ICMP_TIME_EXCEEDED, ICMP_EXC_TTL, 0);
		kfree_skb(skb);
		return -EPERM;
	}

	ip_decrease_ttl(iph);

	if (skb->len > ip_skb_dst_mtu(skb) && !skb_is_gso(skb))
		return ip_fragment(skb, fast_nat_path_output);

	return fast_nat_path_output(skb);
}

/* Returns 1 if nf_hook okfn() needs to be executed by the caller,
 * -EPERM for NF_DROP, 0 otherwise. */
static int fast_nat_path(struct sk_buff *skb)
{
	ntc_shaper_hook_fn *shaper_egress;
	int retval = 0;

	if (likely(1
#if IS_ENABLED(CONFIG_FAST_NAT)
		&& !SWNAT_KA_CHECK_MARK(skb)
#endif
#if IS_ENABLED(CONFIG_RA_HW_NAT)
		&& !FOE_SKB_IS_KEEPALIVE(skb)
#endif
	)) {
		struct nf_conn *ct;
		enum ip_conntrack_info ctinfo;

		ct = nf_ct_get(skb, &ctinfo);
		if (likely(ct != NULL)) {
			struct nf_conn_counter *acct = nf_conn_acct_find(ct);

			if (likely(acct != NULL)) {
				atomic64_inc(&acct[CTINFO2DIR(ctinfo)].packets);
				atomic64_add(skb->len, &acct[CTINFO2DIR(ctinfo)].bytes);
			}
		}
	}

	if (skb_dst(skb) == NULL) {
		const struct iphdr *iph = ip_hdr(skb);
		struct net_device *dev = skb->dev;

		if (ip_route_input(skb, iph->daddr, iph->saddr, iph->tos, dev)) {
			kfree_skb(skb);
			return -EPERM;
		}

		/*  Change skb owner to output device */
		skb->dev = skb_dst(skb)->dev;
	}

	shaper_egress = ntc_shaper_egress_hook_get();

	if (shaper_egress) {
		unsigned int ntc_retval = shaper_egress(skb, 0, 0, NULL, fast_nat_bind_hook_egress, NULL, NULL);

		switch (ntc_retval) {
			case NF_ACCEPT:
				retval = fast_nat_bind_hook_egress(skb);
				break;
			case NF_STOLEN:
				retval = 0;
				break;
			default:
				kfree_skb(skb);
				retval = -EPERM;
				break;
		}
	} else
		retval = fast_nat_bind_hook_egress(skb);

	ntc_shaper_egress_hook_put();

	return retval;
}

static int
fast_nat_do_bindings(struct nf_conn *ct,
		enum ip_conntrack_info ctinfo,
		struct sk_buff *skb,
		struct nf_conntrack_l3proto *l3proto,
		struct nf_conntrack_l4proto *l4proto)
{
	static int hn[2] = {NF_INET_PRE_ROUTING, NF_INET_POST_ROUTING};
	enum ip_conntrack_dir dir = CTINFO2DIR(ctinfo);
	unsigned int i = 0;

	/* This check prevent corrupt conntrack data */
	if(!nat_is_ready(ct) || !skb_is_ready(skb)) {
#ifdef DEBUG
		if (net_ratelimit())
		    printk(KERN_DEBUG "fast_path: SKB or CT not ready for offload\n");
#endif
		return NF_ACCEPT; /* Ignore */
	}

	do {
		enum nf_nat_manip_type mtype = HOOK2MANIP(hn[i]);
		unsigned long statusbit;

		if (mtype == NF_NAT_MANIP_SRC)
			statusbit = IPS_SRC_NAT;
		else
			statusbit = IPS_DST_NAT;

		/* Invert if this is reply dir. */
		if (dir == IP_CT_DIR_REPLY)
			statusbit ^= IPS_NAT_MASK;

		if (ct->status & statusbit) {
			struct nf_conntrack_tuple target;

			if (skb_dst(skb) == NULL && mtype == NF_NAT_MANIP_SRC) {
				const struct iphdr *iph = ip_hdr(skb);
				struct net_device *dev = skb->dev;

				if (ip_route_input(skb, iph->daddr, iph->saddr, iph->tos, dev))
					return NF_DROP;

				/* Change skb owner to output device */
				skb->dev = skb_dst(skb)->dev;
			}

			/* We are aiming to look like inverse of other direction. */
			nf_ct_invert_tuple(&target, &ct->tuplehash[!dir].tuple, l3proto, l4proto);

			if (!manip_pkt(target.dst.protonum, skb, 0, &target, mtype))
				return NF_DROP;
		}
		i++;
	} while (i < 2);

	return NF_FAST_NAT;
}

static int __init fast_nat_init(void)
{
	rcu_assign_pointer(fast_nat_bind_hook_ingress, fast_nat_path);
	rcu_assign_pointer(fast_nat_hit_hook_func, fast_nat_path);
	synchronize_rcu();
	rcu_assign_pointer(fast_nat_bind_hook_func, fast_nat_do_bindings);
	ipv4_fastnat_conntrack = 1;
	printk(KERN_INFO "Fast NAT loaded\n");
	return 0;
}

static void __exit fast_nat_fini(void)
{
#if defined(CONFIG_FAST_NAT_MODULE)
	ipv4_fastnat_conntrack = 0;
	rcu_assign_pointer(fast_nat_bind_hook_func, NULL);
	synchronize_rcu();
	rcu_assign_pointer(fast_nat_hit_hook_func, NULL);
	rcu_assign_pointer(fast_nat_bind_hook_ingress, NULL);
	printk(KERN_INFO "Fast NAT unloaded\n");
#endif
}

module_init(fast_nat_init);
module_exit(fast_nat_fini);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("http://www.ndmsystems.com");
