#include "history/experiment.hpp"

#include "history/build_import.hpp"
#include "history/catalog.hpp"
#include "history/change_evidence.hpp"
#include "history/encoding.hpp"
#include "history/ir.hpp"
#include "history/process.hpp"
#include "history/progressive.hpp"
#include "history/revision_workspace.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

namespace history {
namespace {

nlohmann::json read_json(const std::filesystem::path &path) {
  return nlohmann::json::parse(read_text_file(path).text);
}

std::filesystem::path canonical_directory(const nlohmann::json &manifest,
                                          const char *field) {
  const auto path =
      path_from_utf8(manifest.at(field).get<std::string>());
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(path, error);
  if (error || !std::filesystem::is_directory(canonical))
    throw std::runtime_error(std::string(field) +
                             " must be an existing directory");
  return canonical;
}

void validate_manifest(const nlohmann::json &manifest) {
  if (!manifest.is_object() ||
      manifest.value("schema_version", 0U) != kSchemaVersion)
    throw std::runtime_error("experiment manifest requires schema_version 1");
  for (const auto *field :
       {"repository", "revision", "repository_id", "output", "configurations"})
    if (!manifest.contains(field))
      throw std::runtime_error(std::string("experiment manifest requires ") +
                               field);
  if (!manifest.at("configurations").is_array() ||
      manifest.at("configurations").empty())
    throw std::runtime_error("experiment requires at least one configuration");
}

std::vector<nlohmann::json>
captured_records(const std::filesystem::path &directory) {
  std::vector<std::filesystem::path> files;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(directory))
    if (entry.is_regular_file() && entry.path().extension() == ".json")
      files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  std::vector<nlohmann::json> records;
  for (const auto &file : files) {
    std::ifstream input(file);
    records.push_back(nlohmann::json::parse(input));
  }
  return records;
}

MaterializationManifest
captured_materialization(const std::filesystem::path &directory) {
  std::set<std::string> files;
  bool complete = true;
  for (const auto &record : captured_records(directory)) {
    const auto translation_unit =
        record.value("translation_unit", std::string{});
    if (translation_unit.empty())
      continue;
    files.insert(translation_unit);
    const auto project_files =
        record.value("project_files", std::vector<std::string>{});
    if (project_files.empty())
      complete = false;
    files.insert(project_files.begin(), project_files.end());
  }
  MaterializationManifest result;
  result.files.assign(files.begin(), files.end());
  result.closure_complete = complete && !result.files.empty();
  if (!result.closure_complete)
    result.evidence_gaps.push_back(
        "captured project dependency closure is unavailable");
  return result;
}

bool requires_repository_preparation(const nlohmann::json &manifest) {
  return std::any_of(manifest.at("configurations").begin(),
                     manifest.at("configurations").end(),
                     [](const auto &configuration) {
                       return configuration.contains("prepare_command") &&
                              !configuration.at("prepare_command").empty();
                     });
}

nlohmann::json capture(const nlohmann::json &manifest,
                       const std::filesystem::path &compiler_probe,
                       Catalog &catalog,
                       const std::filesystem::path &capture_root) {
  const auto repository = canonical_directory(manifest, "repository");
  auto revision = manifest.at("revision").get<std::string>();
  const auto current_revision =
      run_process({"git", "-C", path_to_utf8(repository), "rev-parse", "HEAD"});
  const auto requested_revision = run_process(
      {"git", "-C", path_to_utf8(repository), "rev-parse", revision + "^{commit}"});
  const auto trim = [](std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                              value.back() == ' ' || value.back() == '\t'))
      value.pop_back();
    return value;
  };
  if (current_revision.exit_code != 0 || requested_revision.exit_code != 0 ||
      trim(current_revision.output) != trim(requested_revision.output))
    throw std::runtime_error(
        "capture repository is not checked out at requested revision");
  revision = trim(requested_revision.output);
  nlohmann::json runs = nlohmann::json::array();
  std::size_t failed_runs = 0;
  for (const auto &configuration : manifest.at("configurations")) {
    const auto name = configuration.at("name").get<std::string>();
    const auto toolchain = configuration.at("toolchain").get<std::string>();
    if (name.empty() || (toolchain != "armcc5" && toolchain != "armclang6"))
      throw std::runtime_error("configuration name or toolchain is invalid");
    const auto directory = capture_root / stable_hash(name);
    std::filesystem::create_directories(directory);
    ProcessOptions options;
    auto relative_working_directory =
        path_from_utf8(
            configuration.value("working_directory", std::string{"."}))
            .lexically_normal();
    if (relative_working_directory.is_absolute() ||
        (!relative_working_directory.empty() &&
         *relative_working_directory.begin() == ".."))
      throw std::runtime_error(
          "configuration working_directory must be repository-relative");
    options.working_directory = repository / relative_working_directory;
    if (!std::filesystem::is_directory(options.working_directory))
      throw std::runtime_error(
          "configuration working_directory does not exist");
    options.timeout =
        std::chrono::seconds(configuration.value("timeout_seconds", 1800U));
    options.max_output_bytes = 16ULL * 1024ULL * 1024ULL;
    options.environment = configuration.value(
        "environment", std::map<std::string, std::string>{});
    options.environment["REPOTRAVERSE_CAPTURE_DIRECTORY"] = path_to_utf8(directory);
    options.environment["REPOTRAVERSE_CAPTURE_REPOSITORY"] =
        path_to_utf8(repository);
    options.environment["REPOTRAVERSE_CAPTURE_CONFIGURATION"] = name;
    options.environment["REPOTRAVERSE_CAPTURE_REVISION"] = revision;
    const auto build_variant =
        configuration.value("build_variant", nlohmann::json::object());
    options.environment["REPOTRAVERSE_CAPTURE_PRODUCT"] =
        build_variant.value("product", std::string{"unspecified"});
    options.environment["REPOTRAVERSE_CAPTURE_TARGET"] =
        build_variant.value("target", std::string{"unspecified"});
    options.environment["REPOTRAVERSE_CAPTURE_BUILD_CONFIGURATION"] =
        build_variant.value("configuration", name);
    options.environment["REPOTRAVERSE_CAPTURE_TOOLCHAIN"] = toolchain;
    if (configuration.contains("real_compiler"))
      options.environment["REPOTRAVERSE_REAL_COMPILER"] =
          configuration.at("real_compiler").get<std::string>();
    const auto variables = configuration.value(
        "compiler_variables", std::vector<std::string>{"CC", "CXX"});
    for (const auto &variable : variables) {
      if (variable.empty() || variable.starts_with("REPOTRAVERSE_") ||
          !(std::isalpha(static_cast<unsigned char>(variable.front())) ||
            variable.front() == '_') ||
          !std::all_of(variable.begin() + 1, variable.end(), [](const char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
          }))
        throw std::runtime_error("invalid compiler variable name");
      options.environment[variable] = path_to_utf8(compiler_probe);
    }
    auto command = configuration.at("command").get<std::vector<std::string>>();
    for (auto &argument : command) {
      constexpr std::string_view placeholder = "{compiler_probe}";
      for (std::size_t position = 0;
           (position = argument.find(placeholder, position)) !=
           std::string::npos;) {
        const auto probe_name = path_to_utf8(compiler_probe);
        argument.replace(position, placeholder.size(), probe_name);
        position += probe_name.size();
      }
    }
    const auto started = std::chrono::steady_clock::now();
    const auto process = run_process(command, options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const auto records = captured_records(directory).size();
    if (process.exit_code != 0 || process.timed_out || process.output_truncated)
      ++failed_runs;
    runs.push_back({{"configuration", name},
                    {"toolchain", toolchain},
                    {"exit_code", process.exit_code},
                    {"timed_out", process.timed_out},
                    {"output_truncated", process.output_truncated},
                    {"captured_records", records},
                    {"elapsed_ms", elapsed.count()},
                    {"cpu_time_ms", process.cpu_time_ms},
                    {"diagnostic_fingerprint", stable_hash(process.error)}});
  }
  const auto imported = import_build_log(catalog, capture_root, repository);
  return {{"schema_version", kSchemaVersion},
          {"runs", std::move(runs)},
          {"failed_runs", failed_runs},
          {"import", imported}};
}

