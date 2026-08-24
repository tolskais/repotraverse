#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <stop_token>

#include <nlohmann/json.hpp>

#include "history/query.hpp"

namespace history {

struct HttpServerOptions {
  std::string address{"127.0.0.1"};
  std::uint16_t port{7341};
  std::uint32_t max_requests{};
  std::uint32_t idle_timeout_seconds{10};
  std::size_t max_request_bytes{1024ULL * 1024ULL};
  std::size_t max_header_bytes{32ULL * 1024ULL};
  std::size_t max_queued_requests{128};
  std::stop_token stop_token;
  std::function<void(std::uint16_t)> on_listening;
};

void run_http_server(const HttpServerOptions &options, QueryService &service,
                     const nlohmann::json &identity);
nlohmann::json http_query(const std::string &endpoint,
                          const nlohmann::json &request);
nlohmann::json http_status(const std::string &endpoint);
nlohmann::json http_request_status(const std::string &endpoint,
                                   const std::string &request_id);

} // namespace history
