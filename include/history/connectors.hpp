#pragma once

#include "history/config.hpp"
#include "history/outbound_http.hpp"

#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace history {
class Catalog;
class GitCoordinator;

class ConnectorService {
public:
  ConnectorService(Catalog &catalog, GitCoordinator *coordinator,
                   std::vector<ConnectorConfig> connectors,
                   std::vector<CredentialConfig> credentials,
                   std::shared_ptr<OutboundHttpClient> http);

  nlohmann::json sync(const std::string &name, bool full,
                      const std::vector<std::string> &requested_issue_keys = {},
                      std::stop_token stop = {});
  nlohmann::json status(const std::string &name) const;
  nlohmann::json pull_request(const std::string &name,
                              const std::string &external_id) const;
  nlohmann::json issue(const std::string &name,
                       const std::string &external_id) const;

private:
  const ConnectorConfig &connector(const std::string &name) const;
  HttpCredential credential(const ConnectorConfig &connector) const;
  OutboundHttpResponse get(const ConnectorConfig &connector,
                           const std::string &path,
                           std::stop_token stop) const;
  nlohmann::json sync_jira_keys(const ConnectorConfig &connector,
                                const std::vector<std::string> &keys,
                                std::stop_token stop);

  Catalog &catalog_;
  GitCoordinator *coordinator_{};
  std::vector<ConnectorConfig> connectors_;
  std::vector<CredentialConfig> credentials_;
  std::shared_ptr<OutboundHttpClient> http_;
};

} // namespace history
