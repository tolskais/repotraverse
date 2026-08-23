#include "history/process.hpp"
#include "history/progressive.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void write(const std::filesystem::path &path, std::string_view value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << value;
}

std::string git(const std::filesystem::path &repository,
                std::vector<std::string> arguments) {
  std::vector<std::string> command = {GIT_PATH, "-C", repository.string()};
  command.insert(command.end(), arguments.begin(), arguments.end());
  auto result = history::run_process(command);
  if (result.exit_code != 0)
    throw std::runtime_error("fixture git command failed: " + result.error);
  while (!result.output.empty() &&
         (result.output.back() == '\n' || result.output.back() == '\r'))
    result.output.pop_back();
  return result.output;
}

nlohmann::json budget(std::size_t transitions = 20) {
  nlohmann::json result;
  for (const auto &name :
       {"common_leakage", "variable_detail", "stable_island_candidates",
        "high_impact_headers", "controls"})
    result[name] = {{"max_files", 10},
                    {"max_syntax_transitions", transitions},
                    {"max_semantic_elements", 10}};
  result["max_capture_revisions"] = 10;
  result["max_dependency_depth"] = 2;
  result["max_induced_elements_per_transition"] = 100;
  return result;
}

bool has_gap(const nlohmann::json &result, std::string_view kind) {
  return std::any_of(result.at("evidence_gaps").begin(),
                     result.at("evidence_gaps").end(),
                     [&](const auto &gap) { return gap.at("kind") == kind; });
}

} // namespace

int main() {
  try {
    const auto parsed = history::parse_syntax_blob(
        "cpp", "src/parser.cpp", "r1",
        "namespace app { class Parser { public: int parse(int x); }; "
        "int Parser::parse(int x) { return x + 1; } }\n");
    require(parsed.at("coverage").at("status") == "complete",
            "valid C++ syntax was not parsed completely");
    bool definition = false, declaration = false, record = false;
    for (const auto &site : parsed.at("sites")) {
      definition = definition || site.at("kind") == "function_definition";
      declaration = declaration || site.at("kind") == "function_declaration";
      record = record || site.at("kind") == "record";
      require(site.at("identity_kind") == "syntactic_candidate",
              "syntax site was presented as canonical identity");
    }
    require(definition && declaration && record,
            "Tree-sitter did not expose expected C++ sites");

    const auto root =
        std::filesystem::temp_directory_path() /
        ("repotraverse-progressive-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    } cleanup{root};
    const auto repository = root / "repository";
    std::filesystem::create_directories(repository);
    git(repository, {"init", "-b", "main"});
    git(repository, {"config", "user.name", "Test"});
    git(repository, {"config", "user.email", "test@example.invalid"});
    write(repository / "src/common.cpp", "int stable(){return 1;}\n");
    write(repository / "src/variable.cpp", "int process(){return 1;}\n");
    write(repository / "include/config.hpp", "#define VALUE 1\n");
    git(repository, {"add", "."});
    git(repository, {"commit", "-m", "initial"});
    const auto first = git(repository, {"rev-parse", "HEAD"});
    write(repository / "src/variable.cpp", "int process(){return 2;}\n");
    write(repository / "include/config.hpp", "#define VALUE 2\n");
    git(repository, {"add", "."});
    git(repository, {"commit", "-m", "change variable"});
    const auto second = git(repository, {"rev-parse", "HEAD"});
    write(repository / "src/variable.cpp", "int process(){return 3;}\n");
    git(repository, {"add", "."});
    git(repository, {"commit", "-m", "change variable again"});
    const auto third = git(repository, {"rev-parse", "HEAD"});

    history::ProgressiveScreeningOptions options;
    options.repository = repository;
    options.output = root / "output";
    options.revisions = {first, second, third};
    options.partition = {{"stable", {"src/common.cpp"}},
                         {"variable", {"src/variable.cpp"}}};
    options.budget = budget();
    const auto result = history::plan_progressive_screening(options);
    require(result.at("record_type") == "progressive_screening",
            "progressive report type is missing");
    require(!result.at("syntax").at("transitions").empty(),
            "selected source transitions were not syntax-mapped");
    require(!result.at("promotion").at("elements").empty(),
            "changed symbols were not promoted");
    require(result.at("promotion")
                .at("paths")
                .get<std::set<std::string>>()
                .contains("src/variable.cpp"),
            "variable source was not promoted");
    require(!std::filesystem::exists(options.output / "worktrees") &&
                std::filesystem::exists(options.output / "syntax-cache-v1"),
            "syntax screening materialized a worktree or omitted its cache");
    require(result.at("budget_usage").value("syntax_blob_cache_hits", 0U) > 0,
            "identical endpoint blobs were reparsed instead of reused");
    std::set<std::string> promoted_ids;
    for (const auto &candidate : result.at("promotion").at("elements"))
      require(
          promoted_ids
              .insert(candidate.at("syntactic_symbol_id").get<std::string>())
              .second,
          "one syntax site was duplicated across promotion strata");

    const auto cached = history::plan_progressive_screening(options);
    require(cached.at("budget_usage").value("screening_plan_cache_hit", false),
            "an identical screening plan repeated Git history work");

    auto capped = options;
    capped.budget = budget(0);
    const auto partial = history::plan_progressive_screening(capped);
    require(partial.at("coverage").at("status") == "partial" &&
                !partial.at("promotion").at("elements").empty() &&
                !partial.at("evidence_gaps").empty(),
            "syntax cap exhaustion was not preserved as partial evidence");
    require(
        partial.at("budget_usage").value("persistent_syntax_cache_hits", 0U) >
            0,
        "a repeated screening run did not reuse persisted syntax facts");

    auto file_capped = options;
    for (const auto &name :
         {"common_leakage", "variable_detail", "stable_island_candidates",
          "high_impact_headers", "controls"})
      file_capped.budget[name]["max_files"] = 0;
    const auto file_partial = history::plan_progressive_screening(file_capped);
    require(file_partial.at("coverage").at("status") == "partial" &&
                has_gap(file_partial, "file_budget_exhausted"),
            "file cap exhaustion was reported as complete coverage");

    auto element_capped = options;
    for (const auto &name :
         {"common_leakage", "variable_detail", "stable_island_candidates",
          "high_impact_headers", "controls"})
      element_capped.budget[name]["max_semantic_elements"] = 0;
    const auto element_partial =
        history::plan_progressive_screening(element_capped);
    require(
        element_partial.at("coverage").at("status") == "partial" &&
            has_gap(element_partial, "semantic_element_budget_exhausted"),
        "semantic element cap exhaustion was reported as complete coverage");

    bool rejected = false;
    try {
      auto missing = options;
      missing.budget = nlohmann::json::object();
      (void)history::plan_progressive_screening(missing);
    } catch (const std::exception &) {
      rejected = true;
    }
    require(rejected, "missing required progressive caps were accepted");

    std::cout << "progressive tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
