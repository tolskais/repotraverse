#include "history/connectors.hpp"

#include "history/catalog.hpp"
#include "history/git_coordination.hpp"
#include "history/ir.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace history {
namespace {

std::string encode(std::string_view value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (const unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      result.push_back(static_cast<char>(c));
    else {
      result.push_back('%');
      result.push_back(hex[c >> 4]);
      result.push_back(hex[c & 15]);
    }
  }
  return result;
}

std::string text(const nlohmann::json &value, const char *key) {
  const auto found = value.find(key);
  return found != value.end() && found->is_string() ? found->get<std::string>()
                                                    : std::string{};
}

std::string nested_text(const nlohmann::json &value,
                        std::initializer_list<const char *> path) {
  const nlohmann::json *current = &value;
  for (const auto *part : path) {
    if (!current->is_object() || !current->contains(part)) return {};
    current = &current->at(part);
  }
  return current->is_string() ? current->get<std::string>() : std::string{};
}

std::int64_t jira_time(const std::string &value) {
  if (value.size() < 19) return 0;
  std::tm parsed{};
  std::istringstream input(value.substr(0, 19));
  input >> std::get_time(&parsed, "%Y-%m-%dT%H:%M:%S");
  if (input.fail()) return 0;
#ifdef _WIN32
  return _mkgmtime(&parsed);
#else
  return timegm(&parsed);
#endif
}

std::vector<std::string> issue_keys(const ConnectorConfig &config,
                                    const nlohmann::json &pull_request,
                                    nlohmann::json &associations) {
  std::map<std::string, std::set<std::string>> discovered;
  const std::vector<std::pair<std::string, std::string>> sources = {
      {"title", text(pull_request, "title")},
      {"description", text(pull_request, "description")},
      {"source_branch", nested_text(pull_request, {"fromRef", "id"})}};
  for (auto project : config.project_keys) {
    std::transform(project.begin(), project.end(), project.begin(), [](unsigned char c) {
      return static_cast<char>(std::toupper(c));
    });
    if (project.empty() || !std::all_of(project.begin(), project.end(), [](unsigned char c) {
          return std::isalnum(c) || c == '_';
        }))
      continue;
    const std::regex pattern("(^|[^A-Z0-9_])(" + project + "-[0-9]+)([^A-Z0-9_]|$)",
                             std::regex::icase);
    for (const auto &[source_name, source] : sources) {
      for (std::sregex_iterator it(source.begin(), source.end(), pattern), end;
           it != end; ++it) {
        auto key = (*it)[2].str();
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
          return static_cast<char>(std::toupper(c));
        });
        discovered[std::move(key)].insert(source_name);
      }
    }
  }
  std::vector<std::string> keys;
  associations = nlohmann::json::array();
  for (const auto &[key, sources_for_key] : discovered) {
    keys.push_back(key);
    associations.push_back({{"key", key},
                            {"sources", std::vector<std::string>(
                                            sources_for_key.begin(),
                                            sources_for_key.end())}});
  }
  return keys;
}

nlohmann::json parse_success(const OutboundHttpResponse &response) {
  if (response.status < 200 || response.status >= 300)
    throw std::runtime_error("connector returned HTTP status " +
                             std::to_string(response.status));
  auto value = nlohmann::json::parse(response.body, nullptr, false);
  if (value.is_discarded()) throw std::runtime_error("connector returned malformed JSON");
  return value;
}

nlohmann::json user(const nlohmann::json &value) {
  if (!value.is_object()) return nullptr;
  nlohmann::json result = nlohmann::json::object();
  for (const auto *key : {"name", "displayName", "emailAddress"})
    if (value.contains(key) && value.at(key).is_string()) result[key] = value.at(key);
  return result;
}

} // namespace

