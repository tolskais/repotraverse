#include "history/lineage.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace history {
namespace {
template <typename T>
void optional(const nlohmann::json& value, const char* key, T& output) {
    if (const auto found = value.find(key); found != value.end() && !found->is_null()) found->get_to(output);
}

std::string continuity(const ElementSnapshot& before, const ElementSnapshot& after) {
    const bool renamed = before.qualified_name != after.qualified_name;
    const bool moved = before.location.path != after.location.path;
    if (renamed && moved) return "moved_and_renamed";
    if (renamed) return "renamed";
    if (moved) return "moved";
    return "same";
}

std::string content_change(const ElementSnapshot& before, const ElementSnapshot& after) {
    const bool interface = before.interface_fingerprint != after.interface_fingerprint;
    const bool implementation = before.implementation_fingerprint != after.implementation_fingerprint;
    if (interface && implementation) return "both";
    if (interface) return "interface";
    if (implementation) return "implementation";
    return "none";
}

std::string shape_key(const ElementSnapshot& element) {
    return element.kind + "\n" + element.interface_fingerprint + "\n" +
           element.implementation_fingerprint + "\n" + element.dependency_fingerprint;
}

Coverage coverage_for(const EvidenceBundle& before, const EvidenceBundle& after) {
    Coverage result;
    result.status = before.coverage.status == "complete" && after.coverage.status == "complete"
                        ? "complete" : "partial";
    for (const auto& gap : before.coverage.gaps) result.gaps.push_back("before: " + gap);
    for (const auto& gap : after.coverage.gaps) result.gaps.push_back("after: " + gap);
    result.capabilities = {"element_lineage", "interface_fingerprint",
                           "implementation_fingerprint", "dependency_fingerprint"};
    return result;
}
}  // namespace

void to_json(nlohmann::json& value, const LineageCandidate& item) {
    value = {{"candidate_id", item.candidate_id}, {"before_element", item.before_element},
             {"after_element", item.after_element}, {"proposed_continuity", item.proposed_continuity},
             {"confidence", item.confidence}, {"match_basis", item.match_basis},
             {"automatically_resolved", item.automatically_resolved}};
}
void from_json(const nlohmann::json& value, LineageCandidate& item) {
    optional(value, "candidate_id", item.candidate_id); optional(value, "before_element", item.before_element);
    optional(value, "after_element", item.after_element); optional(value, "proposed_continuity", item.proposed_continuity);
    optional(value, "confidence", item.confidence); optional(value, "match_basis", item.match_basis);
    optional(value, "automatically_resolved", item.automatically_resolved);
}
void to_json(nlohmann::json& value, const LineageAssertion& item) {
    value = {{"assertion_id", item.assertion_id}, {"before_element", item.before_element},
             {"after_element", item.after_element}, {"relation", item.relation}, {"status", item.status},
             {"created_by", item.created_by}, {"reviewed_by", item.reviewed_by}};
}
void from_json(const nlohmann::json& value, LineageAssertion& item) {
    optional(value, "assertion_id", item.assertion_id); optional(value, "before_element", item.before_element);
    optional(value, "after_element", item.after_element); optional(value, "relation", item.relation);
    optional(value, "status", item.status); optional(value, "created_by", item.created_by);
    optional(value, "reviewed_by", item.reviewed_by);
}
void to_json(nlohmann::json& value, const TransitionFact& item) {
    value = {{"historical_element_id", item.historical_element_id}, {"before_element", item.before_element},
             {"after_element", item.after_element}, {"before_revision", item.before_revision},
             {"after_revision", item.after_revision}, {"continuity", item.continuity},
             {"content_change", item.content_change},
             {"dependencies_changed", item.dependencies_changed},
             {"resolution", item.resolution},
             {"confidence", item.confidence}};
    if (item.before_location) value["before_location"] = *item.before_location;
    if (item.after_location) value["after_location"] = *item.after_location;
}
void to_json(nlohmann::json& value, const TransitionResult& item) {
    value = {{"before_revision", item.before_revision}, {"after_revision", item.after_revision},
             {"configuration", item.configuration}, {"coverage", item.coverage},
             {"candidates", item.candidates},
             {"relation_candidates", item.relation_candidates},
             {"reviewed_relations", item.reviewed_relations},
             {"facts", item.facts}};
}

