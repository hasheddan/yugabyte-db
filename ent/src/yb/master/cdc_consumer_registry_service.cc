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

#include "yb/master/cdc_consumer_registry_service.h"

#include "yb/master/cdc_rpc_tasks.h"
#include "yb/master/master_util.h"

#include "yb/client/client.h"
#include "yb/cdc/cdc_consumer.pb.h"

#include "yb/util/random_util.h"

namespace yb {
namespace master {
namespace enterprise {

Status RegisterTableSubscriber(
    const std::string& producer_table_id,
    const std::string& consumer_table_id,
    const std::string& producer_master_addrs,
    const GetTableLocationsResponsePB& consumer_tablets_resp,
    std::unordered_set<HostPort, HostPortHash>* tserver_addrs,
    cdc::StreamEntryPB* stream_entry) {
  // Get the tablets in the producer table.
  auto cdc_rpc_tasks = VERIFY_RESULT(CDCRpcTasks::CreateWithMasterAddrs(producer_master_addrs));
  auto producer_table_locations =
      VERIFY_RESULT(cdc_rpc_tasks->GetTableLocations(producer_table_id));
  auto consumer_tablets_size = consumer_tablets_resp.tablet_locations_size();

  stream_entry->set_consumer_table_id(consumer_table_id);
  stream_entry->set_producer_table_id(producer_table_id);
  auto* mutable_map = stream_entry->mutable_consumer_producer_tablet_map();
  // Create the mapping between consumer and producer tablets.
  for (uint32_t i = 0; i < producer_table_locations.size(); i++) {
    const auto& producer = producer_table_locations.Get(i).tablet_id();
    const auto& consumer =
        consumer_tablets_resp.tablet_locations(i % consumer_tablets_size).tablet_id();
    cdc::ProducerTabletListPB producer_tablets;
    auto it = mutable_map->find(consumer);
    if (it != mutable_map->end()) {
      producer_tablets = it->second;
    }
    *producer_tablets.add_tablets() = producer;
    (*mutable_map)[consumer] = producer_tablets;
    for (const auto& replica : producer_table_locations.Get(i).replicas()) {
      // Use the public IP addresses since we're cross-universe
      for (const auto& addr : replica.ts_info().broadcast_addresses()) {
        tserver_addrs->insert(HostPortFromPB(addr));
      }
      // Rarely a viable setup for production replication, but used in testing...
      if (tserver_addrs->empty()) {
        LOG(WARNING) << "No public broadcast addresses found for "
                     << replica.ts_info().permanent_uuid()
                     << ".  Using private addresses instead.";
        for (const auto& addr : replica.ts_info().private_rpc_addresses()) {
          tserver_addrs->insert(HostPortFromPB(addr));
        }
      }
    }
  }
  return Status::OK();
}

Result<std::vector<CDCConsumerStreamInfo>> TEST_GetConsumerProducerTableMap(
    const std::string& producer_master_addrs,
    const ListTablesResponsePB& resp) {

  auto cdc_rpc_tasks = VERIFY_RESULT(CDCRpcTasks::CreateWithMasterAddrs(producer_master_addrs));
  auto producer_tables = VERIFY_RESULT(cdc_rpc_tasks->ListTables());

  std::unordered_map<std::string, std::string> consumer_tables_map;
  for (const auto& table_info : resp.tables()) {
    const auto& table_name_str = Format("$0:$1", table_info.namespace_().name(), table_info.name());
    consumer_tables_map[table_name_str] = table_info.id();
  }

  std::vector<CDCConsumerStreamInfo> consumer_producer_list;
  for (const auto& table : producer_tables) {
    // TODO(Rahul): Fix this for YSQL workload testing.
    if (!master::IsSystemNamespace(table.second.namespace_name())) {
      const auto& table_name_str =
          Format("$0:$1", table.second.namespace_name(), table.second.table_name());
      CDCConsumerStreamInfo stream_info;
      stream_info.stream_id = RandomHumanReadableString(16);
      stream_info.producer_table_id = table.first;
      stream_info.consumer_table_id = consumer_tables_map[table_name_str];
      consumer_producer_list.push_back(std::move(stream_info));
    }
  }
  return consumer_producer_list;
}

} // namespace enterprise
} // namespace master
} // namespace yb
