/*
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "box.h"
#include "error.h"
#include "journal.h"
#include "raft.h"
#include "relay.h"
#include "replication.h"
#include "txn_limbo.h"
#include "xrow.h"
#include "errinj.h"

struct raft box_raft_global = {
	/*
	 * Set an invalid state to validate in runtime the global raft node is
	 * not used before initialization.
	 */
	.state = 0,
};

enum election_mode box_election_mode = ELECTION_MODE_INVALID;

/**
 * Flag whether Raft leader fencing is enabled. If enabled leader will
 * resign when it looses quorum for any reason.
 */
static bool election_fencing_enabled = true;

/**
 * A trigger executed each time the Raft state machine updates any
 * of its visible attributes.
 */
static struct trigger box_raft_on_update;

/** Triggers executed once the node gains a quorum of connected peers. */
static struct trigger box_raft_on_quorum_gain;

/** Triggers executed once the node loses a quorum of connected peers. */
static struct trigger box_raft_on_quorum_loss;

struct rlist box_raft_on_broadcast =
	RLIST_HEAD_INITIALIZER(box_raft_on_broadcast);

/**
 * Worker fiber does all the asynchronous work, which may need yields and can be
 * long. These are WAL writes, network broadcasts. That allows not to block the
 * Raft state machine.
 */
static struct fiber *box_raft_worker = NULL;
/** Flag installed each time when new work appears for the worker fiber. */
static bool box_raft_has_work = false;

/**
 * Flag installed whenever replicaset is extended and unset when quorum is
 * obtained for the first time.
 * Prevents undesired fencing (e.g. during bootstrap).
 */
static bool box_raft_election_fencing_paused = false;

/**
 * Resume fencing.
 */
static void
box_raft_election_fencing_resume(void);

static void
box_raft_msg_to_request(const struct raft_msg *msg, struct raft_request *req)
{
	*req = (struct raft_request) {
		.term = msg->term,
		.vote = msg->vote,
		.leader_id = msg->leader_id,
		.is_leader_seen = msg->is_leader_seen,
		.state = msg->state,
		.vclock = msg->vclock,
	};
}

static void
box_raft_request_to_msg(const struct raft_request *req, struct raft_msg *msg)
{
	*msg = (struct raft_msg) {
		.term = req->term,
		.vote = req->vote,
		.leader_id = req->leader_id,
		.is_leader_seen = req->is_leader_seen,
		.state = req->state,
		.vclock = req->vclock,
	};
}

static void
box_raft_update_synchro_queue(struct raft *raft)
{
	assert(raft == box_raft());
	if (raft->state != RAFT_STATE_LEADER)
		return;
	int rc = 0;
	uint32_t errcode = 0;
	do {
		rc = box_promote_qsync();
		if (rc != 0) {
			struct error *err = diag_last_error(diag_get());
			errcode = box_error_code(err);
			diag_log();
		}
	} while (rc != 0 && errcode == ER_QUORUM_WAIT && !fiber_is_cancelled());
}

static int
box_raft_worker_f(va_list args)
{
	(void)args;
	struct raft *raft = fiber()->f_arg;
	assert(raft == box_raft());
	while (!fiber_is_cancelled()) {
		box_raft_has_work = false;

		raft_process_async(raft);
		box_raft_update_synchro_queue(raft);

		if (!box_raft_has_work)
			fiber_yield();
	}
	return 0;
}

static void
box_raft_schedule_async(struct raft *raft)
{
	assert(raft == box_raft());
	if (box_raft_worker == NULL) {
		box_raft_worker = fiber_new_system("raft_worker",
						   box_raft_worker_f);
		if (box_raft_worker == NULL) {
			/*
			 * XXX: should be handled properly, no need to panic.
			 * The issue though is that most of the Raft state
			 * machine functions are not supposed to fail, and also
			 * they usually wakeup the fiber when their work is
			 * finished. So it is too late to fail. On the other
			 * hand it looks not so good to create the fiber when
			 * Raft is initialized. Because then it will occupy
			 * memory even if Raft is not used.
			 */
			diag_log();
			panic("Could't create Raft worker fiber");
			return;
		}
		box_raft_worker->f_arg = raft;
		fiber_set_joinable(box_raft_worker, true);
	}
	/*
	 * Don't wake the fiber if it writes something (not cancellable).
	 * Otherwise it would be a spurious wakeup breaking the WAL write not
	 * adapted to this. Also don't wakeup the current fiber - it leads to
	 * undefined behaviour.
	 */
	if ((box_raft_worker->flags & FIBER_IS_CANCELLABLE) != 0)
		fiber_wakeup(box_raft_worker);
	box_raft_has_work = true;
}

