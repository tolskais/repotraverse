#include "history/ir.hpp"

#include <set>
#include <stdexcept>

#include <xxhash.h>

namespace history {
namespace {
template <typename T>
void optional(const nlohmann::json &value, const char *key, T &output) {
  if (const auto found = value.find(key);
      found != value.end() && !found->is_null()) {
    found->get_to(output);
  }
}
} // namespace

void to_json(nlohmann::json &value, const SourceAnchor &anchor) {
  value = {{"path", anchor.path},
           {"begin_line", anchor.begin_line},
           {"begin_column", anchor.begin_column},
           {"end_line", anchor.end_line},
           {"end_column", anchor.end_column},
           {"role", anchor.role}};
}
void from_json(const nlohmann::json &value, SourceAnchor &anchor) {
  optional(value, "path", anchor.path);
  optional(value, "begin_line", anchor.begin_line);
  optional(value, "begin_column", anchor.begin_column);
  optional(value, "end_line", anchor.end_line);
  optional(value, "end_column", anchor.end_column);
  optional(value, "role", anchor.role);
}
void to_json(nlohmann::json &value, const Coverage &coverage) {
  value = {{"status", coverage.status},
           {"capabilities", coverage.capabilities},
           {"gaps", coverage.gaps}};
}
void from_json(const nlohmann::json &value, Coverage &coverage) {
  optional(value, "status", coverage.status);
  optional(value, "capabilities", coverage.capabilities);
  optional(value, "gaps", coverage.gaps);
}
void to_json(nlohmann::json &value, const ElementSnapshot &element) {
  value = {{"compiler_id", element.compiler_id},
           {"parent_element_id", element.parent_element_id},
           {"kind", element.kind},
           {"qualified_name", element.qualified_name},
           {"linkage", element.linkage},
           {"interface_fingerprint", element.interface_fingerprint},
           {"implementation_fingerprint", element.implementation_fingerprint},
           {"dependency_fingerprint", element.dependency_fingerprint},
           {"location", element.location},
           {"referenced_compiler_ids", element.referenced_compiler_ids}};
}
void from_json(const nlohmann::json &value, ElementSnapshot &element) {
  optional(value, "compiler_id", element.compiler_id);
  optional(value, "parent_element_id", element.parent_element_id);
  optional(value, "kind", element.kind);
  optional(value, "qualified_name", element.qualified_name);
  optional(value, "linkage", element.linkage);
  optional(value, "interface_fingerprint", element.interface_fingerprint);
  optional(value, "implementation_fingerprint",
           element.implementation_fingerprint);
  optional(value, "dependency_fingerprint", element.dependency_fingerprint);
  optional(value, "location", element.location);
  optional(value, "referenced_compiler_ids", element.referenced_compiler_ids);
}
void to_json(nlohmann::json &value, const ProducerIdentity &producer) {
  value = {{"tool_version", producer.tool_version},
           {"llvm_version", producer.llvm_version},
           {"clang_version", producer.clang_version},
           {"build_mode", producer.build_mode},
           {"host_architecture", producer.host_architecture}};
}
void from_json(const nlohmann::json &value, ProducerIdentity &producer) {
  optional(value, "tool_version", producer.tool_version);
  optional(value, "llvm_version", producer.llvm_version);
  optional(value, "clang_version", producer.clang_version);
  optional(value, "build_mode", producer.build_mode);
  optional(value, "host_architecture", producer.host_architecture);
}
void to_json(nlohmann::json &value, const EvidenceBundle &bundle) {
  value = {{"schema_version", bundle.schema_version},
           {"source_revision", bundle.source_revision},
           {"configuration", bundle.configuration},
           {"context_fingerprint", bundle.context_fingerprint},
           {"extractor_fingerprint", bundle.extractor_fingerprint},
           {"producer", bundle.producer},
           {"coverage", bundle.coverage},
           {"elements", bundle.elements}};
}
void from_json(const nlohmann::json &value, EvidenceBundle &bundle) {
  optional(value, "schema_version", bundle.schema_version);
  if (bundle.schema_version != kSchemaVersion)
    throw std::runtime_error("unsupported schema");
  optional(value, "source_revision", bundle.source_revision);
  optional(value, "configuration", bundle.configuration);
  optional(value, "context_fingerprint", bundle.context_fingerprint);
  optional(value, "extractor_fingerprint", bundle.extractor_fingerprint);
  optional(value, "producer", bundle.producer);
  optional(value, "coverage", bundle.coverage);
  optional(value, "elements", bundle.elements);
}
void to_json(nlohmann::json &v, const BuildTarget &x) {
  v = {{"target_id", x.target_id},
       {"architecture", x.architecture},
       {"cpu", x.cpu},
       {"target_triple", x.target_triple},
       {"endianness", x.endianness},
       {"float_abi", x.float_abi},
       {"abi", x.abi}};
}
void from_json(const nlohmann::json &v, BuildTarget &x) {
  optional(v, "target_id", x.target_id);
  optional(v, "architecture", x.architecture);
  optional(v, "cpu", x.cpu);
  optional(v, "target_triple", x.target_triple);
  optional(v, "endianness", x.endianness);
  optional(v, "float_abi", x.float_abi);
  optional(v, "abi", x.abi);
}
void to_json(nlohmann::json &v, const BuildVariant &x) {
  v = {{"variant_id", x.variant_id},
       {"product", x.product},
       {"target", x.target},
       {"configuration", x.configuration}};
}
void from_json(const nlohmann::json &v, BuildVariant &x) {
  optional(v, "variant_id", x.variant_id);
  optional(v, "product", x.product);
  optional(v, "target", x.target);
  optional(v, "configuration", x.configuration);
}
void to_json(nlohmann::json &v, const CompileContext &x) {
  v = {{"context_id", x.context_id},
       {"configuration", x.configuration},
       {"source_revision", x.source_revision},
       {"translation_unit", x.translation_unit},
       {"working_directory", x.working_directory},
       {"target", x.target},
       {"build_variant", x.build_variant},
       {"frontend_arguments", x.frontend_arguments},
       {"project_files", x.project_files},
       {"toolchain", x.toolchain},
       {"adapter_version", x.adapter_version},
       {"coverage", x.coverage}};
}
void from_json(const nlohmann::json &v, CompileContext &x) {
  optional(v, "context_id", x.context_id);
  optional(v, "configuration", x.configuration);
  optional(v, "source_revision", x.source_revision);
  optional(v, "translation_unit", x.translation_unit);
  optional(v, "working_directory", x.working_directory);
  optional(v, "target", x.target);
  optional(v, "build_variant", x.build_variant);
  optional(v, "frontend_arguments", x.frontend_arguments);
  optional(v, "project_files", x.project_files);
  optional(v, "toolchain", x.toolchain);
  optional(v, "adapter_version", x.adapter_version);
  optional(v, "coverage", x.coverage);
}
void to_json(nlohmann::json &v, const LogicalElement &x) {
  v = {{"element_id", x.element_id},
       {"repository_id", x.repository_id},
       {"compiler_id", x.compiler_id},
       {"kind", x.kind},
       {"qualified_name", x.qualified_name},
       {"linkage", x.linkage},
       {"owner_file", x.owner_file},
       {"parent_element_id", x.parent_element_id}};
}
void from_json(const nlohmann::json &v, LogicalElement &x) {
  optional(v, "element_id", x.element_id);
  optional(v, "repository_id", x.repository_id);
  optional(v, "compiler_id", x.compiler_id);
  optional(v, "kind", x.kind);
  optional(v, "qualified_name", x.qualified_name);
  optional(v, "linkage", x.linkage);
  optional(v, "owner_file", x.owner_file);
  optional(v, "parent_element_id", x.parent_element_id);
}
void to_json(nlohmann::json &v, const SemanticVariant &x) {
  v = {{"variant_id", x.variant_id},
       {"element_id", x.element_id},
       {"interface_fingerprint", x.interface_fingerprint},
       {"implementation_fingerprint", x.implementation_fingerprint},
       {"dependency_fingerprint", x.dependency_fingerprint},
       {"referenced_element_ids", x.referenced_element_ids}};
}
void from_json(const nlohmann::json &v, SemanticVariant &x) {
  optional(v, "variant_id", x.variant_id);
  optional(v, "element_id", x.element_id);
  optional(v, "interface_fingerprint", x.interface_fingerprint);
  optional(v, "implementation_fingerprint", x.implementation_fingerprint);
  optional(v, "dependency_fingerprint", x.dependency_fingerprint);
  optional(v, "referenced_element_ids", x.referenced_element_ids);
}
void to_json(nlohmann::json &v, const ElementObservation &x) {
  v = {{"element_id", x.element_id},
       {"variant_id", x.variant_id},
       {"location", x.location}};
}
void from_json(const nlohmann::json &v, ElementObservation &x) {
  optional(v, "element_id", x.element_id);
  optional(v, "variant_id", x.variant_id);
  optional(v, "location", x.location);
}
void to_json(nlohmann::json &v, const MacroExpansion &x) {
  v = {{"macro_element_id", x.macro_element_id},
       {"containing_element_id", x.containing_element_id},
       {"location", x.location}};
}
void from_json(const nlohmann::json &v, MacroExpansion &x) {
  optional(v, "macro_element_id", x.macro_element_id);
  optional(v, "containing_element_id", x.containing_element_id);
  optional(v, "location", x.location);
}
void to_json(nlohmann::json &v, const TuManifest &x) {
  v = {{"schema_version", x.schema_version},
       {"record_type", "tu_manifest"},
       {"manifest_id", x.manifest_id},
       {"repository_id", x.repository_id},
       {"source_revision", x.source_revision},
       {"translation_unit", x.translation_unit},
       {"source_blob", x.source_blob},
       {"context_id", x.context_id},
       {"configuration", x.configuration},
       {"build_variant", x.build_variant},
       {"extractor_fingerprint", x.extractor_fingerprint},
       {"producer", x.producer},
       {"coverage", x.coverage},
       {"elements", x.elements},
       {"variants", x.variants},
       {"observations", x.observations},
       {"macro_expansions", x.macro_expansions}};
}
void from_json(const nlohmann::json &v, TuManifest &x) {
  optional(v, "schema_version", x.schema_version);
  if (x.schema_version != kSchemaVersion)
    throw std::runtime_error("unsupported TU manifest schema");
  optional(v, "manifest_id", x.manifest_id);
  optional(v, "repository_id", x.repository_id);
  optional(v, "source_revision", x.source_revision);
  optional(v, "translation_unit", x.translation_unit);
  optional(v, "source_blob", x.source_blob);
  optional(v, "context_id", x.context_id);
  optional(v, "configuration", x.configuration);
  optional(v, "build_variant", x.build_variant);
  optional(v, "extractor_fingerprint", x.extractor_fingerprint);
  optional(v, "producer", x.producer);
  optional(v, "coverage", x.coverage);
  optional(v, "elements", x.elements);
  optional(v, "variants", x.variants);
  optional(v, "observations", x.observations);
  optional(v, "macro_expansions", x.macro_expansions);
}
void to_json(nlohmann::json &v, const LineageRelation &x) {
  v = {{"relation_id", x.relation_id},
       {"repository_id", x.repository_id},
       {"kind", x.kind},
       {"source_element_ids", x.source_element_ids},
       {"target_element_ids", x.target_element_ids},
       {"evidence", x.evidence},
       {"confidence", x.confidence},
       {"review_state", x.review_state},
       {"author", x.author},
       {"reviewer", x.reviewer}};
}
void from_json(const nlohmann::json &v, LineageRelation &x) {
  optional(v, "relation_id", x.relation_id);
  optional(v, "repository_id", x.repository_id);
  optional(v, "kind", x.kind);
  optional(v, "source_element_ids", x.source_element_ids);
  optional(v, "target_element_ids", x.target_element_ids);
  optional(v, "evidence", x.evidence);
  optional(v, "confidence", x.confidence);
  optional(v, "review_state", x.review_state);
  optional(v, "author", x.author);
  optional(v, "reviewer", x.reviewer);
}
void to_json(nlohmann::json &v, const SubmoduleRevision &x) {
  v = {{"parent_repository_id", x.parent_repository_id},
       {"parent_revision", x.parent_revision},
       {"path", x.path},
       {"child_repository_id", x.child_repository_id},
       {"child_revision", x.child_revision}};
}
void from_json(const nlohmann::json &v, SubmoduleRevision &x) {
  optional(v, "parent_repository_id", x.parent_repository_id);
  optional(v, "parent_revision", x.parent_revision);
  optional(v, "path", x.path);
  optional(v, "child_repository_id", x.child_repository_id);
  optional(v, "child_revision", x.child_revision);
}
std::string stable_hash(std::string_view input) {
  const auto hash = XXH3_128bits(input.data(), input.size());
  char output[33]{};
  static constexpr char digits[] = "0123456789abcdef";
  XXH128_canonical_t canonical{};
  XXH128_canonicalFromHash(&canonical, hash);
  for (std::size_t index = 0; index < sizeof(canonical.digest); ++index) {
    output[index * 2] = digits[canonical.digest[index] >> 4U];
    output[index * 2 + 1] = digits[canonical.digest[index] & 0x0fU];
  }
  return output;
}
std::string canonical_json(const nlohmann::json &value) {
  return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict) +
         '\n';
}
bool validate_tu_manifest(const TuManifest &manifest, std::string &error) {
  if (manifest.schema_version != kSchemaVersion ||
      manifest.repository_id.empty() || manifest.source_revision.empty() ||
      manifest.translation_unit.empty() || manifest.context_id.empty() ||
      manifest.configuration.empty() ||
      manifest.extractor_fingerprint.empty()) {
    error = "manifest identity fields are incomplete";
    return false;
  }
  std::set<std::string> element_ids, variant_ids;
  for (const auto &element : manifest.elements) {
    const auto domain = element.linkage == "internal"
                            ? manifest.translation_unit
                            : std::string{};
    const auto expected =
        stable_hash(manifest.repository_id + "\n" + element.linkage + "\n" +
                    domain + "\n" + element.compiler_id);
    if (element.repository_id != manifest.repository_id ||
        element.element_id != expected || element.kind.empty() ||
        !element_ids.insert(element.element_id).second) {
      error = "manifest contains an invalid logical element";
      return false;
    }
  }
  for (const auto &element : manifest.elements)
    if (!element.parent_element_id.empty() &&
        !element_ids.contains(element.parent_element_id)) {
      error = "manifest element parent is missing";
      return false;
    }
  for (const auto &variant : manifest.variants) {
    const auto expected =
        stable_hash(variant.element_id + "\n" + variant.interface_fingerprint +
                    "\n" + variant.implementation_fingerprint + "\n" +
                    variant.dependency_fingerprint);
    if (!element_ids.contains(variant.element_id) ||
        variant.variant_id != expected ||
        !variant_ids.insert(variant.variant_id).second) {
      error = "manifest contains an invalid semantic variant";
      return false;
    }
  }
  for (const auto &observation : manifest.observations)
    if (!element_ids.contains(observation.element_id) ||
        !variant_ids.contains(observation.variant_id) ||
        observation.location.path.empty()) {
      error = "manifest contains an invalid observation";
      return false;
    }
  for (const auto &expansion : manifest.macro_expansions)
    if (!element_ids.contains(expansion.macro_element_id) ||
        (!expansion.containing_element_id.empty() &&
         !element_ids.contains(expansion.containing_element_id)) ||
        expansion.location.path.empty()) {
      error = "manifest contains an invalid macro expansion";
      return false;
    }
  const nlohmann::json identity = {
      {"repository", manifest.repository_id},
      {"revision", manifest.source_revision},
      {"configuration", manifest.configuration},
      {"build_variant", manifest.build_variant},
      {"tu", manifest.translation_unit},
      {"blob", manifest.source_blob},
      {"context", manifest.context_id},
      {"observations", manifest.observations},
      {"macro_expansions", manifest.macro_expansions}};
  if (manifest.manifest_id != stable_hash(identity.dump())) {
    error = "manifest identifier does not match its contents";
    return false;
  }
  return true;
}
} // namespace history
