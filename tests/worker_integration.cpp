#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "history/catalog.hpp"
#include "history/file_history.hpp"
#include "history/git_coordination.hpp"
#include "history/process.hpp"
#include "history/query.hpp"
#include "history/worker.hpp"

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::string git(const std::filesystem::path &repository,
                std::vector<std::string> arguments) {
  std::vector<std::string> command = {"git"};
  if (!repository.empty()) {
    command.push_back("-C");
    command.push_back(repository.string());
  }
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto result = history::run_process(command);
  if (result.exit_code != 0)
    throw std::runtime_error("Git setup failed: " + result.error);
  auto output = result.output;
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
    output.pop_back();
  return output;
}
} // namespace

int main() {
  try {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("repotraverse-worker-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    } cleanup{root};
    std::filesystem::create_directories(root);

    const auto source = root / "source";
    std::filesystem::create_directories(source);
    git(source, {"init"});
    git(source, {"config", "user.name", "Test"});
    git(source, {"config", "user.email", "test@example.invalid"});
    {
      std::ofstream output(source / "device.cpp");
      output << "static int helper(int x) { return x + 1; }\n"
                "int device(int x) { return helper(x); }\n";
    }
    git(source, {"add", "device.cpp"});
    git(source, {"commit", "-m", "add device"});
    const auto revision = git(source, {"rev-parse", "HEAD"});

    const auto remote = root / "artifacts.git";
    const auto artifacts = root / "artifacts-first";
    const auto peer_artifacts = root / "artifacts-second";
    git({}, {"init", "--bare", remote.string()});
    git({}, {"clone", remote.string(), artifacts.string()});
    git({}, {"clone", remote.string(), peer_artifacts.string()});

    history::Catalog catalog(root / "catalog");
    history::CompileContext context;
    context.configuration = "arm-debug";
    context.source_revision = revision;
    context.translation_unit = "device.cpp";
    context.frontend_arguments = {"-std=c++17"};
    context.toolchain = "armclang6";
    context.adapter_version = "fixture";
    context.context_id = history::stable_hash("device.cpp\n-std=c++17");
    catalog.store_compile_context(context);

    history::GitCoordinator coordinator(
        catalog, history::CoordinationOptions{artifacts, "origin", 30, 0});
    history::Catalog peer_catalog(root / "peer-catalog");
    history::GitCoordinator peer_coordinator(
        peer_catalog,
        history::CoordinationOptions{peer_artifacts, "origin", 30, 0});
    history::WorkerOptions peer_options{EXTRACTOR_PATH, root / "scratch", source};
    peer_options.repository_id = "fixture-main";
    history::BackgroundWorker worker(peer_catalog, peer_coordinator,
                                     std::move(peer_options));
    history::WorkerOptions scheduler_options{
        EXTRACTOR_PATH, root / "scheduler-scratch", source};
    scheduler_options.repository_id = "fixture-main";
    auto scheduler_worker = std::make_shared<history::BackgroundWorker>(
        catalog, coordinator, std::move(scheduler_options));
    auto scheduler_catalog =
        std::shared_ptr<history::Catalog>(&catalog, [](auto *) {});
    auto scheduler_coordinator =
        std::shared_ptr<history::GitCoordinator>(&coordinator, [](auto *) {});
    history::QueryService service(std::make_shared<history::MemoryFactStore>(),
                                  scheduler_catalog, scheduler_coordinator,
                                  scheduler_worker);
    const nlohmann::json request = {{"schema_version", history::kSchemaVersion},
                                    {"query", "file.history"},
                                    {"params",
                                     {{"repository", source.string()},
                                      {"path", "device.cpp"},
                                      {"since", 1}}}};
    const auto planned_response = service.execute(request);
    require(planned_response.value("ok", false), "file query failed");
    const auto &planned = planned_response.at("result");
    require(planned.at("scheduled_tasks") == 1,
            "file query did not schedule extraction");
    require(planned.at("task_publication").value("state", std::string{}) ==
                "queued",
            "task batch was not queued");
    const auto published = coordinator.publish_tasks(planned.at("pending_work"));
    require(published.value("state", std::string{}) == "published",
            "task batch was not published by coordination loop");
    const auto peer_sync = peer_coordinator.sync();
    require(peer_sync.value("tasks_imported", std::size_t{}) == 1,
            "peer did not import published task");
    const auto worked = worker.run_once();
    require(worked.value("state", std::string{}) == "completed",
            "worker did not publish extraction");
    coordinator.sync();

    const auto completed_response = service.execute(request);
    require(completed_response.value("ok", false),
            "completed file query failed");
    const auto &completed = completed_response.at("result");
    require(completed.at("result_status") == "complete",
            "published extraction did not complete query");
    require(completed.at("analysis").at("element_snapshots").size() == 2,
            "worker manifest did not expose both functions");
    require(completed.at("pending_work").empty(),
            "completed extraction remained pending");
    std::cout << "worker integration passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
