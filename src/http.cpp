#include "history/http.hpp"
#include "history/encoding.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <set>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

#include "history/ir.hpp"
#include "history/telemetry.hpp"

namespace history {
namespace {

struct SocketRuntime {
  SocketRuntime() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
      throw std::runtime_error("cannot initialize Winsock");
#endif
  }
  ~SocketRuntime() {
#ifdef _WIN32
    WSACleanup();
#endif
  }
};

SocketRuntime &socket_runtime() {
  static SocketRuntime runtime;
  return runtime;
}

void close_socket(Socket socket) {
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

void set_socket_timeout(Socket socket, std::uint32_t seconds) {
#ifdef _WIN32
  const DWORD milliseconds = seconds * 1000U;
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char *>(&milliseconds),
             sizeof(milliseconds));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char *>(&milliseconds),
             sizeof(milliseconds));
#else
  timeval timeout{static_cast<long>(seconds), 0};
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

void send_all(Socket socket, std::string_view data) {
  while (!data.empty()) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto count =
        send(socket, data.data(), static_cast<int>(data.size()), flags);
    if (count <= 0)
      throw std::runtime_error("HTTP send failed");
    data.remove_prefix(static_cast<std::size_t>(count));
  }
}

std::string_view trim_ascii(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return value;
}

std::optional<std::size_t> content_length(std::string_view data,
                                          std::size_t header_end) {
  const auto first_line_end = data.find("\r\n");
  if (first_line_end == std::string_view::npos || first_line_end >= header_end)
    throw std::runtime_error("invalid HTTP start line");
  std::optional<std::size_t> result;
  auto begin = first_line_end + 2;
  while (begin < header_end) {
    const auto end = data.find("\r\n", begin);
    if (end == std::string_view::npos || end > header_end)
      throw std::runtime_error("invalid HTTP header line");
    const auto line = data.substr(begin, end - begin);
    const auto separator = line.find(':');
    if (separator == std::string_view::npos)
      throw std::runtime_error("invalid HTTP header line");
    auto name = line.substr(0, separator);
    const auto is_content_length =
        name.size() == std::strlen("content-length") &&
        std::equal(name.begin(), name.end(), "content-length",
                   [](char left, char right) {
                     return std::tolower(static_cast<unsigned char>(left)) ==
                            right;
                   });
    if (is_content_length) {
      if (result)
        throw std::runtime_error("duplicate HTTP Content-Length");
      const auto value = trim_ascii(line.substr(separator + 1));
      std::size_t parsed = 0;
      const auto converted =
          std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (value.empty() || converted.ec != std::errc{} ||
          converted.ptr != value.data() + value.size())
        throw std::runtime_error("invalid HTTP Content-Length");
      result = parsed;
    }
    begin = end + 2;
  }
  return result;
}

std::string receive_all(Socket socket, std::size_t maximum = 256ULL * 1024ULL * 1024ULL,
                        std::size_t maximum_header = 32ULL * 1024ULL) {
  std::string data;
  char buffer[8192];
  std::size_t expected = std::string::npos;
  for (;;) {
    const auto count = recv(socket, buffer, sizeof(buffer), 0);
    if (count <= 0)
      break;
    data.append(buffer, static_cast<std::size_t>(count));
    if (data.size() > maximum + maximum_header)
      throw std::runtime_error("HTTP message exceeds size limit");
    const auto header_end = data.find("\r\n\r\n");
    if (header_end == std::string::npos && data.size() > maximum_header)
      throw std::runtime_error("HTTP headers exceed size limit");
    if (header_end != std::string::npos && expected == std::string::npos) {
      const auto length = content_length(data, header_end).value_or(0);
      if (length > maximum)
        throw std::runtime_error("HTTP body exceeds size limit");
      expected = header_end + 4 + length;
    }
    if (expected != std::string::npos && data.size() >= expected)
      break;
  }
  if (expected != std::string::npos && data.size() < expected)
    throw std::runtime_error("incomplete HTTP body");
  if (expected != std::string::npos && data.size() > expected)
    throw std::runtime_error("unexpected bytes after HTTP body");
  return data;
}

