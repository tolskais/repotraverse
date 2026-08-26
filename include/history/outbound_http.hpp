#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <stop_token>
#include <string>

namespace history {

struct HttpCredential {
  std::string mode{"bearer"};
  std::string environment;
  std::filesystem::path file;
  std::string windows_target;
};

struct OutboundHttpRequest {
  std::string url;
  std::map<std::string, std::string> headers;
  std::size_t maximum_response_bytes{16ULL * 1024ULL * 1024ULL};
  std::chrono::seconds connect_timeout{10};
  std::chrono::seconds overall_timeout{60};
  std::filesystem::path ca_bundle;
  HttpCredential credential;
  std::stop_token stop_token;
};

struct OutboundHttpResponse {
  long status{};
  std::string body;
  std::map<std::string, std::string> headers;
};

class OutboundHttpClient {
public:
  virtual ~OutboundHttpClient() = default;
  virtual OutboundHttpResponse get(const OutboundHttpRequest &request) = 0;
};

std::shared_ptr<OutboundHttpClient> make_curl_http_client();
void validate_curl_runtime(const std::string &configured_auth_mode = {});

} // namespace history
