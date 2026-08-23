#pragma once

#include <filesystem>

#include <nlohmann/json.hpp>

namespace history {

nlohmann::json summarize_change_evidence(
    const std::filesystem::path &repository,
    const nlohmann::json &revision_reports, const nlohmann::json &budget);

} // namespace history
