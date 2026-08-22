#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "history/build_info.hpp"
#include "history/query.hpp"

namespace {

nlohmann::json read_json(std::istream& input) {
    nlohmann::json value;
    input >> value;
    return value;
}

void usage() {
    std::cerr << "usage:\n"
              << "  repotraverse --version\n"
              << "  repotraverse query [--request FILE]\n"
              << "  repotraverse status\n"
              << "  repotraverse benchmark [--elements N]\n"
              << "  repotraverse facts canonicalize [FILE]\n";
}

history::EvidenceBundle synthetic_bundle(std::size_t count, bool changed) {
    history::EvidenceBundle bundle;
    bundle.source_revision = changed ? "synthetic-after" : "synthetic-before";
    bundle.configuration = "benchmark";
    bundle.context_fingerprint = "fixed-seed-1";
    bundle.extractor_fingerprint = "synthetic-v1";
    bundle.coverage.capabilities = {"structural", "source_locations"};
    bundle.elements.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        history::ElementSnapshot element;
        element.compiler_id = "element-" + std::to_string(index);
        element.kind = "function";
        element.qualified_name = "synthetic::function" + std::to_string(index);
        element.interface_fingerprint = history::stable_hash("int(int)");
        element.implementation_fingerprint = history::stable_hash(
            changed && index % 100 == 0 ? "changed" : "unchanged");
        element.dependency_fingerprint = history::stable_hash("dependencies");
        element.location.path = "generated/file" + std::to_string(index / 100) + ".cpp";
        element.location.begin_line = static_cast<std::uint32_t>((index % 100) * 10 + 1);
        element.location.end_line = element.location.begin_line + 5;
        bundle.elements.push_back(std::move(element));
    }
    return bundle;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string command = argv[1];
    try {
        if (command == "--version") {
            std::cout << "repotraverse " << history::build::kToolVersion
                      << " (build mode: " << history::build::kBuildMode
                      << "; host: " << history::build::kHostArchitecture << ")\n";
            return 0;
        }
        if (command == "status") {
            std::cout << history::canonical_json({
                {"schema_version", history::kSchemaVersion},
                {"tool_version", history::build::kToolVersion},
                {"build_mode", history::build::kBuildMode},
                {"host_architecture", history::build::kHostArchitecture},
                {"core", "available"},
                {"query_transport", "one_shot_json"},
                {"fact_store", "memory"},
                {"lineage_model", "functions_methods_v1"},
                {"persistent_raw_source", false}});
            return 0;
        }
        if (command == "benchmark") {
            std::size_t elements = 50'000;
            if (argc == 4 && std::string(argv[2]) == "--elements") {
                elements = std::stoull(argv[3]);
            } else if (argc != 2) {
                usage();
                return 2;
            }
            const auto before = synthetic_bundle(elements, false);
            const auto after = synthetic_bundle(elements, true);
            const auto start = std::chrono::steady_clock::now();
            const auto result = history::trace_transition(before, after);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            std::cout << history::canonical_json({
                {"schema_version", history::kSchemaVersion},
                {"elements", elements}, {"transition_facts", result.facts.size()},
                {"elapsed_ms", elapsed.count()}, {"seed", 1}});
            return 0;
        }
        if (command == "facts" && argc >= 3 && std::string(argv[2]) == "canonicalize") {
            if (argc >= 4) {
                std::ifstream input(argv[3]);
                if (!input) throw std::runtime_error("cannot open input file");
                std::cout << history::canonical_json(read_json(input));
            } else {
                std::cout << history::canonical_json(read_json(std::cin));
            }
            return 0;
        }
        if (command == "query") {
            nlohmann::json request;
            if (argc == 4 && std::string(argv[2]) == "--request") {
                std::ifstream input(argv[3]);
                if (!input) throw std::runtime_error("cannot open request file");
                request = read_json(input);
            } else if (argc == 2) {
                request = read_json(std::cin);
            } else {
                usage();
                return 2;
            }
            history::QueryService service(std::make_shared<history::MemoryFactStore>());
            const auto response = service.execute(request);
            std::cout << history::canonical_json(response);
            return response.value("ok", false) ? 0 : 1;
        }
        usage();
        return 2;
    } catch (const std::exception& error) {
        std::cout << history::canonical_json({
            {"schema_version", history::kSchemaVersion}, {"ok", false},
            {"error", {{"code", "fatal"}, {"message", error.what()}}}});
        return 1;
    }
}
