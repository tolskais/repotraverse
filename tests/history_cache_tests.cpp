#include "catch_amalgamated.hpp"

#include "history/catalog.hpp"
#include "history/git_coordination.hpp"
#include "history/process.hpp"
#include "history/query.hpp"

#include <chrono>
#include <fstream>

namespace {
void git(const std::filesystem::path &repository,
         std::initializer_list<std::string> arguments) {
  std::vector<std::string> command = {"git", "-C", repository.string()};
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto result = history::run_process(command);
  if (result.exit_code != 0) throw std::runtime_error(result.error);
}
} // namespace

TEST_CASE("history pages reuse and recover immutable local plans") {
  const auto root = std::filesystem::temp_directory_path() /
      ("repotraverse-history-cache-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup { std::filesystem::path path; ~Cleanup() {
    std::error_code ignored; std::filesystem::remove_all(path, ignored);
  }} cleanup{root};
  const auto source = root / "source";
  std::filesystem::create_directories(source);
  git(source, {"init"});
  git(source, {"config", "user.name", "Test"});
  git(source, {"config", "user.email", "test@invalid"});
  {
    std::ofstream output(source / "source.cpp");
    output << "int value = 1;\n";
  }
  git(source, {"add", "source.cpp"});
  git(source, {"commit", "-m", "initial"});

  auto catalog = std::make_shared<history::Catalog>(root / "catalog");
  history::CoordinationOptions options;
  options.repository = root / "analysis";
  auto coordinator = std::make_shared<history::GitCoordinator>(*catalog, options);
  history::QueryService service(std::make_shared<history::MemoryFactStore>(),
                                catalog, coordinator);
  const nlohmann::json request = {
      {"schema_version", history::kSchemaVersion},
      {"query", "tool.repository-changes"},
      {"params", {{"repository", source.string()}, {"repository_id", "main"},
                  {"ref", "HEAD"}, {"limit", 1}}}};
  const auto first = service.execute(request);
  REQUIRE(first.value("ok", false));
  const auto directory = catalog->root() / "cache" / "history-plans-v1";
  std::vector<std::filesystem::path> plans;
  for (const auto &entry : std::filesystem::directory_iterator(directory))
    if (entry.path().extension() == ".jsonl") plans.push_back(entry.path());
  REQUIRE(plans.size() == 1);
  const auto second = service.execute(request);
  REQUIRE(second.value("ok", false));
  REQUIRE(second.at("facts").at("pinned_head") ==
          first.at("facts").at("pinned_head"));
  REQUIRE(std::distance(std::filesystem::directory_iterator(directory),
                        std::filesystem::directory_iterator{}) == 1);

  {
    std::ofstream corrupt(plans.front(), std::ios::binary | std::ios::trunc);
    corrupt << "not-json\n";
  }
  const auto recovered = service.execute(request);
  REQUIRE(recovered.value("ok", false));
  std::ifstream payload(plans.front());
  std::string line;
  REQUIRE(static_cast<bool>(std::getline(payload, line)));
  REQUIRE(nlohmann::json::parse(line).at("record_type") == "history_plan");

  {
    std::ofstream output(source / "source.cpp", std::ios::app);
    output << "int next = 2;\n";
  }
  git(source, {"add", "source.cpp"});
  git(source, {"commit", "-m", "next"});
  const auto advanced = service.execute(request);
  REQUIRE(advanced.value("ok", false));
  REQUIRE(advanced.at("facts").at("pinned_head") !=
          first.at("facts").at("pinned_head"));
}
