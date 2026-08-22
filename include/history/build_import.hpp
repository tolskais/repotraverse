#pragma once
#include "history/catalog.hpp"
#include <filesystem>
#include <nlohmann/json.hpp>
namespace history {
nlohmann::json import_build_log(Catalog &catalog,
                                const std::filesystem::path &input,
                                const std::filesystem::path &repository = {});
}
