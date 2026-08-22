#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace history {

nlohmann::json run_capture_experiment(
    const std::filesystem::path &manifest,
    const std::filesystem::path &compiler_probe);
nlohmann::json run_head_experiment(
    const std::filesystem::path &manifest,
    const std::filesystem::path &compiler_probe);
nlohmann::json run_pilot_experiment(
    const std::filesystem::path &manifest,
    const std::filesystem::path &compiler_probe);

} // namespace history
