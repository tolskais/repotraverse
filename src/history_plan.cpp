#include "history/history_plan.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "history/ir.hpp"

namespace history {
namespace {

struct ProcessResult {
  int exit_code{};
  std::string error;
};

struct ChangedPath {
  std::string status;
  std::string path;
  std::string old_path;
};

struct CommitRecord {
  std::string commit;
  std::string parent;
  std::string tree;
  std::int64_t committer_time{};
  std::string subject;
  std::vector<ChangedPath> changes;
};

std::filesystem::path temporary_sibling(const std::filesystem::path &path,
                                        std::string_view role) {
  static std::atomic<unsigned long long> sequence{};
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  auto result = path;
  result += "." + std::string(role) + "." + std::to_string(tick) + "." +
            std::to_string(sequence.fetch_add(1)) + ".tmp";
  return result;
}

#ifdef _WIN32
std::wstring wide(std::string_view value) {
  if (value.empty())
    return {};
  const auto size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0)
    throw std::runtime_error("cannot convert process argument to UTF-16");
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size);
  return result;
}

std::wstring quote_windows_argument(const std::wstring &argument) {
  if (!argument.empty() &&
      argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    return argument;
  std::wstring result{L'"'};
  std::size_t slashes = 0;
  for (const auto character : argument) {
    if (character == L'\\') {
      ++slashes;
    } else if (character == L'"') {
      result.append(slashes * 2 + 1, L'\\');
      result.push_back(L'"');
      slashes = 0;
    } else {
      result.append(slashes, L'\\');
      slashes = 0;
      result.push_back(character);
    }
  }
  result.append(slashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

ProcessResult run_to_file(const std::vector<std::string> &arguments,
                          const std::filesystem::path &output) {
  if (arguments.empty())
    throw std::invalid_argument("process arguments cannot be empty");
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE error_read = nullptr;
  HANDLE error_write = nullptr;
  if (!CreatePipe(&error_read, &error_write, &attributes, 0) ||
      !SetHandleInformation(error_read, HANDLE_FLAG_INHERIT, 0))
    throw std::runtime_error("cannot create process error pipe");
  const auto output_handle =
      CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output_handle == INVALID_HANDLE_VALUE) {
    CloseHandle(error_read);
    CloseHandle(error_write);
    throw std::runtime_error("cannot create process output file");
  }

  std::wstring command_line;
  for (const auto &argument : arguments) {
    if (!command_line.empty())
      command_line.push_back(L' ');
    command_line += quote_windows_argument(wide(argument));
  }
  std::vector<wchar_t> mutable_command(command_line.begin(),
                                       command_line.end());
  mutable_command.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = output_handle;
  startup.hStdError = error_write;
  PROCESS_INFORMATION process{};
  const auto created =
      CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(output_handle);
  CloseHandle(error_write);
  if (!created) {
    CloseHandle(error_read);
    throw std::runtime_error("cannot start process");
  }

  ProcessResult result;
  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(error_read, buffer, sizeof(buffer), &read, nullptr) &&
         read != 0)
    result.error.append(buffer, read);
  CloseHandle(error_read);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  result.exit_code = static_cast<int>(exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return result;
}
#else
ProcessResult run_to_file(const std::vector<std::string> &arguments,
                          const std::filesystem::path &output) {
  if (arguments.empty())
    throw std::invalid_argument("process arguments cannot be empty");
  int error_pipe[2];
  if (pipe(error_pipe) != 0)
    throw std::runtime_error("cannot create process error pipe");
  const auto output_fd =
      open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (output_fd < 0) {
    close(error_pipe[0]);
    close(error_pipe[1]);
    throw std::runtime_error("cannot create process output file");
  }
  const auto child = fork();
  if (child < 0) {
    close(output_fd);
    close(error_pipe[0]);
    close(error_pipe[1]);
    throw std::runtime_error("cannot fork process");
  }
  if (child == 0) {
    dup2(output_fd, STDOUT_FILENO);
    dup2(error_pipe[1], STDERR_FILENO);
    close(output_fd);
    close(error_pipe[0]);
    close(error_pipe[1]);
    std::vector<char *> values;
    values.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
      values.push_back(const_cast<char *>(argument.c_str()));
    values.push_back(nullptr);
    execvp(values.front(), values.data());
    _exit(127);
  }
  close(output_fd);
  close(error_pipe[1]);
  ProcessResult result;
  char buffer[4096];
  ssize_t read_count = 0;
  while ((read_count = read(error_pipe[0], buffer, sizeof(buffer))) > 0)
    result.error.append(buffer, static_cast<std::size_t>(read_count));
  close(error_pipe[0]);
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  return result;
}
#endif

std::string strip_record_prefix(std::string value) {
  while (!value.empty() && (value.front() == '\n' || value.front() == '\r'))
    value.erase(value.begin());
  return value;
}

std::vector<std::string> split_header(const std::string &value) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  for (int count = 0; count < 5; ++count) {
    const auto end = value.find('\t', begin);
    if (end == std::string::npos)
      throw std::runtime_error("invalid Git history record");
    fields.push_back(value.substr(begin, end - begin));
    begin = end + 1;
  }
  fields.push_back(value.substr(begin));
  return fields;
}

bool next_token(std::istream &input, std::string &value) {
  return static_cast<bool>(std::getline(input, value, '\0'));
}

void read_git_history(const std::filesystem::path &input_path,
                      const std::function<void(CommitRecord &&)> &consume) {
  std::ifstream input(input_path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read Git history output");
  std::optional<CommitRecord> current;
  std::string token;
  while (next_token(input, token)) {
    token = strip_record_prefix(std::move(token));
    if (token.empty())
      continue;
    if (token.starts_with("C\t")) {
      if (current)
        consume(std::move(*current));
      const auto fields = split_header(token);
      current.emplace();
      current->commit = fields[1];
      const auto first_parent_end = fields[2].find(' ');
      current->parent = fields[2].substr(0, first_parent_end);
      current->tree = fields[3];
      current->committer_time = std::stoll(fields[4]);
      current->subject = fields[5];
      continue;
    }
    if (!current)
      throw std::runtime_error("Git path record precedes commit record");
    ChangedPath change;
    change.status = token;
    std::string first_path;
    if (!next_token(input, first_path))
      throw std::runtime_error("missing Git path record");
    if (!change.status.empty() &&
        (change.status.front() == 'R' || change.status.front() == 'C')) {
      change.old_path = std::move(first_path);
      if (!next_token(input, change.path))
        throw std::runtime_error("missing renamed Git path");
    } else {
      change.path = std::move(first_path);
    }
    current->changes.push_back(std::move(change));
  }
  if (current)
    consume(std::move(*current));
}

struct PrFacts {
  std::map<std::string, std::vector<nlohmann::json>> matches;
  std::set<std::string> no_pr;
};

PrFacts read_pr_facts(const std::filesystem::path &path) {
  PrFacts result;
  if (path.empty())
    return result;
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot open PR facts: " + path.string());
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.find_first_not_of(" \t\r\n") == std::string::npos)
      continue;
    const auto fact = nlohmann::json::parse(line);
    if (fact.value("pr_mapping_status", std::string{}) == "no_pr") {
      if (!fact.contains("commit") || !fact.at("commit").is_string())
        throw std::runtime_error("no_pr fact lacks commit at line " +
                                 std::to_string(line_number));
      result.no_pr.insert(fact.at("commit").get<std::string>());
      continue;
    }
    if (!fact.contains("pr_id"))
      throw std::runtime_error("PR fact lacks pr_id at line " +
                               std::to_string(line_number));
    std::set<std::string> commits;
    for (const auto *key : {"result_commit", "merge_result_commit"})
      if (fact.contains(key) && fact.at(key).is_string())
        commits.insert(fact.at(key).get<std::string>());
    if (fact.contains("associated_commits") &&
        fact.at("associated_commits").is_array())
      for (const auto &commit : fact.at("associated_commits"))
        if (commit.is_string())
          commits.insert(commit.get<std::string>());
    if (commits.empty())
      throw std::runtime_error("PR fact lacks an associated commit at line " +
                               std::to_string(line_number));
    nlohmann::json normalized = {{"pr_id", fact.at("pr_id")}};
    for (const auto *key :
         {"title", "description", "state", "source", "base_commit",
          "result_commit", "jira_issues", "issue_keys", "author", "reviewers",
          "created_at", "updated_at"})
      if (fact.contains(key))
        normalized[key] = fact.at(key);
    for (const auto &commit : commits)
      result.matches[commit].push_back(normalized);
  }
  for (const auto &commit : result.no_pr)
    if (result.matches.contains(commit))
      throw std::runtime_error("commit has both no_pr and positive PR facts: " +
                               commit);
  return result;
}

