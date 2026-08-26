#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>

#include "history/build_info.hpp"
#include "history/catalog.hpp"
#include "history/config.hpp"
#include "history/connectors.hpp"
#include "history/outbound_http.hpp"
#include "history/encoding.hpp"
#include "history/experiment.hpp"
#include "history/git_coordination.hpp"
#include "history/process.hpp"
#include "history/query.hpp"
#include "history/telemetry.hpp"
#include "history/worker.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

nlohmann::json read_json(std::istream &input) {
  nlohmann::json value;
  input >> value;
  return value;
}

nlohmann::json read_json(const std::filesystem::path &path) {
  return nlohmann::json::parse(history::read_text_file(path).text);
}

nlohmann::json experiment_summary(const std::string &action,
                                  const nlohmann::json &result) {
  nlohmann::json summary = {
      {"schema_version", result.value("schema_version", 1)},
      {"artifact_version", result.value("artifact_version", 1)},
      {"command", "experiment " + action},
      {"output", result.value("output", std::string{})}};
  if (action == "capture") {
    summary["report"] = history::generic_path_to_utf8(
        history::path_from_utf8(summary.at("output").get<std::string>()) /
        "capture-report.v1.json");
    summary["failed_runs"] = result.value("failed_runs", 0U);
    summary["import"] = result.value("import", nlohmann::json::object());
  } else if (action == "head") {
    summary["report"] = history::generic_path_to_utf8(
        history::path_from_utf8(summary.at("output").get<std::string>()) /
        "head-report.v1.json");
    for (const auto *field :
         {"revision", "translation_unit_contexts", "states", "failure_taxonomy",
          "extracted_elements", "cache_hits", "extraction_elapsed_ms",
          "artifact_bytes"})
      if (result.contains(field))
        summary[field] = result.at(field);
  } else if (action == "pilot") {
    summary["report"] = history::generic_path_to_utf8(
        history::path_from_utf8(summary.at("output").get<std::string>()) /
        "pilot-report.v1.json");
    summary["revision_count"] =
        result.value("revisions", nlohmann::json::array()).size();
    summary["semantic_series"] =
        result.value("series", nlohmann::json::array()).size();
    summary["progressive"] =
        result.value("progressive", nlohmann::json::object());
    summary["evidence_gap_count"] =
        result.value("evidence_gaps", nlohmann::json::array()).size();
  }
  return summary;
}

history::EvidenceBundle synthetic_bundle(std::size_t count, bool changed) {
  history::EvidenceBundle bundle;
  bundle.source_revision = changed ? "synthetic-after" : "synthetic-before";
  bundle.configuration = "benchmark";
  bundle.context_fingerprint = "fixed-seed-1";
  bundle.extractor_fingerprint = "synthetic-v1";
  bundle.coverage.capabilities = {"structural", "source_locations"};
  bundle.elements.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    history::ElementSnapshot element;
    element.compiler_id = "element-" + std::to_string(index);
    element.kind = "function";
    element.qualified_name = "synthetic::function" + std::to_string(index);
    element.interface_fingerprint = history::stable_hash("int(int)");
    element.implementation_fingerprint = history::stable_hash(
        changed && index % 100 == 0 ? "changed" : "unchanged");
    element.dependency_fingerprint = history::stable_hash("dependencies");
    element.location.path =
        "generated/file" + std::to_string(index / 100) + ".cpp";
    element.location.begin_line =
        static_cast<std::uint32_t>((index % 100) * 10 + 1);
    element.location.end_line = element.location.begin_line + 5;
    bundle.elements.push_back(std::move(element));
  }
  return bundle;
}

struct Runtime {
  history::CatalogConfig config;
  std::shared_ptr<history::Catalog> catalog;
  std::shared_ptr<history::GitCoordinator> coordinator;
  std::shared_ptr<history::BackgroundWorker> worker;
  std::shared_ptr<history::ConnectorService> connectors;
};

std::set<std::string> credential_capabilities(
    const history::CatalogConfig &config) {
  std::set<std::string> result;
  for (const auto &credential : config.credentials) {
    bool available = false;
    if (!credential.environment.empty()) {
      const auto *value = std::getenv(credential.environment.c_str());
      available = value && *value;
    } else if (!credential.file.empty()) {
      available = std::filesystem::is_regular_file(credential.file);
    } else if (!credential.windows_target.empty()) {
#ifdef _WIN32
      available = true;
#endif
    } else if (credential.mode == "windows_negotiate" ||
               credential.mode == "windows_ntlm") {
#ifdef _WIN32
      available = true;
#endif
    }
    if (available) result.insert(credential.name);
  }
  return result;
}