static int
box_raft_on_update_f(struct trigger *trigger, void *event)
{
	(void)trigger;
	struct raft *raft = (struct raft *)event;
	assert(raft == box_raft());
	/*
	 * When the instance becomes a follower, it's good to make it read-only
	 * ASAP. This way we make sure followers don't write anything.
	 * However, if the instance is transitioning to leader it'll become
	 * writable only after it clears its synchro queue.
	 */
	box_update_ro_summary();
	box_broadcast_election();
	/*
	 * Once the node becomes read-only due to new term, it should stop
	 * finalizing existing synchronous transactions so that it doesn't
	 * trigger split-brain with a new leader which will soon emerge.
	 */
	if (raft->volatile_term > txn_limbo.promote_greatest_term)
		txn_limbo_fence(&txn_limbo);
	if (raft->state != RAFT_STATE_LEADER)
		return 0;
	/*
	 * If the node became a leader, time to clear the synchro queue. But it
	 * must be done in the worker fiber so as not to block the state
	 * machine, which called this trigger.
	 */
	box_raft_schedule_async(raft);
	return 0;
}

void
box_raft_update_election_quorum(void)
{
	struct raft *raft = box_raft();
	int quorum = replicaset_healthy_quorum();
	raft_cfg_election_quorum(raft, quorum);
	int size = MAX(replicaset.registered_count, 1);
	raft_cfg_cluster_size(raft, size);
}

/** Set Raft triggers on quorum loss/gain. */
static void
box_raft_add_quorum_triggers(void);

/** Remove triggers on quorum loss/gain. */
static void
box_raft_remove_quorum_triggers(void);

void
box_raft_cfg_election_mode(enum election_mode mode)
{
	struct raft *raft = box_raft();
	if (mode == box_election_mode)
		return;
	box_election_mode = mode;
	switch (mode) {
	case ELECTION_MODE_OFF:
	case ELECTION_MODE_VOTER:
		box_raft_remove_quorum_triggers();
		raft_cfg_is_candidate(raft, false);
		break;
	case ELECTION_MODE_MANUAL:
		box_raft_add_quorum_triggers();
		if (raft->state == RAFT_STATE_LEADER ||
		    raft->state == RAFT_STATE_CANDIDATE) {
			/*
			 * The node was configured to be a candidate. Don't
			 * disrupt its current leadership or the elections it's
			 * just started.
			 */
			raft_cfg_is_candidate_later(raft, false);
		} else {
			raft_cfg_is_candidate(raft, false);
		}
		break;
	case ELECTION_MODE_CANDIDATE:
		box_raft_add_quorum_triggers();
		if (replicaset_has_healthy_quorum()) {
			raft_cfg_is_candidate(raft, true);
		} else {
			/*
			 * NOP. The candidate will be started as soon as the
			 * node gains a quorum of peers.
			 */
			assert(!raft->is_cfg_candidate);
		}
		break;
	default:
		unreachable();
	}
	raft_cfg_is_enabled(raft, mode != ELECTION_MODE_OFF);
}

/**
 * Enter fencing mode: resign RAFT leadership, freeze limbo (don't write
 * rollbacks nor confirms).
 */
static void
box_raft_fence(void)
{
	struct raft *raft = box_raft();
	if (!raft->is_enabled || raft->state != RAFT_STATE_LEADER ||
	    !election_fencing_enabled || box_raft_election_fencing_paused)
		return;

	txn_limbo_fence(&txn_limbo);
	raft_resign(raft);
}

/**
 * Configure the raft node according to whether it has a quorum of connected
 * peers or not. It can't start elections, when it doesn't.
 */
