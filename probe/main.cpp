#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "history/build_info.hpp"
#include "history/ir.hpp"
#include "history/process.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

std::string environment(const char *name, bool required = false) {
  const auto *value = std::getenv(name);
  if (value && *value)
    return value;
  if (required)
    throw std::runtime_error(std::string("missing environment variable: ") +
                             name);
  return {};
}

bool source_extension(const std::filesystem::path &path) {
  const auto extension = path.extension().string();
  return extension == ".c" || extension == ".cc" || extension == ".cpp" ||
         extension == ".cxx" || extension == ".C" || extension == ".s" ||
         extension == ".S";
}

std::string repository_path(const std::filesystem::path &path,
                            const std::filesystem::path &repository,
                            const std::filesystem::path &cwd) {
  if (path.empty())
    return {};
  const auto absolute = path.is_absolute() ? path : cwd / path;
  std::error_code error;
  auto relative =
      std::filesystem::relative(absolute.lexically_normal(), repository, error);
  if (error || relative.empty() || *relative.begin() == "..")
    return {};
  return relative.generic_string();
}

void materialize(const std::filesystem::path &path,
                 const std::filesystem::path &cwd) {
  if (path.empty())
    return;
  const auto output = path.is_absolute() ? path : cwd / path;
  std::error_code error;
  std::filesystem::create_directories(output.parent_path(), error);
  std::ofstream placeholder(output, std::ios::binary | std::ios::app);
}

std::vector<std::string>
dependencies(const std::filesystem::path &dependency_output,
             const std::filesystem::path &repository,
             const std::filesystem::path &cwd) {
  std::vector<std::string> result;
  if (dependency_output.empty())
    return result;
  const auto path = dependency_output.is_absolute() ? dependency_output
                                                    : cwd / dependency_output;
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > 8ULL * 1024ULL * 1024ULL)
    return result;
  std::ifstream input(path, std::ios::binary);
  std::string content{std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>()};
  for (std::size_t position = 0;
       (position = content.find("\\\n", position)) != std::string::npos;)
    content.replace(position, 2, " ");
  const auto separator = content.find(": ");
  if (separator != std::string::npos)
    content.erase(0, separator + 2);
  std::string token;
  const auto emit = [&] {
    if (token.empty())
      return;
    const auto relative = repository_path(token, repository, cwd);
    if (!relative.empty())
      result.push_back(relative);
    token.clear();
  };
  for (const auto character : content) {
    if (character == ' ' || character == '\t' || character == '\r' ||
        character == '\n')
      emit();
    else
      token.push_back(character);
  }
  emit();
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--version") {
      std::cout << "repotraverse compiler-probe "
                << history::build::kToolVersion << " artifact v1\n";
      return 0;
    }
    const auto capture_directory = std::filesystem::path(
        environment("REPOTRAVERSE_CAPTURE_DIRECTORY", true));
    const auto repository = std::filesystem::weakly_canonical(
        environment("REPOTRAVERSE_CAPTURE_REPOSITORY", true));
    const auto cwd = std::filesystem::current_path();
    std::vector<std::string> arguments;
    std::vector<std::string> response_files;
    std::filesystem::path source, output, dependency_output;
    for (int index = 1; index < argc; ++index) {
      std::string argument = argv[index];
      arguments.push_back(argument);
      if (argument.starts_with('@'))
        response_files.push_back(argument.substr(1));
      if ((argument == "-o" || argument == "--output" ||
           argument == "--depend") &&
          index + 1 < argc) {
        const auto value = std::filesystem::path(argv[index + 1]);
        if (argument == "--depend")
          dependency_output = value;
        else
          output = value;
      } else if (argument.starts_with("--output=")) {
        output = argument.substr(9);
      } else if (argument.starts_with("--depend=")) {
        dependency_output = argument.substr(9);
      } else if (argument.size() > 2 && argument.starts_with("-o")) {
        output = argument.substr(2);
      } else if (!argument.starts_with('-') && source_extension(argument)) {
        source = argument;
      }
    }
    std::filesystem::create_directories(capture_directory);
    std::map<std::string, std::string> response_file_contents;
    for (const auto &response_file : response_files) {
      const auto path = std::filesystem::path(response_file).is_absolute()
                            ? std::filesystem::path(response_file)
                            : cwd / response_file;
      std::error_code error;
      const auto size = std::filesystem::file_size(path, error);
      if (error || size > 1024ULL * 1024ULL)
        throw std::runtime_error("response file is missing or exceeds 1 MiB");
      std::ifstream input(path, std::ios::binary);
      response_file_contents[response_file] =
          std::string(std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>());
    }
    nlohmann::json record = {
        {"schema_version", history::kSchemaVersion},
        {"configuration",
         environment("REPOTRAVERSE_CAPTURE_CONFIGURATION", true)},
        {"build_variant",
         {{"product", environment("REPOTRAVERSE_CAPTURE_PRODUCT")},
          {"target", environment("REPOTRAVERSE_CAPTURE_TARGET")},
          {"configuration",
           environment("REPOTRAVERSE_CAPTURE_BUILD_CONFIGURATION")}}},
        {"source_revision", environment("REPOTRAVERSE_CAPTURE_REVISION", true)},
        {"toolchain", environment("REPOTRAVERSE_CAPTURE_TOOLCHAIN", true)},
        {"working_directory", repository_path(cwd, repository, cwd)},
        {"translation_unit", repository_path(source, repository, cwd)},
        {"arguments", arguments},
        {"response_files", response_files},
        {"response_file_contents", response_file_contents},
        {"output", repository_path(output, repository, cwd)},
        {"environment",
         {{"LANG", environment("LANG")},
          {"ARMCC5INC", environment("ARMCC5INC")},
          {"ARMCC5LIB", environment("ARMCC5LIB")}}}};
    const auto tick =
        std::chrono::steady_clock::now().time_since_epoch().count();
    std::random_device random;
#ifdef _WIN32
    const auto process = _getpid();
#else
    const auto process = getpid();
#endif
    const auto destination =
        capture_directory /
        (std::to_string(tick) + "-" + std::to_string(process) + "-" +
         std::to_string(random()) + ".json");
    const auto real_compiler = environment("REPOTRAVERSE_REAL_COMPILER");
    std::optional<history::ProcessOutput> compiler_result;
    if (!real_compiler.empty()) {
      std::vector<std::string> command{real_compiler};
      command.insert(command.end(), arguments.begin(), arguments.end());
      history::ProcessOptions options;
      options.working_directory = cwd;
      compiler_result = history::run_process(command, options);
    } else {
      materialize(output, cwd);
      materialize(dependency_output, cwd);
    }
    record["project_files"] = dependencies(dependency_output, repository, cwd);
    std::ofstream captured(destination, std::ios::binary);
    captured << record.dump() << '\n';
    captured.close();
    if (!captured)
      throw std::runtime_error("cannot persist compiler invocation");
    if (compiler_result) {
      std::cout << compiler_result->output;
      std::cerr << compiler_result->error;
      return compiler_result->exit_code;
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "compiler probe: " << error.what() << '\n';
    return 2;
  }
}
