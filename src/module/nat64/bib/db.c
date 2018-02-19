#include "nat64/bib/db.h"

#include <net/ip6_checksum.h>

#include "constants.h"
#include "linux-version.h"
#include "icmp-wrapper.h"
#include "module-stats.h"
#include "rbtree.h"
#include "send-packet.h"
#include "str-utils.h"
#include "wkmalloc.h"
#include "nat64/bib/pkt-queue.h"

/*
 * TODO (performance) Maybe pack this?
 */
struct tabled_bib {
	struct ipv6_transport_addr src6;
	struct ipv4_transport_addr src4;
	l4_protocol proto;
	bool is_static;

	struct rb_node hook6;
	struct rb_node hook4;

	struct rb_root sessions;
};

/*
 * TODO (performance) Maybe pack this?
 */
struct tabled_session {
	/**
	 * We don't strictly need to store @dst6; @dst6 is always @dst4 plus the
	 * pool6 prefix. But we store it anyway so I don't have to make more
	 * mess constatly in this module.
	 */
	struct ipv6_transport_addr dst6;
	struct ipv4_transport_addr dst4;
	tcp_state state;
	/** MUST NOT be NULL. */
	struct tabled_bib *bib;

	/**
	 * Sessions only need one tree. The rationale is different for TCP/UDP
	 * vs ICMP sessions:
	 *
	 * In TCP and UDP the dst4 address is just the dst6 address minus the
	 * pool6 prefix. Therefore, and assuming the pool6 prefix stays still
	 * (something I'm well willing to enforce), sessions indexed by dst4
	 * yield exactly the same tree as sessions indexed by dst6.
	 *
	 * In ICMP, dst4.l4 is the same as src4.l4 instead of dst6.l4. This
	 * would normally mean that dst6 sessions would yield a different tree
	 * than dst4 sessions. Luckily, this is not the case because dst4.l4 is
	 * not meaningful to the tree search in ICMP sessions; sessions are
	 * already grouped by BIB entry, which means all of a BIB entry's
	 * sessions will have different dst4.l3. (Which has more precedence than
	 * dst4.l4 during searches.)
	 * (And again, dst4.l3 is just dst6.l3 minus the prefix.)
	 *
	 * This might be a little annoying to wrap one's head around, but I
	 * think it's really nice that we only need to search and rebalance
	 * three trees (instead of four) whenever we need to add a BIB/session
	 * couple during translation.
	 * It's also a very elegant hack; it doesn't result in any special case
	 * handling in the whole code below.
	 */
	struct rb_node tree_hook;

	unsigned long update_time;
	/** MUST NOT be NULL. */
	struct expire_timer *expirer;
	struct list_head list_hook;

	/** See pke_queue.h for some thoughts on stored packets. */
	struct sk_buff *stored;
};

struct bib_session_tuple {
	struct tabled_bib *bib;
	struct tabled_session *session;
};

/**
 * A session that is about to cause Jool to create and send a new packet.
 *
 * This can happen in two situations:
 * - An established TCP session has been hanging for too long and Jool wants to
 *   query the endpoints for status.
 *   This is done by sending an empty TCP packet that should simply be ACK'd.
 * - What initially seemed like a potential TCP SO ended up expiring after a 6-
 *   second wait so it has to be ICMP errored. See pkt_queue.h.
 */
struct probing_session {
	struct session_entry session;
	struct sk_buff *skb;
	struct list_head list_hook;
};

struct expire_timer {
	struct list_head sessions;
	session_timer_type type;
	fate_cb decide_fate_cb;
};

struct bib_table {
	/** Indexes the entries using their IPv6 identifiers. */
	struct rb_root tree6;
	/** Indexes the entries using their IPv4 identifiers. */
	struct rb_root tree4;

	/* Number of entries in this table. */
	u64 session_count;

	spinlock_t lock;

	/** Expires this table's established sessions. */
	struct expire_timer est_timer;

	/*
	 * =============================================================
	 * Fields below are only relevant in the TCP table.
	 * (If you need to know what "type 1" and "type 2" mean, see the
	 * pkt_queue module's .h.)
	 * =============================================================
	 */

	/**
	 * Expires this table's transitory sessions.
	 * This is initialized in the UDP/ICMP tables, but all their operations
	 * become no-ops.
	 */
	struct expire_timer trans_timer;
	/**
	 * Expires this table's type-2 packets and their sessions.
	 * This is initialized in the UDP/ICMP tables, but all their operations
	 * become no-ops.
	 */
	struct expire_timer syn4_timer;

	/** Current number of packets (of both types) in the table. */
	int pkt_count;

	/**
	 * Packet storage for type 1 packets.
	 * This is NULL in UDP/ICMP.
	 */
	struct pktqueue *pkt_queue;
};

struct bib {
	/** The session table for UDP conversations. */
	struct bib_table udp;
	/** The session table for TCP connections. */
	struct bib_table tcp;
	/** The session table for ICMP conversations. */
	struct bib_table icmp;

	struct kref refs;
};

/*
 * A collection of arguments that are usually involved in BIB operations and
 * that would otherwise clutter argument lists horribly.
 *
 * It's basically a struct xlation, except with BIB scope.
 */
struct bib_state {
	struct bib_table *table;
	struct globals_bib *globals;
};

struct slot_group {
	struct tree_slot bib6;
	struct tree_slot bib4;
	struct tree_slot session;
};

struct bib_delete_list {
	struct rb_node *first;
};

/**
 * Just an amalgamation of arguments that are needed whenever the code needs to
 * add an entry to the database while translating IPv6 packets.
 * The main reason why this exists is to minimize argument lists, really.
 */
struct bib_add6_args {
	struct bib_state state;

	/** The entry being added. */
	struct bib_session_tuple new;
	/**
	 * If, while trying to find the database slot where @new should be
	 * added, we found an entry that collides with it, this will point to
	 * it.
	 */
	struct bib_session_tuple old;
	/**
	 * Describes the point in the @table where @new (and its session(s))
	 * should be inserted.
	 * This needs to be remembered because there's always a bit of
	 * processing that needs to be done between finding the slots and
	 * actually placing the entries there.
	 */
	struct slot_group slots;
	/**
	 * Mask address candidates that can be used to create the new BIB
	 * entry's IPv4 transport address.
	 * This field needs to be computed during insertion (as opposed to
	 * during allocation/initialization) because the operation largely
	 * depends on already existing surrouding BIB entries.
	 */
	struct mask_domain *masks;
	/**
	 * If, for some reason, the code decides that some BIB entries need to
	 * be deleted, it will add them to this list.
	 * The reason for that is that this *can* be a fair amount of work that
	 * does not need the spinlock's protection, so it *should* be done
	 * later.
	 */
	struct bib_delete_list rm_list;
};

struct bib_add4_args {
	struct bib_state state;
	struct bib_session_tuple old;
	struct tabled_session *new;
	struct tree_slot session_slot;
};

static struct kmem_cache *bib_cache;
static struct kmem_cache *session_cache;

#define alloc_bib(flags) wkmem_cache_alloc("bib entry", bib_cache, flags)
#define alloc_session(flags) wkmem_cache_alloc("session", session_cache, flags)
#define free_bib(bib) wkmem_cache_free("bib entry", bib_cache, bib)
#define free_session(session) wkmem_cache_free("session", session_cache, session)

static struct tabled_bib *bib6_entry(const struct rb_node *node)
{
	return node ? rb_entry(node, struct tabled_bib, hook6) : NULL;
}

static struct tabled_bib *bib4_entry(const struct rb_node *node)
{
	return node ? rb_entry(node, struct tabled_bib, hook4) : NULL;
}

static struct tabled_session *node2session(const struct rb_node *node)
{
	return node ? rb_entry(node, struct tabled_session, tree_hook) : NULL;
}

/**
 * "[Convert] tabled BIB to BIB entry"
 */
static void tbtobe(struct tabled_bib *tabled, struct bib_entry *bib)
{
	if (!bib)
		return;

	bib->ipv6 = tabled->src6;
	bib->ipv4 = tabled->src4;
	bib->l4_proto = tabled->proto;
}

/**
 * "[Convert] tabled session to session entry"
 */
static void tstose(struct bib_state *state,
		struct tabled_session *tsession,
		struct session_entry *session)
{
	session->src6 = tsession->bib->src6;
	session->dst6 = tsession->dst6;
	session->src4 = tsession->bib->src4;
	session->dst4 = tsession->dst4;
	session->proto = tsession->bib->proto;
	session->state = tsession->state;
	session->timer_type = tsession->expirer->type;
	session->update_time = tsession->update_time;
	session->has_stored = !!tsession->stored;

	/* There's nothing that can be done on error, so just report zero. */

