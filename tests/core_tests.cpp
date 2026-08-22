#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>

#include "history/query.hpp"
#include "history/config.hpp"

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

void test_move_and_rename() {
  const auto before = bundle("a", {element("old-id", "oldName", "old.cpp")});
  const auto after = bundle("b", {element("new-id", "newName", "new.cpp")});
  const auto result = history::trace_transition(before, after);
  require(result.facts.size() == 1, "one lineage fact expected");
  require(result.facts[0].continuity == "moved_and_renamed",
          "move/rename not detected");
  require(result.facts[0].content_change == "none",
          "pure refactoring counted as content");
  require(result.candidates.size() == 1 &&
              result.candidates[0].automatically_resolved,
          "automatic candidate missing");
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
}

void test_production_config_validation() {
  const auto config = history::parse_service_config(
      {{"schema_version", history::kSchemaVersion},
       {"repository_id", "device-main"},
       {"catalog", "catalog"},
       {"artifact_repository", "artifacts"}});
  require(config.listen_address == "127.0.0.1", "loopback default changed");
  bool rejected = false;
  try {
    history::parse_service_config(
        {{"schema_version", history::kSchemaVersion},
         {"repository_id", "device-main"},
         {"catalog", "catalog"},
         {"artifact_repository", "artifacts"},
         {"listen_address", "0.0.0.0"}});
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "non-loopback production bind was accepted");
}

void test_reviewed_extract_candidate() {
  auto before_caller = element("caller", "caller", "source.cpp", "i", "old");
  auto after_caller = element("caller", "caller", "source.cpp", "i", "new");
  after_caller.referenced_compiler_ids = {"helper"};
  auto helper = element("helper", "helper", "source.cpp", "h", "body");
  const auto result = history::trace_transition(
      bundle("before", {before_caller}),
      bundle("after", {after_caller, helper}));
  require(result.relation_candidates.size() == 1,
          "extract relation candidate was not emitted");
  require(result.relation_candidates.front().kind == "extract" &&
              result.relation_candidates.front().review_state == "candidate",
          "extract relation was incorrectly auto-accepted");
}
} // namespace

int main() {
  try {
    test_move_and_rename();
    test_content_and_local_rename();
    test_ambiguity_and_assertion();
    test_producer_identity_round_trip();
    test_schema_hash();
    test_production_config_validation();
    test_reviewed_extract_candidate();
    std::cout << "all tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
  }
}
