#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "history/catalog.hpp"
#include "history/git_coordination.hpp"
#include "history/http.hpp"
#include "history/process.hpp"

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void git(const std::vector<std::string> &arguments) {
  auto command = std::vector<std::string>{"git"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto result = history::run_process(command);
  if (result.exit_code != 0)
    throw std::runtime_error(result.error);
}
} // namespace

int main() {
  try {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("repotraverse-http-" + std::to_string(nonce));
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    } cleanup{root};
    const auto remote = root / "remote.git";
    const auto repository = root / "repository";
    std::filesystem::create_directories(root);
    git({"init", "--bare", remote.string()});
    git({"clone", remote.string(), repository.string()});

    auto catalog = std::make_shared<history::Catalog>(root / "catalog");
    auto coordinator = std::make_shared<history::GitCoordinator>(
        *catalog, history::CoordinationOptions{repository, "origin", 30, 0});
    history::QueryService service(std::make_shared<history::MemoryFactStore>(),
                                  catalog, coordinator);
    history::HttpServerOptions options;
    options.port = static_cast<std::uint16_t>(20000 + (nonce % 10000));
    options.sync_seconds = 300;
    options.max_requests = 2;
    std::thread server([&] {
      history::run_http_server(
          options, service,
          {{"ok", true}, {"producer_id", catalog->producer_id()}});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto endpoint = "http://127.0.0.1:" + std::to_string(options.port);
    const auto status = history::http_status(endpoint);
    require(status.value("producer_id", std::string{}) ==
                catalog->producer_id(),
            "HTTP status identity mismatch");
    const auto response = history::http_query(
        endpoint, {{"schema_version", history::kSchemaVersion},
                   {"query", "lineage.review.submit"},
                   {"params",
                    {{"relation",
                      {{"repository_id", "fixture"},
                       {"kind", "extract"},
                       {"source_element_ids", {"before"}},
                       {"target_element_ids", {"after"}},
                       {"review_state", "accepted"},
                       {"reviewer", "test"}}}}}});
    require(response.value("ok", false), "HTTP query failed");
    require(response.at("result").at("result").at("kind") ==
                "extract",
            "HTTP review result was not persisted");
    server.join();
    std::cout << "HTTP roundtrip tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
