#ifndef _NDB_TXN_BTREE_IMPL_H_
#define _NDB_TXN_BTREE_IMPL_H_

#include <iostream>
#include <vector>
#include <map>

#include "txn_btree.h"
#include "lockguard.h"

namespace private_ {
  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, txn_btree_search_probe0, txn_btree_search_probe0_cg)
  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, txn_btree_search_probe1, txn_btree_search_probe1_cg)
}

template <template <typename> class Transaction>
template <typename Traits>
bool
txn_btree<Transaction>::search(
    Transaction<Traits> &t, const string_type &k,
    string_type &v, size_t max_bytes_read)
{
  //IV(ANON_REGION("txn_btree::search:", &txn_btree_search_probe0_cg));
  INVARIANT(max_bytes_read > 0);
  t.ensure_active();
  typename Transaction<Traits>::txn_context &ctx = t.ctx_map[this];

  // priority is
  // 1) write set
  // 2) absent set
  // 3) absent range set
  // 4) underlying tree
  //
  // note (1)-(3) are served by transaction::local_search()

  {
    if (ctx.local_search_str(t, k, v)) {
      if (v.size() > max_bytes_read)
        v.resize(max_bytes_read);
      return !v.empty();
    }
  }

  btree::value_type underlying_v;
  if (!underlying_btree.search(varkey(k), underlying_v)) {
    // all records exist in the system at MIN_TID with no value
    INVARIANT(ctx.absent_set.find(k) == ctx.absent_set.end());
    ctx.absent_set[k].type = transaction_base::ABSENT_REC_READ;
    return false;
  } else {
    const dbtuple * const ln = (const dbtuple *) underlying_v;
    INVARIANT(ln);
    transaction_base::tid_t start_t = 0;

    const std::pair<bool, transaction_base::tid_t> snapshot_tid_t =
      t.consistent_snapshot_tid();
    const transaction_base::tid_t snapshot_tid = snapshot_tid_t.first ?
      snapshot_tid_t.second : static_cast<transaction_base::tid_t>(dbtuple::MAX_TID);

    {
      PERF_DECL(static std::string probe0_name(std::string(__PRETTY_FUNCTION__) + std::string(":do_read:")));
      ANON_REGION(probe0_name.c_str(), &private_::txn_btree_search_probe0_cg);
      ln->prefetch();
      const bool is_read_only_txn = t.get_flags() & transaction_base::TXN_FLAG_READ_ONLY;
      if (unlikely(!ln->stable_read(snapshot_tid, start_t, v, is_read_only_txn, max_bytes_read))) {
        const transaction_base::abort_reason r =
          transaction_base::ABORT_REASON_UNSTABLE_READ;
        t.abort_impl(r);
        throw transaction_abort_exception(r);
      }
    }

    if (unlikely(!t.cast()->can_read_tid(start_t))) {
      const transaction_base::abort_reason r =
        transaction_base::ABORT_REASON_FUTURE_TID_READ;
      t.abort_impl(r);
      throw transaction_abort_exception(r);
    }

    const bool v_empty = v.empty();
    if (v_empty)
      ++transaction_base::g_evt_read_logical_deleted_node_search;
    PERF_DECL(static std::string probe1_name(std::string(__PRETTY_FUNCTION__) + std::string(":readset:")));
    ANON_REGION(probe1_name.c_str(), &private_::txn_btree_search_probe1_cg);
    transaction_base::read_record_t &read_rec = ctx.read_set[ln];
    if (!read_rec.t) {
      // XXX(stephentu): this doesn't work if we allow wrap around
      read_rec.t = start_t;
    } else if (unlikely(read_rec.t != start_t)) {
      const transaction_base::abort_reason r =
        transaction_base::ABORT_REASON_READ_NODE_INTEREFERENCE;
      t.abort_impl(r);
      throw transaction_abort_exception(r);
    }
    return !v_empty;
  }
}