void prepare_generated_files(const nlohmann::json &manifest,
                             const std::filesystem::path &repository) {
  for (const auto &configuration : manifest.at("configurations")) {
    if (!configuration.contains("prepare_command"))
      continue;
    auto relative =
        path_from_utf8(
            configuration.value("working_directory", std::string{"."}))
            .lexically_normal();
    if (relative.is_absolute() ||
        (!relative.empty() && *relative.begin() == ".."))
      throw std::runtime_error("invalid prepare working directory");
    ProcessOptions options;
    options.working_directory = repository / relative;
    options.environment = configuration.value(
        "environment", std::map<std::string, std::string>{});
    options.timeout =
        std::chrono::seconds(configuration.value("timeout_seconds", 1800U));
    options.max_output_bytes = 16ULL * 1024ULL * 1024ULL;
    const auto result = run_process(
        configuration.at("prepare_command").get<std::vector<std::string>>(),
        options);
    if (result.exit_code != 0 || result.timed_out || result.output_truncated)
      throw std::runtime_error(
          "generated-file preparation failed for configuration " +
          configuration.at("name").get<std::string>() + ": " +
          stable_hash(result.error));
  }
}

std::filesystem::path prepare_output(const nlohmann::json &manifest) {
  const auto output =
      std::filesystem::absolute(
          path_from_utf8(manifest.at("output").get<std::string>()))
          .lexically_normal();
  if (std::filesystem::exists(output))
    throw std::runtime_error("experiment output already exists: " +
                             path_to_utf8(output));
  std::filesystem::create_directories(output / "capture");
  std::filesystem::create_directories(output / "manifests");
  return output;
}

void persist(const std::filesystem::path &path, const nlohmann::json &value) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error)
    throw std::runtime_error("cannot create experiment artifact directory: " +
                             path_to_utf8(path.parent_path()));
  std::ofstream output(path, std::ios::binary);
  output << canonical_json(value) << '\n';
  if (!output)
    throw std::runtime_error("cannot persist experiment artifact: " +
                             path_to_utf8(path));
}