std::pair<std::string, std::uint16_t> parse_endpoint(std::string endpoint) {
  static constexpr std::string_view prefix = "http://";
  if (!endpoint.starts_with(prefix))
    throw std::invalid_argument("endpoint must use http://");
  endpoint.erase(0, prefix.size());
  if (const auto slash = endpoint.find('/'); slash != std::string::npos)
    endpoint.erase(slash);
  const auto colon = endpoint.rfind(':');
  if (colon == std::string::npos)
    return {endpoint, 7341};
  const auto port = std::stoul(endpoint.substr(colon + 1));
  if (port == 0 || port > 65535)
    throw std::invalid_argument("invalid endpoint port");
  return {endpoint.substr(0, colon), static_cast<std::uint16_t>(port)};
}

Socket connect_to(const std::string &host, std::uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  const auto service = std::to_string(port);
  if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0)
    throw std::runtime_error("cannot resolve service endpoint");
  Socket connected = kInvalidSocket;
  for (auto *address = addresses; address; address = address->ai_next) {
    connected =
        socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (connected != kInvalidSocket &&
        connect(connected, address->ai_addr,
                static_cast<int>(address->ai_addrlen)) == 0)
      break;
    if (connected != kInvalidSocket)
      close_socket(connected);
    connected = kInvalidSocket;
  }
  freeaddrinfo(addresses);
  if (connected == kInvalidSocket)
    throw std::runtime_error("cannot connect to local service");
  return connected;
}

nlohmann::json request_http(const std::string &endpoint,
                            std::string_view method, std::string_view path,
                            const nlohmann::json *body) {
  (void)socket_runtime();
  const auto [host, port] = parse_endpoint(endpoint);
  const auto socket = connect_to(host, port);
  struct SocketGuard {
    Socket socket;
    ~SocketGuard() { close_socket(socket); }
  } guard{socket};
  set_socket_timeout(socket, 30);
  const auto payload = body ? body->dump() : std::string{};
  std::string request = std::string(method) + " " + std::string(path) +
                        " HTTP/1.1\r\nHost: " + host +
                        "\r\nConnection: close\r\nContent-Type: "
                        "application/json\r\nContent-Length: " +
                        std::to_string(payload.size()) + "\r\n\r\n" + payload;
  send_all(socket, request);
  const auto response = receive_all(socket);
  const auto split = response.find("\r\n\r\n");
  if (split == std::string::npos)
    throw std::runtime_error("invalid HTTP response");
  const auto line_end = response.find("\r\n");
  if (line_end == std::string::npos ||
      !std::string_view(response).substr(0, line_end).starts_with("HTTP/1.1 "))
    throw std::runtime_error("invalid HTTP response status");
  const auto status_text =
      std::string_view(response).substr(std::strlen("HTTP/1.1 "), 3);
  int status = 0;
  const auto converted = std::from_chars(
      status_text.data(), status_text.data() + status_text.size(), status);
  if (converted.ec != std::errc{} ||
      converted.ptr != status_text.data() + status_text.size() || status < 100 ||
      status > 599)
    throw std::runtime_error("invalid HTTP response status");
  return nlohmann::json::parse(response.substr(split + 4));
}

bool allowed_http_query(const nlohmann::json &request) {
  if (!request.is_object() || !request.contains("query") ||
      !request.at("query").is_string())
    return false;
  static const std::set<std::string> allowed = {
      "file.history", "lineage.review.submit", "lineage.review.get",
      "submodule.revisions"};
  const auto query = request.at("query").get<std::string>();
  return allowed.contains(query) || query.starts_with("tool.");
}

class RequestExecutor {
public:
  RequestExecutor(QueryService &service, std::size_t maximum_queued)
      : service_(service), maximum_queued_(maximum_queued),
        worker_([this](std::stop_token stop) { run(stop); }) {}

  ~RequestExecutor() {
    worker_.request_stop();
    changed_.notify_all();
  }

