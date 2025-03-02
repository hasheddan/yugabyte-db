// Copyright (c) YugaByte, Inc.
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

#include "yb/client/client.h"
#include "yb/client/ql-dml-test-base.h"
#include "yb/client/session.h"
#include "yb/client/table.h"
#include "yb/client/table_handle.h"
#include "yb/client/transaction.h"

#include "yb/consensus/log_reader.h"
#include "yb/consensus/raft_consensus.h"
#include "yb/consensus/replica_state.h"
#include "yb/consensus/retryable_requests.h"

#include "yb/docdb/consensus_frontier.h"
#include "yb/docdb/docdb_rocksdb_util.h"

#include "yb/rocksdb/metadata.h"
#include "yb/rocksdb/utilities/checkpoint.h"

#include "yb/server/hybrid_clock.h"

#include "yb/tablet/tablet_options.h"
#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/util/bfql/gen_opcodes.h"
#include "yb/util/random_util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/size_literals.h"

#include "yb/yql/cql/ql/util/statement_result.h"

DECLARE_double(respond_write_failed_probability);
DECLARE_bool(allow_preempting_compactions);
DECLARE_bool(detect_duplicates_for_retryable_requests);
DECLARE_bool(enable_ondisk_compression);
DECLARE_int32(raft_heartbeat_interval_ms);
DECLARE_bool(combine_batcher_errors);
DECLARE_int64(transaction_rpc_timeout_ms);
DECLARE_double(transaction_max_missed_heartbeat_periods);
DECLARE_int32(retryable_request_range_time_limit_secs);
DECLARE_int64(rocksdb_compact_flush_rate_limit_bytes_per_sec);
DECLARE_int32(rocksdb_level0_file_num_compaction_trigger);
DECLARE_int32(rocksdb_level0_slowdown_writes_trigger);
DECLARE_int32(rocksdb_max_background_compactions);
DECLARE_int32(rocksdb_universal_compaction_min_merge_width);
DECLARE_int32(rocksdb_universal_compaction_size_ratio);
DECLARE_uint64(log_segment_size_bytes);
DECLARE_int32(log_min_seconds_to_retain);
DECLARE_int64(remote_bootstrap_rate_limit_bytes_per_sec);
DECLARE_int64(db_write_buffer_size);
DECLARE_int32(log_cache_size_limit_mb);

using namespace std::literals;

using rocksdb::checkpoint::CreateCheckpoint;
using rocksdb::UserFrontierPtr;
using yb::tablet::TabletOptions;
using yb::docdb::InitRocksDBOptions;

namespace yb {
namespace client {

namespace {

const std::string kValueColumn = "v";

}

class QLStressTest : public QLDmlTestBase {
 public:
  QLStressTest() {
  }

  void SetUp() override {
    ASSERT_NO_FATALS(QLDmlTestBase::SetUp());

    YBSchemaBuilder b;
    InitSchemaBuilder(&b);
    CompleteSchemaBuilder(&b);

    ASSERT_OK(table_.Create(kTableName, NumTablets(), client_.get(), &b));
  }

  virtual void CompleteSchemaBuilder(YBSchemaBuilder* b) {}

  virtual int NumTablets() {
    return CalcNumTablets(3);
  }

  virtual void InitSchemaBuilder(YBSchemaBuilder* builder) {
    builder->AddColumn("h")->Type(INT32)->HashPrimaryKey()->NotNull();
    builder->AddColumn(kValueColumn)->Type(STRING);
  }

  YBqlWriteOpPtr InsertRow(const YBSessionPtr& session,
                           const TableHandle& table,
                           int32_t key,
                           const std::string& value) {
    auto op = table.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, key);
    table.AddStringColumnValue(req, kValueColumn, value);
    EXPECT_OK(session->Apply(op));
    return op;
  }

  CHECKED_STATUS WriteRow(const YBSessionPtr& session,
                          const TableHandle& table,
                          int32_t key,
                          const std::string& value) {
    auto op = InsertRow(session, table, key, value);
    RETURN_NOT_OK(session->Flush());
    if (op->response().status() != QLResponsePB::YQL_STATUS_OK) {
      return STATUS_FORMAT(
          RemoteError, "Write failed: $0", QLResponsePB::QLStatus_Name(op->response().status()));
    }

    return Status::OK();
  }

  YBqlReadOpPtr SelectRow(const YBSessionPtr& session, const TableHandle& table, int32_t key) {
    auto op = table.NewReadOp();
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, key);
    table.AddColumns({kValueColumn}, req);
    EXPECT_OK(session->Apply(op));
    return op;
  }

