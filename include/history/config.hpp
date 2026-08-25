#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

namespace history {

struct ServiceConfig {
  std::uint32_t schema_version{1};
  std::string repository_id;
  std::filesystem::path catalog;
  std::filesystem::path analysis_repository;
  std::filesystem::path source_repository;
  std::filesystem::path extractor;
  std::filesystem::path scratch_root;
  std::string remote{"origin"};
  std::string knowledge_ref{"refs/heads/repotraverse/v1/knowledge/accepted"};
  std::string listen_address{"127.0.0.1"};
  std::uint16_t port{7341};
  std::uint32_t sync_seconds{30};
  std::int64_t lease_seconds{900};
  std::int64_t grace_seconds{120};
  std::uint32_t worker_concurrency{2};
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
};

ServiceConfig parse_service_config(const nlohmann::json &value);

} // namespace history
