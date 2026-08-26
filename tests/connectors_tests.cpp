#include "catch_amalgamated.hpp"

#include "history/catalog.hpp"
#include "history/connectors.hpp"

#include <chrono>
#include <deque>

namespace {
class FakeHttp final : public history::OutboundHttpClient {
public:
  history::OutboundHttpResponse get(const history::OutboundHttpRequest &request) override {
    urls.push_back(request.url);
    REQUIRE_FALSE(request.headers.contains("Authorization"));
    REQUIRE_FALSE(responses.empty());
    auto result = std::move(responses.front());
    responses.pop_front();
    return result;
  }
  std::deque<history::OutboundHttpResponse> responses;
  std::vector<std::string> urls;
};

history::OutboundHttpResponse response(nlohmann::json body) {
  return {200, body.dump(), {}};
}
} // namespace

TEST_CASE("Bitbucket and discovered Jira facts are normalized without raw payloads") {
  const auto root = std::filesystem::temp_directory_path() /
      ("repotraverse-connectors-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup { std::filesystem::path path; ~Cleanup() {
    std::error_code ignored; std::filesystem::remove_all(path, ignored);
  }} cleanup{root};
  history::Catalog catalog(root);
  auto http = std::make_shared<FakeHttp>();
  const nlohmann::json pull_request = {
      {"id", 7}, {"title", "Implement APP-42"},
      {"description", "No secrets"}, {"state", "MERGED"},
      {"createdDate", 1000}, {"updatedDate", 5000},
      {"fromRef", {{"id", "refs/heads/feature/APP-42"}}},
      {"toRef", {{"id", "refs/heads/main"}}},
      {"author", {{"user", {{"name", "alice"}}}}},
      {"properties", {{"mergeCommit", {{"id", "merge-oid"}}}}},
      {"comments", nlohmann::json::array({{{"text", "must never persist"}}})},
      {"attachments", nlohmann::json::array({{{"name", "secret.txt"}}})}};
  http->responses.push_back(response(
      {{"isLastPage", true}, {"size", 1},
       {"values", nlohmann::json::array({pull_request})}}));
  http->responses.push_back(response(
      {{"isLastPage", true}, {"values", {{{"id", "commit-oid"}}}}}));
  http->responses.push_back(response({
      {"key", "APP-42"},
      {"fields", {{"issuetype", {{"name", "Story"}}},
                   {"summary", "Enterprise connector"},
                   {"description", "normalized"},
                   {"status", {{"name", "Done"}}},
                   {"resolution", {{"name", "Fixed"}}},
                   {"reporter", {{"name", "alice"}}},
                   {"assignee", {{"name", "bob"}}},
                   {"labels", {"connector"}},
                   {"components", {{{"name", "Platform"}}}},
                   {"created", "2026-01-01T00:00:00.000+0000"},
                   {"updated", "2026-01-02T00:00:00.000+0000"}}}}));

  history::CredentialConfig credential;
  credential.name = "token";
  credential.mode = "bearer";
  credential.environment = "TEST_CONNECTOR_TOKEN";
  history::ConnectorConfig jira;
  jira.name = "jira"; jira.type = "jira_data_center";
  jira.base_url = "https://jira.invalid"; jira.authentication = "token";
  history::ConnectorConfig bitbucket;
  bitbucket.name = "bitbucket"; bitbucket.type = "bitbucket_data_center";
  bitbucket.base_url = "https://bitbucket.invalid";
  bitbucket.repository_id = "main"; bitbucket.project = "APP";
  bitbucket.repository = "product"; bitbucket.project_keys = {"APP"};
  bitbucket.authentication = "token"; bitbucket.jira_connector = "jira";

  history::ConnectorService service(catalog, nullptr, {jira, bitbucket},
                                    {credential}, http);
  nlohmann::json synchronized;
  try {
    synchronized = service.sync("bitbucket", true);
  } catch (...) {
    INFO("requests completed: " << http->urls.size());
    if (!http->urls.empty()) INFO("last request: " << http->urls.back());
    throw;
  }
  REQUIRE(synchronized.at("pull_requests") == 1);
  REQUIRE(synchronized.at("issues") == 1);
  const auto pr = service.pull_request("bitbucket", "7").at("effective");
  REQUIRE(pr.at("associated_commits") == nlohmann::json::array({"commit-oid"}));
  REQUIRE(pr.at("issue_keys") == nlohmann::json::array({"APP-42"}));
  REQUIRE(pr.at("result_commit") == "merge-oid");
  REQUIRE_FALSE(pr.contains("comments"));
  REQUIRE_FALSE(pr.contains("attachments"));
  const auto issue = service.issue("jira", "APP-42").at("effective");
  REQUIRE(issue.at("summary") == "Enterprise connector");
  REQUIRE_FALSE(issue.contains("worklogs"));
}
