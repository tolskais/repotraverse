#include "history/query.hpp"
#include "history/encoding.hpp"

#include "history/build_import.hpp"
#include "history/catalog.hpp"
#include "history/connectors.hpp"
#include "history/file_history.hpp"
#include "history/git_coordination.hpp"
#include "history/history_plan.hpp"
#include "history/process.hpp"
#include "history/telemetry.hpp"
#include "history/worker.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace history {
namespace {
std::string trimmed(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.pop_back();
  return value;
}

std::string resolved_head(const std::filesystem::path &repository,
                          const std::string &ref) {
  const auto result = run_process(
      {"git", "-C", path_to_utf8(repository), "rev-parse", "--verify", ref + "^{commit}"});
  if (result.exit_code != 0)
    throw std::runtime_error("cannot resolve history ref: " + utf8_lossy(result.error));
  return trimmed(result.output);
}

std::string file_digest(const std::filesystem::path &path) {
  if (path.empty()) return stable_hash("no-pr-facts");
  if (std::filesystem::file_size(path) > 256ULL * 1024ULL * 1024ULL)
    throw std::runtime_error("PR facts exceed the supported snapshot size");
  return stable_hash(read_text_file(path).text);
}

class PinnedPlan {
public:
  PinnedPlan() = default;
  PinnedPlan(std::filesystem::path path, bool cached)
      : path_(std::move(path)), cached_(cached) {
    if (cached_) {
      std::scoped_lock lock(mutex());
      ++pins()[path_to_utf8(path_)];
    }
  }
  PinnedPlan(const PinnedPlan &) = delete;
  PinnedPlan &operator=(const PinnedPlan &) = delete;
  PinnedPlan(PinnedPlan &&other) noexcept
      : path_(std::move(other.path_)), cached_(other.cached_) {
    other.cached_ = false;
  }
  ~PinnedPlan() {
    std::error_code ignored;
    if (!cached_) {
      if (!path_.empty()) std::filesystem::remove(path_, ignored);
      return;
    }
    std::scoped_lock lock(mutex());
    const auto key = path_to_utf8(path_);
    if (auto found = pins().find(key); found != pins().end() && --found->second == 0)
      pins().erase(found);
  }
  const std::filesystem::path &path() const { return path_; }

  static std::mutex &mutex() { static std::mutex value; return value; }
  static std::unordered_map<std::string, std::size_t> &pins() {
    static std::unordered_map<std::string, std::size_t> value;
    return value;
  }

private:
  std::filesystem::path path_;
  bool cached_{};
};

bool valid_plan_payload(const std::filesystem::path &path) {
  std::ifstream input(path);
  std::string line;
  bool header = false, summary = false;
  try {
    while (std::getline(input, line)) {
      if (line.empty()) continue;
      const auto row = nlohmann::json::parse(line);
      header = header || row.value("record_type", std::string{}) == "history_plan";
      summary = summary || row.value("record_type", std::string{}) == "history_plan_summary";
    }
  } catch (...) {
    return false;
  }
  return input.eof() && header && summary;
}

void evict_history_plans(const Catalog &catalog,
                         const std::filesystem::path &keep) {
  std::scoped_lock cache_lock(PinnedPlan::mutex());
  const auto directory = catalog.root() / "cache" / "history-plans-v1";
  struct Entry { std::filesystem::path path; std::uint64_t bytes; std::filesystem::file_time_type access; };
  std::vector<Entry> entries;
  std::uint64_t bytes = 0;
  std::error_code error;
  for (const auto &item : std::filesystem::directory_iterator(directory, error)) {
    if (!item.is_regular_file(error) || item.path().extension() != ".jsonl") continue;
    const auto size = item.file_size(error);
    if (error) { error.clear(); continue; }
    entries.push_back({item.path(), size, item.last_write_time(error)});
    bytes += size;
  }
  std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
    return left.access < right.access;
  });
  auto count = entries.size();
  const auto [fact_count, fact_bytes] = catalog.fact_cache_usage();
  for (const auto &entry : entries) {
    if (count + fact_count <= catalog.maximum_cache_entries() &&
        bytes + fact_bytes <= catalog.maximum_cache_bytes()) break;
    if (entry.path == keep || PinnedPlan::pins().contains(path_to_utf8(entry.path))) continue;
    if (std::filesystem::remove(entry.path, error)) {
      --count;
      bytes -= std::min(bytes, entry.bytes);
    }
    error.clear();
  }
}