	switch (session->proto) {
	case L4PROTO_TCP:
		if (tsession->expirer == &state->table->est_timer) {
			session->timeout = state->globals->ttl.tcp_est;
		} else if (tsession->expirer == &state->table->trans_timer) {
			session->timeout = state->globals->ttl.tcp_trans;
		} else {
			WARN(1, "BIB entry's timer does not match any timer from its table.");
			session->timeout = 0;
		}
		return;
	case L4PROTO_UDP:
		session->timeout = state->globals->ttl.udp;
		return;
	case L4PROTO_ICMP:
		session->timeout = state->globals->ttl.icmp;
		return;
	case L4PROTO_OTHER:
		; /* Fall through */
	}

	WARN(1, "BIB entry contains illegal protocol '%u'.", session->proto);
	session->timeout = 0;
}

/**
 * "[Convert] tabled BIB to bib_session"
 */
static void tbtobs(struct tabled_bib *tabled, struct bib_session *bs)
{
	if (!bs)
		return;

	bs->bib_set = true;
	bs->session.src6 = tabled->src6;
	bs->session.src4 = tabled->src4;
	bs->session.proto = tabled->proto;
}

/**
 * [Convert] tabled session to bib_session"
 */
static void tstobs(struct bib_state *state, struct tabled_session *session,
		struct bib_session *bs)
{
	if (!bs)
		return;

	bs->bib_set = true;
	bs->session_set = true;
	tstose(state, session, &bs->session);
}

/**
 * One-liner to get the session table corresponding to the @proto protocol.
 */
static struct bib_table *get_table(struct bib *db, l4_protocol proto)
{
	switch (proto) {
	case L4PROTO_TCP:
		return &db->tcp;
	case L4PROTO_UDP:
		return &db->udp;
	case L4PROTO_ICMP:
		return &db->icmp;
	case L4PROTO_OTHER:
		break;
	}

	WARN(true, "Unsupported transport protocol: %u.", proto);
	return NULL;
}

static void kill_stored_pkt(struct bib_state *state,
		struct tabled_session *session)
{
	if (!session->stored)
		return;

	log_debug("Deleting stored type 2 packet.");
	kfree_skb(session->stored);
	session->stored = NULL;
	state->table->pkt_count--;
}

int bib_init(void)
{
	bib_cache = kmem_cache_create("bib_nodes",
			sizeof(struct tabled_bib),
			0, 0, NULL);
	if (!bib_cache)
		return -ENOMEM;

	session_cache = kmem_cache_create("session_nodes",
			sizeof(struct tabled_session),
			0, 0, NULL);
	if (!session_cache) {
		kmem_cache_destroy(bib_cache);
		return -ENOMEM;
	}

	return 0;
}

void bib_destroy(void)
{
	kmem_cache_destroy(bib_cache);
	kmem_cache_destroy(session_cache);
}

static enum session_fate just_die(struct session_entry *session, void *arg)
{
	return FATE_RM;
}

static void init_expirer(struct expire_timer *expirer,
		session_timer_type type,
		fate_cb fate_cb)
{
	INIT_LIST_HEAD(&expirer->sessions);
	expirer->type = type;
	expirer->decide_fate_cb = fate_cb;
}

static void init_table(struct bib_table *table, fate_cb est_cb)
{
	table->tree6 = RB_ROOT;
	table->tree4 = RB_ROOT;
	table->session_count = 0;
	spin_lock_init(&table->lock);
	init_expirer(&table->est_timer, SESSION_TIMER_EST, est_cb);
	init_expirer(&table->trans_timer, SESSION_TIMER_TRANS, just_die);
	/* TODO "just_die"? what about the stored packet? */
	init_expirer(&table->syn4_timer, SESSION_TIMER_SYN4, just_die);
	table->pkt_count = 0;
	table->pkt_queue = NULL; /* Will be patched later; see caller. */
}

struct bib *bib_create(void)
{
	struct bib *db;

	db = wkmalloc(struct bib, GFP_KERNEL);
	if (!db)
		return NULL;

	init_table(&db->udp, just_die);
	init_table(&db->tcp, tcp_est_expire_cb);
	init_table(&db->icmp, just_die);

	db->tcp.pkt_queue = pktqueue_create();
	if (!db->tcp.pkt_queue) {
		wkfree(struct bib, db);
		return NULL;
	}

	kref_init(&db->refs);

	return db;
}

void bib_get(struct bib *db)
{
	kref_get(&db->refs);
}

/**
 * Potentially includes a laggy packet fetch; please do not hold spinlocks while
 * calling this function!
 */
static void release_session(struct rb_node *node, void *arg)
{
	struct tabled_session *session = node2session(node);

	if (session->stored) {
		/* icmp64_send_skb(session->stored, ICMPERR_PORT_UNREACHABLE, 0); */
		kfree_skb(session->stored);
	}

	free_session(session);
}

/**
 * Potentially includes laggy packet fetches; please do not hold spinlocks while
 * calling this function!
 */
static void release_bib_entry(struct rb_node *node, void *arg)
{
	struct tabled_bib *bib = bib4_entry(node);
	rbtree_clear(&bib->sessions, release_session, NULL);
	free_bib(bib);
}

static void release_bib(struct kref *refs)
{
	struct bib *db;
	db = container_of(refs, struct bib, refs);

	/*
	 * The trees share the entries, so only one tree of each protocol
	 * needs to be emptied.
	 */
	rbtree_clear(&db->udp.tree4, release_bib_entry, NULL);
	rbtree_clear(&db->tcp.tree4, release_bib_entry, NULL);
	rbtree_clear(&db->icmp.tree4, release_bib_entry, NULL);

	pktqueue_destroy(db->tcp.pkt_queue);

	wkfree(struct bib, db);
}

void bib_put(struct bib *db)
{
	kref_put(&db->refs, release_bib);
}

/*
 * TODO this is happening in-spinlock. Really necessary?
 */
static void log_bib(struct bib_state *state,
		struct tabled_bib *bib,
		char *action)
{
	struct timeval tval;
	struct tm t;

	if (!state->globals->bib_logging)
		return;

	do_gettimeofday(&tval);
	time_to_tm(tval.tv_sec, 0, &t);
	log_info("%ld/%d/%d %d:%d:%d (GMT) - %s %pI6c#%u to %pI4#%u (%s)",
			1900 + t.tm_year, t.tm_mon + 1, t.tm_mday,
			t.tm_hour, t.tm_min, t.tm_sec, action,
			&bib->src6.l3, bib->src6.l4,
			&bib->src4.l3, bib->src4.l4,
			l4proto_to_string(bib->proto));
}

static void log_new_bib(struct bib_state *state, struct tabled_bib *bib)
{
	return log_bib(state, bib, "Mapped");
}

static void log_session(struct bib_state *state,
		struct tabled_session *session,
		char *action)
{
	struct timeval tval;
	struct tm t;

	if (!state->globals->session_logging)
		return;

	do_gettimeofday(&tval);
	time_to_tm(tval.tv_sec, 0, &t);
	log_info("%ld/%d/%d %d:%d:%d (GMT) - %s %pI6c#%u|%pI6c#%u|"
			"%pI4#%u|%pI4#%u|%s",
			1900 + t.tm_year, t.tm_mon + 1, t.tm_mday,
			t.tm_hour, t.tm_min, t.tm_sec, action,
			&session->bib->src6.l3, session->bib->src6.l4,
			&session->dst6.l3, session->dst6.l4,
			&session->bib->src4.l3, session->bib->src4.l4,
			&session->dst4.l3, session->dst4.l4,
			l4proto_to_string(session->bib->proto));
}

static void log_new_session(struct bib_state *state,
		struct tabled_session *session)
{
	return log_session(state, session, "Added session");
}

/**
 * This function does not return a result because whatever needs to happen later
 * needs to happen regardless of probe status.
 *
 * This function does not actually send the probe; it merely prepares it so the
 * caller can commit to sending it after releasing the spinlock.
 */
static void handle_probe(struct bib_state *state,
		struct list_head *probes,
		struct tabled_session *session,
		struct session_entry *tmp)
{
	struct probing_session *probe;

	if (WARN(!probes, "Probe needed but caller doesn't support it"))
		goto discard_probe;

	/*
	 * Why add a dummy session instead of the real one?
	 * In the case of TCP probes it's because the real session's list hook
	 * must remain attached to the database.
	 * In the case of ICMP errors it's because the fact that a session
	 * removal can cascade into a BIB entry removal really complicates
	 * things.
	 * This way requires this malloc but it's otherwise very clean.
	 */
	probe = wkmalloc(struct probing_session, GFP_ATOMIC);
	if (!probe)
		goto discard_probe;

