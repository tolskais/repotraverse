#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace history {

struct ConnectorConfig {
  std::string name;
  std::string type;
  std::string base_url;
  std::string repository_id;
  std::string project;
  std::string repository;
  std::vector<std::string> project_keys;
  std::string authentication;
  std::string jira_connector;
  std::filesystem::path ca_bundle;
  std::uint32_t page_size{100};
  std::uint64_t maximum_response_bytes{16ULL * 1024ULL * 1024ULL};
  std::uint32_t connect_timeout_seconds{10};
  std::uint32_t request_timeout_seconds{60};
};

struct CredentialConfig {
  std::string name;
  std::string mode{"bearer"};
  std::string environment;
  std::filesystem::path file;
  std::string windows_target;
};

struct CatalogConfig {
  std::uint32_t schema_version{1};
  std::string repository_id;
  std::filesystem::path catalog;
  std::filesystem::path analysis_repository;
  std::filesystem::path source_repository;
  std::filesystem::path extractor;
  std::filesystem::path scratch_root;
  std::string remote{"origin"};
  std::string knowledge_ref{"refs/heads/repotraverse/v1/knowledge/accepted"};
  std::uint32_t analysis_sync_freshness_seconds{30};
  std::int64_t lease_seconds{900};
  std::int64_t grace_seconds{120};
  std::uint32_t max_task_attempts{10};
  std::uint32_t git_timeout_seconds{300};
  std::uint32_t extractor_timeout_seconds{1800};
  std::uint64_t max_manifest_bytes{256ULL * 1024ULL * 1024ULL};
  std::uint32_t local_cache_max_facts{10000};
  std::uint64_t local_cache_max_bytes{4ULL * 1024ULL * 1024ULL * 1024ULL};
  std::string workspace_mode{"auto"};
  std::uint32_t workspace_max_revisions{2};
  std::uint64_t workspace_max_bytes{};
  std::uint64_t workspace_free_space_reserve_bytes{5ULL * 1024ULL * 1024ULL *
                                                   1024ULL};
  std::set<std::string> trusted_producers;
  std::string otlp_endpoint;
  std::string otel_service_name{"repotraverse"};
  std::vector<CredentialConfig> credentials;
  std::vector<ConnectorConfig> connectors;
};

CatalogConfig parse_catalog_config(const nlohmann::json &value);

} // namespace history
