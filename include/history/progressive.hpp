#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace history {

struct ProgressiveScreeningOptions {
  std::filesystem::path repository;
  std::filesystem::path output;
  std::vector<std::string> revisions;
  nlohmann::json partition;
  nlohmann::json budget;
};

// Parses one repository blob without a compile command. Returned identities are
// deliberately syntactic candidates and are never canonical C/C++ identities.
nlohmann::json parse_syntax_blob(std::string_view language,
                                 std::string_view path,
                                 std::string_view revision,
                                 std::string_view source);

// Performs Git file screening, syntax-site extraction, hunk mapping and
// deterministic budget allocation without creating a worktree.
nlohmann::json
plan_progressive_screening(const ProgressiveScreeningOptions &options);

} // namespace history
