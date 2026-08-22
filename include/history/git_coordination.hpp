#pragma once

#include <filesystem>
#include <chrono>
#include <mutex>
#include <string>
#include <set>

#include <nlohmann/json.hpp>

#include "history/catalog.hpp"

namespace history {

struct CoordinationOptions {
  std::filesystem::path repository;
  std::string remote{"origin"};
  std::int64_t lease_seconds{900};
  std::int64_t grace_seconds{120};
  std::set<std::string> trusted_producers;
  bool enforce_trusted_producers{};
  std::size_t max_record_bytes{256ULL * 1024ULL * 1024ULL};
};

class GitCoordinator {
public:
  GitCoordinator(Catalog &catalog, CoordinationOptions options);

  nlohmann::json sync();
  nlohmann::json publish_tasks(const nlohmann::json &tasks);
  nlohmann::json acquire(const nlohmann::json &task);
  nlohmann::json heartbeat(const std::string &task_id);
  nlohmann::json complete(const std::string &task_id,
                          const nlohmann::json &result);
  nlohmann::json publish_review(const LineageRelation &relation);
  nlohmann::json status(const std::string &task_id) const;
  std::chrono::seconds heartbeat_interval() const;

private:
  std::string create_lease_commit(const nlohmann::json &lease,
                                  const std::string &parent = {}) const;
  std::string claim_ref(const std::string &task_id) const;
  std::string remote_tracking_ref(const std::string &task_id) const;
  nlohmann::json publish(const std::string &task_id,
                         const nlohmann::json &lease,
                         const std::string &expected_oid);
  std::string publish_result(const std::string &task_id,
                             const nlohmann::json &result);
  std::size_t import_results();
  std::size_t import_tasks();
  std::size_t import_reviews();
  bool trusted_producer(const std::string &producer_id) const;

  Catalog &catalog_;
  CoordinationOptions options_;
  mutable std::recursive_mutex mutex_;
};

} // namespace history
