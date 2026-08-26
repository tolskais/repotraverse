#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>

#include "history/config.hpp"
#include "history/query.hpp"

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

history::ElementSnapshot element(std::string id, std::string name,
                                 std::string path,
                                 std::string interface = "interface",
                                 std::string body = "body",
                                 std::string dependencies = "deps") {
  history::ElementSnapshot result;
  result.compiler_id = std::move(id);
  result.kind = "function";
  result.qualified_name = std::move(name);
  result.location.path = std::move(path);
  result.location.begin_line = 10;
  result.location.end_line = 20;
  result.interface_fingerprint = history::stable_hash(interface);
  result.implementation_fingerprint = history::stable_hash(body);
  result.dependency_fingerprint = history::stable_hash(dependencies);
  return result;
}
history::EvidenceBundle bundle(std::string revision,
                               std::vector<history::ElementSnapshot> elements) {
  history::EvidenceBundle result;
  result.source_revision = std::move(revision);
  result.configuration = "arm";
  result.elements = std::move(elements);
  return result;
}

class BundleStore final : public history::FactStore {
public:
  explicit BundleStore(std::map<std::string, history::EvidenceBundle> bundles)
      : bundles_(std::move(bundles)) {}

  history::EvidenceBundle
  load(const std::filesystem::path &path) const override {
    return bundles_.at(path.string());
  }

private:
  std::map<std::string, history::EvidenceBundle> bundles_;
};

void test_move_and_rename() {
  const auto before = bundle("a", {element("old-id", "oldName", "old.cpp")});
  const auto after = bundle("b", {element("new-id", "newName", "new.cpp")});
  const auto result = history::trace_transition(before, after);
  const auto unresolved = std::find_if(
      result.facts.begin(), result.facts.end(),
      [](const auto &fact) { return fact.before_element == "old-id"; });
  require(unresolved != result.facts.end() &&
              unresolved->continuity == "deleted_or_unresolved",
          "unreviewed move/rename was treated as established lineage");
  require(result.candidates.size() == 1 &&
              !result.candidates[0].automatically_resolved &&
              result.candidates[0].proposed_continuity == "moved_and_renamed",
          "reviewable move/rename candidate missing");
}

void test_content_and_local_rename() {
  auto before_element = element("same", "f", "f.cpp", "interface", "body-a");
  auto after_element = element("same", "f", "f.cpp", "interface", "body-b");
  auto result = history::trace_transition(bundle("a", {before_element}),
                                          bundle("b", {after_element}));
  require(result.facts[0].content_change == "implementation",
          "body change missing");

  // Binding-normalized extractor fingerprints are represented by equal body
  // hashes.
  after_element.implementation_fingerprint =
      before_element.implementation_fingerprint;
  result = history::trace_transition(bundle("a", {before_element}),
                                     bundle("b", {after_element}));
  require(result.facts[0].content_change == "none",
          "local rename must be normalized");
}

void test_ambiguity_and_assertion() {
  const auto before = bundle("a", {element("old", "f", "old.cpp")});
  const auto after = bundle(
      "b", {element("one", "g", "one.cpp"), element("two", "h", "two.cpp")});
  const auto ambiguous = history::trace_transition(before, after);
  require(ambiguous.candidates.size() == 2,
          "ambiguous candidates must be retained");
  const auto unresolved = std::find_if(
      ambiguous.facts.begin(), ambiguous.facts.end(),
      [](const auto &fact) { return fact.before_element == "old"; });
  require(unresolved != ambiguous.facts.end() &&
              unresolved->continuity == "deleted_or_unresolved",
          "ambiguity was merged");

  history::LineageAssertion assertion;
  assertion.assertion_id = "review-1";
  assertion.before_element = "old";
  assertion.after_element = "two";
  assertion.relation = "same_element";
  assertion.status = "accepted";
  assertion.reviewed_by = "developer";
  const auto resolved = history::trace_transition(before, after, {assertion});
  const auto reviewed = std::find_if(
      resolved.facts.begin(), resolved.facts.end(),
      [](const auto &fact) { return fact.before_element == "old"; });
  require(reviewed != resolved.facts.end() &&
              reviewed->after_element == "two" &&
              reviewed->resolution == "reviewed_assertion",
          "accepted assertion did not resolve lineage");
}

