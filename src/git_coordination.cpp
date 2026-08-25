#include "history/git_coordination.hpp"
#include "history/encoding.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string_view>

#include "history/ir.hpp"
#include "history/process.hpp"

namespace history {
namespace {

std::string trim(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                            value.back() == ' ' || value.back() == '\t'))
    value.pop_back();
  const auto begin = value.find_first_not_of(" \t\r\n");
  return begin == std::string::npos ? std::string{} : value.substr(begin);
}

std::int64_t now_seconds() {
  return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

void require_git(const ProcessOutput &result, std::string_view action) {
  if (result.exit_code != 0)
    throw std::runtime_error(std::string(action) +
                             " failed: " + utf8_lossy(trim(result.error)));
}

std::vector<std::string> lines(const std::string &value) {
  std::vector<std::string> result;
  std::size_t begin = 0;
  while (begin < value.size()) {
    const auto end = value.find('\n', begin);
    result.push_back(
        value.substr(begin, end == std::string::npos ? end : end - begin));
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return result;
}

bool schema_v1(const nlohmann::json &value) {
  return value.is_object() && value.contains("schema_version") &&
         value.at("schema_version").is_number_unsigned() &&
         value.at("schema_version").get<std::uint64_t>() == kSchemaVersion;
}

std::string string_field(const nlohmann::json &value, const char *key) {
  const auto found = value.find(key);
  return found != value.end() && found->is_string()
             ? found->get<std::string>()
             : std::string{};
}

std::string relation_identity(const LineageRelation &relation) {
  return stable_hash(
      relation.repository_id + "\n" + relation.kind + "\n" +
      nlohmann::json(relation.source_element_ids).dump() + "\n" +
      nlohmann::json(relation.target_element_ids).dump());
}

} // namespace

GitCoordinator::GitCoordinator(Catalog &catalog, CoordinationOptions options)
    : catalog_(catalog), options_(std::move(options)) {
  if (options_.repository.empty())
    throw std::invalid_argument("analysis repository cannot be empty");
  if (options_.remote.empty() || options_.remote.starts_with('-'))
    throw std::invalid_argument("invalid Git remote");
}

bool GitCoordinator::trusted_producer(const std::string &producer_id) const {
  return producer_id == catalog_.producer_id() ||
         (!options_.enforce_trusted_producers &&
          options_.trusted_producers.empty()) ||
         options_.trusted_producers.contains(producer_id);
}

std::chrono::seconds GitCoordinator::heartbeat_interval() const {
  const auto seconds = std::clamp<std::int64_t>(options_.lease_seconds / 3, 1, 60);
  return std::chrono::seconds(seconds);
}

std::string GitCoordinator::claim_ref(const std::string &task_id) const {
  if (task_id.size() != 32 ||
      !std::all_of(task_id.begin(), task_id.end(),
                   [](unsigned char c) { return std::isxdigit(c) != 0; }))
    throw std::invalid_argument(
        "task_id must be a 32-character hexadecimal identifier");
  return "refs/heads/repotraverse/v1/coordination/claims/" + task_id.substr(0, 2) + "/" +
         task_id;
}

std::string
GitCoordinator::remote_tracking_ref(const std::string &task_id) const {
  return "refs/remotes/" + options_.remote + "/repotraverse/v1/coordination/claims/" +
         task_id.substr(0, 2) + "/" + task_id;
}

nlohmann::json GitCoordinator::sync() {
  std::scoped_lock lock(mutex_);
  const auto fetch = run_process({"git", "-C", path_to_utf8(options_.repository),
                                  "fetch", "--prune", options_.remote,
                                  "+refs/heads/repotraverse/*:refs/remotes/" +
                                      options_.remote + "/repotraverse/*"});
  if (fetch.exit_code != 0)
    return {{"state", "waiting_for_coordination"},
            {"message", utf8_lossy(trim(fetch.error))}};

  const auto list =
      run_process({"git", "-C", path_to_utf8(options_.repository), "for-each-ref",
                   "--format=%(objectname)%09%(refname)",
                   "refs/remotes/" + options_.remote + "/repotraverse/v1/coordination/claims"});
  require_git(list, "list claim refs");
  std::size_t imported = 0;
  std::vector<std::string> observed;
  for (const auto &line : lines(list.output)) {
    if (line.empty())
      continue;
    const auto separator = line.find('\t');
    if (separator == std::string::npos)
      continue;
    const auto oid = line.substr(0, separator);
    const auto ref = line.substr(separator + 1);
    const auto slash = ref.rfind('/');
    if (slash == std::string::npos)
      continue;
    const auto task_id = ref.substr(slash + 1);
    const auto show = run_process({"git", "-C", path_to_utf8(options_.repository),
                                   "show", "-s", "--format=%B", oid});
    require_git(show, "read claim");
    if (show.output.size() > options_.max_record_bytes)
      continue;
    auto lease = nlohmann::json::parse(show.output, nullptr, false);
    if (lease.is_discarded() || !lease.is_object() ||
        !lease.contains("schema_version") ||
        !lease.at("schema_version").is_number_unsigned() ||
        !lease.contains("task_id") || !lease.at("task_id").is_string() ||
        !lease.contains("producer_id") ||
        !lease.at("producer_id").is_string() ||
        !schema_v1(lease) || string_field(lease, "task_id") != task_id ||
        !trusted_producer(string_field(lease, "producer_id")))
      continue;
    catalog_.observe_claim(task_id, oid, lease);
    observed.push_back(task_id);
    ++imported;
  }
  for (const auto &known : catalog_.claims()) {
    const auto task_id = known.value("task_id", std::string{});
    if (std::find(observed.begin(), observed.end(), task_id) == observed.end())
      catalog_.remove_claim(task_id);
  }
  const auto tasks = import_tasks();
  const auto facts = import_results();
  const auto reviews = import_reviews();
  const auto knowledge = import_knowledge();
  std::size_t task_batches_pruned = 0;
  const auto task_prefix = "refs/remotes/" + options_.remote +
                           "/repotraverse/v1/coordination/tasks";
  const auto task_refs = run_process(
      {"git", "-C", path_to_utf8(options_.repository), "for-each-ref",
       "--format=%(refname)", task_prefix});
  require_git(task_refs, "list temporary task refs");
  for (const auto &ref : lines(task_refs.output)) {
    if (ref.empty()) continue;
    const auto commit = run_process({"git", "-C", path_to_utf8(options_.repository),
                                     "rev-parse", "--verify", ref});
    if (commit.exit_code != 0) continue;
    const auto names = run_process({"git", "-C", path_to_utf8(options_.repository),
                                    "ls-tree", "--name-only", trim(commit.output)});
    if (names.exit_code != 0) continue;
    bool any = false, all_complete = true;
    for (const auto &name : lines(names.output)) {
      if (!name.ends_with(".jsonl")) continue;
      const auto show = run_process({"git", "-C", path_to_utf8(options_.repository),
                                     "show", trim(commit.output) + ":" + name});
      if (show.exit_code != 0) {
        all_complete = false;
        continue;
      }
      for (const auto &line : lines(show.output)) {
        if (line.empty()) continue;
        const auto record = nlohmann::json::parse(line, nullptr, false);
        const auto task_id = string_field(record, "task_id");
        if (task_id.empty()) continue;
        any = true;
        all_complete = all_complete && catalog_.fact_for_task(task_id).has_value();
      }
    }
    if (!any || !all_complete) continue;
    const auto remote_prefix = "refs/remotes/" + options_.remote + "/";
    if (!ref.starts_with(remote_prefix)) continue;
    const auto head_ref = "refs/heads/" + ref.substr(remote_prefix.size());
    const auto removed = run_process({"git", "-C", path_to_utf8(options_.repository),
                                      "push", options_.remote, ":" + head_ref});
    if (removed.exit_code == 0) ++task_batches_pruned;
  }
  std::size_t pruned = 0;
  const auto now = now_seconds();
  for (const auto &claim : catalog_.claims()) {
    if (claim.value("state", std::string{}) != "completed" ||
        claim.value("expires_at", std::int64_t{}) > now)
      continue;
    const auto task_id = claim.at("task_id").get<std::string>();
    const auto oid = claim.at("ref_oid").get<std::string>();
    const auto removed =
        run_process({"git", "-C", path_to_utf8(options_.repository), "push",
                     "--force-with-lease=" + claim_ref(task_id) + ":" + oid,
                     options_.remote, ":" + claim_ref(task_id)});
    if (removed.exit_code == 0) {
      const auto result_ref = "refs/heads/repotraverse/v1/coordination/results/" +
                              task_id.substr(0, 2) + "/" + task_id;
      (void)run_process({"git", "-C", path_to_utf8(options_.repository),
                         "push", options_.remote, ":" + result_ref});
      catalog_.remove_claim(task_id);
      ++pruned;
    }
  }
  return {{"state", "synchronized"}, {"claims", imported},
          {"claims_pruned", pruned}, {"tasks_imported", tasks},
          {"task_batches_pruned", task_batches_pruned},
          {"facts_imported", facts}, {"reviews_imported", reviews},
          {"knowledge_imported", knowledge},
          {"snapshot_id", catalog_.snapshot_id()}};
}

nlohmann::json GitCoordinator::publish_tasks(const nlohmann::json &tasks) {
  std::scoped_lock lock(mutex_);
  if (!tasks.is_array())
    throw std::invalid_argument("tasks must be an array");
  const auto synchronized = sync();
  if (synchronized.value("state", std::string{}) != "synchronized")
    return synchronized;
  std::string payload;
  std::vector<std::string> published;
  for (const auto &task : tasks) {
    const auto task_id = task.value("task_id", std::string{});
    if (task_id.empty() || catalog_.task_published(task_id))
      continue;
    auto descriptor = task;
    descriptor.erase("state");
    descriptor.erase("task_id");
    payload += canonical_json({{"schema_version", kSchemaVersion},
                               {"record_type", "extraction_task"},
                               {"producer_id", catalog_.producer_id()},
                               {"task_id", task_id},
                               {"content_hash", stable_hash(descriptor.dump())},
                               {"task", descriptor}});
    published.push_back(task_id);
  }
  if (published.empty())
    return {{"state", "published"}, {"task_count", 0}};
  const auto batch_id = stable_hash(payload);
  const auto blob = run_process({"git", "-C", path_to_utf8(options_.repository),
                                 "hash-object", "-w", "--stdin"},
                                {}, payload);
  require_git(blob, "store task batch");
  const auto entry =
      "100644 blob " + trim(blob.output) + "\t" + batch_id + ".jsonl\n";
  const auto tree = run_process(
      {"git", "-C", path_to_utf8(options_.repository), "mktree"}, {}, entry);
  require_git(tree, "create task batch tree");
  const auto producer_ref =
      "refs/heads/repotraverse/v1/coordination/tasks/" + catalog_.producer_id() +
      "/" + batch_id;
  const auto tracking = "refs/remotes/" + options_.remote +
                        "/repotraverse/v1/coordination/tasks/" + catalog_.producer_id() +
                        "/" + batch_id;
  std::vector<std::string> command = {"git",
                                      "-c",
                                      "user.name=Repotraverse",
                                      "-c",
                                      "user.email=repotraverse@invalid",
                                      "-C",
                                      path_to_utf8(options_.repository),
                                      "commit-tree",
                                      trim(tree.output)};
  command.push_back("-m");
  command.push_back(nlohmann::json({{"record_type", "task_batch"},
                                    {"batch_id", batch_id},
                                    {"task_count", published.size()}})
                        .dump());
  const auto commit = run_process(command);
  require_git(commit, "create task batch commit");
  const auto push =
      run_process({"git", "-C", path_to_utf8(options_.repository), "push",
                   options_.remote, trim(commit.output) + ":" + producer_ref});
  if (push.exit_code != 0)
    return {{"state", "waiting_for_coordination"},
            {"message", utf8_lossy(trim(push.error))}};
  require_git(run_process({"git", "-C", path_to_utf8(options_.repository),
                           "update-ref", tracking, trim(commit.output)}),
              "update task producer ref");
  for (const auto &task_id : published)
    catalog_.mark_task_published(task_id);
  return {{"state", "published"},
          {"task_count", published.size()},
          {"batch_id", batch_id}};
}

std::size_t GitCoordinator::import_tasks() {
  const auto refs =
      run_process({"git", "-C", path_to_utf8(options_.repository), "for-each-ref",
                   "--format=%(refname)",
                   "refs/remotes/" + options_.remote + "/repotraverse/v1/coordination/tasks"});
  require_git(refs, "list task producer refs");
  std::size_t count = 0;
  for (const auto &ref : lines(refs.output)) {
    if (ref.empty())
      continue;
    const auto commits = run_process({"git", "-C", path_to_utf8(options_.repository),
                                      "rev-list", "--reverse", ref});
    require_git(commits, "list task commits");
    for (const auto &commit : lines(commits.output)) {
      if (commit.empty() || catalog_.imported(commit))
        continue;
      const auto names = run_process({"git", "-C", path_to_utf8(options_.repository),
                                      "ls-tree", "--name-only", commit});
      require_git(names, "list task batch");
      for (const auto &name : lines(names.output)) {
        if (name.empty() || !name.ends_with(".jsonl"))
          continue;
        const auto show =
            run_process({"git", "-C", path_to_utf8(options_.repository), "show",
                         commit + ":" + name});
        require_git(show, "read task batch");
        if (show.output.size() > options_.max_record_bytes)
          continue;
        for (const auto &line : lines(show.output)) {
          if (line.empty())
            continue;
          const auto record = nlohmann::json::parse(line, nullptr, false);
          if (record.is_discarded() || !schema_v1(record) ||
              string_field(record, "record_type") != "extraction_task" ||
              !trusted_producer(string_field(record, "producer_id")))
            continue;
          if (!record.contains("task_id") || !record.at("task_id").is_string() ||
              !record.contains("task"))
            continue;
          const auto task_id = record.at("task_id").get<std::string>();
          const auto &task = record.at("task");
          if (!task.is_object() || !task.contains("identity") ||
              string_field(record, "content_hash") !=
                  stable_hash(task.dump()) ||
              stable_hash(task.at("identity").dump()) != task_id)
            continue;
          catalog_.schedule_task(task_id, task);
          catalog_.mark_task_published(task_id);
          ++count;
        }
      }
      catalog_.mark_imported(commit);
    }
  }
  return count;
}

std::size_t GitCoordinator::import_results() {
  const auto refs = run_process(
      {"git", "-C", path_to_utf8(options_.repository), "for-each-ref",
       "--format=%(refname)",
       "refs/remotes/" + options_.remote + "/repotraverse/v1/coordination/results"});
  require_git(refs, "list producer refs");
  std::size_t count = 0;
  for (const auto &ref : lines(refs.output)) {
    if (ref.empty())
      continue;
    const auto head = run_process({"git", "-C", path_to_utf8(options_.repository),
                                   "rev-parse", "--verify", ref});
    require_git(head, "resolve temporary result ref");
    for (const auto &commit : lines(head.output)) {
      if (commit.empty() || catalog_.imported(commit))
        continue;
      const auto names = run_process({"git", "-C", path_to_utf8(options_.repository),
                                      "ls-tree", "--name-only", commit});
      require_git(names, "list result shard");
      for (const auto &name : lines(names.output)) {
        if (name.empty() || !name.ends_with(".jsonl"))
          continue;
        const auto show =
            run_process({"git", "-C", path_to_utf8(options_.repository), "show",
                         commit + ":" + name});
        require_git(show, "read result shard");
        if (show.output.size() > options_.max_record_bytes)
          continue;
        for (const auto &line : lines(show.output)) {
          if (line.empty())
            continue;
          const auto fact = nlohmann::json::parse(line, nullptr, false);
          if (fact.is_discarded() || !schema_v1(fact) ||
              string_field(fact, "record_type") != "extraction_result" ||
              !trusted_producer(string_field(fact, "producer_id")))
            continue;
          const auto result = fact.value("result", nlohmann::json{});
          if (!result.is_object() ||
              (string_field(result, "record_type") != "tu_manifest" &&
               string_field(result, "record_type") != "tu_failure") ||
              stable_hash(result.dump()) !=
                  fact.value("fact_id", std::string{}))
            continue;
          if (string_field(result, "record_type") == "tu_manifest") {
            std::string validation_error;
            try {
              const auto manifest = result.get<TuManifest>();
              if (!validate_tu_manifest(manifest, validation_error))
                continue;
            } catch (const std::exception &) {
              continue;
            }
          } else if (!schema_v1(result) ||
                     string_field(result, "source_revision").empty() ||
                     string_field(result, "translation_unit").empty() ||
                     string_field(result, "context_id").empty()) {
            continue;
          }
          if (!fact.contains("fact_id") || !fact.at("fact_id").is_string() ||
              !fact.contains("task_id") || !fact.at("task_id").is_string())
            continue;
          catalog_.store_fact(fact.at("fact_id").get<std::string>(),
                              fact.at("task_id").get<std::string>(), fact,
                              commit);
          ++count;
        }
      }
      catalog_.mark_imported(commit);
    }
  }
  return count;
}

std::size_t GitCoordinator::import_reviews() {
  const auto refs = run_process(
      {"git", "-C", path_to_utf8(options_.repository), "for-each-ref",
       "--format=%(refname)",
       "refs/remotes/" + options_.remote + "/repotraverse/v1/knowledge/lineage"});
  require_git(refs, "list review producer refs");
  std::size_t count = 0;
  for (const auto &ref : lines(refs.output)) {
    if (ref.empty()) continue;
    const auto commits = run_process({"git", "-C", path_to_utf8(options_.repository),
                                      "rev-list", "--reverse", ref});
    require_git(commits, "list review commits");
    for (const auto &commit : lines(commits.output)) {
      if (commit.empty() || catalog_.imported(commit)) continue;
      const auto names = run_process({"git", "-C", path_to_utf8(options_.repository),
                                      "ls-tree", "--name-only", commit});
      require_git(names, "list review facts");
      for (const auto &name : lines(names.output)) {
        if (!name.ends_with(".json")) continue;
        const auto show = run_process({"git", "-C", path_to_utf8(options_.repository),
                                       "show", commit + ":" + name});
        require_git(show, "read review fact");
        if (show.output.size() > options_.max_record_bytes) continue;
        const auto record = nlohmann::json::parse(show.output, nullptr, false);
        if (record.is_discarded() || !schema_v1(record) ||
            string_field(record, "record_type") != "lineage_relation" ||
            !trusted_producer(string_field(record, "producer_id")) ||
            !record.contains("relation"))
          continue;
        try {
          auto relation = record.at("relation").get<LineageRelation>();
          if (relation.relation_id != relation_identity(relation) ||
              string_field(record, "content_hash") !=
                  stable_hash(record.at("relation").dump()) ||
              (relation.review_state != "accepted" &&
               relation.review_state != "rejected") ||
              (relation.review_state == "accepted" &&
               relation.reviewer.empty()))
            continue;
          catalog_.store_lineage_relation(relation);
          ++count;
        } catch (const std::exception &) {
          continue;
        }
      }
      catalog_.mark_imported(commit);
    }
  }
  return count;
}

nlohmann::json GitCoordinator::publish_review(
    const LineageRelation &relation) {
  std::scoped_lock lock(mutex_);
  if (relation.relation_id != relation_identity(relation))
    throw std::invalid_argument("lineage relation identity mismatch");
  const auto synchronized = sync();
  if (synchronized.value("state", std::string{}) != "synchronized")
    return synchronized;
  const nlohmann::json record = {
      {"schema_version", kSchemaVersion},
      {"record_type", "lineage_relation"},
      {"producer_id", catalog_.producer_id()},
      {"content_hash", stable_hash(nlohmann::json(relation).dump())},
      {"relation", relation}};
  const auto payload = canonical_json(record);
  const auto blob = run_process({"git", "-C", path_to_utf8(options_.repository),
                                 "hash-object", "-w", "--stdin"},
                                {}, payload);
  require_git(blob, "store lineage review");
  const auto entry = "100644 blob " + trim(blob.output) + "\t" +
                     relation.relation_id + ".json\n";
  const auto tree = run_process(
      {"git", "-C", path_to_utf8(options_.repository), "mktree"}, {}, entry);
  require_git(tree, "create lineage review tree");
  const auto producer_ref =
      "refs/heads/repotraverse/v1/knowledge/lineage/" + catalog_.producer_id();
  const auto tracking = "refs/remotes/" + options_.remote +
                        "/repotraverse/v1/knowledge/lineage/" + catalog_.producer_id();
  const auto prior = run_process({"git", "-C", path_to_utf8(options_.repository),
                                  "rev-parse", "--verify", tracking});
  std::vector<std::string> command = {
      "git", "-c", "user.name=Repotraverse", "-c",
      "user.email=repotraverse@invalid", "-C", path_to_utf8(options_.repository),
      "commit-tree", trim(tree.output)};
  if (prior.exit_code == 0) {
    command.push_back("-p");
    command.push_back(trim(prior.output));
  }
  command.insert(command.end(),
                 {"-m", nlohmann::json({{"record_type", "lineage_relation"},
                                        {"relation_id", relation.relation_id},
                                        {"review_state", relation.review_state}})
                            .dump()});
  const auto commit = run_process(command);
  require_git(commit, "create lineage review commit");
  const auto push = run_process({"git", "-C", path_to_utf8(options_.repository),
                                 "push", options_.remote,
                                 trim(commit.output) + ":" + producer_ref});
  require_git(push, "publish lineage review");
  require_git(run_process({"git", "-C", path_to_utf8(options_.repository),
                           "update-ref", tracking, trim(commit.output)}),
              "update lineage review tracking ref");
  catalog_.store_lineage_relation(relation);
  catalog_.mark_imported(trim(commit.output));
  return {{"state", "published"},
          {"relation_id", relation.relation_id},
          {"commit", trim(commit.output)}};
}

nlohmann::json GitCoordinator::publish_knowledge_record(
    const nlohmann::json &record, const std::string &record_id,
    const std::string &ref) {
  const auto payload = canonical_json(record);
  const auto blob = run_process({"git", "-C", path_to_utf8(options_.repository),
                                 "hash-object", "-w", "--stdin"}, {}, payload);
  require_git(blob, "store knowledge record");
  const auto entry = "100644 blob " + trim(blob.output) + "\t" + record_id + ".json\n";
  const auto tree = run_process(
      {"git", "-C", path_to_utf8(options_.repository), "mktree"}, {}, entry);
  require_git(tree, "create knowledge tree");
  const auto heads = std::string{"refs/heads/"};
  if (!ref.starts_with(heads))
    throw std::invalid_argument("knowledge publication requires a heads ref");
  const auto tracking = "refs/remotes/" + options_.remote + "/" +
                        ref.substr(heads.size());
  const auto prior = run_process({"git", "-C", path_to_utf8(options_.repository),
                                  "rev-parse", "--verify", tracking});
  std::vector<std::string> command = {
      "git", "-c", "user.name=Repotraverse", "-c",
      "user.email=repotraverse@invalid", "-C", path_to_utf8(options_.repository),
      "commit-tree", trim(tree.output)};
  if (prior.exit_code == 0)
    command.insert(command.end(), {"-p", trim(prior.output)});
  command.insert(command.end(), {"-m", record.dump()});
  const auto commit = run_process(command);
  require_git(commit, "create knowledge commit");
  const auto push = run_process({"git", "-C", path_to_utf8(options_.repository),
                                 "push", options_.remote,
                                 trim(commit.output) + ":" + ref});
  require_git(push, "publish knowledge record");
  require_git(run_process({"git", "-C", path_to_utf8(options_.repository),
                           "update-ref", tracking, trim(commit.output)}),
              "update knowledge tracking ref");
  return {{"state", "published"}, {"record_id", record_id},
          {"commit", trim(commit.output)}, {"ref", ref}};
}

nlohmann::json GitCoordinator::publish_receipt(const EvidenceReceipt &receipt) {
  std::scoped_lock lock(mutex_);
  const auto synchronized = sync();
  if (synchronized.value("state", std::string{}) != "synchronized")
    return synchronized;
  auto result = publish_knowledge_record(
      {{"schema_version", kSchemaVersion}, {"record_type", "evidence_receipt"},
       {"producer_id", catalog_.producer_id()}, {"receipt", receipt}},
      receipt.receipt_id,
      "refs/heads/repotraverse/v1/knowledge/receipts/" + catalog_.producer_id());
  catalog_.store_receipt(receipt);
  return result;
}

nlohmann::json GitCoordinator::publish_inference_claim(const InferenceClaim &claim) {
  std::scoped_lock lock(mutex_);
  const auto synchronized = sync();
  if (synchronized.value("state", std::string{}) != "synchronized")
    return synchronized;
  auto result = publish_knowledge_record(
      {{"schema_version", kSchemaVersion}, {"record_type", "inference_claim"},
       {"producer_id", catalog_.producer_id()}, {"claim", claim}},
      claim.claim_id,
      "refs/heads/repotraverse/v1/knowledge/proposals/" + catalog_.producer_id());
  catalog_.store_inference_claim(claim);
  return result;
}

nlohmann::json
GitCoordinator::publish_knowledge_decision(const KnowledgeDecision &decision) {
  std::scoped_lock lock(mutex_);
  const auto synchronized = sync();
  if (synchronized.value("state", std::string{}) != "synchronized")
    return synchronized;
  auto result = publish_knowledge_record(
      {{"schema_version", kSchemaVersion}, {"record_type", "knowledge_decision"},
       {"producer_id", catalog_.producer_id()}, {"decision", decision}},
      decision.decision_id, options_.knowledge_ref);
  catalog_.store_knowledge_decision(decision, result.at("commit"));
  return result;
}

nlohmann::json GitCoordinator::publish_pr_import(const nlohmann::json &record) {
  std::scoped_lock lock(mutex_);
  const auto synchronized = sync();
  if (synchronized.value("state", std::string{}) != "synchronized")
    return synchronized;
  const auto import_id = stable_hash(canonical_json(record));
  auto result = publish_knowledge_record(
      {{"schema_version", kSchemaVersion}, {"record_type", "pr_import"},
       {"producer_id", catalog_.producer_id()}, {"import_id", import_id},
       {"record", record}},
      import_id,
      "refs/heads/repotraverse/v1/knowledge/pr-imports/" +
          catalog_.producer_id());
  catalog_.store_pr_import(import_id, record, result.at("commit"));
  return result;
}

std::size_t GitCoordinator::import_knowledge() {
  const auto prefix = "refs/remotes/" + options_.remote +
                      "/repotraverse/v1/knowledge";
  const auto authoritative = "refs/remotes/" + options_.remote + "/" +
      options_.knowledge_ref.substr(std::string{"refs/heads/"}.size());
  const auto refs = run_process({"git", "-C", path_to_utf8(options_.repository),
                                 "for-each-ref", "--format=%(refname)", prefix});
  require_git(refs, "list knowledge refs");
  std::size_t count = 0;
  auto knowledge_refs = lines(refs.output);
  std::stable_sort(knowledge_refs.begin(), knowledge_refs.end(),
                   [&](const auto &left, const auto &right) {
                     return (left == authoritative) < (right == authoritative);
                   });
  for (const auto &ref : knowledge_refs) {
    if (ref.empty() || ref.find("/lineage/") != std::string::npos) continue;
    const auto commits = run_process({"git", "-C", path_to_utf8(options_.repository),
                                      "rev-list", "--reverse", ref});
    require_git(commits, "list knowledge commits");
    for (const auto &commit : lines(commits.output)) {
      if (commit.empty() || catalog_.imported(commit)) continue;
      const auto names = run_process({"git", "-C", path_to_utf8(options_.repository),
                                      "ls-tree", "--name-only", commit});
      require_git(names, "list knowledge record");
      for (const auto &name : lines(names.output)) {
        if (!name.ends_with(".json")) continue;
        const auto show = run_process({"git", "-C", path_to_utf8(options_.repository),
                                       "show", commit + ":" + name});
        require_git(show, "read knowledge record");
        if (show.output.size() > options_.max_record_bytes) continue;
        const auto record = nlohmann::json::parse(show.output, nullptr, false);
        if (record.is_discarded() || !schema_v1(record) ||
            !trusted_producer(string_field(record, "producer_id"))) continue;
        try {
          const auto type = string_field(record, "record_type");
          if (type == "evidence_receipt" && record.contains("receipt")) {
            catalog_.store_receipt(record.at("receipt").get<EvidenceReceipt>());
            ++count;
          } else if (type == "inference_claim" && record.contains("claim")) {
            catalog_.store_inference_claim(record.at("claim").get<InferenceClaim>());
            ++count;
          } else if (type == "knowledge_decision" &&
                     record.contains("decision") && ref == authoritative) {
            catalog_.store_knowledge_decision(
                record.at("decision").get<KnowledgeDecision>(), commit);
            ++count;
          } else if (type == "pr_import" && record.contains("record")) {
            const auto imported = record.at("record");
            const auto import_id = string_field(record, "import_id");
            if (import_id == stable_hash(canonical_json(imported))) {
              catalog_.store_pr_import(import_id, imported, commit);
              ++count;
            }
          }
        } catch (const std::exception &) {
          continue;
        }
      }
      catalog_.mark_imported(commit);
    }
  }
  return count;
}

std::string
GitCoordinator::create_lease_commit(const nlohmann::json &lease,
                                    const std::string &parent) const {
  const auto empty = catalog_.root() / "empty-tree-input";
  if (!std::filesystem::exists(empty)) {
    std::ofstream output(empty, std::ios::binary);
    if (!output)
      throw std::runtime_error("cannot create empty tree input");
  }
  const auto tree =
      run_process({"git", "-C", path_to_utf8(options_.repository), "hash-object",
                   "-t", "tree", "-w", path_to_utf8(empty)});
  require_git(tree, "create empty lease tree");
  std::vector<std::string> command = {"git",
                                      "-c",
                                      "user.name=Repotraverse",
                                      "-c",
                                      "user.email=repotraverse@invalid",
                                      "-C",
                                      path_to_utf8(options_.repository),
                                      "commit-tree",
                                      trim(tree.output)};
  if (!parent.empty()) {
    command.push_back("-p");
    command.push_back(parent);
  }
  command.push_back("-m");
  command.push_back(lease.dump());
  const auto commit = run_process(command);
  require_git(commit, "create lease commit");
  return trim(commit.output);
}

nlohmann::json GitCoordinator::publish(const std::string &task_id,
                                       const nlohmann::json &lease,
                                       const std::string &expected_oid) {
  const auto oid = create_lease_commit(lease, expected_oid);
  auto command = std::vector<std::string>{"git", "-C",
                                          path_to_utf8(options_.repository), "push"};
  if (!expected_oid.empty())
    command.push_back("--force-with-lease=" + claim_ref(task_id) + ":" +
                      expected_oid);
  command.push_back(options_.remote);
  command.push_back(oid + ":" + claim_ref(task_id));
  const auto push = run_process(command);
  if (push.exit_code != 0)
    return {{"state", "claim_rejected"},
            {"message", utf8_lossy(trim(push.error))}};
  catalog_.observe_claim(task_id, oid, lease);
  auto result = lease;
  result["ref_oid"] = oid;
  result["state"] = lease.at("state");
  return result;
}

nlohmann::json GitCoordinator::acquire(const nlohmann::json &task) {
  std::scoped_lock lock(mutex_);
  if (!task.is_object())
    throw std::invalid_argument("task must be an object");
  const auto task_id = stable_hash(
      task.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict));
  const auto synchronized = sync();
  if (synchronized.at("state") != "synchronized") {
    return {{"task_id", task_id},
            {"state", "waiting_for_coordination"},
            {"message", synchronized.value("message", std::string{})}};
  }
  if (const auto fact = catalog_.fact_for_task(task_id)) {
    auto result = *fact;
    result["state"] = "completed";
    return result;
  }
  const auto existing = catalog_.claim(task_id);
  const auto now = now_seconds();
  std::string expected;
  if (existing) {
    if (existing->value("state", std::string{}) == "completed")
      return *existing;
    if (now <= existing->value("expires_at", std::int64_t{}) +
                   options_.grace_seconds) {
      auto result = *existing;
      result["state"] = existing->value("producer_id", std::string{}) ==
                                catalog_.producer_id()
                            ? "processing_local"
                            : "claimed_elsewhere";
      return result;
    }
    expected = existing->at("ref_oid").get<std::string>();
  }
  const nlohmann::json lease = {{"schema_version", kSchemaVersion},
                                {"task_id", task_id},
                                {"task", task},
                                {"producer_id", catalog_.producer_id()},
                                {"state", "processing"},
                                {"acquired_at", now},
                                {"expires_at", now + options_.lease_seconds},
                                {"heartbeat_sequence", 0}};
  auto result = publish(task_id, lease, expected);
  if (result.value("state", std::string{}) == "claim_rejected") {
    const auto retry = sync();
    if (retry.value("state", std::string{}) == "synchronized") {
      if (const auto winner = catalog_.claim(task_id)) {
        result = *winner;
        result["state"] = "claimed_elsewhere";
      }
    } else {
      result["state"] = "waiting_for_coordination";
    }
  }
  return result;
}