static void
box_raft_notify_have_quorum(void)
{
	struct raft *raft = box_raft();
	bool has_healthy_quorum = replicaset_has_healthy_quorum();
	if (box_raft_election_fencing_paused && has_healthy_quorum)
		box_raft_election_fencing_resume();

	switch (box_election_mode) {
	case ELECTION_MODE_MANUAL:
		/* Quorum loss shouldn't interfere with manual elections. */
		assert(!raft->is_cfg_candidate);
		if (!has_healthy_quorum)
			box_raft_fence();
		break;
	case ELECTION_MODE_CANDIDATE:
		if (has_healthy_quorum) {
			raft_cfg_is_candidate(raft, true);
		} else if (raft->state == RAFT_STATE_CANDIDATE ||
			   raft->state == RAFT_STATE_LEADER) {
			box_raft_fence();
			raft_cfg_is_candidate_later(raft, false);
		} else {
			raft_cfg_is_candidate(raft, false);
		}
		break;
	/* Triggers can't fire while the node can't start elections. */
	case ELECTION_MODE_OFF:
	case ELECTION_MODE_VOTER:
	default:
		unreachable();
	}
}

void
box_raft_recover(const struct raft_request *req)
{
	struct raft_msg msg;
	box_raft_request_to_msg(req, &msg);
	raft_process_recovery(box_raft(), &msg);
}

void
box_raft_checkpoint_local(struct raft_request *req)
{
	struct raft_msg msg;
	raft_checkpoint_local(box_raft(), &msg);
	box_raft_msg_to_request(&msg, req);
}

void
box_raft_checkpoint_remote(struct raft_request *req)
{
	struct raft_msg msg;
	raft_checkpoint_remote(box_raft(), &msg);
	box_raft_msg_to_request(&msg, req);
}

int
box_raft_process(struct raft_request *req, uint32_t source)
{
	struct raft_msg msg;
	box_raft_request_to_msg(req, &msg);
	return raft_process_msg(box_raft(), &msg, source);
}

static void
box_raft_broadcast(struct raft *raft, const struct raft_msg *msg)
{
	(void)raft;
	assert(raft == box_raft());
	struct raft_request req;
	box_raft_msg_to_request(msg, &req);
	replicaset_foreach(replica)
		relay_push_raft(replica->relay, &req);
	trigger_run(&box_raft_on_broadcast, NULL);
}

static void
box_raft_write(struct raft *raft, const struct raft_msg *msg)
{
	(void)raft;
	assert(raft == box_raft());
	/* See Raft implementation why these fields are never written. */
	assert(msg->vclock == NULL);
	assert(msg->state == 0);

	struct raft_request req;
	box_raft_msg_to_request(msg, &req);
	struct region *region = &fiber()->gc;
	uint32_t svp = region_used(region);
	struct xrow_header row;
	char buf[sizeof(struct journal_entry) +
		 sizeof(struct xrow_header *)];
	struct journal_entry *entry = (struct journal_entry *)buf;
	entry->rows[0] = &row;

	if (xrow_encode_raft(&row, region, &req) != 0)
		goto fail;
	journal_entry_create(entry, 1, xrow_approx_len(&row),
			     journal_entry_fiber_wakeup_cb, fiber());

	/*
	 * A non-cancelable fiber is considered non-wake-able, generally. Raft
	 * follows this pattern of 'protection'.
	 */
	bool cancellable = fiber_set_cancellable(false);
	bool is_err = journal_write(entry) != 0;
	fiber_set_cancellable(cancellable);
	if (is_err)
		goto fail;
	if (entry->res < 0) {
		diag_set_journal_res(entry->res);
		goto fail;
	}

	region_truncate(region, svp);
	return;
fail:
	diag_log();
	/*
	 * XXX: the stub is supposed to be removed once it is defined what to do
	 * when a raft request WAL write fails.
	 */
	panic("Could not write a raft request to WAL\n");
}

/**
 * Context of waiting for a Raft term outcome. Which is either a leader is
 * elected, or a new term starts, or Raft is disabled.
 */
struct box_raft_watch_ctx {
	bool is_done;
	uint64_t term;
	struct fiber *owner;
};

static int
box_raft_wait_term_outcome_f(struct trigger *trig, void *event)
{
	struct raft *raft = event;
	assert(raft == box_raft());
	struct box_raft_watch_ctx *ctx = trig->data;
	/*
	 * Term ended with nothing, probably split vote which led to a next
	 * term.
	 */
	if (raft->volatile_term > ctx->term)
		goto done;
	/* Instance does not participate in terms anymore. */
	if (!raft->is_enabled)
		goto done;
	/* The term ended with a leader being found. */
	if (raft->leader != REPLICA_ID_NIL)
		goto done;
	/* The term still continues with no resolution. */
	return 0;
done:
	ctx->is_done = true;
	fiber_wakeup(ctx->owner);
	return 0;
}

