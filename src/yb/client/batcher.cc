// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/client/batcher.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "yb/client/async_rpc.h"
#include "yb/client/callbacks.h"
#include "yb/client/client.h"
#include "yb/client/client-internal.h"
#include "yb/client/error_collector.h"
#include "yb/client/in_flight_op.h"
#include "yb/client/meta_cache.h"
#include "yb/client/session.h"
#include "yb/client/table.h"
#include "yb/client/transaction.h"
#include "yb/client/yb_op.h"

#include "yb/common/wire_protocol.h"
#include "yb/gutil/strings/human_readable.h"
#include "yb/gutil/strings/join.h"

#include "yb/util/debug-util.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"

DEFINE_bool(redis_allow_reads_from_followers, false,
            "If true, the read will be served from the closest replica in the same AZ, which can "
            "be a follower.");
TAG_FLAG(redis_allow_reads_from_followers, evolving);
TAG_FLAG(redis_allow_reads_from_followers, runtime);

// When this flag is set to false and we have separate errors for operation, then batcher would
// report IO Error status. Otherwise we will try to combine errors from separate operation to
// status of batch. Useful in tests, when we don't need complex error analysis.
DEFINE_test_flag(bool, combine_batcher_errors, false,
                 "Whether combine errors into batcher status.");

using std::pair;
using std::set;
using std::unique_ptr;
using std::shared_ptr;
using std::unordered_map;
using strings::Substitute;

using namespace std::placeholders;

