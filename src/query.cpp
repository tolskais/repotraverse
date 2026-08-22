#include "history/query.hpp"

#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace history {
namespace {
const nlohmann::json& parameters(const nlohmann::json& request) {
    if (!request.contains("params") || !request.at("params").is_object())
        throw std::runtime_error("request.params must be an object");
    return request.at("params");
}
std::filesystem::path path_parameter(const nlohmann::json& params, const char* name) {
    if (!params.contains(name) || !params.at(name).is_string())
        throw std::runtime_error(std::string("missing path parameter: ") + name);
    return params.at(name).get<std::string>();
}
std::vector<LineageAssertion> assertions_from(const nlohmann::json& params) {
    if (!params.contains("assertions")) return {};
    std::ifstream input(path_parameter(params, "assertions"));
    if (!input) throw std::runtime_error("cannot open lineage assertions");
    nlohmann::json value; input >> value;
    if (value.value("schema_version", 0U) != kSchemaVersion || !value.contains("assertions"))
        throw std::runtime_error("invalid lineage assertion resource");
    auto assertions = value.at("assertions").get<std::vector<LineageAssertion>>();
    std::map<std::string, std::string> accepted_successors;
    for (const auto& assertion : assertions) {
        if (assertion.status == "accepted" && assertion.reviewed_by.empty())
            throw std::runtime_error("accepted lineage assertion requires reviewed_by");
        if (assertion.status == "accepted" && assertion.relation == "same_element") {
            const auto [found, inserted] = accepted_successors.emplace(
                assertion.before_element, assertion.after_element);
            if (!inserted && found->second != assertion.after_element)
                throw std::runtime_error("conflicting accepted lineage assertions");
        }
    }
    return assertions;
}
}  // namespace

EvidenceBundle MemoryFactStore::load(const std::filesystem::path& path) const {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open evidence bundle: " + path.string());
    nlohmann::json value; input >> value; return value.get<EvidenceBundle>();
}

QueryService::QueryService(std::shared_ptr<const FactStore> store) : store_(std::move(store)) {
    if (!store_) throw std::invalid_argument("FactStore cannot be null");
}

nlohmann::json QueryService::execute(const nlohmann::json& request) const {
    try {
        if (request.value("schema_version", 0U) != kSchemaVersion)
            throw std::runtime_error("unsupported request schema");
        const auto& params = parameters(request);
        const auto query = request.value("query", std::string{});
        if (query == "lineage.transition" || query == "lineage.resolve") {
            const auto before = store_->load(path_parameter(params, "before"));
            const auto after = store_->load(path_parameter(params, "after"));
            const auto assertions = query == "lineage.resolve" ? assertions_from(params)
                                                                : std::vector<LineageAssertion>{};
            const auto result = trace_transition(before, after, assertions);
            return {{"schema_version", kSchemaVersion}, {"ok", true},
                    {"coverage", result.coverage}, {"result", result}};
        }
        if (query == "element.history_stats") {
            if (!params.contains("bundles") || !params.at("bundles").is_array() ||
                params.at("bundles").size() < 2)
                throw std::runtime_error("element.history_stats requires ordered bundles");
            std::vector<EvidenceBundle> bundles;
            for (const auto& path : params.at("bundles")) bundles.push_back(store_->load(path.get<std::string>()));
            const auto assertions = assertions_from(params);
            std::vector<TransitionResult> transitions;
            std::map<std::string, std::string> parent;
            const auto find = [&](auto&& self, const std::string& id) -> std::string {
                auto [it, inserted] = parent.emplace(id, id);
                if (it->second == id) return id;
                return it->second = self(self, it->second);
            };
            const auto unite = [&](const std::string& left, const std::string& right) {
                const auto a = find(find, left); const auto b = find(find, right);
                if (a != b) parent[std::max(a, b)] = std::min(a, b);
            };
            for (std::size_t i = 0; i + 1 < bundles.size(); ++i) {
                transitions.push_back(trace_transition(bundles[i], bundles[i + 1], assertions));
                for (const auto& fact : transitions.back().facts)
                    if (!fact.before_element.empty() && !fact.after_element.empty())
                        unite(fact.before_element, fact.after_element);
            }
            struct Stats {
                std::set<std::size_t> versions, changed, interface_changed,
                    implementation_changed, renamed, moved, ambiguous;
                std::string current_name, last_changed_revision;
            };
            std::map<std::string, Stats> stats;
            for (std::size_t version = 0; version < bundles.size(); ++version)
                for (const auto& element : bundles[version].elements) {
                    auto& item = stats[find(find, element.compiler_id)];
                    item.versions.insert(version); item.current_name = element.qualified_name;
                }
            for (std::size_t i = 0; i < transitions.size(); ++i)
                for (const auto& fact : transitions[i].facts) {
                    const auto id = !fact.before_element.empty() ? fact.before_element : fact.after_element;
                    auto& item = stats[find(find, id)];
                    if (fact.content_change == "interface" || fact.content_change == "both")
                        item.interface_changed.insert(i);
                    if (fact.content_change == "implementation" || fact.content_change == "both")
                        item.implementation_changed.insert(i);
                    if (fact.content_change != "none" && fact.content_change != "unverified") {
                        item.changed.insert(i); item.last_changed_revision = fact.after_revision;
                    }
                    if (fact.continuity == "renamed" || fact.continuity == "moved_and_renamed") item.renamed.insert(i);
                    if (fact.continuity == "moved" || fact.continuity == "moved_and_renamed") item.moved.insert(i);
                    if (fact.confidence == "ambiguous") item.ambiguous.insert(i);
                }
            nlohmann::json rows = nlohmann::json::array();
            for (const auto& [root, item] : stats) rows.push_back({
                {"historical_element_id", "history-" + stable_hash(root)},
                {"current_name", item.current_name}, {"observed_versions", item.versions.size()},
                {"observable_transitions", item.versions.size() > 0 ? item.versions.size() - 1 : 0},
                {"content_changed_transitions", item.changed.size()},
                {"interface_changed_transitions", item.interface_changed.size()},
                {"implementation_changed_transitions", item.implementation_changed.size()},
                {"renamed_transitions", item.renamed.size()}, {"moved_transitions", item.moved.size()},
                {"ambiguous_transitions", item.ambiguous.size()},
                {"last_content_change_revision", item.last_changed_revision}});
            Coverage coverage;
            coverage.capabilities = {"element_lineage", "history_statistics"};
            for (const auto& transition : transitions) {
                if (transition.coverage.status != "complete") coverage.status = "partial";
                for (const auto& gap : transition.coverage.gaps)
                    coverage.gaps.push_back(transition.after_revision + ": " + gap);
            }
            return {{"schema_version", kSchemaVersion}, {"ok", true},
                    {"coverage", coverage},
                    {"result", {{"bundle_count", bundles.size()}, {"elements", std::move(rows)}}}};
        }
        if (query == "analysis.coverage") {
            const auto bundle = store_->load(path_parameter(params, "bundle"));
            return {{"schema_version", kSchemaVersion}, {"ok", true},
                    {"coverage", bundle.coverage}, {"result", bundle.coverage}};
        }
        throw std::runtime_error("unknown query: " + query);
    } catch (const std::exception& error) {
        return {{"schema_version", kSchemaVersion}, {"ok", false},
                {"error", {{"code", "invalid_request"}, {"message", error.what()}}}};
    }
}
}  // namespace history