Runtime make_runtime(const std::filesystem::path &config_path) {
  Runtime runtime;
  runtime.config = history::parse_catalog_config(read_json(config_path));
  const auto &config = runtime.config;
  history::set_default_process_timeout(
      std::chrono::seconds(config.git_timeout_seconds));
  history::Telemetry::instance().configure(config.otlp_endpoint,
                                           config.otel_service_name);
  runtime.catalog = std::make_shared<history::Catalog>(
      config.catalog, config.local_cache_max_facts,
      config.local_cache_max_bytes);
  history::CoordinationOptions coordination;
  coordination.repository = config.analysis_repository;
  coordination.remote = config.remote;
  coordination.knowledge_ref = config.knowledge_ref;
  coordination.lease_seconds = config.lease_seconds;
  coordination.grace_seconds = config.grace_seconds;
  coordination.trusted_producers = config.trusted_producers;
  coordination.enforce_trusted_producers = true;
  coordination.max_record_bytes = config.max_manifest_bytes;
  runtime.coordinator =
      std::make_shared<history::GitCoordinator>(*runtime.catalog, coordination);
  if (!config.connectors.empty())
    runtime.connectors = std::make_shared<history::ConnectorService>(
        *runtime.catalog, runtime.coordinator.get(), config.connectors, config.credentials,
        history::make_curl_http_client());
  if (!config.extractor.empty()) {
    history::WorkspaceLimits workspace_limits;
    workspace_limits.max_revisions = config.workspace_max_revisions;
    workspace_limits.max_bytes = config.workspace_max_bytes;
    workspace_limits.free_space_reserve_bytes =
        config.workspace_free_space_reserve_bytes;
    auto workspace_pool = std::make_shared<history::RevisionWorkspacePool>(
        config.scratch_root, workspace_limits);
    history::WorkerOptions options;
    options.extractor = config.extractor;
    options.scratch_root = config.scratch_root;
    options.source_repository = config.source_repository;
    options.repository_id = config.repository_id;
    options.max_attempts = config.max_task_attempts;
    options.extractor_timeout =
        std::chrono::seconds(config.extractor_timeout_seconds);
    options.max_manifest_bytes = config.max_manifest_bytes;
    options.workspace_pool = std::move(workspace_pool);
    runtime.worker = std::make_shared<history::BackgroundWorker>(
        *runtime.catalog, *runtime.coordinator, options);
  }
  return runtime;
}

std::string executable_identity(const std::filesystem::path &executable) {
  return history::stable_hash(history::path_to_utf8(
      std::filesystem::weakly_canonical(executable)) + "\n" +
      std::string{history::build::kToolVersion});
}

bool spawn_runner(const std::filesystem::path &executable,
                  const std::filesystem::path &config,
                  const std::string &token) {
#ifdef _WIN32
  const auto command = L"\"" + executable.wstring() + L"\" runner --config \"" +
                       config.wstring() + L"\" --adoption-token " +
                       history::utf8_to_wide(token);
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const auto created = CreateProcessW(
      executable.c_str(), buffer.data(), nullptr, nullptr, FALSE,
      DETACHED_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  if (!created) return false;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
#else
  const auto child = fork();
  if (child < 0) return false;
  if (child == 0) {
    if (setsid() < 0) _exit(127);
    const auto grandchild = fork();
    if (grandchild < 0) _exit(127);
    if (grandchild > 0) _exit(0);
    const auto null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      dup2(null_fd, STDIN_FILENO);
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
      if (null_fd > STDERR_FILENO) close(null_fd);
    }
    const auto executable_text = history::path_to_utf8(executable);
    const auto config_text = history::path_to_utf8(config);
    execl(executable_text.c_str(), executable_text.c_str(), "runner", "--config",
          config_text.c_str(), "--adoption-token", token.c_str(),
          static_cast<char *>(nullptr));
    _exit(127);
  }
  int status = 0;
  return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0;
#endif
}