void test_synthetic_lineage_capability_boundary() {
  struct Scenario {
    const char *name;
    history::ElementSnapshot before, after;
    const char *continuity;
    const char *content_change;
    const char *confidence;
  };
  const std::vector<Scenario> resolved = {
      {"body change", element("same", "f", "a.cpp", "i", "old"),
       element("same", "f", "a.cpp", "i", "new"), "same", "implementation",
       "exact"},
      {"signature change", element("same", "f", "a.cpp", "old", "b"),
       element("same", "f", "a.cpp", "new", "b"), "same", "interface", "exact"},
      {"dependency change", element("same", "f", "a.cpp", "i", "b", "x"),
       element("same", "f", "a.cpp", "i", "b", "y"), "same", "none", "exact"}};
  for (const auto &scenario : resolved) {
    const auto result = history::trace_transition(
        bundle("before", {scenario.before}), bundle("after", {scenario.after}));
    require(result.facts.size() == 1, scenario.name);
    require(result.facts.front().continuity == scenario.continuity,
            scenario.name);
    require(result.facts.front().content_change == scenario.content_change,
            scenario.name);
    require(result.facts.front().confidence == scenario.confidence,
            scenario.name);
  }

  const auto rename_and_modify = history::trace_transition(
      bundle("before", {element("old", "parse", "parser.cpp", "i", "old")}),
      bundle("after",
             {element("new", "parseToken", "parser.cpp", "i", "new")}));
  const auto unresolved = std::find_if(
      rename_and_modify.facts.begin(), rename_and_modify.facts.end(),
      [](const auto &fact) { return fact.before_element == "old"; });
  require(unresolved != rename_and_modify.facts.end() &&
              unresolved->continuity == "deleted_or_unresolved" &&
              unresolved->confidence == "ambiguous",
          "rename plus modification capability boundary changed");

  const auto deleted = history::trace_transition(
      bundle("before", {element("old", "f", "a.cpp")}), bundle("after", {}));
  require(deleted.facts.front().continuity == "deleted_or_unresolved",
          "deletion must remain explicitly unresolved");
}

void test_build_variant_identity_round_trip() {
  history::CompileContext context;
  context.configuration = "product-a-debug";
  context.build_variant.product = "product-a";
  context.build_variant.target = "cortex-r5";
  context.build_variant.configuration = "debug";
  context.build_variant.variant_id =
      history::stable_hash(nlohmann::json({{"product", "product-a"},
                                           {"target", "cortex-r5"},
                                           {"configuration", "debug"}})
                               .dump());
  const auto restored = nlohmann::json(context).get<history::CompileContext>();
  require(restored.build_variant.variant_id ==
                  context.build_variant.variant_id &&
              restored.build_variant.target == "cortex-r5",
          "build variant identity was not preserved");
}

void test_producer_identity_round_trip() {
  auto original = bundle("a", {element("id", "f", "f.cpp")});
  original.producer.tool_version = "0.1.0";
  original.producer.llvm_version = "llvm-test";
  original.producer.clang_version = "clang-test";
  original.producer.build_mode = "native";
  original.producer.host_architecture = "x86_64";

  const auto restored = nlohmann::json(original).get<history::EvidenceBundle>();
  require(restored.producer.tool_version == "0.1.0", "tool version was lost");
  require(restored.producer.llvm_version == "llvm-test",
          "LLVM version was lost");
  require(restored.producer.clang_version == "clang-test",
          "Clang version was lost");
  require(restored.producer.build_mode == "native", "build mode was lost");
  require(restored.producer.host_architecture == "x86_64",
          "host architecture was lost");
}

void test_schema_hash() {
  require(history::stable_hash("") == "99aa06d3014798d86001c324468d497f",
          "XXH3-128 canonical encoding changed");
  history::StableHashBuilder streaming;
  streaming.append("canonical ");
  streaming.append("body");
  require(streaming.digest() == history::stable_hash("canonical body"),
          "streaming XXH3-128 differs from one-shot hashing");
}

void test_production_config_validation() {
  const auto config = history::parse_catalog_config(
      {{"schema_version", history::kSchemaVersion},
       {"repository_id", "device-main"},
       {"catalog", "catalog"},
       {"analysis_repository", "analysis"}});
  require(config.analysis_sync_freshness_seconds == 30,
          "analysis synchronization freshness default changed");
  require(config.workspace_mode == "auto" &&
              config.workspace_max_revisions == 2 &&
              config.workspace_max_bytes == 0,
          "disk-bounded workspace defaults changed");
  bool rejected = false;
  try {
    history::parse_catalog_config({{"schema_version", history::kSchemaVersion},
                                   {"repository_id", "device-main"},
                                   {"catalog", "catalog"},
                                   {"analysis_repository", "analysis"},
                                   {"port", 7341}});
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "removed inbound HTTP option was accepted");
}

void test_reviewed_extract_candidate() {
  auto before_caller = element("caller", "caller", "source.cpp", "i", "old");
  auto after_caller = element("caller", "caller", "source.cpp", "i", "new");
  after_caller.referenced_compiler_ids = {"helper"};
  auto helper = element("helper", "helper", "source.cpp", "h", "body");
  const auto result =
      history::trace_transition(bundle("before", {before_caller}),
                                bundle("after", {after_caller, helper}));
  require(result.relation_candidates.size() == 1,
          "extract relation candidate was not emitted");
  require(result.relation_candidates.front().kind == "extract" &&
              result.relation_candidates.front().review_state == "candidate",
          "extract relation was incorrectly auto-accepted");
}

