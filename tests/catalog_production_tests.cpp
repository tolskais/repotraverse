#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "history/catalog.hpp"

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
}

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("repotraverse-production-catalog-" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{root};
  try {
    history::Catalog catalog(root);
    const nlohmann::json request = {
        {"schema_version", history::kSchemaVersion},
        {"query", "analysis.coverage"},
        {"params", {{"bundle", "fixture.json"}}}};
    const auto first = catalog.create_request(request);
    require(first == catalog.create_request(request),
            "request creation is not idempotent");
    catalog.update_request(first, "partial", {{"pending_work", 2}});
    const auto job = catalog.request_job(first);
    require(job && job->at("state") == "partial",
            "durable request state was not stored");

    catalog.schedule_task("0123456789abcdef0123456789abcdef",
                          {{"request_id", first}, {"identity", {}}});
    const auto retried = catalog.fail_task(
        "0123456789abcdef0123456789abcdef", "diagnostic", 1);
    require(retried.at("state") == "quarantined",
            "permanent task was not quarantined");
    require(!catalog.next_pending_task(),
            "quarantined task remained executable");

    history::LineageRelation relation;
    relation.relation_id = "relation-1";
    relation.repository_id = "main";
    relation.kind = "extract";
    relation.source_element_ids = {"source"};
    relation.target_element_ids = {"target"};
    relation.review_state = "accepted";
    relation.reviewer = "reviewer";
    catalog.store_lineage_relation(relation);
    require(catalog.lineage_relation(relation.relation_id)->reviewer ==
                "reviewer",
            "lineage review was not persisted");

    catalog.store_submodule_revision(
        {"main", "parent", "third_party/lib", "library", "child"});
    require(catalog.submodule_revisions("main", "parent").size() == 1,
            "submodule revision was not persisted");

    history::TuManifest manifest;
    manifest.repository_id = "main";
    manifest.source_revision = "revision";
    manifest.translation_unit = "source.cpp";
    manifest.source_blob = "blob";
    manifest.context_id = "context";
    manifest.configuration = "default";
    manifest.extractor_fingerprint = "extractor";
    history::LogicalElement source, target;
    source.repository_id = target.repository_id = "main";
    source.compiler_id = "source";
    target.compiler_id = "target";
    source.kind = target.kind = "function";
    source.linkage = target.linkage = "external";
    source.owner_file = target.owner_file = "source.cpp";
    source.element_id = history::stable_hash("main\nexternal\n\nsource");
    target.element_id = history::stable_hash("main\nexternal\n\ntarget");
    history::SemanticVariant source_variant, target_variant;
    source_variant.element_id = source.element_id;
    target_variant.element_id = target.element_id;
    source_variant.referenced_element_ids = {target.element_id};
    for (auto *variant : {&source_variant, &target_variant})
      variant->variant_id = history::stable_hash(
          variant->element_id + "\n\n\n");
    history::SourceAnchor source_location, target_location;
    source_location.path = target_location.path = "source.cpp";
    manifest.elements = {source, target};
    manifest.variants = {source_variant, target_variant};
    manifest.observations = {
        {source.element_id, source_variant.variant_id, source_location},
        {target.element_id, target_variant.variant_id, target_location}};
    const nlohmann::json identity = {
        {"repository", manifest.repository_id},
        {"revision", manifest.source_revision},
        {"configuration", manifest.configuration},
        {"tu", manifest.translation_unit},
        {"blob", manifest.source_blob},
        {"context", manifest.context_id},
        {"observations", manifest.observations},
        {"macro_expansions", manifest.macro_expansions}};
    manifest.manifest_id = history::stable_hash(identity.dump());
    catalog.store_fact("semantic-fact", "semantic-task",
                       {{"result", nlohmann::json(manifest)}}, "commit");
    const auto dependents = catalog.semantic_dependents(
        "main", "revision", {target.element_id});
    require(dependents.at("dependents").size() == 1 &&
                dependents.at("dependents").front().at("element_id") ==
                    source.element_id,
            "reverse semantic dependency was not indexed");
    std::cout << "production catalog tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
