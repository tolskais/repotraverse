#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "history/lineage.hpp"

namespace history {
class Catalog;
class GitCoordinator;
class BackgroundWorker;

class FactStore {
public:
  virtual ~FactStore() = default;
  virtual EvidenceBundle load(const std::filesystem::path &path) const = 0;
};

class MemoryFactStore final : public FactStore {
public:
  EvidenceBundle load(const std::filesystem::path &path) const override;
};

class QueryService {
public:
  explicit QueryService(std::shared_ptr<const FactStore> store);
  QueryService(std::shared_ptr<const FactStore> store,
               std::shared_ptr<Catalog> catalog,
               std::shared_ptr<GitCoordinator> coordinator,
               std::shared_ptr<BackgroundWorker> worker = {});
  nlohmann::json execute(const nlohmann::json &request) const;
  nlohmann::json enqueue(const nlohmann::json &request) const;
  nlohmann::json submit(const nlohmann::json &request) const;
  nlohmann::json request_status(const std::string &request_id,
                                bool refresh = true) const;
  nlohmann::json cancel_request(const std::string &request_id) const;

private:
  std::shared_ptr<const FactStore> store_;
  std::shared_ptr<Catalog> catalog_;
  std::shared_ptr<GitCoordinator> coordinator_;
  std::shared_ptr<BackgroundWorker> worker_;
};

} // namespace history
