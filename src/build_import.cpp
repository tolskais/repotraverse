#include "history/build_import.hpp"
#include "history/ir.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace history {
namespace {

bool source_argument(const std::string &value) {
  const auto extension = std::filesystem::path(value).extension().string();
  return extension == ".c" || extension == ".cc" || extension == ".cpp" ||
         extension == ".cxx" || extension == ".C" || extension == ".s" ||
         extension == ".S";
}

void gap(Coverage &coverage, std::string message) {
  coverage.status = "partial";
  if (std::find(coverage.gaps.begin(), coverage.gaps.end(), message) ==
      coverage.gaps.end())
    coverage.gaps.push_back(std::move(message));
}

std::filesystem::path safe_relative(std::filesystem::path path,
                                    const char *field) {
  path = path.lexically_normal();
  if (path.empty())
    return ".";
  if (path.is_absolute() || *path.begin() == "..")
    throw std::runtime_error(std::string("captured ") + field +
                             " is not repository-relative");
  return path;
}

std::string semantic_path(const std::string &value,
                          const std::filesystem::path &working_directory,
                          const std::filesystem::path &repository,
                          Coverage &coverage) {
  std::filesystem::path path(value);
  if (path.is_absolute()) {
    if (repository.empty()) {
      gap(coverage, "absolute semantic path cannot be made portable: " + value);
      return path.generic_string();
    }
    std::error_code error;
    path = std::filesystem::relative(path, repository, error);
    if (error || path.empty() || *path.begin() == "..") {
      gap(coverage, "semantic path is outside the repository: " + value);
      return value;
    }
  } else {
    path = working_directory / path;
  }
  return safe_relative(path, "semantic path").generic_string();
}

std::vector<std::string> tokenize_response(std::string_view input) {
  std::vector<std::string> result;
  std::string current;
  char quote = 0;
  bool escaped = false;
  for (const char character : input) {
    if (escaped) {
      current.push_back(character);
      escaped = false;
    } else if (character == '\\' && quote != '\'') {
      escaped = true;
    } else if (quote) {
      if (character == quote)
        quote = 0;
      else
        current.push_back(character);
    } else if (character == '\'' || character == '"') {
      quote = character;
    } else if (character == ' ' || character == '\t' || character == '\r' ||
               character == '\n') {
      if (!current.empty()) {
        result.push_back(std::move(current));
        current.clear();
      }
    } else {
      current.push_back(character);
    }
  }
  if (escaped || quote)
    throw std::runtime_error("unterminated escape or quote in response file");
  if (!current.empty())
    result.push_back(std::move(current));
  return result;
}

std::vector<std::string>
expand_response_files(const std::vector<std::string> &arguments,
                      const std::filesystem::path &working_directory,
                      const std::filesystem::path &repository,
                      const std::map<std::string, std::string> &embedded,
                      Coverage &coverage) {
  std::vector<std::string> result;
  for (const auto &argument : arguments) {
    if (!argument.starts_with('@')) {
      result.push_back(argument);
      continue;
    }
    const auto name = argument.substr(1);
    if (repository.empty() && !embedded.contains(name)) {
      gap(coverage, "response file requires repository path: " + argument);
      continue;
    }
    const auto relative = safe_relative(working_directory / name,
                                        "response file");
    std::string content;
    if (const auto found = embedded.find(name); found != embedded.end()) {
      content = found->second;
    } else {
      const auto path = repository / relative;
      std::error_code error;
      const auto size = std::filesystem::file_size(path, error);
      if (error || size > 1024ULL * 1024ULL)
        throw std::runtime_error("response file is missing or exceeds 1 MiB: " +
                                 relative.generic_string());
      std::ifstream input(path, std::ios::binary);
      content.assign(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
    }
    auto expanded = tokenize_response(content);
    if (std::any_of(expanded.begin(), expanded.end(),
                    [](const auto &value) { return value.starts_with('@'); }))
      throw std::runtime_error("nested response files are not supported");
    result.insert(result.end(), expanded.begin(), expanded.end());
    coverage.capabilities.push_back("response_files_expanded");
  }
  return result;
}

std::vector<std::string>
normalize(const std::vector<std::string> &captured,
          const std::filesystem::path &working_directory,
          const std::filesystem::path &repository,
          const std::map<std::string, std::string> &response_files,
          Coverage &coverage) {
  const auto input = expand_response_files(captured, working_directory,
                                           repository, response_files, coverage);
  std::vector<std::string> out;
  const auto path_pair = [&](const std::string &option, const std::string &value) {
    out.push_back(option);
    out.push_back(semantic_path(value, working_directory, repository, coverage));
  };
  for (std::size_t i = 0; i < input.size(); ++i) {
    const auto &a = input[i];
    if (a == "-o" || a == "--output" || a == "--depend" ||
        a == "--depend_dir" || a == "--list" || a == "--list_dir") {
      if (i + 1 < input.size())
        ++i;
      continue;
    }
    if (a == "-c" || a == "-E" || a == "-S" || a.starts_with("-W") ||
        a.starts_with("-O") || a.starts_with("-g") ||
        a.starts_with("--diag_") || a.starts_with("--remarks") ||
        a.starts_with("--depend=") || a.starts_with("--list=") ||
        a.starts_with("--output=") ||
        (a.starts_with("-o") && a.size() > 2))
      continue;
    if ((a == "--cpu" || a == "--fpu") && i + 1 < input.size()) {
      out.push_back(std::string(a == "--cpu" ? "-mcpu=" : "-mfpu=") +
                    input[++i]);
      continue;
    }
    if (a.starts_with("--cpu=")) {
      out.push_back("-mcpu=" + a.substr(6));
      continue;
    }
    if (a.starts_with("--fpu=")) {
      out.push_back("-mfpu=" + a.substr(6));
      continue;
    }
    if (a == "--cpp") {
      out.insert(out.end(), {"-x", "c++"});
      continue;
    }
    if (a == "--c99") {
      out.push_back("-std=c99");
      continue;
    }
    if (a == "--preinclude" && i + 1 < input.size()) {
      path_pair("-include", input[++i]);
      continue;
    }
    static const std::map<std::string, std::string> translations = {
        {"--signed_chars", "-fsigned-char"},
        {"--unsigned_chars", "-funsigned-char"},
        {"--enum_is_int", "-fno-short-enums"},
        {"--wchar32", "-fno-short-wchar"},
        {"--exceptions", "-fexceptions"},
        {"--no_exceptions", "-fno-exceptions"},
        {"--rtti", "-frtti"},
        {"--no_rtti", "-fno-rtti"}};
    if (const auto found = translations.find(a); found != translations.end()) {
      out.push_back(found->second);
      continue;
    }
    if (a.starts_with("--apcs")) {
      gap(coverage, "unsupported armcc APCS option: " + a);
      continue;
    }
    if (a == "-I" || a == "-isystem" || a == "-include") {
      if (i + 1 < input.size())
        path_pair(a, input[++i]);
      else
        gap(coverage, "missing value for semantic option: " + a);
      continue;
    }
    if (a == "-D" || a == "-U" || a == "-x" || a == "--target" ||
        a == "--sysroot") {
      if (i + 1 >= input.size()) {
        gap(coverage, "missing value for semantic option: " + a);
      } else if (a == "--target") {
        out.push_back("--target=" + input[++i]);
      } else if (a == "--sysroot") {
        path_pair("--sysroot", input[++i]);
      } else {
        out.push_back(a);
        out.push_back(input[++i]);
      }
      continue;
    }
    if (a.starts_with("-I") && a.size() > 2) {
      out.push_back("-I" + semantic_path(a.substr(2), working_directory,
                                          repository, coverage));
      continue;
    }
    if (a.starts_with("-D") || a.starts_with("-U") ||
        a.starts_with("-std=") || a.starts_with("--target=") ||
        a == "-mthumb" || a == "-marm" || a == "-mbig-endian" ||
        a == "-mlittle-endian" || a.starts_with("-mcpu=") ||
        a.starts_with("-mfpu=") || a.starts_with("-mfloat-abi=")) {
      out.push_back(a);
      continue;
    }
    if (a.starts_with('-')) {
      gap(coverage, "untranslated frontend option: " + a);
      continue;
    }
    if (!source_argument(a))
      gap(coverage, "unclassified compiler input: " + a);
  }
  return out;
}

std::vector<nlohmann::json> load_records(const std::filesystem::path &input) {
  std::vector<nlohmann::json> records;
  if (std::filesystem::is_directory(input)) {
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(input))
      if (entry.is_regular_file() && entry.path().extension() == ".json")
        files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const auto &file : files) {
      std::ifstream stream(file);
      records.push_back(nlohmann::json::parse(stream));
    }
    return records;
  }
  std::ifstream stream(input);
  if (!stream)
    throw std::runtime_error("cannot open captured build log");
  std::string line;
  while (std::getline(stream, line)) {
    if (line.find_first_not_of(" \t\r\n") == std::string::npos)
      continue;
    if (line.size() > 8ULL * 1024ULL * 1024ULL)
      throw std::runtime_error("captured build record exceeds size limit");
    records.push_back(nlohmann::json::parse(line));
  }
  return records;
}

} // namespace

