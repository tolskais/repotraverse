#pragma once

#include <optional>

#include "history/ir.hpp"

namespace history {

struct LineageCandidate {
    std::string candidate_id;
    std::string before_element;
    std::string after_element;
    std::string proposed_continuity;
    std::string confidence;
    std::vector<std::string> match_basis;
    bool automatically_resolved{};
};

struct LineageAssertion {
    std::string assertion_id;
    std::string before_element;
    std::string after_element;
    std::string relation;
    std::string status;
    std::string created_by;
    std::string reviewed_by;
};

struct TransitionFact {
    std::string historical_element_id;
    std::string before_element;
    std::string after_element;
    std::string before_revision;
    std::string after_revision;
    std::string continuity;
    std::string content_change;
    bool dependencies_changed{};
    std::optional<SourceAnchor> before_location;
    std::optional<SourceAnchor> after_location;
    std::string resolution{"automatic"};
    std::string confidence{"exact"};
    std::string transition_id;
    std::vector<OriginEvidence> origin_evidence;
};

struct TransitionResult {
    std::string before_revision;
    std::string after_revision;
    std::string configuration;
    Coverage coverage;
    std::vector<LineageCandidate> candidates;
    std::vector<LineageRelation> relation_candidates;
    std::vector<LineageRelation> reviewed_relations;
    std::vector<TransitionFact> facts;
};

void to_json(nlohmann::json&, const LineageCandidate&);
void from_json(const nlohmann::json&, LineageCandidate&);
void to_json(nlohmann::json&, const LineageAssertion&);
void from_json(const nlohmann::json&, LineageAssertion&);
void to_json(nlohmann::json&, const TransitionFact&);
void to_json(nlohmann::json&, const TransitionResult&);

TransitionResult trace_transition(const EvidenceBundle& before, const EvidenceBundle& after,
                                  const std::vector<LineageAssertion>& assertions = {},
                                  const std::string &integration_unit_id = {});

}  // namespace history
