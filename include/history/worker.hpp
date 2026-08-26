#pragma once
#include "history/catalog.hpp"
#include "history/git_coordination.hpp"
#include "history/revision_workspace.hpp"
#include <chrono>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
namespace history {
struct WorkerOptions {
  std::filesystem::path extractor, scratch_root, source_repository;
  std::string repository_id;
  std::uint32_t max_attempts{10};
  std::chrono::seconds extractor_timeout{1800};
  std::uint64_t max_manifest_bytes{256ULL * 1024ULL * 1024ULL};
  std::shared_ptr<RevisionWorkspacePool> workspace_pool;
};
class BackgroundWorker {
public:
  BackgroundWorker(Catalog &, GitCoordinator &, WorkerOptions);
  nlohmann::json run_once();
  nlohmann::json run_task(const nlohmann::json &task);
  const std::string &extractor_identity() const { return extractor_identity_; }
  const std::string &repository_id() const { return options_.repository_id; }
  const std::filesystem::path &source_repository() const {
    return options_.source_repository;
  }

private:
  Catalog &catalog_;
  GitCoordinator &coordinator_;
  WorkerOptions options_;
  std::string extractor_identity_;
  std::mutex mutex_;
};
} // namespace history