	probe->session = *tmp;
	if (session->stored) {
		probe->skb = session->stored;
		session->stored = NULL;
		state->table->pkt_count--;
	} else {
		probe->skb = NULL;
	}
	list_add(&probe->list_hook, probes);
	return;

discard_probe:
	/*
	 * We're going to have to pretend that we sent it anyway; a probe
	 * failure should not prevent the state from evolving from V4 INIT and
	 * we do not want that massive thing to linger in the database anymore,
	 * especially if we failed due to a memory allocation.
	 */
	kill_stored_pkt(state, session);
}

static void rm(struct bib_state *state,
		struct list_head *probes,
		struct tabled_session *session,
		struct session_entry *tmp)
{
	struct tabled_bib *bib = session->bib;

	if (session->stored)
		handle_probe(state, probes, session, tmp);

	rb_erase(&session->tree_hook, &bib->sessions);
	list_del(&session->list_hook);
	log_session(state, session, "Forgot session");
	free_session(session);
	state->table->session_count--;

	if (!bib->is_static && RB_EMPTY_ROOT(&bib->sessions)) {
		rb_erase(&bib->hook6, &state->table->tree6);
		rb_erase(&bib->hook4, &state->table->tree4);
		log_bib(state, bib, "Forgot");
		free_bib(bib);
	}
}

static void handle_fate_timer(struct tabled_session *session,
		struct expire_timer *timer)
{
	session->update_time = jiffies;
	session->expirer = timer;
	list_del(&session->list_hook);
	list_add_tail(&session->list_hook, &timer->sessions);
}

static int queue_unsorted_session(struct bib_state *state,
		struct tabled_session *session,
		session_timer_type timer_type,
		bool remove_first)
{
	struct expire_timer *expirer;
	struct list_head *list;
	struct list_head *cursor;
	struct tabled_session *old;

	switch (timer_type) {
	case SESSION_TIMER_EST:
		expirer = &state->table->est_timer;
		break;
	case SESSION_TIMER_TRANS:
		expirer = &state->table->trans_timer;
		break;
	case SESSION_TIMER_SYN4:
		expirer = &state->table->syn4_timer;
		break;
	default:
		log_warn_once("incoming joold session's timer (%d) is unknown.",
				timer_type);
		return -EINVAL;
	}

	list = &expirer->sessions;
	for (cursor = list->prev; cursor != list; cursor = cursor->prev) {
		old = list_entry(cursor, struct tabled_session, list_hook);
		if (old->update_time < session->update_time)
			break;
	}

	if (remove_first)
		list_del(&session->list_hook);
	list_add(&session->list_hook, cursor);
	session->expirer = expirer;
	return 0;
}

/**
 * Assumes result->session has been set (result->session_set is true).
 */
static int decide_fate(struct collision_cb *cb,
		struct bib_state *state,
		struct tabled_session *session,
		struct list_head *probes)
{
	struct session_entry tmp;
	enum session_fate fate;

	if (!cb)
		return 0;

	tstose(state, session, &tmp);
	fate = cb->cb(&tmp, cb->arg);

	/* The callback above is entitled to tweak these fields. */
	session->state = tmp.state;
	session->update_time = tmp.update_time;
	if (!tmp.has_stored)
		kill_stored_pkt(state, session);
	/* Also the expirer, which is down below. */

	switch (fate) {
	case FATE_TIMER_EST:
		handle_fate_timer(session, &state->table->est_timer);
		break;

	case FATE_PROBE:
		/* TODO ICMP errors aren't supposed to drop down to TRANS. */
		handle_probe(state, probes, session, &tmp);
		/* Fall through. */
	case FATE_TIMER_TRANS:
		handle_fate_timer(session, &state->table->trans_timer);
		break;

	case FATE_RM:
		rm(state, probes, session, &tmp);
		break;

	case FATE_PRESERVE:
		break;
	case FATE_DROP:
		return -EINVAL;

	case FATE_TIMER_SLOW:
		/*
		 * Nothing to do with the return value.
		 * If timer type was invalid, well don't change the expirer.
		 * We left a warning in the log.
		 */
		queue_unsorted_session(state, session, tmp.timer_type, true);
		break;
	}

	return 0;
}

/**
 * Sends a probe packet to @session's IPv6 endpoint, to trigger a confirmation
 * ACK if the connection is still alive.
 *
 * RFC 6146 page 30.
 *
 * Best if not called with spinlocks held.
 */
static void send_probe_packet(struct session_entry *session)
{
	struct sk_buff *skb;
	struct ipv6hdr *iph;
	struct tcphdr *th;

	unsigned int l3_hdr_len = sizeof(*iph);
	unsigned int l4_hdr_len = sizeof(*th);

	skb = alloc_skb(LL_MAX_HEADER + l3_hdr_len + l4_hdr_len, GFP_ATOMIC);
	if (!skb) {
		log_debug("Could now allocate a probe packet.");
		log_debug("A TCP connection will probably break.");
		return;
	}

	skb_reserve(skb, LL_MAX_HEADER);
	skb_put(skb, l3_hdr_len + l4_hdr_len);
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb_set_transport_header(skb, l3_hdr_len);

	iph = ipv6_hdr(skb);
	iph->version = 6;
	iph->priority = 0;
	iph->flow_lbl[0] = 0;
	iph->flow_lbl[1] = 0;
	iph->flow_lbl[2] = 0;
	iph->payload_len = cpu_to_be16(l4_hdr_len);
	iph->nexthdr = NEXTHDR_TCP;
	iph->hop_limit = 255;
	iph->saddr = session->dst6.l3;
	iph->daddr = session->src6.l3;

	th = tcp_hdr(skb);
	th->source = cpu_to_be16(session->dst6.l4);
	th->dest = cpu_to_be16(session->src6.l4);
	th->seq = htonl(0);
	th->ack_seq = htonl(0);
	th->res1 = 0;
	th->doff = l4_hdr_len / 4;
	th->fin = 0;
	th->syn = 0;
	th->rst = 0;
	th->psh = 0;
	th->ack = 1;
	th->urg = 0;
	th->ece = 0;
	th->cwr = 0;
	th->window = htons(8192);
	th->check = 0;
	th->urg_ptr = 0;

	th->check = csum_ipv6_magic(&iph->saddr, &iph->daddr, l4_hdr_len,
			IPPROTO_TCP, csum_partial(th, l4_hdr_len, 0));
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	sendpkt_send_skb(skb);
}

/**
 * Sends all the probes and ICMP errors listed in @probes.
 */
static void post_fate(struct list_head *probes)
{
	struct probing_session *probe;
	struct probing_session *tmp;

	list_for_each_entry_safe(probe, tmp, probes, list_hook) {
		if (probe->skb) {
			/* The "probe" is not a probe; it's an ICMP error. */
			/* icmp64_send_skb(probe->skb, ICMPERR_PORT_UNREACHABLE, 0); */
			kfree_skb(probe->skb);
		} else {
			/* Actual TCP probe. */
			send_probe_packet(&probe->session);
		}
		wkfree(struct probing_session, probe);
	}
}

static void commit_bib_add(struct slot_group *slots)
{
	treeslot_commit(&slots->bib6);
	treeslot_commit(&slots->bib4);
}

static void commit_session_add(struct bib_state *state, struct tree_slot *slot)
{
	treeslot_commit(slot);
	state->table->session_count++;
}

static void attach_timer(struct tabled_session *session,
		struct expire_timer *expirer)
{
	session->update_time = jiffies;
	session->expirer = expirer;
	list_add_tail(&session->list_hook, &expirer->sessions);
}

static int compare_src6(struct tabled_bib *a, struct ipv6_transport_addr *b)
{
	return taddr6_compare(&a->src6, b);
}

static int compare_src6_rbnode(struct rb_node *a, struct rb_node *b)
{
	return taddr6_compare(&bib6_entry(a)->src6, &bib6_entry(b)->src6);
}

static int compare_src4(struct tabled_bib const *a,
		struct ipv4_transport_addr const *b)
{
	return taddr4_compare(&a->src4, b);
}

static int compare_src4_rbnode(struct rb_node *a, struct rb_node *b)
{
	return taddr4_compare(&bib4_entry(a)->src4, &bib4_entry(b)->src4);
}

static int compare_dst4(struct tabled_session *a, struct tabled_session *b)
{
	return taddr4_compare(&a->dst4, &b->dst4);
}

static struct tabled_bib *find_bib6(struct bib_table *table,
		struct ipv6_transport_addr *addr)
{
	return rbtree_find(addr, &table->tree6, compare_src6, struct tabled_bib,
			hook6);
}

static struct tabled_bib *find_bib4(struct bib_table *table,
		struct ipv4_transport_addr *addr)
{
	return rbtree_find(addr, &table->tree4, compare_src4,
			struct tabled_bib, hook4);
}

static struct tabled_bib *find_bibtree6_slot(struct bib_table *table,
		struct tabled_bib *new,
		struct tree_slot *slot)
{
	struct rb_node *collision;
	collision = rbtree_find_slot(&new->hook6, &table->tree6,
			compare_src6_rbnode, slot);
	return bib6_entry(collision);
}