int
box_raft_wait_term_outcome(void)
{
	struct raft *raft = box_raft();
	struct trigger trig;
	struct box_raft_watch_ctx ctx = {
		.is_done = false,
		.term = raft->volatile_term,
		.owner = fiber(),
	};
	trigger_create(&trig, box_raft_wait_term_outcome_f, &ctx, NULL);
	raft_on_update(raft, &trig);
	/*
	 * XXX: it is not a good idea not to have a timeout here. If all nodes
	 * are voters, the term might never end with any result nor bump to a
	 * new value.
	 */
	while (!fiber_is_cancelled() && !ctx.is_done)
		fiber_yield();
	trigger_clear(&trig);
	if (fiber_is_cancelled()) {
		diag_set(FiberIsCancelled);
		return -1;
	}
	if (!raft->is_enabled) {
		diag_set(ClientError, ER_ELECTION_DISABLED);
		return -1;
	}
	return 0;
}

struct raft_wait_persisted_data {
	struct fiber *waiter;
	uint64_t term;
};

static int
box_raft_wait_term_persisted_f(struct trigger *trig, void *event)
{
	struct raft *raft = event;
	struct raft_wait_persisted_data *data = trig->data;
	if (raft->term >= data->term)
		fiber_wakeup(data->waiter);
	return 0;
}

int
box_raft_wait_term_persisted(void)
{
	struct raft *raft = box_raft();
	if (raft->term == raft->volatile_term)
		return 0;
	struct raft_wait_persisted_data data = {
		.waiter = fiber(),
		.term = raft->volatile_term,
	};
	struct trigger trig;
	trigger_create(&trig, box_raft_wait_term_persisted_f, &data, NULL);
	raft_on_update(raft, &trig);

	do {
		fiber_yield();
		ERROR_INJECT_YIELD(ERRINJ_RAFT_WAIT_TERM_PERSISTED_DELAY);
	} while (raft->term < data.term && !fiber_is_cancelled());

	trigger_clear(&trig);
	if (fiber_is_cancelled()) {
		diag_set(FiberIsCancelled);
		return -1;
	}
	return 0;
}

static int
box_raft_on_quorum_change_f(struct trigger *trigger, void *event)
{
	(void)trigger;
	(void)event;

	box_raft_notify_have_quorum();

	return 0;
}

static inline void
box_raft_add_quorum_triggers(void)
{
	trigger_add_unique(&replicaset_on_quorum_gain,
			   &box_raft_on_quorum_gain);
	trigger_add_unique(&replicaset_on_quorum_loss,
			   &box_raft_on_quorum_loss);
}

static inline void
box_raft_remove_quorum_triggers(void)
{
	trigger_clear(&box_raft_on_quorum_loss);
	trigger_clear(&box_raft_on_quorum_gain);
}

void
box_raft_set_election_fencing_enabled(bool enabled)
{
	election_fencing_enabled = enabled;
	say_info("RAFT: fencing %s", enabled ? "enabled" : "disabled");
	if (!enabled)
		txn_limbo_unfence(&txn_limbo);
	replicaset_on_health_change();
}

void
box_raft_election_fencing_pause(void)
{
	say_info("RAFT: fencing paused");
	box_raft_election_fencing_paused = true;
}

static void
box_raft_election_fencing_resume(void)
{
	say_info("RAFT: fencing resumed");
	box_raft_election_fencing_paused = false;
}

void
box_raft_init(void)
{
	static const struct raft_vtab box_raft_vtab = {
		.broadcast = box_raft_broadcast,
		.write = box_raft_write,
		.schedule_async = box_raft_schedule_async,
	};
	raft_create(&box_raft_global, &box_raft_vtab);
	trigger_create(&box_raft_on_update, box_raft_on_update_f, NULL, NULL);
	raft_on_update(box_raft(), &box_raft_on_update);

	trigger_create(&box_raft_on_quorum_gain, box_raft_on_quorum_change_f,
		       NULL, NULL);
	trigger_create(&box_raft_on_quorum_loss, box_raft_on_quorum_change_f,
		       NULL, NULL);
}

void
box_raft_free(void)
{
	struct raft *raft = box_raft();
	/*
	 * Can't join the fiber, because the event loop is stopped already, and
	 * yields are not allowed.
	 */
	box_raft_worker = NULL;
	raft_destroy(raft);
	/*
	 * Invalidate so as box_raft() would fail if any usage attempt happens.
	 */
	raft->state = 0;

	box_raft_remove_quorum_triggers();
}