  Result<QLValue> ReadRow(const YBSessionPtr& session, const TableHandle& table, int32_t key) {
    auto op = SelectRow(session, table, key);
    RETURN_NOT_OK(session->Flush());
    if (op->response().status() != QLResponsePB::YQL_STATUS_OK) {
      return STATUS_FORMAT(
          RemoteError, "Read failed: $0", QLResponsePB::QLStatus_Name(op->response().status()));
    }
    auto rowblock = ql::RowsResult(op.get()).GetRowBlock();
    if (rowblock->row_count() != 1) {
      return STATUS_FORMAT(NotFound, "Bad count for $0, count: $1", key, rowblock->row_count());
    }
    const auto& row = rowblock->row(0);
    return row.column(0);
  }

  YBqlWriteOpPtr InsertRow(const YBSessionPtr& session,
                           int32_t key,
                           const std::string& value) {
    return QLStressTest::InsertRow(session, table_, key, value);
  }

  CHECKED_STATUS WriteRow(const YBSessionPtr& session,
                          int32_t key,
                          const std::string& value) {
    return QLStressTest::WriteRow(session, table_, key, value);
  }

  YBqlReadOpPtr SelectRow(const YBSessionPtr& session, int32_t key) {
    return QLStressTest::SelectRow(session, table_, key);
  }

  Result<QLValue> ReadRow(const YBSessionPtr& session, int32_t key) {
    return QLStressTest::ReadRow(session, table_, key);
  }

  void VerifyFlushedFrontiers();

  void TestRetryWrites(bool restarts);

  bool CheckRetryableRequestsCounts(size_t* total_entries, size_t* total_leaders);

  void AddWriter(
      std::string value_prefix, std::atomic<int>* key, TestThreadHolder* thread_holder,
      const std::chrono::nanoseconds& sleep_duration = std::chrono::nanoseconds());

  TableHandle table_;

  int checkpoint_index_ = 0;
};

/*
 * Create a lot of tables and check that each of them are usable (can read/write to them).
 * Test enough rows/keys to ensure that most tablets will be hit.
 */
TEST_F(QLStressTest, LargeNumberOfTables) {
  int num_tables = NonTsanVsTsan(20, 10);
  int num_tablets_per_table = NonTsanVsTsan(3, 1);
  auto session = NewSession();
  for (int i = 0; i < num_tables; i++) {
    YBSchemaBuilder b;
    InitSchemaBuilder(&b);
    CompleteSchemaBuilder(&b);
    TableHandle table;
    client::YBTableName table_name("my_keyspace", "ql_client_test_table_" + std::to_string(i));
    ASSERT_OK(table.Create(table_name, num_tablets_per_table, client_.get(), &b));

    int num_rows = num_tablets_per_table * 5;
    for (int key = i; key < i + num_rows; key++) {
      string value = "value_" + std::to_string(key);
      ASSERT_OK(WriteRow(session, table, key, value));
      auto read_value = ASSERT_RESULT(ReadRow(session, table, key));
      ASSERT_EQ(read_value.string_value(), value) << read_value.ToString();
    }
  }
}

bool QLStressTest::CheckRetryableRequestsCounts(size_t* total_entries, size_t* total_leaders) {
  *total_entries = 0;
  *total_leaders = 0;
  bool result = true;
  size_t replicated_limit = FLAGS_detect_duplicates_for_retryable_requests ? 1 : 0;
  auto peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kAll);
  for (const auto& peer : peers) {
    auto leader = peer->LeaderStatus() != consensus::LeaderStatus::NOT_LEADER;
    if (!peer->tablet() || peer->tablet()->metadata()->table_id() != table_.table()->id()) {
      continue;
    }
    size_t tablet_entries = peer->tablet()->TEST_CountRegularDBRecords();
    auto raft_consensus = down_cast<consensus::RaftConsensus*>(peer->consensus());
    auto request_counts = raft_consensus->TEST_CountRetryableRequests();
    LOG(INFO) << "T " << peer->tablet()->tablet_id() << " P " << peer->permanent_uuid()
              << ", entries: " << tablet_entries
              << ", running: " << request_counts.running
              << ", replicated: " << request_counts.replicated
              << ", leader: " << leader;
    if (leader) {
      *total_entries += tablet_entries;
      ++*total_leaders;
    }
    // Last write request could be rejected as duplicate, so followers would not be able to
    // cleanup replicated requests.
    if (request_counts.running != 0 || (leader && request_counts.replicated > replicated_limit)) {
      result = false;
    }
  }

  if (result && FLAGS_detect_duplicates_for_retryable_requests) {
    auto peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kLeaders);
    for (const auto& peer : peers) {
      if (peer->tablet()->metadata()->table_id() != table_.table()->id()) {
        continue;
      }
      auto db = peer->tablet()->TEST_db();
      rocksdb::ReadOptions read_opts;
      read_opts.query_id = rocksdb::kDefaultQueryId;
      std::unique_ptr<rocksdb::Iterator> iter(db->NewIterator(read_opts));
      std::unordered_map<std::string, std::string> keys;

      for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        Slice key = iter->key();
        EXPECT_OK(DocHybridTime::DecodeFromEnd(&key));
        auto emplace_result = keys.emplace(key.ToBuffer(), iter->key().ToBuffer());
        if (!emplace_result.second) {
          LOG(ERROR)
              << "Duplicate key: " << docdb::SubDocKey::DebugSliceToString(iter->key())
              << " vs " << docdb::SubDocKey::DebugSliceToString(emplace_result.first->second);
        }
      }
    }
  }

  return result;
}

