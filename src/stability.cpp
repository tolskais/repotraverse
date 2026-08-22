#include "history/stability.hpp"

#include "history/ir.hpp"
#include "history/lineage.hpp"
#include "history/query.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <stdexcept>

namespace history {
namespace {

bool wildcard(std::string_view pattern, std::string_view value) {
  std::vector<bool> previous(value.size() + 1), current(value.size() + 1);
  previous[0] = true;
  for (const char token : pattern) {
    current.assign(value.size() + 1, false);
    if (token == '*') {
      current[0] = previous[0];
      for (std::size_t index = 1; index <= value.size(); ++index)
        current[index] = previous[index] || current[index - 1];
    } else {
      for (std::size_t index = 1; index <= value.size(); ++index)
        current[index] = previous[index - 1] &&
                         (token == '?' || token == value[index - 1]);
    }
    previous.swap(current);
  }
  return previous[value.size()];
}

bool matches(const nlohmann::json &patterns, const std::string &path) {
  if (!patterns.is_array())
    return false;
  for (const auto &pattern : patterns)
    if (wildcard(pattern.get<std::string>(), path))
      return true;
  return false;
}

std::string developer_label(const nlohmann::json &partition,
                            const std::string &path) {
  if (matches(partition.value("exclude", nlohmann::json::array()), path))
    return "excluded";
  const bool stable =
      matches(partition.value("stable", nlohmann::json::array()), path);
  const bool variable =
      matches(partition.value("variable", nlohmann::json::array()), path);
  if (stable && variable)
    return "conflict";
  if (stable)
    return "stable";
  if (variable)
    return "variable";
  return "unlabeled";
}

nlohmann::json load_json(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot open stability experiment manifest");
  return nlohmann::json::parse(input);
}

void persist(const std::filesystem::path &path, const nlohmann::json &value) {
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << canonical_json(value) << '\n';
  if (!output)
    throw std::runtime_error("cannot persist stability report");
}

std::string csv(std::string value) {
  if (value.find_first_of(",\"\r\n") == std::string::npos)
    return value;
  std::string result{"\""};
  for (const auto character : value) {
    if (character == '"')
      result += "\"\"";
    else
      result.push_back(character);
  }
  result.push_back('"');
  return result;
}

void persist_csv(const std::filesystem::path &path,
                 const nlohmann::json &rows) {
  std::ofstream output(path, std::ios::binary);
  output << "historical_element_id,configuration,translation_unit,current_name,"
            "kind,path,classification,developer_label,volatility_score,"
            "implementation_score,interface_instability_score,"
            "structural_volatility_score,observable_transitions,change_units,"
            "authors,dependency_changes\n";
  for (const auto &row : rows) {
    output << csv(row.value("historical_element_id", std::string{})) << ','
           << csv(row.value("configuration", std::string{})) << ','
           << csv(row.value("translation_unit", std::string{})) << ','
           << csv(row.value("current_name", std::string{})) << ','
           << csv(row.value("kind", std::string{})) << ','
           << csv(row.value("path", std::string{})) << ','
           << csv(row.value("classification", std::string{})) << ','
           << csv(row.value("developer_label", std::string{})) << ','
           << std::setprecision(17) << row.value("volatility_score", 0.0) << ','
           << row.value("implementation_score", 0.0) << ','
           << row.value("interface_instability_score", 0.0) << ','
           << row.value("structural_volatility_score", 0.0) << ','
           << row.value("observable_transitions", 0U) << ','
           << row.value("change_units", 0U) << ','
           << row.value("authors", 0U) << ','
           << row.value("dependency_changes", 0U) << '\n';
  }
  if (!output)
    throw std::runtime_error("cannot persist stability CSV report");
}

} // namespace

nlohmann::json run_stability_experiment(
    const std::filesystem::path &manifest_path) {
  const auto manifest = load_json(manifest_path);
  if (manifest.value("schema_version", 0U) != kSchemaVersion ||
      !manifest.contains("series") || !manifest.at("series").is_array() ||
      !manifest.contains("partition"))
    throw std::runtime_error(
        "stability experiment requires v1 series and partition");
  const auto policy = manifest.value("policy", nlohmann::json::object());
  const auto minimum_transitions =
      policy.value("minimum_observable_transitions", 3U);
  const auto half_life = policy.value("half_life_transitions", 8.0);
  const auto half_life_days = policy.value("half_life_days", 90.0);
  const auto stable_threshold = policy.value("stable_threshold", 0.15);
  const auto variable_threshold = policy.value("variable_threshold", 0.75);
  const auto implementation_weight = policy.value("implementation_weight", 1.0);
  const auto interface_weight = policy.value("interface_weight", 4.0);
  const auto dependency_weight = policy.value("dependency_weight", 2.0);
  const auto move_weight = policy.value("move_weight", 0.2);
  const auto rename_weight = policy.value("rename_weight", 0.2);
  if (minimum_transitions == 0 || half_life <= 0 || half_life_days <= 0 ||
      stable_threshold < 0 ||
      variable_threshold <= stable_threshold)
    throw std::runtime_error("invalid stability policy");

  MemoryFactStore store;
  const auto revision_authors = manifest.value(
      "revision_authors", std::map<std::string, std::string>{});
  const auto revision_times = manifest.value(
      "revision_times", std::map<std::string, std::int64_t>{});
  std::int64_t latest_time = 0;
  for (const auto &[revision, time] : revision_times)
    latest_time = std::max(latest_time, time);
  nlohmann::json rows = nlohmann::json::array();
  nlohmann::json leakage = nlohmann::json::array();
  nlohmann::json islands = nlohmann::json::array();
  std::map<std::string, std::size_t> classifications;
  std::map<std::string, std::map<std::string, std::size_t>> agreement;
  for (const auto &series : manifest.at("series")) {
    if (!series.contains("bundles") || !series.at("bundles").is_array() ||
        series.at("bundles").size() < 2)
      throw std::runtime_error("each stability series requires at least two bundles");
    std::vector<EvidenceBundle> bundles;
    for (const auto &path : series.at("bundles"))
      bundles.push_back(store.load(path.get<std::string>()));
    std::map<std::string, std::string> parent;
    const auto find = [&](auto &&self, const std::string &id) -> std::string {
      auto [position, inserted] = parent.emplace(id, id);
      if (position->second == id)
        return id;
      return position->second = self(self, position->second);
    };
    const auto unite = [&](const std::string &left, const std::string &right) {
      const auto a = find(find, left), b = find(find, right);
      if (a != b)
        parent[std::max(a, b)] = std::min(a, b);
    };
    std::vector<TransitionResult> transitions;
    bool complete = std::all_of(
        bundles.begin(), bundles.end(), [](const auto &bundle) {
          return bundle.coverage.status == "complete";
        });
    for (std::size_t index = 0; index + 1 < bundles.size(); ++index) {
      transitions.push_back(trace_transition(bundles[index], bundles[index + 1]));
      complete = complete &&
                 bundles[index].configuration == bundles[index + 1].configuration;
      for (const auto &fact : transitions.back().facts)
        if (!fact.before_element.empty() && !fact.after_element.empty() &&
            fact.confidence != "ambiguous")
          unite(fact.before_element, fact.after_element);
    }
    struct Metrics {
      std::size_t observations{}, implementation_changes{}, interface_changes{},
          dependency_changes{}, moves{}, renames{}, ambiguous{};
      double implementation_score{}, interface_score{}, structural_score{};
      std::set<std::size_t> changed_transitions;
      std::set<std::string> authors;
      std::string name, kind, path;
    };
    std::map<std::string, Metrics> metrics;
    for (const auto &bundle : bundles)
      for (const auto &element : bundle.elements) {
        auto &item = metrics[find(find, element.compiler_id)];
        ++item.observations;
        item.name = element.qualified_name;
        item.kind = element.kind;
        item.path = element.location.path;
      }
    for (std::size_t index = 0; index < transitions.size(); ++index) {
      const auto age = static_cast<double>(transitions.size() - index - 1);
      const auto transition_recency = std::pow(0.5, age / half_life);
      for (const auto &fact : transitions[index].facts) {
        auto recency = transition_recency;
        if (const auto time = revision_times.find(fact.after_revision);
            time != revision_times.end() && latest_time >= time->second) {
          const auto age_days =
              static_cast<double>(latest_time - time->second) / 86400.0;
          recency = std::pow(0.5, age_days / half_life_days);
        }
        const auto id = !fact.before_element.empty() ? fact.before_element
                                                     : fact.after_element;
        auto &item = metrics[find(find, id)];
        if (fact.continuity == "added_or_unresolved" &&
            fact.before_element.empty())
          continue;
        if (fact.confidence == "ambiguous" ||
            fact.content_change == "unverified") {
          ++item.ambiguous;
          continue;
        }
        bool changed_fact = false;
        if (fact.content_change == "implementation" ||
            fact.content_change == "both") {
          ++item.implementation_changes;
          item.implementation_score += implementation_weight * recency;
          changed_fact = true;
        }
        if (fact.content_change == "interface" ||
            fact.content_change == "both") {
          ++item.interface_changes;
          item.interface_score += interface_weight * recency;
          changed_fact = true;
        }
        if (fact.dependencies_changed) {
          ++item.dependency_changes;
          item.structural_score += dependency_weight * recency;
          changed_fact = true;
        }
        if (fact.continuity == "moved" ||
            fact.continuity == "moved_and_renamed") {
          ++item.moves;
          item.structural_score += move_weight * recency;
          changed_fact = true;
        }
        if (fact.continuity == "renamed" ||
            fact.continuity == "moved_and_renamed") {
          ++item.renames;
          item.structural_score += rename_weight * recency;
          changed_fact = true;
        }
        if (changed_fact) {
          item.changed_transitions.insert(index);
          if (const auto author = revision_authors.find(fact.after_revision);
              author != revision_authors.end())
            item.authors.insert(author->second);
        }
      }
    }
    for (const auto &[root, item] : metrics) {
      const auto observable = item.observations > 0 ? item.observations - 1 : 0;
      const auto score = observable == 0
                             ? 0.0
                             : (item.implementation_score + item.interface_score +
                                item.structural_score) /
                                   static_cast<double>(observable);
      std::vector<std::string> evidence_gaps;
      if (!complete)
        evidence_gaps.push_back("series coverage is partial");
      if (item.ambiguous)
        evidence_gaps.push_back("ambiguous lineage transitions");
      if (observable < minimum_transitions)
        evidence_gaps.push_back("insufficient observable transitions");
      std::string classification = "insufficient_evidence";
      if (evidence_gaps.empty() && score <= stable_threshold)
        classification = "stable";
      else if (evidence_gaps.empty() && score >= variable_threshold)
        classification = "variable";
      const auto expected = developer_label(manifest.at("partition"), item.path);
      nlohmann::json row = {
          {"historical_element_id", "history-" + stable_hash(root)},
          {"configuration", series.value("configuration", std::string{})},
          {"translation_unit", series.value("translation_unit", std::string{})},
          {"current_name", item.name},
          {"kind", item.kind},
          {"path", item.path},
          {"classification", classification},
          {"developer_label", expected},
          {"volatility_score", score},
          {"implementation_score", item.implementation_score},
          {"interface_instability_score", item.interface_score},
          {"structural_volatility_score", item.structural_score},
          {"observed_versions", item.observations},
          {"observable_transitions", observable},
          {"implementation_changes", item.implementation_changes},
          {"interface_changes", item.interface_changes},
          {"dependency_changes", item.dependency_changes},
          {"moves", item.moves},
          {"renames", item.renames},
          {"ambiguous_transitions", item.ambiguous},
          {"change_units", item.changed_transitions.size()},
          {"authors", item.authors.size()},
          {"evidence_gaps", evidence_gaps}};
      rows.push_back(row);
      ++classifications[classification];
      ++agreement[expected][classification];
      if (expected == "stable" && classification == "variable")
        leakage.push_back(row);
      if (expected == "variable" && classification == "stable")
        islands.push_back(row);
    }
  }
  nlohmann::json result = {
      {"schema_version", kSchemaVersion},
      {"artifact_version", 1},
      {"policy",
       {{"minimum_observable_transitions", minimum_transitions},
        {"half_life_transitions", half_life},
        {"half_life_days", half_life_days},
        {"stable_threshold", stable_threshold},
        {"variable_threshold", variable_threshold},
        {"implementation_weight", implementation_weight},
        {"interface_weight", interface_weight},
        {"dependency_weight", dependency_weight},
        {"move_weight", move_weight},
        {"rename_weight", rename_weight}}},
      {"classifications", classifications},
      {"agreement", agreement},
      {"variation_leakage", std::move(leakage)},
      {"stable_islands", std::move(islands)},
      {"elements", std::move(rows)}};
  if (manifest.contains("report")) {
    const auto report =
        std::filesystem::path(manifest.at("report").get<std::string>());
    persist(report, result);
    auto csv_report = report;
    csv_report.replace_extension(".csv");
    persist_csv(csv_report, result.at("elements"));
  }
  return result;
}

} // namespace history