std::vector<std::string> lines(std::string value) {
  std::vector<std::string> result;
  std::string current;
  for (const auto character : value) {
    if (character == '\n' || character == '\r') {
      if (!current.empty()) {
        result.push_back(std::move(current));
        current.clear();
      }
    } else {
      current.push_back(character);
    }
  }
  if (!current.empty())
    result.push_back(std::move(current));
  return result;
}

void git_success(const ProcessOutput &result, const char *operation) {
  if (result.exit_code != 0 || result.timed_out || result.output_truncated)
    throw std::runtime_error(std::string("git failed while attempting to ") +
                             operation + ": " + stable_hash(result.error));
}

std::string executable_identity(const std::filesystem::path &executable,
                                const char *description) {
  const auto version = run_process({path_to_utf8(executable), "--version"});
  if (version.exit_code != 0 || version.timed_out || version.output_truncated)
    throw std::runtime_error(std::string("cannot identify ") + description);
  return stable_hash(generic_path_to_utf8(executable) + "\n" + version.output);
}

void reuse_capture(const std::filesystem::path &source,
                   const std::filesystem::path &destination,
                   const std::string &revision) {
  std::filesystem::create_directories(destination);
  std::size_t index = 0;
  for (auto record : captured_records(source)) {
    record["source_revision"] = revision;
    persist(destination / (std::to_string(index++) + ".json"), record);
  }
}

std::vector<std::string> changed_paths(const std::filesystem::path &repository,
                                       const std::string &before,
                                       const std::string &after,
                                       const std::string &filter = {}) {
  std::vector<std::string> command = {"git", "-C", path_to_utf8(repository), "diff",
                                      "--name-only"};
  if (!filter.empty())
    command.push_back("--diff-filter=" + filter);
  command.insert(command.end(), {before, after});
  const auto result = run_process(command);
  git_success(result, "inspect pilot changes");
  return lines(result.output);
}

bool source_path(const std::string &path) {
  const auto extension = path_to_utf8(path_from_utf8(path).extension());
  return extension == ".c" || extension == ".cc" || extension == ".cpp" ||
         extension == ".cxx" || extension == ".C";
}

bool path_pattern(std::string_view pattern, std::string_view path) {
  std::vector<bool> previous(path.size() + 1), current(path.size() + 1);
  previous[0] = true;
  for (const auto token : pattern) {
    current.assign(path.size() + 1, false);
    if (token == '*') {
      current[0] = previous[0];
      for (std::size_t index = 1; index <= path.size(); ++index)
        current[index] = previous[index] || current[index - 1];
    } else {
      for (std::size_t index = 1; index <= path.size(); ++index)
        current[index] =
            previous[index - 1] && (token == '?' || token == path[index - 1]);
    }
    previous.swap(current);
  }
  return previous[path.size()];
}

bool build_context_changed(const std::filesystem::path &repository,
                           const std::string &before, const std::string &after,
                           const nlohmann::json &pilot) {
  for (const auto &path : changed_paths(repository, before, after, "ADR"))
    if (source_path(path))
      return true;
  const auto patterns = pilot.value(
      "build_files", std::vector<std::string>{"Makefile", "makefile", "*.mk",
                                              "config", "toolchain"});
  const auto changed = changed_paths(repository, before, after);
  for (const auto &path : changed)
    for (const auto &pattern : patterns)
      if (path == pattern || path.starts_with(pattern + "/") ||
          path_pattern(pattern, path))
        return true;
  return false;
}

void refresh_manifest_identity(TuManifest &manifest) {
  for (auto &observation : manifest.observations)
    observation.observation_id = stable_hash(
        manifest.repository_id + "\n" + manifest.source_revision + "\n" +
        manifest.translation_unit + "\n" + manifest.context_id + "\n" +
        manifest.build_variant.variant_id + "\n" + observation.element_id +
        "\n" + observation.variant_id + "\n" +
        nlohmann::json(observation.location).dump());
  const nlohmann::json identity = {
      {"repository", manifest.repository_id},
      {"revision", manifest.source_revision},
      {"configuration", manifest.configuration},
      {"build_variant", manifest.build_variant},
      {"tu", manifest.translation_unit},
      {"blob", manifest.source_blob},
      {"context", manifest.context_id},
      {"observations", manifest.observations},
      {"macro_expansions", manifest.macro_expansions}};
  manifest.manifest_id = stable_hash(identity.dump());
}

struct ExtractionOutcome {
  std::string state{"failed"}, failure, failure_detail;
  std::size_t elements{};
  std::uint64_t cpu_time_ms{};
  bool cache_hit{};
  nlohmann::json unit;
};

std::optional<std::string>
semantic_cache_key(const CompileContext &context, const RevisionTreeIndex &tree,
                   const std::string &source_blob,
                   const std::string &extractor_identity) {
  if (context.project_files.empty() || extractor_identity.empty())
    return std::nullopt;
  nlohmann::json dependencies = nlohmann::json::object();
  for (const auto &path : context.project_files) {
    const auto blob = tree.blob_at(path);
    if (!blob)
      return std::nullopt;
    dependencies[path] = *blob;
  }
  return stable_hash(nlohmann::json({{"source_blob", source_blob},
                                     {"context_id", context.context_id},
                                     {"dependencies", dependencies},
                                     {"extractor", extractor_identity}})
                         .dump());
}