void QLStressTest::TestRetryWrites(bool restarts) {
  const size_t kConcurrentWrites = 5;
  // Used only when table is transactional.
  const double kTransactionalWriteProbability = 0.5;

  SetAtomicFlag(0.25, &FLAGS_respond_write_failed_probability);

  const bool transactional = table_.table()->schema().table_properties().is_transactional();
  boost::optional<TransactionManager> txn_manager;
  if (transactional) {
    server::ClockPtr clock(new server::HybridClock(WallClock()));
    ASSERT_OK(clock->Init());
    txn_manager.emplace(client_.get(), clock, client::LocalTabletFilter());
  }

  TestThreadHolder thread_holder;
  std::atomic<int32_t> key_source(0);
  for (int i = 0; i != kConcurrentWrites; ++i) {
    thread_holder.AddThreadFunctor(
        [this, &key_source, &stop_requested = thread_holder.stop_flag(),
         &txn_manager, kTransactionalWriteProbability] {
      CDSAttacher attacher;
      auto session = NewSession();
      while (!stop_requested.load(std::memory_order_acquire)) {
        int32_t key = key_source.fetch_add(1, std::memory_order_acq_rel);
        YBTransactionPtr txn;
        if (txn_manager &&
            RandomActWithProbability(kTransactionalWriteProbability)) {
          txn = std::make_shared<YBTransaction>(txn_manager.get_ptr());
          ASSERT_OK(txn->Init(IsolationLevel::SNAPSHOT_ISOLATION));
          session->SetTransaction(txn);
        } else {
          session->SetTransaction(nullptr);
        }

        auto op = InsertRow(session, key, Format("value_$0", key));
        auto flush_status = session->Flush();
        if (flush_status.ok()) {
          ASSERT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);

          if (txn) {
            auto commit_status = txn->CommitFuture().get();
            if (!commit_status.ok()) {
              LOG(INFO) << "Commit failed, key: " << key << ", txn: " << txn->id()
                        << ", commit failed: " << commit_status;
              ASSERT_TRUE(commit_status.IsExpired());
            }
          }
          continue;
        }
        ASSERT_TRUE(flush_status.IsIOError()) << "Status: " << flush_status;
        ASSERT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_RUNTIME_ERROR);
        ASSERT_EQ(op->response().error_message(), "Duplicate request");
      }
    });
  }

  std::thread restart_thread;
  if (restarts) {
    thread_holder.AddThread(RestartsThread(cluster_.get(), 5s, &thread_holder.stop_flag()));
  }

  thread_holder.WaitAndStop(restarts ? 60s : 15s);

  int written_keys = key_source.load(std::memory_order_acquire);
  auto session = NewSession();
  for (int key = 0; key != written_keys; ++key) {
    auto value = ASSERT_RESULT(ReadRow(session, key));
    ASSERT_EQ(value.string_value(), Format("value_$0", key));
  }

  size_t total_entries = 0;
  size_t total_leaders = 0;
  ASSERT_OK(WaitFor(
      std::bind(&QLStressTest::CheckRetryableRequestsCounts, this, &total_entries, &total_leaders),
      15s, "Retryable requests cleanup"));

  ASSERT_EQ(total_leaders, table_.table()->GetPartitions().size());

  // We have 2 entries per row.
  if (FLAGS_detect_duplicates_for_retryable_requests) {
    ASSERT_EQ(total_entries, written_keys * 2);
  } else {
    // If duplicate request tracking is disabled, then total_entries should be greater than
    // written keys, otherwise test does not work.
    ASSERT_GT(total_entries, written_keys * 2);
  }

  ASSERT_GE(written_keys, RegularBuildVsSanitizers(100, 40));
}

TEST_F(QLStressTest, RetryWrites) {
  FLAGS_detect_duplicates_for_retryable_requests = true;
  TestRetryWrites(false /* restarts */);
}

