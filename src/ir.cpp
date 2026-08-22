#include "history/ir.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace history {
namespace {
template <typename T>
void optional(const nlohmann::json& value, const char* key, T& output) {
    if (const auto found = value.find(key); found != value.end() && !found->is_null()) {
        found->get_to(output);
    }
}
}  // namespace

void to_json(nlohmann::json& value, const SourceAnchor& anchor) {
    value = {{"path", anchor.path}, {"begin_line", anchor.begin_line},
             {"begin_column", anchor.begin_column}, {"end_line", anchor.end_line},
             {"end_column", anchor.end_column}, {"role", anchor.role}};
}
void from_json(const nlohmann::json& value, SourceAnchor& anchor) {
    optional(value, "path", anchor.path); optional(value, "begin_line", anchor.begin_line);
    optional(value, "begin_column", anchor.begin_column); optional(value, "end_line", anchor.end_line);
    optional(value, "end_column", anchor.end_column); optional(value, "role", anchor.role);
}
void to_json(nlohmann::json& value, const Coverage& coverage) {
    value = {{"status", coverage.status}, {"capabilities", coverage.capabilities}, {"gaps", coverage.gaps}};
}
void from_json(const nlohmann::json& value, Coverage& coverage) {
    optional(value, "status", coverage.status); optional(value, "capabilities", coverage.capabilities);
    optional(value, "gaps", coverage.gaps);
}
void to_json(nlohmann::json& value, const ElementSnapshot& element) {
    value = {{"compiler_id", element.compiler_id}, {"kind", element.kind},
             {"qualified_name", element.qualified_name}, {"interface_fingerprint", element.interface_fingerprint},
             {"implementation_fingerprint", element.implementation_fingerprint},
             {"dependency_fingerprint", element.dependency_fingerprint}, {"location", element.location},
             {"referenced_compiler_ids", element.referenced_compiler_ids}};
}
void from_json(const nlohmann::json& value, ElementSnapshot& element) {
    optional(value, "compiler_id", element.compiler_id); optional(value, "kind", element.kind);
    optional(value, "qualified_name", element.qualified_name);
    optional(value, "interface_fingerprint", element.interface_fingerprint);
    optional(value, "implementation_fingerprint", element.implementation_fingerprint);
    optional(value, "dependency_fingerprint", element.dependency_fingerprint);
    optional(value, "location", element.location); optional(value, "referenced_compiler_ids", element.referenced_compiler_ids);
}
void to_json(nlohmann::json& value, const ProducerIdentity& producer) {
    value = {{"tool_version", producer.tool_version}, {"llvm_version", producer.llvm_version},
             {"clang_version", producer.clang_version},
             {"build_mode", producer.build_mode},
             {"host_architecture", producer.host_architecture}};
}
void from_json(const nlohmann::json& value, ProducerIdentity& producer) {
    optional(value, "tool_version", producer.tool_version);
    optional(value, "llvm_version", producer.llvm_version);
    optional(value, "clang_version", producer.clang_version);
    optional(value, "build_mode", producer.build_mode);
    optional(value, "host_architecture", producer.host_architecture);
}
void to_json(nlohmann::json& value, const EvidenceBundle& bundle) {
    value = {{"schema_version", bundle.schema_version}, {"source_revision", bundle.source_revision},
             {"configuration", bundle.configuration}, {"context_fingerprint", bundle.context_fingerprint},
             {"extractor_fingerprint", bundle.extractor_fingerprint}, {"producer", bundle.producer},
             {"coverage", bundle.coverage},
             {"elements", bundle.elements}};
}
void from_json(const nlohmann::json& value, EvidenceBundle& bundle) {
    optional(value, "schema_version", bundle.schema_version);
    if (bundle.schema_version != kSchemaVersion) throw std::runtime_error("unsupported schema");
    optional(value, "source_revision", bundle.source_revision); optional(value, "configuration", bundle.configuration);
    optional(value, "context_fingerprint", bundle.context_fingerprint);
    optional(value, "extractor_fingerprint", bundle.extractor_fingerprint);
    optional(value, "producer", bundle.producer);
    optional(value, "coverage", bundle.coverage); optional(value, "elements", bundle.elements);
}
std::string stable_hash(std::string_view input) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : input) { hash ^= byte; hash *= 1099511628211ULL; }
    std::ostringstream output; output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}
std::string canonical_json(const nlohmann::json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict) + '\n';
}
}  // namespace history
