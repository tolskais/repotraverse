#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "history/build_info.hpp"
#include "history/catalog.hpp"
#include "history/config.hpp"
#include "history/experiment.hpp"
#include "history/git_coordination.hpp"
#include "history/http.hpp"
#include "history/query.hpp"
#include "history/process.hpp"
#include "history/stability.hpp"
#include "history/worker.hpp"
#include "history/telemetry.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

nlohmann::json read_json(std::istream &input) {
  nlohmann::json value;
  input >> value;
  return value;
}

void usage() {
  std::cerr << "usage:\n"
            << "  repotraverse --version\n"
            << "  repotraverse serve --config FILE\n"
#ifdef _WIN32
            << "  repotraverse service --config FILE\n"
#endif
            << "  repotraverse query [--request FILE]\n"
            << "  repotraverse query --endpoint URL [--request FILE]\n"
            << "  repotraverse status [--endpoint URL]\n"
            << "  repotraverse identity init --catalog DIRECTORY\n"
            << "  repotraverse experiment capture --manifest FILE\n"
            << "  repotraverse experiment head --manifest FILE\n"
            << "  repotraverse experiment pilot --manifest FILE\n"
            << "  repotraverse experiment classify --manifest FILE\n"
            << "  repotraverse benchmark [--elements N]\n"
            << "  repotraverse facts canonicalize [FILE]\n";
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
  std::ifstream input(config_path);
  if (!input)
    throw std::runtime_error("cannot open service configuration");
  const auto config = history::parse_service_config(read_json(input));
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
    history::WorkerOptions options;
    options.extractor = config.extractor;
    options.scratch_root = config.scratch_root;
    options.source_repository = config.source_repository;
    options.repository_id = config.repository_id;
    options.max_attempts = config.max_task_attempts;
    options.extractor_timeout =
        std::chrono::seconds(config.extractor_timeout_seconds);
    options.max_manifest_bytes = config.max_manifest_bytes;
    for (std::uint32_t index = 0; index < config.worker_concurrency; ++index)
      workers.push_back(std::make_shared<history::BackgroundWorker>(
          *catalog, *coordinator, options));
  }
  history::QueryService service(std::make_shared<history::MemoryFactStore>(),
                                catalog, coordinator,
                                workers.empty() ? nullptr : workers.front());
  std::vector<std::jthread> worker_threads;
  std::jthread coordination_thread(
      [catalog, coordinator, config, service_stop](std::stop_token thread_stop) {
        while (!thread_stop.stop_requested() &&
               !service_stop.stop_requested()) {
          try {
            const auto pending = catalog->pending_tasks();
            const auto coordinated =
                !pending.empty() ? coordinator->publish_tasks(pending)
                                 : coordinator->sync();
            const auto state = coordinated.value("state", std::string{});
            history::Telemetry::instance().gauge(
                "coordination.ready",
                state == "published" || state == "synchronized" ? 1 : 0);
          } catch (const std::exception &error) {
            history::Telemetry::instance().gauge("coordination.ready", 0);
            history::Telemetry::instance().increment(
                "coordination.sync_failures");
            history::Telemetry::instance().log(
                "warning", "coordination.sync_failed",
                {{"diagnostic_fingerprint", history::stable_hash(error.what())}});
          }
          for (std::uint32_t elapsed = 0;
               elapsed < config.sync_seconds &&
               !thread_stop.stop_requested() &&
               !service_stop.stop_requested();
               ++elapsed)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
      });
  for (const auto &worker : workers) {
    worker_threads.emplace_back(
        [worker, service_stop](std::stop_token thread_stop) {
          while (!thread_stop.stop_requested() &&
                 !service_stop.stop_requested()) {
            const auto result = worker->run_once();
            if (result.value("state", std::string{}) != "completed")
              std::this_thread::sleep_for(std::chrono::seconds(1));
          }
        });
  }
  history::HttpServerOptions server;
  server.address = config.listen_address;
  server.port = config.port;
  server.sync_seconds = config.sync_seconds;
  server.stop_token = service_stop;
  history::run_http_server(
      server, service,
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

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  try {
    if (command == "--version") {
      std::cout << "repotraverse " << history::build::kToolVersion
                << " (build mode: " << history::build::kBuildMode
                << "; host: " << history::build::kHostArchitecture << ")\n";
      return 0;
    }
    if (command == "experiment") {
      if (argc != 5 || std::string(argv[3]) != "--manifest" ||
          (std::string(argv[2]) != "capture" &&
           std::string(argv[2]) != "head" &&
           std::string(argv[2]) != "pilot" &&
           std::string(argv[2]) != "classify")) {
        usage();
        return 2;
      }
      auto probe = std::filesystem::absolute(argv[0]).parent_path() /
                   "repotraverse-compiler-probe";
#ifdef _WIN32
      probe += ".exe";
#endif
      const auto action = std::string(argv[2]);
      const auto result =
          action == "capture"
              ? history::run_capture_experiment(argv[4], probe)
              : action == "head"
                    ? history::run_head_experiment(argv[4], probe)
                    : action == "pilot"
                          ? history::run_pilot_experiment(argv[4], probe)
                          : history::run_stability_experiment(argv[4]);
      std::cout << history::canonical_json(result) << '\n';
      return 0;
    }
    if (command == "serve") {
      if (argc != 4 || std::string(argv[2]) != "--config") {
        usage();
        return 2;
      }
      return run_service(argv[3]);
    }
#ifdef _WIN32
    if (command == "service") {
      if (argc != 4 || std::string(argv[2]) != "--config") {
        usage();
        return 2;
      }
      windows_service_config = argv[3];
      SERVICE_TABLE_ENTRYW table[] = {
          {const_cast<wchar_t *>(L"Repotraverse"), service_entry},
          {nullptr, nullptr}};
      if (!StartServiceCtrlDispatcherW(table))
        throw std::runtime_error("cannot connect to Windows Service Control Manager");
      return 0;
    }
#endif
    if (command == "status") {
      if (argc == 4 && std::string(argv[2]) == "--endpoint") {
        std::cout << history::canonical_json(history::http_status(argv[3]));
        return 0;
      }
      if (argc != 2) {
        usage();
        return 2;
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
           {"file_history", "parallel_endpoints_v1"},
           {"build_context_adapter", "armcc5_armclang6_v1"},
           {"background_worker", "git_task_batches_v1"},
           {"lineage_model", "cpp_elements_reviewed_relations_v1"},
           {"persistent_raw_source", false}});
      return 0;
    }
    if (command == "identity" && argc == 5 &&
        std::string(argv[2]) == "init" &&
        std::string(argv[3]) == "--catalog") {
      history::Catalog catalog(argv[4]);
      std::cout << history::canonical_json(
          {{"schema_version", history::kSchemaVersion},
           {"ok", true},
           {"producer_id", catalog.producer_id()},
           {"catalog", std::filesystem::absolute(argv[4]).string()}});
      return 0;
    }
    if (command == "benchmark") {
      std::size_t elements = 50'000;
      if (argc == 4 && std::string(argv[2]) == "--elements") {
        elements = std::stoull(argv[3]);
      } else if (argc != 2) {
        usage();
        return 2;
      }
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
    if (command == "facts" && argc >= 3 &&
        std::string(argv[2]) == "canonicalize") {
      if (argc >= 4) {
        std::ifstream input(argv[3]);
        if (!input)
          throw std::runtime_error("cannot open input file");
        std::cout << history::canonical_json(read_json(input));
      } else {
        std::cout << history::canonical_json(read_json(std::cin));
      }
      return 0;
    }
    if (command == "query") {
      nlohmann::json request;
      std::string endpoint;
      if (argc == 6 && std::string(argv[2]) == "--endpoint" &&
          std::string(argv[4]) == "--request") {
        endpoint = argv[3];
        std::ifstream input(argv[5]);
        if (!input)
          throw std::runtime_error("cannot open request file");
        request = read_json(input);
      } else if (argc == 4 && std::string(argv[2]) == "--endpoint") {
        endpoint = argv[3];
        request = read_json(std::cin);
      } else if (argc == 4 && std::string(argv[2]) == "--request") {
        std::ifstream input(argv[3]);
        if (!input)
          throw std::runtime_error("cannot open request file");
        request = read_json(input);
      } else if (argc == 2) {
        request = read_json(std::cin);
      } else {
        usage();
        return 2;
      }
      const auto response =
          endpoint.empty() ? history::QueryService(
                                 std::make_shared<history::MemoryFactStore>())
                                 .execute(request)
                           : history::http_query(endpoint, request);
      std::cout << history::canonical_json(response);
      return response.value("ok", false) ? 0 : 1;
    }
    usage();
    return 2;
  } catch (const std::exception &error) {
    std::cout << history::canonical_json(
        {{"schema_version", history::kSchemaVersion},
         {"ok", false},
         {"error", {{"code", "fatal"}, {"message", error.what()}}}});
    return 1;
  }
}
