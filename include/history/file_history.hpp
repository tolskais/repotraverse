#pragma once
#include "history/catalog.hpp"
#include <filesystem>
#include <nlohmann/json.hpp>
namespace history {
struct FileHistoryOptions {
  std::filesystem::path repository, pr_facts;
  std::string ref{"HEAD"}, path, scope{"direct"};
  std::string repository_id;
  std::string extractor_identity{"schema-v1"};
  std::int64_t since{};
  std::size_t offset{}, page_size{100};
};
nlohmann::json plan_file_history(Catalog &, const FileHistoryOptions &);
} // namespace history