static struct tabled_bib *find_bibtree4_slot(struct bib_table *table,
		struct tabled_bib *new,
		struct tree_slot *slot)
{
	struct rb_node *collision;
	collision = rbtree_find_slot(&new->hook4, &table->tree4,
			compare_src4_rbnode, slot);
	return bib4_entry(collision);
}

/**
 * Attempts to find the slot where @new would be inserted if you wanted to add
 * it to @bib's session tree.
 *
 * On success:
 * - Initializes @slots as the place (in @bib's session tree) where @new would
 *   be inserted if you wanted to do so.
 * - Returns NULL.
 *
 * If @session collides with @bib's session S:
 * - @slot is undefined.
 * - S is returned.
 *
 * As a side effect, @allow will tell you whether the entry is allowed to be
 * added to the tree if address-dependent filtering is enabled. Send NULL if you
 * don't care about that.
 *
 * Please notice: This searches via @new's dst4, *not* dst6. @new *must* carry
 * an initialized dst4.
 */
static struct tabled_session *find_session_slot(struct tabled_bib *bib,
		struct tabled_session *new,
		bool *allow,
		struct tree_slot *slot)
{
	struct tabled_session *session;
	struct rb_node *node;
	int comparison;

	treeslot_init(slot, &bib->sessions, &new->tree_hook);
	node = bib->sessions.rb_node;
	if (allow)
		*allow = false;

	while (node) {
		session = node2session(node);
		comparison = compare_dst4(session, new);

		if (allow && session->dst4.l3.s_addr == new->dst4.l3.s_addr)
			*allow = true;

		slot->parent = node;
		if (comparison < 0) {
			slot->rb_link = &node->rb_right;
			node = node->rb_right;
		} else if (comparison > 0) {
			slot->rb_link = &node->rb_left;
			node = node->rb_left;
		} else {
			return session;
		}
	}

	return NULL;
}

static int alloc_bib_session(struct bib_session_tuple *tuple)
{
	tuple->bib = alloc_bib(GFP_ATOMIC);
	if (!tuple->bib)
		return -ENOMEM;

	tuple->session = alloc_session(GFP_ATOMIC);
	if (!tuple->session) {
		free_bib(tuple->bib);
		return -ENOMEM;
	}

	return 0;
}

static int create_bib_session6(struct xlation *state,
		struct bib_session_tuple *tuple,
		struct ipv4_transport_addr *dst4,
		tcp_state tstate)
{
	struct tuple *tuple6 = &state->in.tuple;
	int error;

	error = alloc_bib_session(tuple);
	if (error)
		return enomem(state);

	/*
	 * Hooks, expirer fields and session->bib are left uninitialized since
	 * they depend on database knowledge.
	 */

	tuple->bib->src6 = tuple6->src.addr6;
	/*
	 * src4 is left uninitialized on purpose.
	 * It needs to be inferred later by comparing the masks and the existing
	 * BIB entries.
	 */
	tuple->bib->proto = tuple6->l4_proto;
	tuple->bib->is_static = false;
	tuple->bib->sessions = RB_ROOT;
	tuple->session->dst6 = tuple6->dst.addr6;
	tuple->session->dst4 = *dst4;
	tuple->session->state = tstate;
	tuple->session->stored = NULL;
	return 0;
}

static struct tabled_session *create_session4(struct tuple *tuple4,
		struct ipv6_transport_addr *dst6,
		tcp_state state)
{
	struct tabled_session *session;

	session = alloc_session(GFP_ATOMIC);
	if (!session)
		return NULL;

	/*
	 * Hooks, expirer fields and session->bib are left uninitialized since
	 * they depend on database knowledge.
	 */
	session->dst6 = *dst6;
	session->dst4 = tuple4->src.addr4;
	session->state = state;
	session->stored = NULL;
	return session;
}

/**
 * Boilerplate code to finish hanging @new->session (and potentially @new->bib
 * as well) on one af @table's trees. 6-to-4 direction.
 *
 * It assumes @slots already describes the tree containers where the entries are
 * supposed to be added.
 */
static void commit_add6(struct bib_add6_args *args,
		struct expire_timer *expirer,
		struct bib_session *result)
{
	struct bib_session_tuple *old= &args->old;
	struct bib_session_tuple *new = &args->new;

	new->session->bib = old->bib ? : new->bib;
	commit_session_add(&args->state, &args->slots.session);
	attach_timer(new->session, expirer);
	log_new_session(&args->state, new->session);
	tstobs(&args->state, new->session, result);
	new->session = NULL; /* Do not free! */

	if (!old->bib) {
		commit_bib_add(&args->slots);
		log_new_bib(&args->state, new->bib);
		new->bib = NULL; /* Do not free! */
	}
}

/**
 * Boilerplate code to finish hanging *@new on one af @table's trees.
 * 4-to-6 direction.
 *
 * It assumes @slot already describes the tree container where the session is
 * supposed to be added.
 */
static void commit_add4(struct bib_add4_args *args,
		struct expire_timer *expirer,
		struct bib_session *result)
{
	struct tabled_session *session = args->new;

	session->bib = args->old.bib;
	commit_session_add(&args->state, &args->session_slot);
	attach_timer(session, expirer);
	log_new_session(&args->state, session);
	tstobs(&args->state, session, result);
	args->new = NULL; /* Do not free! */
}

struct detach_args {
	struct bib_table *table;
	struct sk_buff *probes;
	unsigned int detached;
};

static void detach_session(struct rb_node *node, void *arg)
{
	struct tabled_session *session = node2session(node);
	struct detach_args *args = arg;

	list_del(&session->list_hook);
	if (session->stored)
		args->table->pkt_count--;
	args->detached++;
}

static unsigned int detach_sessions(struct bib_table *table,
		struct tabled_bib *bib)
{
	struct detach_args arg = { .table = table, .detached = 0, };
	rbtree_foreach(&bib->sessions, detach_session, &arg);
	return arg.detached;
}

static void detach_bib(struct bib_table *table, struct tabled_bib *bib)
{
	rb_erase(&bib->hook6, &table->tree6);
	rb_erase(&bib->hook4, &table->tree4);
	table->session_count -= detach_sessions(table, bib);
}

static void add_to_delete_list(struct bib_delete_list *list, struct rb_node *node)
{
	node->rb_right = list->first;
	list->first = node;
}

static void commit_delete_list(struct bib_delete_list *list)
{
	struct rb_node *node;
	struct rb_node *next;

	for (node = list->first; node; node = next) {
		next = node->rb_right;
		release_bib_entry(node, NULL);
	}
}

/**
 * Tests whether @predecessor's immediate succesor tree slot is a suitable
 * placeholder for @bib. Returns the colliding node.
 *
 * (That is, returns NULL on success, a collision on failure.)
 *
 * In other words:
 * Assumes that @predecessor belongs to @table's v4 tree and that it is @bib's
 * predecessor. (ie. @predecessor's transport address is @bib's transport
 * address - 1.) You want to test whether @bib can be inserted to the tree.
 * If @predecessor's succesor collides with @bib (ie. it has @bib's v4 address),
 * it returns the colliding succesor.
 * If @predecessor's succesor does not collide with @bib, it returns NULL and
 * initializes @slot so you can actually add @bib to the tree.
 */
static struct tabled_bib *try_next(struct bib_state *state,
		struct tabled_bib *predecessor,
		struct tabled_bib *bib,
		struct tree_slot *slot)
{
	struct tabled_bib *next;

	next = bib4_entry(rb_next(&predecessor->hook4));
	if (!next) {
		/* There is no succesor and therefore no collision. */
		slot->tree = &state->table->tree4;
		slot->entry = &bib->hook4;
		slot->parent = &predecessor->hook4;
		slot->rb_link = &slot->parent->rb_right;
		return NULL;
	}

	if (taddr4_equals(&next->src4, &bib->src4))
		return next; /* Next is yet another collision. */

	slot->tree = &state->table->tree4;
	slot->entry = &bib->hook4;
	if (predecessor->hook4.rb_right) {
		slot->parent = &next->hook4;
		slot->rb_link = &slot->parent->rb_left;
	} else {
		slot->parent = &predecessor->hook4;
		slot->rb_link = &slot->parent->rb_right;
	}
	return NULL;
}

/**
 * This is this function in pseudocode form:
 *
 * 	// wraps around until offset - 1
 * 	foreach (mask in @masks starting from some offset)
 * 		if (mask is not taken by an existing BIB entry from @table)
 * 			init the new BIB entry, @bib, using mask
 * 			init @slot as the tree slot where @bib should be added
 * 			return success (0)
 * 	return failure (-ENOENT)
 *
 */
static int find_available_mask(struct bib_state *state,
		struct mask_domain *masks,
		struct tabled_bib *bib,
		struct tree_slot *slot)
{
	struct tabled_bib *collision = NULL;
	bool consecutive;
	int error;