int run_runner(const std::filesystem::path &config_path,
               const std::filesystem::path &executable,
               const std::string &adoption_token) {
  auto runtime = make_runtime(config_path);
  const auto capabilities = credential_capabilities(runtime.config);
  const auto executable_id = executable_identity(executable);
  const auto config_id = history::stable_hash(history::canonical_json(read_json(config_path)));
#ifdef _WIN32
  const auto pid = static_cast<std::int64_t>(GetCurrentProcessId());
#else
  const auto pid = static_cast<std::int64_t>(getpid());
#endif
  const auto owner = history::stable_hash(adoption_token + ":" + std::to_string(pid));
  const auto started = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  if (!runtime.catalog->adopt_runner(adoption_token, owner, pid, started,
                                     executable_id, config_id, capabilities))
    return 3;
  std::mutex heartbeat_mutex;
  std::condition_variable_any heartbeat_changed;
  std::jthread heartbeat([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::unique_lock lock(heartbeat_mutex);
      heartbeat_changed.wait_for(lock, stop, std::chrono::seconds(5),
                                 [] { return false; });
      if (!stop.stop_requested()) {
        try {
          runtime.catalog->heartbeat_runner(owner);
        } catch (...) {
        }
      }
    }
  });
  try {
    runtime.coordinator->sync();
    const auto pending = runtime.catalog->pending_tasks();
    if (!pending.empty()) runtime.coordinator->publish_tasks(pending);
  } catch (...) {
  }
  for (;;) {
    const auto work = runtime.catalog->claim_next_work(owner, capabilities);
    if (!work) {
      try {
        const auto pending = runtime.catalog->pending_tasks();
        if (!pending.empty()) runtime.coordinator->publish_tasks(pending);
        runtime.coordinator->sync();
      } catch (...) {
      }
      if (runtime.catalog->release_runner_if_empty(owner)) break;
      continue;
    }
    const auto work_id = work->at("work_id").get<std::string>();
    try {
      const auto kind = work->at("kind").get<std::string>();
      auto parameters = work->at("parameters");
      nlohmann::json progress;
      if (kind == "extraction") {
        if (!runtime.worker) throw std::runtime_error("extractor is not configured");
        parameters["task_id"] = work_id;
        progress = runtime.worker->run_task(parameters);
      } else if (kind == "connector_sync") {
        if (!runtime.connectors) throw std::runtime_error("connectors are not configured");
        progress = runtime.connectors->sync(
            parameters.at("connector").get<std::string>(),
            parameters.value("full", false),
            parameters.value("issue_keys", std::vector<std::string>{}));
      } else if (kind == "analysis_git_sync") {
        progress = runtime.coordinator->sync();
      } else {
        throw std::runtime_error("unsupported work kind");
      }
      runtime.catalog->complete_work(work_id, owner, progress);
    } catch (const std::exception &error) {
      runtime.catalog->fail_work(work_id, owner, history::stable_hash(error.what()),
                                 runtime.config.max_task_attempts);
    }
  }
  heartbeat.request_stop();
  heartbeat_changed.notify_all();
  return 0;
}

} // namespace

