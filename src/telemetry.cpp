#include "history/telemetry.hpp"
#include "history/ir.hpp"

#include <chrono>
#include <iostream>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#endif

namespace history {
namespace {
void write_event_log(const std::string &payload, bool error);
}

Telemetry &Telemetry::instance() {
  static Telemetry telemetry;
  return telemetry;
}

Telemetry::~Telemetry() {
  if (exporter_.joinable()) {
    exporter_.request_stop();
    changed_.notify_all();
  }
}

void Telemetry::configure(std::string endpoint, std::string service_name) {
  std::scoped_lock lock(mutex_);
  endpoint_ = std::move(endpoint);
  service_name_ = std::move(service_name);
  if (!endpoint_.empty() && !exporter_.joinable())
    exporter_ = std::jthread([this](std::stop_token stop) { export_loop(stop); });
}

void Telemetry::increment(const std::string &name, std::int64_t amount) {
  std::scoped_lock lock(mutex_);
  counters_[name] += amount;
}

void Telemetry::gauge(const std::string &name, std::int64_t value) {
  std::scoped_lock lock(mutex_);
  gauges_[name] = value;
}

nlohmann::json Telemetry::snapshot() const {
  std::scoped_lock lock(mutex_);
  return {{"counters", counters_}, {"gauges", gauges_}};
}

void Telemetry::log(const std::string &level, const std::string &event,
                    nlohmann::json fields) {
  fields["timestamp_unix_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  fields["level"] = level;
  fields["event"] = event;
  std::scoped_lock lock(mutex_);
  const auto serialized = fields.dump();
  std::cerr << serialized << '\n';
#ifdef _WIN32
  if (level == "warning" || level == "error" || level == "fatal")
    write_event_log(serialized, level != "warning");
#endif
  if (!endpoint_.empty()) {
    if (pending_logs_.size() >= 1000) {
      pending_logs_.pop_front();
      ++counters_["telemetry.logs_dropped"];
    }
    pending_logs_.push_back(std::move(fields));
    changed_.notify_all();
  }
}

void Telemetry::span(const std::string &name, std::int64_t duration_ms,
                     nlohmann::json attributes) {
  const auto ended = std::chrono::system_clock::now();
  const auto ended_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            ended.time_since_epoch())
                            .count();
  const auto started_ns = ended_ns - duration_ms * 1000000;
  const auto seed = name + std::to_string(ended_ns) + attributes.dump();
  nlohmann::json span = {
      {"traceId", stable_hash(seed)},
      {"spanId", stable_hash("span\n" + seed).substr(0, 16)},
      {"name", name},
      {"kind", 1},
      {"startTimeUnixNano", std::to_string(started_ns)},
      {"endTimeUnixNano", std::to_string(ended_ns)},
      {"attributes",
       {{{"key", "repotraverse.attributes"},
         {"value", {{"stringValue", attributes.dump()}}}}}}};
  std::scoped_lock lock(mutex_);
  if (endpoint_.empty()) return;
  if (pending_spans_.size() >= 1000) {
    pending_spans_.pop_front();
    ++counters_["telemetry.spans_dropped"];
  }
  pending_spans_.push_back(std::move(span));
  changed_.notify_all();
}

namespace {
#ifdef _WIN32
std::wstring wide(std::string_view value) {
  if (value.empty()) return {};
  const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        value.data(),
                                        static_cast<int>(value.size()),
                                        nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size);
  return result;
}

bool post_otlp(const std::string &endpoint, const wchar_t *suffix,
               const nlohmann::json &value) {
  const auto url = wide(endpoint);
  if (url.empty()) return false;
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) ||
      parts.nScheme != INTERNET_SCHEME_HTTPS)
    return false;
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (!path.empty() && path.back() == L'/') path.pop_back();
  path += suffix;
  const auto session = WinHttpOpen(
      L"repotraverse/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
  const auto connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
  const auto request = connection
                           ? WinHttpOpenRequest(
                                 connection, L"POST", path.c_str(), nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE)
                           : nullptr;
  const auto payload = value.dump();
  const wchar_t headers[] = L"Content-Type: application/json\r\n";
  const bool sent = request &&
                    WinHttpSendRequest(
                        request, headers, static_cast<DWORD>(-1),
                        const_cast<char *>(payload.data()),
                        static_cast<DWORD>(payload.size()),
                        static_cast<DWORD>(payload.size()), 0) &&
                    WinHttpReceiveResponse(request, nullptr);
  DWORD status = 0, status_size = sizeof(status);
  const bool accepted = sent &&
                        WinHttpQueryHeaders(
                            request,
                            WINHTTP_QUERY_STATUS_CODE |
                                WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                            WINHTTP_NO_HEADER_INDEX) &&
                        status >= 200 && status < 300;
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return accepted;
}
void write_event_log(const std::string &payload, bool error) {
  const auto source = RegisterEventSourceW(nullptr, L"Repotraverse");
  if (!source) return;
  const auto message = wide(payload);
  const wchar_t *messages[] = {message.c_str()};
  ReportEventW(source, error ? EVENTLOG_ERROR_TYPE : EVENTLOG_WARNING_TYPE, 0,
               1, nullptr, 1, 0, messages, nullptr);
  DeregisterEventSource(source);
}
#else
bool post_otlp(const std::string &, const wchar_t *, const nlohmann::json &) {
  return false;
}
void write_event_log(const std::string &, bool) {}
#endif