TEST_F(QLStressTest, RetryWritesWithRestarts) {
  FLAGS_detect_duplicates_for_retryable_requests = true;
  TestRetryWrites(true /* restarts */);
}

class QLTransactionalStressTest : public QLStressTest {
 public:
  void SetUp() override {
    FLAGS_transaction_rpc_timeout_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(1min).count();
    FLAGS_transaction_max_missed_heartbeat_periods = 1000000;
    FLAGS_retryable_request_range_time_limit_secs = 600;
    ASSERT_NO_FATALS(QLStressTest::SetUp());
  }

  void CompleteSchemaBuilder(YBSchemaBuilder* b) override {
    TableProperties table_properties;
    table_properties.SetTransactional(true);
    b->SetTableProperties(table_properties);
  }
};

TEST_F_EX(QLStressTest, RetryTransactionalWrites, QLTransactionalStressTest) {
  FLAGS_detect_duplicates_for_retryable_requests = true;
  TestRetryWrites(false /* restarts */);
}

TEST_F_EX(QLStressTest, RetryTransactionalWritesWithRestarts, QLTransactionalStressTest) {
  FLAGS_detect_duplicates_for_retryable_requests = true;
  TestRetryWrites(true /* restarts */);
}

TEST_F(QLStressTest, RetryWritesDisabled) {
  FLAGS_detect_duplicates_for_retryable_requests = false;
  TestRetryWrites(false /* restarts */);
}

class QLStressTestIntValue : public QLStressTest {
 private:
  void InitSchemaBuilder(YBSchemaBuilder* builder) override {
    builder->AddColumn("h")->Type(INT32)->HashPrimaryKey()->NotNull();
    builder->AddColumn(kValueColumn)->Type(INT64);
  }
};