TransitionResult trace_transition(const EvidenceBundle& before, const EvidenceBundle& after,
                                  const std::vector<LineageAssertion>& assertions) {
    TransitionResult result{before.source_revision, after.source_revision, after.configuration,
                            coverage_for(before, after)};
    if (before.configuration != after.configuration) {
        result.coverage.status = "partial";
        result.coverage.gaps.push_back("configuration mismatch");
    }
    std::vector<bool> used(after.elements.size());
    std::map<std::string, std::size_t> after_by_id;
    std::multimap<std::string, std::size_t> after_by_shape;
    for (std::size_t i = 0; i < after.elements.size(); ++i) {
        after_by_id.emplace(after.elements[i].compiler_id, i);
        after_by_shape.emplace(shape_key(after.elements[i]), i);
    }
    std::map<std::string, const LineageAssertion*> accepted;
    std::set<std::string> rejected;
    for (const auto& assertion : assertions) {
        const auto key = assertion.before_element + "\n" + assertion.after_element;
        if (assertion.status == "accepted" && assertion.relation == "same_element") accepted[assertion.before_element] = &assertion;
        if (assertion.status == "accepted" && assertion.relation == "not_same_element") rejected.insert(key);
    }
    for (const auto& old_element : before.elements) {
        std::optional<std::size_t> match;
        std::string resolution = "automatic";
        std::string confidence = "exact";
        if (const auto asserted = accepted.find(old_element.compiler_id); asserted != accepted.end()) {
            if (const auto found = after_by_id.find(asserted->second->after_element); found != after_by_id.end() && !used[found->second]) {
                match = found->second; resolution = "reviewed_assertion"; confidence = "reviewed";
            }
        }
        if (!match) {
            if (const auto found = after_by_id.find(old_element.compiler_id); found != after_by_id.end() && !used[found->second]) match = found->second;
        }
        if (!match) {
            std::vector<std::size_t> candidates;
            const auto [begin, end] = after_by_shape.equal_range(shape_key(old_element));
            for (auto it = begin; it != end; ++it) {
                const auto key = old_element.compiler_id + "\n" + after.elements[it->second].compiler_id;
                if (!used[it->second] && !rejected.contains(key)) candidates.push_back(it->second);
            }
            if (candidates.size() == 1) {
                match = candidates.front(); confidence = "high";
                const auto& next = after.elements[*match];
                LineageCandidate candidate;
                candidate.before_element = old_element.compiler_id; candidate.after_element = next.compiler_id;
                candidate.proposed_continuity = continuity(old_element, next); candidate.confidence = "high";
                candidate.match_basis = {"interface_shape_equal", "implementation_shape_equal", "dependency_shape_equal"};
                candidate.automatically_resolved = true;
                candidate.candidate_id = "candidate-" + stable_hash(candidate.before_element + candidate.after_element);
                result.candidates.push_back(std::move(candidate));
            } else if (candidates.size() > 1) {
                result.coverage.status = "partial";
                result.coverage.gaps.push_back("ambiguous lineage for " + old_element.compiler_id);
                for (const auto index : candidates) {
                    LineageCandidate candidate;
                    candidate.before_element = old_element.compiler_id; candidate.after_element = after.elements[index].compiler_id;
                    candidate.proposed_continuity = continuity(old_element, after.elements[index]); candidate.confidence = "ambiguous";
                    candidate.match_basis = {"multiple_exact_shape_matches"};
                    candidate.candidate_id = "candidate-" + stable_hash(candidate.before_element + candidate.after_element);
                    result.candidates.push_back(std::move(candidate));
                }
            }
        }
        TransitionFact fact;
        fact.before_revision = before.source_revision; fact.after_revision = after.source_revision;
        fact.before_element = old_element.compiler_id; fact.before_location = old_element.location;
        if (match) {
            const auto& next = after.elements[*match]; used[*match] = true;
            fact.after_element = next.compiler_id; fact.after_location = next.location;
            fact.historical_element_id = "history-" + stable_hash(old_element.compiler_id);
            fact.continuity = continuity(old_element, next); fact.content_change = content_change(old_element, next);
            fact.dependencies_changed = old_element.dependency_fingerprint !=
                                        next.dependency_fingerprint;
            fact.resolution = resolution; fact.confidence = confidence;
        } else {
            fact.historical_element_id = "history-" + stable_hash(old_element.compiler_id);
            fact.continuity = "deleted_or_unresolved"; fact.content_change = "unverified";
            fact.confidence = "ambiguous";
            result.coverage.status = "partial";
            result.coverage.gaps.push_back("unresolved successor for " + old_element.compiler_id);
        }
        result.facts.push_back(std::move(fact));
    }
    for (std::size_t i = 0; i < after.elements.size(); ++i) if (!used[i]) {
        TransitionFact fact;
        fact.historical_element_id = "history-" + stable_hash(after.elements[i].compiler_id);
        fact.after_element = after.elements[i].compiler_id; fact.after_revision = after.source_revision;
        fact.before_revision = before.source_revision; fact.after_location = after.elements[i].location;
        fact.continuity = "added_or_unresolved"; fact.content_change = "unverified";
        fact.confidence = "ambiguous"; result.facts.push_back(std::move(fact));
    }
    const auto references = [](const ElementSnapshot& element,
                               const ElementSnapshot& target) {
        return std::find(element.referenced_compiler_ids.begin(),
                         element.referenced_compiler_ids.end(),
                         target.compiler_id) !=
               element.referenced_compiler_ids.end();
    };
    for (const auto& fact : result.facts) {
        if (fact.before_element.empty() || fact.after_element.empty()) continue;
        const auto old_it = std::find_if(before.elements.begin(), before.elements.end(),
            [&](const auto& item) { return item.compiler_id == fact.before_element; });
        const auto new_it = std::find_if(after.elements.begin(), after.elements.end(),
            [&](const auto& item) { return item.compiler_id == fact.after_element; });
        if (old_it == before.elements.end() || new_it == after.elements.end()) continue;
        for (std::size_t i = 0; i < after.elements.size(); ++i) {
            if (used[i]) continue;
            if (references(*new_it, after.elements[i]) &&
                !references(*old_it, after.elements[i])) {
                LineageRelation relation;
                relation.kind = "extract";
                relation.source_element_ids = {old_it->compiler_id};
                relation.target_element_ids = {new_it->compiler_id,
                                               after.elements[i].compiler_id};
                relation.evidence = {"new_dependency_from_surviving_element",
                                     "new_target_added_in_transition"};
                relation.confidence = 0.8;
                relation.relation_id = stable_hash(
                    relation.kind + "\n" + nlohmann::json(relation.source_element_ids).dump() +
                    "\n" + nlohmann::json(relation.target_element_ids).dump());
                result.relation_candidates.push_back(std::move(relation));
            }
        }
        for (const auto& old_target : before.elements) {
            const auto removed = std::none_of(result.facts.begin(), result.facts.end(),
                [&](const auto& candidate) {
                    return candidate.before_element == old_target.compiler_id &&
                           !candidate.after_element.empty();
                });
            if (removed && references(*old_it, old_target) &&
                !references(*new_it, old_target)) {
                LineageRelation relation;
                relation.kind = "inline";
                relation.source_element_ids = {old_it->compiler_id,
                                               old_target.compiler_id};
                relation.target_element_ids = {new_it->compiler_id};
                relation.evidence = {"removed_dependency_from_surviving_element",
                                     "dependency_target_removed_in_transition"};
                relation.confidence = 0.8;
                relation.relation_id = stable_hash(
                    relation.kind + "\n" + nlohmann::json(relation.source_element_ids).dump() +
                    "\n" + nlohmann::json(relation.target_element_ids).dump());
                result.relation_candidates.push_back(std::move(relation));
            }
        }
    }
    std::sort(result.relation_candidates.begin(), result.relation_candidates.end(),
              [](const auto& left, const auto& right) {
                  return left.relation_id < right.relation_id;
              });
    result.relation_candidates.erase(
        std::unique(result.relation_candidates.begin(),
                    result.relation_candidates.end(),
                    [](const auto& left, const auto& right) {
                        return left.relation_id == right.relation_id;
                    }),
        result.relation_candidates.end());
    std::sort(result.facts.begin(), result.facts.end(), [](const auto& a, const auto& b) {
        return std::tie(a.historical_element_id, a.continuity) < std::tie(b.historical_element_id, b.continuity);
    });
    return result;
}
}  // namespace history
