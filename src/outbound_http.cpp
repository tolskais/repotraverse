#include "history/outbound_http.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#endif

namespace history {
namespace {

std::string trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.pop_back();
  const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  value.erase(value.begin(), begin);
  return value;
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

struct Origin {
  std::string value;
  std::string base;
};

Origin origin_of(const std::string &url) {
  if (!url.starts_with("https://"))
    throw std::invalid_argument("outbound connector URL must use HTTPS");
  const auto authority_end = url.find('/', 8);
  const auto authority = url.substr(8, authority_end == std::string::npos
                                           ? std::string::npos
                                           : authority_end - 8);
  if (authority.empty() || authority.find('@') != std::string::npos)
    throw std::invalid_argument("outbound connector URL has an invalid origin");
  const auto normalized = "https://" + lower(authority);
  return {normalized, normalized};
}

std::string resolve_redirect(const std::string &current,
                             const std::string &location) {
  if (location.starts_with("https://")) return location;
  if (!location.starts_with('/'))
    throw std::runtime_error("connector redirect must be absolute or origin-relative");
  return origin_of(current).base + location;
}

std::string credential_value(const HttpCredential &credential) {
  if (credential.mode != "bearer") return {};
  unsigned sources = !credential.environment.empty() + !credential.file.empty() +
                     !credential.windows_target.empty();
  if (sources != 1)
    throw std::runtime_error("bearer authentication requires exactly one credential source");
  if (!credential.environment.empty()) {
    const auto *value = std::getenv(credential.environment.c_str());
    if (!value || !*value)
      throw std::runtime_error("configured bearer-token environment variable is unavailable");
    return value;
  }
  if (!credential.file.empty()) {
#ifndef _WIN32
    struct stat status {};
    if (stat(credential.file.c_str(), &status) != 0 ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) != 0)
      throw std::runtime_error("bearer-token file must not be accessible by group or others");
#endif
    std::ifstream input(credential.file, std::ios::binary);
    std::string value((std::istreambuf_iterator<char>(input)), {});
    if (!input.eof() || value.size() > 64ULL * 1024ULL)
      throw std::runtime_error("cannot read bounded bearer-token file");
    value = trim(std::move(value));
    if (value.empty()) throw std::runtime_error("bearer-token file is empty");
    return value;
  }
#ifdef _WIN32
  const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        credential.windows_target.c_str(), -1,
                                        nullptr, 0);
  if (size <= 0) throw std::runtime_error("invalid Windows credential target");
  std::wstring target(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                      credential.windows_target.c_str(), -1, target.data(), size);
  PCREDENTIALW stored = nullptr;
  if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &stored))
    throw std::runtime_error("configured Windows generic credential is unavailable");
  struct Guard { PCREDENTIALW value; ~Guard() { CredFree(value); } } guard{stored};
  if (!stored->CredentialBlob || stored->CredentialBlobSize == 0)
    throw std::runtime_error("configured Windows generic credential is empty");
  return std::string(reinterpret_cast<const char *>(stored->CredentialBlob),
                     stored->CredentialBlobSize);
#else
  throw std::runtime_error("Windows Credential Manager is unavailable on this platform");
#endif
}

struct Transfer {
  std::string body;
  std::map<std::string, std::string> headers;
  std::size_t maximum{};
  std::stop_token stop;
  bool overflow{};
};

size_t write_body(char *data, size_t size, size_t count, void *opaque) {
  auto &transfer = *static_cast<Transfer *>(opaque);
  const auto bytes = size * count;
  if (bytes > transfer.maximum - std::min(transfer.maximum, transfer.body.size())) {
    transfer.overflow = true;
    return 0;
  }
  transfer.body.append(data, bytes);
  return bytes;
}