// This test does 100 concurrent increments of the same row.
// It is expected that resulting value will be equal to 100.
TEST_F_EX(QLStressTest, Increment, QLStressTestIntValue) {
  const auto kIncrements = 100;
  const auto kKey = 1;

  auto session = NewSession();
  {
    auto op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, kKey);
    table_.AddInt64ColumnValue(req, kValueColumn, 0);
    ASSERT_OK(session->ApplyAndFlush(op));
    ASSERT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  std::vector<YBqlWriteOpPtr> write_ops;
  std::vector<std::shared_future<Status>> futures;

  auto value_column_id = table_.ColumnId(kValueColumn);
  for (int i = 0; i != kIncrements; ++i) {
    auto op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_UPDATE);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, kKey);
    req->mutable_column_refs()->add_ids(value_column_id);
    auto* column_value = req->add_column_values();
    column_value->set_column_id(value_column_id);
    auto* bfcall = column_value->mutable_expr()->mutable_bfcall();
    bfcall->set_opcode(to_underlying(bfql::BFOpcode::OPCODE_AddI64I64_80));
    bfcall->add_operands()->set_column_id(value_column_id);
    bfcall->add_operands()->mutable_value()->set_int64_value(1);
    write_ops.push_back(op);
  }

  for (const auto& op : write_ops) {
    ASSERT_OK(session->Apply(op));
    futures.push_back(session->FlushFuture());
  }

  for (size_t i = 0; i != write_ops.size(); ++i) {
    ASSERT_OK(futures[i].get());
    ASSERT_EQ(write_ops[i]->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  auto value = ASSERT_RESULT(ReadRow(session, kKey));
  ASSERT_EQ(value.int64_value(), kIncrements) << value.ToString();
}

class QLStressTestSingleTablet : public QLStressTest {
 private:
  int NumTablets() override {
    return 1;
  }
};

// This test has the following scenario:
// Add some operations to the old leader, but don't add to other nodes.
// Switch leadership to a new leader, but don't accept updates from new leader by old leader.
// Also don't replicate no op by the new leader.
// Switch leadership back to the old leader.
// New leader should successfully accept old operations from old leader.
TEST_F_EX(QLStressTest, ShortTimeLeaderDoesNotReplicateNoOp, QLStressTestSingleTablet) {
  auto session = NewSession();
  ASSERT_OK(WriteRow(session, 0, "value0"));

  auto leaders = ListTabletPeers(cluster_.get(), ListPeersFilter::kLeaders);
  ASSERT_EQ(1, leaders.size());
  auto old_leader = leaders[0];

  auto followers = ListTabletPeers(cluster_.get(), ListPeersFilter::kNonLeaders);
  ASSERT_EQ(2, followers.size());
  tablet::TabletPeerPtr temp_leader = followers[0];
  tablet::TabletPeerPtr always_follower = followers[1];

  ASSERT_OK(WaitFor([old_leader, always_follower]() -> Result<bool> {
    auto leader_op_id = old_leader->consensus()->GetLastReceivedOpId();
    auto follower_op_id = always_follower->consensus()->GetLastReceivedOpId();
    return follower_op_id == leader_op_id;
  }, 5s, "Follower catch up"));

  for (const auto& follower : followers) {
    down_cast<consensus::RaftConsensus*>(follower->consensus())->TEST_RejectMode(
        consensus::RejectMode::kAll);
  }

  InsertRow(session, 1, "value1");
  auto flush_future = session->FlushFuture();

  InsertRow(session, 2, "value2");
  auto flush_future2 = session->FlushFuture();

  // Give leader some time to receive operation.
  // TODO wait for specific event.
  std::this_thread::sleep_for(1s);

  LOG(INFO) << "Step down old leader " << old_leader->permanent_uuid()
            << " in favor of " << temp_leader->permanent_uuid();

  ASSERT_OK(StepDown(old_leader, temp_leader->permanent_uuid(), ForceStepDown::kFalse));

  down_cast<consensus::RaftConsensus*>(old_leader->consensus())->TEST_RejectMode(
      consensus::RejectMode::kAll);
  down_cast<consensus::RaftConsensus*>(temp_leader->consensus())->TEST_RejectMode(
      consensus::RejectMode::kNone);
  down_cast<consensus::RaftConsensus*>(always_follower->consensus())->TEST_RejectMode(
      consensus::RejectMode::kNonEmpty);

  ASSERT_OK(WaitForLeaderOfSingleTablet(
      cluster_.get(), temp_leader, 20s, "Waiting for new leader"));

  // Give new leader some time to request lease.
  // TODO wait for specific event.
  std::this_thread::sleep_for(3s);
  auto temp_leader_safe_time = temp_leader->tablet()->SafeTime(tablet::RequireLease::kTrue);
  LOG(INFO) << "Safe time: " << temp_leader_safe_time;
  ASSERT_FALSE(temp_leader_safe_time.is_valid());

  LOG(INFO) << "Transferring leadership from " << temp_leader->permanent_uuid()
            << " back to " << old_leader->permanent_uuid();
  ASSERT_OK(StepDown(temp_leader, old_leader->permanent_uuid(), ForceStepDown::kTrue));

  ASSERT_OK(WaitForLeaderOfSingleTablet(
      cluster_.get(), old_leader, 20s, "Waiting old leader to restore leadership"));

  down_cast<consensus::RaftConsensus*>(always_follower->consensus())->TEST_RejectMode(
      consensus::RejectMode::kNone);

  ASSERT_OK(WriteRow(session, 3, "value3"));

  ASSERT_OK(flush_future.get());
  ASSERT_OK(flush_future2.get());
}

namespace {

void VerifyFlushedFrontier(const UserFrontierPtr& frontier, OpId* op_id) {
  ASSERT_TRUE(frontier);
  if (frontier) {
    *op_id = down_cast<docdb::ConsensusFrontier*>(frontier.get())->op_id();
    ASSERT_GT(op_id->term, 0);
    ASSERT_GT(op_id->index, 0);
  }
}

}  // anonymous namespace
void QLStressTest::VerifyFlushedFrontiers() {
  for (const auto& mini_tserver : cluster_->mini_tablet_servers()) {
    auto peers = mini_tserver->server()->tablet_manager()->GetTabletPeers();
    for (const auto& peer : peers) {
      rocksdb::DB* db = peer->tablet()->TEST_db();
      OpId op_id;
      ASSERT_NO_FATALS(VerifyFlushedFrontier(db->GetFlushedFrontier(), &op_id));

      // Also check that if we checkpoint this DB and open the checkpoint separately, the
      // flushed frontier non-zero as well.
      std::string checkpoint_dir;
      ASSERT_OK(Env::Default()->GetTestDirectory(&checkpoint_dir));
      checkpoint_dir += Format("/checkpoint_$0", checkpoint_index_);
      checkpoint_index_++;

      ASSERT_OK(CreateCheckpoint(db, checkpoint_dir));

      rocksdb::Options options;
      auto tablet_options = TabletOptions();
      tablet_options.rocksdb_env = db->GetEnv();
      InitRocksDBOptions(&options, "", nullptr, tablet_options);
      std::unique_ptr<rocksdb::DB> checkpoint_db;
      rocksdb::DB* checkpoint_db_raw_ptr = nullptr;

      options.create_if_missing = false;
      ASSERT_OK(rocksdb::DB::Open(options, checkpoint_dir, &checkpoint_db_raw_ptr));
      checkpoint_db.reset(checkpoint_db_raw_ptr);
      OpId checkpoint_op_id;
      ASSERT_NO_FATALS(
          VerifyFlushedFrontier(checkpoint_db->GetFlushedFrontier(), &checkpoint_op_id));
      ASSERT_OK(Env::Default()->DeleteRecursively(checkpoint_dir));

      ASSERT_LE(op_id, checkpoint_op_id);
    }
  }
}

TEST_F_EX(QLStressTest, FlushCompact, QLStressTestSingleTablet) {
  std::atomic<int> key;

  TestThreadHolder thread_holder;

  AddWriter("value_", &key, &thread_holder);

  auto start_time = MonoTime::Now();
  const auto kTimeout = MonoDelta::FromSeconds(60);
  int num_iter = 0;
  while (MonoTime::Now() - start_time < kTimeout) {
    ++num_iter;
    std::this_thread::sleep_for(1s);
    ASSERT_OK(cluster_->FlushTablets());
    ASSERT_NO_FATALS(VerifyFlushedFrontiers());
    std::this_thread::sleep_for(1s);
    auto compact_status = cluster_->CompactTablets();
    LOG_IF(INFO, !compact_status.ok()) << "Compaction failed: " << compact_status;
    ASSERT_NO_FATALS(VerifyFlushedFrontiers());
  }
  ASSERT_GE(num_iter, 5);
}

// The scenario of this test is the following:
// We do writes in background.
// Isolate leader for 10 seconds.
// Restore connectivity.
// Check that old leader was able to catch up after the partition is healed.
TEST_F_EX(QLStressTest, OldLeaderCatchUpAfterNetworkPartition, QLStressTestSingleTablet) {
  FLAGS_combine_batcher_errors = true;

  tablet::TabletPeer* leader_peer = nullptr;
  std::atomic<int> key(0);
  {
    TestThreadHolder thread_holder;

    AddWriter("value_", &key, &thread_holder);

    tserver::MiniTabletServer* leader = nullptr;
    for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
      auto current = cluster_->mini_tablet_server(i);
      auto peers = current->server()->tablet_manager()->GetTabletPeers();
      ASSERT_EQ(peers.size(), 1);
      if (peers.front()->LeaderStatus() != consensus::LeaderStatus::NOT_LEADER) {
        leader = current;
        leader_peer = peers.front().get();
        break;
      }
    }

    ASSERT_NE(leader, nullptr);

    std::this_thread::sleep_for(5s * yb::kTimeMultiplier);

    auto pre_isolate_op_id = leader_peer->GetLatestLogEntryOpId();
    LOG(INFO) << "Isolate, last op id: " << pre_isolate_op_id << ", key: " << key;
    ASSERT_EQ(pre_isolate_op_id.term, 1);
    ASSERT_GT(pre_isolate_op_id.index, key);
    leader->SetIsolated(true);
    std::this_thread::sleep_for(10s * yb::kTimeMultiplier);

    auto pre_restore_op_id = leader_peer->GetLatestLogEntryOpId();
    LOG(INFO) << "Restore, last op id: " << pre_restore_op_id << ", key: " << key;
    ASSERT_EQ(pre_restore_op_id.term, 1);
    ASSERT_GE(pre_restore_op_id.index, pre_isolate_op_id.index);
    ASSERT_LE(pre_restore_op_id.index, pre_isolate_op_id.index + 10);
    leader->SetIsolated(false);

    thread_holder.WaitAndStop(5s * yb::kTimeMultiplier);
  }

  ASSERT_OK(WaitFor([leader_peer, &key] {
    return leader_peer->GetLatestLogEntryOpId().index > key;
  }, 5s, "Old leader has enough operations"));

  auto finish_op_id = leader_peer->GetLatestLogEntryOpId();
  LOG(INFO) << "Finish, last op id: " << finish_op_id << ", key: " << key;
  ASSERT_GT(finish_op_id.term, 1);
  ASSERT_GT(finish_op_id.index, key);
}