ExtractionOutcome extract_context(
    const CompileContext &context, const std::filesystem::path &repository,
    const std::string &revision, const std::string &repository_id,
    const std::string &extractor, const nlohmann::json &experiment,
    const std::filesystem::path &output, const std::set<std::string> &changed,
    const RevisionTreeIndex &tree,
    const std::map<std::tuple<std::string, std::string, std::string>,
                   TuManifest> &previous_manifests) {
  ExtractionOutcome outcome;
  std::string blob_id = "unresolved";
  if (const auto blob = tree.blob_at(context.translation_unit))
    blob_id = *blob;
  const auto previous_key = std::make_tuple(
      context.configuration, context.translation_unit, context.context_id);
  bool invalidated = changed.contains(context.translation_unit);
  for (const auto &dependency : context.project_files)
    invalidated = invalidated || changed.contains(dependency);
  if (context.project_files.empty())
    for (const auto &path : changed) {
      const auto extension = path_to_utf8(path_from_utf8(path).extension());
      if (extension == ".h" || extension == ".hh" || extension == ".hpp" ||
          extension == ".hxx")
        invalidated = true;
    }
  const auto previous = previous_manifests.find(previous_key);
  if (!invalidated && previous != previous_manifests.end()) {
    auto reused = previous->second;
    reused.source_revision = revision;
    reused.source_blob = blob_id;
    reused.build_variant = context.build_variant;
    refresh_manifest_identity(reused);
    outcome.state = context.coverage.status == "complete" &&
                            reused.coverage.status == "complete"
                        ? "complete"
                        : "partial";
    outcome.elements = reused.elements.size();
    outcome.cache_hit = true;
    persist(output / "manifests" /
                (context.configuration + "-" + context.context_id + ".v1.json"),
            nlohmann::json(reused));
    outcome.unit = {{"configuration", context.configuration},
                    {"translation_unit", context.translation_unit},
                    {"context_id", context.context_id},
                    {"state", outcome.state},
                    {"failure", ""},
                    {"cache_hit", true},
                    {"cache_source", "previous_revision"},
                    {"elements", outcome.elements},
                    {"context_coverage", context.coverage},
                    {"extractor_coverage", reused.coverage},
                    {"diagnostic_fingerprint", ""}};
    return outcome;
  }
  const auto cache_key =
      semantic_cache_key(context, tree, blob_id,
                         experiment.value("extractor_identity", std::string{}));
  if (cache_key && experiment.contains("artifact_cache")) {
    const auto cache_path =
        path_from_utf8(
            experiment.at("artifact_cache").get<std::string>()) /
        ("tu-" + *cache_key + ".v1.json");
    if (std::filesystem::exists(cache_path)) {
      auto reused = read_json(cache_path).get<TuManifest>();
      reused.source_revision = revision;
      reused.source_blob = blob_id;
      reused.configuration = context.configuration;
      reused.build_variant = context.build_variant;
      refresh_manifest_identity(reused);
      outcome.state = context.coverage.status == "complete" &&
                              reused.coverage.status == "complete"
                          ? "complete"
                          : "partial";
      outcome.elements = reused.elements.size();
      outcome.cache_hit = true;
      persist(
          output / "manifests" /
              (context.configuration + "-" + context.context_id + ".v1.json"),
          nlohmann::json(reused));
      outcome.unit = {{"configuration", context.configuration},
                      {"translation_unit", context.translation_unit},
                      {"context_id", context.context_id},
                      {"state", outcome.state},
                      {"failure", ""},
                      {"cache_hit", true},
                      {"cache_source", "content_addressed"},
                      {"elements", outcome.elements},
                      {"context_coverage", context.coverage},
                      {"extractor_coverage", reused.coverage},
                      {"diagnostic_fingerprint", ""}};
      return outcome;
    }
  }
  std::vector<std::string> command = {
      extractor,
      "--source-revision",
      revision,
      "--configuration",
      context.configuration,
      "--build-variant",
      nlohmann::json(context.build_variant).dump(),
      "--context-fingerprint",
      context.context_id,
      "--source-blob",
      blob_id,
      "--project-root",
      path_to_utf8(repository),
      "--repository-id",
      repository_id,
      path_to_utf8(repository / path_from_utf8(context.translation_unit)),
      "--"};
  command.insert(command.end(), context.frontend_arguments.begin(),
                 context.frontend_arguments.end());
  ProcessOptions options;
  options.working_directory = repository;
  options.timeout = std::chrono::seconds(
      experiment.value("extractor_timeout_seconds", 1800U));
  options.max_output_bytes =
      experiment.value("max_manifest_bytes", 256ULL * 1024ULL * 1024ULL);
  const auto extracted = run_process(command, options);
  outcome.cpu_time_ms = extracted.cpu_time_ms;
  Coverage extractor_coverage;
  if (extracted.timed_out) {
    outcome.failure = "timeout";
  } else if (extracted.output_truncated) {
    outcome.failure = "output_limit";
  } else if (extracted.exit_code != 0) {
    outcome.failure = "extractor_exit";
  } else {
    std::optional<nlohmann::json> value;
    try {
      value = nlohmann::json::parse(extracted.output);
      const auto tu_manifest = value->get<TuManifest>();
      std::string error;
      if (!validate_tu_manifest(tu_manifest, error))
        throw std::runtime_error(error);
      outcome.elements = tu_manifest.elements.size();
      extractor_coverage = tu_manifest.coverage;
      outcome.state = context.coverage.status == "complete" &&
                              tu_manifest.coverage.status == "complete"
                          ? "complete"
                          : "partial";
    } catch (const std::exception &error) {
      outcome.failure = "invalid_manifest";
      outcome.failure_detail = utf8_lossy(error.what());
    } catch (...) {
      outcome.failure = "invalid_manifest";
      outcome.failure_detail = "unknown manifest validation error";
    }
    if (value && outcome.failure.empty()) {
      persist(
          output / "manifests" /
              (context.configuration + "-" + context.context_id + ".v1.json"),
          *value);
      if (cache_key && experiment.contains("artifact_cache"))
        persist(path_from_utf8(
                    experiment.at("artifact_cache").get<std::string>()) /
                    ("tu-" + *cache_key + ".v1.json"),
                *value);
    }
  }
  if (outcome.state == "failed") {
    extractor_coverage.status = "partial";
    extractor_coverage.gaps.push_back(outcome.failure);
  }
  outcome.unit = {{"configuration", context.configuration},
                  {"translation_unit", context.translation_unit},
                  {"context_id", context.context_id},
                  {"state", outcome.state},
                  {"failure", outcome.failure},
                  {"failure_detail", outcome.failure_detail},
                  {"cache_hit", false},
                  {"cache_source", ""},
                  {"elements", outcome.elements},
                  {"context_coverage", context.coverage},
                  {"extractor_coverage", extractor_coverage},
                  {"diagnostic_fingerprint", stable_hash(extracted.error)}};
  return outcome;
}

} // namespace