nlohmann::json GitCoordinator::heartbeat(const std::string &task_id) {
  std::scoped_lock lock(mutex_);
  sync();
  const auto existing = catalog_.claim(task_id);
  if (!existing)
    return {{"task_id", task_id}, {"state", "not_claimed"}};
  if (existing->value("producer_id", std::string{}) != catalog_.producer_id()) {
    auto result = *existing;
    result["state"] = "claimed_elsewhere";
    return result;
  }
  auto lease = *existing;
  lease.erase("ref_oid");
  lease.erase("observed_at");
  lease["state"] = "processing";
  lease["expires_at"] = now_seconds() + options_.lease_seconds;
  lease["heartbeat_sequence"] = lease.value("heartbeat_sequence", 0) + 1;
  return publish(task_id, lease, existing->at("ref_oid").get<std::string>());
}

nlohmann::json GitCoordinator::complete(const std::string &task_id,
                                        const nlohmann::json &result) {
  std::scoped_lock lock(mutex_);
  if (result.is_null())
    throw std::invalid_argument("result cannot be null");
  sync();
  const auto existing = catalog_.claim(task_id);
  if (!existing)
    return {{"task_id", task_id}, {"state", "not_claimed"}};
  if (existing->value("producer_id", std::string{}) != catalog_.producer_id()) {
    auto result = *existing;
    result["state"] = "lease_lost";
    return result;
  }
  const auto result_id = publish_result(task_id, result);
  auto lease = *existing;
  lease.erase("ref_oid");
  lease.erase("observed_at");
  lease["state"] = "completed";
  lease["result_id"] = result_id;
  lease["completed_at"] = now_seconds();
  lease["expires_at"] = now_seconds() + 3600;
  return publish(task_id, lease, existing->at("ref_oid").get<std::string>());
}