TEST_F_EX(QLStressTest, SlowUpdateConsensus, QLStressTestSingleTablet) {
  std::atomic<int> key(0);

  auto peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kNonLeaders);
  ASSERT_EQ(peers.size(), 2);

  down_cast<consensus::RaftConsensus*>(peers[0]->consensus())->TEST_DelayUpdate(20s);

  TestThreadHolder thread_holder;
  AddWriter(std::string(100_KB, 'X'), &key, &thread_holder, 100ms);

  thread_holder.WaitAndStop(30s);

  down_cast<consensus::RaftConsensus*>(peers[0]->consensus())->TEST_DelayUpdate(0s);

  int64_t max_peak_consumption = 0;
  for (int i = 1; i <= cluster_->num_tablet_servers(); ++i) {
    auto server_tracker = MemTracker::FindTracker(Format("server $0", i));
    auto call_tracker = MemTracker::FindTracker("Call", server_tracker);
    auto inbound_rpc_tracker = MemTracker::FindTracker("Inbound RPC", call_tracker);
    max_peak_consumption = std::max(max_peak_consumption, inbound_rpc_tracker->peak_consumption());
  }
  LOG(INFO) << "Peak consumption: " << max_peak_consumption;
  ASSERT_LE(max_peak_consumption, 150_MB);
}