	/*
	 * We're going to assume the masks are generally consecutive.
	 * I think it's a fair assumption until someone requests otherwise as a
	 * new feature.
	 * This allows us to find an unoccupied mask with minimal further tree
	 * traversal.
	 */
	do {
		error = mask_domain_next(masks, &bib->src4, &consecutive);
		if (error)
			return error;

		/*
		 * Just for the sake of clarity:
		 * @consecutive is never true on the first iteration.
		 */
		collision = consecutive
				? try_next(state, collision, bib, slot)
				: find_bibtree4_slot(state->table, bib, slot);

	} while (collision);

	return 0;
}

static int upgrade_pktqueue_session(struct bib_state *state,
		struct mask_domain *masks,
		struct bib_session_tuple *new,
		struct bib_session_tuple *old)
{
	struct pktqueue_session *sos; /* "simultaneous open" session */
	struct tabled_bib *bib;
	struct tabled_bib *collision;
	struct tabled_session *session;
	struct tree_slot bib_slot6;
	struct tree_slot bib_slot4;
	int error;

	if (new->bib->proto != L4PROTO_TCP)
		return -ESRCH;

	sos = pktqueue_find(state->table->pkt_queue, &new->session->dst6, masks);
	if (!sos)
		return -ESRCH;
	state->table->pkt_count--;

	if (!masks) {
		/*
		 * This happens during joold adds. It's a lost cause.
		 *
		 * The point of SO is that the v4 node decides session [*, dst6,
		 * src4, dst4] and the first v6 packet needing a new mask that
		 * matches that session keeps it.
		 *
		 * But we're not synchronizing pktqueue sessions, because we
		 * want to keep joold as simple as possible (which is not simple
		 * enough), at least so long as it remains a niche thing.
		 *
		 * So if one Jool instance gets the v4 SO packet and some other
		 * instance gets the v6 SO packet, the latter will choose a
		 * random src4 and mess up the SO. That situation is this if.
		 * Our reaction is to go like "whatever" and pretend that we
		 * never received the v4 packet.
		 *
		 * One might argue that we should send the ICMP error when this
		 * happens. But that doesn't yield satisfactory behavior either;
		 * The SO failed anyway. To fix this properly we would need to
		 * sync the pktqueue sessions. Combine that with the fact that
		 * sending the ICMP error would be a pain in the ass (because we
		 * want to do it outside of the spinlock, and we don't want to
		 * send it if the random src4 selected happens to match the
		 * stored session), and the result is a big fat meh. I really
		 * don't want to do it.
		 *
		 * The admin signed a best-effort contract when s/he enabled
		 * joold anyway. And this is only a problem in active-active
		 * scenarios.
		 */
		pktqueue_put_node(sos);
		return -ESRCH;
	}

	log_debug("Simultaneous Open!");
	/*
	 * We're going to pretend that @sos has been a valid V4 INIT session all
	 * along.
	 */
	error = alloc_bib_session(old);
	if (error) {
		pktqueue_put_node(sos);
		return error;
	}

	bib = old->bib;
	session = old->session;

	bib->src6 = new->bib->src6;
	bib->src4 = sos->src4;
	bib->proto = L4PROTO_TCP;
	bib->is_static = false;
	bib->sessions = RB_ROOT;

	session->dst6 = sos->dst6;
	session->dst4 = sos->dst4;
	session->state = V4_INIT;
	session->bib = bib;
	session->update_time = jiffies;
	session->stored = NULL;

	/*
	 * This *has* to work. src6 wasn't in the database because we just
	 * looked it up and src4 wasn't either because pktqueue had it.
	 */
	collision = find_bibtree6_slot(state->table, bib, &bib_slot6);
	if (WARN(collision, "BIB entry was and then wasn't in the v6 tree."))
		goto trainwreck;
	collision = find_bibtree4_slot(state->table, bib, &bib_slot4);
	if (WARN(collision, "BIB entry was and then wasn't in the v4 tree."))
		goto trainwreck;
	treeslot_commit(&bib_slot6);
	treeslot_commit(&bib_slot4);

	rb_link_node(&session->tree_hook, NULL, &bib->sessions.rb_node);
	rb_insert_color(&session->tree_hook, &bib->sessions);
	attach_timer(session, &state->table->syn4_timer);

	pktqueue_put_node(sos);

	log_new_bib(state, bib);
	log_new_session(state, session);
	return 0;

trainwreck:
	pktqueue_put_node(sos);
	free_bib(bib);
	free_session(session);
	return -EINVAL;
}

static bool issue216_needed(struct mask_domain *masks,
		struct bib_session_tuple *old)
{
	if (!masks)
		return false;
	return mask_domain_is_dynamic(masks)
			&& !mask_domain_matches(masks, &old->bib->src4);
}

/**
 * This is a find and an add at the same time, for both @new->bib and
 * @new->session.
 *
 * If @new->bib needs to be added, initializes @slots->bib*.
 * If @new->session needs to be added, initializes @slots->session.
 * If @new->bib collides, you will find the collision in @old->bib.
 * If @new->session collides, you will find the collision in @old->session.
 *
 * @masks will be used to init @new->bib.src4 if applies.
 */
static int find_bib_session6(struct xlation *xstate, struct bib_add6_args *args)
{
	struct bib_session_tuple *old = &args->old;
	struct bib_session_tuple *new = &args->new;
	struct slot_group *slots = &args->slots;
	int error;

	/*
	 * Please be careful around this function. All it wants to do is
	 * find/add, but it is constrained by several requirements at the same
	 * time:
	 *
	 * 1. If @new->bib->proto is ICMP (ie. 3-tuple), then
	 *    @new->session->dst4.l4 is invalid and needs to be patched. Though
	 *    it cannot be patched until we acquire a valid BIB entry.
	 *    (dst4.l4 is just fat that should not be used in 3-tuple
	 *    translation code, but a chunk of Jool assumes that
	 *    dst4.l4 == dst6.l4 in 5-tuples and dst4.l4 == src4.l4 in
	 *    3-tuples.)
	 * 2. Never mind; @args->masks can no longer be NULL.
	 *
	 * See below for more stuff.
	 */

	old->bib = find_bibtree6_slot(args->state.table, new->bib, &slots->bib6);
	if (old->bib) {
		if (!issue216_needed(args->masks, old)) {
			if (new->bib->proto == L4PROTO_ICMP)
				new->session->dst4.l4 = old->bib->src4.l4;

			old->session = find_session_slot(old->bib, new->session,
					NULL, &slots->session);
			return 0; /* Typical happy path for existing sessions */
		}

		/*
		 * Issue #216:
		 * If pool4 was empty (when @masks was generated) and the BIB
		 * entry's IPv4 address is no longer a mask candidate, drop the
		 * BIB entry and recompute it from scratch.
		 * https://github.com/NICMx/Jool/issues/216
		 */
		log_debug("Issue #216.");
		detach_bib(args->state.table, old->bib);
		add_to_delete_list(&args->rm_list, &old->bib->hook4);

		/*
		 * The detaching above might have involved a rebalance.
		 * I believe that completely invalidates the bib6 slot.
		 * Tough luck; we'll need another lookup.
		 * At least this only happens on empty pool4s. (Low traffic.)
		 */
		old->bib = find_bibtree6_slot(args->state.table, new->bib, &slots->bib6);
		if (WARN(old->bib, "Found a BIB entry I just removed!"))
			return eunknown6(xstate, -EINVAL);

	} else {
		/*
		 * No BIB nor session in the main database? Try the SO
		 * sub-database.
		 */
		error = upgrade_pktqueue_session(&args->state, args->masks, new, old);
		if (!error)
			return 0; /* Unusual happy path for existing sessions */
	}

	/*
	 * In case you're tweaking this function: By this point, old->bib has to
	 * be NULL and slots->bib6 has to be a valid potential tree slot. We're
	 * now in create-new-BIB-and-session mode.
	 * Time to worry about slots->bib4.
	 *
	 * (BTW: If old->bib is NULL, then old->session is also supposed to be
	 * NULL.)
	 */

	error = find_available_mask(&args->state, args->masks, new->bib,
			&slots->bib4);
	if (error) {
		if (WARN(error != -ENOENT, "Unknown error: %d", error))
			return eunknown6(xstate, error);
		/*
		 * TODO the rate limit might be a bit of a problem.
		 * If both mark 0 and mark 1 are running out of
		 * addresses, only one of them will be logged.
		 * The problem is that remembering which marks have been
		 * logged might get pretty ridiculous.
		 * I don't think it's too bad because there will still
		 * be at least one message every minute.
		 * Also, it's better than what we had before. (Not
		 * logging the offending mark.)
		 * Might not be worth fixing since #175 is in the radar.
		 */
		log_warn_once("I'm running out of pool4 addresses for mark %u.",
				mask_domain_get_mark(args->masks));
		return breakdown(xstate, JOOL_MIB_POOL4_EXHAUSTED, error);
	}

	if (new->bib->proto == L4PROTO_ICMP)
		new->session->dst4.l4 = new->bib->src4.l4;

	/* Ok, time to worry about slots->session now. */

	treeslot_init(&slots->session, &new->bib->sessions,
			&new->session->tree_hook);
	old->session = NULL;

	return 0; /* Happy path for new sessions */
}