ConnectorService::ConnectorService(Catalog &catalog, GitCoordinator *coordinator,
                                   std::vector<ConnectorConfig> connectors,
                                   std::vector<CredentialConfig> credentials,
                                   std::shared_ptr<OutboundHttpClient> http)
    : catalog_(catalog), coordinator_(coordinator), connectors_(std::move(connectors)),
      credentials_(std::move(credentials)), http_(std::move(http)) {
  if (!http_) throw std::invalid_argument("connector service requires an HTTP client");
  for (const auto &item : connectors_)
    validate_curl_runtime(credential(item).mode);
}

const ConnectorConfig &ConnectorService::connector(const std::string &name) const {
  const auto found = std::find_if(connectors_.begin(), connectors_.end(),
                                  [&](const auto &item) { return item.name == name; });
  if (found == connectors_.end()) throw std::invalid_argument("unknown connector");
  return *found;
}

HttpCredential ConnectorService::credential(const ConnectorConfig &item) const {
  const auto found = std::find_if(credentials_.begin(), credentials_.end(),
      [&](const auto &candidate) { return candidate.name == item.authentication; });
  if (found == credentials_.end()) throw std::invalid_argument("unknown connector credential");
  return {found->mode, found->environment, found->file, found->windows_target};
}

OutboundHttpResponse ConnectorService::get(const ConnectorConfig &item,
                                           const std::string &path,
                                           std::stop_token stop) const {
  OutboundHttpRequest request;
  request.url = item.base_url + path;
  request.headers = {{"Accept", "application/json"}};
  request.maximum_response_bytes = item.maximum_response_bytes;
  request.connect_timeout = std::chrono::seconds(item.connect_timeout_seconds);
  request.overall_timeout = std::chrono::seconds(item.request_timeout_seconds);
  request.ca_bundle = item.ca_bundle;
  request.credential = credential(item);
  request.stop_token = stop;
  return http_->get(request);
}