class QLStressTestDelayWrite : public QLStressTestSingleTablet {
 private:
  void SetUp() override {
    FLAGS_db_write_buffer_size = 1_KB;
    FLAGS_rocksdb_level0_slowdown_writes_trigger = 4;
    FLAGS_rocksdb_level0_file_num_compaction_trigger = 2;
    FLAGS_rocksdb_universal_compaction_min_merge_width = 2;
    FLAGS_rocksdb_universal_compaction_size_ratio = 1000;
    QLStressTestSingleTablet::SetUp();
  }
};

void QLStressTest::AddWriter(
    std::string value_prefix, std::atomic<int>* key, TestThreadHolder* thread_holder,
    const std::chrono::nanoseconds& sleep_duration) {
  thread_holder->AddThreadFunctor([this, &stop = thread_holder->stop_flag(), key, sleep_duration,
                                   value_prefix = std::move(value_prefix)] {
    auto session = NewSession();
    while (!stop.load(std::memory_order_acquire)) {
      auto new_key = *key + 1;
      ASSERT_OK(WriteRow(session, new_key, value_prefix + std::to_string(new_key)));
      if (sleep_duration.count() > 0) {
        std::this_thread::sleep_for(sleep_duration);
      }
      ++*key;
    }
  });
}

TEST_F_EX(QLStressTest, DelayWrite, QLStressTestDelayWrite) {
  std::atomic<int> key(0);

  const std::string value_prefix = std::string(1_KB, 'X');

  TestThreadHolder thread_holder;
  AddWriter(value_prefix, &key, &thread_holder);

  thread_holder.AddThreadFunctor([this, &stop = thread_holder.stop_flag(), &key, &value_prefix] {
    auto session = NewSession();
    while (!stop.load(std::memory_order_acquire)) {
      auto had_key = key.load(std::memory_order_acquire);
      if (had_key == 0) {
        std::this_thread::sleep_for(50ms);
        continue;
      }
      auto value = ASSERT_RESULT(ReadRow(session, had_key)).string_value();
      ASSERT_EQ(value, value_prefix + std::to_string(had_key));
    }
  });

  thread_holder.AddThread(RestartsThread(cluster_.get(), 5s, &thread_holder.stop_flag()));
  thread_holder.WaitAndStop(60s);

  LOG(INFO) << "Total keys written: " << key.load(std::memory_order_acquire);
  ASSERT_GT(key.load(std::memory_order_acquire), RegularBuildVsSanitizers(2000, 100));

  ASSERT_OK(WaitFor([cluster = cluster_.get()] {
    auto peers = ListTabletPeers(cluster, ListPeersFilter::kAll);
    OpId first_op_id;
    for (const auto& peer : peers) {
      if (!peer->consensus()) {
        return false;
      }
      auto current = peer->consensus()->GetLastCommittedOpId();
      if (!first_op_id) {
        first_op_id = current;
      } else if (current != first_op_id) {
        return false;
      }
    }
    return true;
  }, 30s, "Waiting tablets to sync up"));
}

class QLStressTestLongRemoteBootstrap : public QLStressTestSingleTablet {
 public:
  void SetUp() override {
    FLAGS_log_cache_size_limit_mb = 1;
    FLAGS_log_segment_size_bytes = 96_KB;
    QLStressTestSingleTablet::SetUp();
  }
};