nlohmann::json
run_capture_experiment(const std::filesystem::path &manifest_path,
                       const std::filesystem::path &compiler_probe) {
  auto manifest = read_json(manifest_path);
  validate_manifest(manifest);
  const auto output = prepare_output(manifest);
  Catalog catalog(output / "catalog");
  auto result = capture(manifest, compiler_probe, catalog, output / "capture");
  result["artifact_version"] = 1;
  result["output"] = generic_path_to_utf8(output);
  persist(output / "capture-report.v1.json", result);
  return result;
}

nlohmann::json
run_head_experiment(const std::filesystem::path &manifest_path,
                    const std::filesystem::path &compiler_probe) {
  auto manifest = read_json(manifest_path);
  validate_manifest(manifest);
  if (!manifest.contains("extractor"))
    throw std::runtime_error("HEAD experiment requires extractor");
  const auto repository = canonical_directory(manifest, "repository");
  auto revision = manifest.at("revision").get<std::string>();
  const auto current =
      run_process({"git", "-C", path_to_utf8(repository), "rev-parse", "HEAD"});
  const auto requested = run_process(
      {"git", "-C", path_to_utf8(repository), "rev-parse", revision + "^{commit}"});
  git_success(current, "resolve current HEAD");
  git_success(requested, "resolve requested HEAD revision");
  if (lines(current.output) != lines(requested.output))
    throw std::runtime_error(
        "HEAD experiment repository is not checked out at requested revision");
  revision = lines(requested.output).front();
  manifest["revision"] = revision;
  const auto output = prepare_output(manifest);
  Catalog catalog(output / "catalog");
  nlohmann::json capture_result;
  if (manifest.contains("capture_input")) {
    prepare_generated_files(manifest, repository);
    reuse_capture(path_from_utf8(manifest.at("capture_input").get<std::string>()),
                  output / "capture", revision);
    capture_result = {
        {"schema_version", kSchemaVersion},
        {"reused", true},
        {"import", import_build_log(catalog, output / "capture", repository)}};
  } else {
    capture_result =
        capture(manifest, compiler_probe, catalog, output / "capture");
    capture_result["reused"] = false;
  }
  const auto repository_id = manifest.at("repository_id").get<std::string>();
  const auto extractor = manifest.at("extractor").get<std::string>();
  const auto extractor_version = run_process({extractor, "--version"});
  if (extractor_version.exit_code != 0)
    throw std::runtime_error("cannot identify experiment extractor");
  auto extraction_options = manifest;
  extraction_options["extractor_identity"] =
      stable_hash(extractor_version.output);
  std::set<std::pair<std::string, std::string>> unique_contexts;
  nlohmann::json units = nlohmann::json::array();
  std::map<std::string, std::size_t> states, failures;
  std::uint64_t elements = 0, cache_hits = 0, extraction_cpu_ms = 0;
  std::set<std::string> changed;
  if (manifest.contains("changed_files"))
    for (const auto &path : manifest.at("changed_files"))
      changed.insert(path.get<std::string>());
  std::map<std::tuple<std::string, std::string, std::string>, TuManifest>
      previous_manifests;
  if (manifest.contains("previous_manifests")) {
    const auto directory = path_from_utf8(
        manifest.at("previous_manifests").get<std::string>());
    if (std::filesystem::exists(directory))
      for (const auto &entry : std::filesystem::directory_iterator(directory))
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
          const auto previous = read_json(entry.path()).get<TuManifest>();
          previous_manifests[{previous.configuration, previous.translation_unit,
                              previous.context_id}] = previous;
        }
  }
  std::vector<CompileContext> contexts;
  for (const auto &record : captured_records(output / "capture")) {
    const auto tu = record.at("translation_unit").get<std::string>();
    for (const auto &context : catalog.compile_contexts(tu, revision)) {
      if (!unique_contexts.emplace(context.configuration, context.context_id)
               .second)
        continue;
      contexts.push_back(context);
    }
  }
  if (manifest.contains("selected_files")) {
    const auto selected =
        manifest.at("selected_files").get<std::set<std::string>>();
    contexts.erase(
        std::remove_if(contexts.begin(), contexts.end(),
                       [&](const auto &context) {
                         if (selected.contains(context.translation_unit))
                           return false;
                         return std::none_of(context.project_files.begin(),
                                             context.project_files.end(),
                                             [&](const auto &path) {
                                               return selected.contains(path);
                                             });
                       }),
        contexts.end());
  }
  const RevisionTreeIndex tree(repository, revision);
  const auto started = std::chrono::steady_clock::now();
  std::vector<ExtractionOutcome> outcomes(contexts.size());
  std::atomic<std::size_t> next{};
  std::exception_ptr worker_error;
  std::mutex error_mutex;
  const auto configured_concurrency =
      manifest.value("extractor_concurrency",
                     std::max(1U, std::thread::hardware_concurrency()));
  const auto concurrency = std::max<std::size_t>(
      1, std::min<std::size_t>({configured_concurrency, contexts.size(), 64}));
  std::vector<std::jthread> workers;
  workers.reserve(concurrency);
  for (std::size_t worker = 0; worker < concurrency; ++worker)
    workers.emplace_back([&] {
      for (;;) {
        const auto index = next.fetch_add(1);
        if (index >= contexts.size())
          return;
        try {
          outcomes[index] = extract_context(
              contexts[index], repository, revision, repository_id, extractor,
              extraction_options, output, changed, tree, previous_manifests);
        } catch (...) {
          std::scoped_lock lock(error_mutex);
          if (!worker_error)
            worker_error = std::current_exception();
          return;
        }
      }
    });
  workers.clear();
  if (worker_error)
    std::rethrow_exception(worker_error);
  for (const auto &outcome : outcomes) {
    elements += outcome.elements;
    extraction_cpu_ms += outcome.cpu_time_ms;
    if (outcome.cache_hit)
      ++cache_hits;
    ++states[outcome.state];
    if (!outcome.failure.empty())
      ++failures[outcome.failure];
    units.push_back(outcome.unit);
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  std::uint64_t artifact_bytes = 0;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(output))
    if (entry.is_regular_file())
      artifact_bytes += entry.file_size();
  nlohmann::json result = {
      {"schema_version", kSchemaVersion},
      {"artifact_version", 1},
      {"revision", revision},
      {"capture", capture_result},
      {"translation_unit_contexts", contexts.size()},
      {"extractor_concurrency", concurrency},
      {"extractor_identity", extraction_options.at("extractor_identity")},
      {"states", states},
      {"failure_taxonomy", failures},
      {"extracted_elements", elements},
      {"cache_hits", cache_hits},
      {"extraction_elapsed_ms", elapsed.count()},
      {"extraction_cpu_ms", extraction_cpu_ms},
      {"artifact_bytes", artifact_bytes},
      {"units", std::move(units)},
      {"output", generic_path_to_utf8(output)}};
  persist(output / "head-report.v1.json", result);
  return result;
}

