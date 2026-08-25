#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include "history/ir.hpp"
#include <nlohmann/json.hpp>

struct sqlite3;

namespace history {

class Catalog {
public:
  explicit Catalog(std::filesystem::path root,
                   std::size_t maximum_cached_facts = 10000,
                   std::uint64_t maximum_cached_fact_bytes =
                       4ULL * 1024ULL * 1024ULL * 1024ULL);
  ~Catalog();
  Catalog(const Catalog &) = delete;
  Catalog &operator=(const Catalog &) = delete;

  const std::filesystem::path &root() const { return root_; }
  const std::string &producer_id() const { return producer_id_; }

  void observe_claim(const std::string &task_id, const std::string &ref_oid,
                     const nlohmann::json &lease);
  void remove_claim(const std::string &task_id);
  std::optional<nlohmann::json> claim(const std::string &task_id) const;
  nlohmann::json claims() const;
  void store_fact(const std::string &fact_id, const std::string &task_id,
                  const nlohmann::json &fact, const std::string &source_commit);
  std::optional<nlohmann::json> fact_for_task(const std::string &task_id) const;
  bool imported(const std::string &commit) const;
  void mark_imported(const std::string &commit);
  void store_compile_context(const CompileContext &context);
  std::vector<CompileContext>
  compile_contexts(const std::string &translation_unit,
                   const std::string &revision = {}) const;
  void schedule_task(const std::string &task_id, const nlohmann::json &task);
  nlohmann::json pending_tasks(const std::string &request_id = {}) const;
  nlohmann::json results_for_request(const std::string &request_id) const;
  std::optional<nlohmann::json> next_pending_task() const;
  void set_task_state(const std::string &task_id, const std::string &state);
  nlohmann::json fail_task(const std::string &task_id,
                           const std::string &diagnostic,
                           std::uint32_t maximum_attempts);
  void retry_task(const std::string &task_id);
  void cancel_request_tasks(const std::string &request_id);
  std::string create_request(const nlohmann::json &request);
  std::optional<nlohmann::json> request_job(const std::string &request_id) const;
  void update_request(const std::string &request_id, const std::string &state,
                      const nlohmann::json &progress,
                      const nlohmann::json &result = {},
                      const nlohmann::json &error = {});
  void store_lineage_relation(const LineageRelation &relation);
  std::optional<LineageRelation>
  lineage_relation(const std::string &relation_id) const;
  void store_submodule_revision(const SubmoduleRevision &revision);
  nlohmann::json submodule_revisions(const std::string &parent_repository_id,
                                     const std::string &parent_revision) const;
  nlohmann::json semantic_dependents(
      const std::string &repository_id, const std::string &revision,
      const std::vector<std::string> &element_ids,
      std::size_t maximum = 10000) const;
  nlohmann::json symbol_search(const std::string &repository_id,
                               const std::string &revision,
                               const nlohmann::json &selector,
                               std::size_t maximum = 100) const;
  nlohmann::json file_symbols(const std::string &repository_id,
                              const std::string &revision,
                              const std::string &path,
                              std::size_t maximum = 1000) const;
  void store_receipt(const EvidenceReceipt &receipt);
  std::optional<EvidenceReceipt> receipt(const std::string &receipt_id) const;
  void store_inference_claim(const InferenceClaim &claim);
  void store_knowledge_decision(const KnowledgeDecision &decision,
                                const std::string &knowledge_commit);
  nlohmann::json inference_for_transition(
      const std::string &transition_id, bool include_unreviewed = false) const;
  void store_pr_import(const std::string &import_id,
                       const nlohmann::json &record,
                       const std::string &source_commit = {});
  nlohmann::json pr_imports(const std::string &repository_id) const;
  bool task_published(const std::string &task_id) const;
  void mark_task_published(const std::string &task_id);
  std::string snapshot_id() const;

private:
  void execute(const char *sql) const;
  std::int64_t integer_pragma(const char *sql) const;

  std::filesystem::path root_;
  std::string producer_id_;
  sqlite3 *database_{};
  mutable std::mutex mutex_;
  std::size_t maximum_cached_facts_;
  std::uint64_t maximum_cached_fact_bytes_;
};

} // namespace history
