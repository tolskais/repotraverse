#include "history/change_evidence.hpp"
#include "history/ir.hpp"
#include "history/process.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void write(const std::filesystem::path &path, const nlohmann::json &value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << value.dump();
}

void write_text(const std::filesystem::path &path, std::string_view value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << value;
}

std::string git(const std::filesystem::path &repository,
                std::vector<std::string> arguments) {
  std::vector<std::string> command = {GIT_PATH, "-C", repository.string()};
  command.insert(command.end(), arguments.begin(), arguments.end());
  auto result = history::run_process(command);
  if (result.exit_code != 0)
    throw std::runtime_error(result.error);
  while (!result.output.empty() &&
         (result.output.back() == '\n' || result.output.back() == '\r'))
    result.output.pop_back();
  return result.output;
}

history::TuManifest manifest(const std::string &revision,
                             const std::string &implementation,
                             const std::string &interface) {
  history::TuManifest result;
  result.source_revision = revision;
  result.translation_unit = "src/main.cpp";
  result.context_id = "same-context";
  result.configuration = "debug";
  result.build_variant.variant_id = "host-debug";
  history::LogicalElement caller;
  caller.element_id = "caller";
  caller.compiler_id = "caller-compiler";
  caller.qualified_name = "run";
  history::LogicalElement api;
  api.element_id = "api";
  api.compiler_id = "api-compiler";
  api.qualified_name = "api";
  history::SemanticVariant caller_variant;
  caller_variant.variant_id = "caller-variant-" + revision;
  caller_variant.element_id = caller.element_id;
  caller_variant.interface_fingerprint = "caller-interface";
  caller_variant.implementation_fingerprint = implementation;
  caller_variant.dependency_fingerprint = "caller-dependency-" + revision;
  caller_variant.referenced_element_ids = {api.element_id};
  history::SemanticVariant api_variant;
  api_variant.variant_id = "api-variant-" + revision;
  api_variant.element_id = api.element_id;
  api_variant.interface_fingerprint = interface;
  api_variant.implementation_fingerprint = "api-body";
  api_variant.dependency_fingerprint = "none";
  history::SourceAnchor definition;
  definition.path = "src/main.cpp";
  definition.role = "definition";
  history::SourceAnchor declaration;
  declaration.path = "include/api.hpp";
  declaration.role = "declaration";
  result.elements = {caller, api};
  result.variants = {caller_variant, api_variant};
  result.observations = {{caller.element_id, caller_variant.variant_id,
                          definition, {}},
                         {api.element_id, api_variant.variant_id, declaration, {}}};
  return result;
}

} // namespace

int main() {
  try {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("repotraverse-change-evidence-" +
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
    write_text(repository / "include/api.hpp", "int api(int);\n");
    write_text(repository / "src/main.cpp", "int run(){return api(1);}\n");
    git(repository, {"add", "."});
    git(repository, {"commit", "-m", "base"});
    const auto first = git(repository, {"rev-parse", "HEAD"});
    write_text(repository / "include/api.hpp", "long api(long);\n");
    git(repository, {"add", "."});
    git(repository, {"commit", "-m", "api change"});
    const auto second = git(repository, {"rev-parse", "HEAD"});

    nlohmann::json reports = nlohmann::json::array();
    for (const auto &[index, revision] :
         std::vector<std::pair<int, std::string>>{{0, first}, {1, second}}) {
      const auto output = root / ("revision-" + std::to_string(index));
      write(output / "capture/0.json",
            {{"translation_unit", "src/main.cpp"},
             {"project_files", {"src/main.cpp", "include/api.hpp"}}});
      write(output / "manifests/debug.json",
            manifest(revision, index == 0 ? "caller-before" : "caller-after",
                     index == 0 ? "api-before" : "api-after"));
      reports.push_back({{"revision", revision}, {"output", output.string()}});
    }
    const nlohmann::json budget = {
        {"max_dependency_depth", 2},
        {"max_induced_elements_per_transition", 20}};
    const auto evidence =
        history::summarize_change_evidence(repository, reports, budget);
    require(evidence.at("origin_counts").value("own_declaration", 0U) == 1,
            "own header declaration was not intrinsic evidence");
    require(evidence.at("origin_counts").value("confirmed_induced", 0U) >= 1,
            "dependent semantic change was not confirmed as induced");
    bool header_cause = false;
    for (const auto &fact :
         evidence.at("transitions").front().at("facts"))
      if (fact.at("origin") == "confirmed_induced" &&
          fact.at("causes").get<std::set<std::string>>().contains(
              "include/api.hpp"))
        header_cause = true;
    require(header_cause, "induced evidence did not retain its causal set");
    std::cout << "change evidence tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