template <template <typename> class Transaction>
template <typename Traits, typename StringAllocator>
void
txn_btree<Transaction>::txn_search_range_callback<Traits, StringAllocator>::on_resp_node(
    const btree::node_opaque_t *n, uint64_t version)
{
  VERBOSE(std::cerr << "on_resp_node(): <node=0x" << util::hexify(intptr_t(n))
               << ", version=" << version << ">" << std::endl);
  VERBOSE(std::cerr << "  " << btree::NodeStringify(n) << std::endl);
  if (t->get_flags() & transaction_base::TXN_FLAG_LOW_LEVEL_SCAN) {
    typename Transaction<Traits>::node_scan_map::iterator it = ctx->node_scan.find(n);
    if (it == ctx->node_scan.end()) {
      ctx->node_scan[n] = version;
    } else {
      if (unlikely(it->second != version)) {
        const transaction_base::abort_reason r =
          transaction_base::ABORT_REASON_NODE_SCAN_READ_VERSION_CHANGED;
        t->abort_impl(r);
        throw transaction_abort_exception(r);
      }
    }
  }
}

template <template <typename> class Transaction>
template <typename Traits, typename StringAllocator>
bool
txn_btree<Transaction>::txn_search_range_callback<Traits, StringAllocator>::invoke(
    const btree::string_type &k, btree::value_type v,
    const btree::node_opaque_t *n, uint64_t version)
{
  t->ensure_active();
  VERBOSE(std::cerr << "search range k: " << util::hexify(k) << " from <node=0x" << util::hexify(intptr_t(n))
               << ", version=" << version << ">" << std::endl
               << "  " << *((dbtuple *) v) << std::endl);
  if (!(t->get_flags() & transaction_base::TXN_FLAG_LOW_LEVEL_SCAN)) {
    key_range_t r =
      invoked ? key_range_t(util::next_key(prev_key), k) :
                key_range_t(lower, k);
    VERBOSE(std::cerr << "  range: " << r << std::endl);
    if (!r.is_empty_range())
      ctx->add_absent_range(r);
    prev_key = k;
  }
  invoked = true;

  string_type *local_v_ptr = sa();
  if (!local_v_ptr) {
    temp_buf0.clear();
    local_v_ptr = &temp_buf0;
  }
  string_type &local_v(*local_v_ptr);
  INVARIANT(temp_buf0.empty());

  bool local_read = ctx->local_search_str(*t, k, local_v);
  bool ret = true; // true means keep going, false means stop
  if (local_read && !local_v.empty()) {
    // found locally non-deleted copy, so let client read own writes
    ret = caller_callback->invoke(k, local_v);
  }
  const dbtuple * const ln = (dbtuple *) v;
  if (ctx->read_set.find(ln) == ctx->read_set.end()) {
    INVARIANT(ln);
    transaction_base::tid_t start_t = 0;
    string_type *r_ptr = sa();
    if (!r_ptr) {
      temp_buf1.clear();
      r_ptr = &temp_buf1;
    }
    string_type &r(*r_ptr);
    INVARIANT(r.empty());
    const std::pair<bool, transaction_base::tid_t> snapshot_tid_t =
      t->consistent_snapshot_tid();
    const transaction_base::tid_t snapshot_tid = snapshot_tid_t.first ?
      snapshot_tid_t.second : static_cast<transaction_base::tid_t>(dbtuple::MAX_TID);
    const bool is_read_only_txn = t->get_flags() & transaction_base::TXN_FLAG_READ_ONLY;
    ln->prefetch();
    if (unlikely(!ln->stable_read(snapshot_tid, start_t, r, is_read_only_txn))) {
      const transaction_base::abort_reason r =
        transaction_base::ABORT_REASON_UNSTABLE_READ;
      t->abort_impl(r);
      throw transaction_abort_exception(r);
    }
    if (unlikely(!t->cast()->can_read_tid(start_t))) {
      const transaction_base::abort_reason r =
        transaction_base::ABORT_REASON_FUTURE_TID_READ;
      t->abort_impl(r);
      throw transaction_abort_exception(r);
    }
    if (r.empty())
      ++transaction_base::g_evt_read_logical_deleted_node_scan;
    transaction_base::read_record_t * const read_rec = &ctx->read_set[ln];
    if (!read_rec->t) {
      // XXX(stephentu): this doesn't work if we allow wrap around
      read_rec->t = start_t;
    } else if (unlikely(read_rec->t != start_t)) {
      const transaction_base::abort_reason r =
        transaction_base::ABORT_REASON_READ_NODE_INTEREFERENCE;
      t->abort_impl(r);
      throw transaction_abort_exception(r);
    }
    VERBOSE(std::cerr << "read <t=" << start_t << ", sz=" << r.size()
                 << "> (local_read="
                 << (local_read ? "Y" : "N") << ")" << std::endl);
    if (!local_read && !r.empty())
      ret = caller_callback->invoke(k, r);
  }
  if (!ret)
    caller_stopped = true;
  return ret;
}