size_t write_header(char *data, size_t size, size_t count, void *opaque) {
  auto &transfer = *static_cast<Transfer *>(opaque);
  const auto bytes = size * count;
  std::string_view line(data, bytes);
  const auto colon = line.find(':');
  if (colon != std::string_view::npos) {
    auto name = lower(std::string(line.substr(0, colon)));
    auto value = trim(std::string(line.substr(colon + 1)));
    if (name != "set-cookie" && name != "authorization")
      transfer.headers[name] = std::move(value);
  }
  return bytes;
}

int progress(void *opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<Transfer *>(opaque)->stop.stop_requested() ? 1 : 0;
}

bool retry_status(long status) {
  return status == 429 || status == 502 || status == 503 || status == 504;
}

void interruptible_wait(std::stop_token stop, std::chrono::seconds duration) {
  std::mutex mutex;
  std::condition_variable_any changed;
  std::unique_lock lock(mutex);
  changed.wait_for(lock, stop, duration, [] { return false; });
}

class CurlHttpClient final : public OutboundHttpClient {
public:
  CurlHttpClient() {
    static const int initialized = [] {
      if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        throw std::runtime_error("cannot initialize libcurl");
      return 1;
    }();
    (void)initialized;
    handle_ = curl_easy_init();
    if (!handle_) throw std::runtime_error("cannot allocate libcurl handle");
  }
  ~CurlHttpClient() override { curl_easy_cleanup(handle_); }

  OutboundHttpResponse get(const OutboundHttpRequest &request) override {
    std::scoped_lock lock(mutex_);
    const auto configured_origin = origin_of(request.url).value;
    auto url = request.url;
    auto token = credential_value(request.credential);
    bool refreshed = false;
    unsigned retries = 0, redirects = 0;
    for (;;) {
      if (request.stop_token.stop_requested())
        throw std::runtime_error("outbound HTTP request cancelled");
      Transfer transfer{{}, {}, request.maximum_response_bytes,
                        request.stop_token, false};
      curl_easy_reset(handle_);
      char error[CURL_ERROR_SIZE]{};
      curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
      curl_easy_setopt(handle_, CURLOPT_PROTOCOLS_STR, "https");
      curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT_MS,
                       static_cast<long>(request.connect_timeout.count() * 1000));
      curl_easy_setopt(handle_, CURLOPT_TIMEOUT_MS,
                       static_cast<long>(request.overall_timeout.count() * 1000));
      curl_easy_setopt(handle_, CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt(handle_, CURLOPT_SSL_VERIFYHOST, 2L);
      curl_easy_setopt(handle_, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, write_body);
      curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &transfer);
      curl_easy_setopt(handle_, CURLOPT_HEADERFUNCTION, write_header);
      curl_easy_setopt(handle_, CURLOPT_HEADERDATA, &transfer);
      curl_easy_setopt(handle_, CURLOPT_XFERINFOFUNCTION, progress);
      curl_easy_setopt(handle_, CURLOPT_XFERINFODATA, &transfer);
      curl_easy_setopt(handle_, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(handle_, CURLOPT_ERRORBUFFER, error);
      if (!request.ca_bundle.empty())
        curl_easy_setopt(handle_, CURLOPT_CAINFO, request.ca_bundle.string().c_str());
      if (request.credential.mode == "windows_negotiate") {
        curl_easy_setopt(handle_, CURLOPT_HTTPAUTH, CURLAUTH_NEGOTIATE);
        curl_easy_setopt(handle_, CURLOPT_USERPWD, ":");
      } else if (request.credential.mode == "windows_ntlm") {
        curl_easy_setopt(handle_, CURLOPT_HTTPAUTH, CURLAUTH_NTLM);
        curl_easy_setopt(handle_, CURLOPT_USERPWD, ":");
      } else if (request.credential.mode != "bearer") {
        throw std::runtime_error("unsupported connector authentication mode");
      }
      curl_slist *headers = nullptr;
      struct HeaderGuard { curl_slist *value{}; ~HeaderGuard() { curl_slist_free_all(value); } } guard;
      if (!token.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
      for (const auto &[name, value] : request.headers) {
        const auto normalized = lower(name);
        if (normalized == "authorization" || normalized == "cookie")
          throw std::invalid_argument("caller-supplied secret HTTP headers are forbidden");
        headers = curl_slist_append(headers, (name + ": " + value).c_str());
      }
      guard.value = headers;
      curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, headers);
      const auto code = curl_easy_perform(handle_);
      long status = 0;
      curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &status);
      if (transfer.overflow)
        throw std::runtime_error("connector response exceeds configured size limit");
      if (code == CURLE_ABORTED_BY_CALLBACK)
        throw std::runtime_error("outbound HTTP request cancelled");
      if (code != CURLE_OK) {
        if (retries++ < 3) continue;
        throw std::runtime_error(std::string("outbound HTTPS transport failed: ") +
                                 (error[0] ? error : curl_easy_strerror(code)));
      }
      if (status >= 300 && status < 400) {
        const auto found = transfer.headers.find("location");
        if (found == transfer.headers.end() || redirects++ >= 5)
          throw std::runtime_error("connector redirect is missing or excessive");
        const auto redirected = resolve_redirect(url, found->second);
        if (origin_of(redirected).value != configured_origin)
          throw std::runtime_error("connector redirect crossed the configured origin");
        url = redirected;
        continue;
      }
      if (status == 401 && !refreshed) {
        refreshed = true;
        token = credential_value(request.credential);
        continue;
      }
      if (retry_status(status) && retries++ < 3) {
        std::chrono::seconds delay(1U << std::min(retries, 5U));
        if (const auto found = transfer.headers.find("retry-after"); found != transfer.headers.end()) {
          unsigned parsed{};
          const auto converted = std::from_chars(found->second.data(),
                                                  found->second.data() + found->second.size(), parsed);
          if (converted.ec == std::errc{} && converted.ptr == found->second.data() + found->second.size())
            delay = std::chrono::seconds(std::min(parsed, 60U));
        }
        interruptible_wait(request.stop_token, delay);
        continue;
      }
      return {status, std::move(transfer.body), std::move(transfer.headers)};
    }
  }

