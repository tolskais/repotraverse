#include "history/catalog.hpp"
#include "history/file_history.hpp"
#include "history/process.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string git(const std::filesystem::path &repo,
                std::vector<std::string> args) {
  std::vector<std::string> command = {"git", "-C", repo.string()};
  command.insert(command.end(), args.begin(), args.end());
  auto result = history::run_process(command);
  if (result.exit_code)
    throw std::runtime_error(result.error);
  while (!result.output.empty() &&
         (result.output.back() == '\n' || result.output.back() == '\r'))
    result.output.pop_back();
  return result.output;
}
void write(const std::filesystem::path &path, const std::string &value) {
  std::ofstream out(path);
  out << value;
}
history::CompileContext context(const std::string &revision,
                                const std::string &path,
                                const std::string &config,
                                const std::string &semantic) {
  history::CompileContext value;
  value.source_revision = revision;
  value.translation_unit = path;
  value.configuration = config;
  value.context_id = history::stable_hash(path + semantic);
  value.toolchain = "armclang6";
  value.adapter_version = "test";
  return value;
}
} // namespace
int main() {
  try {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("repotraverse-file-history-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
      std::filesystem::path p;
      ~Cleanup() {
        std::error_code e;
        std::filesystem::remove_all(p, e);
      }
    } cleanup{root};
    const auto repo = root / "source";
    std::filesystem::create_directories(repo);
    git(repo, {"init"});
    git(repo, {"config", "user.name", "Test"});
    git(repo, {"config", "user.email", "test@example.invalid"});
    write(repo / "old.cpp", "int f(){return 1;}\n");
    git(repo, {"add", "old.cpp"});
    git(repo, {"commit", "-m", "add"});
    const auto first = git(repo, {"rev-parse", "HEAD"});
    git(repo, {"mv", "old.cpp", "new.cpp"});
    git(repo, {"commit", "-m", "rename"});
    const auto second = git(repo, {"rev-parse", "HEAD"});
    write(repo / "new.cpp", "int f(){return 2;}\n");
    git(repo, {"add", "new.cpp"});
    git(repo, {"commit", "-m", "change"});
    const auto third = git(repo, {"rev-parse", "HEAD"});
    history::Catalog catalog(root / "catalog");
    for (const auto &c : {context(first, "old.cpp", "debug", "same"),
                          context(first, "old.cpp", "release", "same"),
                          context(second, "new.cpp", "debug", "same"),
                          context(second, "new.cpp", "release", "same"),
                          context(third, "new.cpp", "debug", "changed"),
                          context(third, "new.cpp", "release", "changed")})
      catalog.store_compile_context(c);
    history::FileHistoryOptions options;
    options.repository = repo;
    options.path = "new.cpp";
    options.since = 1;
    const auto result = history::plan_file_history(catalog, options);
    require(result.at("change_unit_count") == 3, "file changes missing");
    require(result.at("historical_path") == "old.cpp", "rename chain missing");
    require(result.at("path_segments").size() == 1, "rename segment missing");
    require(!result.at("change_units").back().at("changed_ranges").empty(),
            "zero-context changed ranges missing");
    require(result.at("scheduled_tasks") == 3,
            "equivalent configurations or endpoints were not deduplicated");
    require(result.at("coverage").at("status") == "partial" &&
                result.at("coverage").at("gaps").empty(),
            "pending extraction coverage was not distinguished from gaps");
    for (const auto &transition : result.at("analysis").at("transitions"))
      require(transition.at("state") == "pending" &&
                  transition.at("element_changes").empty(),
              "missing endpoints were reported as semantic changes");
    for (const auto &task :
         catalog.pending_tasks(result.at("request_id").get<std::string>())) {
      history::TuManifest manifest;
      manifest.source_revision = task.at("source_commit");
      manifest.translation_unit = task.at("translation_unit");
      manifest.source_blob = "fixture-blob";
      manifest.context_id = task.at("context_id");
      const auto path = task.at("requested_file").get<std::string>();
      history::LogicalElement element;
      element.element_id = "element-f";
      element.compiler_id = "compiler-f";
      element.kind = "function";
      element.qualified_name = "f";
      element.linkage = "external";
      element.owner_file = path;
      history::SemanticVariant variant;
      variant.element_id = element.element_id;
      variant.interface_fingerprint = "interface";
      variant.implementation_fingerprint = manifest.source_revision == third
                                               ? "implementation-2"
                                               : "implementation-1";
      variant.dependency_fingerprint = "dependencies";
      variant.variant_id = history::stable_hash(
          variant.element_id + variant.implementation_fingerprint);
      history::SourceAnchor location;
      location.path = path;
      location.begin_line = 1;
      location.end_line = 1;
      manifest.elements = {element};
      manifest.variants = {variant};
      manifest.observations = {
          {element.element_id, variant.variant_id, location, {}}};
      manifest.manifest_id =
          history::stable_hash(manifest.source_revision + manifest.context_id);
      const auto task_id = task.at("task_id").get<std::string>();
      const nlohmann::json fact = {
          {"schema_version", history::kSchemaVersion},
          {"record_type", "extraction_result"},
          {"fact_id", history::stable_hash(task_id + "fact")},
          {"task_id", task_id},
          {"result", manifest}};
      catalog.store_fact(fact.at("fact_id"), task_id, fact, "fixture");
    }
    const auto completed = history::plan_file_history(catalog, options);
    require(completed.at("result_status") == "complete",
            "completed facts still appear pending");
    require(completed.at("analysis").at("element_snapshots").size() == 3,
            "endpoint snapshots were not deduplicated");
    bool implementation_change = false;
    for (const auto &transition : completed.at("analysis").at("transitions"))
      for (const auto &change : transition.at("element_changes"))
        if (change.value("change_kind", std::string{}) == "modified" &&
            std::find(change.at("semantic_dimensions").begin(),
                      change.at("semantic_dimensions").end(),
                      "implementation") !=
                change.at("semantic_dimensions").end())
          implementation_change = true;
    require(implementation_change,
            "implementation-only semantic change was not summarized");
    std::cout << "file history tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
