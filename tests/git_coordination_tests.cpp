#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "history/catalog.hpp"
#include "history/git_coordination.hpp"
#include "history/process.hpp"

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void git(const std::vector<std::string> &arguments) {
  auto command = std::vector<std::string>{"git"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto result = history::run_process(command);
  if (result.exit_code != 0)
    throw std::runtime_error("Git test setup failed: " + result.error);
}

} // namespace

int main() {
  try {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("repotraverse-coordination-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto remote = root / "remote.git";
    const auto first_repo = root / "first-repo";
    const auto second_repo = root / "second-repo";
    std::filesystem::create_directories(root);
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    } cleanup{root};

    git({"init", "--bare", remote.string()});
    git({"clone", remote.string(), first_repo.string()});
    git({"clone", remote.string(), second_repo.string()});

    history::Catalog first_catalog(root / "first-catalog");
    history::Catalog second_catalog(root / "second-catalog");
    history::CoordinationOptions first_options;
    first_options.repository = first_repo;
    first_options.lease_seconds = 5;
    first_options.grace_seconds = 0;
    history::CoordinationOptions second_options;
    second_options.repository = second_repo;
    second_options.lease_seconds = 5;
    second_options.grace_seconds = 0;
    history::GitCoordinator first(first_catalog, first_options);
    history::GitCoordinator second(second_catalog, second_options);
    const nlohmann::json task = {{"source_commit", "0123456789abcdef"},
                                 {"translation_unit", "src/device.cpp"},
                                 {"context", "arm-debug"},
                                 {"extractor", "test-v1"}};

    const auto acquired = first.acquire(task);
    require(acquired.value("state", std::string{}) == "processing",
            "first VM did not acquire task");
    const auto task_id = acquired.at("task_id").get<std::string>();
    require(task_id.size() == 32, "task identifier is not XXH3-128 width");

    const auto denied = second.acquire(task);
    require(denied.value("state", std::string{}) == "claimed_elsewhere",
            "second VM did not observe active owner");
    require(denied.value("producer_id", std::string{}) ==
                first_catalog.producer_id(),
            "second VM observed wrong owner");

    std::this_thread::sleep_for(std::chrono::seconds(6));
    const auto takeover = second.acquire(task);
    require(takeover.value("state", std::string{}) == "processing",
            "second VM did not take over expired lease");
    require(takeover.value("producer_id", std::string{}) ==
                second_catalog.producer_id(),
            "takeover has wrong owner");

    const auto late = first.heartbeat(task_id);
    require(late.value("state", std::string{}) == "claimed_elsewhere",
            "previous owner retained lease after takeover");

    const auto completed = second.complete(
        task_id,
        {{"schema_version", history::kSchemaVersion},
         {"record_type", "tu_failure"},
         {"source_revision", "fixture"},
         {"translation_unit", "fixture.cpp"},
         {"context_id", "fixture-context"},
         {"coverage", {{"status", "partial"}, {"gaps", {"fixture"}}}}});
    require(completed.value("state", std::string{}) == "completed",
            "new owner could not complete task");
    require(completed.value("result_id", std::string{}).size() == 32,
            "completed lease lacks result identity");

    first.sync();
    const auto visible = first.status(task_id);
    require(visible.value("state", std::string{}) == "completed",
            "completion did not propagate to first VM");
    require(first_catalog.fact_for_task(task_id).has_value(),
            "published result fact did not propagate to first VM");

    history::LineageRelation relation;
    relation.repository_id = "fixture-main";
    relation.kind = "extract";
    relation.source_element_ids = {"before"};
    relation.target_element_ids = {"after", "extracted"};
    relation.review_state = "accepted";
    relation.reviewer = "reviewer";
    relation.relation_id = history::stable_hash(
        relation.repository_id + "\n" + relation.kind + "\n" +
        nlohmann::json(relation.source_element_ids).dump() + "\n" +
        nlohmann::json(relation.target_element_ids).dump());
    require(first.publish_review(relation).value("state", std::string{}) ==
                "published",
            "lineage review was not published");
    second.sync();
    require(second_catalog.lineage_relation(relation.relation_id).has_value(),
            "lineage review did not propagate");

    history::Catalog offline_catalog(root / "offline-catalog");
    history::CoordinationOptions offline_options;
    offline_options.repository = first_repo;
    offline_options.remote = "missing-remote";
    offline_options.lease_seconds = 1;
    offline_options.grace_seconds = 0;
    history::GitCoordinator offline(offline_catalog, offline_options);
    const auto unavailable = offline.acquire({{"task", "offline"}});
    require(unavailable.value("state", std::string{}) ==
                "waiting_for_coordination",
            "offline VM started uncoordinated work");
    std::cout << "git coordination tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
