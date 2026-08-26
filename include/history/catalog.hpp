#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "history/ir.hpp"
#include <nlohmann/json.hpp>

struct sqlite3;

namespace history {

class Catalog {
public:
  struct EnqueueResult {
    std::string work_id;
    bool inserted{};
  };
  explicit Catalog(std::filesystem::path root,
                   std::size_t maximum_cached_facts = 10000,
                   std::uint64_t maximum_cached_fact_bytes =
                       4ULL * 1024ULL * 1024ULL * 1024ULL);
  ~Catalog();
  Catalog(const Catalog &) = delete;
  Catalog &operator=(const Catalog &) = delete;

  const std::filesystem::path &root() const { return root_; }
  const std::string &producer_id() const { return producer_id_; }
  std::size_t maximum_cache_entries() const { return maximum_cached_facts_; }
  std::uint64_t maximum_cache_bytes() const { return maximum_cached_fact_bytes_; }
  std::pair<std::size_t, std::uint64_t> fact_cache_usage() const;

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
  std::string imported_ref_tip(const std::string &ref) const;
  void mark_imported_ref_tip(const std::string &ref, const std::string &commit);
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
  void store_external_fact(const std::string &connector,
                           const std::string &kind,
                           const std::string &external_id,
                           const std::string &content_id,
                           std::int64_t source_updated_at,
                           const nlohmann::json &fact,
                           const std::string &source_commit = {});
  nlohmann::json external_fact(const std::string &connector,
                               const std::string &kind,
                               const std::string &external_id) const;
  void store_connector_status(const std::string &connector,
                              const nlohmann::json &status);
  nlohmann::json connector_status(const std::string &connector) const;
  bool task_published(const std::string &task_id) const;
  void mark_task_published(const std::string &task_id);
  std::string snapshot_id() const;

  EnqueueResult enqueue_work(const std::string &kind,
                             const nlohmann::json &parameters,
                             const std::string &invocation_id = {},
                             const std::string &credential_reference = {},
                             bool maintenance = false,
                             const std::vector<std::string> &dependencies = {});
  void configure_launch(const std::set<std::string> &credential_capabilities,
                        const std::string &executable_identity,
                        const std::string &config_identity);
  std::optional<std::string> pending_launch_token() const;
  std::optional<std::string>
  claim_runner_launch(const std::set<std::string> &credential_capabilities,
                      const std::string &executable_identity,
                      const std::string &config_identity);
  bool clear_failed_launch(const std::string &adoption_token);
  bool adopt_runner(const std::string &adoption_token,
                    const std::string &owner, std::int64_t pid,
                    const std::string &process_start_identity,
                    const std::string &executable_identity,
                    const std::string &config_identity,
                    const std::set<std::string> &credential_capabilities);
  bool heartbeat_runner(const std::string &owner);
  std::optional<nlohmann::json>
  claim_next_work(const std::string &owner,
                  const std::set<std::string> &credential_capabilities);
  void complete_work(const std::string &work_id, const std::string &owner,
                     const nlohmann::json &progress = {});
  nlohmann::json fail_work(const std::string &work_id,
                           const std::string &owner,
                           const std::string &error_fingerprint,
                           std::uint32_t maximum_attempts);
  bool release_runner_if_empty(const std::string &owner);
  nlohmann::json work_status(const std::string &work_id = {}) const;
  bool cancel_work(const std::string &work_id);
  bool work_cancellation_requested(const std::string &work_id) const;
  bool invocation_settled(const std::string &invocation_id) const;
  nlohmann::json runner_status() const;

private:
  void execute(const char *sql) const;
  std::int64_t integer_pragma(const char *sql) const;

  std::filesystem::path root_;
  std::string producer_id_;
  sqlite3 *database_{};
  mutable std::mutex mutex_;
  std::size_t maximum_cached_facts_;
  std::uint64_t maximum_cached_fact_bytes_;
  mutable std::optional<std::string> snapshot_id_cache_;
  std::set<std::string> launch_credential_capabilities_;
  std::string launch_executable_identity_;
  std::string launch_config_identity_;
};

} // namespace history