void test_history_statistics_are_evidence_not_a_label() {
  auto first = element("same", "target", "target.cpp", "api", "one");
  auto second = element("same", "target", "target.cpp", "api", "two");
  auto third = second;
  const auto store = std::make_shared<BundleStore>(
      std::map<std::string, history::EvidenceBundle>{
          {"one", bundle("r1", {first})},
          {"two", bundle("r2", {second})},
          {"three", bundle("r3", {third})}});
  history::QueryService service(store);
  const auto response = service.execute(
      {{"schema_version", history::kSchemaVersion},
       {"query", "element.history_stats"},
       {"params", {{"bundles", {"one", "two", "three"}},
                   {"change_units",
                    {{{"change_unit_id", "pr-1"},
                      {"base_revision", "r1"},
                      {"result_revision", "r2"},
                      {"association_status", "confirmed_pr"},
                      {"integrated_commits", {"c1", "c2"}}},
                     {{"change_unit_id", "pr-2"},
                      {"base_revision", "r2"},
                      {"result_revision", "r3"},
                      {"association_status", "confirmed_pr"},
                      {"integrated_commits", {"c3"}}}}},
                   {"revision_timestamps", {{"r1", 10}, {"r2", 20},
                                             {"r3", 30}}}}}});
  const auto &facts = response.at("result").at("elements").front();
  require(facts.at("observable_transitions") == 2,
          "adjacent observation opportunities were counted incorrectly");
  require(facts.at("implementation_changed_transitions") == 1 &&
              facts.at("implementation_change_rate") == 0.5,
          "implementation history evidence is incomplete");
  require(facts.at("change_events").size() == 1 &&
              facts.at("observations").size() == 3,
          "LLM-facing history evidence is missing events or observations");
  require(response.at("result").at("grouping_unit") == "change_unit" &&
              facts.at("observed_change_units") == 2 &&
              facts.at("change_units_with_implementation_changes") == 1 &&
              facts.at("change_events").front().at("change_unit_id") ==
                  "pr-1",
          "element history was not grouped by developer change unit");
  require(!facts.contains("stability") && !facts.contains("classification"),
          "factual history unexpectedly emitted a stability judgment");

  const auto file_params = nlohmann::json{
      {"bundles", {"one", "two", "three"}},
      {"change_units",
       {{{"change_unit_id", "pr-1"},
         {"base_revision", "r1"},
         {"result_revision", "r2"},
         {"association_status", "confirmed_pr"}},
        {{"change_unit_id", "pr-2"},
         {"base_revision", "r2"},
         {"result_revision", "r3"},
         {"association_status", "confirmed_pr"}}}}};
  const auto file_response = service.execute(
      {{"schema_version", history::kSchemaVersion},
       {"query", "file.history_stats"},
       {"params", file_params}});
  const auto &file = file_response.at("result").at("files").front();
  require(file.at("path") == "target.cpp" &&
              file.at("observed_change_units") == 2 &&
              file.at("change_units_with_element_changes") == 1 &&
              file.at("change_units_with_implementation_changes") == 1 &&
              file.at("change_events").front().at("change_unit_id") ==
                  "pr-1",
          "file-level change-unit evidence is incomplete");
  require(!file.contains("stability") && !file.contains("classification"),
          "file evidence unexpectedly emitted a stability judgment");

  const auto discontinuous_store = std::make_shared<BundleStore>(
      std::map<std::string, history::EvidenceBundle>{
          {"one", bundle("r1", {first})}, {"two", bundle("r2", {})},
          {"three", bundle("r3", {third})}});
  history::QueryService discontinuous(discontinuous_store);
  const auto discontinuous_response = discontinuous.execute(
      {{"schema_version", history::kSchemaVersion},
       {"query", "element.history_stats"},
       {"params", {{"bundles", {"one", "two", "three"}}}}});
  const auto &discontinuous_facts =
      discontinuous_response.at("result").at("elements").front();
  require(discontinuous_facts.at("observable_transitions") == 0,
          "a gap between observations was treated as an observable transition");
  require(discontinuous_response.at("result").at("grouping_unit") ==
                  "revision_transition" &&
              discontinuous_facts.at("observed_change_units").is_null(),
          "revision transitions were mislabeled as developer change units");
  require(discontinuous_facts.at("evidence_status") == "partial",
          "an unresolved observation gap was not exposed");
}
} // namespace

int main() {
  try {
    test_move_and_rename();
    test_content_and_local_rename();
    test_ambiguity_and_assertion();
    test_synthetic_lineage_capability_boundary();
    test_build_variant_identity_round_trip();
    test_producer_identity_round_trip();
    test_schema_hash();
    test_production_config_validation();
    test_reviewed_extract_candidate();
    test_history_statistics_are_evidence_not_a_label();
    std::cout << "all tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
  }
}