PinnedPlan cached_history_plan(Catalog *catalog, HistoryPlanOptions options,
                               const std::string &pr_digest) {
  if (!catalog) {
    write_history_plan(options);
    return PinnedPlan(options.output, false);
  }
  const auto head = resolved_head(options.repository, options.ref);
  const auto key = stable_hash(canonical_json({
      {"kind", "history_plan"}, {"schema_version", kSchemaVersion},
      {"planner", "first_parent_v1"}, {"repository_id", options.repository_id},
      {"repository", path_to_utf8(std::filesystem::weakly_canonical(options.repository))},
      {"resolved_head", head}, {"start_exclusive", options.start_exclusive},
      {"pr_fact_snapshot", pr_digest}}));
  const auto directory = catalog->root() / "cache" / "history-plans-v1";
  std::filesystem::create_directories(directory);
  const auto target = directory / (key + ".jsonl");
  std::unique_lock lock(PinnedPlan::mutex());
  std::error_code error;
  if (std::filesystem::exists(target) && valid_plan_payload(target)) {
    std::filesystem::last_write_time(target, std::filesystem::file_time_type::clock::now(), error);
    lock.unlock();
    return PinnedPlan(target, true);
  }
  std::filesystem::remove(target, error);
  const auto temporary = directory / (key + ".tmp-" +
      stable_hash(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
  options.ref = head;
  options.output = temporary;
  try {
    write_history_plan(options);
    const auto size = std::filesystem::file_size(temporary);
    const auto [fact_count, fact_bytes] = catalog->fact_cache_usage();
    if (fact_count >= catalog->maximum_cache_entries() ||
        size > catalog->maximum_cache_bytes() -
                   std::min(catalog->maximum_cache_bytes(), fact_bytes))
      {
        lock.unlock();
      return PinnedPlan(temporary, false);
      }
    std::filesystem::rename(temporary, target);
    lock.unlock();
    evict_history_plans(*catalog, target);
    return PinnedPlan(target, true);
  } catch (...) {
    std::filesystem::remove(temporary, error);
    throw;
  }
}

const nlohmann::json &parameters(const nlohmann::json &request) {
  if (!request.contains("params") || !request.at("params").is_object())
    throw std::runtime_error("request.params must be an object");
  return request.at("params");
}
std::filesystem::path path_parameter(const nlohmann::json &params,
                                     const char *name) {
  if (!params.contains(name) || !params.at(name).is_string())
    throw std::runtime_error(std::string("missing path parameter: ") + name);
  return path_from_utf8(params.at(name).get<std::string>());
}
std::vector<LineageAssertion> assertions_from(const nlohmann::json &params) {
  if (!params.contains("assertions"))
    return {};
  const auto value = nlohmann::json::parse(
      read_text_file(path_parameter(params, "assertions")).text);
  if (value.value("schema_version", 0U) != kSchemaVersion ||
      !value.contains("assertions"))
    throw std::runtime_error("invalid lineage assertion resource");
  auto assertions = value.at("assertions").get<std::vector<LineageAssertion>>();
  std::map<std::string, std::string> accepted_successors;
  for (const auto &assertion : assertions) {
    if (assertion.status == "accepted" && assertion.reviewed_by.empty())
      throw std::runtime_error(
          "accepted lineage assertion requires reviewed_by");
    if (assertion.status == "accepted" &&
        assertion.relation == "same_element") {
      const auto [found, inserted] = accepted_successors.emplace(
          assertion.before_element, assertion.after_element);
      if (!inserted && found->second != assertion.after_element)
        throw std::runtime_error("conflicting accepted lineage assertions");
    }
  }
  return assertions;
}
} // namespace

EvidenceBundle MemoryFactStore::load(const std::filesystem::path &path) const {
  const auto value = nlohmann::json::parse(read_text_file(path).text);
  if (value.value("record_type", std::string{}) == "tu_manifest") {
    const auto manifest = value.get<TuManifest>();
    EvidenceBundle bundle;
    bundle.source_revision = manifest.source_revision;
    bundle.configuration = manifest.configuration;
    bundle.context_fingerprint = manifest.context_id;
    bundle.extractor_fingerprint = manifest.extractor_fingerprint;
    bundle.producer = manifest.producer;
    bundle.coverage = manifest.coverage;
    std::map<std::string, LogicalElement> elements;
    std::map<std::string, SemanticVariant> variants;
    for (const auto &item : manifest.elements)
      elements[item.element_id] = item;
    for (const auto &item : manifest.variants)
      variants[item.variant_id] = item;
    std::map<std::string, ElementSnapshot> snapshots;
    for (const auto &observation : manifest.observations) {
      const auto &logical = elements.at(observation.element_id);
      const auto &variant = variants.at(observation.variant_id);
      ElementSnapshot snapshot;
      snapshot.compiler_id = logical.element_id;
      snapshot.kind = logical.kind;
      snapshot.qualified_name = logical.qualified_name;
      snapshot.linkage = logical.linkage;
      snapshot.interface_fingerprint = variant.interface_fingerprint;
      snapshot.implementation_fingerprint = variant.implementation_fingerprint;
      snapshot.dependency_fingerprint = variant.dependency_fingerprint;
      snapshot.referenced_compiler_ids = variant.referenced_element_ids;
      snapshot.location = observation.location;
      const auto existing = snapshots.find(observation.element_id);
      if (existing == snapshots.end() ||
          observation.location.role == "definition")
        snapshots[observation.element_id] = std::move(snapshot);
    }
    for (auto &[id, snapshot] : snapshots)
      bundle.elements.push_back(std::move(snapshot));
    return bundle;
  }
  return value.get<EvidenceBundle>();
}

QueryService::QueryService(std::shared_ptr<const FactStore> store)
    : store_(std::move(store)) {
  if (!store_)
    throw std::invalid_argument("FactStore cannot be null");
}

QueryService::QueryService(std::shared_ptr<const FactStore> store,
                           std::shared_ptr<Catalog> catalog,
                           std::shared_ptr<GitCoordinator> coordinator,
                           std::shared_ptr<BackgroundWorker> worker,
                           std::shared_ptr<ConnectorService> connectors)
    : store_(std::move(store)), catalog_(std::move(catalog)),
      coordinator_(std::move(coordinator)), worker_(std::move(worker)),
      connectors_(std::move(connectors)) {
  if (!store_)
    throw std::invalid_argument("FactStore cannot be null");
  if (!catalog_ || !coordinator_)
    throw std::invalid_argument(
        "federated query service requires catalog and coordinator");
}

nlohmann::json QueryService::execute(const nlohmann::json &request) const {
  auto response = execute_impl(request);
  const auto query = request.value("query", std::string{});
  if (!query.starts_with("tool.")) return response;
  response["operation"] = query.substr(5);
  const auto result_status = response.value("result_status", std::string{});
  response["status"] = !response.value("ok", false) ? "failed"
                         : result_status.empty() ? "complete" : result_status;
  if (!response.contains("facts")) response["facts"] = nlohmann::json::object();
  if (!response.contains("inference"))
    response["inference"] = {{"accepted", nlohmann::json::array()},
                             {"conflicts", nlohmann::json::array()}};
  else if (response.at("inference").is_array())
    response["inference"] = {{"accepted", response.at("inference")},
                             {"conflicts", nlohmann::json::array()}};
  if (response.at("inference").is_object()) {
    if (!response["inference"].contains("accepted"))
      response["inference"]["accepted"] = nlohmann::json::array();
    if (!response["inference"].contains("conflicts"))
      response["inference"]["conflicts"] = nlohmann::json::array();
  }
  if (!response.contains("coverage"))
    response["coverage"] = {{"status", response.at("status")},
                            {"capabilities", nlohmann::json::array()},
                            {"gaps", nlohmann::json::array()}};
  response["provenance"] = {
      {"snapshot_id", response.value("snapshot_id", std::string{})}};
  response["pending_work"] = response.value("pending_work", nlohmann::json::array());
  if (!response.contains("continuation"))
    response["continuation"] = response.at("facts").contains("continuation")
                                   ? response.at("facts").at("continuation")
                                   : nlohmann::json(nullptr);
  return response;
}

nlohmann::json QueryService::execute_impl(const nlohmann::json &request) const {
  try {
    if (request.value("schema_version", 0U) != kSchemaVersion)
      throw std::runtime_error("unsupported request schema");
    const auto &params = parameters(request);
    const auto query = request.value("query", std::string{});
    if (query == "tool.connector-sync") {
      if (!connectors_) throw std::runtime_error("connector synchronization is not configured");
      const auto name = params.value("connector", std::string{});
      if (name.empty()) throw std::runtime_error("connector-sync requires connector");
      const auto keys = params.value("issue_keys", std::vector<std::string>{});
      auto result = connectors_->sync(name, params.value("full", false), keys);
      return {{"schema_version", kSchemaVersion}, {"ok", true},
              {"result_status", "complete"}, {"snapshot_id", catalog_->snapshot_id()},
              {"facts", std::move(result)}, {"inference", nlohmann::json::array()}};
    }
    if (query == "tool.connector-status") {
      if (!connectors_) throw std::runtime_error("connectors are not configured");
      const auto name = params.value("connector", std::string{});
      return {{"schema_version", kSchemaVersion}, {"ok", true},
              {"result_status", "complete"}, {"facts", connectors_->status(name)},
              {"inference", nlohmann::json::array()}};
    }
    if (query == "tool.pull-request-get" || query == "tool.issue-get") {
      if (!connectors_) throw std::runtime_error("connectors are not configured");
      const auto name = params.value("connector", std::string{});
      const auto id = params.value(query == "tool.issue-get" ? "key" : "external_id",
                                   std::string{});
      if (name.empty() || id.empty()) throw std::runtime_error(query + " requires connector and external identity");
      auto result = query == "tool.issue-get" ? connectors_->issue(name, id)
                                               : connectors_->pull_request(name, id);
      if (result.empty()) throw std::runtime_error("external fact not found");
      return {{"schema_version", kSchemaVersion}, {"ok", true},
              {"result_status", result.at("conflicts").empty() ? "complete" : "partial"},
              {"snapshot_id", catalog_->snapshot_id()}, {"facts", std::move(result)},
              {"inference", nlohmann::json::array()}};
    }
    if (query == "tool.repository-changes" || query == "tool.history-summary" ||
        query == "tool.change-unit") {
      HistoryPlanOptions options;
      options.repository = path_parameter(params, "repository");
      options.ref = params.value("ref", std::string{"HEAD"});
      options.repository_id = params.value("repository_id", std::string{});
      options.start_exclusive = params.value("start_exclusive", std::string{});
      if (params.contains("pr_facts"))
        options.pr_facts = path_parameter(params, "pr_facts");
      const auto nonce = std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
      options.output = (catalog_ ? catalog_->root() : std::filesystem::temp_directory_path()) /
                       ("history-plan-" + stable_hash(nonce) + ".jsonl");
      std::filesystem::path imported_pr_facts;
      if (options.pr_facts.empty() && catalog_ && params.contains("repository_id")) {
        const auto imports = catalog_->pr_imports(
            params.at("repository_id").get<std::string>());
        if (!imports.empty()) {
          imported_pr_facts = options.output;
          imported_pr_facts += ".pr-imports";
          std::ofstream imported(imported_pr_facts);
          if (!imported) throw std::runtime_error("cannot materialize cached PR imports");
          for (auto record : imports) {
            record.erase("import_id");
            record.erase("knowledge_commit");
            imported << canonical_json(record) << '\n';
          }
          options.pr_facts = imported_pr_facts;
        }
      }
      struct RemoveImports {
        std::filesystem::path path;
        ~RemoveImports() {
          std::error_code ignored;
          std::filesystem::remove(path, ignored);
        }
      } remove_imports{imported_pr_facts};
      auto pinned_plan = cached_history_plan(catalog_.get(), options,
                                             file_digest(options.pr_facts));
      std::ifstream input(pinned_plan.path());
      nlohmann::json units = nlohmann::json::array();
      nlohmann::json plan_header, summary;
      std::map<std::string, std::size_t> file_touches;
      std::string line;
      while (std::getline(input, line)) {
        if (line.empty()) continue;
        auto record = nlohmann::json::parse(line);
        const auto type = record.value("record_type", std::string{});
        if (type == "history_plan") plan_header = std::move(record);
        else if (type == "history_plan_summary") summary = std::move(record);
        else if (type == "integration_unit") {
          for (const auto &change : record.value("changes", nlohmann::json::array()))
            ++file_touches[change.value("path", std::string{})];
          units.push_back(std::move(record));
        }
      }
      if (!input.eof() || plan_header.is_null() || summary.is_null())
        throw std::runtime_error("cached history plan is incomplete");
      auto plan_summary = summary;
      plan_summary.erase("schema_version");
      plan_summary.erase("record_type");
      plan_summary["output"] = path_to_utf8(pinned_plan.path());
      plan_summary["ref"] = options.ref;
      if (query == "tool.change-unit") {
        const auto wanted = params.value("integration_unit_id", std::string{});
        nlohmann::json found;
        for (const auto &unit : units)
          if (unit.value("integration_unit_id", std::string{}) == wanted) {
            found = unit;
            break;
          }
        if (found.is_null()) throw std::runtime_error("integration unit not found");
        return {{"schema_version", kSchemaVersion}, {"ok", true},
                {"result_status", "complete"}, {"facts", found},
                {"inference", nlohmann::json::array()}};
      }
      nlohmann::json files = nlohmann::json::array();
      for (const auto &[path, touches] : file_touches)
        if (!path.empty()) files.push_back({{"path", path}, {"integration_unit_touches", touches}});
      std::sort(files.begin(), files.end(), [](const auto &left, const auto &right) {
        const auto lc = left.at("integration_unit_touches").template get<std::size_t>();
        const auto rc = right.at("integration_unit_touches").template get<std::size_t>();
        return lc != rc ? lc > rc : left.at("path") < right.at("path");
      });
      const auto total_files = files.size();
      const auto file_limit = std::clamp<std::size_t>(
          params.value("file_limit", std::size_t{100}), 1, 1000);
      if (files.size() > file_limit) files.erase(files.begin() + file_limit, files.end());
      const auto pinned_head = summary.value("resolved_head", std::string{});
      if (params.contains("pinned_head") &&
          params.at("pinned_head").get<std::string>() != pinned_head)
        throw std::runtime_error("history ref moved since the requested page was pinned");
      const auto offset = params.value("offset", std::size_t{});
      const auto limit = std::clamp<std::size_t>(params.value("limit", std::size_t{100}), 1, 1000);
      nlohmann::json page = nlohmann::json::array();
      for (std::size_t index = offset; index < units.size() && page.size() < limit; ++index)
        page.push_back(units[index]);
      nlohmann::json facts = {{"plan", plan_header}, {"summary", summary},
                              {"planner", plan_summary}, {"pinned_head", pinned_head},
                              {"files_by_change_count", files},
                              {"file_count", total_files},
                              {"files_truncated", total_files > files.size()}};
      if (query == "tool.repository-changes") facts["integration_units"] = std::move(page);
      facts["continuation"] = query == "tool.repository-changes" &&
                                      offset + page.size() < units.size()
                                  ? nlohmann::json{{"offset", offset + page.size()},
                                                   {"pinned_head", pinned_head}}
                                  : nlohmann::json(nullptr);
      return {{"schema_version", kSchemaVersion}, {"ok", true},
              {"result_status", "complete"}, {"facts", std::move(facts)},
              {"inference", nlohmann::json::array()},
              {"coverage", {{"status", "complete"},
                            {"capabilities", {"git_integration_units", "file_change_counts"}},
                            {"gaps", nlohmann::json::array()}}}};
    }
    if (query == "tool.symbol-search" || query == "tool.symbol-history") {
      if (!catalog_) throw std::runtime_error(query + " requires service catalog");
      const auto repository_id = params.value("repository_id", std::string{});
      const auto revision = query == "tool.symbol-history"
                                ? std::string{}
                                : params.value("revision", std::string{});
      if (repository_id.empty() || !params.contains("selector"))
        throw std::runtime_error(query + " requires repository_id and selector");
      const auto maximum = std::clamp<std::size_t>(
          params.value("maximum", std::size_t{100}), 1, 10000);
      auto result = catalog_->symbol_search(repository_id, revision,
                                            params.at("selector"), maximum);
      return {{"schema_version", kSchemaVersion}, {"ok", true},
              {"result_status", "complete"},
              {"snapshot_id", catalog_->snapshot_id()},
              {"facts", result}, {"inference", nlohmann::json::array()},
              {"coverage", {{"status", "complete"},
                            {"capabilities", {"compiler_symbols"}},
                            {"gaps", nlohmann::json::array()}}}};
    }
    if (query == "tool.file-symbols") {
      if (!catalog_) throw std::runtime_error("tool.file-symbols requires service catalog");
      const auto repository_id = params.value("repository_id", std::string{});
      const auto revision = params.value("revision", std::string{});
      const auto path = params.value("path", std::string{});
      if (repository_id.empty() || revision.empty() || path.empty())
        throw std::runtime_error("tool.file-symbols requires repository_id, revision, and path");
      const auto maximum = std::clamp<std::size_t>(
          params.value("maximum", std::size_t{1000}), 1, 10000);
      auto facts = catalog_->file_symbols(repository_id, revision, path, maximum);
      const auto contexts = catalog_->compile_contexts(path, revision);
      nlohmann::json observations = nlohmann::json::array();
      bool inventory_complete = !contexts.empty();
      for (const auto &context : contexts) {
        observations.push_back({{"inventory_id", context.inventory_id},
                                {"context_id", context.context_id},
                                {"translation_unit", context.translation_unit},
                                {"build_variant", context.build_variant},
                                {"inventory_complete", context.inventory_complete},
                                {"dependency_map_complete", context.dependency_map_complete}});
        inventory_complete = inventory_complete && context.inventory_complete &&
                             context.dependency_map_complete;
      }
      facts["translation_unit_observations"] = std::move(observations);
      facts["inventory_complete"] = inventory_complete;
      return {{"schema_version", kSchemaVersion}, {"ok", true},
              {"result_status", inventory_complete ? "complete" : "partial"},
              {"snapshot_id", catalog_->snapshot_id()}, {"facts", facts},
              {"inference", nlohmann::json::array()},
              {"coverage", {{"status", inventory_complete ? "complete" : "partial"},
                            {"capabilities", {"compiler_symbols", "including_translation_units"}},
                            {"gaps", inventory_complete ? nlohmann::json::array()
                                                        : nlohmann::json::array({"declared inventory or dependency map is incomplete"})}}}};
    }
    if (query == "tool.symbol-relations") {
      if (!catalog_) throw std::runtime_error("tool.symbol-relations requires service catalog");
      const auto repository_id = params.value("repository_id", std::string{});
      const auto revision = params.value("revision", std::string{});
      if (!params.contains("element_ids") || !params.at("element_ids").is_array())
        throw std::runtime_error("tool.symbol-relations requires element_ids");
      auto facts = catalog_->semantic_dependents(
          repository_id, revision,
          params.at("element_ids").get<std::vector<std::string>>(),
          std::clamp<std::size_t>(params.value("maximum", std::size_t{1000}), 1, 100000));
      return {{"schema_version", kSchemaVersion}, {"ok", true},
              {"result_status", facts.value("truncated", false) ? "partial" : "complete"},
              {"snapshot_id", catalog_->snapshot_id()}, {"facts", facts},
              {"inference", nlohmann::json::array()}};
    }
    if (query == "tool.inference-get") {
      if (!catalog_) throw std::runtime_error("tool.inference-get requires service catalog");
      const auto transition_id = params.value("transition_id", std::string{});
      if (transition_id.empty()) throw std::runtime_error("transition_id is required");
      return {{"schema_version", kSchemaVersion}, {"ok", true},
              {"result_status", "complete"},
              {"facts", {{"transition_id", transition_id}}},
              {"inference", catalog_->inference_for_transition(
                  transition_id, params.value("include_unreviewed", false))}};
    }
    if (query == "tool.claim-propose") {
      if (!catalog_) throw std::runtime_error("tool.claim-propose requires service catalog");
      if (!params.contains("claim")) throw std::runtime_error("claim is required");
      auto claim = params.at("claim").get<InferenceClaim>();
      if (claim.producer_id.empty()) claim.producer_id = catalog_->producer_id();
      for (const auto &receipt_id : claim.evidence_receipt_ids) {
        const auto receipt = catalog_->receipt(receipt_id);
        if (!receipt)
          throw std::runtime_error("claim references an unknown evidence receipt");
        if (receipt->transition_id != claim.transition_id)
          throw std::runtime_error("claim evidence receipt targets another transition");
      }
      if (claim.claim_id.empty()) {
        auto identity = nlohmann::json(claim);
        identity.erase("claim_id");
        claim.claim_id = stable_hash(canonical_json(identity));
      }
      nlohmann::json publication = coordinator_
                                       ? coordinator_->publish_inference_claim(claim)
                                       : nlohmann::json{{"state", "local_only"}};
      return {{"schema_version", kSchemaVersion}, {"ok", true},
              {"result_status", "complete"},
              {"facts", nlohmann::json::object()},
              {"inference", {{"proposal", claim}, {"review_state", "unreviewed"},
                             {"publication", publication}}}};
    }
    if (query == "tool.receipt-put" || query == "tool.receipt-get") {
      if (!catalog_) throw std::runtime_error(query + " requires service catalog");
      if (query == "tool.receipt-put") {
        if (!params.contains("receipt")) throw std::runtime_error("receipt is required");
        auto receipt = params.at("receipt").get<EvidenceReceipt>();
        if (receipt.producer_id.empty()) receipt.producer_id = catalog_->producer_id();
        if (receipt.receipt_id.empty()) {
          auto identity = nlohmann::json(receipt);
          identity.erase("receipt_id");
          receipt.receipt_id = stable_hash(canonical_json(identity));
        }
        nlohmann::json publication = coordinator_
                                         ? coordinator_->publish_receipt(receipt)
                                         : nlohmann::json{{"state", "local_only"}};
        return {{"schema_version", kSchemaVersion}, {"ok", true},
                {"result_status", "complete"},
                {"facts", {{"receipt", receipt}, {"publication", publication}}},
                {"inference", nlohmann::json::array()}};
      }
      const auto receipt = catalog_->receipt(params.value("receipt_id", std::string{}));
      if (!receipt) throw std::runtime_error("evidence receipt not found");
      return {{"schema_version", kSchemaVersion}, {"ok", true},
              {"result_status", "complete"}, {"facts", *receipt},
              {"inference", nlohmann::json::array()}};
    }
    if (query == "tool.claim-verify") {
      if (!catalog_ || !coordinator_)
        throw std::runtime_error("tool.claim-verify requires coordinated service");
      if (!params.contains("decision")) throw std::runtime_error("decision is required");
      auto decision = params.at("decision").get<KnowledgeDecision>();
      if (decision.decision_id.empty()) {
        auto identity = nlohmann::json(decision);
        identity.erase("decision_id");
        decision.decision_id = stable_hash(canonical_json(identity));
      }
      const auto publication = coordinator_->publish_knowledge_decision(decision);
      return {{"schema_version", kSchemaVersion}, {"ok", true},
              {"result_status", "complete"},
              {"facts", {{"publication", publication}}},
              {"inference", {{"decision", decision}}}};
    }
    if (query == "tool.pr-import") {
      if (!catalog_ || !coordinator_)
        throw std::runtime_error("tool.pr-import requires coordinated service");
      if (!params.contains("records") || !params.at("records").is_array())
        throw std::runtime_error("tool.pr-import requires records");
      nlohmann::json publications = nlohmann::json::array();
      for (const auto &record : params.at("records")) {
        if (!record.is_object() || record.value("repository_id", std::string{}).empty() ||
            record.value("source", std::string{}).empty())
          throw std::runtime_error("each PR import requires repository_id and source provenance");
        publications.push_back(coordinator_->publish_pr_import(record));
      }
      return {{"schema_version", kSchemaVersion}, {"ok", true},
              {"result_status", "complete"},
              {"facts", {{"imported", publications.size()},
                         {"publications", publications}}},
              {"inference", nlohmann::json::array()}};
    }
    if (query == "build.import") {
      if (!catalog_)
        throw std::runtime_error("build.import requires federated service");
      const auto result =
          import_build_log(*catalog_, path_parameter(params, "input"),
                           params.value("repository", std::string{}));
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"result_status", "complete"},
              {"snapshot_id", catalog_->snapshot_id()},
              {"result", result}};
    }
    if (query == "file.history") {
      if (!catalog_)
        throw std::runtime_error("file.history requires federated service");
      if (!worker_)
        throw std::runtime_error(
            "file.history requires a configured extractor worker");
      FileHistoryOptions options;
      options.repository = path_parameter(params, "repository");
      if (worker_ && !worker_->source_repository().empty() &&
          std::filesystem::weakly_canonical(options.repository) !=
              std::filesystem::weakly_canonical(worker_->source_repository()))
        throw std::runtime_error(
            "file.history repository is outside the configured source root");
      if (params.contains("pr_facts"))
        options.pr_facts = path_parameter(params, "pr_facts");
      options.ref = params.value("ref", std::string{"HEAD"});
      options.repository_id = params.value("repository_id", std::string{});
      options.path = params.value("path", std::string{});
      options.scope = params.value("scope", std::string{"direct"});
      if (worker_)
        options.extractor_identity = worker_->extractor_identity();
      if (worker_)
        options.repository_id = worker_->repository_id();
      options.since = params.value("since", std::int64_t{});
      options.page_size = params.value("page_size", std::size_t{100});
      const auto continuation = params.value("continuation", std::string{});
      if (!continuation.empty())
        options.offset = std::stoull(continuation);
      auto result = plan_file_history(*catalog_, options);
      nlohmann::json publication = {
          {"state", result.at("pending_work").empty() ? "published" : "queued"},
          {"task_count", result.at("pending_work").size()}};
      result["task_publication"] = publication;
      if (publication.value("state", std::string{}) != "published") {
        result["result_status"] = "partial";
        result["coverage"]["status"] = "partial";
        result["coverage"]["gaps"].push_back(
            {{"kind", "task_publication_pending"},
             {"message", "tasks are queued for the coordination loop"}});
      }
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"result_status", result.at("result_status")},
              {"snapshot_id", catalog_->snapshot_id()},
              {"request_id", result.at("request_id")},
              {"coverage", result.at("coverage")},
              {"pending_work", result.at("pending_work")},
              {"result", std::move(result)}};
    }
    if (query == "catalog.sync") {
      if (!coordinator_)
        throw std::runtime_error("catalog.sync requires federated service");
      const auto result = coordinator_->sync();
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"result_status",
               result.value("state", std::string{}) == "synchronized"
                   ? "complete"
                   : "partial"},
              {"snapshot_id", catalog_->snapshot_id()},
              {"result", result}};
    }
    if (query == "history.plan") {
      HistoryPlanOptions options;
      options.repository = path_parameter(params, "repository");
      options.output = path_parameter(params, "output");
      options.ref = params.value("ref", std::string{"HEAD"});
      options.start_exclusive = params.value("start_exclusive", std::string{});
      if (params.contains("pr_facts"))
        options.pr_facts = path_parameter(params, "pr_facts");
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"result", write_history_plan(options)}};
    }
    if (query == "lineage.review.submit") {
      if (!catalog_)
        throw std::runtime_error("lineage review requires federated service");
      if (!params.contains("relation"))
        throw std::runtime_error("lineage review requires relation");
      auto relation = params.at("relation").get<LineageRelation>();
      static const std::set<std::string> kinds = {
          "same",    "renamed", "moved", "moved_and_renamed",
          "extract", "inline",  "split", "merge"};
      static const std::set<std::string> states = {"candidate", "accepted",
                                                   "rejected"};
      if (relation.repository_id.empty() || !kinds.contains(relation.kind) ||
          !states.contains(relation.review_state) ||
          relation.source_element_ids.empty() ||
          relation.target_element_ids.empty())
        throw std::runtime_error("invalid lineage relation");
      if (worker_ && !worker_->repository_id().empty() &&
          relation.repository_id != worker_->repository_id())
        throw std::runtime_error("lineage relation repository mismatch");
      if (relation.review_state == "accepted" && relation.reviewer.empty())
        throw std::runtime_error("accepted relation requires reviewer");
      std::sort(relation.source_element_ids.begin(),
                relation.source_element_ids.end());
      std::sort(relation.target_element_ids.begin(),
                relation.target_element_ids.end());
      if (relation.relation_id.empty())
        relation.relation_id = stable_hash(
            relation.repository_id + "\n" + relation.kind + "\n" +
            nlohmann::json(relation.source_element_ids).dump() + "\n" +
            nlohmann::json(relation.target_element_ids).dump());
      nlohmann::json publication = {{"state", "local_candidate"}};
      if (relation.review_state == "candidate")
        catalog_->store_lineage_relation(relation);
      else
        publication = coordinator_->publish_review(relation);
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"result_status",
               publication.value("state", std::string{}) == "published"
                   ? "complete"
                   : "partial"},
              {"result", relation},
              {"publication", publication}};
    }
    if (query == "lineage.review.get") {
      if (!catalog_)
        throw std::runtime_error("lineage review requires federated service");
      const auto relation_id = params.value("relation_id", std::string{});
      const auto relation = catalog_->lineage_relation(relation_id);
      if (!relation)
        throw std::runtime_error("lineage relation not found");
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"result_status", "complete"},
              {"result", *relation}};
    }
    if (query == "submodule.map") {
      if (!catalog_)
        throw std::runtime_error(
            "submodule mapping requires federated service");
      if (!params.contains("mapping"))
        throw std::runtime_error("submodule.map requires mapping");
      const auto mapping = params.at("mapping").get<SubmoduleRevision>();
      if (worker_ && !worker_->repository_id().empty() &&
          mapping.parent_repository_id != worker_->repository_id())
        throw std::runtime_error("submodule parent repository mismatch");
      catalog_->store_submodule_revision(mapping);
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"result_status", "complete"},
              {"result", mapping}};
    }
    if (query == "submodule.revisions") {
      if (!catalog_)
        throw std::runtime_error(
            "submodule mapping requires federated service");
      const auto parent = params.value("parent_repository_id", std::string{});
      const auto revision = params.value("parent_revision", std::string{});
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"result_status", "complete"},
              {"result", catalog_->submodule_revisions(parent, revision)}};
    }
    if (query == "semantic.dependents") {
      if (!catalog_)
        throw std::runtime_error(
            "semantic dependencies require federated service");
      const auto repository_id = params.value("repository_id", std::string{});
      const auto revision = params.value("revision", std::string{});
      if (!params.contains("element_ids") ||
          !params.at("element_ids").is_array())
        throw std::runtime_error("semantic.dependents requires element_ids");
      const auto maximum = std::clamp<std::size_t>(
          params.value("maximum", std::size_t{10000}), 1, 100000);
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"result_status", "complete"},
              {"result",
               catalog_->semantic_dependents(
                   repository_id, revision,
                   params.at("element_ids").get<std::vector<std::string>>(),
                   maximum)}};
    }
    if (query == "lineage.transition" || query == "lineage.resolve") {
      const auto before = store_->load(path_parameter(params, "before"));
      const auto after = store_->load(path_parameter(params, "after"));
      auto assertions = query == "lineage.resolve"
                            ? assertions_from(params)
                            : std::vector<LineageAssertion>{};
      std::vector<LineageRelation> reviewed;
      if (query == "lineage.resolve" && params.contains("relation_ids")) {
        if (!catalog_ || !params.at("relation_ids").is_array())
          throw std::runtime_error("relation_ids require federated service");
        for (const auto &id : params.at("relation_ids")) {
          const auto relation =
              catalog_->lineage_relation(id.get<std::string>());
          if (!relation || relation->review_state != "accepted")
            throw std::runtime_error("lineage relation is not accepted");
          reviewed.push_back(*relation);
          if (relation->source_element_ids.size() == 1 &&
              relation->target_element_ids.size() == 1)
            assertions.push_back(
                {relation->relation_id, relation->source_element_ids.front(),
                 relation->target_element_ids.front(), "same_element",
                 "accepted", relation->author, relation->reviewer});
        }
      }
      auto result = trace_transition(
          before, after, assertions,
          params.value("integration_unit_id", std::string{}));
      result.reviewed_relations = std::move(reviewed);
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"coverage", result.coverage},
              {"result", result}};
    }
    if (query == "element.history_stats" || query == "file.history_stats") {
      if (!params.contains("bundles") || !params.at("bundles").is_array() ||
          params.at("bundles").size() < 2)
        throw std::runtime_error(
            query + " requires ordered bundles");
      std::vector<EvidenceBundle> bundles;
      for (const auto &path : params.at("bundles"))
        bundles.push_back(store_->load(path.get<std::string>()));
      nlohmann::json change_units = nlohmann::json::array();
      const bool supplied_change_units = params.contains("change_units");
      if (supplied_change_units &&
          (!params.at("change_units").is_array() ||
           params.at("change_units").size() + 1 != bundles.size()))
        throw std::runtime_error(
            "change_units must contain one record per adjacent bundle pair");
      for (std::size_t i = 0; i + 1 < bundles.size(); ++i) {
        const auto supplied = supplied_change_units
                                  ? params.at("change_units").at(i)
                                  : nlohmann::json::object();
        if (!supplied.is_object())
          throw std::runtime_error("change unit must be an object");
        const auto base = supplied.value(
            "base_revision",
            supplied.value("base_commit", bundles[i].source_revision));
        const auto result = supplied.value(
            "result_revision",
            supplied.value("result_commit",
                           supplied.value("head_commit",
                                          bundles[i + 1].source_revision)));
        if (base != bundles[i].source_revision ||
            result != bundles[i + 1].source_revision)
          throw std::runtime_error(
              "change unit endpoints do not match adjacent bundles");
        auto id = supplied.value(
            "change_unit_id",
            supplied.value("integration_unit_id", std::string{}));
        if (id.empty()) {
          if (supplied_change_units)
            throw std::runtime_error("change unit identity is required");
          id = "revision-transition-" + stable_hash(
                   base + "\n" + result + "\n" + bundles[i].configuration);
        }
        nlohmann::json unit = {
            {"change_unit_id", id},
            {"base_revision", base},
            {"result_revision", result},
            {"unit_kind", supplied_change_units ? "integration_unit"
                                                 : "revision_transition"},
            {"association_status",
             supplied.value("association_status",
                            supplied_change_units ? std::string{"unknown"}
                                                  : std::string{"not_supplied"})}};
        if (supplied.contains("integration_unit_id"))
          unit["integration_unit_id"] = supplied.at("integration_unit_id");
        if (supplied.contains("pr_matches"))
          unit["pr_matches"] = supplied.at("pr_matches");
        if (supplied.contains("integrated_commits")) {
          if (!supplied.at("integrated_commits").is_array())
            throw std::runtime_error("integrated_commits must be an array");
          unit["integrated_commit_count"] =
              supplied.at("integrated_commits").size();
        }
        change_units.push_back(std::move(unit));
      }
      const auto assertions = assertions_from(params);
      std::vector<TransitionResult> transitions;
      std::map<std::string, std::string> parent;
      const auto find = [&](auto &&self, const std::string &id) -> std::string {
        auto [it, inserted] = parent.emplace(id, id);
        if (it->second == id)
          return id;
        return it->second = self(self, it->second);
      };
      const auto unite = [&](const std::string &left,
                             const std::string &right) {
        const auto a = find(find, left);
        const auto b = find(find, right);
        if (a != b)
          parent[std::max(a, b)] = std::min(a, b);
      };
      for (std::size_t i = 0; i + 1 < bundles.size(); ++i) {
        transitions.push_back(trace_transition(
            bundles[i], bundles[i + 1], assertions,
            change_units.at(i).at("change_unit_id").get<std::string>()));
        for (const auto &fact : transitions.back().facts)
          if (!fact.before_element.empty() && !fact.after_element.empty())
            unite(fact.before_element, fact.after_element);
      }
      struct Stats {
        std::set<std::size_t> versions, changed, interface_changed,
            implementation_changed, dependency_changed, renamed, moved,
            ambiguous, unresolved;
        std::string current_name, current_kind, current_path, added_revision,
            last_observed_revision, last_changed_revision;
        nlohmann::json events = nlohmann::json::array();
      };
      std::map<std::string, Stats> stats;
      for (std::size_t version = 0; version < bundles.size(); ++version)
        for (const auto &element : bundles[version].elements) {
          auto &item = stats[find(find, element.compiler_id)];
          item.versions.insert(version);
          item.current_name = element.qualified_name;
          item.current_kind = element.kind;
          item.current_path = element.location.path;
          if (item.added_revision.empty())
            item.added_revision = bundles[version].source_revision;
          item.last_observed_revision = bundles[version].source_revision;
        }
      for (std::size_t i = 0; i < transitions.size(); ++i)
        for (const auto &fact : transitions[i].facts) {
          const auto id = !fact.before_element.empty() ? fact.before_element
                                                       : fact.after_element;
          auto &item = stats[find(find, id)];
          if (fact.content_change == "interface" ||
              fact.content_change == "both")
            item.interface_changed.insert(i);
          if (fact.content_change == "implementation" ||
              fact.content_change == "both")
            item.implementation_changed.insert(i);
          if (fact.content_change != "none" &&
              fact.content_change != "unverified") {
            item.changed.insert(i);
            item.last_changed_revision = fact.after_revision;
          }
          if (fact.dependencies_changed)
            item.dependency_changed.insert(i);
          if (fact.continuity == "renamed" ||
              fact.continuity == "moved_and_renamed")
            item.renamed.insert(i);
          if (fact.continuity == "moved" ||
              fact.continuity == "moved_and_renamed")
            item.moved.insert(i);
          if (fact.confidence == "ambiguous")
            item.ambiguous.insert(i);
          if (fact.continuity == "added_or_unresolved" ||
              fact.continuity == "deleted_or_unresolved")
            item.unresolved.insert(i);
          if (fact.content_change != "none" || fact.dependencies_changed ||
              fact.continuity != "same")
            item.events.push_back(
                {{"change_unit_id",
                  change_units.at(i).at("change_unit_id")},
                 {"unit_kind", change_units.at(i).at("unit_kind")},
                 {"association_status",
                  change_units.at(i).at("association_status")},
                 {"before_revision", fact.before_revision},
                 {"after_revision", fact.after_revision},
                 {"before_element", fact.before_element},
                 {"after_element", fact.after_element},
                 {"continuity", fact.continuity},
                 {"content_change", fact.content_change},
                 {"dependencies_changed", fact.dependencies_changed},
                 {"resolution", fact.resolution},
                 {"confidence", fact.confidence},
                 {"transition_id", fact.transition_id}});
        }
      nlohmann::json rows = nlohmann::json::array();
      const auto revision_timestamps =
          params.value("revision_timestamps", nlohmann::json::object());
      if (!revision_timestamps.is_object())
        throw std::runtime_error("revision_timestamps must be an object");
      for (const auto &[root, item] : stats) {
        std::size_t observable = 0;
        for (const auto version : item.versions)
          if (item.versions.contains(version + 1)) ++observable;
        nlohmann::json observations = nlohmann::json::array();
        for (const auto version : item.versions) {
          nlohmann::json observation = {
              {"revision", bundles[version].source_revision},
              {"bundle_index", version}};
          if (const auto timestamp = revision_timestamps.find(
                  bundles[version].source_revision);
              timestamp != revision_timestamps.end()) {
            if (!timestamp->is_number_integer())
              throw std::runtime_error(
                  "revision timestamp must be an integer Unix timestamp");
            observation["timestamp"] = *timestamp;
          }
          observations.push_back(std::move(observation));
        }
        const auto rate = [&](std::size_t count) -> nlohmann::json {
          return observable == 0
                     ? nlohmann::json(nullptr)
                     : nlohmann::json(static_cast<double>(count) /
                                      static_cast<double>(observable));
        };
        const auto change_unit_count = [&](std::size_t count) -> nlohmann::json {
          return supplied_change_units ? nlohmann::json(count)
                                       : nlohmann::json(nullptr);
        };
        rows.push_back(
             {{"historical_element_id", "history-" + stable_hash(root)},
             {"current_name", item.current_name},
             {"current_kind", item.current_kind},
             {"current_path", item.current_path},
             {"present_in_latest_bundle",
              item.versions.contains(bundles.size() - 1)},
             {"observed_versions", item.versions.size()},
             {"observations", std::move(observations)},
             {"observable_transitions", observable},
             {"observed_change_units", change_unit_count(observable)},
             {"content_changed_transitions", item.changed.size()},
             {"change_units_with_content_changes",
              change_unit_count(item.changed.size())},
             {"content_change_rate", rate(item.changed.size())},
             {"interface_changed_transitions", item.interface_changed.size()},
             {"change_units_with_interface_changes",
              change_unit_count(item.interface_changed.size())},
             {"interface_change_rate", rate(item.interface_changed.size())},
             {"implementation_changed_transitions",
              item.implementation_changed.size()},
             {"change_units_with_implementation_changes",
              change_unit_count(item.implementation_changed.size())},
             {"implementation_change_rate",
              rate(item.implementation_changed.size())},
             {"dependency_changed_transitions", item.dependency_changed.size()},
             {"change_units_with_dependency_changes",
              change_unit_count(item.dependency_changed.size())},
             {"dependency_change_rate", rate(item.dependency_changed.size())},
             {"renamed_transitions", item.renamed.size()},
             {"moved_transitions", item.moved.size()},
             {"ambiguous_transitions", item.ambiguous.size()},
             {"unresolved_transitions", item.unresolved.size()},
             {"added_revision", item.added_revision},
             {"last_observed_revision", item.last_observed_revision},
             {"last_content_change_revision", item.last_changed_revision},
             {"change_events", item.events},
             {"evidence_status",
              item.ambiguous.empty() && item.unresolved.empty()
                  ? "complete"
                  : "partial"}});
      }
      Coverage coverage;
      coverage.capabilities = {
          "element_lineage",
          query == "file.history_stats" ? "file_history_statistics"
                                         : "element_history_statistics",
          supplied_change_units ? "change_unit_grouping"
                                : "revision_transition_grouping"};
      for (const auto &transition : transitions) {
        if (transition.coverage.status != "complete")
          coverage.status = "partial";
        for (const auto &gap : transition.coverage.gaps)
          coverage.gaps.push_back(transition.after_revision + ": " + gap);
      }
      const auto gap_count = coverage.gaps.size();
      constexpr std::size_t maximum_reported_gaps = 100;
      if (coverage.gaps.size() > maximum_reported_gaps) {
        coverage.gaps.resize(maximum_reported_gaps);
        coverage.gaps.push_back(std::to_string(gap_count - maximum_reported_gaps) +
                                " additional coverage gaps omitted");
      }
      auto coverage_json = nlohmann::json(coverage);
      coverage_json["gap_count"] = gap_count;
      coverage_json["gaps_truncated"] = gap_count > maximum_reported_gaps;
      nlohmann::json result = {
          {"bundle_count", bundles.size()},
          {"grouping_unit", supplied_change_units ? "change_unit"
                                                   : "revision_transition"},
          {"change_units", change_units}};
      if (query == "file.history_stats") {
        struct FileStats {
          std::set<std::size_t> versions, observed, changed, content_changed,
              interface_changed, implementation_changed, dependency_changed,
              renamed, moved, unresolved;
          std::set<std::string> elements, changed_elements;
          std::size_t current_elements{};
        };
        struct FileUnitEvent {
          std::set<std::string> elements;
          std::size_t content{}, interface{}, implementation{}, dependency{},
              renamed{}, moved{}, unresolved{};
        };
        std::map<std::string, FileStats> files;
        std::map<std::string, std::map<std::size_t, FileUnitEvent>> events;
        for (std::size_t version = 0; version < bundles.size(); ++version)
          for (const auto &element : bundles[version].elements)
            if (!element.location.path.empty()) {
              auto &file = files[element.location.path];
              file.versions.insert(version);
              file.elements.insert(find(find, element.compiler_id));
              if (version + 1 == bundles.size())
                ++file.current_elements;
            }
        for (std::size_t i = 0; i < transitions.size(); ++i)
          for (const auto &fact : transitions[i].facts) {
            std::set<std::string> paths;
            if (fact.before_location && !fact.before_location->path.empty())
              paths.insert(fact.before_location->path);
            if (fact.after_location && !fact.after_location->path.empty())
              paths.insert(fact.after_location->path);
            const auto id = !fact.before_element.empty() ? fact.before_element
                                                         : fact.after_element;
            const auto root = find(find, id);
            for (const auto &path : paths) {
              auto &file = files[path];
              auto &event = events[path][i];
              file.elements.insert(root);
              if (!fact.before_element.empty() && !fact.after_element.empty())
                file.observed.insert(i);
              const bool content = fact.content_change != "none" &&
                                   fact.content_change != "unverified";
              if (content) {
                file.content_changed.insert(i);
                file.changed_elements.insert(root);
                ++event.content;
              }
              if (fact.content_change == "interface" ||
                  fact.content_change == "both") {
                file.interface_changed.insert(i);
                ++event.interface;
              }
              if (fact.content_change == "implementation" ||
                  fact.content_change == "both") {
                file.implementation_changed.insert(i);
                ++event.implementation;
              }
              if (fact.dependencies_changed) {
                file.dependency_changed.insert(i);
                file.changed_elements.insert(root);
                ++event.dependency;
              }
              if (fact.continuity == "renamed" ||
                  fact.continuity == "moved_and_renamed") {
                file.renamed.insert(i);
                ++event.renamed;
              }
              if (fact.continuity == "moved" ||
                  fact.continuity == "moved_and_renamed") {
                file.moved.insert(i);
                ++event.moved;
              }
              const bool unresolved_fact =
                  fact.continuity == "added_or_unresolved" ||
                  fact.continuity == "deleted_or_unresolved";
              if (unresolved_fact) {
                file.unresolved.insert(i);
                ++event.unresolved;
              }
              const bool verified_change =
                  content || fact.dependencies_changed ||
                  (!fact.before_element.empty() &&
                   !fact.after_element.empty() && fact.continuity != "same");
              if (verified_change)
                file.changed.insert(i);
              if (verified_change || unresolved_fact)
                event.elements.insert(root);
            }
          }
        nlohmann::json file_rows = nlohmann::json::array();
        for (const auto &[path, file] : files) {
          nlohmann::json file_events = nlohmann::json::array();
          if (const auto found = events.find(path); found != events.end())
            for (const auto &[index, event] : found->second)
              if (file.changed.contains(index) || event.unresolved != 0)
                file_events.push_back(
                    {{"change_unit_id",
                      change_units.at(index).at("change_unit_id")},
                     {"unit_kind", change_units.at(index).at("unit_kind")},
                     {"association_status",
                      change_units.at(index).at("association_status")},
                     {"before_revision",
                      change_units.at(index).at("base_revision")},
                     {"after_revision",
                      change_units.at(index).at("result_revision")},
                     {"affected_elements", event.elements.size()},
                     {"content_changes", event.content},
                     {"interface_changes", event.interface},
                     {"implementation_changes", event.implementation},
                     {"dependency_changes", event.dependency},
                     {"renames", event.renamed},
                     {"moves", event.moved},
                     {"unresolved", event.unresolved}});
          const auto rate = [&](std::size_t count) -> nlohmann::json {
            return file.observed.empty()
                       ? nlohmann::json(nullptr)
                       : nlohmann::json(
                             static_cast<double>(count) /
                             static_cast<double>(file.observed.size()));
          };
          const auto change_unit_count =
              [&](std::size_t count) -> nlohmann::json {
            return supplied_change_units ? nlohmann::json(count)
                                         : nlohmann::json(nullptr);
          };
          file_rows.push_back(
              {{"path", path},
               {"observed_versions", file.versions.size()},
               {"observed_elements", file.elements.size()},
               {"current_elements", file.current_elements},
               {"observed_transitions", file.observed.size()},
               {"transitions_with_element_changes", file.changed.size()},
               {"transition_change_rate", rate(file.changed.size())},
               {"observed_change_units",
                change_unit_count(file.observed.size())},
               {"change_units_with_element_changes",
                change_unit_count(file.changed.size())},
               {"change_unit_change_rate",
                supplied_change_units ? rate(file.changed.size())
                                      : nlohmann::json(nullptr)},
               {"change_units_with_content_changes",
                change_unit_count(file.content_changed.size())},
               {"change_units_with_interface_changes",
                change_unit_count(file.interface_changed.size())},
               {"change_units_with_implementation_changes",
                change_unit_count(file.implementation_changed.size())},
               {"change_units_with_dependency_changes",
                change_unit_count(file.dependency_changed.size())},
               {"change_units_with_renames",
                change_unit_count(file.renamed.size())},
               {"change_units_with_moves", change_unit_count(file.moved.size())},
               {"unresolved_change_units",
                change_unit_count(file.unresolved.size())},
               {"changed_elements", file.changed_elements.size()},
               {"change_events", std::move(file_events)},
               {"evidence_status",
                file.unresolved.empty() ? "complete" : "partial"}});
        }
        result["files"] = std::move(file_rows);
      } else {
        result["elements"] = std::move(rows);
      }
      return {
          {"schema_version", kSchemaVersion},
          {"ok", true},
          {"coverage", std::move(coverage_json)},
          {"result", std::move(result)}};
    }
    if (query == "analysis.coverage") {
      const auto bundle = store_->load(path_parameter(params, "bundle"));
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"coverage", bundle.coverage},
              {"result", bundle.coverage}};
    }
    throw std::runtime_error("unknown query: " + query);
  } catch (const std::exception &error) {
    return {
        {"schema_version", kSchemaVersion},
        {"ok", false},
        {"error",
         {{"code", "invalid_request"},
          {"message", utf8_lossy(error.what())}}}};
  }
}
} // namespace history
