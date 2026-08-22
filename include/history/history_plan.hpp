#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace history {

struct HistoryPlanOptions {
  std::filesystem::path repository;
  std::string ref{"HEAD"};
  std::string start_exclusive;
  std::filesystem::path pr_facts;
  std::filesystem::path output;
};

nlohmann::json write_history_plan(const HistoryPlanOptions &options);

} // namespace history