std::string GitCoordinator::publish_result(const std::string &task_id,
                                           const nlohmann::json &result) {
  const auto result_id = stable_hash(result.dump());
  const nlohmann::json fact = {{"schema_version", kSchemaVersion},
                               {"record_type", "extraction_result"},
                               {"fact_id", result_id},
                               {"task_id", task_id},
                               {"producer_id", catalog_.producer_id()},
                               {"result", result}};
  const auto payload = canonical_json(fact);
  const auto blob = run_process({"git", "-C", path_to_utf8(options_.repository),
                                 "hash-object", "-w", "--stdin"},
                                {}, payload);
  require_git(blob, "store result blob");
  const auto filename = task_id + ".jsonl";
  const auto entry =
      "100644 blob " + trim(blob.output) + "\t" + filename + "\n";
  const auto tree = run_process(
      {"git", "-C", path_to_utf8(options_.repository), "mktree"}, {}, entry);
  require_git(tree, "create result tree");
  const auto producer_ref =
      "refs/heads/repotraverse/v1/coordination/results/" + task_id.substr(0, 2) +
      "/" + task_id;
  const auto tracking = "refs/remotes/" + options_.remote +
                        "/repotraverse/v1/coordination/results/" +
                        task_id.substr(0, 2) + "/" + task_id;
  std::vector<std::string> command = {"git",
                                      "-c",
                                      "user.name=Repotraverse",
                                      "-c",
                                      "user.email=repotraverse@invalid",
                                      "-C",
                                      path_to_utf8(options_.repository),
                                      "commit-tree",
                                      trim(tree.output)};
  command.push_back("-m");
  command.push_back(nlohmann::json({{"record_type", "result_shard"},
                                    {"task_id", task_id},
                                    {"fact_id", result_id}})
                        .dump());
  const auto commit = run_process(command);
  require_git(commit, "create result commit");
  const auto push =
      run_process({"git", "-C", path_to_utf8(options_.repository), "push",
                   options_.remote, trim(commit.output) + ":" + producer_ref});
  require_git(push, "publish result");
  const auto update =
      run_process({"git", "-C", path_to_utf8(options_.repository), "update-ref",
                   tracking, trim(commit.output)});
  require_git(update, "update producer tracking ref");
  catalog_.store_fact(result_id, task_id, fact, trim(commit.output));
  catalog_.mark_imported(trim(commit.output));
  return result_id;
}

nlohmann::json GitCoordinator::status(const std::string &task_id) const {
  std::scoped_lock lock(mutex_);
  if (const auto found = catalog_.claim(task_id))
    return *found;
  return {{"task_id", task_id}, {"state", "not_claimed"}};
}

} // namespace history