private:
  CURL *handle_{};
  std::mutex mutex_;
};

} // namespace

void validate_curl_runtime(const std::string &mode) {
  const auto *info = curl_version_info(CURLVERSION_NOW);
  if (!info || !info->version || std::string_view(info->version) != "8.21.0")
    throw std::runtime_error("Repotraverse requires the pinned libcurl 8.21.0 runtime");
  if ((info->features & CURL_VERSION_SSL) == 0)
    throw std::runtime_error("libcurl runtime does not support HTTPS/TLS");
  bool https = false;
  if (info->protocols)
    for (const char *const *protocol = info->protocols; *protocol; ++protocol)
      https = https || std::string_view(*protocol) == "https";
  if (!https)
    throw std::runtime_error("libcurl runtime does not expose the HTTPS protocol");
  if (mode == "windows_negotiate" &&
      (info->features & CURL_VERSION_SPNEGO) == 0)
    throw std::runtime_error("libcurl runtime does not support SPNEGO/Negotiate");
  if (mode == "windows_ntlm" && (info->features & CURL_VERSION_NTLM) == 0)
    throw std::runtime_error("libcurl runtime does not support NTLM");
#ifdef _WIN32
  if (!info->ssl_version ||
      !std::string_view(info->ssl_version).starts_with("Schannel"))
    throw std::runtime_error("Windows libcurl runtime must use Schannel");
  if ((mode == "windows_negotiate" || mode == "windows_ntlm") &&
      (info->features & CURL_VERSION_SSPI) == 0)
    throw std::runtime_error("libcurl runtime does not support Windows SSPI");
#endif
}

std::shared_ptr<OutboundHttpClient> make_curl_http_client() {
  validate_curl_runtime();
  return std::make_shared<CurlHttpClient>();
}

} // namespace history