std::string unix_nanos() {
  return std::to_string(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}
} // namespace

void Telemetry::export_loop(std::stop_token stop) {
  while (!stop.stop_requested()) {
    std::deque<nlohmann::json> logs;
    std::deque<nlohmann::json> spans;
    std::map<std::string, std::int64_t> counters, gauges;
    std::string endpoint, service;
    {
      std::unique_lock lock(mutex_);
      changed_.wait_for(lock, stop, std::chrono::seconds(5),
                        [this] { return !pending_logs_.empty(); });
      logs.swap(pending_logs_);
      spans.swap(pending_spans_);
      counters = counters_;
      gauges = gauges_;
      endpoint = endpoint_;
      service = service_name_;
    }
    if (endpoint.empty()) continue;
    const auto resource = nlohmann::json{
        {"attributes", {{{"key", "service.name"},
                          {"value", {{"stringValue", service}}}}}}};
    if (!logs.empty()) {
      nlohmann::json records = nlohmann::json::array();
      for (const auto &entry : logs)
        records.push_back({{"timeUnixNano", unix_nanos()},
                           {"severityText", entry.value("level", "INFO")},
                           {"body", {{"stringValue", entry.dump()}}}});
      const nlohmann::json payload = {
          {"resourceLogs",
           {{{"resource", resource},
             {"scopeLogs", {{{"scope", {{"name", "repotraverse"}}},
                              {"logRecords", records}}}}}}}};
      if (!post_otlp(endpoint, L"/v1/logs", payload)) {
        std::scoped_lock lock(mutex_);
        counters_["telemetry.export_failures"]++;
      }
    }
    if (!spans.empty()) {
      const nlohmann::json payload = {
          {"resourceSpans",
           {{{"resource", resource},
             {"scopeSpans", {{{"scope", {{"name", "repotraverse"}}},
                               {"spans", spans}}}}}}}};
      if (!post_otlp(endpoint, L"/v1/traces", payload)) {
        std::scoped_lock lock(mutex_);
        counters_["telemetry.export_failures"]++;
      }
    }
    nlohmann::json metrics = nlohmann::json::array();
    for (const auto &[name, value] : counters)
      metrics.push_back({{"name", name},
                         {"sum", {{"aggregationTemporality", 2},
                                  {"isMonotonic", true},
                                  {"dataPoints", {{{"timeUnixNano", unix_nanos()},
                                                   {"asInt", std::to_string(value)}}}}}}});
    for (const auto &[name, value] : gauges)
      metrics.push_back({{"name", name},
                         {"gauge", {{"dataPoints", {{{"timeUnixNano", unix_nanos()},
                                                     {"asInt", std::to_string(value)}}}}}}});
    if (!metrics.empty())
      post_otlp(endpoint, L"/v1/metrics",
                {{"resourceMetrics",
                  {{{"resource", resource},
                    {"scopeMetrics", {{{"scope", {{"name", "repotraverse"}}},
                                       {"metrics", metrics}}}}}}}});
  }
}

} // namespace history