nlohmann::json ConnectorService::sync(const std::string &name, bool full,
                                      const std::vector<std::string> &requested_issue_keys,
                                      std::stop_token stop) {
  const auto &item = connector(name);
  try {
    if (item.type == "jira_data_center")
      return sync_jira_keys(item, requested_issue_keys, stop);
    const auto initial_cursor = full ? std::int64_t{}
        : catalog_.connector_status(name).value("cursor", std::int64_t{});
    auto newest_cursor = initial_cursor;
    std::size_t start = 0;
    std::size_t pull_requests = 0, commits = 0;
    std::set<std::string> discovered;
    bool reached_cursor = false;
    for (;;) {
      const auto path = "/rest/api/1.0/projects/" + encode(item.project) +
                        "/repos/" + encode(item.repository) +
                        "/pull-requests?state=ALL&order=NEWEST&limit=" +
                        std::to_string(item.page_size) + "&start=" + std::to_string(start);
      const auto page = parse_success(get(item, path, stop));
      if (!page.is_object() || !page.value("values", nlohmann::json::array()).is_array())
        throw std::runtime_error("Bitbucket pull-request page is malformed");
      for (const auto &pr : page.value("values", nlohmann::json::array())) {
        if (!pr.is_object() || !pr.contains("id") || !pr.at("id").is_number_integer())
          throw std::runtime_error("Bitbucket pull request lacks an integer id");
        const auto id = std::to_string(pr.at("id").get<std::int64_t>());
        const auto updated = pr.value("updatedDate", std::int64_t{}) / 1000;
        newest_cursor = std::max(newest_cursor, updated);
        if (!full && initial_cursor > 0 && updated <= initial_cursor) {
          reached_cursor = true;
          continue;
        }
        nlohmann::json associated = nlohmann::json::array();
        std::size_t commit_start = 0;
        for (;;) {
          const auto commit_page = parse_success(get(
              item, "/rest/api/1.0/projects/" + encode(item.project) + "/repos/" +
                        encode(item.repository) + "/pull-requests/" + id +
                        "/commits?limit=" + std::to_string(item.page_size) +
                        "&start=" + std::to_string(commit_start), stop));
          if (!commit_page.is_object() ||
              !commit_page.value("values", nlohmann::json::array()).is_array())
            throw std::runtime_error("Bitbucket commit page is malformed");
          for (const auto &commit : commit_page.value("values", nlohmann::json::array()))
            if (commit.is_object() && commit.contains("id") && commit.at("id").is_string()) {
              associated.push_back(commit.at("id"));
              ++commits;
            }
          if (commit_page.value("isLastPage", true)) break;
          if (!commit_page.contains("nextPageStart") ||
              !commit_page.at("nextPageStart").is_number_unsigned())
            throw std::runtime_error("Bitbucket commit page lacks nextPageStart");
          const auto next = commit_page.at("nextPageStart").get<std::size_t>();
          if (next <= commit_start)
            throw std::runtime_error("Bitbucket commit pagination did not advance");
          commit_start = next;
        }
        nlohmann::json jira_associations;
        const auto keys = issue_keys(item, pr, jira_associations);
        discovered.insert(keys.begin(), keys.end());
        nlohmann::json reviewers = nlohmann::json::array();
        for (const auto &reviewer : pr.value("reviewers", nlohmann::json::array()))
          reviewers.push_back({{"user", user(reviewer.value("user", nlohmann::json::object()))},
                               {"approved", reviewer.value("approved", false)},
                               {"status", text(reviewer, "status")}});
        nlohmann::json normalized = {
            {"provider", "bitbucket_data_center"}, {"connector", item.name},
            {"repository_id", item.repository_id}, {"external_id", id}, {"pr_id", id},
            {"title", text(pr, "title")}, {"description", text(pr, "description")},
            {"state", text(pr, "state")},
            {"source_ref", nested_text(pr, {"fromRef", "id"})},
            {"target_ref", nested_text(pr, {"toRef", "id"})},
            {"source", "bitbucket_data_center:" + item.name},
            {"associated_commits", associated},
            {"author", user(pr.value("author", nlohmann::json::object()).value(
                           "user", nlohmann::json::object()))},
            {"reviewers", reviewers}, {"created_at", pr.value("createdDate", std::int64_t{}) / 1000},
            {"updated_at", updated}, {"issue_keys", keys},
            {"jira_issues", keys},
            {"jira_associations", jira_associations},
            {"provenance", {{"provider", "bitbucket_data_center"},
                              {"connector", item.name}, {"source_updated_at", updated}}}};
        const auto merge = nested_text(pr, {"properties", "mergeCommit", "id"});
        if (!merge.empty()) normalized["result_commit"] = merge;
        const auto self = pr.value("links", nlohmann::json::object()).value(
            "self", nlohmann::json::array());
        if (self.is_array() && !self.empty() && self.front().is_object())
          normalized["url"] = text(self.front(), "href");
        const auto content_id = stable_hash("bitbucket_data_center\n" + item.name + "\n" +
                                            id + "\n" + canonical_json(normalized) + "\n" +
                                            std::to_string(updated) + "\nnormalized-v1");
        catalog_.store_external_fact(item.name, "pull_request", id, content_id,
                                     updated, normalized);
        if (coordinator_) coordinator_->publish_pr_import(normalized);
        ++pull_requests;
      }
      if (reached_cursor) break;
      if (page.value("isLastPage", true)) {
        start = page.value("nextPageStart", start + page.value("size", 0U));
        break;
      }
      if (!page.contains("nextPageStart") || !page.at("nextPageStart").is_number_unsigned())
        throw std::runtime_error("Bitbucket page lacks nextPageStart");
      const auto next = page.at("nextPageStart").get<std::size_t>();
      if (next <= start) throw std::runtime_error("Bitbucket pagination did not advance");
      start = next;
    }
    std::size_t issues = 0;
    if (!item.jira_connector.empty() && !discovered.empty())
      issues = sync_jira_keys(connector(item.jira_connector),
                             {discovered.begin(), discovered.end()}, stop).value("issues", 0U);
    nlohmann::json result = {{"connector", name}, {"state", "synchronized"},
                             {"mode", full ? "full" : "incremental"},
                             {"cursor_before", initial_cursor}, {"cursor", newest_cursor},
                             {"pull_requests", pull_requests}, {"commits", commits},
                             {"issues", issues},
                             {"synchronized_at", std::chrono::system_clock::to_time_t(
                                                      std::chrono::system_clock::now())}};
    catalog_.store_connector_status(name, result);
    return result;
  } catch (const std::exception &error) {
    const nlohmann::json failed = {{"connector", name}, {"state", "failed"},
                                   {"failure_fingerprint", stable_hash(error.what())}};
    catalog_.store_connector_status(name, failed);
    throw;
  }
}

