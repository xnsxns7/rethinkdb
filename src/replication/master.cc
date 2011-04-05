#include "master.hpp"

#include <boost/scoped_ptr.hpp>

#include "db_thread_info.hpp"
#include "logger.hpp"
#include "replication/backfill.hpp"
#include "replication/net_structs.hpp"
#include "replication/protocol.hpp"
#include "server/key_value_store.hpp"

namespace replication {

void master_t::register_key_value_store(btree_key_value_store_t *store) {
    on_thread_t th(home_thread);
    rassert(queue_store_ == NULL);
    queue_store_.reset(new queueing_store_t(store));
    rassert(!listener_);
    listener_.reset(new tcp_listener_t(listener_port_));
    listener_->set_callback(this);
}

void master_t::get_cas(const store_key_t& key, castime_t castime) {
    // There is something disgusting with this.  What if the thing
    // that the tmp_hold would prevent happens before we grap the
    // tmp_hold?  Right now, this can't happen because spawn_on_thread
    // won't block before we grab the tmp_hold, because we don't do
    // anything before.

    // TODO Is grabbing a tmp_hold really necessary?  I wish it wasn't.
    snag_ptr_t<master_t> tmp_hold(this);

    if (stream_) {
        consider_nop_dispatch_and_update_latest_timestamp(castime.timestamp);

        size_t n = sizeof(net_get_cas_t) + key.size;
        scoped_malloc<net_get_cas_t> msg(n);
        msg->proposed_cas = castime.proposed_cas;
        msg->timestamp = castime.timestamp;
        msg->key_size = key.size;

        memcpy(msg->key, key.contents, key.size);

        if (stream_) stream_->send(msg.get());
    }
}

void master_t::sarc(const store_key_t& key, unique_ptr_t<data_provider_t> data, mcflags_t flags, exptime_t exptime, castime_t castime, add_policy_t add_policy, replace_policy_t replace_policy, cas_t old_cas) {
    snag_ptr_t<master_t> tmp_hold(this);

    if (stream_) {
        consider_nop_dispatch_and_update_latest_timestamp(castime.timestamp);

        net_sarc_t stru;
        stru.timestamp = castime.timestamp;
        stru.proposed_cas = castime.proposed_cas;
        stru.flags = flags;
        stru.exptime = exptime;
        stru.key_size = key.size;
        stru.value_size = data->get_size();
        stru.add_policy = add_policy;
        stru.replace_policy = replace_policy;
        stru.old_cas = old_cas;
        if (stream_) stream_->send(&stru, key.contents, data);
    }
}

void master_t::incr_decr(incr_decr_kind_t kind, const store_key_t &key, uint64_t amount, castime_t castime) {
    snag_ptr_t<master_t> tmp_hold(this);

    if (stream_) {
        consider_nop_dispatch_and_update_latest_timestamp(castime.timestamp);

        if (kind == incr_decr_INCR) {
            incr_decr_like<net_incr_t>(INCR, key, amount, castime);
        } else {
            rassert(kind == incr_decr_DECR);
            incr_decr_like<net_decr_t>(DECR, key, amount, castime);
        }
    }
}

template <class net_struct_type>
void master_t::incr_decr_like(UNUSED uint8_t msgcode, const store_key_t &key, uint64_t amount, castime_t castime) {
    // TODO: We aren't using the parameter msgcode.  This is obviously broken!

    size_t n = sizeof(net_struct_type) + key.size;

    scoped_malloc<net_struct_type> msg(n);
    msg->timestamp = castime.timestamp;
    msg->proposed_cas = castime.proposed_cas;
    msg->amount = amount;
    msg->key_size = key.size;
    memcpy(msg->key, key.contents, key.size);

    if (stream_) stream_->send(msg.get());
}


void master_t::append_prepend(append_prepend_kind_t kind, const store_key_t &key, unique_ptr_t<data_provider_t> data, castime_t castime) {
    snag_ptr_t<master_t> tmp_hold(this);

    if (stream_) {
        consider_nop_dispatch_and_update_latest_timestamp(castime.timestamp);

        if (kind == append_prepend_APPEND) {
            net_append_t appendstruct;
            appendstruct.timestamp = castime.timestamp;
            appendstruct.proposed_cas = castime.proposed_cas;
            appendstruct.key_size = key.size;
            appendstruct.value_size = data->get_size();

            if (stream_) stream_->send(&appendstruct, key.contents, data);
        } else {
            rassert(kind == append_prepend_PREPEND);

            net_prepend_t prependstruct;
            prependstruct.timestamp = castime.timestamp;
            prependstruct.proposed_cas = castime.proposed_cas;
            prependstruct.key_size = key.size;
            prependstruct.value_size = data->get_size();

            if (stream_) stream_->send(&prependstruct, key.contents, data);
        }
    }
}

void master_t::delete_key(const store_key_t &key, repli_timestamp timestamp) {
    snag_ptr_t<master_t> tmp_hold(this);

    size_t n = sizeof(net_delete_t) + key.size;
    if (stream_) {
        // TODO: THIS IS A COMPLETE HACK, figure out when we're passing repli_timestamp::invalid.
        if (timestamp.time != repli_timestamp::invalid.time) {
            consider_nop_dispatch_and_update_latest_timestamp(timestamp);
        }

        scoped_malloc<net_delete_t> msg(n);
        msg->timestamp = timestamp;
        msg->key_size = key.size;
        memcpy(msg->key, key.contents, key.size);

        if (stream_) stream_->send(msg.get());
    }
}

void nop_timer_trigger(void *master_) {
    master_t *master = reinterpret_cast<master_t *>(master_);

    master->consider_nop_dispatch_and_update_latest_timestamp(current_time());
}

void master_t::on_tcp_listener_accept(boost::scoped_ptr<linux_tcp_conn_t>& conn) {
    // TODO: Carefully handle case where a slave is already connected.

    // Right now we uncleanly close the slave connection.  What if
    // somebody has partially written a message to it (and writes the
    // rest of the message to conn?)  That will happen, the way the
    // code is, right now.
    {
        debugf("listener accept, destroying existing slave conn\n");
        destroy_existing_slave_conn_if_it_exists();
        debugf("making new repli_stream..\n");
        stream_ = new repli_stream_t(conn, this);
        next_timestamp_nop_timer_ = timer_handler_->add_timer_internal(1100, nop_timer_trigger, this, true);
        shutdown_cond_.reset();
        debugf("made repli_stream.\n");
    }
    // TODO when sending/receiving hello handshake, use database magic
    // to handle case where slave is already connected.

    // TODO receive hello handshake before sending other messages.
}

void master_t::destroy_existing_slave_conn_if_it_exists() {
    assert_thread();
    if (stream_) {
        stream_->shutdown();   // Will cause conn_closed() to happen
        shutdown_cond_.wait();
    }
    rassert(stream_ == NULL);
}

void master_t::consider_nop_dispatch_and_update_latest_timestamp(repli_timestamp timestamp) {
    assert_thread();
    rassert(timestamp.time != repli_timestamp::invalid.time);

    if (timestamp.time > latest_timestamp_.time) {
        latest_timestamp_ = timestamp;
        coro_t::spawn_now(boost::bind(&master_t::do_nop_rebound, this, timestamp));
    }

    timer_handler_->cancel_timer(next_timestamp_nop_timer_);
    next_timestamp_nop_timer_ = timer_handler_->add_timer_internal(1100, nop_timer_trigger, this, true);
}

void roundtrip_to_db_thread(int slice_thread, repli_timestamp timestamp, cond_t *cond, int *counter) {
    {
        on_thread_t th(slice_thread);

        repli_timestamp t = current_time();

        // TODO: Don't crash just because the slave sent a bunch of
        // crap to us.  Just disconnect the slave.
        guarantee(t.time >= timestamp.time);
    }

    --*counter;
    rassert(*counter >= 0);
    if (*counter == 0) {
        cond->pulse();
    }
}

void master_t::do_nop_rebound(repli_timestamp t) {

    snag_ptr_t<master_t> temp_hold(this);
    assert_thread();

    cond_t cond;
    int n = get_num_db_threads();
    int counter = n;
    for (int i = 0; i < n; ++i) {
        coro_t::spawn(boost::bind(roundtrip_to_db_thread, i, t, &cond, &counter));
    }

    cond.wait();

    net_nop_t msg;
    msg.timestamp = t;
    if (stream_) stream_->send(msg);
}


void master_t::do_backfill(repli_timestamp since_when) {
    // TODO: Right now, this tmp_hold means that we will wait until we
    // have finished backfilling before shutting down.  This is
    // undesireable behavior.  (Of course, if we simply remove the
    // tmp_hold, we have an unclean shutdown process.  So we need to
    // properly implement the shutting down of master.)

    snag_ptr_t<master_t> tmp_hold(this);
    if (stream_) {
        assert_thread();

        do_backfill_cb cb(home_thread, &stream_);

        queue_store_->inner()->spawn_backfill(since_when, &cb);

        repli_timestamp minimum_timestamp = cb.wait();
        net_backfill_complete_t msg;
        msg.time_barrier_timestamp = minimum_timestamp;
        if (stream_) stream_->send(&msg);
    }
}


master_dispatcher_t::master_dispatcher_t(master_t *master)
    : master_(master) {
}

struct change_visitor_t : public boost::static_visitor<mutation_t> {
    master_t *master;
    castime_t castime;
    mutation_t operator()(const get_cas_mutation_t& m) {
        coro_t::spawn_on_thread(master->home_thread, boost::bind(&master_t::get_cas, master, m.key, castime));
        return m;
    }
    mutation_t operator()(const sarc_mutation_t& m) {
        unique_ptr_t<buffer_borrowing_data_provider_t> borrower(new buffer_borrowing_data_provider_t(master->home_thread, m.data));
        coro_t::spawn_on_thread(master->home_thread, boost::bind(&master_t::sarc, master,
            m.key, borrower->side_provider(), m.flags, m.exptime, castime, m.add_policy, m.replace_policy, m.old_cas));
        sarc_mutation_t m2(m);
        m2.data = borrower;
        return m2;
    }
    mutation_t operator()(const incr_decr_mutation_t& m) {
        coro_t::spawn_on_thread(master->home_thread, boost::bind(&master_t::incr_decr, master, m.kind, m.key, m.amount, castime));
        return m;
    }
    mutation_t operator()(const append_prepend_mutation_t &m) {
        unique_ptr_t<buffer_borrowing_data_provider_t> borrower(new buffer_borrowing_data_provider_t(master->home_thread, m.data));
        coro_t::spawn_on_thread(master->home_thread, boost::bind(&master_t::append_prepend, master,
            m.kind, m.key, borrower->side_provider(), castime));
        append_prepend_mutation_t m2(m);
        m2.data = borrower;
        return m2;
    }
    mutation_t operator()(const delete_mutation_t& m) {
        coro_t::spawn_on_thread(master->home_thread, boost::bind(&master_t::delete_key, master, m.key, castime.timestamp));
        return m;
    }
};


mutation_t master_dispatcher_t::dispatch_change(const mutation_t& m, castime_t castime) {
    if (master_ != NULL) {
        change_visitor_t functor;
        functor.master = master_;
        functor.castime = castime;
        return boost::apply_visitor(functor, m.mutation);
    } else {
        return m;
    }
}


}  // namespace replication