/**
 * TODO you know what, there's probably not much reason to compute @dst4
 * outside anymore. Just bring it in.
 *
 * @db current BIB & session database.
 * @masks Should a BIB entry be created, its IPv4 address mask will be allocated
 *     from one of these candidates.
 * @tuple6 The connection that you want to mask.
 * @dst4 translated version of @tuple.dst.addr6.
 * @result A copy of the resulting BIB entry and session from the database will
 *     be placed here. (if not NULL)
 */
int bib_add6(struct xlation *xstate,
		struct mask_domain *masks,
		struct ipv4_transport_addr *dst4)
{
	struct bib_add6_args args;
	int error;

	args.state.table = get_table(xstate->jool.bib, xstate->in.tuple.l4_proto);
	if (!args.state.table)
		return einval(xstate, JOOL_MIB_UNKNOWN6);
	args.state.globals = &xstate->jool.global->cfg.bib;
	args.masks = masks;
	args.rm_list.first = NULL;

	/*
	 * We might have a lot to do. This function may index three RB-trees
	 * so spinlock time is tight.
	 *
	 * (That's 3 potential lookups (2 guaranteed) and 3 potential
	 * rebalances, though at least one of the trees is usually minuscule.)
	 *
	 * There's also the potential need for a port allocation, which in the
	 * worst case is an unfortunate full traversal of @masks.
	 *
	 * Let's start by allocating and initializing the objects as much as we
	 * can, even if we end up not needing them.
	 */
	error = create_bib_session6(xstate, &args.new, dst4, ESTABLISHED);
	if (error)
		return error;

	spin_lock_bh(&args.state.table->lock); /* Here goes... */

	error = find_bib_session6(xstate, &args);
	if (error)
		goto end;

	if (args.old.session) { /* Session already exists. */
		handle_fate_timer(args.old.session, &args.state.table->est_timer);
		tstobs(&args.state, args.old.session, &xstate->entries);
		goto end;
	}

	/* New connection; add the session. (And maybe the BIB entry as well) */
	commit_add6(&args, &args.state.table->est_timer, &xstate->entries);
	/* Fall through */

end:
	spin_unlock_bh(&args.state.table->lock);

	if (args.new.bib)
		free_bib(args.new.bib);
	if (args.new.session)
		free_session(args.new.session);
	commit_delete_list(&args.rm_list);

	return error;
}

static void find_bib_session4(struct xlation *state,
		struct bib_add4_args *args,
		bool *allow)
{
	struct bib_session_tuple *old = &args->old;

	old->bib = find_bib4(args->state.table, &state->in.tuple.dst.addr4);
	old->session = old->bib
			? find_session_slot(old->bib, args->new, allow,
					&args->session_slot)
			: NULL;
}

/**
 * See @bib_add6.
 */
int bib_add4(struct xlation *xstate, struct ipv6_transport_addr *dst6)
{
	struct bib_add4_args args;
	bool allow;
	int error = 0;

	args.state.table = get_table(xstate->jool.bib, xstate->in.tuple.l4_proto);
	if (!args.state.table)
		return eunknown4(xstate, -EINVAL);
	args.state.globals = &xstate->jool.global->cfg.bib;

	args.new = create_session4(&xstate->in.tuple, dst6, ESTABLISHED);
	if (!args.new)
		return enomem(xstate);

	spin_lock_bh(&args.state.table->lock);

	find_bib_session4(xstate, &args, &allow);

	if (args.old.session) {
		handle_fate_timer(args.old.session, &args.state.table->est_timer);
		tstobs(&args.state, args.old.session, &xstate->entries);
		goto end;
	}

	if (!args.old.bib) {
		error = esrch(xstate, JOOL_MIB_NO_BIB);
		goto end;
	}

	/* Address-Dependent Filtering. */
	if (args.state.globals->drop_by_addr && !allow) {
		error = eperm(xstate, JOOL_MIB_ADF);
		goto end;
	}

	/* Ok, no issues; add the session. */
	commit_add4(&args, &args.state.table->est_timer, &xstate->entries);
	/* Fall through */

end:
	spin_unlock_bh(&args.state.table->lock);
	if (args.new)
		free_session(args.new);
	return error;
}

/**
 * Note: This particular incarnation of fate_cb is not prepared to return
 * FATE_PROBE.
 */
int bib_add_tcp6(struct xlation *xstate,
		struct mask_domain *masks,
		struct ipv4_transport_addr *dst4,
		struct collision_cb *cb)
{
	struct bib_add6_args args;
	int error;

	if (WARN(xstate->in.tuple.l4_proto != L4PROTO_TCP, "Incorrect l4 proto in TCP handler."))
		return eunknown6(xstate, -EINVAL);

	error = create_bib_session6(xstate, &args.new, dst4, V6_INIT);
	if (error)
		return error;

	args.state.table = &xstate->jool.bib->tcp;
	args.state.globals = &xstate->jool.global->cfg.bib;
	args.masks = masks;
	args.rm_list.first = NULL;

	spin_lock_bh(&args.state.table->lock);

	error = find_bib_session6(xstate, &args);
	if (error)
		goto end;

	if (args.old.session) {
		/* All states except CLOSED. */
		error = decide_fate(cb, &args.state, args.old.session, NULL);
		if (error)
			einval(xstate, JOOL_MIB_TCP_SM);
		else
			tstobs(&args.state, args.old.session, &xstate->entries);
		goto end;
	}

	/* CLOSED state beginning now. */

	if (!pkt_tcp_hdr(&xstate->in)->syn) {
		if (args.old.bib) {
			tbtobs(args.old.bib, &xstate->entries);
			error = 0;
		} else {
			log_debug("Packet is not SYN and lacks state.");
			error = einval(xstate, JOOL_MIB_NO_BIB);
		}
		goto end;
	}

	/* All exits up till now require @new.* to be deleted. */

	commit_add6(&args, &args.state.table->trans_timer, &xstate->entries);
	/* Fall through */

end:
	spin_unlock_bh(&args.state.table->lock);

	if (args.new.bib)
		free_bib(args.new.bib);
	if (args.new.session)
		free_session(args.new.session);
	commit_delete_list(&args.rm_list);

	return error;
}

/**
 * Note: This particular incarnation of fate_cb is not prepared to return
 * FATE_PROBE.
 */
int bib_add_tcp4(struct xlation *xstate,
		struct ipv6_transport_addr *dst6,
		struct collision_cb *cb)
{
	struct bib_add4_args args;
	int error;

	if (WARN(xstate->in.tuple.l4_proto != L4PROTO_TCP, "Incorrect l4 proto in TCP handler."))
		return eunknown4(xstate, -EINVAL);

	args.state.table = &xstate->jool.bib->tcp;
	args.state.globals = &xstate->jool.global->cfg.bib;
	args.new = create_session4(&xstate->in.tuple, dst6, V4_INIT);
	if (!args.new)
		return enomem(xstate);

	spin_lock_bh(&args.state.table->lock);

	find_bib_session4(xstate, &args, NULL);

	if (args.old.session) {
		/* All states except CLOSED. */
		error = decide_fate(cb, &args.state, args.old.session, NULL);
		if (error)
			einval(xstate, JOOL_MIB_TCP_SM);
		else
			tstobs(&args.state, args.old.session, &xstate->entries);
		goto end;
	}

	/* CLOSED state beginning now. */

	if (!pkt_tcp_hdr(&xstate->in)->syn) {
		if (args.old.bib) {
			tbtobs(args.old.bib, &xstate->entries);
			error = 0;
		} else {
			log_debug("Packet is not SYN and lacks state.");
			error = einval(xstate, JOOL_MIB_NO_BIB);
		}
		goto end;
	}

	if (args.state.globals->drop_external_tcp) {
		log_debug("Externally initiated TCP connections are prohibited.");
		error = eperm(xstate, JOOL_MIB_EXTERNAL_SYN_PROHIBITED);
		goto end;
	}

