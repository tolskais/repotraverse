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
  std::string knowledge_ref{"refs/heads/repotraverse/v1/knowledge/accepted"};
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
  nlohmann::json publish_receipt(const EvidenceReceipt &receipt);
  nlohmann::json publish_inference_claim(const InferenceClaim &claim);
  nlohmann::json publish_knowledge_decision(const KnowledgeDecision &decision);
  nlohmann::json publish_pr_import(const nlohmann::json &record);
  nlohmann::json publish_external_fact(const std::string &connector,
                                       const std::string &kind,
                                       const std::string &external_id,
                                       const std::string &content_id,
                                       std::int64_t source_updated_at,
                                       const nlohmann::json &fact);
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
  std::size_t import_knowledge();
  nlohmann::json publish_knowledge_record(const nlohmann::json &record,
                                          const std::string &record_id,
                                          const std::string &ref);
  bool trusted_producer(const std::string &producer_id) const;
  void refresh_claim(const std::string &task_id);

  Catalog &catalog_;
  CoordinationOptions options_;
  mutable std::recursive_mutex mutex_;
};

} // namespace history
