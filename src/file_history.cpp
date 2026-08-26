#include "history/file_history.hpp"
#include "history/encoding.hpp"
#include "history/history_plan.hpp"
#include "history/ir.hpp"
#include "history/process.hpp"
#include "history/revision_workspace.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
namespace history {
namespace {
std::int64_t prior_year(std::int64_t timestamp) {
  using namespace std::chrono;
  const sys_seconds point{seconds{timestamp}};
  const auto day = floor<days>(point);
  const year_month_day current{day};
  const auto year = current.year() - years{1};
  const auto month = current.month();
  auto wanted = current.day();
  const year_month_day_last last{year, month_day_last{month}};
  if (wanted > last.day())
    wanted = last.day();
  const sys_days result = year_month_day{year, month, wanted};
  return duration_cast<seconds>(result.time_since_epoch() + (point - day))
      .count();
}
bool is_rename(const nlohmann::json &change) {
  const auto status = change.value("status", std::string{});
  return !status.empty() && status.front() == 'R';
}

bool is_header_path(const std::string &path) {
  auto extension = path_to_utf8(path_from_utf8(path).extension());
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  static const std::set<std::string> headers = {
      ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp", ".tpp"};
  return headers.contains(extension);
}

std::string observation_family(const CompileContext &context) {
  return context.translation_unit + "\n" + context.build_variant.product + "\n" +
         context.build_variant.target + "\n" + context.build_variant.configuration;
}

bool overlaps_change(const SourceAnchor &location, const nlohmann::json &ranges,
                     bool after) {
  const auto start_key = after ? "after_start" : "before_start";
  const auto count_key = after ? "after_count" : "before_count";
  for (const auto &range : ranges) {
    const auto start = range.value(start_key, std::uint64_t{});
    const auto count = range.value(count_key, std::uint64_t{});
    const auto end = count == 0 ? start : start + count - 1;
    if (location.begin_line <= end && location.end_line >= start) return true;
  }
  return false;
}

nlohmann::json direct_origin(const nlohmann::json &unit,
                             const SourceAnchor *before,
                             const SourceAnchor *after) {
  const auto ranges = unit.value("changed_ranges", nlohmann::json::array());
  if ((!before || !overlaps_change(*before, ranges, false)) &&
      (!after || !overlaps_change(*after, ranges, true)))
    return nlohmann::json::array();
  OriginEvidence origin;
  origin.kind = "direct_git_change";
  origin.evidence.push_back(
      {"integration_unit", unit.value("integration_unit_id", std::string{}),
       unit.value("head_commit", std::string{}), {}});
  origin.causes = {"element source range overlaps the integration-unit diff"};
  origin.confidence = "exact";
  origin.coverage.capabilities = {"git_diff", "source_anchor"};
  return nlohmann::json::array({origin});
}

struct ElementView {
  LogicalElement element;
  SourceAnchor location;
  std::map<std::string, SemanticVariant> variants;
  std::map<std::string, std::set<std::string>> variant_configurations;
  std::set<std::string> context_ids;
  std::set<std::string> translation_units;
};

using EndpointView = std::map<std::string, ElementView>;

std::string endpoint_key(const std::string &revision, const std::string &path) {
  return revision + "\n" + path;
}

nlohmann::json changed_ranges(const std::filesystem::path &repository,
                              const std::string &before,
                              const std::string &after,
                              const std::string &before_path,
                              const std::string &after_path) {
  std::vector<std::string> command = {"git", "-C", path_to_utf8(repository)};
  if (before.empty()) {
    command.insert(command.end(), {"diff-tree", "--root", "--no-commit-id",
                                   "-p", "--unified=0", after, "--"});
  } else {
    command.insert(command.end(),
                   {"diff", "--no-color", "--unified=0", before, after, "--"});
  }
  if (!before_path.empty())
    command.push_back(before_path);
  if (!after_path.empty() && after_path != before_path)
    command.push_back(after_path);
  const auto result = run_process(command);
  if (result.exit_code != 0)
    throw std::runtime_error("cannot locate changed ranges: " + result.error);
  static const std::regex hunk(
      R"(^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@)");
  nlohmann::json ranges = nlohmann::json::array();
  std::size_t begin = 0;
  while (begin < result.output.size()) {
    const auto end = result.output.find('\n', begin);
    const auto line = result.output.substr(
        begin, end == std::string::npos ? end : end - begin);
    std::smatch match;
    if (std::regex_search(line, match, hunk))
      ranges.push_back(
          {{"before_start", std::stoull(match[1].str())},
           {"before_count",
            match[2].matched ? std::stoull(match[2].str()) : 1ULL},
           {"after_start", std::stoull(match[3].str())},
           {"after_count",
            match[4].matched ? std::stoull(match[4].str()) : 1ULL}});
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return ranges;
}

std::set<std::string> strings(const nlohmann::json &value) {
  std::set<std::string> result;
  if (value.is_array())
    for (const auto &item : value)
      result.insert(item.get<std::string>());
  return result;
}

template <typename Member>
std::set<std::string> fingerprints(const ElementView &view, Member member) {
  std::set<std::string> result;
  for (const auto &[id, variant] : view.variants) {
    (void)id;
    result.insert(variant.*member);
  }
  return result;
}

std::string semantic_shape(const ElementView &view) {
  std::string result = view.element.kind + "\n";
  std::set<std::string> shapes;
  for (const auto &[id, variant] : view.variants) {
    (void)id;
    shapes.insert(variant.interface_fingerprint + "\n" +
                  variant.implementation_fingerprint + "\n" +
                  variant.dependency_fingerprint);
  }
  for (const auto &shape : shapes)
    result += shape + "\n";
  return result;
}

nlohmann::json aggregate_analysis(Catalog &catalog,
                                  const std::string &request_id,
                                  const nlohmann::json &page,
                                  const std::string &current_revision,
                                  const std::string &current_path,
                                  nlohmann::json &gaps) {
  std::set<std::string> wanted;
  wanted.insert(endpoint_key(current_revision, current_path));
  for (const auto &unit : page) {
    if (unit.value("file_exists_before", true))
      wanted.insert(
          endpoint_key(unit.value("base_commit", std::string{}),
                       unit.value("file_path_before", std::string{})));
    if (unit.value("file_exists_after", true))
      wanted.insert(endpoint_key(unit.value("head_commit", std::string{}),
                                 unit.value("file_path_after", std::string{})));
  }
  std::map<std::string, EndpointView> endpoints;
  std::set<std::string> completed_endpoints, unavailable_endpoints;
  for (const auto &task : catalog.pending_tasks(request_id)) {
    unavailable_endpoints.insert(
        endpoint_key(task.value("source_commit", std::string{}),
                     task.value("requested_file", std::string{})));
    const auto state = task.value("state", std::string{});
    if (state == "quarantined" || state == "incompatible_worker")
      gaps.push_back({{"kind", "task_" + state},
                      {"task_id", task.value("task_id", std::string{})},
                      {"revision", task.value("source_commit", std::string{})},
                      {"path", task.value("requested_file", std::string{})}});
  }
  for (const auto &row : catalog.results_for_request(request_id)) {
    const auto &task = row.at("task");
    const auto &fact = row.at("fact");
    const auto result = fact.value("result", nlohmann::json{});
    const auto revision = task.value("source_commit", std::string{});
    const auto path = task.value("requested_file", std::string{});
    const auto key = endpoint_key(revision, path);
    if (!wanted.contains(key))
      continue;
    if (result.value("record_type", std::string{}) == "tu_failure") {
      unavailable_endpoints.insert(key);
      gaps.push_back(
          {{"kind", "tu_extraction_failed"},
           {"revision", revision},
           {"path", path},
           {"context_id", task.value("context_id", std::string{})},
           {"coverage", result.value("coverage", nlohmann::json{})}});
      continue;
    }
    if (result.value("record_type", std::string{}) != "tu_manifest")
      continue;
    const auto manifest = result.get<TuManifest>();
    completed_endpoints.insert(key);
    if (manifest.coverage.status != "complete") {
      unavailable_endpoints.insert(key);
      gaps.push_back({{"kind", "tu_manifest_partial"},
                      {"manifest_id", manifest.manifest_id},
                      {"revision", revision},
                      {"path", path},
                      {"coverage", manifest.coverage}});
    }
    std::map<std::string, LogicalElement> elements;
    std::map<std::string, SemanticVariant> variants;
    for (const auto &element : manifest.elements)
      elements[element.element_id] = element;
    for (const auto &variant : manifest.variants)
      variants[variant.variant_id] = variant;
    const auto configurations =
        strings(task.value("configurations", nlohmann::json::array()));
    for (const auto &observation : manifest.observations) {
      if (observation.location.path != path)
        continue;
      const auto element = elements.find(observation.element_id);
      const auto variant = variants.find(observation.variant_id);
      if (element == elements.end() || variant == variants.end()) {
        unavailable_endpoints.insert(key);
        gaps.push_back({{"kind", "invalid_tu_manifest"},
                        {"manifest_id", manifest.manifest_id}});
        continue;
      }
      auto &view = endpoints[key][observation.element_id];
      view.element = element->second;
      view.location = observation.location;
      view.variants[observation.variant_id] = variant->second;
      auto &variant_configs =
          view.variant_configurations[observation.variant_id];
      variant_configs.insert(configurations.begin(), configurations.end());
      view.context_ids.insert(manifest.context_id);
      view.translation_units.insert(manifest.translation_unit);
    }
  }

  nlohmann::json snapshots = nlohmann::json::array();
  for (const auto &[key, elements] : endpoints) {
    const auto separator = key.find('\n');
    const auto revision = key.substr(0, separator);
    const auto path = key.substr(separator + 1);
    for (const auto &[element_id, view] : elements) {
      (void)element_id;
      nlohmann::json variants = nlohmann::json::array();
      for (const auto &[variant_id, variant] : view.variants)
        variants.push_back(
            {{"variant", variant},
             {"configurations", view.variant_configurations.at(variant_id)}});
      snapshots.push_back({{"revision", revision},
                           {"path", path},
                           {"element", view.element},
                           {"location", view.location},
                           {"variants", variants},
                           {"context_ids", view.context_ids},
                           {"translation_units", view.translation_units}});
    }
  }

  nlohmann::json transitions = nlohmann::json::array();
  for (const auto &unit : page) {
    const auto before_key =
        endpoint_key(unit.value("base_commit", std::string{}),
                     unit.value("file_path_before", std::string{}));
    const auto after_key =
        endpoint_key(unit.value("head_commit", std::string{}),
                     unit.value("file_path_after", std::string{}));
    const EndpointView empty;
    const auto before_it = endpoints.find(before_key);
    const auto after_it = endpoints.find(after_key);
    const auto &before =
        before_it == endpoints.end() ? empty : before_it->second;
    const auto &after = after_it == endpoints.end() ? empty : after_it->second;
    nlohmann::json changes = nlohmann::json::array();
    const bool before_ready = !unit.value("file_exists_before", true) ||
                              (completed_endpoints.contains(before_key) &&
                               !unavailable_endpoints.contains(before_key));
    const bool after_ready = !unit.value("file_exists_after", true) ||
                             (completed_endpoints.contains(after_key) &&
                              !unavailable_endpoints.contains(after_key));
    if (!before_ready || !after_ready) {
      transitions.push_back(
          {{"integration_unit_id", unit.value("integration_unit_id", std::string{})},
           {"change_unit_id", unit.value("change_unit_id", std::string{})},
           {"before_revision", unit.value("base_commit", std::string{})},
           {"after_revision", unit.value("head_commit", std::string{})},
           {"state", "pending"},
           {"element_changes", nlohmann::json::array()},
           {"lineage_candidates", nlohmann::json::array()}});
      continue;
    }
    std::set<std::string> unmatched_before, unmatched_after;
    for (const auto &[id, old_view] : before) {
      const auto found = after.find(id);
      if (found == after.end()) {
        unmatched_before.insert(id);
        continue;
      }
      const auto &new_view = found->second;
      nlohmann::json dimensions = nlohmann::json::array();
      if (fingerprints(old_view, &SemanticVariant::interface_fingerprint) !=
          fingerprints(new_view, &SemanticVariant::interface_fingerprint))
        dimensions.push_back("interface");
      if (fingerprints(old_view,
                       &SemanticVariant::implementation_fingerprint) !=
          fingerprints(new_view, &SemanticVariant::implementation_fingerprint))
        dimensions.push_back("implementation");
      if (fingerprints(old_view, &SemanticVariant::dependency_fingerprint) !=
          fingerprints(new_view, &SemanticVariant::dependency_fingerprint))
        dimensions.push_back("dependencies");
      changes.push_back(
          {{"element_id", id},
           {"change_kind", dimensions.empty() ? "unchanged" : "modified"},
           {"semantic_dimensions", dimensions},
           {"location_changed",
            old_view.location.path != new_view.location.path ||
                old_view.location.begin_line != new_view.location.begin_line},
           {"before_location", old_view.location},
           {"after_location", new_view.location},
           {"origin_evidence", direct_origin(unit, &old_view.location,
                                              &new_view.location)}});
    }
    for (const auto &[id, view] : after)
      if (!before.contains(id))
        unmatched_after.insert(id);
    nlohmann::json candidates = nlohmann::json::array();
    std::set<std::string> candidate_before, candidate_after;
    for (const auto &old_id : unmatched_before) {
      std::vector<std::string> matches;
      for (const auto &new_id : unmatched_after)
        if (semantic_shape(before.at(old_id)) ==
            semantic_shape(after.at(new_id)))
          matches.push_back(new_id);
      if (matches.size() == 1) {
        const auto &new_id = matches.front();
        std::size_t reverse_matches = 0;
        for (const auto &other_old : unmatched_before)
          if (semantic_shape(before.at(other_old)) ==
              semantic_shape(after.at(new_id)))
            ++reverse_matches;
        if (reverse_matches == 1) {
          candidate_before.insert(old_id);
          candidate_after.insert(new_id);
          candidates.push_back(
              {{"before_element", old_id},
               {"after_element", new_id},
               {"proposed_continuity", "renamed_or_reidentified"},
               {"confidence", "high"},
               {"match_basis", "unique_semantic_shape"},
               {"requires_review", true}});
        }
      }
    }
    for (const auto &id : unmatched_before)
      changes.push_back(
          {{"element_id", id},
           {"change_kind",
            candidate_before.contains(id) ? "lineage_unresolved" : "removed"},
           {"before_location", before.at(id).location},
           {"origin_evidence", direct_origin(unit, &before.at(id).location,
                                              nullptr)}});
    for (const auto &id : unmatched_after)
      changes.push_back(
          {{"element_id", id},
           {"change_kind",
            candidate_after.contains(id) ? "lineage_unresolved" : "added"},
           {"after_location", after.at(id).location},
           {"origin_evidence", direct_origin(unit, nullptr,
                                              &after.at(id).location)}});
    for (auto &change : changes) {
      change["logical_element_id"] = change.value("element_id", std::string{});
      change["transition_id"] = stable_hash(
          unit.value("integration_unit_id", std::string{}) + "\n" +
          change.value("element_id", std::string{}) + "\n" +
          unit.value("base_commit", std::string{}) + "\n" +
          unit.value("head_commit", std::string{}));
    }
    transitions.push_back(
        {{"integration_unit_id", unit.value("integration_unit_id", std::string{})},
         {"change_unit_id", unit.value("change_unit_id", std::string{})},
         {"before_revision", unit.value("base_commit", std::string{})},
         {"after_revision", unit.value("head_commit", std::string{})},
         {"state", "complete"},
         {"element_changes", changes},
         {"lineage_candidates", candidates}});
  }
  return {{"element_snapshots", snapshots}, {"transitions", transitions}};
}
} // namespace

nlohmann::json plan_file_history(Catalog &catalog,
                                 const FileHistoryOptions &options) {
  if (options.repository.empty() || options.path.empty())
    throw std::runtime_error("file.history requires repository and path");
  if (options.scope != "direct" && options.scope != "semantic")
    throw std::runtime_error("file.history scope must be direct or semantic");
  const auto plan =
      catalog.root() /
      ("history-" +
       stable_hash(path_to_utf8(std::filesystem::absolute(options.repository)) +
                   "\n" + options.ref) +
       ".jsonl");
  write_history_plan(
      {options.repository, options.ref, {}, options.pr_facts, plan,
       options.repository_id});
  std::ifstream input(plan);
  std::string line;
  std::vector<nlohmann::json> units;
  std::int64_t head_time = 0;
  while (std::getline(input, line)) {
    if (line.empty())
      continue;
    auto value = nlohmann::json::parse(line);
    if (value.value("record_type", std::string{}) == "integration_unit") {
      head_time =
          std::max(head_time, value.value("committer_time", std::int64_t{}));
      units.push_back(std::move(value));
    }
  }
  input.close();
  {
    std::error_code ignored;
    std::filesystem::remove(plan, ignored);
  }
  const auto since = options.since ? options.since : prior_year(head_time);
  std::string tracked = options.path;
  std::vector<nlohmann::json> selected, path_segments;
  for (auto it = units.rbegin(); it != units.rend(); ++it) {
    bool touched = false;
    const auto path_after = tracked;
    auto path_before = tracked;
    bool exists_before = true, exists_after = true;
    for (const auto &change : it->at("changes")) {
      const auto path = change.value("path", std::string{});
      const auto old = change.value("old_path", std::string{});
      if (path == tracked || old == tracked)
        touched = true;
      const auto status = change.value("status", std::string{});
      if (path == tracked && status == "A")
        exists_before = false;
      if (path == tracked && status == "D")
        exists_after = false;
      if (is_rename(change) && path == tracked) {
        path_segments.push_back(
            {{"from", old},
             {"to", path},
             {"at_commit", change.value("at_commit", std::string{})}});
        path_before = old;
      }
    }
    if (touched && it->value("committer_time", std::int64_t{}) >= since) {
      auto unit = *it;
      unit["file_path_before"] = path_before;
      unit["file_path_after"] = path_after;
      unit["file_exists_before"] = exists_before;
      unit["file_exists_after"] = exists_after;
      unit["changed_ranges"] = changed_ranges(
          options.repository, unit.value("base_commit", std::string{}),
          unit.value("head_commit", std::string{}), path_before, path_after);
      selected.push_back(std::move(unit));
    }
    tracked = path_before;
  }
  std::reverse(selected.begin(), selected.end());
  std::reverse(path_segments.begin(), path_segments.end());
  const auto request_id = stable_hash(
      path_to_utf8(std::filesystem::absolute(options.repository)) + "\n" +
      options.ref + "\n" + options.path + "\n" + options.scope + "\n" +
      std::to_string(since) + "\n" + options.extractor_identity);
  std::size_t scheduled = 0, complete = 0;
  nlohmann::json gaps = nlohmann::json::array();
  bool semantic_map_missing = false;
  std::set<std::string> seen_tasks, missing_contexts;
  const auto schedule_endpoint = [&](const std::string &revision,
                                     const std::string &endpoint_path,
                                     const std::set<std::string> &additional_tus) {
    if (revision.empty() || endpoint_path.empty())
      return;
    auto contexts = catalog.compile_contexts(endpoint_path, revision);
    std::set<std::string> context_keys;
    for (const auto &context : contexts)
      context_keys.insert(context.context_id + "\n" + context.configuration +
                          "\n" + context.translation_unit);
    for (const auto &tu : additional_tus)
      for (const auto &context : catalog.compile_contexts(tu, revision))
        if (context_keys.insert(context.context_id + "\n" + context.configuration +
                                "\n" + context.translation_unit).second)
          contexts.push_back(context);
    if (contexts.empty()) {
      const auto key = endpoint_key(revision, endpoint_path);
      if (missing_contexts.insert(key).second)
        gaps.push_back({{"kind", "build_context_required"},
                        {"revision", revision},
                        {"path", endpoint_path}});
      return;
    }
    if (options.scope == "semantic") {
          const auto mapped = std::any_of(
          contexts.begin(), contexts.end(), [](const CompileContext &context) {
            return context.dependency_map_complete;
          });
      semantic_map_missing = semantic_map_missing || !mapped;
    }
    std::map<std::string, std::pair<CompileContext, std::vector<std::string>>>
        groups;
    for (const auto &context : contexts) {
      auto &group =
          groups[context.context_id + "\n" + context.build_variant.variant_id];
      group.first = context;
      group.second.push_back(context.configuration);
    }
    for (const auto &[group_id, group] : groups) {
      (void)group_id;
      const auto &context = group.first;
      const auto &context_id = context.context_id;
      nlohmann::json identity = {
          {"task_type", "tu.extract"},
          {"repository_id", options.repository_id},
          {"source_commit", revision},
          {"translation_unit", context.translation_unit},
          {"context_id", context_id},
          {"build_variant", context.build_variant},
          {"inventory_id", context.inventory_id},
          {"toolchain_fingerprint", context.toolchain_fingerprint},
          {"generated_inputs_fingerprint",
           context.generated_inputs_fingerprint},
          {"extractor_identity", options.extractor_identity},
          {"extractor_schema", kSchemaVersion}};
      const auto id = stable_hash(identity.dump());
      if (!seen_tasks.insert(id).second)
        continue;
      nlohmann::json task = {
          {"task_type", "tu.extract"},
          {"repository_id", options.repository_id},
          {"request_id", request_id},
          {"identity", identity},
          {"repository",
           path_to_utf8(std::filesystem::absolute(options.repository))},
          {"source_commit", revision},
          {"translation_unit", context.translation_unit},
          {"requested_file", endpoint_path},
          {"context_id", context_id},
          {"inventory_id", context.inventory_id},
          {"configurations", group.second},
          {"build_variant", context.build_variant},
          {"toolchain_fingerprint", context.toolchain_fingerprint},
          {"generated_inputs_fingerprint",
           context.generated_inputs_fingerprint},
          {"frontend_arguments", context.frontend_arguments},
          {"materialization",
           MaterializationManifest{
               context.project_files,
               context.dependency_map_complete,
               context.project_files.empty()
                   ? std::vector<std::string>{"project dependency closure is "
                                              "unavailable"}
                   : std::vector<std::string>{}}},
          {"extractor_schema", kSchemaVersion}};
      if (catalog.fact_for_task(id))
        ++complete;
      else {
        catalog.schedule_task(id, task);
        ++scheduled;
      }
    }
  };
  for (const auto &unit : selected) {
    const auto before_revision = unit.value("base_commit", std::string{});
    const auto after_revision = unit.value("head_commit", std::string{});
    const auto before_path = unit.value("file_path_before", options.path);
    const auto after_path = unit.value("file_path_after", options.path);
    std::set<std::string> before_tus, after_tus;
    if (is_header_path(options.path)) {
      for (const auto &context : catalog.compile_contexts(before_path, before_revision))
        before_tus.insert(context.translation_unit);
      for (const auto &context : catalog.compile_contexts(after_path, after_revision))
        after_tus.insert(context.translation_unit);
    }
    if (unit.value("file_exists_before", true))
      schedule_endpoint(before_revision, before_path, after_tus);
    if (unit.value("file_exists_after", true))
      schedule_endpoint(after_revision, after_path, before_tus);
  }
  const auto resolved = run_process(
      {"git", "-C", path_to_utf8(options.repository), "rev-parse", options.ref});
  if (resolved.exit_code != 0)
    throw std::runtime_error("cannot resolve file.history ref: " +
                             resolved.error);
  auto current_revision = resolved.output;
  while (!current_revision.empty() &&
         (current_revision.back() == '\n' || current_revision.back() == '\r'))
    current_revision.pop_back();
  schedule_endpoint(current_revision, options.path, {});
  nlohmann::json header_observation_matrix = nlohmann::json::array();
  if (is_header_path(options.path)) {
    for (const auto &unit : selected) {
      const auto before_revision = unit.value("base_commit", std::string{});
      const auto after_revision = unit.value("head_commit", std::string{});
      const auto before_path = unit.value("file_path_before", options.path);
      const auto after_path = unit.value("file_path_after", options.path);
      const auto before_contexts = unit.value("file_exists_before", true)
                                       ? catalog.compile_contexts(before_path, before_revision)
                                       : std::vector<CompileContext>{};
      const auto after_contexts = unit.value("file_exists_after", true)
                                      ? catalog.compile_contexts(after_path, after_revision)
                                      : std::vector<CompileContext>{};
      std::map<std::string, CompileContext> before_by_family, after_by_family;
      for (const auto &context : before_contexts)
        before_by_family[observation_family(context)] = context;
      for (const auto &context : after_contexts)
        after_by_family[observation_family(context)] = context;
      std::set<std::string> families;
      for (const auto &[family, ignored] : before_by_family) {
        (void)ignored;
        families.insert(family);
      }
      for (const auto &[family, ignored] : after_by_family) {
        (void)ignored;
        families.insert(family);
      }
      for (const auto &family : families) {
        if (!before_by_family.contains(family) && after_by_family.contains(family))
          for (const auto &context : catalog.compile_contexts(
                   after_by_family.at(family).translation_unit, before_revision))
            if (observation_family(context) == family) {
              before_by_family[family] = context;
              break;
            }
        if (!after_by_family.contains(family) && before_by_family.contains(family))
          for (const auto &context : catalog.compile_contexts(
                   before_by_family.at(family).translation_unit, after_revision))
            if (observation_family(context) == family) {
              after_by_family[family] = context;
              break;
            }
      }
      for (const auto &family : families) {
        const auto before = before_by_family.find(family);
        const auto after = after_by_family.find(family);
        const auto complete_context = [](const auto &found, const auto &end) {
          return found != end && found->second.inventory_complete &&
                 found->second.dependency_map_complete;
        };
        const bool before_complete = complete_context(before, before_by_family.end());
        const bool after_complete = complete_context(after, after_by_family.end());
        header_observation_matrix.push_back(
            {{"integration_unit_id", unit.value("integration_unit_id", std::string{})},
             {"context_family", stable_hash(family)},
             {"translation_unit", before != before_by_family.end()
                                      ? before->second.translation_unit
                                      : after->second.translation_unit},
             {"before", {{"revision", before_revision},
                         {"context_id", before != before_by_family.end()
                                            ? before->second.context_id : std::string{}},
                         {"status", before_complete ? "expected" : "missing"}}},
             {"after", {{"revision", after_revision},
                        {"context_id", after != after_by_family.end()
                                           ? after->second.context_id : std::string{}},
                        {"status", after_complete ? "expected" : "missing"}}}});
        if ((!before_revision.empty() && unit.value("file_exists_before", true) && !before_complete) ||
            (unit.value("file_exists_after", true) && !after_complete))
          gaps.push_back({{"kind", "header_observation_matrix_incomplete"},
                          {"integration_unit_id", unit.value("integration_unit_id", std::string{})},
                          {"context_family", stable_hash(family)}});
      }
    }
  }
  if (options.scope == "semantic" && semantic_map_missing)
    gaps.push_back(
        {{"kind", "semantic_dependency_map_incomplete"},
         {"message",
          "one or more build contexts lack captured project dependencies"}});
  const auto begin = std::min(options.offset, selected.size());
  const auto end =
      std::min(begin + std::clamp<std::size_t>(options.page_size, 1, 500),
               selected.size());
  nlohmann::json page = nlohmann::json::array();
  for (auto i = begin; i < end; ++i)
    page.push_back(selected[i]);
  const auto pending = catalog.pending_tasks(request_id);
  scheduled = pending.size();
  auto analysis = aggregate_analysis(catalog, request_id, page,
                                     current_revision, options.path, gaps);
  const bool done = scheduled == 0 && gaps.empty();
  return {
      {"request_id", request_id},
      {"result_status", done ? "complete" : "partial"},
      {"scope", options.scope},
      {"path", options.path},
      {"historical_path", tracked},
      {"since", since},
      {"current_revision", current_revision},
      {"change_unit_count", selected.size()},
      {"change_units", page},
      {"analysis", std::move(analysis)},
      {"header_observation_matrix", std::move(header_observation_matrix)},
      {"path_segments", path_segments},
      {"enqueued_work_items", scheduled},
      {"completed_tasks", complete},
      {"coverage", {{"status", done ? "complete" : "partial"}, {"gaps", gaps}}},
      {"pending_work", pending},
      {"continuation",
       end < selected.size() ? std::to_string(end) : std::string{}}};
}
} // namespace history
