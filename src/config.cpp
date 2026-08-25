#include "history/config.hpp"
#include "history/encoding.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "history/ir.hpp"

namespace history {
namespace {

template <typename T>
T bounded(const nlohmann::json &value, const char *key, T fallback, T minimum,
          T maximum) {
  const auto result = value.value(key, fallback);
  if (result < minimum || result > maximum)
    throw std::invalid_argument(std::string("service config ") + key +
                                " is outside the supported range");
  return result;
}

bool valid_name(const std::string &value) {
  return !value.empty() && value.size() <= 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '-' || c == '_' || c == '.';
         });
}

} // namespace

ServiceConfig parse_service_config(const nlohmann::json &value) {
  if (!value.is_object())
    throw std::invalid_argument("service configuration must be an object");
  static const std::set<std::string> known = {
      "schema_version",
      "repository_id",
      "catalog",
      "analysis_repository",
      "source_repository",
      "extractor",
      "scratch_root",
      "remote",
      "knowledge_ref",
      "listen_address",
      "port",
      "sync_seconds",
      "lease_seconds",
      "grace_seconds",
      "worker_concurrency",
      "max_task_attempts",
      "git_timeout_seconds",
      "extractor_timeout_seconds",
      "max_manifest_bytes",
      "local_cache_max_facts",
      "local_cache_max_bytes",
      "workspace_mode",
      "workspace_max_revisions",
      "workspace_max_bytes",
      "workspace_free_space_reserve_bytes",
      "trusted_producers",
      "otlp_endpoint",
      "otel_service_name"};
  for (const auto &[key, ignored] : value.items()) {
    (void)ignored;
    if (!known.contains(key))
      throw std::invalid_argument("unknown service config option: " + key);
  }

  ServiceConfig result;
  result.schema_version = value.value("schema_version", 0U);
  if (result.schema_version != kSchemaVersion)
    throw std::invalid_argument(
        "service configuration requires schema_version 1");
  result.repository_id = value.value("repository_id", std::string{});
  if (!valid_name(result.repository_id))
    throw std::invalid_argument(
        "service config requires a valid repository_id");
  result.catalog = path_from_utf8(value.value("catalog", std::string{}));
  result.analysis_repository = path_from_utf8(
      value.value("analysis_repository", std::string{}));
  result.source_repository =
      path_from_utf8(value.value("source_repository", std::string{}));
  result.extractor = path_from_utf8(value.value("extractor", std::string{}));
  result.scratch_root = value.contains("scratch_root")
                            ? path_from_utf8(value.at("scratch_root").get<std::string>())
                            : result.catalog / "scratch";
  if (result.catalog.empty() || result.analysis_repository.empty())
    throw std::invalid_argument(
        "service config requires catalog and analysis_repository");
  result.remote = value.value("remote", std::string{"origin"});
  if (!valid_name(result.remote) || result.remote.starts_with('-'))
    throw std::invalid_argument(
        "service config contains an invalid Git remote");
  result.knowledge_ref = value.value(
      "knowledge_ref", std::string{"refs/heads/repotraverse/v1/knowledge/accepted"});
  if (!result.knowledge_ref.starts_with("refs/heads/repotraverse/v1/knowledge/"))
    throw std::invalid_argument("knowledge_ref must be a repotraverse v1 knowledge ref");
  result.listen_address =
      value.value("listen_address", std::string{"127.0.0.1"});
  if (result.listen_address != "127.0.0.1")
    throw std::invalid_argument("production service must bind to 127.0.0.1");
  result.port = bounded<std::uint16_t>(value, "port", 7341, 1, 65535);
  result.sync_seconds =
      bounded<std::uint32_t>(value, "sync_seconds", 30, 1, 3600);
  result.lease_seconds =
      bounded<std::int64_t>(value, "lease_seconds", 900, 30, 86400);
  result.grace_seconds =
      bounded<std::int64_t>(value, "grace_seconds", 120, 0, 3600);
  result.worker_concurrency =
      bounded<std::uint32_t>(value, "worker_concurrency", 2, 1, 4);
  result.max_task_attempts =
      bounded<std::uint32_t>(value, "max_task_attempts", 10, 1, 100);
  result.git_timeout_seconds =
      bounded<std::uint32_t>(value, "git_timeout_seconds", 300, 1, 3600);
  result.extractor_timeout_seconds =
      bounded<std::uint32_t>(value, "extractor_timeout_seconds", 1800, 1, 7200);
  result.max_manifest_bytes = bounded<std::uint64_t>(
      value, "max_manifest_bytes", 256ULL * 1024ULL * 1024ULL,
      1024ULL * 1024ULL, 1024ULL * 1024ULL * 1024ULL);
  result.local_cache_max_facts = bounded<std::uint32_t>(
      value, "local_cache_max_facts", 10000, 100, 1000000);
  result.local_cache_max_bytes = bounded<std::uint64_t>(
      value, "local_cache_max_bytes", 4ULL * 1024ULL * 1024ULL * 1024ULL,
      64ULL * 1024ULL * 1024ULL, 1024ULL * 1024ULL * 1024ULL * 1024ULL);
  result.workspace_mode = value.value("workspace_mode", std::string{"auto"});
  if (result.workspace_mode != "auto")
    throw std::invalid_argument("workspace_mode currently requires auto");
  result.workspace_max_revisions =
      bounded<std::uint32_t>(value, "workspace_max_revisions", 2, 1, 64);
  result.workspace_max_bytes = value.value("workspace_max_bytes", 0ULL);
  result.workspace_free_space_reserve_bytes = value.value(
      "workspace_free_space_reserve_bytes", 5ULL * 1024ULL * 1024ULL * 1024ULL);
  if (value.contains("trusted_producers")) {
    if (!value.at("trusted_producers").is_array())
      throw std::invalid_argument("trusted_producers must be an array");
    for (const auto &producer : value.at("trusted_producers")) {
      const auto id = producer.get<std::string>();
      if (id.size() != 32 || !std::all_of(id.begin(), id.end(), [](char c) {
            return std::isxdigit(static_cast<unsigned char>(c)) != 0;
          }))
        throw std::invalid_argument("trusted_producers contains an invalid ID");
      result.trusted_producers.insert(id);
    }
  }
  if (!result.extractor.empty() && result.source_repository.empty())
    throw std::invalid_argument(
        "configured extractor requires source_repository");
  result.otlp_endpoint = value.value("otlp_endpoint", std::string{});
  result.otel_service_name =
      value.value("otel_service_name", std::string{"repotraverse"});
  if (!result.otlp_endpoint.empty() &&
      !result.otlp_endpoint.starts_with("https://"))
    throw std::invalid_argument("otlp_endpoint must use https://");
  if (!valid_name(result.otel_service_name))
    throw std::invalid_argument("invalid otel_service_name");
  return result;
}

} // namespace history
