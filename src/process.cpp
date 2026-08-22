#include "history/process.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/resource.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace history {
namespace {

std::atomic<std::int64_t> default_timeout_ms{300000};

std::filesystem::path temporary_path(const char *role) {
  static std::atomic<unsigned long long> sequence{};
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  std::random_device random;
  return std::filesystem::temp_directory_path() /
         ("repotraverse-" + std::string(role) + "-" + std::to_string(tick) +
          "-" + std::to_string(random()) + "-" +
          std::to_string(sequence.fetch_add(1)) + ".tmp");
}

std::string read_file(const std::filesystem::path &path, std::size_t maximum,
                      bool &truncated) {
  std::ifstream input(path, std::ios::binary);
  std::string result;
  result.resize(maximum);
  input.read(result.data(), static_cast<std::streamsize>(maximum));
  result.resize(static_cast<std::size_t>(input.gcount()));
  truncated = input.peek() != std::char_traits<char>::eof();
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

std::wstring quote(const std::wstring &argument) {
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

std::vector<wchar_t>
environment_block(const std::map<std::string, std::string> &overrides) {
  if (overrides.empty())
    return {};
  std::map<std::wstring, std::wstring> values;
  const auto inherited = GetEnvironmentStringsW();
  if (!inherited)
    throw std::runtime_error("cannot read process environment");
  for (auto cursor = inherited; *cursor;) {
    std::wstring entry(cursor);
    cursor += entry.size() + 1;
    const auto separator = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
    if (separator != std::wstring::npos)
      values[entry.substr(0, separator)] = entry.substr(separator + 1);
  }
  FreeEnvironmentStringsW(inherited);
  for (const auto &[name, value] : overrides)
    values[wide(name)] = wide(value);
  std::vector<wchar_t> block;
  for (const auto &[name, value] : values) {
    block.insert(block.end(), name.begin(), name.end());
    block.push_back(L'=');
    block.insert(block.end(), value.begin(), value.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}
#endif

} // namespace

ProcessOutput run_process(const std::vector<std::string> &arguments,
                          const ProcessOptions &options) {
  if (arguments.empty())
    throw std::invalid_argument("process arguments cannot be empty");
  const auto temporary_root = temporary_path("process");
  if (!std::filesystem::create_directory(temporary_root))
    throw std::runtime_error("cannot create private process scratch directory");
  std::error_code permission_error;
  std::filesystem::permissions(temporary_root,
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace,
                               permission_error);
  const auto out_path = temporary_root / "stdout";
  const auto err_path = temporary_root / "stderr";
  const auto in_path = temporary_root / "stdin";
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
  } cleanup{temporary_root};
  {
    std::ofstream stream(in_path, std::ios::binary);
    if (!stream)
      throw std::runtime_error("cannot create process input file");
    stream.write(options.input.data(),
                 static_cast<std::streamsize>(options.input.size()));
  }

#ifdef _WIN32
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  const auto out_handle =
      CreateFileW(out_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  const auto err_handle =
      CreateFileW(err_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  const auto in_handle =
      CreateFileW(in_path.c_str(), GENERIC_READ, FILE_SHARE_READ, &attributes,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out_handle == INVALID_HANDLE_VALUE ||
      err_handle == INVALID_HANDLE_VALUE || in_handle == INVALID_HANDLE_VALUE) {
    if (out_handle != INVALID_HANDLE_VALUE)
      CloseHandle(out_handle);
    if (err_handle != INVALID_HANDLE_VALUE)
      CloseHandle(err_handle);
    if (in_handle != INVALID_HANDLE_VALUE)
      CloseHandle(in_handle);
    throw std::runtime_error("cannot create process output files");
  }
  std::wstring command_line;
  for (const auto &argument : arguments) {
    if (!command_line.empty())
      command_line.push_back(L' ');
    command_line += quote(wide(argument));
  }
  std::vector<wchar_t> command(command_line.begin(), command_line.end());
  command.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = in_handle;
  startup.hStdOutput = out_handle;
  startup.hStdError = err_handle;
  PROCESS_INFORMATION process{};
  const auto cwd = options.working_directory.empty()
                       ? std::wstring{}
                       : options.working_directory.wstring();
  const auto job = CreateJobObjectW(nullptr, nullptr);
  if (!job) {
    CloseHandle(out_handle);
    CloseHandle(err_handle);
    CloseHandle(in_handle);
    throw std::runtime_error("cannot create process job");
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                          sizeof(limits));
  auto environment = environment_block(options.environment);
  const auto created = CreateProcessW(
      nullptr, command.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
      environment.empty() ? nullptr : environment.data(),
      cwd.empty() ? nullptr : cwd.c_str(), &startup, &process);
  CloseHandle(out_handle);
  CloseHandle(err_handle);
  CloseHandle(in_handle);
  if (!created) {
    CloseHandle(job);
    throw std::runtime_error("cannot start process");
  }
  if (!AssignProcessToJobObject(job, process.hProcess)) {
    TerminateProcess(process.hProcess, 126);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(job);
    throw std::runtime_error("cannot assign process to containment job");
  }
  ResumeThread(process.hThread);
  ProcessOutput result;
  const auto deadline = std::chrono::steady_clock::now() + options.timeout;
  for (;;) {
    if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0)
      break;
    std::error_code ignored;
    const auto out_size = std::filesystem::file_size(out_path, ignored);
    ignored.clear();
    const auto err_size = std::filesystem::file_size(err_path, ignored);
    if (!ignored && (out_size > options.max_output_bytes ||
                     err_size > options.max_output_bytes)) {
      result.output_truncated = true;
      TerminateJobObject(job, 125);
      WaitForSingleObject(process.hProcess, 5000);
      break;
    }
    if (options.timeout.count() > 0 &&
        std::chrono::steady_clock::now() >= deadline) {
      result.timed_out = true;
      TerminateJobObject(job, 124);
      WaitForSingleObject(process.hProcess, 5000);
      break;
    }
  }
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  FILETIME created_time{}, exited_time{}, kernel_time{}, user_time{};
  if (GetProcessTimes(process.hProcess, &created_time, &exited_time,
                      &kernel_time, &user_time)) {
    ULARGE_INTEGER kernel{}, user{};
    kernel.LowPart = kernel_time.dwLowDateTime;
    kernel.HighPart = kernel_time.dwHighDateTime;
    user.LowPart = user_time.dwLowDateTime;
    user.HighPart = user_time.dwHighDateTime;
    result.cpu_time_ms = (kernel.QuadPart + user.QuadPart) / 10000ULL;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(job);
  result.exit_code = static_cast<int>(exit_code);
#else
  const auto child = fork();
  if (child < 0)
    throw std::runtime_error("cannot fork process");
  if (child == 0) {
    setpgid(0, 0);
    if (!options.working_directory.empty() &&
        chdir(options.working_directory.c_str()) != 0)
      _exit(126);
    const auto out = fopen(out_path.c_str(), "wb");
    const auto err = fopen(err_path.c_str(), "wb");
    const auto in = fopen(in_path.c_str(), "rb");
    if (!out || !err || !in)
      _exit(126);
    dup2(fileno(in), STDIN_FILENO);
    dup2(fileno(out), STDOUT_FILENO);
    dup2(fileno(err), STDERR_FILENO);
    fclose(out);
    fclose(err);
    fclose(in);
    for (const auto &[name, value] : options.environment)
      if (setenv(name.c_str(), value.c_str(), 1) != 0)
        _exit(126);
    std::vector<char *> values;
    values.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
      values.push_back(const_cast<char *>(argument.c_str()));
    values.push_back(nullptr);
    execvp(values.front(), values.data());
    _exit(127);
  }
  setpgid(child, child);
  int status = 0;
  rusage usage{};
  const auto deadline = std::chrono::steady_clock::now() + options.timeout;
  bool reaped = false;
  bool exceeded = false;
  for (;;) {
    const auto waited = wait4(child, &status, WNOHANG, &usage);
    if (waited == child) {
      reaped = true;
      break;
    }
    if (waited < 0 && errno != EINTR)
      break;
    std::error_code ignored;
    const auto out_size = std::filesystem::file_size(out_path, ignored);
    ignored.clear();
    const auto err_size = std::filesystem::file_size(err_path, ignored);
    exceeded = (!ignored && (out_size > options.max_output_bytes ||
                             err_size > options.max_output_bytes));
    const bool expired = options.timeout.count() > 0 &&
                         std::chrono::steady_clock::now() >= deadline;
    if (expired || exceeded) {
      kill(-child, SIGKILL);
      while (wait4(child, &status, 0, &usage) < 0 && errno == EINTR) {
      }
      reaped = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ProcessOutput result;
  result.timed_out = !exceeded && options.timeout.count() > 0 &&
                     std::chrono::steady_clock::now() >= deadline;
  result.output_truncated = exceeded;
  result.exit_code = reaped && WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  result.cpu_time_ms =
      static_cast<std::uint64_t>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) *
          1000ULL +
      static_cast<std::uint64_t>(usage.ru_utime.tv_usec +
                                 usage.ru_stime.tv_usec) /
          1000ULL;
#endif
  bool out_truncated = false, err_truncated = false;
  result.output = read_file(out_path, options.max_output_bytes, out_truncated);
  result.error = read_file(err_path, options.max_output_bytes, err_truncated);
  result.output_truncated =
      result.output_truncated || out_truncated || err_truncated;
  return result;
}

void set_default_process_timeout(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0)
    throw std::invalid_argument("default process timeout must be positive");
  default_timeout_ms.store(timeout.count());
}

ProcessOutput run_process(const std::vector<std::string> &arguments,
                          const std::filesystem::path &working_directory,
                          std::string_view input) {
  ProcessOptions options;
  options.timeout = std::chrono::milliseconds(default_timeout_ms.load());
  options.working_directory = working_directory;
  options.input = input;
  return run_process(arguments, options);
}

} // namespace history
