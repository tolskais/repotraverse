#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace history {

inline constexpr std::uint32_t kSchemaVersion = 1;

struct SourceAnchor {
  std::string path;
  std::uint32_t begin_line{};
  std::uint32_t begin_column{};
  std::uint32_t end_line{};
  std::uint32_t end_column{};
  std::string role{"spelling"};
};

struct Coverage {
  std::string status{"complete"};
  std::vector<std::string> capabilities;
  std::vector<std::string> gaps;
};

struct ElementSnapshot {
  std::string compiler_id;
  std::string parent_element_id;
  std::string kind;
  std::string qualified_name;
  std::string linkage{"external"};
  std::string interface_fingerprint;
  std::string implementation_fingerprint;
  std::string dependency_fingerprint;
  SourceAnchor location;
  std::vector<std::string> referenced_compiler_ids;
};

struct ProducerIdentity {
  std::string tool_version;
  std::string llvm_version;
  std::string clang_version;
  std::string build_mode;
  std::string host_architecture;
};

struct EvidenceBundle {
  std::uint32_t schema_version{kSchemaVersion};
  std::string source_revision;
  std::string configuration;
  std::string context_fingerprint;
  std::string extractor_fingerprint;
  ProducerIdentity producer;
  Coverage coverage;
  std::vector<ElementSnapshot> elements;
};

struct BuildTarget {
  std::string target_id, architecture, cpu, target_triple, endianness,
      float_abi, abi;
};

struct BuildVariant {
  std::string variant_id, product, target, configuration;
};

struct CompileContext {
  std::string context_id, configuration, source_revision, translation_unit,
      working_directory;
  BuildTarget target;
  BuildVariant build_variant;
  std::vector<std::string> frontend_arguments;
  std::vector<std::string> project_files;
  std::string toolchain, adapter_version;
  Coverage coverage;
};

struct LogicalElement {
  std::string element_id, repository_id, compiler_id, kind, qualified_name,
      linkage, owner_file, parent_element_id;
};

struct SemanticVariant {
  std::string variant_id, element_id, interface_fingerprint,
      implementation_fingerprint, dependency_fingerprint;
  std::vector<std::string> referenced_element_ids;
};

struct ElementObservation {
  std::string element_id, variant_id;
  SourceAnchor location;
};

struct MacroExpansion {
  std::string macro_element_id;
  std::string containing_element_id;
  SourceAnchor location;
};

struct TuManifest {
  std::uint32_t schema_version{kSchemaVersion};
  std::string manifest_id, repository_id, source_revision, translation_unit,
      source_blob, context_id, configuration, extractor_fingerprint;
  ProducerIdentity producer;
  BuildVariant build_variant;
  Coverage coverage;
  std::vector<LogicalElement> elements;
  std::vector<SemanticVariant> variants;
  std::vector<ElementObservation> observations;
  std::vector<MacroExpansion> macro_expansions;
};

struct LineageRelation {
  std::string relation_id;
  std::string repository_id;
  std::string kind;
  std::vector<std::string> source_element_ids;
  std::vector<std::string> target_element_ids;
  std::vector<std::string> evidence;
  double confidence{};
  std::string review_state{"candidate"};
  std::string author;
  std::string reviewer;
};

struct SubmoduleRevision {
  std::string parent_repository_id;
  std::string parent_revision;
  std::string path;
  std::string child_repository_id;
  std::string child_revision;
};

void to_json(nlohmann::json &, const SourceAnchor &);
void from_json(const nlohmann::json &, SourceAnchor &);
void to_json(nlohmann::json &, const Coverage &);
void from_json(const nlohmann::json &, Coverage &);
void to_json(nlohmann::json &, const ElementSnapshot &);
void from_json(const nlohmann::json &, ElementSnapshot &);
void to_json(nlohmann::json &, const ProducerIdentity &);
void from_json(const nlohmann::json &, ProducerIdentity &);
void to_json(nlohmann::json &, const EvidenceBundle &);
void from_json(const nlohmann::json &, EvidenceBundle &);
void to_json(nlohmann::json &, const BuildTarget &);
void from_json(const nlohmann::json &, BuildTarget &);
void to_json(nlohmann::json &, const BuildVariant &);
void from_json(const nlohmann::json &, BuildVariant &);
void to_json(nlohmann::json &, const CompileContext &);
void from_json(const nlohmann::json &, CompileContext &);
void to_json(nlohmann::json &, const LogicalElement &);
void from_json(const nlohmann::json &, LogicalElement &);
void to_json(nlohmann::json &, const SemanticVariant &);
void from_json(const nlohmann::json &, SemanticVariant &);
void to_json(nlohmann::json &, const ElementObservation &);
void from_json(const nlohmann::json &, ElementObservation &);
void to_json(nlohmann::json &, const MacroExpansion &);
void from_json(const nlohmann::json &, MacroExpansion &);
void to_json(nlohmann::json &, const TuManifest &);
void from_json(const nlohmann::json &, TuManifest &);
void to_json(nlohmann::json &, const LineageRelation &);
void from_json(const nlohmann::json &, LineageRelation &);
void to_json(nlohmann::json &, const SubmoduleRevision &);
void from_json(const nlohmann::json &, SubmoduleRevision &);

std::string stable_hash(std::string_view input);
std::string canonical_json(const nlohmann::json &value);
bool validate_tu_manifest(const TuManifest &manifest, std::string &error);

} // namespace history
