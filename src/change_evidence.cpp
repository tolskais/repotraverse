#include "history/change_evidence.hpp"

#include "history/ir.hpp"
#include "history/process.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace history {
namespace {

struct ElementView {
  std::string name;
  std::set<std::string> interfaces, implementations, dependencies, references;
  std::vector<SourceAnchor> locations;
};

struct TuView {
  std::string context_id, translation_unit;
  std::map<std::string, ElementView> elements;
  std::set<std::string> project_files;
  std::map<std::string, std::set<std::string>> dependents;
};

using ViewKey = std::tuple<std::string, std::string, std::string>;

nlohmann::json read_json(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot read semantic evidence: " +
                             path.string());
  return nlohmann::json::parse(input);
}

std::vector<nlohmann::json>
capture_records(const std::filesystem::path &directory) {
  std::vector<nlohmann::json> records;
  if (!std::filesystem::exists(directory))
    return records;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(directory))
    if (entry.is_regular_file() && entry.path().extension() == ".json")
      records.push_back(read_json(entry.path()));
  return records;
}

std::map<ViewKey, TuView> views(const nlohmann::json &report) {
  const auto output =
      std::filesystem::path(report.at("output").get<std::string>());
  std::map<std::string, std::set<std::string>> project_files;
  for (const auto &record : capture_records(output / "capture")) {
    const auto tu = record.value("translation_unit", std::string{});
    auto &files = project_files[tu];
    for (const auto &path :
         record.value("project_files", std::vector<std::string>{}))
      files.insert(path);
  }
  std::map<ViewKey, TuView> result;
  const auto directory = output / "manifests";
  if (!std::filesystem::exists(directory))
    return result;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json")
      continue;
    const auto manifest = read_json(entry.path()).get<TuManifest>();
    auto &view = result[{manifest.build_variant.variant_id,
                         manifest.configuration,
                         manifest.translation_unit}];
    view.context_id = manifest.context_id;
    view.translation_unit = manifest.translation_unit;
    view.project_files = project_files[manifest.translation_unit];
    std::map<std::string, std::string> names;
    for (const auto &element : manifest.elements)
      names[element.element_id] = element.qualified_name;
    for (const auto &variant : manifest.variants) {
      auto &element = view.elements[variant.element_id];
      element.name = names[variant.element_id];
      element.interfaces.insert(variant.interface_fingerprint);
      element.implementations.insert(variant.implementation_fingerprint);
      element.dependencies.insert(variant.dependency_fingerprint);
      element.references.insert(variant.referenced_element_ids.begin(),
                                variant.referenced_element_ids.end());
      for (const auto &dependency : variant.referenced_element_ids)
        view.dependents[dependency].insert(variant.element_id);
    }
    for (const auto &observation : manifest.observations)
      view.elements[observation.element_id].locations.push_back(
          observation.location);
    for (const auto &expansion : manifest.macro_expansions)
      if (!expansion.containing_element_id.empty())
        view.dependents[expansion.macro_element_id].insert(
            expansion.containing_element_id);
  }
  return result;
}

std::set<std::string> changed_paths(const std::filesystem::path &repository,
                                    const std::string &before,
                                    const std::string &after) {
  const auto process = run_process({"git", "-C", repository.string(), "diff",
                                    "--name-only", "-z", before, after,
                                    "--"});
  if (process.exit_code != 0 || process.timed_out || process.output_truncated)
    throw std::runtime_error("cannot read paths for change-origin evidence");
  std::set<std::string> result;
  std::size_t begin = 0;
  while (begin < process.output.size()) {
    const auto end = process.output.find('\0', begin);
    const auto path = process.output.substr(
        begin, end == std::string::npos ? end : end - begin);
    if (!path.empty())
      result.insert(path);
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return result;
}

std::vector<std::string> dimensions(const ElementView &before,
                                    const ElementView &after) {
  std::vector<std::string> result;
  if (before.interfaces != after.interfaces)
    result.push_back("interface");
  if (before.implementations != after.implementations)
    result.push_back("implementation");
  if (before.dependencies != after.dependencies)
    result.push_back("dependencies");
  return result;
}

} // namespace