  nlohmann::json enqueue(const nlohmann::json &request) {
    auto response = service_.enqueue(request);
    if (response.value("state", std::string{}) != "queued")
      return response;
    const auto request_id = response.at("request_id").get<std::string>();
    {
      std::scoped_lock lock(mutex_);
      if (!queued_.contains(request_id) &&
          requests_.size() >= maximum_queued_)
        return service_.fail_request(request_id, "request_queue_full");
      if (queued_.insert(request_id).second)
        requests_.push_back({request_id, request});
    }
    changed_.notify_all();
    return response;
  }

private:
  void run(std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::pair<std::string, nlohmann::json> pending;
      {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, stop, [this] { return !requests_.empty(); });
        if (stop.stop_requested())
          return;
        pending = std::move(requests_.front());
        requests_.pop_front();
      }
      try {
        const auto status = service_.request_status(pending.first, false);
        if (status.value("state", std::string{}) != "cancelled")
          (void)service_.submit(pending.second);
      } catch (const std::exception &error) {
        Telemetry::instance().increment("requests.executor_failures");
        Telemetry::instance().log(
            "error", "request.executor_failed",
             {{"request_id", pending.first},
              {"diagnostic_fingerprint", stable_hash(error.what())}});
        try {
          (void)service_.fail_request(pending.first,
                                      "request_executor_failed");
        } catch (...) {
          Telemetry::instance().increment(
              "requests.executor_failure_persistence_failures");
        }
      }
      {
        std::scoped_lock lock(mutex_);
        queued_.erase(pending.first);
      }
    }
  }

  QueryService &service_;
  std::size_t maximum_queued_;
  std::mutex mutex_;
  std::condition_variable_any changed_;
  std::deque<std::pair<std::string, nlohmann::json>> requests_;
  std::set<std::string> queued_;
  std::jthread worker_;
};

void respond(Socket client, int status, const nlohmann::json &body) {
  const auto payload = body.dump();
  const auto reason = status == 200   ? "OK"
                      : status == 202 ? "Accepted"
                      : status == 404 ? "Not Found"
                      : status == 503 ? "Service Unavailable"
                                      : "Bad Request";
  const auto response =
      "HTTP/1.1 " + std::to_string(status) + " " + reason +
      "\r\nContent-Type: application/json\r\nContent-Length: " +
      std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" +
      payload;
  send_all(client, response);
}

} // namespace

void run_http_server(const HttpServerOptions &options, QueryService &service,
                     const nlohmann::json &identity) {
  (void)socket_runtime();
  RequestExecutor requests(service, options.max_queued_requests);
  const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == kInvalidSocket)
    throw std::runtime_error("cannot create HTTP listener");
  if (options.address != "127.0.0.1") {
    close_socket(listener);
    throw std::runtime_error("HTTP service must bind to loopback");
  }
  int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char *>(&reuse), sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(options.port);
  if (inet_pton(AF_INET, options.address.c_str(), &address.sin_addr) != 1 ||
      bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
          0 ||
      listen(listener, 16) != 0) {
    close_socket(listener);
    throw std::runtime_error("cannot bind local HTTP service");
  }
  if (options.on_listening) {
    sockaddr_in bound{};
#ifdef _WIN32
    int bound_size = sizeof(bound);
#else
    socklen_t bound_size = sizeof(bound);
#endif
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&bound),
                    &bound_size) != 0) {
      close_socket(listener);
      throw std::runtime_error("cannot identify HTTP listener port");
    }
    try {
      options.on_listening(ntohs(bound.sin_port));
    } catch (...) {
      close_socket(listener);
      throw;
    }
  }

  std::uint32_t handled = 0;
  while (!options.stop_token.stop_requested() &&
         (options.max_requests == 0 || handled < options.max_requests)) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    timeval timeout{1, 0};
#ifdef _WIN32
    const auto selected = select(0, &readable, nullptr, nullptr, &timeout);
#else
    const auto selected =
        select(listener + 1, &readable, nullptr, nullptr, &timeout);