	if (!args.old.bib) {
		bool too_many;

		log_debug("Potential Simultaneous Open; storing type 1 packet.");
		too_many = args.state.table->pkt_count >= args.state.globals->max_stored_pkts;
		error = pktqueue_add(args.state.table->pkt_queue, &xstate->in, dst6, too_many);
		switch (error) {
		case -ESTOLEN:
			args.state.table->pkt_count++;
			jstat_inc(xstate->jool.stats, JOOL_MIB_SO1_STORED_PKT);
			goto end;
		case -EEXIST:
			log_debug("Simultaneous Open already exists.");
			eexist(xstate, JOOL_MIB_SO1_EXISTS);
			break;
		case -ENOSPC:
			enospc(xstate, JOOL_MIB_SO1_FULL);
			goto too_many_pkts;
		case -ENOMEM:
			enomem(xstate);
			break;
		default:
			WARN(1, "pktqueue_add() threw unknown error %d", error);
			eunknown4(xstate, error);
			break;
		}

		goto end;
	}

	error = 0;

	if (args.state.globals->drop_by_addr) {
		if (args.state.table->pkt_count >= args.state.globals->max_stored_pkts) {
			enospc(xstate, JOOL_MIB_SO2_FULL);
			goto too_many_pkts;
		}

		log_debug("Potential Simultaneous Open; storing type 2 packet.");
		args.new->stored = pkt_original_pkt(&xstate->in)->skb;
		error = -ESTOLEN;
		jstat_inc(xstate->jool.stats, JOOL_MIB_SO2_STORED_PKT);
		args.state.table->pkt_count++;
		/*
		 * Yes, fall through. No goto; we need to add this session.
		 * Notice that if you need to cancel before the spin unlock then
		 * you need to revert the packet storing above.
		 */
	}

	commit_add4(&args,
			args.new->stored
					? &args.state.table->syn4_timer
					: &args.state.table->trans_timer,
			&xstate->entries);
	/* Fall through */

end:
	spin_unlock_bh(&args.state.table->lock);

	if (args.new)
		free_session(args.new);

	return error;

too_many_pkts:
	spin_unlock_bh(&args.state.table->lock);
	free_session(args.new);
	log_debug("Too many Simultaneous Opens.");
	/* Fall back to assume there's no SO. */
	icmp64_send(&xstate->in, ICMPERR_PORT_UNREACHABLE, 0);
	return -EINVAL;
}

int bib_find(struct bib *db, struct tuple *tuple, struct bib_session *result)
{
	struct bib_entry tmp;
	int error;

	switch (tuple->l3_proto) {
	case L3PROTO_IPV6:
		error = bib_find6(db, tuple->l4_proto, &tuple->src.addr6, &tmp);
		break;
	case L3PROTO_IPV4:
		error = bib_find4(db, tuple->l4_proto, &tuple->dst.addr4, &tmp);
		break;
	default:
		WARN(true, "Unknown layer 3 protocol: %u", tuple->l3_proto);
		return -EINVAL;
	}

	if (error)
		return error;

	result->bib_set = true;
	result->session.src6 = tmp.ipv6;
	result->session.src4 = tmp.ipv4;
	result->session.proto = tmp.l4_proto;
	return 0;
}

static void __clean(struct expire_timer *expirer,
		struct bib_state *state,
		struct list_head *probes,
		unsigned int timeout)
{
	struct tabled_session *session;
	struct tabled_session *tmp;
	struct collision_cb cb;

	timeout = msecs_to_jiffies(1000 * timeout);

	cb.cb = expirer->decide_fate_cb;
	cb.arg = NULL;

	list_for_each_entry_safe(session, tmp, &expirer->sessions, list_hook) {
		/*
		 * "list" is sorted by expiration date,
		 * so stop on the first unexpired session.
		 */
		if (time_before(jiffies, session->update_time + timeout))
			break;
		decide_fate(&cb, state, session, probes);
	}
}

static void check_empty_expirer(struct expire_timer *expirer,
		struct bib_state *state,
		struct list_head *probes)
{
	if (WARN(!list_empty(&expirer->sessions), "Expirer is just a stand-in but has sessions."))
		__clean(expirer, state, probes, 0); /* Remove them anyway. */
}

/**
 * Forgets or downgrades (from EST to TRANS) old sessions.
 */
void bib_clean(struct bib *db, struct globals *globals)
{
	struct bib_state state;
	LIST_HEAD(probes);
	LIST_HEAD(icmps);

	state.table = &db->tcp;
	state.globals = &globals->bib;

	spin_lock_bh(&db->tcp.lock);
	__clean(&db->tcp.est_timer, &state, &probes, globals->bib.ttl.tcp_est);
	__clean(&db->tcp.trans_timer, &state, &probes, globals->bib.ttl.tcp_trans);
	__clean(&db->tcp.syn4_timer, &state, &probes, TCP_INCOMING_SYN);
	db->tcp.pkt_count -= pktqueue_prepare_clean(db->tcp.pkt_queue, &icmps);
	spin_unlock_bh(&db->tcp.lock);

	state.table = &db->udp;

	spin_lock_bh(&db->udp.lock);
	__clean(&db->udp.est_timer, &state, &probes, globals->bib.ttl.udp);
	check_empty_expirer(&db->udp.trans_timer, &state, &probes);
	check_empty_expirer(&db->udp.syn4_timer, &state, &probes);
	spin_unlock_bh(&db->udp.lock);

	state.table = &db->icmp;

	spin_lock_bh(&db->icmp.lock);
	__clean(&db->icmp.est_timer, &state, &probes, globals->bib.ttl.icmp);
	check_empty_expirer(&db->icmp.trans_timer, &state, &probes);
	check_empty_expirer(&db->icmp.syn4_timer, &state, &probes);
	spin_unlock_bh(&db->icmp.lock);

	post_fate(&probes);
	pktqueue_clean(&icmps);
}

static struct rb_node *find_starting_point(struct bib_table *table,
		const struct ipv4_transport_addr *offset,
		bool include_offset)
{
	struct tabled_bib *bib;
	struct rb_node **node;
	struct rb_node *parent;

	/* If there's no offset, start from the beginning. */
	if (!offset)
		return rb_first(&table->tree4);

	/* If offset is found, start from offset or offset's next. */
	rbtree_find_node(offset, &table->tree4, compare_src4, struct tabled_bib,
			hook4, parent, node);
	if (*node)
		return include_offset ? (*node) : rb_next(*node);

	if (!parent)
		return NULL;

	/*
	 * If offset is not found, start from offset's next anyway.
	 * (If offset was meant to exist, it probably timed out and died while
	 * the caller wasn't holding the spinlock; it's nothing to worry about.)
	 */
	bib = rb_entry(parent, struct tabled_bib, hook4);
	return (compare_src4(bib, offset) < 0) ? rb_next(parent) : parent;
}

int bib_foreach(struct bib *db, l4_protocol proto,
		struct bib_foreach_func *func,
		const struct ipv4_transport_addr *offset)
{
	struct bib_table *table;
	struct rb_node *node;
	struct tabled_bib *tabled;
	struct bib_entry bib;
	int error = 0;

	table = get_table(db, proto);
	if (!table)
		return -EINVAL;

	spin_lock_bh(&table->lock);

	node = find_starting_point(table, offset, false);
	for (; node && !error; node = rb_next(node)) {
		tabled = bib4_entry(node);
		tbtobe(tabled, &bib);
		error = func->cb(&bib, tabled->is_static, func->arg);
	}

	spin_unlock_bh(&table->lock);
	return error;
}

static struct rb_node *slot_next(struct tree_slot *slot)
{
	if (!slot->parent)
		return NULL;
	if (&slot->parent->rb_left == slot->rb_link)
		return slot->parent;
	/* else if (slot->parent->rb_right == &slot->rb_link) */
	return rb_next(slot->parent);
}

static void next_bib(struct rb_node *next, struct bib_session_tuple *pos)
{
	pos->bib = bib4_entry(next);
}

static void next_session(struct rb_node *next, struct bib_session_tuple *pos)
{
	pos->session = node2session(next);
	if (!pos->session) {
		/* Tree was empty or the previous was the last session. */
		/* Cascade "next" to the supertree. */
		next_bib(rb_next(&pos->bib->hook4), pos);
	}
}

/**
 * Finds the BIB entry and/or session where a foreach of the sessions should
 * start with, based on @offset.
 *
 * If a session that matches @offset is found, will initialize both @pos->bib
 * and @pos->session to point to this session.
 * If @pos->bib is defined but @pos->session is not, the foreach should start
 * from @pos->bib's first session.
 * If neither @pos->bib nor @pos->session are defined, iteration ended.
 * (offset lies after the last session.)
 *
 * If @offset is not found, it always tries to return the session that would
 * follow one that would match perfectly. This is because sessions expiring
 * during ongoing fragmented foreaches are not considered a problem.
 */
static void find_session_offset(struct bib_state *state,
		struct session_foreach_offset *offset,
		struct bib_session_tuple *pos)
{
	struct tabled_bib tmp_bib;
	struct tabled_session tmp_session;
	struct tree_slot slot;

	memset(pos, 0, sizeof(*pos));

