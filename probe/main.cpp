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
#include "history/encoding.hpp"
#include "history/ir.hpp"
#include "history/process.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

std::string environment(const char *name, bool required = false) {
  const auto value = history::environment_utf8(name);
  if (value && !value->empty())
    return *value;
  if (required)
    throw std::runtime_error(std::string("missing environment variable: ") +
                             name);
  return {};
}

bool source_extension(const std::filesystem::path &path) {
  const auto extension = history::path_to_utf8(path.extension());
  return extension == ".c" || extension == ".cc" || extension == ".cpp" ||
         extension == ".cxx" || extension == ".C" || extension == ".s" ||
         extension == ".S";
}

std::optional<std::filesystem::path>
dependency_argument(const std::string &argument) {
  for (const auto &prefix :
       {std::string{"-MF"}, std::string{"--dependency-file="}})
    if (argument.starts_with(prefix) && argument.size() > prefix.size())
      return history::path_from_utf8(argument.substr(prefix.size()));
  if (argument.starts_with("-Wp,")) {
    std::vector<std::string> parts;
    std::string part;
    for (const auto character : argument.substr(4)) {
      if (character == ',') {
        parts.push_back(std::move(part));
        part.clear();
      } else {
        part.push_back(character);
      }
    }
    parts.push_back(std::move(part));
    for (std::size_t index = 0; index < parts.size(); ++index) {
      if ((parts[index] == "-MD" || parts[index] == "-MMD" ||
           parts[index] == "-MF") &&
          index + 1 < parts.size() && !parts[index + 1].empty())
        return history::path_from_utf8(parts[index + 1]);
      if (parts[index].starts_with("-MF") && parts[index].size() > 3)
        return history::path_from_utf8(parts[index].substr(3));
    }
  }
  return std::nullopt;
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
  return history::generic_path_to_utf8(relative);
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

struct CapturedDependencies {
  std::vector<std::string> paths;
  std::string encoding;
};

CapturedDependencies
dependencies(const std::filesystem::path &dependency_output,
             const std::filesystem::path &repository,
             const std::filesystem::path &cwd) {
  std::vector<std::string> result;
  if (dependency_output.empty())
    return {result, {}};
  const auto path = dependency_output.is_absolute() ? dependency_output
                                                    : cwd / dependency_output;
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > 8ULL * 1024ULL * 1024ULL)
    return {result, {}};
  std::ifstream input(path, std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
  auto decoded = history::decode_text(bytes, true);
  auto &content = decoded.text;
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
    const auto relative =
        repository_path(history::path_from_utf8(token), repository, cwd);
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
  return {std::move(result), std::move(decoded.encoding)};
}

} // namespace

int compiler_probe_main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--version") {
      std::cout << "repotraverse compiler-probe "
                << history::build::kToolVersion << " artifact v1\n";
      return 0;
    }
    const auto capture_directory = history::path_from_utf8(
        environment("REPOTRAVERSE_CAPTURE_DIRECTORY", true));
    const auto repository = std::filesystem::weakly_canonical(
        history::path_from_utf8(
            environment("REPOTRAVERSE_CAPTURE_REPOSITORY", true)));
    const auto cwd = std::filesystem::current_path();
    std::vector<std::string> arguments;
    std::vector<std::string> response_files;
    std::filesystem::path source, output, dependency_output;
    bool implicit_dependency_output = false;
    for (int index = 1; index < argc; ++index) {
      std::string argument = argv[index];
      arguments.push_back(argument);
      if (argument.starts_with('@'))
        response_files.push_back(argument.substr(1));
      if ((argument == "-o" || argument == "--output" ||
           argument == "--depend" || argument == "-MF" ||
           argument == "--dependency-file") &&
          index + 1 < argc) {
        const auto value = history::path_from_utf8(argv[index + 1]);
        if (argument == "--depend" || argument == "-MF" ||
            argument == "--dependency-file")
          dependency_output = value;
        else
          output = value;
      } else if (argument.starts_with("--output=")) {
        output = history::path_from_utf8(argument.substr(9));
      } else if (argument.starts_with("--depend=")) {
        dependency_output = history::path_from_utf8(argument.substr(9));
      } else if (argument.size() > 2 && argument.starts_with("-o")) {
        output = history::path_from_utf8(argument.substr(2));
      } else if (const auto dependency = dependency_argument(argument)) {
        dependency_output = *dependency;
      } else if (argument == "-MD" || argument == "-MMD") {
        implicit_dependency_output = true;
      } else if (!argument.starts_with('-') &&
                 source_extension(history::path_from_utf8(argument))) {
        source = history::path_from_utf8(argument);
      }
    }
    if (dependency_output.empty() && implicit_dependency_output &&
        !output.empty()) {
      dependency_output = output;
      dependency_output.replace_extension(".d");
    }
    std::filesystem::create_directories(capture_directory);
    std::map<std::string, std::string> response_file_contents;
    std::map<std::string, std::string> response_file_encodings;
    for (const auto &response_file : response_files) {
      const auto response_path = history::path_from_utf8(response_file);
      const auto path = response_path.is_absolute() ? response_path
                                                    : cwd / response_path;
      std::error_code error;
      const auto size = std::filesystem::file_size(path, error);
      if (error || size > 1024ULL * 1024ULL)
        throw std::runtime_error("response file is missing or exceeds 1 MiB");
      auto decoded = history::read_text_file(path, true);
      response_file_contents[response_file] = std::move(decoded.text);
      response_file_encodings[response_file] = std::move(decoded.encoding);
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
        {"response_file_encodings", response_file_encodings},
        {"output", repository_path(output, repository, cwd)},
        {"environment",
         {{"LANG", environment("LANG")},
          {"ARMCC5INC", environment("ARMCC5INC")},
          {"ARMCC5LIB", environment("ARMCC5LIB")}}}};
    record["invocation_kind"] =
        record.at("translation_unit").get<std::string>().empty()
            ? "non_translation_unit"
            : "translation_unit";
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
    auto captured_dependencies =
        dependencies(dependency_output, repository, cwd);
    record["project_files"] = std::move(captured_dependencies.paths);
    if (!captured_dependencies.encoding.empty())
      record["dependency_file_encoding"] =
          std::move(captured_dependencies.encoding);
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

#ifdef _WIN32
int wmain(int argc, wchar_t **wide_argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index)
    arguments.push_back(history::wide_to_utf8(wide_argv[index]));
  std::vector<char *> argv;
  argv.reserve(arguments.size());
  for (auto &argument : arguments)
    argv.push_back(argument.data());
  return compiler_probe_main(argc, argv.data());
}
#else
int main(int argc, char **argv) { return compiler_probe_main(argc, argv); }
#endif
