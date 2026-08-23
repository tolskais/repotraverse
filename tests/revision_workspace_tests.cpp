#include "history/process.hpp"
#include "history/revision_workspace.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void write(const std::filesystem::path &path, std::string_view value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << value;
}

std::string trim(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
    value.pop_back();
  return value;
}

void git(const std::vector<std::string> &arguments,
         const std::filesystem::path &directory = {}) {
  const auto result = history::run_process(arguments, directory);
  if (result.exit_code != 0)
    throw std::runtime_error("git fixture command failed: " + result.error);
}

} // namespace

int main() {
  try {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("repotraverse-workspace-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    } cleanup{root};
    const auto repository = root / "repository";
    std::filesystem::create_directories(repository);
    write(repository / "src/a.cpp", "#include \"a.hpp\"\nint a(){return A;}\n");
    write(repository / "include/a.hpp", "#define A 1\n");
    write(repository / "include/special[1].hpp", "#define SPECIAL 1\n");
    write(repository / "!forced.hpp", "#define FORCED 1\n");
    write(repository / "src/unrelated.cpp", "int unrelated(){return 2;}\n");
    git({GIT_PATH, "init", "-b", "main"}, repository);
    git({GIT_PATH, "-C", repository.string(), "add", "."});
    git({GIT_PATH, "-C", repository.string(), "-c", "user.name=Test", "-c",
         "user.email=test@example.invalid", "commit", "-m", "fixture"});
    const auto revision =
        trim(history::run_process(
                 {GIT_PATH, "-C", repository.string(), "rev-parse", "HEAD"})
                 .output);

    history::RevisionTreeIndex index(repository, revision);
    require(index.blob_at("src/a.cpp").has_value(),
            "tree index did not resolve tracked blob");
    require(!index.blob_at("missing.cpp").has_value(),
            "tree index resolved absent blob");
    require(index.total_blob_bytes() > *index.size_at("src/a.cpp"),
            "tree index did not account for the complete tree");

    history::WorkspaceLimits limits;
    limits.max_revisions = 2;
    limits.max_bytes = 1024 * 1024;
    limits.free_space_reserve_bytes = 0;
    history::RevisionWorkspacePool pool(root / "workspaces", limits);
    history::MaterializationManifest first{
        {"src/a.cpp", "include/a.hpp", "include/special[1].hpp", "!forced.hpp"},
        true,
        {}};
    auto a = pool.acquire(repository, revision, first);
    require(!a.full(), "complete closure unexpectedly used full checkout");
    require(std::filesystem::exists(a.path() / "src/a.cpp") &&
                std::filesystem::exists(a.path() / "include/a.hpp"),
            "sparse workspace omitted required files");
    require(std::filesystem::exists(a.path() / "include/special[1].hpp") &&
                std::filesystem::exists(a.path() / "!forced.hpp"),
            "sparse workspace mishandled Git pattern characters");
    require(!std::filesystem::exists(a.path() / "src/unrelated.cpp"),
            "sparse workspace materialized unrelated file");

    history::MaterializationManifest expanded{{"src/unrelated.cpp"}, true, {}};
    auto b = pool.acquire(repository, revision, expanded);
    require(a.path() == b.path(), "same revision did not reuse workspace");
    require(std::filesystem::exists(b.path() / "src/unrelated.cpp"),
            "shared sparse workspace did not expand monotonically");
    const auto shared_path = b.path();
    a = {};
    b = {};
    require(std::filesystem::exists(shared_path),
            "idle sparse workspace was not retained");

    history::MaterializationManifest incomplete{
        {"src/a.cpp"}, false, {"dependency map missing"}};
    auto full = pool.acquire(repository, revision, incomplete);
    require(full.full(), "incomplete closure did not trigger full fallback");
    require(std::filesystem::exists(full.path() / "src/unrelated.cpp"),
            "full fallback omitted repository file");
    const auto full_path = full.path();
    full = {};
    require(!std::filesystem::exists(full_path),
            "temporary full workspace was retained after release");

    history::WorkspaceLimits tiny = limits;
    tiny.max_bytes = 1;
    history::RevisionWorkspacePool constrained(root / "constrained", tiny);
    bool rejected = false;
    try {
      auto ignored = constrained.acquire(repository, revision, first);
      (void)ignored;
    } catch (const std::exception &error) {
      rejected = std::string(error.what()).find("disk_space_insufficient") !=
                 std::string::npos;
    }
    require(rejected, "workspace byte limit did not reject materialization");

    std::cout << "revision workspace tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