nlohmann::json summarize_change_evidence(
    const std::filesystem::path &repository,
    const nlohmann::json &revision_reports, const nlohmann::json &budget) {
  const auto maximum =
      budget.at("max_induced_elements_per_transition").get<std::size_t>();
  const auto maximum_depth =
      budget.at("max_dependency_depth").get<std::size_t>();
  nlohmann::json transitions = nlohmann::json::array();
  nlohmann::json gaps = nlohmann::json::array();
  std::map<std::string, std::size_t> origin_counts;
  for (std::size_t revision_index = 1;
       revision_index < revision_reports.size(); ++revision_index) {
    const auto &before_report = revision_reports[revision_index - 1];
    const auto &after_report = revision_reports[revision_index];
    const auto before_revision =
        before_report.at("revision").get<std::string>();
    const auto after_revision =
        after_report.at("revision").get<std::string>();
    const auto touched_paths =
        changed_paths(repository, before_revision, after_revision);
    const auto before_views = views(before_report);
    const auto after_views = views(after_report);
    nlohmann::json facts = nlohmann::json::array();
    std::set<std::tuple<std::string, std::string, std::string>> emitted;
    bool capped = false;

    for (const auto &[key, after] : after_views) {
      const auto old_view = before_views.find(key);
      if (old_view == before_views.end())
        continue;
      const auto &before = old_view->second;
      std::map<std::string, std::vector<std::string>> semantic_changes;
      std::map<std::string, std::set<std::string>> own_roles;
      for (const auto &[id, element] : after.elements) {
        const auto old = before.elements.find(id);
        if (old == before.elements.end())
          continue;
        auto changed = dimensions(old->second, element);
        if (!changed.empty())
          semantic_changes[id] = std::move(changed);
        for (const auto &location : element.locations)
          if (touched_paths.contains(location.path))
            own_roles[id].insert(location.role);
      }
      std::set<std::string> changed_dependencies;
      for (const auto &path : after.project_files)
        if (path != after.translation_unit && touched_paths.contains(path))
          changed_dependencies.insert(path);
      const bool context_changed = before.context_id != after.context_id;

      const auto emit = [&](const std::string &id, const std::string &origin,
                            const std::vector<std::string> &semantic,
                            const std::set<std::string> &causes,
                            std::size_t depth) {
        if (!emitted.emplace(id, origin, after.translation_unit).second)
          return;
        if (facts.size() >= maximum) {
          capped = true;
          return;
        }
        const auto element = after.elements.find(id);
        facts.push_back(
            {{"element_id", id},
             {"qualified_name",
              element == after.elements.end() ? std::string{}
                                               : element->second.name},
             {"translation_unit", after.translation_unit},
             {"configuration", std::get<1>(key)},
             {"build_variant_id", std::get<0>(key)},
             {"origin", origin},
             {"semantic_dimensions", semantic},
             {"causes", causes},
             {"dependency_depth", depth},
             {"evidence_tier", "semantic"}});
        ++origin_counts[origin];
      };

      std::set<std::string> propagation_roots;
      for (const auto &[id, semantic] : semantic_changes) {
        if (const auto own = own_roles.find(id);
            own != own_roles.end() && !own->second.empty()) {
          const bool declaration_only =
              own->second.contains("declaration") &&
              !own->second.contains("definition");
          emit(id, declaration_only ? "own_declaration" : "direct_source",
               semantic, {}, 0);
          propagation_roots.insert(id);
        } else if (context_changed) {
          emit(id, "build_configuration", semantic, changed_dependencies, 0);
        } else if (!changed_dependencies.empty()) {
          emit(id, "confirmed_induced", semantic, changed_dependencies, 1);
          propagation_roots.insert(id);
        } else {
          emit(id, "unattributed_semantic_change", semantic, {}, 0);
        }
      }

      // An included header changing is exposure. It is only a confirmed
      // induced transition when the dependent element's own semantic
      // fingerprints also changed under the same context.
      if (!changed_dependencies.empty())
        for (const auto &[id, element] : after.elements) {
          (void)element;
          if (own_roles.contains(id))
            continue;
          emit(id,
               semantic_changes.contains(id) && !context_changed
                   ? "confirmed_induced"
                   : "upstream_exposure",
               semantic_changes.contains(id) && !context_changed
                   ? semantic_changes.at(id)
                   : std::vector<std::string>{},
               changed_dependencies, 1);
        }

      std::queue<std::tuple<std::string, std::size_t, std::string>> pending;
      for (const auto &root : propagation_roots)
        pending.emplace(root, 0, root);
      std::set<std::pair<std::string, std::string>> visited;
      while (!pending.empty()) {
        auto [source, depth, root] = pending.front();
        pending.pop();
        if (depth >= maximum_depth)
          continue;
        const auto dependents = after.dependents.find(source);
        if (dependents == after.dependents.end())
          continue;
        for (const auto &dependent : dependents->second) {
          if (!visited.emplace(root, dependent).second)
            continue;
          emit(dependent,
               semantic_changes.contains(dependent) ? "confirmed_induced"
                                                    : "upstream_exposure",
               semantic_changes.contains(dependent)
                   ? semantic_changes.at(dependent)
                   : std::vector<std::string>{},
               {root}, depth + 1);
          pending.emplace(dependent, depth + 1, root);
        }
      }
    }
    if (capped)
      gaps.push_back(
          {{"kind", "induced_element_budget_exhausted"},
           {"before_revision", before_revision},
           {"after_revision", after_revision},
           {"observed", facts.size()},
           {"cap", maximum}});
    transitions.push_back({{"before_revision", before_revision},
                           {"after_revision", after_revision},
                           {"changed_paths", touched_paths},
                           {"facts", facts},
                           {"coverage", capped ? "partial" : "complete"}});
  }
  return {{"transitions", transitions},
          {"origin_counts", origin_counts},
          {"evidence_gaps", gaps},
          {"dependency_depth_cap", maximum_depth},
          {"per_transition_element_cap", maximum}};
}

} // namespace history