bool source_path(std::string path) {
  std::transform(path.begin(), path.end(), path.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  const auto extension = std::filesystem::path(path).extension().string();
  static const std::set<std::string> extensions = {
      ".asm", ".c",   ".cc",  ".cpp", ".cxx", ".h", ".hh",
      ".hpp", ".hxx", ".inc", ".inl", ".ipp", ".s", ".tpp"};
  return extensions.contains(extension);
}

bool build_path(std::string path) {
  std::transform(path.begin(), path.end(), path.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  const auto filename = std::filesystem::path(path).filename().string();
  const auto extension = std::filesystem::path(path).extension().string();
  return filename.starts_with("makefile") || filename == "gnumakefile" ||
         filename == "cmakelists.txt" || extension == ".cmake" ||
         extension == ".mk" || extension == ".mak" || extension == ".sln" ||
         extension == ".vcxproj" || extension == ".props" ||
         extension == ".targets";
}

nlohmann::json change_json(const ChangedPath &change,
                           const std::string &commit) {
  nlohmann::json value = {
      {"status", change.status}, {"path", change.path}, {"at_commit", commit}};
  if (!change.old_path.empty())
    value["old_path"] = change.old_path;
  return value;
}

std::string pr_key(const nlohmann::json &match) {
  return match.at("pr_id").is_string() ? match.at("pr_id").get<std::string>()
                                       : match.at("pr_id").dump();
}

} // namespace

nlohmann::json write_history_plan(const HistoryPlanOptions &options) {
  if (options.repository.empty())
    throw std::runtime_error("history.plan requires repository");
  if (options.output.empty())
    throw std::runtime_error("history.plan requires output");
  if (options.ref.empty() || options.ref.starts_with('-'))
    throw std::runtime_error("invalid history ref");
  if (!options.start_exclusive.empty() &&
      options.start_exclusive.starts_with('-'))
    throw std::runtime_error("invalid start_exclusive revision");
  if (!std::filesystem::is_directory(options.repository))
    throw std::runtime_error("repository is not a directory: " +
                             options.repository.string());
  if (std::filesystem::exists(options.output) &&
      !std::filesystem::is_regular_file(options.output))
    throw std::runtime_error("history plan output is not a regular file: " +
                             options.output.string());
  if (!options.output.parent_path().empty())
    std::filesystem::create_directories(options.output.parent_path());

  const auto pr_facts = read_pr_facts(options.pr_facts);
  const auto git_output = temporary_sibling(options.output, "git");
  const auto plan_output = temporary_sibling(options.output, "plan");
  struct Cleanup {
    std::filesystem::path first, second;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove(first, ignored);
      std::filesystem::remove(second, ignored);
    }
  } cleanup{git_output, plan_output};

  std::vector<std::string> command = {
      "git",
      "-c",
      "i18n.logOutputEncoding=UTF-8",
      "-C",
      options.repository.string(),
      "log",
      "--first-parent",
      "--reverse",
      "--no-decorate",
      "--format=C%x09%H%x09%P%x09%T%x09%ct%x09%s",
      "--name-status",
      "-M",
      "-z"};
  command.push_back(options.start_exclusive.empty()
                        ? options.ref
                        : options.start_exclusive + ".." + options.ref);
  command.push_back("--");
  const auto process = run_to_file(command, git_output);
  if (process.exit_code != 0)
    throw std::runtime_error("git history traversal failed: " + process.error);

  std::ofstream output(plan_output, std::ios::binary);
  if (!output)
    throw std::runtime_error("cannot create history plan: " +
                             plan_output.string());
  output << canonical_json(
      {{"schema_version", kSchemaVersion},
       {"record_type", "history_plan"},
       {"repository", std::filesystem::absolute(options.repository)
                          .lexically_normal()
                          .string()},
       {"ref", options.ref},
       {"start_exclusive", options.start_exclusive},
       {"traversal", "first_parent"}});

  std::size_t commits = 0, units = 0, changed_paths = 0;
  std::size_t identified = 0, ambiguous = 0, no_pr = 0, unknown = 0;
  std::size_t source_units = 0, build_units = 0;
  std::string resolved_head;
  nlohmann::json pending;
  std::string pending_pr;

  const auto flush = [&] {
    if (pending.is_null())
      return;
    if (pending["has_source_changes"].get<bool>())
      ++source_units;
    if (pending["has_build_changes"].get<bool>())
      ++build_units;
    pending["sequence"] = units++;
    output << canonical_json(pending);
    pending = nullptr;
    pending_pr.clear();
  };

  read_git_history(git_output, [&](CommitRecord &&commit) {
    ++commits;
    resolved_head = commit.commit;
    changed_paths += commit.changes.size();
    bool has_source = false, has_build = false;
    nlohmann::json changes = nlohmann::json::array();
    for (const auto &change : commit.changes) {
      changes.push_back(change_json(change, commit.commit));
      has_source = has_source || source_path(change.path) ||
                   source_path(change.old_path);
      has_build =
          has_build || build_path(change.path) || build_path(change.old_path);
    }
    const auto found = pr_facts.matches.find(commit.commit);
    const auto matches = found == pr_facts.matches.end()
                             ? std::vector<nlohmann::json>{}
                             : found->second;
    const bool confirmed_no_pr = pr_facts.no_pr.contains(commit.commit);
    const auto grouping_status =
        !matches.empty() ? (matches.size() == 1 ? "identified" : "ambiguous")
                         : (confirmed_no_pr ? "no_pr" : "unknown");
    if (confirmed_no_pr)
      ++no_pr;
    else if (matches.empty())
      ++unknown;
    else if (matches.size() == 1)
      ++identified;
    else
      ++ambiguous;
    const auto exact_pr =
        matches.size() == 1 ? pr_key(matches.front()) : std::string{};

    if (!pending.is_null() && !exact_pr.empty() && pending_pr == exact_pr) {
      pending["head_commit"] = commit.commit;
      pending["tree"] = commit.tree;
      pending["committer_time"] = commit.committer_time;
      pending["subject"] = commit.subject;
      pending["commits"].push_back(commit.commit);
      for (auto &change : changes)
        pending["changes"].push_back(std::move(change));
      pending["has_source_changes"] =
          pending["has_source_changes"].get<bool>() || has_source;
      pending["has_build_changes"] =
          pending["has_build_changes"].get<bool>() || has_build;
      return;
    }
    flush();
    pending_pr = exact_pr;
    pending = {{"schema_version", kSchemaVersion},
               {"record_type", "change_unit"},
               {"change_unit_id", exact_pr.empty()
                                      ? "commit:" + commit.commit
                                      : "bitbucket-pr:" + exact_pr},
               {"base_commit", commit.parent},
               {"head_commit", commit.commit},
               {"commits", nlohmann::json::array({commit.commit})},
               {"tree", commit.tree},
               {"committer_time", commit.committer_time},
               {"subject", commit.subject},
               {"grouping_status", grouping_status},
               {"pr_matches", matches},
               {"has_source_changes", has_source},
               {"has_build_changes", has_build},
               {"changes", std::move(changes)}};
  });
  flush();
  output << canonical_json({{"schema_version", kSchemaVersion},
                            {"record_type", "history_plan_summary"},
                            {"resolved_head", resolved_head},
                            {"commit_count", commits},
                            {"change_unit_count", units},
                            {"changed_path_count", changed_paths},
                            {"source_change_units", source_units},
                            {"build_change_units", build_units},
                            {"pr_identified_commits", identified},
                            {"pr_ambiguous_commits", ambiguous},
                            {"pr_no_pr_commits", no_pr},
                            {"pr_unknown_commits", unknown}});
  output.close();
  if (!output)
    throw std::runtime_error("cannot finish history plan");
  std::error_code error;
  std::filesystem::remove(options.output, error);
  error.clear();
  std::filesystem::rename(plan_output, options.output, error);
  if (error)
    throw std::runtime_error("cannot publish history plan: " + error.message());

  return {{"output", options.output.string()},
          {"ref", options.ref},
          {"resolved_head", resolved_head},
          {"commit_count", commits},
          {"change_unit_count", units},
          {"changed_path_count", changed_paths},
          {"source_change_units", source_units},
          {"build_change_units", build_units},
          {"pr_identified_commits", identified},
          {"pr_ambiguous_commits", ambiguous},
          {"pr_no_pr_commits", no_pr},
          {"pr_unknown_commits", unknown}};
}

} // namespace history
