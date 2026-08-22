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
    std::string kind;
    std::string qualified_name;
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

void to_json(nlohmann::json&, const SourceAnchor&);
void from_json(const nlohmann::json&, SourceAnchor&);
void to_json(nlohmann::json&, const Coverage&);
void from_json(const nlohmann::json&, Coverage&);
void to_json(nlohmann::json&, const ElementSnapshot&);
void from_json(const nlohmann::json&, ElementSnapshot&);
void to_json(nlohmann::json&, const ProducerIdentity&);
void from_json(const nlohmann::json&, ProducerIdentity&);
void to_json(nlohmann::json&, const EvidenceBundle&);
void from_json(const nlohmann::json&, EvidenceBundle&);

std::string stable_hash(std::string_view input);
std::string canonical_json(const nlohmann::json& value);

}  // namespace history
