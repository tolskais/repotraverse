#include "history/worker.hpp"
#include <condition_variable>
#include "history/encoding.hpp"
#include "history/ir.hpp"
#include "history/process.hpp"
#include "history/telemetry.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
namespace history {
namespace {
std::string trim(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
    value.pop_back();
  return value;
}
void git_ok(const ProcessOutput &result, const char *action) {
  if (result.exit_code)
    throw std::runtime_error(std::string(action) + ": " + result.error);
}
bool safe_frontend_argument(const std::string &argument) {
  if (argument.empty() || argument.front() == '@' || argument == "-Xclang" ||
      argument == "-load" || argument == "-plugin" ||
      argument.starts_with("-fplugin") || argument.starts_with("-MJ"))
    return false;
  return argument == "-D" || argument == "-U" || argument == "-I" ||
         argument == "-include" || argument == "-isystem" || argument == "-x" ||
         argument == "--sysroot" || argument == "-fsigned-char" ||
         argument == "-funsigned-char" || argument == "-fno-short-enums" ||
         argument == "-fno-short-wchar" || argument == "-fexceptions" ||
         argument == "-fno-exceptions" || argument == "-frtti" ||
         argument == "-fno-rtti" || argument.starts_with("-D") ||
         argument.starts_with("-U") || argument.starts_with("-I") ||
         argument.starts_with("-std=") || argument.starts_with("--target=") ||
         argument.starts_with("-m") || argument.front() != '-';
}
} // namespace
BackgroundWorker::BackgroundWorker(Catalog &c, GitCoordinator &g,
                                   WorkerOptions o)
    : catalog_(c), coordinator_(g), options_(std::move(o)) {
  if (options_.extractor.empty() || options_.scratch_root.empty())
    throw std::invalid_argument("worker requires extractor and scratch_root");
  const auto version = run_process({path_to_utf8(options_.extractor), "--version"});
  git_ok(version, "identify extractor");
  extractor_identity_ = stable_hash(trim(version.output));
  std::filesystem::create_directories(options_.scratch_root);
  if (!options_.workspace_pool) {
    WorkspaceLimits limits;
    limits.free_space_reserve_bytes = 0;
    options_.workspace_pool =
        std::make_shared<RevisionWorkspacePool>(options_.scratch_root, limits);
  }
}
nlohmann::json BackgroundWorker::run_once() {
  const auto pending = catalog_.next_pending_task();
  if (!pending)
    return {{"state", "idle"}};
  return run_task(*pending);
}
nlohmann::json BackgroundWorker::run_task(const nlohmann::json &task) {
  std::scoped_lock worker_lock(mutex_);
  const auto pending = &task;
  const auto task_id = pending->at("task_id").get<std::string>();
  Telemetry::instance().increment("worker.tasks_started");
  try {
    if (pending->at("identity").value("extractor_identity", std::string{}) !=
        extractor_identity_) {
      catalog_.set_task_state(task_id, "incompatible_worker");
      return {{"state", "incompatible_worker"}, {"task_id", task_id}};
    }
    const auto claim = coordinator_.acquire(pending->at("identity"));
    const auto state = claim.value("state", std::string{});
    if (state == "completed") {
      catalog_.set_task_state(task_id, "completed");
      return claim;
    }
    if (state != "processing" && state != "processing_local") {
      catalog_.set_task_state(task_id, "waiting");
      return {{"state", state}, {"task_id", task_id}};
    }
    catalog_.set_task_state(task_id, "processing");
    const auto repository =
        options_.source_repository.empty()
            ? path_from_utf8(pending->at("repository").get<std::string>())
            : options_.source_repository;
    const auto revision = pending->at("source_commit").get<std::string>();
    if (revision.empty() || revision.starts_with('-'))
      throw std::runtime_error("invalid source revision");
    if (!options_.repository_id.empty() &&
        pending->value("repository_id", options_.repository_id) !=
            options_.repository_id)
      throw std::runtime_error("task repository identity mismatch");
    const auto tu = pending->at("translation_unit").get<std::string>();
    const auto relative = path_from_utf8(tu).lexically_normal();
    if (relative.is_absolute() || relative.empty() || *relative.begin() == "..")
      throw std::runtime_error("translation unit must be repository-relative");
    auto materialization = pending->value(
        "materialization",
        MaterializationManifest{{generic_path_to_utf8(relative)},
                                false,
                                {"legacy task has no dependency closure"}});
    if (std::find(materialization.files.begin(), materialization.files.end(),
                  generic_path_to_utf8(relative)) == materialization.files.end())
      materialization.files.push_back(generic_path_to_utf8(relative));
    auto workspace =
        options_.workspace_pool->acquire(repository, revision, materialization,
                                         !materialization.closure_complete);
    const auto &worktree = workspace.path();
    const RevisionTreeIndex tree(repository, revision);
    const auto blob = tree.blob_at(generic_path_to_utf8(relative));
    if (!blob)
      throw std::runtime_error(
          "resolve source blob: path is absent at revision");
    std::vector<std::string> command = {
        path_to_utf8(options_.extractor),
        "--source-revision",
        revision,
        "--configuration",
        pending->at("configurations").dump(),
        "--build-variant",
        nlohmann::json(pending->value("build_variant", BuildVariant{})).dump(),
        "--context-fingerprint",
        pending->at("context_id").get<std::string>(),
        "--source-blob",
        *blob,
        "--project-root",
        path_to_utf8(worktree),
        "--repository-id",
        options_.repository_id,
        path_to_utf8(worktree / relative),
        "--"};
    for (const auto &argument : pending->at("frontend_arguments")) {
      const auto value = argument.get<std::string>();
      if (!safe_frontend_argument(value))
        throw std::runtime_error("unsafe frontend argument");
      command.push_back(value);
    }
    const auto heartbeat_period = coordinator_.heartbeat_interval();
    std::mutex heartbeat_mutex;
    std::condition_variable_any heartbeat_changed;
    std::jthread heartbeat([&](std::stop_token stop) {
      while (!stop.stop_requested()) {
        std::unique_lock lock(heartbeat_mutex);
        heartbeat_changed.wait_for(lock, stop, heartbeat_period,
                                   [] { return false; });
        if (stop.stop_requested()) break;
        lock.unlock();
        try {
          coordinator_.heartbeat(task_id);
        } catch (...) {
        }
      }
    });
    ProcessOptions process_options;
    process_options.working_directory = worktree;
    process_options.timeout = options_.extractor_timeout;
    process_options.max_output_bytes =
        static_cast<std::size_t>(options_.max_manifest_bytes);
    process_options.cancellation_requested = [this, task_id] {
      return catalog_.work_cancellation_requested(task_id);
    };
    const auto process = run_process(command, process_options);
    heartbeat.request_stop();
    heartbeat_changed.notify_all();
    heartbeat.join();
    if (process.cancelled)
      return {{"state", "cancelled"}, {"task_id", task_id}};
    nlohmann::json result;
    if (process.exit_code == 0 && !process.timed_out &&
        !process.output_truncated) {
      result = nlohmann::json::parse(process.output);
      if (result.value("record_type", std::string{}) != "tu_manifest")
        throw std::runtime_error("extractor did not return TU manifest");
      const auto manifest = result.get<TuManifest>();
      std::string manifest_error;
      if (!validate_tu_manifest(manifest, manifest_error))
        throw std::runtime_error(manifest_error);
      if (manifest.source_revision != revision ||
          manifest.translation_unit != generic_path_to_utf8(relative) ||
          manifest.context_id != pending->at("context_id").get<std::string>() ||
          (!options_.repository_id.empty() &&
           manifest.repository_id != options_.repository_id))
        throw std::runtime_error("extractor manifest identity mismatch");
    } else if (process.timed_out || process.output_truncated) {
      throw std::runtime_error(process.timed_out
                                   ? "extractor_timeout"
                                   : "extractor_output_limit");
    } else
      result = {
          {"schema_version", kSchemaVersion},
          {"record_type", "tu_failure"},
          {"source_revision", revision},
          {"translation_unit", tu},
          {"context_id", pending->at("context_id")},
          {"coverage",
           {{"status", "partial"},
            {"gaps",
             {std::string(process.timed_out ? "extractor timed out; "
                          : process.output_truncated
                              ? "extractor output limit exceeded; "
                              : "extractor failed; ") +
              "diagnostics fingerprint: " + stable_hash(process.error)}}}}};
    const auto completed = coordinator_.complete(task_id, result);
    if (completed.value("state", std::string{}) == "completed")
      catalog_.set_task_state(task_id, "completed");
    else
      catalog_.set_task_state(task_id, "pending");
    if (completed.value("state", std::string{}) == "completed")
      Telemetry::instance().increment("worker.tasks_completed");
    return completed;
  } catch (const std::exception &error) {
    Telemetry::instance().increment("worker.task_failures");
    const auto failure = catalog_.fail_task(task_id, stable_hash(error.what()),
                                            options_.max_attempts);
    const std::string diagnostic = error.what();
    const auto error_code = diagnostic.starts_with("disk_space_insufficient")
                                ? "disk_space_insufficient"
                                : "worker_failure";
    return {{"state", failure.at("state")},
            {"task_id", task_id},
            {"error", {{"code", error_code}}},
            {"attempt_count", failure.at("attempt_count")},
            {"next_attempt_at", failure.at("next_attempt_at")},
            {"diagnostic_fingerprint", stable_hash(error.what())}};
  }
}
} // namespace history