#endif
    if (selected <= 0)
      continue;
    const auto client = accept(listener, nullptr, nullptr);
    if (client == kInvalidSocket)
      continue;
    set_socket_timeout(client, options.idle_timeout_seconds);
    try {
      const auto raw = receive_all(client, options.max_request_bytes,
                                   options.max_header_bytes);
      const auto line_end = raw.find("\r\n");
      const auto header_end = raw.find("\r\n\r\n");
      if (line_end == std::string::npos || header_end == std::string::npos)
        throw std::runtime_error("invalid HTTP request");
      const auto first = raw.substr(0, line_end);
      const auto declared_length = content_length(raw, header_end);
      if (first.starts_with("POST ") && !declared_length)
        throw std::runtime_error("POST requires Content-Length");
      if (first.starts_with("GET /v1/health/live ")) {
        respond(client, 200,
                {{"schema_version", kSchemaVersion}, {"ok", true}});
      } else if (first.starts_with("GET /v1/health/ready ")) {
        const auto metrics = Telemetry::instance().snapshot();
        const auto ready =
            metrics.at("gauges").value("coordination.ready", 0) == 1;
        respond(client, ready ? 200 : 503,
                {{"schema_version", kSchemaVersion},
                 {"ok", ready},
                 {"state", ready ? "ready" : "not_ready"}});
      } else if (first.starts_with("GET /v1/status ")) {
        auto status = identity;
        status["metrics"] = Telemetry::instance().snapshot();
        respond(client, 200, status);
      } else if (first.starts_with("GET /v1/metrics ")) {
        respond(client, 200,
                {{"schema_version", kSchemaVersion},
                 {"ok", true},
                 {"metrics", Telemetry::instance().snapshot()}});
      } else if (first.starts_with("POST /v1/requests ")) {
        const auto request =
            nlohmann::json::parse(raw.substr(header_end + 4));
        if (!allowed_http_query(request))
          throw std::runtime_error("query is restricted to the local CLI");
        const auto queued = requests.enqueue(request);
        respond(client, queued.value("ok", false) ? 202 : 503, queued);
      } else if (first.starts_with("POST /v1/queries ")) {
        const auto request =
            nlohmann::json::parse(raw.substr(header_end + 4));
        const auto query = request.value("query", std::string{});
        if (!query.starts_with("tool."))
          throw std::runtime_error("direct HTTP queries are limited to raw tool operations");
        const auto response = service.execute(request);
        respond(client, response.value("ok", false) ? 200 : 400, response);
      } else if (first.starts_with("GET /v1/requests/")) {
        const auto begin = std::strlen("GET /v1/requests/");
        const auto end = first.find(' ', begin);
        auto resource = first.substr(begin, end - begin);
        const auto results_suffix = resource.find("/results");
        const auto request_id = resource.substr(0, results_suffix);
        const auto status = service.request_status(request_id, false);
        if (results_suffix == std::string::npos)
          respond(client, status.value("ok", true) ? 200 : 404, status);
        else
          respond(client, status.value("ok", true) ? 200 : 404,
                  {{"request_id", request_id},
                   {"state", status.value("state", std::string{})},
                   {"result", status.value("result", nlohmann::json{})}});
      } else if (first.starts_with("POST /v1/requests/") &&
                 first.find("/cancel ") != std::string::npos) {
        const auto begin = std::strlen("POST /v1/requests/");
        const auto suffix = first.find("/cancel ", begin);
        const auto request_id = first.substr(begin, suffix - begin);
        const auto cancelled = service.cancel_request(request_id);
        respond(client, cancelled.value("ok", true) ? 200 : 404, cancelled);
      } else {
        respond(client, 404,
                {{"ok", false}, {"error", {{"code", "not_found"}}}});
      }
    } catch (const std::exception &error) {
      Telemetry::instance().increment("http.request_errors");
      Telemetry::instance().log("warning", "http.request_failed",
                                {{"message", utf8_lossy(error.what())}});
      try {
        respond(client, 400,
                {{"ok", false},
                 {"error",
                  {{"code", "invalid_request"},
                   {"message", utf8_lossy(error.what())}}}});
      } catch (...) {
      }
    }
    close_socket(client);
    ++handled;
    Telemetry::instance().increment("http.requests");
  }
  close_socket(listener);
}

nlohmann::json http_query(const std::string &endpoint,
                          const nlohmann::json &request) {
  const auto path = request.value("query", std::string{}).starts_with("tool.")
                        ? "/v1/queries"
                        : "/v1/requests";
  return request_http(endpoint, "POST", path, &request);
}

nlohmann::json http_status(const std::string &endpoint) {
  return request_http(endpoint, "GET", "/v1/status", nullptr);
}

nlohmann::json http_request_status(const std::string &endpoint,
                                   const std::string &request_id) {
  return request_http(endpoint, "GET", "/v1/requests/" + request_id, nullptr);
}

} // namespace history
