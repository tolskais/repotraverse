#include "history/query.hpp"

#include "history/build_import.hpp"
#include "history/catalog.hpp"
#include "history/file_history.hpp"
#include "history/git_coordination.hpp"
#include "history/history_plan.hpp"
#include "history/worker.hpp"
#include "history/telemetry.hpp"

#include <fstream>
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>

namespace history {
namespace {
const nlohmann::json &parameters(const nlohmann::json &request) {
  if (!request.contains("params") || !request.at("params").is_object())
    throw std::runtime_error("request.params must be an object");
  return request.at("params");
}
std::filesystem::path path_parameter(const nlohmann::json &params,
                                     const char *name) {
  if (!params.contains(name) || !params.at(name).is_string())
    throw std::runtime_error(std::string("missing path parameter: ") + name);
  return params.at(name).get<std::string>();
}
std::vector<LineageAssertion> assertions_from(const nlohmann::json &params) {
  if (!params.contains("assertions"))
    return {};
  std::ifstream input(path_parameter(params, "assertions"));
  if (!input)
    throw std::runtime_error("cannot open lineage assertions");
  nlohmann::json value;
  input >> value;
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
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot open evidence bundle: " + path.string());
  nlohmann::json value;
  input >> value;
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
      bundle.elements.push_back(std::move(snapshot));
    }
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
                           std::shared_ptr<BackgroundWorker> worker)
    : store_(std::move(store)), catalog_(std::move(catalog)),
      coordinator_(std::move(coordinator)), worker_(std::move(worker)) {
  if (!store_)
    throw std::invalid_argument("FactStore cannot be null");
  if (!catalog_ || !coordinator_)
    throw std::invalid_argument(
        "federated query service requires catalog and coordinator");
}

nlohmann::json QueryService::submit(const nlohmann::json &request) const {
  if (!catalog_)
    return execute(request);
  const auto request_id = catalog_->create_request(request);
  if (const auto existing = catalog_->request_job(request_id);
      existing && (existing->value("state", std::string{}) == "complete" ||
                   existing->value("state", std::string{}) == "cancelled"))
  {
    auto response = *existing;
    response["schema_version"] = kSchemaVersion;
    response["ok"] = response.value("state", std::string{}) != "failed";
    return response;
  }
  catalog_->update_request(request_id, "running", {{"phase", "planning"}});
  const auto started = std::chrono::steady_clock::now();
  const auto response = execute(request);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  Telemetry::instance().increment("requests.submitted");
  Telemetry::instance().gauge("requests.last_latency_ms", elapsed);
  const auto state = !response.value("ok", false)
                         ? "failed"
                         : response.value("result_status", std::string{}) ==
                                   "complete"
                               ? "complete"
                               : "partial";
  Telemetry::instance().span(
      "request.plan", elapsed,
      {{"request_id", request_id},
       {"query", request.value("query", std::string{})},
       {"state", state}});
  const auto error = response.value("error", nlohmann::json::object());
  catalog_->update_request(
      request_id, state,
      {{"pending_work",
        response.value("pending_work", nlohmann::json::array()).size()},
       {"snapshot_id", response.value("snapshot_id", std::string{})}},
      response, error);
  auto job = *catalog_->request_job(request_id);
  job["schema_version"] = kSchemaVersion;
  job["ok"] = state != std::string{"failed"};
  return job;
}

nlohmann::json QueryService::request_status(const std::string &request_id,
                                            bool refresh) const {
  if (!catalog_)
    return {{"ok", false}, {"error", {{"code", "jobs_unavailable"}}}};
  const auto job = catalog_->request_job(request_id);
  if (!job)
    return {{"ok", false}, {"error", {{"code", "not_found"}}}};
  const auto state = job->value("state", std::string{});
  if (refresh && state != "complete" && state != "failed" &&
      state != "cancelled")
    return submit(job->at("request"));
  auto response = *job;
  response["schema_version"] = kSchemaVersion;
  response["ok"] = state != "failed";
  return response;
}

nlohmann::json QueryService::cancel_request(
    const std::string &request_id) const {
  if (!catalog_ || !catalog_->request_job(request_id))
    return {{"ok", false}, {"error", {{"code", "not_found"}}}};
  catalog_->cancel_request_tasks(request_id);
  catalog_->update_request(request_id, "cancelled", {{"phase", "cancelled"}});
  auto response = *catalog_->request_job(request_id);
  response["schema_version"] = kSchemaVersion;
  response["ok"] = true;
  return response;
}

nlohmann::json QueryService::execute(const nlohmann::json &request) const {
  try {
    if (request.value("schema_version", 0U) != kSchemaVersion)
      throw std::runtime_error("unsupported request schema");
    const auto &params = parameters(request);
    const auto query = request.value("query", std::string{});
    if (query == "work.run") {
      if (!worker_)
        return {{"schema_version", kSchemaVersion},
                {"ok", true},
                {"result_status", "partial"},
                {"result", {{"state", "worker_unavailable"}}}};
      const auto result = worker_->run_once();
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"result_status", result.value("state", std::string{}) == "idle"
                                    ? "complete"
                                    : "partial"},
              {"snapshot_id", catalog_->snapshot_id()},
              {"result", result}};
    }
    if (query == "work.retry") {
      if (!catalog_)
        throw std::runtime_error("work.retry requires federated service");
      const auto task_id = params.value("task_id", std::string{});
      if (task_id.empty())
        throw std::runtime_error("work.retry requires task_id");
      catalog_->retry_task(task_id);
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"result_status", "complete"},
              {"result", {{"task_id", task_id}, {"state", "pending"}}}};
    }
    if (query == "build.import") {
      if (!catalog_)
        throw std::runtime_error("build.import requires federated service");
      const auto result = import_build_log(
          *catalog_, path_parameter(params, "input"),
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
    if (query == "work.acquire") {
      if (!coordinator_)
        throw std::runtime_error("work.acquire requires federated service");
      if (!params.contains("task"))
        throw std::runtime_error("work.acquire requires task");
      const auto result = coordinator_->acquire(params.at("task"));
      const auto state = result.value("state", std::string{});
      return {
          {"schema_version", kSchemaVersion},
          {"ok", true},
          {"result_status", state == "processing" || state == "processing_local"
                                ? "complete"
                                : "partial"},
          {"snapshot_id", catalog_->snapshot_id()},
          {"result", result}};
    }
    if (query == "work.heartbeat" || query == "work.complete" ||
        query == "work.status") {
      if (!coordinator_)
        throw std::runtime_error(query + " requires federated service");
      const auto task_id = params.value("task_id", std::string{});
      if (task_id.empty())
        throw std::runtime_error(query + " requires task_id");
      nlohmann::json result;
      if (query == "work.heartbeat")
        result = coordinator_->heartbeat(task_id);
      else if (query == "work.complete") {
        const auto completed = params.value("result", nlohmann::json{});
        const auto type = completed.value("record_type", std::string{});
        if (type != "tu_manifest" && type != "tu_failure")
          throw std::runtime_error(
              "work.complete result must be a tu_manifest or tu_failure");
        result = coordinator_->complete(task_id, completed);
      } else {
        coordinator_->sync();
        result = coordinator_->status(task_id);
        if (const auto fact = catalog_->fact_for_task(task_id)) {
          result = *fact;
          result["state"] = "completed";
        }
      }
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"result_status",
               result.value("state", std::string{}) == "completed" ? "complete"
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
          "same", "renamed", "moved", "moved_and_renamed", "extract",
          "inline", "split", "merge"};
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
        throw std::runtime_error("submodule mapping requires federated service");
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
        throw std::runtime_error("submodule mapping requires federated service");
      const auto parent = params.value("parent_repository_id", std::string{});
      const auto revision = params.value("parent_revision", std::string{});
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"result_status", "complete"},
              {"result", catalog_->submodule_revisions(parent, revision)}};
    }
    if (query == "semantic.dependents") {
      if (!catalog_)
        throw std::runtime_error("semantic dependencies require federated service");
      const auto repository_id =
          params.value("repository_id", std::string{});
      const auto revision = params.value("revision", std::string{});
      if (!params.contains("element_ids") ||
          !params.at("element_ids").is_array())
        throw std::runtime_error("semantic.dependents requires element_ids");
      const auto maximum =
          std::clamp<std::size_t>(params.value("maximum", std::size_t{10000}),
                                  1, 100000);
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
                {relation->relation_id,
                 relation->source_element_ids.front(),
                 relation->target_element_ids.front(),
                 "same_element", "accepted", relation->author,
                 relation->reviewer});
        }
      }
      auto result = trace_transition(before, after, assertions);
      result.reviewed_relations = std::move(reviewed);
      return {{"schema_version", kSchemaVersion},
              {"ok", true},
              {"coverage", result.coverage},
              {"result", result}};
    }
    if (query == "element.history_stats") {
      if (!params.contains("bundles") || !params.at("bundles").is_array() ||
          params.at("bundles").size() < 2)
        throw std::runtime_error(
            "element.history_stats requires ordered bundles");
      std::vector<EvidenceBundle> bundles;
      for (const auto &path : params.at("bundles"))
        bundles.push_back(store_->load(path.get<std::string>()));
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
        transitions.push_back(
            trace_transition(bundles[i], bundles[i + 1], assertions));
        for (const auto &fact : transitions.back().facts)
          if (!fact.before_element.empty() && !fact.after_element.empty())
            unite(fact.before_element, fact.after_element);
      }
      struct Stats {
        std::set<std::size_t> versions, changed, interface_changed,
            implementation_changed, renamed, moved, ambiguous;
        std::string current_name, last_changed_revision;
      };
      std::map<std::string, Stats> stats;
      for (std::size_t version = 0; version < bundles.size(); ++version)
        for (const auto &element : bundles[version].elements) {
          auto &item = stats[find(find, element.compiler_id)];
          item.versions.insert(version);
          item.current_name = element.qualified_name;
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
          if (fact.continuity == "renamed" ||
              fact.continuity == "moved_and_renamed")
            item.renamed.insert(i);
          if (fact.continuity == "moved" ||
              fact.continuity == "moved_and_renamed")
            item.moved.insert(i);
          if (fact.confidence == "ambiguous")
            item.ambiguous.insert(i);
        }
      nlohmann::json rows = nlohmann::json::array();
      for (const auto &[root, item] : stats)
        rows.push_back(
            {{"historical_element_id", "history-" + stable_hash(root)},
             {"current_name", item.current_name},
             {"observed_versions", item.versions.size()},
             {"observable_transitions",
              item.versions.size() > 0 ? item.versions.size() - 1 : 0},
             {"content_changed_transitions", item.changed.size()},
             {"interface_changed_transitions", item.interface_changed.size()},
             {"implementation_changed_transitions",
              item.implementation_changed.size()},
             {"renamed_transitions", item.renamed.size()},
             {"moved_transitions", item.moved.size()},
             {"ambiguous_transitions", item.ambiguous.size()},
             {"last_content_change_revision", item.last_changed_revision}});
      Coverage coverage;
      coverage.capabilities = {"element_lineage", "history_statistics"};
      for (const auto &transition : transitions) {
        if (transition.coverage.status != "complete")
          coverage.status = "partial";
        for (const auto &gap : transition.coverage.gaps)
          coverage.gaps.push_back(transition.after_revision + ": " + gap);
      }
      return {
          {"schema_version", kSchemaVersion},
          {"ok", true},
          {"coverage", coverage},
          {"result",
           {{"bundle_count", bundles.size()}, {"elements", std::move(rows)}}}};
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
        {"error", {{"code", "invalid_request"}, {"message", error.what()}}}};
  }
}
} // namespace history