int repotraverse_main(int argc, char **argv) {
  CLI::App cli{"Compiler-derived C/C++ element history across Git revisions",
               "repotraverse"};
  cli.require_subcommand(1);
  const auto version =
      std::string{"repotraverse "} +
      std::string{history::build::kToolVersion} + " (build mode: " +
      std::string{history::build::kBuildMode} + "; host: " +
      std::string{history::build::kHostArchitecture} + ")";
  cli.set_version_flag("--version", version);

  std::string experiment_manifest;
  bool full_output = false;
  auto *experiment = cli.add_subcommand("experiment", "Run v1 experiments");
  experiment->require_subcommand(1);
  const auto add_experiment = [&](const char *name, const char *description) {
    auto *subcommand = experiment->add_subcommand(name, description);
    subcommand->add_option("--manifest", experiment_manifest, "Manifest file")
        ->required();
    subcommand->add_flag("--full-output", full_output,
                         "Print the complete persisted report");
    return subcommand;
  };
  auto *capture = add_experiment("capture", "Capture build contexts");
  auto *head = add_experiment("head", "Analyze the head revision");
  auto *pilot = add_experiment("pilot", "Run progressive history analysis");

  std::string catalog_path_value;
  auto *identity = cli.add_subcommand("identity", "Manage producer identity");
  identity->require_subcommand(1);
  auto *identity_init = identity->add_subcommand("init", "Initialize identity");
  identity_init
      ->add_option("--catalog", catalog_path_value, "Catalog directory")
      ->required();

  std::size_t elements = 50'000;
  auto *benchmark = cli.add_subcommand("benchmark", "Run a synthetic benchmark");
  benchmark->add_option("--elements", elements, "Synthetic element count")
      ->check(CLI::PositiveNumber);

  std::string facts_file;
  auto *facts = cli.add_subcommand("facts", "Manage fact documents");
  facts->require_subcommand(1);
  auto *canonicalize =
      facts->add_subcommand("canonicalize", "Write canonical JSON");
  canonicalize->add_option("file", facts_file, "Input file; defaults to stdin");

  std::string command_config, command_input, repository, repository_id,
      revision, ref, path, connector, external_id, issue_key, work_id;
  std::size_t limit = 0, offset = 0;
  bool wait = false, full = false;
  const std::vector<std::string> operations = {
      "repository-changes", "history-summary", "change-unit",
      "symbol-search", "symbol-history", "file-symbols", "symbol-relations",
      "inference-get", "claim-propose", "claim-verify", "receipt-put",
      "receipt-get", "pr-import", "pull-request-get", "issue-get",
      "connector-status", "connector-sync", "file-history", "build-import",
      "catalog-sync"};
  std::map<std::string, CLI::App *> operation_commands;
  for (const auto &operation : operations) {
    auto *command = cli.add_subcommand(operation, "Run " + operation);
    command->add_option("--config", command_config, "Catalog/project configuration file")
        ->required();
    command->add_option("--input", command_input, "JSON parameter file; use - for stdin");
    command->add_option("--repository", repository, "Source repository path");
    command->add_option("--repository-id", repository_id, "Repository identity");
    command->add_option("--revision", revision, "Source revision");
    command->add_option("--ref", ref, "Git ref");
    command->add_option("--path", path, "Repository-relative path");
    command->add_option("--connector", connector, "Configured connector name");
    command->add_option("--external-id", external_id, "External record identity");
    command->add_option("--key", issue_key, "Issue key");
    command->add_option("--limit", limit, "Maximum result count");
    command->add_option("--offset", offset, "Pagination offset");
    command->add_flag("--full", full, "Request a full synchronization");
    command->add_flag("--wait", wait, "Wait for work from this invocation");
    operation_commands.emplace(operation, command);
  }
  auto *work_status = cli.add_subcommand("work-status", "Show redacted queue and runner state");
  work_status->add_option("--config", command_config, "Catalog/project configuration file")->required();
  work_status->add_option("--work-id", work_id, "Content-derived work identity");
  auto *work_cancel = cli.add_subcommand("work-cancel", "Request cancellation of work");
  work_cancel->add_option("--config", command_config, "Catalog/project configuration file")->required();
  work_cancel->add_option("--work-id", work_id, "Content-derived work identity")->required();
  std::string adoption_token;
  auto *runner = cli.add_subcommand("runner", "Internal on-demand runner");
  runner->group("");
  runner->add_option("--config", command_config)->required();
  runner->add_option("--adoption-token", adoption_token)->required();

  try {
    cli.parse(argc, argv);
  } catch (const CLI::ParseError &error) {
    const auto code = cli.exit(error);
    return code == 0 ? 0 : 2;
  }

  try {
    if (capture->parsed() || head->parsed() || pilot->parsed()) {
      auto probe = std::filesystem::absolute(history::path_from_utf8(argv[0]))
                       .parent_path() /
                   "repotraverse-compiler-probe";
#ifdef _WIN32
      probe += ".exe";
#endif
      const auto action = capture->parsed()  ? std::string{"capture"}
                          : head->parsed()   ? std::string{"head"}
                          : std::string{"pilot"};
      const auto manifest = history::path_from_utf8(experiment_manifest);
      const auto result =
          action == "capture"
              ? history::run_capture_experiment(manifest, probe)
          : action == "head"
              ? history::run_head_experiment(manifest, probe)
          : history::run_pilot_experiment(manifest, probe);
      std::cout << history::canonical_json(
                       full_output ? result
                                   : experiment_summary(action, result))
                << '\n';
      return 0;
    }
    const auto executable = std::filesystem::absolute(history::path_from_utf8(argv[0]));
    if (runner->parsed())
      return run_runner(history::path_from_utf8(command_config), executable,
                        adoption_token);
    if (identity_init->parsed()) {
      const auto catalog_path = history::path_from_utf8(catalog_path_value);
      history::Catalog catalog(catalog_path);
      std::cout << history::canonical_json(
          {{"schema_version", history::kSchemaVersion},
           {"ok", true},
           {"producer_id", catalog.producer_id()},
           {"catalog", history::path_to_utf8(
                           std::filesystem::absolute(catalog_path))}});
      return 0;
    }
    if (benchmark->parsed()) {
      const auto before = synthetic_bundle(elements, false);
      const auto after = synthetic_bundle(elements, true);
      const auto start = std::chrono::steady_clock::now();
      const auto result = history::trace_transition(before, after);
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start);
      std::cout << history::canonical_json(
          {{"schema_version", history::kSchemaVersion},
           {"elements", elements},
           {"transition_facts", result.facts.size()},
           {"elapsed_ms", elapsed.count()},
           {"seed", 1}});
      return 0;
    }
    if (canonicalize->parsed()) {
      if (!facts_file.empty()) {
        std::cout << history::canonical_json(
            read_json(history::path_from_utf8(facts_file)));
      } else {
        std::cout << history::canonical_json(read_json(std::cin));
      }
      return 0;
    }
    if (work_status->parsed() || work_cancel->parsed()) {
      auto runtime = make_runtime(history::path_from_utf8(command_config));
      const auto cancelled = work_cancel->parsed()
                                 ? runtime.catalog->cancel_work(work_id)
                                 : false;
      const auto status_config_path = history::path_from_utf8(command_config);
      const auto status_config_id = history::stable_hash(
          history::canonical_json(read_json(status_config_path)));
      if (const auto token = runtime.catalog->claim_runner_launch(
              credential_capabilities(runtime.config),
              executable_identity(executable), status_config_id)) {
        if (!spawn_runner(executable, std::filesystem::absolute(status_config_path),
                          *token))
          runtime.catalog->clear_failed_launch(*token);
      }
      std::cout << history::canonical_json(
          {{"schema_version", history::kSchemaVersion},
           {"operation", work_cancel->parsed() ? "work-cancel" : "work-status"},
           {"status", "complete"},
           {"snapshot_id", runtime.catalog->snapshot_id()},
           {"facts", {{"work", runtime.catalog->work_status(work_id)},
                       {"cancelled", cancelled}}},
           {"inference", {{"accepted", nlohmann::json::array()},
                           {"conflicts", nlohmann::json::array()}}},
           {"coverage", {{"status", "complete"},
                          {"capabilities", nlohmann::json::array()},
                          {"gaps", nlohmann::json::array()}}},
           {"pending_work", nlohmann::json::array()},
           {"runner", runtime.catalog->runner_status()}})
                << '\n';
      return work_cancel->parsed() && !cancelled ? 1 : 0;
    }
    for (const auto &[operation, command] : operation_commands) {
      if (!command->parsed()) continue;
      nlohmann::json params = nlohmann::json::object();
      if (command_input == "-")
        params = read_json(std::cin);
      else if (!command_input.empty())
        params = read_json(history::path_from_utf8(command_input));
      if (!params.is_object())
        throw std::runtime_error("command input must be a JSON object");
      if (!repository.empty()) params["repository"] = repository;
      if (!repository_id.empty()) params["repository_id"] = repository_id;
      if (!revision.empty()) params["revision"] = revision;
      if (!ref.empty()) params["ref"] = ref;
      if (!path.empty()) params["path"] = path;
      if (!connector.empty()) params["connector"] = connector;
      if (!external_id.empty()) params["external_id"] = external_id;
      if (!issue_key.empty()) params["key"] = issue_key;
      if (limit) params["limit"] = limit;
      if (offset) params["offset"] = offset;
      if (full) params["full"] = true;
      auto runtime = make_runtime(history::path_from_utf8(command_config));
      const auto capabilities = credential_capabilities(runtime.config);
      const auto config_path = history::path_from_utf8(command_config);
      const auto config_id = history::stable_hash(
          history::canonical_json(read_json(config_path)));
      runtime.catalog->configure_launch(
          capabilities, executable_identity(executable), config_id);
      auto invocation_id = history::stable_hash(
          operation + "\n" + history::canonical_json(params) + "\n" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      nlohmann::json response;
      if (operation == "connector-sync") {
        const auto credential = [&] {
          for (const auto &item : runtime.config.connectors)
            if (item.name == params.value("connector", std::string{}))
              return item.authentication;
          return std::string{};
        }();
        const auto enqueued = runtime.catalog->enqueue_work(
            "connector_sync", params, invocation_id, credential);
        response = {{"schema_version", history::kSchemaVersion}, {"ok", true},
                    {"operation", operation}, {"status", "partial"},
                    {"snapshot_id", runtime.catalog->snapshot_id()},
                    {"facts", runtime.connectors
                                  ? runtime.connectors->status(params.value("connector", std::string{}))
                                  : nlohmann::json::object()},
                    {"pending_work", nlohmann::json::array({{{"work_id", enqueued.work_id},
                                                               {"kind", "connector_sync"}}})}};
      } else {
        const auto query_name = operation == "file-history" ? "file.history"
                                : operation == "build-import" ? "build.import"
                                : operation == "catalog-sync" ? "catalog.sync"
                                                               : "tool." + operation;
        history::QueryService service(std::make_shared<history::MemoryFactStore>(),
                                      runtime.catalog, runtime.coordinator,
                                      runtime.worker, runtime.connectors);
        response = service.execute({{"schema_version", history::kSchemaVersion},
                                    {"query", query_name}, {"params", params}});
        if (response.contains("request_id"))
          invocation_id = response.at("request_id").get<std::string>();
      }
      const auto freshness = std::max<std::uint32_t>(
          1, runtime.config.analysis_sync_freshness_seconds);
      const auto boundary = std::chrono::system_clock::to_time_t(
                                std::chrono::system_clock::now()) /
                            freshness;
      runtime.catalog->enqueue_work(
          "analysis_git_sync", {{"freshness_boundary", boundary}}, {}, {}, true);
      auto token = runtime.catalog->pending_launch_token();
      if (!token)
        token = runtime.catalog->claim_runner_launch(
            capabilities, executable_identity(executable), config_id);
      if (token) {
        if (!spawn_runner(executable, std::filesystem::absolute(config_path), *token))
          runtime.catalog->clear_failed_launch(*token);
      }
      if (wait) {
        while (!runtime.catalog->invocation_settled(invocation_id))
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (operation != "connector-sync") {
          history::QueryService service(std::make_shared<history::MemoryFactStore>(),
                                        runtime.catalog, runtime.coordinator,
                                        runtime.worker, runtime.connectors);
          const auto query_name = operation == "file-history" ? "file.history"
                                  : operation == "build-import" ? "build.import"
                                  : operation == "catalog-sync" ? "catalog.sync"
                                                                 : "tool." + operation;
          response = service.execute({{"schema_version", history::kSchemaVersion},
                                      {"query", query_name}, {"params", params}});
        }
      }
      if (!response.contains("operation")) response["operation"] = operation;
      if (!response.contains("status"))
        response["status"] = response.value("result_status", std::string{"complete"});
      if (!response.contains("snapshot_id")) response["snapshot_id"] = runtime.catalog->snapshot_id();
      if (!response.contains("facts"))
        response["facts"] = response.contains("result")
                                ? response.at("result")
                                : nlohmann::json::object();
      if (!response.contains("inference"))
        response["inference"] = {{"accepted", nlohmann::json::array()},
                                 {"conflicts", nlohmann::json::array()}};
      if (!response.contains("coverage"))
        response["coverage"] = {{"status", response.at("status")},
                                {"capabilities", nlohmann::json::array()},
                                {"gaps", nlohmann::json::array()}};
      if (!response.contains("pending_work")) response["pending_work"] = nlohmann::json::array();
      response["runner"] = runtime.catalog->runner_status();
      std::cout << history::canonical_json(response) << '\n';
      return response.value("ok", false) ? 0 : 1;
    }
    return 2;
  } catch (const std::exception &error) {
    std::cout << history::canonical_json(
        {{"schema_version", history::kSchemaVersion},
         {"ok", false},
         {"error", {{"code", "fatal"},
                    {"message", history::utf8_lossy(error.what())}}}});
    return 1;
  }
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wide_argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index)
    arguments.push_back(history::wide_to_utf8(wide_argv[index]));
  std::vector<char *> argv;
  argv.reserve(arguments.size());
  for (auto &argument : arguments)
    argv.push_back(argument.data());
  return repotraverse_main(argc, argv.data());
}
#else
int main(int argc, char **argv) { return repotraverse_main(argc, argv); }
#endif
