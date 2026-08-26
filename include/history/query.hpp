#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "history/lineage.hpp"

namespace history {
class Catalog;
class GitCoordinator;
class BackgroundWorker;
class ConnectorService;

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
               std::shared_ptr<BackgroundWorker> worker = {},
               std::shared_ptr<ConnectorService> connectors = {});
  nlohmann::json execute(const nlohmann::json &request) const;

private:
  nlohmann::json execute_impl(const nlohmann::json &request) const;
  std::shared_ptr<const FactStore> store_;
  std::shared_ptr<Catalog> catalog_;
  std::shared_ptr<GitCoordinator> coordinator_;
  std::shared_ptr<BackgroundWorker> worker_;
  std::shared_ptr<ConnectorService> connectors_;
};

} // namespace history
