#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>

#include "history/build_info.hpp"
#include "history/catalog.hpp"
#include "history/config.hpp"
#include "history/encoding.hpp"
#include "history/experiment.hpp"
#include "history/git_coordination.hpp"
#include "history/http.hpp"
#include "history/process.hpp"
#include "history/query.hpp"
#include "history/stability.hpp"
#include "history/telemetry.hpp"
#include "history/worker.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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
  if (action == "classify")
    return result;
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

int run_service(const std::filesystem::path &config_path,
                std::stop_token service_stop = {}) {
  const auto config = history::parse_service_config(read_json(config_path));
  history::set_default_process_timeout(
      std::chrono::seconds(config.git_timeout_seconds));
  history::Telemetry::instance().configure(config.otlp_endpoint,
                                           config.otel_service_name);
  auto catalog = std::make_shared<history::Catalog>(config.catalog);
  history::CoordinationOptions coordination;
  coordination.repository = config.artifact_repository;
  coordination.remote = config.remote;
  coordination.lease_seconds = config.lease_seconds;
  coordination.grace_seconds = config.grace_seconds;
  coordination.trusted_producers = config.trusted_producers;
  coordination.enforce_trusted_producers = true;
  coordination.max_record_bytes = config.max_manifest_bytes;
  auto coordinator =
      std::make_shared<history::GitCoordinator>(*catalog, coordination);
  std::vector<std::shared_ptr<history::BackgroundWorker>> workers;
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
    for (std::uint32_t index = 0; index < config.worker_concurrency; ++index)
      workers.push_back(std::make_shared<history::BackgroundWorker>(
          *catalog, *coordinator, options));
  }
  history::QueryService service(std::make_shared<history::MemoryFactStore>(),
                                catalog, coordinator,
                                workers.empty() ? nullptr : workers.front());
  std::vector<std::jthread> worker_threads;
  std::jthread coordination_thread([catalog, coordinator, config,
                                    service_stop](std::stop_token thread_stop) {
    while (!thread_stop.stop_requested() && !service_stop.stop_requested()) {
      try {
        const auto pending = catalog->pending_tasks();
        const auto coordinated = !pending.empty()
                                     ? coordinator->publish_tasks(pending)
                                     : coordinator->sync();
        const auto state = coordinated.value("state", std::string{});
        history::Telemetry::instance().gauge(
            "coordination.ready",
            state == "published" || state == "synchronized" ? 1 : 0);
      } catch (const std::exception &error) {
        history::Telemetry::instance().gauge("coordination.ready", 0);
        history::Telemetry::instance().increment("coordination.sync_failures");
        history::Telemetry::instance().log(
            "warning", "coordination.sync_failed",
            {{"diagnostic_fingerprint", history::stable_hash(error.what())}});
      }
      for (std::uint32_t elapsed = 0;
           elapsed < config.sync_seconds && !thread_stop.stop_requested() &&
           !service_stop.stop_requested();
           ++elapsed)
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  });
  for (const auto &worker : workers) {
    worker_threads.emplace_back([worker,
                                 service_stop](std::stop_token thread_stop) {
      while (!thread_stop.stop_requested() && !service_stop.stop_requested()) {
        const auto result = worker->run_once();
        if (result.value("state", std::string{}) != "completed")
          std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    });
  }
  history::HttpServerOptions server;
  server.address = config.listen_address;
  server.port = config.port;
  server.stop_token = service_stop;
  history::run_http_server(server, service,
                           {{"schema_version", history::kSchemaVersion},
                            {"ok", true},
                            {"tool_version", history::build::kToolVersion},
                            {"repository_id", config.repository_id},
                            {"producer_id", catalog->producer_id()},
                            {"snapshot_id", catalog->snapshot_id()},
                            {"listen_address", server.address},
                            {"port", server.port}});
  return 0;
}

#ifdef _WIN32
std::filesystem::path windows_service_config;
std::stop_source windows_service_stop;
SERVICE_STATUS_HANDLE windows_service_handle{};

void WINAPI service_control(DWORD control) {
  if (control != SERVICE_CONTROL_STOP && control != SERVICE_CONTROL_SHUTDOWN)
    return;
  SERVICE_STATUS status{};
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = SERVICE_STOP_PENDING;
  status.dwControlsAccepted = 0;
  status.dwWaitHint = 5000;
  SetServiceStatus(windows_service_handle, &status);
  windows_service_stop.request_stop();
}

void WINAPI service_entry(DWORD, wchar_t **) {
  windows_service_handle =
      RegisterServiceCtrlHandlerW(L"Repotraverse", service_control);
  if (!windows_service_handle)
    return;
  SERVICE_STATUS status{};
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = SERVICE_RUNNING;
  status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  SetServiceStatus(windows_service_handle, &status);
  DWORD exit_code = 0;
  try {
    exit_code = static_cast<DWORD>(
        run_service(windows_service_config, windows_service_stop.get_token()));
  } catch (...) {
    exit_code = ERROR_SERVICE_SPECIFIC_ERROR;
  }
  status.dwCurrentState = SERVICE_STOPPED;
  status.dwControlsAccepted = 0;
  status.dwWin32ExitCode = exit_code;
  SetServiceStatus(windows_service_handle, &status);
}
#endif

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
  auto *classify =
      add_experiment("classify", "Classify a completed experiment");

  std::string service_config;
  auto *serve = cli.add_subcommand("serve", "Run the local service");
  serve->add_option("--config", service_config, "Service configuration file")
      ->required();
#ifdef _WIN32
  auto *windows_service =
      cli.add_subcommand("service", "Run under Windows Service Control Manager");
  windows_service
      ->add_option("--config", service_config, "Service configuration file")
      ->required();
#endif

  std::string status_endpoint;
  auto *status = cli.add_subcommand("status", "Show local or service status");
  status->add_option("--endpoint", status_endpoint, "Service HTTP endpoint");

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

  std::string query_endpoint, request_file;
  auto *query = cli.add_subcommand("query", "Execute a JSON request");
  query->add_option("--endpoint", query_endpoint, "Service HTTP endpoint");
  query->add_option("--request", request_file, "Request file; defaults to stdin");

  try {
    cli.parse(argc, argv);
  } catch (const CLI::ParseError &error) {
    const auto code = cli.exit(error);
    return code == 0 ? 0 : 2;
  }

  try {
    if (capture->parsed() || head->parsed() || pilot->parsed() ||
        classify->parsed()) {
      auto probe = std::filesystem::absolute(history::path_from_utf8(argv[0]))
                       .parent_path() /
                   "repotraverse-compiler-probe";
#ifdef _WIN32
      probe += ".exe";
#endif
      const auto action = capture->parsed()  ? std::string{"capture"}
                          : head->parsed()   ? std::string{"head"}
                          : pilot->parsed()  ? std::string{"pilot"}
                                             : std::string{"classify"};
      const auto manifest = history::path_from_utf8(experiment_manifest);
      const auto result =
          action == "capture"
              ? history::run_capture_experiment(manifest, probe)
          : action == "head"
              ? history::run_head_experiment(manifest, probe)
          : action == "pilot"
              ? history::run_pilot_experiment(manifest, probe)
              : history::run_stability_experiment(manifest);
      std::cout << history::canonical_json(
                       full_output ? result
                                   : experiment_summary(action, result))
                << '\n';
      return 0;
    }
    if (serve->parsed())
      return run_service(history::path_from_utf8(service_config));
#ifdef _WIN32
    if (windows_service->parsed()) {
      windows_service_config = history::path_from_utf8(service_config);
      SERVICE_TABLE_ENTRYW table[] = {
          {const_cast<wchar_t *>(L"Repotraverse"), service_entry},
          {nullptr, nullptr}};
      if (!StartServiceCtrlDispatcherW(table))
        throw std::runtime_error(
            "cannot connect to Windows Service Control Manager");
      return 0;
    }
#endif
    if (status->parsed()) {
      if (!status_endpoint.empty()) {
        std::cout << history::canonical_json(
            history::http_status(status_endpoint));
        return 0;
      }
      std::cout << history::canonical_json(
          {{"schema_version", history::kSchemaVersion},
           {"tool_version", history::build::kToolVersion},
           {"build_mode", history::build::kBuildMode},
           {"host_architecture", history::build::kHostArchitecture},
           {"core", "available"},
           {"query_transport", "durable_http_jobs_v1"},
           {"fact_store", "memory"},
           {"federated_service", "http_json_v1"},
           {"local_materialization", "sqlite"},
           {"shared_transport", "git_refs_v1"},
           {"identifier_model", "xxh3_128_v1"},
           {"history_planner", "first_parent_v1"},
           {"progressive_history", "git_tree_sitter_clang_v1"},
           {"file_history", "parallel_endpoints_v1"},
           {"build_context_adapter", "armcc5_armclang6_v1"},
           {"background_worker", "git_task_batches_v1"},
           {"lineage_model", "cpp_elements_reviewed_relations_v1"},
           {"persistent_raw_source", false}});
      return 0;
    }
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
    if (query->parsed()) {
      nlohmann::json request;
      if (!request_file.empty())
        request = read_json(history::path_from_utf8(request_file));
      else
        request = read_json(std::cin);
      const auto response =
          query_endpoint.empty()
              ? history::QueryService(
                    std::make_shared<history::MemoryFactStore>())
                    .execute(request)
              : history::http_query(query_endpoint, request);
      std::cout << history::canonical_json(response);
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