nlohmann::json import_build_log(Catalog &catalog,
                                const std::filesystem::path &input,
                                const std::filesystem::path &repository) {
  std::size_t imported = 0, partial = 0;
  std::set<std::string> configurations, contexts, translation_units;
  for (const auto &record : load_records(input)) {
    if (record.value("schema_version", 0U) != kSchemaVersion)
      throw std::runtime_error("captured build record requires schema_version 1");
    CompileContext context;
    context.configuration = record.at("configuration").get<std::string>();
    context.source_revision = record.at("source_revision").get<std::string>();
    context.translation_unit = record.at("translation_unit").get<std::string>();
    context.toolchain = record.at("toolchain").get<std::string>();
    context.working_directory =
        safe_relative(record.value("working_directory", "."),
                      "working directory")
            .generic_string();
    const auto tu = safe_relative(context.translation_unit, "translation unit");
    if (context.configuration.empty() || context.configuration.size() > 256 ||
        context.source_revision.empty() || context.source_revision.starts_with('-') ||
        context.translation_unit.empty() ||
        (context.toolchain != "armcc5" && context.toolchain != "armclang6"))
      throw std::runtime_error("captured build record has invalid identity fields");
    context.translation_unit = tu.generic_string();
    context.adapter_version =
        context.toolchain == "armcc5" ? "armcc5_v1" : "armclang6_v1";
    context.target = record.value("target", BuildTarget{});
    context.project_files =
        record.value("project_files",
                     record.value("dependencies", std::vector<std::string>{}));
    context.coverage.capabilities = {"normalized_frontend_arguments",
                                     "captured_working_directory"};
    if (!context.project_files.empty())
      context.coverage.capabilities.push_back("project_dependency_map");
    else
      gap(context.coverage, "project dependency map was not captured");
    context.frontend_arguments = normalize(
        record.at("arguments").get<std::vector<std::string>>(),
        context.working_directory, repository,
        record.value("response_file_contents",
                     std::map<std::string, std::string>{}),
        context.coverage);
    if (context.frontend_arguments.size() > 4096 ||
        std::any_of(context.frontend_arguments.begin(),
                    context.frontend_arguments.end(), [](const auto &argument) {
                      return argument.size() > 32768;
                    }))
      throw std::runtime_error("captured frontend arguments exceed limits");
    for (auto &project_file : context.project_files)
      project_file = safe_relative(project_file, "project path").generic_string();
    const nlohmann::json identity = {
        {"translation_unit", context.translation_unit},
        {"working_directory", context.working_directory},
        {"target", context.target},
        {"arguments", context.frontend_arguments},
        {"toolchain", context.toolchain},
        {"adapter", context.adapter_version}};
    context.context_id = stable_hash(identity.dump());
    catalog.store_compile_context(context);
    configurations.insert(context.configuration);
    contexts.insert(context.context_id);
    translation_units.insert(context.translation_unit);
    if (context.coverage.status != "complete")
      ++partial;
    ++imported;
  }
  return {{"schema_version", kSchemaVersion},
          {"records", imported},
          {"translation_units", translation_units.size()},
          {"configurations", configurations.size()},
          {"distinct_contexts", contexts.size()},
          {"partial_contexts", partial}};
}

} // namespace history
