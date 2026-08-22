#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace history {

nlohmann::json run_stability_experiment(
    const std::filesystem::path &manifest_path);

} // namespace history
