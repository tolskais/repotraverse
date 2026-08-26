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
    throw std::invalid_argument(std::string("catalog config ") + key +
                                " is outside the supported range");
  return result;
}

bool valid_name(const std::string &value) {
  return !value.empty() && value.size() <= 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '-' || c == '_' || c == '.';
         });
}

void require_https_origin(const std::string &url) {
  if (!url.starts_with("https://") || url.size() <= 8 ||
      url.find('@', 8) != std::string::npos)
    throw std::invalid_argument("connector base_url must use an HTTPS origin without userinfo");
}

} // namespace

CatalogConfig parse_catalog_config(const nlohmann::json &value) {
  if (!value.is_object())
    throw std::invalid_argument("catalog configuration must be an object");
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
      "analysis_sync_freshness_seconds",
      "lease_seconds",
      "grace_seconds",
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
      "otel_service_name",
      "credentials",
      "connectors"};
  for (const auto &[key, ignored] : value.items()) {
    (void)ignored;
    if (!known.contains(key))
      throw std::invalid_argument("unknown catalog config option: " + key);
  }

  CatalogConfig result;
  result.schema_version = value.value("schema_version", 0U);
  if (result.schema_version != kSchemaVersion)
    throw std::invalid_argument(
        "catalog configuration requires schema_version 1");
  result.repository_id = value.value("repository_id", std::string{});
  if (!valid_name(result.repository_id))
    throw std::invalid_argument(
        "catalog config requires a valid repository_id");
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
        "catalog config requires catalog and analysis_repository");
  result.remote = value.value("remote", std::string{"origin"});
  if (!valid_name(result.remote) || result.remote.starts_with('-'))
    throw std::invalid_argument(
        "catalog config contains an invalid Git remote");
  result.knowledge_ref = value.value(
      "knowledge_ref", std::string{"refs/heads/repotraverse/v1/knowledge/accepted"});
  if (!result.knowledge_ref.starts_with("refs/heads/repotraverse/v1/knowledge/"))
    throw std::invalid_argument("knowledge_ref must be a repotraverse v1 knowledge ref");
  result.analysis_sync_freshness_seconds = bounded<std::uint32_t>(
      value, "analysis_sync_freshness_seconds", 30, 1, 3600);
  result.lease_seconds =
      bounded<std::int64_t>(value, "lease_seconds", 900, 30, 86400);
  result.grace_seconds =
      bounded<std::int64_t>(value, "grace_seconds", 120, 0, 3600);
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
  if (value.contains("credentials")) {
    if (!value.at("credentials").is_array())
      throw std::invalid_argument("credentials must be an array");
    std::set<std::string> names;
    for (const auto &item : value.at("credentials")) {
      if (!item.is_object()) throw std::invalid_argument("credential must be an object");
      CredentialConfig credential;
      credential.name = item.value("name", std::string{});
      credential.mode = item.value("mode", std::string{"bearer"});
      credential.environment = item.value(
          "environment", item.value("token_environment", std::string{}));
      const auto token_file = item.value("file", item.value("token_file", std::string{}));
      if (!token_file.empty()) credential.file = path_from_utf8(token_file);
      credential.windows_target = item.value(
          "windows_target", item.value("credential_target", std::string{}));
      if (!valid_name(credential.name) || !names.insert(credential.name).second)
        throw std::invalid_argument("credential names must be valid and unique");
      if (credential.mode != "bearer" && credential.mode != "windows_negotiate" &&
          credential.mode != "windows_ntlm")
        throw std::invalid_argument("unsupported connector authentication mode");
      if (credential.mode == "bearer") {
        const auto sources = !credential.environment.empty() + !credential.file.empty() +
                             !credential.windows_target.empty();
        if (sources != 1) throw std::invalid_argument(
            "bearer credential requires exactly one environment, file, or windows_target source");
      } else if (!credential.environment.empty() || !credential.file.empty() ||
                 !credential.windows_target.empty()) {
        throw std::invalid_argument("Windows service-identity authentication cannot contain a secret source");
      }
      result.credentials.push_back(std::move(credential));
    }
  }
  if (value.contains("connectors")) {
    if (!value.at("connectors").is_array())
      throw std::invalid_argument("connectors must be an array");
    std::set<std::string> names;
    for (const auto &item : value.at("connectors")) {
      if (!item.is_object()) throw std::invalid_argument("connector must be an object");
      ConnectorConfig connector;
      connector.name = item.value("name", std::string{});
      connector.type = item.value("type", std::string{});
      connector.base_url = item.value("base_url", std::string{});
      while (connector.base_url.ends_with('/')) connector.base_url.pop_back();
      const auto mapping = item.value("mapping", nlohmann::json::object());
      if (!mapping.is_object()) throw std::invalid_argument("connector mapping must be an object");
      connector.repository_id = item.value(
          "repository_id", mapping.value("repository_id", result.repository_id));
      connector.project = item.value("project", mapping.value("project", std::string{}));
      connector.repository = item.value(
          "repository", mapping.value("repository", std::string{}));
      connector.authentication = item.value(
          "authentication_ref", item.value("authentication", std::string{}));
      connector.jira_connector = item.value("jira_connector", std::string{});
      if (item.contains("project_keys"))
        connector.project_keys = item.at("project_keys").get<std::vector<std::string>>();
      else if (mapping.contains("project_keys"))
        connector.project_keys = mapping.at("project_keys").get<std::vector<std::string>>();
      if (item.contains("ca_bundle"))
        connector.ca_bundle = path_from_utf8(item.at("ca_bundle").get<std::string>());
      const auto limits = item.value("limits", nlohmann::json::object());
      if (!limits.is_object()) throw std::invalid_argument("connector limits must be an object");
      connector.page_size = item.contains("page_size")
          ? bounded<std::uint32_t>(item, "page_size", 100, 1, 1000)
          : bounded<std::uint32_t>(limits, "page_size", 100, 1, 1000);
      connector.maximum_response_bytes = bounded<std::uint64_t>(
          item.contains("maximum_response_bytes") ? item : limits,
          "maximum_response_bytes", 16ULL * 1024ULL * 1024ULL,
          1024, 256ULL * 1024ULL * 1024ULL);
      connector.connect_timeout_seconds = bounded<std::uint32_t>(
          item.contains("connect_timeout_seconds") ? item : limits,
          "connect_timeout_seconds", 10, 1, 300);
      connector.request_timeout_seconds = bounded<std::uint32_t>(
          item.contains("request_timeout_seconds") ? item : limits,
          "request_timeout_seconds", 60, 1, 1800);
      if (!valid_name(connector.name) || !names.insert(connector.name).second)
        throw std::invalid_argument("connector names must be valid and unique");
      if (connector.type != "bitbucket_data_center" &&
          connector.type != "jira_data_center")
        throw std::invalid_argument("unsupported connector type");
      require_https_origin(connector.base_url);
      if (connector.authentication.empty() ||
          std::none_of(result.credentials.begin(), result.credentials.end(),
                       [&](const auto &credential) {
                         return credential.name == connector.authentication;
                       }))
        throw std::invalid_argument("connector references an unknown authentication entry");
      if (connector.type == "bitbucket_data_center" &&
          (connector.project.empty() || connector.repository.empty()))
        throw std::invalid_argument("Bitbucket connector requires project and repository mapping");
      result.connectors.push_back(std::move(connector));
    }
    for (const auto &connector : result.connectors)
      if (!connector.jira_connector.empty() &&
          std::none_of(result.connectors.begin(), result.connectors.end(),
                       [&](const auto &candidate) {
                         return candidate.name == connector.jira_connector &&
                                candidate.type == "jira_data_center";
                       }))
        throw std::invalid_argument("Bitbucket connector references an unknown Jira connector");
  }
  return result;
}

} // namespace history
