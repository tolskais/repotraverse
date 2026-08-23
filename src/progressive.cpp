#include "history/progressive.hpp"

#include "history/ir.hpp"
#include "history/process.hpp"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

extern "C" const TSLanguage *tree_sitter_c(void);
extern "C" const TSLanguage *tree_sitter_cpp(void);

namespace history {
namespace {

constexpr std::string_view kParserIdentity =
    "tree-sitter-0.26.11;c-0.24.2;cpp-0.23.4";

struct ParserDeleter {
  void operator()(TSParser *value) const { ts_parser_delete(value); }
};
struct TreeDeleter {
  void operator()(TSTree *value) const { ts_tree_delete(value); }
};
using ParserPtr = std::unique_ptr<TSParser, ParserDeleter>;
using TreePtr = std::unique_ptr<TSTree, TreeDeleter>;

struct PathChange {
  std::string status, before_path, after_path;
  std::uint64_t additions{}, deletions{};
};

struct Touch {
  std::string before_revision, after_revision, before_path, after_path, status;
  std::uint64_t additions{}, deletions{};
  std::int64_t time{};
};

struct FileFact {
  std::string path;
  bool exists{true}, header{};
  std::uint64_t touches{}, additions{}, deletions{}, renames{}, moves{};
  std::uint64_t include_fanout{};
  std::uint64_t recent_touches{};
  std::int64_t first_change{}, last_change{};
  std::set<std::int64_t> active_months;
  std::vector<Touch> transitions;
  std::set<std::string> promotion_reasons;
};

struct StratumBudget {
  std::size_t max_files{}, max_syntax_transitions{}, max_semantic_elements{};
};

bool source_or_header(std::string path) {
  std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  const auto extension = std::filesystem::path(path).extension().string();
  static const std::set<std::string> extensions = {
      ".c",   ".cc",  ".cpp", ".cxx", ".h",   ".hh", ".hpp",
      ".hxx", ".inc", ".inl", ".ipp", ".tpp"};
  return extensions.contains(extension);
}

bool header_path(std::string path) {
  std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  const auto extension = std::filesystem::path(path).extension().string();
  static const std::set<std::string> extensions = {
      ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp", ".tpp"};
  return extensions.contains(extension);
}

std::string screening_language(const std::string &path) {
  auto extension = std::filesystem::path(path).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension == ".c" ? "c" : "cpp";
}

void require_git(const ProcessOutput &result, std::string_view operation) {
  if (result.exit_code != 0 || result.timed_out || result.output_truncated)
    throw std::runtime_error("git failed while attempting to " +
                             std::string(operation) + ": " +
                             stable_hash(result.error));
}

std::vector<std::string> nul_tokens(std::string_view value) {
  std::vector<std::string> result;
  std::size_t begin = 0;
  while (begin < value.size()) {
    const auto end = value.find('\0', begin);
    result.emplace_back(value.substr(
        begin, end == std::string_view::npos ? value.size() - begin
                                             : end - begin));
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }
  return result;
}

std::map<std::string, std::pair<std::uint64_t, std::uint64_t>>
numstat(const std::filesystem::path &repository, const std::string &before,
        const std::string &after) {
  const auto result = run_process({"git", "-C", repository.string(), "diff",
                                   "--numstat", "-z", before, after, "--"});
  require_git(result, "read file churn");
  std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> values;
  std::size_t cursor = 0;
  while (cursor < result.output.size()) {
    const auto first_tab = result.output.find('\t', cursor);
    const auto second_tab = result.output.find('\t', first_tab + 1);
    const auto terminator = result.output.find('\0', second_tab + 1);
    if (first_tab == std::string::npos || second_tab == std::string::npos ||
        terminator == std::string::npos)
      break;
    const auto added = result.output.substr(cursor, first_tab - cursor);
    const auto deleted =
        result.output.substr(first_tab + 1, second_tab - first_tab - 1);
    auto path = result.output.substr(second_tab + 1, terminator - second_tab - 1);
    cursor = terminator + 1;
    if (path.empty()) {
      const auto old_end = result.output.find('\0', cursor);
      if (old_end == std::string::npos)
        break;
      cursor = old_end + 1;
      const auto new_end = result.output.find('\0', cursor);
      if (new_end == std::string::npos)
        break;
      path = result.output.substr(cursor, new_end - cursor);
      cursor = new_end + 1;
    }
    if (added != "-" && deleted != "-")
      values[path] = {std::stoull(added), std::stoull(deleted)};
  }
  return values;
}

std::vector<PathChange>
path_changes(const std::filesystem::path &repository, const std::string &before,
             const std::string &after) {
  const auto result = run_process({"git", "-C", repository.string(), "diff",
                                   "--name-status", "-M", "-z", before,
                                   after, "--"});
  require_git(result, "read changed paths");
  const auto tokens = nul_tokens(result.output);
  const auto churn = numstat(repository, before, after);
  std::vector<PathChange> changes;
  for (std::size_t index = 0; index < tokens.size();) {
    const auto status = tokens[index++];
    if (status.empty() || index >= tokens.size())
      break;
    PathChange change;
    change.status = status;
    if (status.front() == 'R' || status.front() == 'C') {
      change.before_path = tokens[index++];
      if (index >= tokens.size())
        break;
      change.after_path = tokens[index++];
    } else {
      change.after_path = tokens[index++];
      change.before_path = change.after_path;
      if (status.front() == 'A')
        change.before_path.clear();
      if (status.front() == 'D')
        change.after_path.clear();
    }
    const auto key = change.after_path.empty() ? change.before_path
                                               : change.after_path;
    if (const auto found = churn.find(key); found != churn.end()) {
      change.additions = found->second.first;
      change.deletions = found->second.second;
    }
    changes.push_back(std::move(change));
  }
  return changes;
}

std::int64_t revision_time(const std::filesystem::path &repository,
                           const std::string &revision) {
  const auto result = run_process({"git", "-C", repository.string(), "show",
                                   "-s", "--format=%ct", revision});
  require_git(result, "read revision time");
  return std::stoll(result.output);
}

std::int64_t calendar_month(std::int64_t timestamp) {
  using namespace std::chrono;
  const year_month_day value{
      floor<days>(sys_seconds{seconds{timestamp}})};
  return static_cast<int>(value.year()) * 12 +
         static_cast<unsigned>(value.month());
}

std::vector<std::string> tracked_paths(
    const std::filesystem::path &repository, const std::string &revision) {
  const auto result = run_process({"git", "-C", repository.string(),
                                   "ls-tree", "-r", "--name-only", "-z",
                                   revision});
  require_git(result, "list tracked screening files");
  return nul_tokens(result.output);
}

void measure_include_fanout(const std::filesystem::path &repository,
                            const std::string &revision,
                            std::map<std::string, FileFact> &facts) {
  std::map<std::string, std::vector<std::string>> header_by_spelling;
  for (const auto &[path, fact] : facts) {
    if (!fact.header)
      continue;
    std::string suffix = path;
    for (;;) {
      header_by_spelling[suffix].push_back(path);
      const auto separator = suffix.find('/');
      if (separator == std::string::npos)
        break;
      suffix.erase(0, separator + 1);
    }
  }
  ProcessOptions options;
  options.max_output_bytes = 512ULL * 1024ULL * 1024ULL;
  const auto result = run_process(
      {"git", "-C", repository.string(), "grep", "-I", "-n", "-z", "-E",
       "^[[:space:]]*#[[:space:]]*include[[:space:]]*[<\"]", revision,
       "--", "*.c", "*.cc", "*.cpp", "*.cxx", "*.h", "*.hh", "*.hpp",
       "*.hxx"},
      options);
  if (result.exit_code == 1)
    return;
  require_git(result, "measure header include fanout");
  static const std::regex include(R"([<"]([^>"]+)[>"])");
  std::map<std::string, std::set<std::string>> includers;
  std::size_t cursor = 0;
  while (cursor < result.output.size()) {
    const auto path_end = result.output.find('\0', cursor);
    if (path_end == std::string::npos)
      break;
    auto includer = result.output.substr(cursor, path_end - cursor);
    cursor = path_end + 1;
    const auto line_end = result.output.find('\0', cursor);
    if (line_end == std::string::npos)
      break;
    cursor = line_end + 1;
    const auto content_end = result.output.find('\n', cursor);
    const auto content = result.output.substr(
        cursor, content_end == std::string::npos ? content_end
                                                 : content_end - cursor);
    cursor = content_end == std::string::npos ? result.output.size()
                                               : content_end + 1;
    const auto prefix = revision + ":";
    if (includer.starts_with(prefix))
      includer.erase(0, prefix.size());
    std::smatch match;
    if (!std::regex_search(content, match, include))
      continue;
    const auto candidates = header_by_spelling.find(match[1].str());
    if (candidates == header_by_spelling.end() ||
        candidates->second.size() != 1)
      continue;
    includers[candidates->second.front()].insert(includer);
  }
  for (auto &[path, files] : includers)
    facts[path].include_fanout = files.size();
}

std::optional<std::string> blob(const std::filesystem::path &repository,
                                const std::string &revision,
                                const std::string &path) {
  if (revision.empty() || path.empty())
    return std::nullopt;
  ProcessOptions options;
  options.max_output_bytes = 512ULL * 1024ULL * 1024ULL;
  const auto result = run_process({"git", "-C", repository.string(), "show",
                                   revision + ":" + path}, options);
  if (result.exit_code != 0 || result.timed_out || result.output_truncated)
    return std::nullopt;
  return result.output;
}

bool wildcard(std::string_view pattern, std::string_view value) {
  std::vector<bool> previous(value.size() + 1), current(value.size() + 1);
  previous[0] = true;
  for (const auto token : pattern) {
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
  return std::any_of(patterns.begin(), patterns.end(), [&](const auto &pattern) {
    return pattern.is_string() &&
           wildcard(pattern.template get<std::string>(), path);
  });
}

std::string developer_label(const nlohmann::json &partition,
                            const std::string &path) {
  if (matches(partition.value("exclude", nlohmann::json::array()), path))
    return "excluded";
  const bool common =
      matches(partition.value("stable", nlohmann::json::array()), path);
  const bool variable =
      matches(partition.value("variable", nlohmann::json::array()), path);
  if (common && variable)
    return "conflict";
  if (common)
    return "common";
  if (variable)
    return "variable";
  return "unlabeled";
}

StratumBudget stratum_budget(const nlohmann::json &budget,
                             const std::string &name) {
  if (!budget.contains(name) || !budget.at(name).is_object())
    throw std::runtime_error("pilot budget requires stratum " + name);
  const auto &value = budget.at(name);
  for (const auto *field :
       {"max_files", "max_syntax_transitions", "max_semantic_elements"})
    if (!value.contains(field) || !value.at(field).is_number_integer() ||
        value.at(field).get<std::int64_t>() < 0)
      throw std::runtime_error("pilot budget " + name + " requires " + field);
  return {value.at("max_files").get<std::size_t>(),
          value.at("max_syntax_transitions").get<std::size_t>(),
          value.at("max_semantic_elements").get<std::size_t>()};
}

void validate_budget(const nlohmann::json &budget) {
  if (!budget.is_object())
    throw std::runtime_error("pilot requires budget object");
  for (const auto &name : {"common_leakage", "variable_detail",
                           "stable_island_candidates", "high_impact_headers",
                           "controls"})
    (void)stratum_budget(budget, name);
  for (const auto *field : {"max_capture_revisions", "max_dependency_depth",
                            "max_induced_elements_per_transition"})
    if (!budget.contains(field) || !budget.at(field).is_number_integer() ||
        budget.at(field).get<std::int64_t>() < 0)
      throw std::runtime_error(std::string("pilot budget requires ") + field);
}

std::string node_text(TSNode node, std::string_view source) {
  const auto begin = static_cast<std::size_t>(ts_node_start_byte(node));
  const auto end = static_cast<std::size_t>(ts_node_end_byte(node));
  if (begin > end || end > source.size())
    return {};
  return std::string(source.substr(begin, end - begin));
}

std::string normalized(std::string_view value) {
  std::string result;
  bool whitespace = false;
  for (const unsigned char character : value) {
    if (std::isspace(character)) {
      whitespace = true;
    } else {
      if (whitespace && !result.empty())
        result.push_back(' ');
      whitespace = false;
      result.push_back(static_cast<char>(character));
    }
  }
  return result;
}

bool identifier_node(std::string_view type) {
  return type == "identifier" || type == "field_identifier" ||
         type == "type_identifier" || type == "namespace_identifier" ||
         type == "operator_name" || type == "destructor_name";
}

std::optional<TSNode> find_name_node(TSNode node) {
  if (const auto named =
          ts_node_child_by_field_name(node, "name", sizeof("name") - 1);
      !ts_node_is_null(named))
    return named;
  if (identifier_node(ts_node_type(node)))
    return node;
  if (const auto declarator = ts_node_child_by_field_name(
          node, "declarator", sizeof("declarator") - 1);
      !ts_node_is_null(declarator))
    if (const auto found = find_name_node(declarator))
      return found;
  const auto count = ts_node_named_child_count(node);
  for (std::uint32_t index = 0; index < count; ++index)
    if (const auto found = find_name_node(ts_node_named_child(node, index)))
      return found;
  return std::nullopt;
}

bool contains_type(TSNode node, std::string_view wanted) {
  if (wanted == ts_node_type(node))
    return true;
  const auto count = ts_node_named_child_count(node);
  for (std::uint32_t index = 0; index < count; ++index)
    if (contains_type(ts_node_named_child(node, index), wanted))
      return true;
  return false;
}

std::string site_kind(TSNode node) {
  const std::string_view type = ts_node_type(node);
  if (type == "function_definition")
    return "function_definition";
  if ((type == "declaration" || type == "field_declaration") &&
      contains_type(node, "function_declarator"))
    return "function_declaration";
  if (type == "class_specifier" || type == "struct_specifier" ||
      type == "union_specifier")
    return "record";
  if (type == "enum_specifier")
    return "enum";
  if (type == "type_definition" || type == "alias_declaration")
    return "type_alias";
  if (type == "namespace_definition")
    return "namespace";
  if (type == "preproc_def" || type == "preproc_function_def")
    return "macro";
  return {};
}

bool scope_kind(std::string_view kind) {
  return kind == "record" || kind == "enum" || kind == "namespace";
}

std::string interface_text(TSNode node, std::string_view source,
                           std::string_view kind) {
  if (kind == "function_definition") {
    const auto body =
        ts_node_child_by_field_name(node, "body", sizeof("body") - 1);
    if (!ts_node_is_null(body)) {
      const auto begin = static_cast<std::size_t>(ts_node_start_byte(node));
      const auto end = static_cast<std::size_t>(ts_node_start_byte(body));
      if (begin <= end && end <= source.size())
        return normalized(source.substr(begin, end - begin));
    }
  }
  if (kind == "record" || kind == "enum") {
    const auto name = find_name_node(node);
    return name ? node_text(*name, source) : std::string{};
  }
  return normalized(node_text(node, source));
}

void collect_sites(TSNode node, std::string_view source,
                   const std::string &path,
                   std::vector<std::string> enclosing,
                   std::vector<nlohmann::json> &sites,
                   std::map<std::string, std::size_t> &occurrences,
                   std::size_t &error_nodes) {
  const std::string_view type = ts_node_type(node);
  if (type == "ERROR" || ts_node_is_missing(node))
    ++error_nodes;
  const auto kind = site_kind(node);
  std::string name;
  if (!kind.empty())
    if (const auto found = find_name_node(node))
      name = normalized(node_text(*found, source));
  if (!kind.empty() && !name.empty()) {
    std::string qualified;
    for (const auto &part : enclosing) {
      if (!qualified.empty())
        qualified += "::";
      qualified += part;
    }
    if (!qualified.empty())
      qualified += "::";
    qualified += name;
    const auto interface_shape = interface_text(node, source, kind);
    const auto base = stable_hash(path + "\n" + kind + "\n" + qualified +
                                  "\n" + interface_shape);
    const auto occurrence = occurrences[base]++;
    const auto start = ts_node_start_point(node);
    const auto end = ts_node_end_point(node);
    sites.push_back(
        {{"syntactic_symbol_id",
          stable_hash(base + "\n" + std::to_string(occurrence))},
         {"identity_kind", "syntactic_candidate"},
         {"kind", kind},
         {"name", name},
         {"qualified_name", qualified},
         {"path", path},
         {"begin_line", start.row + 1},
         {"begin_column", start.column + 1},
         {"end_line", end.row + 1},
         {"end_column", end.column + 1},
         {"interface_fingerprint", stable_hash(interface_shape)},
         {"structural_fingerprint",
          stable_hash(normalized(node_text(node, source)))}});
    if (scope_kind(kind))
      enclosing.push_back(name);
  }
  const auto count = ts_node_named_child_count(node);
  for (std::uint32_t index = 0; index < count; ++index)
    collect_sites(ts_node_named_child(node, index), source, path, enclosing,
                  sites, occurrences, error_nodes);
}

nlohmann::json changed_ranges(const std::filesystem::path &repository,
                              const Touch &touch) {
  std::vector<std::string> command = {"git", "-C", repository.string(),
                                      "diff", "--no-color", "--unified=0",
                                      touch.before_revision,
                                      touch.after_revision, "--"};
  if (!touch.before_path.empty())
    command.push_back(touch.before_path);
  if (!touch.after_path.empty() && touch.after_path != touch.before_path)
    command.push_back(touch.after_path);
  const auto result = run_process(command);
  require_git(result, "map changed ranges");
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

bool overlaps(const nlohmann::json &site, std::uint64_t start,
              std::uint64_t count) {
  const auto last = count == 0 ? start : start + count - 1;
  return site.value("begin_line", 0ULL) <= last &&
         site.value("end_line", 0ULL) >= start;
}

nlohmann::json mapped_site(const nlohmann::json &sites, std::uint64_t start,
                           std::uint64_t count) {
  const nlohmann::json *best = nullptr;
  std::uint64_t best_span = UINT64_MAX;
  std::size_t matches = 0;
  for (const auto &site : sites) {
    if (!overlaps(site, start, count))
      continue;
    ++matches;
    const auto span = site.value("end_line", 0ULL) -
                      site.value("begin_line", 0ULL);
    if (span < best_span) {
      best = &site;
      best_span = span;
    }
  }
  if (!best)
    return {{"state", "unmapped"}};
  auto result = *best;
  result["state"] = matches == 1 ? "mapped" : "mapped_smallest_enclosing";
  result["overlapping_sites"] = matches;
  return result;
}

nlohmann::json set_json(const std::set<std::string> &values) {
  return nlohmann::json(values);
}

} // namespace

nlohmann::json parse_syntax_blob(std::string_view language,
                                 std::string_view path,
                                 std::string_view revision,
                                 std::string_view source) {
  const TSLanguage *grammar = nullptr;
  if (language == "c")
    grammar = tree_sitter_c();
  else if (language == "cpp")
    grammar = tree_sitter_cpp();
  else
    throw std::runtime_error("syntax screening language must be c or cpp");
  if (source.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::runtime_error("source blob exceeds Tree-sitter v1 size limit");
  ParserPtr parser(ts_parser_new());
  if (!parser || !ts_parser_set_language(parser.get(), grammar))
    throw std::runtime_error("cannot initialize Tree-sitter parser");
  TreePtr tree(ts_parser_parse_string(parser.get(), nullptr, source.data(),
                                      static_cast<std::uint32_t>(source.size())));
  if (!tree)
    throw std::runtime_error("Tree-sitter failed to parse source blob");
  std::vector<nlohmann::json> sites;
  std::map<std::string, std::size_t> occurrences;
  std::size_t error_nodes = 0;
  collect_sites(ts_tree_root_node(tree.get()), source, std::string(path), {},
                sites, occurrences, error_nodes);
  for (auto &site : sites)
    site["language"] = language;
  return {{"schema_version", 1},
          {"artifact_version", 1},
          {"record_type", "syntax_blob"},
          {"identity_kind", "syntactic_candidate"},
          {"parser_identity", kParserIdentity},
          {"language", language},
          {"revision", revision},
          {"path", path},
          {"source_fingerprint", stable_hash(source)},
          {"source_bytes", source.size()},
          {"coverage",
           {{"status", error_nodes == 0 ? "complete" : "partial"},
            {"error_nodes", error_nodes},
            {"canonical_identity", false},
            {"build_context_observed", false}}},
          {"sites", sites}};
}

nlohmann::json
plan_progressive_screening(const ProgressiveScreeningOptions &options) {
  if (!std::filesystem::is_directory(options.repository))
    throw std::runtime_error("progressive screening repository is invalid");
  if (options.output.empty())
    throw std::runtime_error("progressive screening requires output path");
  if (options.revisions.size() < 2)
    throw std::runtime_error("progressive screening requires two revisions");
  validate_budget(options.budget);

  std::map<std::string, FileFact> facts;
  for (const auto &path : tracked_paths(options.repository,
                                        options.revisions.back()))
    if (source_or_header(path)) {
      auto &fact = facts[path];
      fact.path = path;
      fact.header = header_path(path);
    }
  for (std::size_t index = 1; index < options.revisions.size(); ++index) {
    const auto time = revision_time(options.repository, options.revisions[index]);
    for (const auto &change : path_changes(options.repository,
                                           options.revisions[index - 1],
                                           options.revisions[index])) {
      const auto path = change.after_path.empty() ? change.before_path
                                                  : change.after_path;
      if (!source_or_header(path) && !source_or_header(change.before_path))
        continue;
      if (!change.before_path.empty() && !change.after_path.empty() &&
          change.before_path != change.after_path) {
        if (auto old = facts.find(change.before_path); old != facts.end()) {
          auto moved = std::move(old->second);
          facts.erase(old);
          moved.path = change.after_path;
          ++moved.renames;
          if (std::filesystem::path(change.before_path).parent_path() !=
              std::filesystem::path(change.after_path).parent_path())
            ++moved.moves;
          facts[change.after_path] = std::move(moved);
        }
      }
      auto &fact = facts[path];
      fact.path = path;
      fact.header = header_path(path);
      fact.exists = change.status.empty() || change.status.front() != 'D';
      ++fact.touches;
      fact.additions += change.additions;
      fact.deletions += change.deletions;
      if (fact.first_change == 0)
        fact.first_change = time;
      fact.last_change = time;
      fact.active_months.insert(calendar_month(time));
      fact.transitions.push_back(
          {options.revisions[index - 1], options.revisions[index],
           change.before_path, change.after_path, change.status,
           change.additions, change.deletions, time});
    }
  }
  measure_include_fanout(options.repository, options.revisions.back(), facts);
  const auto latest_time =
      revision_time(options.repository, options.revisions.back());
  for (auto &[path, fact] : facts) {
    (void)path;
    fact.recent_touches = static_cast<std::uint64_t>(std::count_if(
        fact.transitions.begin(), fact.transitions.end(), [&](const auto &touch) {
          return touch.time >= latest_time - 90LL * 24LL * 60LL * 60LL;
        }));
  }

  std::map<std::string, StratumBudget> budgets;
  for (const auto &name : {"common_leakage", "variable_detail",
                           "stable_island_candidates", "high_impact_headers",
                           "controls"})
    budgets[name] = stratum_budget(options.budget, name);

  std::map<std::string, std::vector<FileFact *>> ranked;
  for (auto &[path, fact] : facts) {
    if (!fact.exists)
      continue;
    const auto label = developer_label(options.partition, path);
    if (label == "common" && fact.touches > 0)
      ranked["common_leakage"].push_back(&fact);
    if (label == "variable") {
      if (fact.touches > 0)
        ranked["variable_detail"].push_back(&fact);
      ranked["stable_island_candidates"].push_back(&fact);
    }
    if (fact.header && fact.touches > 0)
      ranked["high_impact_headers"].push_back(&fact);
    if (label != "excluded" && label != "conflict")
      ranked["controls"].push_back(&fact);
  }
  const auto high_change = [](const FileFact *left, const FileFact *right) {
    return std::tie(left->recent_touches, left->touches, left->additions,
                    left->deletions, left->last_change, left->path) >
           std::tie(right->recent_touches, right->touches, right->additions,
                    right->deletions, right->last_change, right->path);
  };
  for (const auto &name : {"common_leakage", "variable_detail"})
    std::sort(ranked[name].begin(), ranked[name].end(), high_change);
  std::sort(ranked["stable_island_candidates"].begin(),
            ranked["stable_island_candidates"].end(),
            [](const FileFact *left, const FileFact *right) {
              return std::tie(left->touches, left->additions, left->deletions,
                              left->path) <
                     std::tie(right->touches, right->additions,
                              right->deletions, right->path);
            });
  std::sort(ranked["high_impact_headers"].begin(),
            ranked["high_impact_headers"].end(),
            [](const FileFact *left, const FileFact *right) {
              const auto left_impact =
                  left->touches * (left->include_fanout + 1);
              const auto right_impact =
                  right->touches * (right->include_fanout + 1);
              return std::tie(left_impact, left->touches, left->path) >
                     std::tie(right_impact, right->touches, right->path);
            });
  std::sort(ranked["controls"].begin(), ranked["controls"].end(),
            [](const FileFact *left, const FileFact *right) {
              return std::make_tuple(stable_hash(left->path), left->path) <
                     std::make_tuple(stable_hash(right->path), right->path);
            });

  std::set<std::string> selected_non_controls;
  for (const auto &name : {"common_leakage", "variable_detail",
                           "stable_island_candidates",
                           "high_impact_headers"}) {
    std::size_t selected = 0;
    for (auto *fact : ranked[name]) {
      if (selected >= budgets[name].max_files)
        break;
      fact->promotion_reasons.insert(name);
      selected_non_controls.insert(fact->path);
      ++selected;
    }
  }
  std::size_t control_count = 0;
  for (auto *fact : ranked["controls"]) {
    if (control_count >= budgets["controls"].max_files)
      break;
    if (selected_non_controls.contains(fact->path))
      continue;
    fact->promotion_reasons.insert("controls");
    ++control_count;
  }

  nlohmann::json file_facts = nlohmann::json::array();
  for (const auto &[path, fact] : facts)
    file_facts.push_back(
        {{"path", path},
         {"exists_at_head", fact.exists},
         {"file_kind", fact.header ? "header" : "source"},
         {"developer_label", developer_label(options.partition, path)},
         {"direct_touches", fact.touches},
         {"recent_90d_touches", fact.recent_touches},
         {"active_months", fact.active_months.size()},
         {"lines_added", fact.additions},
         {"lines_deleted", fact.deletions},
         {"renames", fact.renames},
         {"moves", fact.moves},
         {"include_fanout", fact.include_fanout},
         {"first_change_time", fact.first_change},
         {"last_change_time", fact.last_change},
         {"total_transitions", fact.transitions.size()},
         {"promotion_reasons", set_json(fact.promotion_reasons)},
         {"screening_label",
          fact.promotion_reasons.empty() ? "screened_not_selected"
                                         : "selected_for_syntax"}});

  nlohmann::json syntax_transitions = nlohmann::json::array();
  nlohmann::json syntax_snapshots = nlohmann::json::array();
  std::map<std::string, std::map<std::string, nlohmann::json>> candidates;
  std::map<std::string, std::size_t> used_transitions;
  std::map<std::string, std::set<std::string>> processed_touch;
  std::map<std::string, nlohmann::json> parsed_endpoints;
  std::map<std::string, nlohmann::json> parsed_blobs;
  std::map<std::string, std::size_t> transition_output_index,
      snapshot_output_index;
  std::size_t syntax_cache_hits = 0, persistent_syntax_cache_hits = 0;
  const auto syntax_cache_root = options.output / "syntax-cache-v1";
  const auto parse_endpoint = [&](const std::string &revision,
                                  const std::string &path)
      -> const nlohmann::json & {
    const auto key = revision + "\n" + path;
    if (const auto found = parsed_endpoints.find(key);
        found != parsed_endpoints.end())
      return found->second;
    nlohmann::json parsed = nlohmann::json::object();
    if (const auto source = blob(options.repository, revision, path)) {
      const auto language = screening_language(path);
      const auto blob_key = language + "\n" + path + "\n" +
                            stable_hash(*source) + "\n" +
                            std::string(kParserIdentity);
      if (const auto found = parsed_blobs.find(blob_key);
          found != parsed_blobs.end()) {
        parsed = found->second;
        parsed["revision"] = revision;
        ++syntax_cache_hits;
      } else {
        const auto cache_id = stable_hash(blob_key);
        const auto cache_path = syntax_cache_root / cache_id.substr(0, 2) /
                                ("syntax-" + cache_id + ".v1.json");
        if (std::filesystem::exists(cache_path)) {
          std::ifstream input(cache_path);
          if (input)
            parsed = nlohmann::json::parse(input, nullptr, false);
          if (!parsed.is_discarded() &&
              parsed.value("parser_identity", std::string{}) ==
                  kParserIdentity &&
              parsed.value("source_fingerprint", std::string{}) ==
                  stable_hash(*source) &&
              parsed.value("language", std::string{}) == language &&
              parsed.value("path", std::string{}) == path) {
            parsed["revision"] = revision;
            ++syntax_cache_hits;
            ++persistent_syntax_cache_hits;
          } else {
            parsed = nlohmann::json::object();
          }
        }
        if (parsed.empty()) {
          parsed = parse_syntax_blob(language, path, revision, *source);
          std::filesystem::create_directories(cache_path.parent_path());
          std::ofstream output(cache_path, std::ios::binary);
          output << canonical_json(parsed);
          if (!output)
            throw std::runtime_error("cannot persist syntax blob cache");
        }
        parsed_blobs.emplace(blob_key, parsed);
      }
    }
    return parsed_endpoints.emplace(key, std::move(parsed)).first->second;
  };
  bool syntax_complete = true;
  for (const auto &name : {"common_leakage", "variable_detail",
                           "stable_island_candidates", "high_impact_headers",
                           "controls"}) {
    for (auto *fact : ranked[name]) {
      if (!fact->promotion_reasons.contains(name))
        continue;
      for (auto touch = fact->transitions.rbegin();
           touch != fact->transitions.rend(); ++touch) {
        const auto touch_key = touch->after_revision + "\n" + fact->path;
        if (processed_touch[name].contains(touch_key))
          continue;
        if (used_transitions[name] >= budgets[name].max_syntax_transitions) {
          syntax_complete = false;
          break;
        }
        processed_touch[name].insert(touch_key);
        ++used_transitions[name];
        const auto &before =
            parse_endpoint(touch->before_revision, touch->before_path);
        const auto &after =
            parse_endpoint(touch->after_revision, touch->after_path);
        auto ranges = changed_ranges(options.repository, *touch);
        nlohmann::json mapped = nlohmann::json::array();
        for (const auto &range : ranges) {
          const auto old_site =
              before.contains("sites")
                  ? mapped_site(before.at("sites"),
                                range.value("before_start", 0ULL),
                                range.value("before_count", 0ULL))
                  : nlohmann::json{{"state", "absent_endpoint"}};
          const auto new_site =
              after.contains("sites")
                  ? mapped_site(after.at("sites"),
                                range.value("after_start", 0ULL),
                                range.value("after_count", 0ULL))
                  : nlohmann::json{{"state", "absent_endpoint"}};
          mapped.push_back({{"range", range},
                            {"before_site", old_site},
                            {"after_site", new_site}});
          for (const auto *site : {&old_site, &new_site}) {
            const auto id = site->value("syntactic_symbol_id", std::string{});
            if (id.empty())
              continue;
            auto &candidate = candidates[name][id];
            if (candidate.empty()) {
              candidate = *site;
              candidate["touches"] = 0;
              candidate["revisions"] = nlohmann::json::array();
              candidate["promotion_reason"] = name;
            }
            candidate["touches"] = candidate.value("touches", 0U) + 1;
            candidate["revisions"].push_back(touch->before_revision);
            candidate["revisions"].push_back(touch->after_revision);
          }
        }
        const auto output_key = touch->before_revision + "\n" +
                                touch->after_revision + "\n" +
                                touch->before_path + "\n" + touch->after_path;
        if (const auto existing = transition_output_index.find(output_key);
            existing != transition_output_index.end()) {
          syntax_transitions[existing->second]["strata"].push_back(name);
        } else {
          transition_output_index[output_key] = syntax_transitions.size();
          syntax_transitions.push_back(
              {{"strata", nlohmann::json::array({name})},
               {"before_revision", touch->before_revision},
               {"after_revision", touch->after_revision},
               {"before_path", touch->before_path},
               {"after_path", touch->after_path},
               {"status", touch->status},
               {"ranges", ranges},
               {"mapped_regions", mapped},
               {"before_coverage",
                before.value("coverage", nlohmann::json{})},
               {"after_coverage",
                after.value("coverage", nlohmann::json{})}});
        }
      }
    }
  }

  // A selected file may be unchanged throughout the window. Parse its current
  // blob once so control samples and stable-island candidates still have sites
  // that can be promoted for semantic observation.
  for (const auto &name : {"common_leakage", "variable_detail",
                           "stable_island_candidates", "high_impact_headers",
                           "controls"}) {
    std::set<std::string> seen_paths;
    for (auto *fact : ranked[name]) {
      if (!fact->promotion_reasons.contains(name) ||
          !seen_paths.insert(fact->path).second)
        continue;
      const auto &snapshot =
          parse_endpoint(options.revisions.back(), fact->path);
      if (!snapshot.contains("sites"))
        continue;
      if (snapshot.at("coverage").value("status", std::string{}) !=
          "complete")
        syntax_complete = false;
      for (const auto &site : snapshot.at("sites")) {
        const auto id = site.at("syntactic_symbol_id").get<std::string>();
        auto &candidate = candidates[name][id];
        if (candidate.empty()) {
          candidate = site;
          candidate["touches"] = 0;
          candidate["revisions"] =
              nlohmann::json::array({options.revisions.back()});
          candidate["promotion_reason"] = name;
        }
      }
      const auto output_key = options.revisions.back() + "\n" + fact->path;
      if (const auto existing = snapshot_output_index.find(output_key);
          existing != snapshot_output_index.end()) {
        syntax_snapshots[existing->second]["strata"].push_back(name);
      } else {
        snapshot_output_index[output_key] = syntax_snapshots.size();
        syntax_snapshots.push_back(
            {{"strata", nlohmann::json::array({name})},
             {"revision", options.revisions.back()},
             {"path", fact->path},
             {"coverage", snapshot.at("coverage")},
             {"site_count", snapshot.at("sites").size()},
             {"sites", snapshot.at("sites")}});
      }
    }
  }

  std::map<std::string, nlohmann::json> promoted_by_id;
  std::map<std::string, std::size_t> promoted_by_stratum;
  std::set<std::string> promoted_paths;
  for (const auto &name : {"common_leakage", "variable_detail",
                           "stable_island_candidates", "high_impact_headers",
                           "controls"}) {
    std::vector<nlohmann::json> values;
    for (auto &[id, candidate] : candidates[name])
      values.push_back(std::move(candidate));
    std::sort(values.begin(), values.end(), [](const auto &left,
                                               const auto &right) {
      return std::tie(left.at("touches"), left.at("qualified_name"),
                      left.at("syntactic_symbol_id")) >
             std::tie(right.at("touches"), right.at("qualified_name"),
                      right.at("syntactic_symbol_id"));
    });
    if (values.size() > budgets[name].max_semantic_elements)
      values.resize(budgets[name].max_semantic_elements);
    promoted_by_stratum[name] = values.size();
    for (auto &candidate : values) {
      candidate["semantic_state"] = "promoted_for_clang_confirmation";
      promoted_paths.insert(candidate.value("path", std::string{}));
      const auto id = candidate.at("syntactic_symbol_id").get<std::string>();
      if (auto found = promoted_by_id.find(id); found != promoted_by_id.end()) {
        found->second["promotion_reasons"].push_back(name);
        found->second["touches"] =
            std::max(found->second.value("touches", 0U),
                     candidate.value("touches", 0U));
      } else {
        candidate.erase("promotion_reason");
        candidate["promotion_reasons"] = nlohmann::json::array({name});
        promoted_by_id.emplace(id, std::move(candidate));
      }
    }
  }
  nlohmann::json promoted = nlohmann::json::array();
  for (auto &[id, candidate] : promoted_by_id) {
    (void)id;
    promoted.push_back(std::move(candidate));
  }
  std::vector<std::string> semantic_revisions;
  // Complete observations, including unchanged revisions, are required for a
  // classifier denominator. A capture cap may select only the newest suffix,
  // but that suffix is then explicitly incomplete and cannot be classified.
  for (auto it = options.revisions.rbegin();
       !promoted.empty() && it != options.revisions.rend(); ++it)
    if (semantic_revisions.size() <
        options.budget.at("max_capture_revisions").get<std::size_t>())
      semantic_revisions.push_back(*it);
  std::reverse(semantic_revisions.begin(), semantic_revisions.end());

  nlohmann::json usage = nlohmann::json::object();
  for (const auto &[name, value] : budgets)
    usage[name] =
        {{"selected_files",
          std::count_if(facts.begin(), facts.end(), [&](const auto &item) {
            return item.second.promotion_reasons.contains(name);
          })},
         {"syntax_transitions", used_transitions[name]},
         {"semantic_elements", promoted_by_stratum[name]},
         {"caps", options.budget.at(name)}};
  usage["capture_revisions"] = semantic_revisions.size();
  usage["capture_revision_cap"] = options.budget.at("max_capture_revisions");
  usage["syntax_blob_cache_hits"] = syntax_cache_hits;
  usage["persistent_syntax_cache_hits"] = persistent_syntax_cache_hits;
  usage["unique_syntax_blobs"] = parsed_blobs.size();

  nlohmann::json gaps = nlohmann::json::array();
  if (!syntax_complete)
    gaps.push_back({{"kind", "syntax_budget_exhausted"},
                    {"effect", "semantic_history_incomplete"}});
  if (!promoted.empty() && options.revisions.size() > semantic_revisions.size())
    gaps.push_back({{"kind", "capture_revision_budget_exhausted"},
                    {"required", options.revisions.size()},
                    {"observed", semantic_revisions.size()},
                    {"effect", "build_context_unobserved"}});
  return {{"schema_version", 1},
          {"artifact_version", 1},
          {"record_type", "progressive_screening"},
          {"parser_identity", kParserIdentity},
          {"screening", {{"files", file_facts}}},
          {"syntax", {{"transitions", syntax_transitions},
                       {"snapshots", syntax_snapshots}}},
          {"promotion", {{"elements", promoted},
                          {"paths", promoted_paths},
                          {"semantic_revisions", semantic_revisions}}},
          {"coverage",
           {{"status", gaps.empty() ? "complete" : "partial"},
            {"history_complete", gaps.empty()},
            {"canonical_identity", false}}},
          {"budget_usage", usage},
          {"evidence_gaps", gaps}};
}

} // namespace history