nlohmann::json
run_pilot_experiment(const std::filesystem::path &manifest_path,
                     const std::filesystem::path &compiler_probe) {
  const auto manifest = read_json(manifest_path);
  validate_manifest(manifest);
  const auto repository = canonical_directory(manifest, "repository");
  const auto output =
      std::filesystem::absolute(
          path_from_utf8(manifest.at("output").get<std::string>()))
          .lexically_normal();
  const auto pilot = manifest.value("pilot", nlohmann::json::object());
  if (!pilot.contains("budget"))
    throw std::runtime_error("pilot experiment requires explicit budget caps");
  std::filesystem::create_directories(output / "revisions");
  std::filesystem::create_directories(output / "worktrees");
  WorkspaceLimits workspace_limits;
  workspace_limits.max_revisions =
      manifest.value("workspace_max_revisions", std::size_t{2});
  workspace_limits.max_bytes =
      manifest.value("workspace_max_bytes", std::uint64_t{});
  workspace_limits.free_space_reserve_bytes = manifest.value(
      "workspace_free_space_reserve_bytes", 5ULL * 1024ULL * 1024ULL * 1024ULL);
  RevisionWorkspacePool workspace_pool(output / "worktrees", workspace_limits);
  std::vector<std::string> revisions;
  if (pilot.contains("revisions")) {
    revisions = pilot.at("revisions").get<std::vector<std::string>>();
  } else {
    const auto maximum = pilot.value("max_revisions", 20U);
    const auto ref =
        pilot.value("ref", manifest.at("revision").get<std::string>());
    const auto listed = run_process(
        {"git", "-C", path_to_utf8(repository), "rev-list", "--first-parent",
         "--max-count=" + std::to_string(maximum), ref});
    git_success(listed, "list pilot revisions");
    revisions = lines(listed.output);
    std::reverse(revisions.begin(), revisions.end());
  }
  if (revisions.size() < 2)
    throw std::runtime_error(
        "pilot experiment requires at least two revisions");
  const auto progressive = plan_progressive_screening(
      {repository, output, revisions,
       manifest.value("partition", nlohmann::json::object()),
       pilot.at("budget")});
  persist(output / "progressive-screening.v1.json", progressive);
  const auto semantic_revisions = progressive.at("promotion")
                                      .at("semantic_revisions")
                                      .get<std::vector<std::string>>();
  const auto selected_files = progressive.at("promotion").at("paths");
  if (!manifest.contains("extractor"))
    throw std::runtime_error("pilot experiment requires extractor");
  const auto pilot_analysis_identity = stable_hash(
      nlohmann::json(
          {{"manifest", manifest},
           {"resolved_revisions", revisions},
           {"screening_identity",
            progressive.value("screening_identity", std::string{})},
           {"compiler_probe_identity",
            executable_identity(compiler_probe, "experiment compiler probe")},
           {"extractor_identity",
            executable_identity(
                path_from_utf8(manifest.at("extractor").get<std::string>()),
                                "experiment extractor")},
           {"pilot_engine_identity", "pilot-analysis-v1.1"}})
          .dump());
  nlohmann::json revision_reports = nlohmann::json::array();
  for (std::size_t index = 0; index < semantic_revisions.size(); ++index) {
    const auto &semantic_revision = semantic_revisions[index];
    const auto original =
        std::find(revisions.begin(), revisions.end(), semantic_revision);
    const auto original_index =
        static_cast<std::size_t>(std::distance(revisions.begin(), original));
    const auto revision_output = output / "revisions" /
                                 (std::to_string(original_index) + "-" +
                                  semantic_revision.substr(0, 12));
    const auto report_path = revision_output / "head-report.v1.json";
    const auto revision_identity = stable_hash(
        nlohmann::json({{"pilot_analysis_identity", pilot_analysis_identity},
                        {"revision", semantic_revision}})
            .dump());
    if (std::filesystem::exists(report_path)) {
      auto cached = read_json(report_path);
      if (cached.value("pilot_revision_identity", std::string{}) ==
          revision_identity) {
        revision_reports.push_back(std::move(cached));
        continue;
      }
    }
    if (std::filesystem::exists(revision_output)) {
      auto quarantined = revision_output;
      quarantined += ".incomplete." +
                     std::to_string(
                         std::chrono::steady_clock::now().time_since_epoch().count());
      std::filesystem::rename(revision_output, quarantined);
    }
    const bool capture_reused =
        index > 0 &&
        !build_context_changed(repository, semantic_revisions[index - 1],
                               semantic_revision, pilot);
    MaterializationManifest materialization;
    if (capture_reused) {
      const auto previous_output = path_from_utf8(
          revision_reports.back().at("output").get<std::string>());
      materialization = captured_materialization(previous_output / "capture");
    }
    const bool require_full =
        !capture_reused || requires_repository_preparation(manifest);
    auto workspace = workspace_pool.acquire(repository, semantic_revision,
                                            materialization, require_full);
    const auto &worktree = workspace.path();
    auto child = manifest;
    child["repository"] = path_to_utf8(worktree);
    child["revision"] = semantic_revision;
    child["output"] = path_to_utf8(revision_output);
    child["selected_files"] = selected_files;
    child["artifact_cache"] = manifest.value(
        "artifact_cache", path_to_utf8(output / "artifact-cache-v1"));
    child.erase("pilot");
    child.erase("partition");
    child.erase("policy");
    if (capture_reused) {
      const auto previous_output = path_from_utf8(
          revision_reports.back().at("output").get<std::string>());
      child["capture_input"] = path_to_utf8(previous_output / "capture");
      child["previous_manifests"] =
          path_to_utf8(previous_output / "manifests");
      child["changed_files"] = changed_paths(
          repository, semantic_revisions[index - 1], semantic_revision);
    }
    const auto child_manifest =
        output / ("revision-" + std::to_string(index) + ".v1.json");
    persist(child_manifest, child);
    auto revision_report = run_head_experiment(child_manifest, compiler_probe);
    revision_report["pilot_analysis_identity"] = pilot_analysis_identity;
    revision_report["pilot_revision_identity"] = revision_identity;
    revision_report["workspace"] = {
        {"mode", workspace.full() ? "temporary_full" : "sparse"},
        {"closure_complete", materialization.closure_complete},
        {"requested_files", materialization.files.size()}};
    persist(report_path, revision_report);
    revision_reports.push_back(std::move(revision_report));
  }

  std::map<std::tuple<std::string, std::string, std::string>, nlohmann::json>
      series;
  std::map<std::tuple<std::string, std::string, std::string>,
           std::set<std::string>>
      observed_revisions;
  for (const auto &revision : revision_reports) {
    const auto manifest_directory =
        path_from_utf8(revision.at("output").get<std::string>()) /
        "manifests";
    if (!std::filesystem::exists(manifest_directory))
      continue;
    for (const auto &entry :
         std::filesystem::directory_iterator(manifest_directory)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json")
        continue;
      const auto value = read_json(entry.path());
      const auto tu = value.get<TuManifest>();
      auto &item = series[{tu.build_variant.variant_id, tu.configuration,
                           tu.translation_unit}];
      item["configuration"] = tu.configuration;
      item["build_variant"] = tu.build_variant;
      item["translation_unit"] = tu.translation_unit;
      item["evidence_tier"] = "semantic";
      observed_revisions[{tu.build_variant.variant_id, tu.configuration,
                          tu.translation_unit}]
          .insert(tu.source_revision);
      if (!item.contains("bundles"))
        item["bundles"] = nlohmann::json::array();
      item["bundles"].push_back(path_to_utf8(entry.path()));
    }
  }
  nlohmann::json usable_series = nlohmann::json::array();
  nlohmann::json series_evidence_gaps = nlohmann::json::array();
  const std::set<std::string> required_revisions(semantic_revisions.begin(),
                                                 semantic_revisions.end());
  for (auto &[key, value] : series) {
    std::vector<std::string> missing_revisions;
    std::set_difference(required_revisions.begin(), required_revisions.end(),
                        observed_revisions[key].begin(),
                        observed_revisions[key].end(),
                        std::back_inserter(missing_revisions));
    value["coverage_complete"] =
        progressive.at("coverage").value("history_complete", false) &&
        missing_revisions.empty();
    value["missing_revisions"] = missing_revisions;
    if (!missing_revisions.empty())
      series_evidence_gaps.push_back(
          {{"kind", "semantic_revision_observation_missing"},
           {"build_variant_id", std::get<0>(key)},
           {"configuration", std::get<1>(key)},
           {"translation_unit", std::get<2>(key)},
           {"missing_revisions", missing_revisions},
           {"effect", "classification_incomplete"}});
    if (value.at("bundles").size() >= 2)
      usable_series.push_back(std::move(value));
  }

  std::map<std::string, std::string> revision_authors;
  std::map<std::string, std::int64_t> revision_times;
  std::map<std::string, std::vector<std::string>> revision_file_touches;
  for (const auto &revision : revisions) {
    const auto author = run_process({"git", "-C", path_to_utf8(repository), "show",
                                     "-s", "--format=%ae%n%ct", revision});
    git_success(author, "read pilot revision author");
    auto values = lines(author.output);
    revision_authors[revision] =
        values.empty() ? std::string{} : stable_hash(values.front());
    if (values.size() >= 2)
      revision_times[revision] = std::stoll(values[1]);
  }
  for (std::size_t index = 1; index < revisions.size(); ++index)
    revision_file_touches[revisions[index]] =
        changed_paths(repository, revisions[index - 1], revisions[index]);
  const auto change_evidence = summarize_change_evidence(
      repository, revision_reports, pilot.at("budget"));
  auto evidence_gaps = progressive.at("evidence_gaps");
  for (const auto &gap : change_evidence.at("evidence_gaps"))
    evidence_gaps.push_back(gap);
  for (const auto &gap : series_evidence_gaps)
    evidence_gaps.push_back(gap);

  const nlohmann::json progressive_summary = {
      {"artifact", generic_path_to_utf8(output / "progressive-screening.v1.json")},
      {"screening_identity",
       progressive.value("screening_identity", std::string{})},
      {"screened_files", progressive.at("screening").at("files").size()},
      {"syntax_transitions", progressive.at("syntax").at("transitions").size()},
      {"promoted_elements", progressive.at("promotion").at("elements").size()},
      {"paths", progressive.at("promotion").at("paths")},
      {"semantic_revisions",
       progressive.at("promotion").at("semantic_revisions")},
      {"coverage", progressive.at("coverage")},
      {"evidence_gaps", progressive.at("evidence_gaps")}};

  nlohmann::json result = {{"schema_version", kSchemaVersion},
                           {"artifact_version", 1},
                           {"pilot_analysis_identity", pilot_analysis_identity},
                           {"revisions", revisions},
                           {"revision_reports", revision_reports},
                           {"series", usable_series},
                           {"revision_authors", revision_authors},
                           {"revision_times", revision_times},
                           {"revision_file_touches", revision_file_touches},
                           {"progressive", progressive_summary},
                           {"change_evidence", change_evidence},
                           {"evidence_gaps", evidence_gaps},
                           {"budget_usage", progressive.at("budget_usage")},
                           {"resume_granularity", "revision"},
                           {"output", generic_path_to_utf8(output)}};
  persist(output / "pilot-report.v1.json", result);
  return result;
}

} // namespace history
