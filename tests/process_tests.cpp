#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "history/encoding.hpp"
#include "history/process.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace

int process_test_main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--sleep") {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "--output") {
    std::cout << std::string(1024 * 1024, 'x');
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "--unicode-child") {
    const auto environment =
        history::environment_utf8("REPOTRAVERSE_UNICODE_TEST")
            .value_or(std::string{});
    std::cout << argv[2] << '\n'
              << history::path_to_utf8(std::filesystem::current_path()) << '\n'
              << environment << '\n';
    return 0;
  }
#ifdef _WIN32
  if (argc == 3 && std::string(argv[1]) == "--handle-must-be-closed") {
    const auto value = static_cast<std::uintptr_t>(std::stoull(argv[2]));
    DWORD flags = 0;
    return GetHandleInformation(reinterpret_cast<HANDLE>(value), &flags) ? 9
                                                                         : 0;
  }
  if (argc == 3 && std::string(argv[1]) == "--count-environment") {
    const auto wanted = history::utf8_to_wide(argv[2]);
    std::size_t count = 0;
    const auto block = GetEnvironmentStringsW();
    if (!block)
      return 10;
    for (auto cursor = block; *cursor;) {
      std::wstring entry(cursor);
      cursor += entry.size() + 1;
      const auto separator = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
      if (separator != std::wstring::npos) {
        const auto name = entry.substr(0, separator);
        if (CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
                                 wanted.data(), static_cast<int>(wanted.size()),
                                 TRUE) == CSTR_EQUAL)
          ++count;
      }
    }
    FreeEnvironmentStringsW(block);
    std::cout << count << '\n'
              << history::environment_utf8(argv[2]).value_or(std::string{})
              << '\n';
    return 0;
  }
#endif
  try {
    bool invalid_environment_rejected = false;
    try {
      history::ProcessOptions invalid;
      invalid.environment["INVALID=NAME"] = "value";
      (void)history::run_process({argv[0], "--sleep"}, invalid);
    } catch (const std::invalid_argument &) {
      invalid_environment_rejected = true;
    }
    require(invalid_environment_rejected,
            "invalid environment name was accepted");

    history::ProcessOptions timeout;
    timeout.timeout = std::chrono::milliseconds(100);
    const auto timed = history::run_process({argv[0], "--sleep"}, timeout);
    require(timed.timed_out, "process deadline was not enforced");

    history::ProcessOptions bounded;
    bounded.timeout = std::chrono::seconds(5);
    bounded.max_output_bytes = 4096;
    const auto oversized = history::run_process({argv[0], "--output"}, bounded);
    require(oversized.output_truncated,
            "process output limit was not enforced");
    require(oversized.output.size() <= bounded.max_output_bytes,
            "bounded process output exceeded its limit");

    const auto unicode_directory = std::filesystem::temp_directory_path() /
                                   history::path_from_utf8("한글 경로");
    std::filesystem::create_directories(unicode_directory);
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    } cleanup{unicode_directory};
    history::ProcessOptions unicode;
    unicode.working_directory = unicode_directory;
    unicode.environment["REPOTRAVERSE_UNICODE_TEST"] = "환경 값 🚀";
    const auto test_executable =
        std::filesystem::absolute(history::path_from_utf8(argv[0]));
    const auto round_trip =
        history::run_process({history::path_to_utf8(test_executable),
                              "--unicode-child", "인수 값 🚀"},
                             unicode);
    require(round_trip.exit_code == 0, "Unicode child process failed");
#ifdef _WIN32
    constexpr std::string_view newline = "\r\n";
#else
    constexpr std::string_view newline = "\n";
#endif
    const auto expected = std::string("인수 값 🚀") + std::string(newline) +
                          history::path_to_utf8(unicode_directory) +
                          std::string(newline) + "환경 값 🚀" +
                          std::string(newline);
    require(round_trip.output == expected,
            "Unicode process boundary did not round-trip exactly");
#ifdef _WIN32
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr,
                                    TRUE};
    const auto sentinel = CreateEventW(&inheritable, TRUE, FALSE, nullptr);
    require(sentinel != nullptr, "could not create inheritable sentinel");
    const auto sentinel_value = std::to_string(
        reinterpret_cast<std::uintptr_t>(sentinel));
    const auto inherited = history::run_process(
        {history::path_to_utf8(test_executable), "--handle-must-be-closed",
         sentinel_value});
    CloseHandle(sentinel);
    require(inherited.exit_code == 0,
            "child inherited a handle outside its explicit handle list");

    history::ProcessOptions environment_case;
    environment_case.environment["pAtH"] = "C:\\한글 도구";
    const auto environment_result = history::run_process(
        {history::path_to_utf8(test_executable), "--count-environment", "PATH"},
        environment_case);
    require(environment_result.exit_code == 0,
            "environment inspection child failed");
    require(environment_result.output == "1\r\nC:\\한글 도구\r\n",
            "environment override was not case-insensitive");
#endif
    std::cout << "process tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wide_argv) {
  std::vector<std::string> arguments;
  for (int index = 0; index < argc; ++index)
    arguments.push_back(history::wide_to_utf8(wide_argv[index]));
  std::vector<char *> argv;
  for (auto &argument : arguments)
    argv.push_back(argument.data());
  return process_test_main(argc, argv.data());
}
#else
int main(int argc, char **argv) { return process_test_main(argc, argv); }
#endif