template <template <typename> class Transaction>
template <typename Traits, typename StringAllocator>
void
txn_btree<Transaction>::search_range_call(
    Transaction<Traits> &t,
    const string_type &lower,
    const string_type *upper,
    search_range_callback &callback,
    const StringAllocator &sa)
{
  t.ensure_active();
  typename Transaction<Traits>::txn_context &ctx = t.ctx_map[this];

  if (upper)
    VERBOSE(std::cerr << "txn_btree(0x" << util::hexify(intptr_t(this))
                 << ")::search_range_call [" << util::hexify(lower)
                 << ", " << util::hexify(*upper) << ")" << std::endl);
  else
    VERBOSE(std::cerr << "txn_btree(0x" << util::hexify(intptr_t(this))
                 << ")::search_range_call [" << util::hexify(lower)
                 << ", +inf)" << std::endl);

  // many cases to consider:
  // 1) for each dbtuple returned from the scan, we need to
  //    record it in our local read set. there are several cases:
  //    A) if the dbtuple corresponds to a key we have written, then
  //       we emit the version from the local write set
  //    B) if the dbtuple corresponds to a key we have previous read,
  //       then we emit the previous version
  // 2) for each dbtuple node *not* returned from the scan, we need
  //    to record its absense. we optimize this by recording the absense
  //    of contiguous ranges
  if (unlikely(upper && *upper <= lower))
    return;

  key_type lower_k(lower);
  key_type upper_k(upper ? key_type(*upper) : key_type());
  txn_search_range_callback<Traits, StringAllocator> c(&t, &ctx, lower_k, &callback, sa);
  underlying_btree.search_range_call(lower_k, upper ? &upper_k : NULL, c, c.sa());
  if (c.caller_stopped)
    return;
  if (!(t.get_flags() & transaction_base::TXN_FLAG_LOW_LEVEL_SCAN)) {
    if (upper)
      ctx.add_absent_range(
          key_range_t(
            c.invoked ? util::next_key(c.prev_key) : lower, *upper));
    else
      ctx.add_absent_range(
          key_range_t(
            c.invoked ? util::next_key(c.prev_key) : lower));
  }
}

template <template <typename> class Transaction>
template <typename Traits>
void
txn_btree<Transaction>::do_tree_put(
    Transaction<Traits> &t, const string_type &k,
    const string_type &v, bool expect_new)
{
  t.ensure_active();
  typename Transaction<Traits>::txn_context &ctx = t.ctx_map[this];
  if (unlikely(t.get_flags() & transaction_base::TXN_FLAG_READ_ONLY)) {
    transaction_base::abort_reason r = transaction_base::ABORT_REASON_USER;
    t.abort_impl(r);
    throw transaction_abort_exception(r);
  }
  dbtuple *tuple = nullptr;
  if (expect_new) {
    auto ret = t.try_insert_new_tuple(underlying_btree, ctx, k, v);
    INVARIANT(!ret.second || ret.first);
    if (unlikely(ret.second)) {
      transaction_base::abort_reason r = transaction_base::ABORT_REASON_WRITE_NODE_INTERFERENCE;
      t.abort_impl(r);
      throw transaction_abort_exception(r);
    }
    tuple = ret.first;
  }
  ctx.write_set[k] = transaction_base::write_record_t(v, tuple);

}

