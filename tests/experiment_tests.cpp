#include "history/experiment.hpp"
#include "history/ir.hpp"
#include "history/process.hpp"
#include "history/stability.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void write(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
}

history::EvidenceBundle bundle(std::string revision, bool changed) {
  history::EvidenceBundle result;
  result.source_revision = std::move(revision);
  result.configuration = "debug";
  result.coverage.capabilities = {"element_lineage"};
  history::ElementSnapshot stable;
  stable.compiler_id = "stable-id";
  stable.kind = "function";
  stable.qualified_name = "stable_function";
  stable.interface_fingerprint = "interface";
  stable.implementation_fingerprint = "implementation";
  stable.dependency_fingerprint = "dependencies";
  stable.location.path = "stable/stable.cpp";
  history::ElementSnapshot variable = stable;
  variable.compiler_id = "variable-id";
  variable.qualified_name = "variable_function";
  variable.location.path = "variable/variable.cpp";
  variable.implementation_fingerprint = changed ? result.source_revision : "base";
  result.elements = {stable, variable};
  return result;
}
} // namespace

int main() {
  try {
    const auto root = std::filesystem::temp_directory_path() /
                      ("repotraverse-experiment-" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    } cleanup{root};
    const auto repository = root / "repository";
    write(repository / "include/config.hpp", "#define VALUE 7\n");
    write(repository / "src/sample.cpp",
          "#include \"config.hpp\"\nint sample() { return VALUE; }\n");
    require(history::run_process({GIT_PATH, "init", "-b", "main"}, repository)
                    .exit_code == 0,
            "cannot initialize experiment repository");
    require(history::run_process(
                {GIT_PATH, "-C", repository.string(), "-c", "user.name=Test",
                 "-c", "user.email=test@example.invalid", "add", "."})
                    .exit_code == 0,
            "cannot stage experiment repository");
    require(history::run_process(
                {GIT_PATH, "-C", repository.string(), "-c", "user.name=Test",
                 "-c", "user.email=test@example.invalid", "commit", "-m", "base"})
                    .exit_code == 0,
            "cannot commit experiment repository");
    auto revision = history::run_process(
        {GIT_PATH, "-C", repository.string(), "rev-parse", "HEAD"});
    while (!revision.output.empty() &&
           (revision.output.back() == '\n' || revision.output.back() == '\r'))
      revision.output.pop_back();
    const auto experiment_manifest = root / "head.json";
    const nlohmann::json experiment = {
        {"schema_version", history::kSchemaVersion},
        {"repository", repository.string()},
        {"repository_id", "fixture"},
        {"revision", revision.output},
        {"output", (root / "head-output").string()},
        {"artifact_cache", (root / "artifact-cache-v1").string()},
        {"extractor", EXTRACTOR_PATH},
        {"configurations",
         nlohmann::json::array(
             {{{"name", "debug"},
               {"toolchain", "armclang6"},
               {"real_compiler", FAKE_ARM_COMPILER_PATH},
               {"command", {FAKE_MAKE_PATH}}}})}};
    write(experiment_manifest, experiment.dump());
    const auto head = history::run_head_experiment(experiment_manifest, PROBE_PATH);
    require(head.at("translation_unit_contexts") == 1,
            "HEAD experiment did not extract captured TU");
    require(head.at("extracted_elements").get<std::uint64_t>() > 0,
            "HEAD experiment did not extract elements");
    require(head.at("states").value("complete", 0U) == 1,
            "captured dependency map did not produce complete coverage");

    for (int index = 1; index <= 3; ++index) {
      write(repository / "README.md", "documentation " + std::to_string(index));
      require(history::run_process(
                  {GIT_PATH, "-C", repository.string(), "add", "README.md"})
                      .exit_code == 0,
              "cannot stage pilot revision");
      require(history::run_process(
                  {GIT_PATH, "-C", repository.string(), "-c", "user.name=Test",
                   "-c", "user.email=test@example.invalid", "commit", "-m",
                   "documentation"})
                      .exit_code == 0,
              "cannot commit pilot revision");
    }
    auto pilot_experiment = experiment;
    pilot_experiment["revision"] = "main";
    pilot_experiment["output"] = (root / "pilot-output").string();
    pilot_experiment["pilot"] = {{"ref", "main"}, {"max_revisions", 4}};
    pilot_experiment["partition"] = {{"stable", {"src/*", "include/*"}},
                                      {"variable", nlohmann::json::array()}};
    const auto pilot_manifest = root / "pilot.json";
    write(pilot_manifest, pilot_experiment.dump());
    const auto pilot =
        history::run_pilot_experiment(pilot_manifest, PROBE_PATH);
    require(pilot.at("series").size() == 1 &&
                pilot.at("series").front().at("bundles").size() == 4,
            "pilot did not create an ordered four-revision series");
    require(pilot.at("revision_reports").back().at("cache_hits") == 1,
            "pilot did not reuse an unaffected TU manifest");
    require(pilot.at("revision_reports").back().at("capture").at("reused") ==
                true,
            "pilot did not reuse an unchanged build context");
    require(pilot.at("revision_reports").front().at("units").front().at(
                "cache_source") == "content_addressed",
            "pilot did not reuse the content-addressed HEAD artifact");
    require(pilot.at("stability").at("classifications").value("stable", 0U) > 0,
            "pilot did not classify unchanged elements as stable");

    nlohmann::json paths = nlohmann::json::array();
    for (int index = 0; index < 4; ++index) {
      const auto path = root / ("bundle-" + std::to_string(index) + ".json");
      write(path, nlohmann::json(bundle("r" + std::to_string(index), index > 0)).dump());
      paths.push_back(path.string());
    }
    const auto stability_manifest = root / "stability.json";
    write(stability_manifest,
          nlohmann::json(
              {{"schema_version", history::kSchemaVersion},
               {"series", {{{"configuration", "debug"}, {"bundles", paths}}}},
               {"partition",
                {{"stable", {"stable/*"}}, {"variable", {"variable/*"}}}}})
              .dump());
    const auto stability =
        history::run_stability_experiment(stability_manifest);
    require(stability.at("classifications").value("stable", 0U) == 1,
            "stable element was not classified");
    require(stability.at("classifications").value("variable", 0U) == 1,
            "variable element was not classified");
    require(stability.at("variation_leakage").empty() &&
                stability.at("stable_islands").empty(),
            "matching developer partition reported disagreements");
    std::cout << "experiment tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
