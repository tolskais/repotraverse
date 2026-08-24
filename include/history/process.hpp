#pragma once

#include <filesystem>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <map>
#include <vector>

namespace history {

struct ProcessOutput {
  int exit_code{};
  std::string output;
  std::string error;
  bool timed_out{};
  bool output_truncated{};
  std::uint64_t cpu_time_ms{};
};

struct ProcessOptions {
  std::filesystem::path working_directory;
  std::map<std::string, std::string> environment;
  std::string_view input;
  std::chrono::milliseconds timeout{std::chrono::minutes(5)};
  std::size_t max_output_bytes{256ULL * 1024ULL * 1024ULL};
};

ProcessOutput run_process(const std::vector<std::string> &arguments,
                          const ProcessOptions &options);
// String arguments and environment overrides are UTF-8 on every platform.
void set_default_process_timeout(std::chrono::milliseconds timeout);

ProcessOutput run_process(const std::vector<std::string> &arguments,
                          const std::filesystem::path &working_directory = {},
                          std::string_view input = {});

} // namespace history