template <template <typename> class Transaction>
template <typename Traits>
void
txn_btree<Transaction>::do_tree_put(
    Transaction<Traits> &t, string_type &&k,
    string_type &&v, bool expect_new)
{
  t.ensure_active();
  typename Transaction<Traits>::txn_context &ctx = t.ctx_map[this];
  if (unlikely(t.get_flags() & transaction_base::TXN_FLAG_READ_ONLY)) {
    transaction_base::abort_reason r = transaction_base::ABORT_REASON_USER;
    t.abort_impl(r);
    throw transaction_abort_exception(r);
  }
  dbtuple *tuple = nullptr;
  if (expect_new) {
    auto ret = t.try_insert_new_tuple(underlying_btree, ctx, k, v);
    INVARIANT(!ret.second || ret.first);
    if (unlikely(ret.second)) {
      transaction_base::abort_reason r = transaction_base::ABORT_REASON_WRITE_NODE_INTERFERENCE;
      t.abort_impl(r);
      throw transaction_abort_exception(r);
    }
    tuple = ret.first;
  }
  ctx.write_set[std::move(k)] = transaction_base::write_record_t(std::move(v), tuple);
}

template <template <typename> class Transaction>
void
txn_btree<Transaction>::unsafe_purge(bool dump_stats)
{
  ALWAYS_ASSERT(!been_destructed);
  been_destructed = true;
  handler.on_destruct(); // stop background tasks
  purge_tree_walker w;
  underlying_btree.tree_walk(w);
  underlying_btree.clear();
#ifdef TXN_BTREE_DUMP_PURGE_STATS
  if (!dump_stats)
    return;
  w.dump_stats();
#endif
}

template <template <typename> class Transaction>
void
txn_btree<Transaction>::purge_tree_walker::on_node_begin(const btree::node_opaque_t *n)
{
  INVARIANT(spec_values.empty());
  spec_values = btree::ExtractValues(n);
}

template <template <typename> class Transaction>
void
txn_btree<Transaction>::purge_tree_walker::on_node_success()
{
  for (size_t i = 0; i < spec_values.size(); i++) {
    dbtuple *ln =
      (dbtuple *) spec_values[i].first;
    INVARIANT(ln);
#ifdef TXN_BTREE_DUMP_PURGE_STATS
    // XXX(stephentu): should we also walk the chain?
    purge_stats_ln_record_size_counts[ln->size]++;
    purge_stats_ln_alloc_size_counts[ln->alloc_size]++;
#endif
    if (txn_btree_handler<Transaction>::has_background_task) {
#ifdef CHECK_INVARIANTS
      lock_guard<dbtuple> l(ln, false);
#endif
      dbtuple::release(ln);
    } else {
      dbtuple::release_no_rcu(ln);
    }
  }
#ifdef TXN_BTREE_DUMP_PURGE_STATS
  purge_stats_nkeys_node.push_back(spec_values.size());
  purge_stats_nodes++;
  for (size_t i = 0; i < spec_values.size(); i++)
    if (spec_values[i].second)
      goto done;
  purge_stats_nosuffix_nodes++;
done:
#endif
  spec_values.clear();
}

template <template <typename> class Transaction>
void
txn_btree<Transaction>::purge_tree_walker::on_node_failure()
{
  spec_values.clear();
}

#endif /* _NDB_TXN_BTREE_IMPL_H_ */