	tmp_bib.src4 = offset->offset.src;
	pos->bib = find_bibtree4_slot(state->table, &tmp_bib, &slot);
	if (!pos->bib) {
		next_bib(slot_next(&slot), pos);
		return;
	}

	tmp_session.dst4 = offset->offset.dst;
	pos->session = find_session_slot(pos->bib, &tmp_session, NULL, &slot);
	if (!pos->session) {
		next_session(slot_next(&slot), pos);
		return;
	}

	if (!offset->include_offset)
		next_session(rb_next(&pos->session->tree_hook), pos);
}

#define foreach_bib(table, node) \
		for (node = bib4_entry(rb_first(&(table)->tree4)); \
				node; \
				node = bib4_entry(rb_next(&node->hook4)))
#define foreach_session(tree, node) \
		for (node = node2session(rb_first(tree)); \
				node; \
				node = node2session(rb_next(&node->tree_hook)))

int bib_foreach_session(struct bib *db,
		struct globals *globals,
		l4_protocol proto,
		struct session_foreach_func *func,
		struct session_foreach_offset *offset)
{
	struct bib_state state;
	struct bib_session_tuple pos;
	struct session_entry tmp;
	int error;

	state.table = get_table(db, proto);
	if (!state.table)
		return -EINVAL;
	state.globals = &globals->bib;

	spin_lock_bh(&state.table->lock);

	if (offset) {
		find_session_offset(&state, offset, &pos);
		/* if pos.session != NULL, then pos.bib != NULL. */
		if (pos.session)
			goto goto_session;
		if (pos.bib)
			goto goto_bib;
		goto end;
	}

	foreach_bib(state.table, pos.bib) {
goto_bib:	foreach_session(&pos.bib->sessions, pos.session) {
goto_session:		tstose(&state, pos.session, &tmp);
			error = func->cb(&tmp, func->arg);
			if (error)
				goto end;
		}
	}

end:
	spin_unlock_bh(&state.table->lock);
	return error;
}

#undef foreach_session
#undef foreach_bib

int bib_find6(struct bib *db, l4_protocol proto,
		struct ipv6_transport_addr *addr,
		struct bib_entry *result)
{
	struct bib_table *table;
	struct tabled_bib *bib;

	table = get_table(db, proto);
	if (!table)
		return -EINVAL;

	spin_lock_bh(&table->lock);
	bib = find_bib6(table, addr);
	if (bib)
		tbtobe(bib, result);
	spin_unlock_bh(&table->lock);

	return bib ? 0 : -ESRCH;
}

int bib_find4(struct bib *db, l4_protocol proto,
		struct ipv4_transport_addr *addr,
		struct bib_entry *result)
{
	struct bib_table *table;
	struct tabled_bib *bib;

	table = get_table(db, proto);
	if (!table)
		return -EINVAL;

	spin_lock_bh(&table->lock);
	bib = find_bib4(table, addr);
	if (bib)
		tbtobe(bib, result);
	spin_unlock_bh(&table->lock);

	return bib ? 0 : -ESRCH;
}

static void bib2tabled(struct bib_entry *bib, struct tabled_bib *tabled)
{
	tabled->src6 = bib->ipv6;
	tabled->src4 = bib->ipv4;
	tabled->proto = bib->l4_proto;
	tabled->is_static = true;
	tabled->sessions = RB_ROOT;
}

int bib_add_static(struct bib *db, struct bib_entry *new,
		struct bib_entry *old)
{
	struct bib_table *table;
	struct tabled_bib *bib;
	struct tabled_bib *collision;
	struct tree_slot slot6;
	struct tree_slot slot4;

	table = get_table(db, new->l4_proto);
	if (!table)
		return -EINVAL;

	bib = alloc_bib(GFP_ATOMIC);
	if (!bib)
		return -ENOMEM;
	bib2tabled(new, bib);

	spin_lock_bh(&table->lock);

	collision = find_bibtree6_slot(table, bib, &slot6);
	if (collision) {
		if (taddr4_equals(&bib->src4, &collision->src4))
			goto upgrade;
		goto eexist;
	}

	collision = find_bibtree4_slot(table, bib, &slot4);
	if (collision)
		goto eexist;

	treeslot_commit(&slot6);
	treeslot_commit(&slot4);

	/*
	 * Since the BIB entry is now available, and assuming ADF is disabled,
	 * it would make sense to translate the relevant type 1 stored packets.
	 * That's bound to be a lot of messy code though, and the v4 client is
	 * going to retry anyway, so let's just forget the packets instead.
	 */
	if (new->l4_proto == L4PROTO_TCP)
		pktqueue_rm(db->tcp.pkt_queue, &new->ipv4);

	spin_unlock_bh(&table->lock);
	return 0;

upgrade:
	collision->is_static = true;
	spin_unlock_bh(&table->lock);
	free_bib(bib);
	return 0;

eexist:
	tbtobe(collision, old);
	spin_unlock_bh(&table->lock);
	free_bib(bib);
	return -EEXIST;
}

int bib_rm(struct bib *db, struct bib_entry *entry)
{
	struct bib_table *table;
	struct tabled_bib key;
	struct tabled_bib *bib;
	int error = -ESRCH;

	table = get_table(db, entry->l4_proto);
	if (!table)
		return -EINVAL;

	bib2tabled(entry, &key);

	spin_lock_bh(&table->lock);

	bib = find_bib6(table, &key.src6);
	if (bib && taddr4_equals(&key.src4, &bib->src4)) {
		detach_bib(table, bib);
		error = 0;
	}

	spin_unlock_bh(&table->lock);

	if (!error)
		release_bib_entry(&bib->hook4, NULL);

	return error;
}

void bib_rm_range(struct bib *db, l4_protocol proto, struct ipv4_range *range)
{
	struct bib_table *table;
	struct ipv4_transport_addr offset;
	struct rb_node *node;
	struct rb_node *next;
	struct tabled_bib *bib;
	struct bib_delete_list delete_list = { NULL };

	table = get_table(db, proto);
	if (!table)
		return;

	offset.l3 = range->prefix.addr;
	offset.l4 = range->ports.min;

	spin_lock_bh(&table->lock);

	node = find_starting_point(table, &offset, true);
	for (; node; node = next) {
		next = rb_next(node);
		bib = bib4_entry(node);

		if (!prefix4_contains(&range->prefix, &bib->src4.l3))
			break;
		if (port_range_contains(&range->ports, bib->src4.l4)) {
			detach_bib(table, bib);
			add_to_delete_list(&delete_list, node);
		}
	}

	spin_unlock_bh(&table->lock);

	commit_delete_list(&delete_list);
}

static void flush_table(struct bib_table *table)
{
	struct rb_node *node;
	struct rb_node *next;
	struct bib_delete_list delete_list = { NULL };

	spin_lock_bh(&table->lock);

	for (node = rb_first(&table->tree4); node; node = next) {
		next = rb_next(node);
		detach_bib(table, bib4_entry(node));
		add_to_delete_list(&delete_list, node);
	}

	spin_unlock_bh(&table->lock);

	commit_delete_list(&delete_list);
}

void bib_flush(struct bib *db)
{
	flush_table(&db->tcp);
	flush_table(&db->udp);
	flush_table(&db->icmp);
}

static void print_tabs(int tabs)
{
	int i;
	for (i = 0; i < tabs; i++)
		pr_cont("  ");
}

static void print_session(struct rb_node *node, int tabs, char *prefix)
{
	struct tabled_session *session;

	if (!node)
		return;
	pr_info("[Ssn]");

	session = node2session(node);
	print_tabs(tabs);
	pr_cont("[%s] %pI4#%u %pI6c#%u\n", prefix,
			&session->dst4.l3, session->dst4.l4,
			&session->dst6.l3, session->dst6.l4);

	print_session(node->rb_left, tabs + 1, "L"); /* "Left" */
	print_session(node->rb_right, tabs + 1, "R"); /* "Right" */
}

static void print_bib(struct rb_node *node, int tabs)
{
	struct tabled_bib *bib;

	if (!node)
		return;
	pr_info("[BIB]");

	bib = bib4_entry(node);
	print_tabs(tabs);
	pr_cont("%pI4#%u %pI6c#%u\n", &bib->src4.l3, bib->src4.l4,
			&bib->src6.l3, bib->src6.l4);

	print_session(bib->sessions.rb_node, tabs + 1, "T"); /* "Tree" */
	print_bib(node->rb_left, tabs + 1);
	print_bib(node->rb_right, tabs + 1);
}

void bib_print(struct bib *db)
{
	log_debug("TCP:");
	print_bib(db->tcp.tree4.rb_node, 1);
	log_debug("UDP:");
	print_bib(db->udp.tree4.rb_node, 1);
	log_debug("ICMP:");
	print_bib(db->icmp.tree4.rb_node, 1);
}
