#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "history/lineage.hpp"

namespace history {

class FactStore {
public:
    virtual ~FactStore() = default;
    virtual EvidenceBundle load(const std::filesystem::path& path) const = 0;
};

class MemoryFactStore final : public FactStore {
public:
    EvidenceBundle load(const std::filesystem::path& path) const override;
};

class QueryService {
public:
    explicit QueryService(std::shared_ptr<const FactStore> store);
    nlohmann::json execute(const nlohmann::json& request) const;

private:
    std::shared_ptr<const FactStore> store_;
};

}  // namespace history
