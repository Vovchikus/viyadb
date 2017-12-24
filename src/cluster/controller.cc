/*
 * Copyright (c) 2017 ViyaDB Group
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <chrono>
#include <glog/logging.h>
#include <boost/exception/diagnostic_information.hpp>
#include "cluster/controller.h"
#include "cluster/notifier.h"
#include "cluster/configurator.h"

namespace viya {
namespace cluster {

Controller::Controller(const util::Config& config):
  config_(config),
  cluster_id_(config.str("cluster_id")),
  consul_(config),
  db_(config, 0, 0) {

  ReadClusterConfig();

  session_ = consul_.CreateSession(std::string("viyadb-controller"));
  le_ = consul_.ElectLeader(*session_, "clusters/" + cluster_id_ + "/nodes/controller/leader");

  initializer_ = std::make_unique<util::Later>(10000L, [this]() {
    try {
      Initialize();
    } catch (...) {
      LOG(ERROR)<<"Error initializing controller: "<<boost::current_exception_diagnostic_information();
    }
  });
}

void Controller::ReadClusterConfig() {
  cluster_config_ = util::Config(consul_.GetKey("clusters/" + cluster_id_ + "/config"));
  LOG(INFO)<<"Using cluster configuration: "<<cluster_config_.dump();

  tables_configs_.clear();
  for (auto& table : cluster_config_.strlist("tables", {})) {
    auto& table_conf = tables_configs_.emplace(
      table, consul_.GetKey("tables/" + table + "/config")).first->second;

    // Adapt metrics:
    json* raw_config = reinterpret_cast<json*>(table_conf.json_ptr());
    for (auto& metric : (*raw_config)["metrics"]) {
      if (metric["type"] == "count") {
        metric["type"] = "long_sum";
      }
    }

    db_.CreateTable(table_conf);
  }
  LOG(INFO)<<"Read "<<tables_configs_.size()<<" tables configurations";

  indexers_configs_.clear();
  for (auto& indexer_id : cluster_config_.strlist("indexers", {})) {
    indexers_configs_.emplace(indexer_id, consul_.GetKey("indexers/" + indexer_id + "/config"));
  }
  LOG(INFO)<<"Read "<<indexers_configs_.size()<<" indexers configurations";
}

bool Controller::ReadWorkersConfigs() {
  auto active_workers = consul_.ListKeys("clusters/" + cluster_id_ + "/nodes/workers");
  auto minimum_workers = (size_t)cluster_config_.num("minimum_workers", 0L);
  if (minimum_workers > 0 && active_workers.size() < minimum_workers) {
    LOG(INFO)<<"Number of active workers is less than the minimal number of workers ("<<minimum_workers<<")";
    return false;
  }
  LOG(INFO)<<"Found "<<active_workers.size()<<" active workers";

  workers_configs_.clear();
  for (auto& worker_id : active_workers) {
    workers_configs_.emplace(worker_id, consul_.GetKey(
        "clusters/" + cluster_id_ + "/nodes/workers/" + worker_id, false, "{}"));
  }
  LOG(INFO)<<"Read "<<workers_configs_.size()<<" workers configurations";
  return true;
}

void Controller::Initialize() {
  FetchLatestBatchInfo();

  InitializePartitioning();

  InitializePlan();

  std::string load_prefix = config_.str("state_dir") + "/input";

  Configurator configurator(*this, load_prefix);
  configurator.ConfigureWorkers();

  feeder_ = std::make_unique<Feeder>(*this, load_prefix);

  StartHttpServer();
}

void Controller::FetchLatestBatchInfo() {
  indexers_batches_.clear();
  for (auto& it : indexers_configs_) {
    auto notifier = NotifierFactory::Create(it.first, it.second.sub("batch").sub("notifier"), IndexerType::BATCH);
    auto msg = notifier->GetLastMessage();
    if (msg) {
      indexers_batches_.emplace(it.first, std::move(static_cast<BatchInfo*>(msg.release())));
    }
  }
  LOG(INFO)<<"Fetched "<<indexers_batches_.size()<<" batches from indexers notifiers";
}

void Controller::InitializePartitioning() {
  tables_partitioning_.clear();

  if (indexers_batches_.empty()) {
    LOG(WARNING)<<"No historical batches information available - generating default partitioning";

    for (auto& table_it : tables_configs_) {
      auto& table_name = table_it.first;
      auto& table_conf = table_it.second;

      util::Config partitioning;
      if (table_conf.exists("partitioning")) {
        partitioning = table_conf.sub("partitioning");
      } else {
        // Take partitioning config from indexer responsible for that table:
        for (auto& indexer_it : indexers_configs_) {
          auto indexer_tables = indexer_it.second.strlist("tables");
          if (std::find_if(indexer_tables.begin(), indexer_tables.end(), [&table_name](auto& t) {
            return table_name == t;
          }) != indexer_tables.end()) {
            auto batch_conf = indexer_it.second.sub("batch");
            if (batch_conf.exists("partitioning")) {
              partitioning = batch_conf.sub("partitioning");
            }
            break;
          }
        }
      }

      if (partitioning.exists("columns")) {
        size_t total_partitions = partitioning.num("partitions");
        // Every key value goes to a partition:
        std::vector<uint32_t> mapping;
        mapping.resize(total_partitions);
        for (uint32_t v = 0; v < total_partitions; ++v) {
          mapping[v] = v;
        }
        tables_partitioning_.emplace(
          std::piecewise_construct, std::forward_as_tuple(table_name),
          std::forward_as_tuple(mapping, total_partitions, partitioning.strlist("columns")));
      } else {
        // TODO: generate default partitioning based on workers number
      }
    }
  } else {
    for (auto& batches_it : indexers_batches_) {
      for (auto& tables_it : batches_it.second->tables_info()) {
        auto& table_name = tables_it.first;
        if (tables_partitioning_.find(table_name) != tables_partitioning_.end()) {
          throw std::runtime_error("Multiple indexers operate on same tables!");
        }
        // TODO: check whether partitioning is available (or make it mandatory on indexer side)
        tables_partitioning_.emplace(table_name, tables_it.second.partitioning());
      }
    }
  }
}

void Controller::InitializePlan() {
  while (true) {
    if (le_->Leader()) {
      if (GeneratePlan()) {
        break;
      }
      LOG(INFO)<<"Can't generate or store partitioning plan right now... will retry soon";
      std::this_thread::sleep_for(std::chrono::seconds(10));
    } else {
      if (ReadPlan()) {
        break;
      }
      LOG(INFO)<<"Partitioning plan is not available yet... waiting for leader to generate it";
      std::this_thread::sleep_for(std::chrono::seconds(10));
    }
  }
}

bool Controller::ReadPlan() {
  json existing_plan = json::parse(consul_.GetKey("clusters/" + cluster_id_ + "/plan", false, "{}"));
  if (existing_plan.empty()) {
    return false;
  }

  LOG(INFO)<<"Reading cached plan from Consul";
  tables_plans_.clear();

  json tables_plans = existing_plan["plan"];
  for (auto it = tables_plans.begin(); it != tables_plans.end(); ++it) {
    tables_plans_.emplace(std::piecewise_construct, std::forward_as_tuple(it.key()),
                          std::forward_as_tuple(it.value(), workers_configs_));
  }
  return true;
}

bool Controller::GeneratePlan() {
  while (!ReadWorkersConfigs()) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
  }

  LOG(INFO)<<"Generating partitioning plan";
  PlanGenerator plan_generator(cluster_config_);
  tables_plans_.clear();

  for (auto& it : tables_partitioning_) {
    tables_plans_.emplace(it.first, std::move(
        plan_generator.Generate(it.second.total(), workers_configs_)));
  }

  LOG(INFO)<<"Storing partitioning plan to Consul";
  json cache = json({});
  for (auto& it : tables_plans_) {
    cache[it.first] = it.second.ToJson();
  }
  return session_->EphemeralKey("clusters/" + cluster_id_ + "/plan", cache.dump());
}

void Controller::StartHttpServer() {
  http_service_ = std::make_unique<http::Service>(*this);
  http_service_->Start();
}

std::string Controller::WorkerUrl(const std::string& worker_id) const {
  auto& worker_config = workers_configs_.at(worker_id);
  return "http://" + worker_config.str("hostname") + ":" + std::to_string(worker_config.num("http_port"));
}

}}
