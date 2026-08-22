#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>

#include "history/query.hpp"

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

history::ElementSnapshot element(std::string id, std::string name, std::string path,
                                 std::string interface = "interface", std::string body = "body",
                                 std::string dependencies = "deps") {
    history::ElementSnapshot result;
    result.compiler_id = std::move(id); result.kind = "function";
    result.qualified_name = std::move(name); result.location.path = std::move(path);
    result.location.begin_line = 10; result.location.end_line = 20;
    result.interface_fingerprint = history::stable_hash(interface);
    result.implementation_fingerprint = history::stable_hash(body);
    result.dependency_fingerprint = history::stable_hash(dependencies);
    return result;
}
history::EvidenceBundle bundle(std::string revision, std::vector<history::ElementSnapshot> elements) {
    history::EvidenceBundle result; result.source_revision = std::move(revision);
    result.configuration = "arm"; result.elements = std::move(elements); return result;
}

void test_move_and_rename() {
    const auto before = bundle("a", {element("old-id", "oldName", "old.cpp")});
    const auto after = bundle("b", {element("new-id", "newName", "new.cpp")});
    const auto result = history::trace_transition(before, after);
    require(result.facts.size() == 1, "one lineage fact expected");
    require(result.facts[0].continuity == "moved_and_renamed", "move/rename not detected");
    require(result.facts[0].content_change == "none", "pure refactoring counted as content");
    require(result.candidates.size() == 1 && result.candidates[0].automatically_resolved,
            "automatic candidate missing");
}

void test_content_and_local_rename() {
    auto before_element = element("same", "f", "f.cpp", "interface", "body-a");
    auto after_element = element("same", "f", "f.cpp", "interface", "body-b");
    auto result = history::trace_transition(bundle("a", {before_element}), bundle("b", {after_element}));
    require(result.facts[0].content_change == "implementation", "body change missing");

    // Binding-normalized extractor fingerprints are represented by equal body hashes.
    after_element.implementation_fingerprint = before_element.implementation_fingerprint;
    result = history::trace_transition(bundle("a", {before_element}), bundle("b", {after_element}));
    require(result.facts[0].content_change == "none", "local rename must be normalized");
}

void test_ambiguity_and_assertion() {
    const auto before = bundle("a", {element("old", "f", "old.cpp")});
    const auto after = bundle("b", {element("one", "g", "one.cpp"), element("two", "h", "two.cpp")});
    const auto ambiguous = history::trace_transition(before, after);
    require(ambiguous.candidates.size() == 2, "ambiguous candidates must be retained");
    const auto unresolved = std::find_if(ambiguous.facts.begin(), ambiguous.facts.end(),
        [](const auto& fact) { return fact.before_element == "old"; });
    require(unresolved != ambiguous.facts.end() &&
                unresolved->continuity == "deleted_or_unresolved",
            "ambiguity was merged");

    history::LineageAssertion assertion;
    assertion.assertion_id = "review-1"; assertion.before_element = "old";
    assertion.after_element = "two"; assertion.relation = "same_element";
    assertion.status = "accepted"; assertion.reviewed_by = "developer";
    const auto resolved = history::trace_transition(before, after, {assertion});
    const auto reviewed = std::find_if(resolved.facts.begin(), resolved.facts.end(),
        [](const auto& fact) { return fact.before_element == "old"; });
    require(reviewed != resolved.facts.end() && reviewed->after_element == "two" &&
                reviewed->resolution == "reviewed_assertion",
            "accepted assertion did not resolve lineage");
}
}  // namespace

int main() {
    try {
        test_move_and_rename(); test_content_and_local_rename(); test_ambiguity_and_assertion();
        std::cout << "all tests passed\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n'; return 1;
    }
}