nlohmann::json ConnectorService::sync_jira_keys(
    const ConnectorConfig &item, const std::vector<std::string> &keys,
    std::stop_token stop) {
  std::set<std::string> unique(keys.begin(), keys.end());
  std::size_t count = 0;
  for (const auto &key : unique) {
    if (key.empty()) continue;
    const auto issue = parse_success(get(
        item, "/rest/api/2/issue/" + encode(key) +
                  "?fields=issuetype,summary,description,status,resolution,reporter,assignee,labels,components,created,updated",
        stop));
    if (!issue.is_object() || text(issue, "key").empty() ||
        !issue.value("fields", nlohmann::json::object()).is_object())
      throw std::runtime_error("Jira issue response is malformed");
    const auto &fields = issue.at("fields");
    const auto updated_text = text(fields, "updated");
    const auto updated = jira_time(updated_text);
    nlohmann::json components = nlohmann::json::array();
    for (const auto &component : fields.value("components", nlohmann::json::array()))
      if (component.is_object()) components.push_back(text(component, "name"));
    nlohmann::json normalized = {
        {"provider", "jira_data_center"}, {"connector", item.name},
        {"external_id", key}, {"key", key},
        {"type", nested_text(fields, {"issuetype", "name"})},
        {"summary", text(fields, "summary")},
        {"description", fields.value("description", nlohmann::json(nullptr))},
        {"status", nested_text(fields, {"status", "name"})},
        {"resolution", nested_text(fields, {"resolution", "name"})},
        {"reporter", user(fields.value("reporter", nlohmann::json::object()))},
        {"assignee", user(fields.value("assignee", nlohmann::json::object()))},
        {"labels", fields.value("labels", nlohmann::json::array())},
        {"components", components}, {"created_at", text(fields, "created")},
        {"updated_at", updated_text}, {"url", item.base_url + "/browse/" + encode(key)},
        {"provenance", {{"provider", "jira_data_center"}, {"connector", item.name},
                          {"source_updated_at", updated}}}};
    const auto content_id = stable_hash("jira_data_center\n" + item.name + "\n" + key +
                                        "\n" + canonical_json(normalized) + "\n" +
                                        std::to_string(updated) + "\nnormalized-v1");
    if (coordinator_)
      coordinator_->publish_external_fact(item.name, "issue", key, content_id,
                                          updated, normalized);
    else
      catalog_.store_external_fact(item.name, "issue", key, content_id, updated,
                                   normalized);
    ++count;
  }
  nlohmann::json result = {{"connector", item.name}, {"state", "synchronized"},
                           {"issues", count}, {"requested_keys", unique.size()},
                           {"synchronized_at", std::chrono::system_clock::to_time_t(
                                                    std::chrono::system_clock::now())}};
  catalog_.store_connector_status(item.name, result);
  return result;
}

nlohmann::json ConnectorService::status(const std::string &name) const {
  (void)connector(name);
  return catalog_.connector_status(name);
}

nlohmann::json ConnectorService::pull_request(const std::string &name,
                                              const std::string &id) const {
  (void)connector(name);
  return catalog_.external_fact(name, "pull_request", id);
}

nlohmann::json ConnectorService::issue(const std::string &name,
                                       const std::string &id) const {
  (void)connector(name);
  return catalog_.external_fact(name, "issue", id);
}

} // namespace history