namespace yb {

using tserver::WriteResponsePB;
using tserver::WriteResponsePB_PerRowErrorPB;

namespace client {

namespace internal {

// TODO: instead of using a string error message, make Batcher return a status other than IOError.
// (https://github.com/YugaByte/yugabyte-db/issues/702)
const std::string Batcher::kErrorReachingOutToTServersMsg(
    "Errors occured while reaching out to the tablet servers");

// About lock ordering in this file:
// ------------------------------
// The locks must be acquired in the following order:
//   - Batcher::lock_
//   - InFlightOp::lock_
//
// It's generally important to release all the locks before either calling
// a user callback, or chaining to another async function, since that function
// may also chain directly to the callback. Without releasing locks first,
// the lock ordering may be violated, or a lock may deadlock on itself (these
// locks are non-reentrant).
// ------------------------------------------------------------

Batcher::Batcher(YBClient* client,
                 ErrorCollector* error_collector,
                 const YBSessionPtr& session,
                 YBTransactionPtr transaction,
                 ConsistentReadPoint* read_point,
                 bool force_consistent_read)
  : client_(client),
    weak_session_(session),
    error_collector_(error_collector),
    next_op_sequence_number_(0),
    async_rpc_metrics_(session->async_rpc_metrics()),
    transaction_(std::move(transaction)),
    read_point_(read_point),
    force_consistent_read_(force_consistent_read) {
}

void Batcher::Abort(const Status& status) {
  bool run_callback;
  {
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    state_ = BatcherState::kAborted;

    InFlightOps to_abort;
    for (auto& op : ops_) {
      std::lock_guard<simple_spinlock> l(op->lock_);
      if (op->state == InFlightOpState::kBufferedToTabletServer) {
        to_abort.push_back(op);
      }
    }

    for (auto& op : to_abort) {
      VLOG(1) << "Aborting op: " << op->ToString();
      MarkInFlightOpFailedUnlocked(op, status);
    }

    run_callback = flush_callback_;
  }

  if (run_callback) {
    RunCallback(status);
  }
}

Batcher::~Batcher() {
  if (PREDICT_FALSE(!ops_.empty())) {
    for (auto& op : ops_) {
      LOG(ERROR) << "Orphaned op: " << op->ToString();
    }
    LOG(FATAL) << "ops_ not empty";
  }
  CHECK(state_ == BatcherState::kComplete || state_ == BatcherState::kAborted)
      << "Bad state: " << state_;
}

void Batcher::SetTimeout(MonoDelta timeout) {
  CHECK_GE(timeout, MonoDelta::kZero);
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  timeout_ = timeout;
}

bool Batcher::HasPendingOperations() const {
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  return !ops_.empty();
}

int Batcher::CountBufferedOperations() const {
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  if (state_ == BatcherState::kGatheringOps) {
    return ops_.size();
  } else {
    // If we've already started to flush, then the ops aren't
    // considered "buffered".
    return 0;
  }
}

void Batcher::CheckForFinishedFlush() {
  YBSessionPtr session;
  {
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    if (!ops_.empty()) {
      // Did not finish yet.
      return;
    }

    // Possible cases when we should ignore this check:
    // kComplete - because of race condition CheckForFinishedFlush could be invoked from 2 threads
    //             and one of them just finished last operation.
    // kGatheringOps - lookup failure happened while batcher is getting filled with operations.
    if (state_ == BatcherState::kComplete || state_ == BatcherState::kGatheringOps) {
      return;
    }

    if (state_ != BatcherState::kResolvingTablets &&
        state_ != BatcherState::kTransactionReady) {
      LOG(DFATAL) << "Batcher finished in a wrong state: " << state_;
      return;
    }

    session = weak_session_.lock();
    state_ = BatcherState::kComplete;
  }

  if (session) {
    // Important to do this outside of the lock so that we don't have
    // a lock inversion deadlock -- the session lock should always
    // come before the batcher lock.
    session->FlushFinished(this);
  }

  Status s;
  if (!combined_error_.ok()) {
    s = combined_error_;
  } else if (had_errors_.load(std::memory_order_acquire)) {
    // In the general case, the user is responsible for fetching errors from the error collector.
    // TODO: use the Combined status here, so it is easy to recognize.
    // https://github.com/YugaByte/yugabyte-db/issues/702
    s = STATUS(IOError, kErrorReachingOutToTServersMsg);
  }

  RunCallback(s);
}

void Batcher::RunCallback(const Status& status) {
  auto runnable = std::make_shared<yb::FunctionRunnable>(
      [ cb{std::move(flush_callback_)}, status ]() { cb(status); });
  if (!client_->callback_threadpool() || !client_->callback_threadpool()->Submit(runnable).ok()) {
    runnable->Run();
  }
}

CoarseTimePoint Batcher::ComputeDeadlineUnlocked() const {
  MonoDelta timeout = timeout_;
  if (PREDICT_FALSE(!timeout.Initialized())) {
    YB_LOG_EVERY_N(WARNING, 100000) << "Client writing with no timeout set, using 60 seconds.\n"
                                    << GetStackTrace();
    timeout = MonoDelta::FromSeconds(60);
  }
  return CoarseMonoClock::now() + timeout;
}

void Batcher::FlushAsync(StatusFunctor callback) {
  {
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    CHECK_EQ(state_, BatcherState::kGatheringOps);
    state_ = BatcherState::kResolvingTablets;
    flush_callback_ = std::move(callback);
    deadline_ = ComputeDeadlineUnlocked();
  }

  // In the case that we have nothing buffered, just call the callback
  // immediately. Otherwise, the callback will be called by the last callback
  // when it sees that the ops_ list has drained.
  CheckForFinishedFlush();

  // Trigger flushing of all of the buffers. Some of these may already have
  // been flushed through an async path, but it's idempotent - a second call
  // to flush would just be a no-op.
  //
  // If some of the operations are still in-flight, then they'll get sent
  // when they hit 'per_tablet_ops', since our state is now kResolvingTablets.
  FlushBuffersIfReady();
}

Status Batcher::Add(shared_ptr<YBOperation> yb_op) {
  // As soon as we get the op, start looking up where it belongs,
  // so that when the user calls Flush, we are ready to go.
  auto in_flight_op = std::make_shared<InFlightOp>();
  RETURN_NOT_OK(yb_op->GetPartitionKey(&in_flight_op->partition_key));
  in_flight_op->yb_op = yb_op;
  in_flight_op->state = InFlightOpState::kLookingUpTablet;

  if (yb_op->table()->partition_schema().IsHashPartitioning()) {
    switch (yb_op->type()) {
      case YBOperation::Type::QL_READ:
        if (!in_flight_op->partition_key.empty()) {
          down_cast<YBqlOp *>(yb_op.get())->SetHashCode(
              PartitionSchema::DecodeMultiColumnHashValue(in_flight_op->partition_key));
        }
        break;
      case YBOperation::Type::QL_WRITE:
        down_cast<YBqlOp*>(yb_op.get())->SetHashCode(
            PartitionSchema::DecodeMultiColumnHashValue(in_flight_op->partition_key));
        break;
      case YBOperation::Type::REDIS_READ:
        down_cast<YBRedisReadOp*>(yb_op.get())->SetHashCode(
            PartitionSchema::DecodeMultiColumnHashValue(in_flight_op->partition_key));
        break;
      case YBOperation::Type::REDIS_WRITE:
        down_cast<YBRedisWriteOp*>(yb_op.get())->SetHashCode(
            PartitionSchema::DecodeMultiColumnHashValue(in_flight_op->partition_key));
        break;
      case YBOperation::Type::PGSQL_READ:
        if (!in_flight_op->partition_key.empty()) {
          down_cast<YBPgsqlReadOp *>(yb_op.get())->SetHashCode(
              PartitionSchema::DecodeMultiColumnHashValue(in_flight_op->partition_key));
        }
        break;
      case YBOperation::Type::PGSQL_WRITE:
        down_cast<YBPgsqlWriteOp*>(yb_op.get())->SetHashCode(
            PartitionSchema::DecodeMultiColumnHashValue(in_flight_op->partition_key));
        break;
    }
  }

  AddInFlightOp(in_flight_op);
  VLOG(3) << "Looking up tablet for " << in_flight_op->yb_op->ToString();

  if (yb_op->tablet()) {
    TabletLookupFinished(std::move(in_flight_op), yb_op->tablet());
  } else {
    // deadline_ is set in FlushAsync(), after all Add() calls are done, so
    // here we're forced to create a new deadline.
    auto deadline = ComputeDeadlineUnlocked();
    client_->data_->meta_cache_->LookupTabletByKey(
        in_flight_op->yb_op->table(), in_flight_op->partition_key, deadline,
        std::bind(&Batcher::TabletLookupFinished, BatcherPtr(this), in_flight_op, _1));
  }
  return Status::OK();
}

void Batcher::AddInFlightOp(const InFlightOpPtr& op) {
  LOG_IF(DFATAL, op->state != InFlightOpState::kLookingUpTablet)
      << "Adding in flight op in a wrong state: " << op->state;

  std::lock_guard<decltype(mutex_)> lock(mutex_);
  CHECK_EQ(state_, BatcherState::kGatheringOps);
  CHECK(ops_.insert(op).second);
  op->sequence_number_ = next_op_sequence_number_++;
  ++outstanding_lookups_;
}

bool Batcher::IsAbortedUnlocked() const {
  return state_ == BatcherState::kAborted;
}

void Batcher::CombineErrorUnlocked(const InFlightOpPtr& in_flight_op, const Status& status) {
  error_collector_->AddError(in_flight_op->yb_op, status);
  if (FLAGS_combine_batcher_errors) {
    if (combined_error_.ok()) {
      combined_error_ = status;
    } else if (!combined_error_.IsCombined() && combined_error_.code() != status.code()) {
      combined_error_ = STATUS(Combined, "Multiple failures");
    }
  }
  had_errors_.store(true, std::memory_order_release);
}

void Batcher::MarkInFlightOpFailedUnlocked(const InFlightOpPtr& in_flight_op, const Status& s) {
  CHECK_EQ(1, ops_.erase(in_flight_op)) << "Could not remove op " << in_flight_op->ToString()
                                        << " from in-flight list";

  CombineErrorUnlocked(in_flight_op, s);
}

void Batcher::TabletLookupFinished(
    InFlightOpPtr op, const Result<internal::RemoteTabletPtr>& lookup_result) {
  // Acquire the batcher lock early to atomically:
  // 1. Test if the batcher was aborted, and
  // 2. Change the op state.

  bool all_lookups_finished;
  {
    std::lock_guard<decltype(mutex_)> lock(mutex_);

    if (lookup_result.ok()) {
      op->tablet = *lookup_result;
    }

    --outstanding_lookups_;
    all_lookups_finished = outstanding_lookups_ == 0;

    if (IsAbortedUnlocked()) {
      VLOG(1) << "Aborted batch: TabletLookupFinished for " << op->yb_op->ToString();
      MarkInFlightOpFailedUnlocked(op, STATUS(Aborted, "Batch aborted"));
      // 'op' is deleted by above function.
      return;
    }

    VLOG(3) << "TabletLookupFinished for " << op->yb_op->ToString() << ": " << lookup_result
            << ", outstanding lookups: " << outstanding_lookups_;

    if (lookup_result.ok()) {
      std::lock_guard<simple_spinlock> l2(op->lock_);
      CHECK_EQ(op->state, InFlightOpState::kLookingUpTablet);
      CHECK(*lookup_result);

      op->state = InFlightOpState::kBufferedToTabletServer;

      ops_queue_.push_back(op);
    } else {
      MarkInFlightOpFailedUnlocked(op, lookup_result.status());
    }
  }

  if (!lookup_result.ok()) {
    CheckForFinishedFlush();
  }

  if (all_lookups_finished) {
    FlushBuffersIfReady();
  }
}

void Batcher::TransactionReady(const Status& status, const BatcherPtr& self) {
  if (status.ok()) {
    {
      std::lock_guard<decltype(mutex_)> lock(mutex_);
      if (state_ != BatcherState::kTransactionPrepare) {
        // Batcher was aborted.
        LOG_IF(DFATAL, state_ != BatcherState::kAborted)
            << "Batcher in a wrong state when transaction get ready: " << state_;
        return;
      }
      state_ = BatcherState::kTransactionReady;
    }
    FlushBuffersIfReady();
  } else {
    Abort(status);
  }
}

YB_DEFINE_ENUM(OpGroup, (kWrite)(kLeaderRead)(kConsistentPrefixRead));

namespace {
inline bool IsOkToReadFromFollower(const InFlightOpPtr& op) {
  return op->yb_op->type() == YBOperation::Type::REDIS_READ &&
         FLAGS_redis_allow_reads_from_followers;
}

inline bool IsQLConsistentPrefixRead(const InFlightOpPtr& op) {
  return op->yb_op->type() == YBOperation::Type::QL_READ &&
         std::static_pointer_cast<YBqlReadOp>(op->yb_op)->yb_consistency_level() ==
         YBConsistencyLevel::CONSISTENT_PREFIX;
}
} // namespace

OpGroup GetOpGroup(const InFlightOpPtr& op) {
  if (!op->yb_op->read_only()) {
    return OpGroup::kWrite;
  }
  if (IsOkToReadFromFollower(op) || IsQLConsistentPrefixRead(op)) {
    return OpGroup::kConsistentPrefixRead;
  }

  return OpGroup::kLeaderRead;
}

void Batcher::FlushBuffersIfReady() {
  {
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    if (outstanding_lookups_ != 0) {
      // FlushBuffersIfReady is also invoked when all lookups finished, so it ok to just return
      // here.
      VLOG(3) << "FlushBuffersIfReady: " << outstanding_lookups_ << " ops still in lookup";
      return;
    }

    if (state_ == BatcherState::kResolvingTablets) {
      state_ = BatcherState::kTransactionPrepare;

      // All operations were added, and tablets for them were resolved.
      // So we could sort them.
      std::sort(ops_queue_.begin(),
                ops_queue_.end(),
                [](const InFlightOpPtr& lhs, const InFlightOpPtr& rhs) {
        if (lhs->tablet.get() == rhs->tablet.get()) {
          auto lgroup = GetOpGroup(lhs);
          auto rgroup = GetOpGroup(rhs);
          if (lgroup != rgroup) {
            return lgroup < rgroup;
          }
          return lhs->sequence_number_ < rhs->sequence_number_;
        }
        return lhs->tablet.get() < rhs->tablet.get();
      });
    } else if (state_ != BatcherState::kTransactionReady) {
      VLOG(3) << "FlushBuffersIfReady: batcher not yet in transaction ready state: " << state_;
      return;
    }

  }

  bool force_consistent_read = force_consistent_read_;

  auto transaction = this->transaction();
  if (transaction) {
    // If this Batcher is executed in context of transaction,
    // then this transaction should initialize metadata used by RPC calls.
    //
    // If transaction is not yet ready to do it, then it will notify as via provided when
    // it could be done.
    if (!transaction->Prepare(ops_queue_,
                              force_consistent_read_,
                              deadline_,
                              std::bind(&Batcher::TransactionReady, this, _1, BatcherPtr(this)),
                              &transaction_metadata_)) {
      return;
    }

    // Set force_consistent_read to true, so async rpc would use read time from batcher.
    force_consistent_read = true;
  }

  // We're only ready to flush if:
  // 1. The batcher is in the flushing state (i.e. FlushAsync was called).
  // 2. All outstanding ops have finished lookup. Why? To avoid a situation
  //    where ops are flushed one by one as they finish lookup.
  {
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    state_ = BatcherState::kTransactionReady;
  }

  // All asynchronous requests were completed, so we could access ops_queue_ w/o holding the lock.
  if (ops_queue_.empty()) {
    return;
  }

  // Use big enough value for preallocated storage, to avoid unnecessary allocations.
  boost::container::small_vector<std::shared_ptr<AsyncRpc>, 40> rpcs;

  // Now flush the ops for each tablet.
  auto start = ops_queue_.begin();
  auto start_group = GetOpGroup(*start);
  for (auto it = start; it != ops_queue_.end(); ++it) {
    auto it_group = GetOpGroup(*it);
    // Aggregate and flush the ops so far if either:
    //   - we reached the next tablet or group
    if ((**it).tablet.get() != (**start).tablet.get() ||
        start_group != it_group) {
      // Consistent read is not required when whole batch fits into one command.
      bool need_consistent_read = force_consistent_read || start != ops_queue_.begin() ||
                                  it != ops_queue_.end();
      rpcs.push_back(CreateRpc(
          start->get()->tablet.get(), start, it, /* allow_local_calls_in_curr_thread */ false,
          need_consistent_read));
      start = it;
      start_group = it_group;
    }
  }

  // Consistent read is not required when whole batch fits into one command.
  bool need_consistent_read = force_consistent_read || start != ops_queue_.begin();
  rpcs.push_back(CreateRpc(
      start->get()->tablet.get(), start, ops_queue_.end(),
      allow_local_calls_in_curr_thread_, need_consistent_read));

  ops_queue_.clear();

  for (const auto& rpc : rpcs) {
    rpc->SendRpc();
  }
}

rpc::Messenger* Batcher::messenger() const {
  return client_->messenger();
}

rpc::ProxyCache& Batcher::proxy_cache() const {
  return client_->proxy_cache();
}

YBTransactionPtr Batcher::transaction() const {
  return transaction_;
}

const std::string& Batcher::proxy_uuid() const {
  return client_->proxy_uuid();
}

const ClientId& Batcher::client_id() const {
  return client_->id();
}

std::pair<RetryableRequestId, RetryableRequestId> Batcher::NextRequestIdAndMinRunningRequestId(
    const TabletId& tablet_id) {
  return client_->NextRequestIdAndMinRunningRequestId(tablet_id);
}

void Batcher::RequestFinished(const TabletId& tablet_id, RetryableRequestId request_id) {
  client_->RequestFinished(tablet_id, request_id);
}

std::shared_ptr<AsyncRpc> Batcher::CreateRpc(
    RemoteTablet* tablet, InFlightOps::const_iterator begin, InFlightOps::const_iterator end,
    const bool allow_local_calls_in_curr_thread, const bool need_consistent_read) {
  VLOG(3) << "FlushBuffersIfReady: already in flushing state, immediately flushing to "
          << tablet->tablet_id();

  CHECK(begin != end);

  // Create and send an RPC that aggregates the ops. The RPC is freed when
  // its callback completes.
  //
  // The RPC object takes ownership of the in flight ops.
  // The underlying YB OP is not directly owned, only a reference is kept.

  // Split the read operations according to consistency levels since based on consistency
  // levels the read algorithm would differ.
  InFlightOps ops(begin, end);
  auto op_group = GetOpGroup(*begin);
  AsyncRpcData data{this, tablet, allow_local_calls_in_curr_thread, need_consistent_read,
                    memory_limit_score_, std::move(ops)};
  switch (op_group) {
    case OpGroup::kWrite:
      return std::make_shared<WriteRpc>(&data);
    case OpGroup::kLeaderRead:
      return std::make_shared<ReadRpc>(&data);
    case OpGroup::kConsistentPrefixRead:
      return std::make_shared<ReadRpc>(&data, YBConsistencyLevel::CONSISTENT_PREFIX);
  }
  FATAL_INVALID_ENUM_VALUE(OpGroup, op_group);
}

using tserver::ReadResponsePB;

void Batcher::AddOpCountMismatchError() {
  // TODO: how to handle this kind of error where the array of response PB's don't match
  //       the size of the array of requests. We don't have a specific YBOperation to
  //       create an error with, because there are multiple YBOps in one Rpc.
  LOG(DFATAL) << "Received wrong number of responses compared to request(s) sent.";
}

void Batcher::RemoveInFlightOpsAfterFlushing(
    const InFlightOps& ops, const Status& status, FlushExtraResult flush_extra_result) {
  auto transaction = this->transaction();
  if (transaction) {
    transaction->Flushed(ops, flush_extra_result.used_read_time, status);
  }
  if (status.ok() && read_point_) {
    read_point_->UpdateClock(flush_extra_result.propagated_hybrid_time);
  }

  std::lock_guard<decltype(mutex_)> lock(mutex_);
  for (auto& op : ops) {
    CHECK_EQ(1, ops_.erase(op))
      << "Could not remove op " << op->ToString() << " from in-flight list";
  }
}

void Batcher::ProcessRpcStatus(const AsyncRpc &rpc, const Status &s) {
  // TODO: there is a potential race here -- if the Batcher gets destructed while
  // RPCs are in-flight, then accessing state_ will crash. We probably need to keep
  // track of the in-flight RPCs, and in the destructor, change each of them to an
  // "aborted" state.
  CHECK_EQ(state_, BatcherState::kTransactionReady);

  if (PREDICT_FALSE(!s.ok())) {
    // Mark each of the ops as failed, since the whole RPC failed.
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    for (auto& in_flight_op : rpc.ops()) {
      CombineErrorUnlocked(in_flight_op, s);
    }
  }
}

void Batcher::ProcessReadResponse(const ReadRpc &rpc, const Status &s) {
  ProcessRpcStatus(rpc, s);
}

void Batcher::ProcessWriteResponse(const WriteRpc &rpc, const Status &s) {
  ProcessRpcStatus(rpc, s);

  if (s.ok() && rpc.resp().has_propagated_hybrid_time()) {
    client_->data_->UpdateLatestObservedHybridTime(rpc.resp().propagated_hybrid_time());
  }

  // Check individual row errors.
  for (const WriteResponsePB_PerRowErrorPB& err_pb : rpc.resp().per_row_errors()) {
    // TODO: handle case where we get one of the more specific TS errors
    // like the tablet not being hosted?

    if (err_pb.row_index() >= rpc.ops().size()) {
      LOG(ERROR) << "Received a per_row_error for an out-of-bound op index "
                 << err_pb.row_index() << " (sent only "
                 << rpc.ops().size() << " ops)";
      LOG(ERROR) << "Response from tablet " << rpc.tablet().tablet_id() << ":\n"
                 << rpc.resp().DebugString();
      continue;
    }
    shared_ptr<YBOperation> yb_op = rpc.ops()[err_pb.row_index()]->yb_op;
    VLOG(1) << "Error on op " << yb_op->ToString() << ": " << err_pb.error().ShortDebugString();
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    CombineErrorUnlocked(rpc.ops()[err_pb.row_index()], StatusFromPB(err_pb.error()));
  }
}

}  // namespace internal
}  // namespace client
}  // namespace yb