TEST_F_EX(QLStressTest, LongRemoteBootstrap, QLStressTestLongRemoteBootstrap) {
  FLAGS_log_min_seconds_to_retain = 1;
  FLAGS_remote_bootstrap_rate_limit_bytes_per_sec = 1_MB;

  cluster_->mini_tablet_server(0)->Shutdown();

  ASSERT_OK(WaitFor([this] {
    auto leaders = ListTabletPeers(cluster_.get(), ListPeersFilter::kLeaders);
    if (leaders.empty()) {
      return false;
    }
    LOG(INFO) << "Tablet id: " << leaders.front()->tablet_id();
    return true;
  }, 30s, "Leader elected"));

  std::atomic<int> key(0);

  TestThreadHolder thread_holder;
  constexpr size_t kValueSize = 32_KB;
  AddWriter(std::string(kValueSize, 'X'), &key, &thread_holder, 100ms);

  std::this_thread::sleep_for(20s); // Wait some time to have logs.

  ASSERT_OK(WaitFor([this]() -> Result<bool> {
    RETURN_NOT_OK(cluster_->CleanTabletLogs());
    auto leaders = ListTabletPeers(cluster_.get(), ListPeersFilter::kLeaders);
    if (leaders.empty()) {
      return false;
    }

    RETURN_NOT_OK(leaders.front()->tablet()->Flush(tablet::FlushMode::kSync));
    RETURN_NOT_OK(leaders.front()->RunLogGC());

    // Check that first log was garbage collected, so remote bootstrap will be required.
    consensus::ReplicateMsgs replicates;
    return !leaders.front()->log()->GetLogReader()->ReadReplicatesInRange(
        100, 101, 0, &replicates).ok();
  }, 30s, "Logs cleaned"));

  LOG(INFO) << "Bring replica back, keys written: " << key.load(std::memory_order_acquire);
  ASSERT_OK(cluster_->mini_tablet_server(0)->Start());

  thread_holder.AddThreadFunctor([this, &stop = thread_holder.stop_flag()] {
    while (!stop.load(std::memory_order_acquire)) {
      ASSERT_OK(cluster_->FlushTablets());
      ASSERT_OK(cluster_->CleanTabletLogs());
      std::this_thread::sleep_for(100ms);
    }
  });

  ASSERT_OK(WaitAllReplicasHaveIndex(cluster_.get(), key.load(std::memory_order_acquire), 40s));
  LOG(INFO) << "All replicas ready";

  // Write some more values and check that replica still in touch.
  std::this_thread::sleep_for(5s);
  ASSERT_OK(WaitAllReplicasHaveIndex(cluster_.get(), key.load(std::memory_order_acquire), 1s));
}

class QLStressDynamicCompactionPriorityTest : public QLStressTest {
 public:
  void SetUp() override {
    FLAGS_allow_preempting_compactions = true;
    FLAGS_db_write_buffer_size = 16_KB;
    FLAGS_enable_ondisk_compression = false;
    FLAGS_rocksdb_max_background_compactions = 1;
    FLAGS_rocksdb_compact_flush_rate_limit_bytes_per_sec = 160_KB;
    QLStressTest::SetUp();
  }

  int NumTablets() override {
    return 1;
  }

  void InitSchemaBuilder(YBSchemaBuilder* builder) override {
    builder->AddColumn("h")->Type(INT32)->HashPrimaryKey()->NotNull();
    builder->AddColumn(kValueColumn)->Type(STRING);
  }
};

TEST_F_EX(QLStressTest, DynamicCompactionPriority, QLStressDynamicCompactionPriorityTest) {
  YBSchemaBuilder b;
  InitSchemaBuilder(&b);
  CompleteSchemaBuilder(&b);

  TableHandle table2;
  ASSERT_OK(table2.Create(YBTableName(kTableName.namespace_name(), kTableName.table_name() + "_2"),
                          NumTablets(), client_.get(), &b));

  TestThreadHolder thread_holder;
  thread_holder.AddThreadFunctor([this, &table2, &stop = thread_holder.stop_flag()] {
    auto session = NewSession();
    int key = 1;
    std::string value(FLAGS_db_write_buffer_size, 'X');
    int left_writes_to_current_table = 0;
    TableHandle* table = nullptr;
    while (!stop.load(std::memory_order_acquire)) {
      if (left_writes_to_current_table == 0) {
        table = RandomUniformBool() ? &table_ : &table2;
        left_writes_to_current_table = RandomUniformInt(1, std::max(1, key / 5));
      } else {
        --left_writes_to_current_table;
      }
      const auto op = table->NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
      auto* const req = op->mutable_request();
      QLAddInt32HashValue(req, key);
      table_.AddStringColumnValue(req, kValueColumn, value);
      ASSERT_OK(session->Apply(op));
      ASSERT_OK(session->Flush());
      ASSERT_OK(CheckOp(op.get()));
      std::this_thread::sleep_for(100ms);
      ++key;
    }
  });

  thread_holder.WaitAndStop(60s);

  auto delete_start = CoarseMonoClock::now();
  ASSERT_OK(client_->DeleteTable(table_->id(), true));
  MonoDelta delete_time(CoarseMonoClock::now() - delete_start);
  LOG(INFO) << "Delete time: " << delete_time;
  ASSERT_LE(delete_time, 10s);
}

} // namespace client
} // namespace yb
