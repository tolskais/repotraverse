#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <thread>

#include "catch_amalgamated.hpp"

#include "history/catalog.hpp"
#include "history/git_coordination.hpp"
#include "history/http.hpp"
#include "history/process.hpp"

namespace {
void require(bool condition, const char *message) {
  INFO(message);
  REQUIRE(condition);
}
void git(const std::vector<std::string> &arguments) {
  auto command = std::vector<std::string>{"git"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto result = history::run_process(command);
  if (result.exit_code != 0)
    throw std::runtime_error(result.error);
}
} // namespace

TEST_CASE("HTTP requests are queued and completed without blocking status") {
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
    history::CoordinationOptions coordination;
    coordination.repository = repository;
    coordination.lease_seconds = 30;
    coordination.grace_seconds = 0;
    auto coordinator =
        std::make_shared<history::GitCoordinator>(*catalog, coordination);
    history::QueryService service(std::make_shared<history::MemoryFactStore>(),
                                  catalog, coordinator);
    history::HttpServerOptions options;
    options.port = 0;
    options.sync_seconds = 300;
    std::promise<std::uint16_t> listening;
    options.on_listening = [&](std::uint16_t port) { listening.set_value(port); };
    std::exception_ptr server_error;
    std::jthread server([&, options](std::stop_token stop) mutable {
      options.stop_token = stop;
      try {
        history::run_http_server(
            options, service,
            {{"ok", true}, {"producer_id", catalog->producer_id()}});
      } catch (...) {
        server_error = std::current_exception();
        try {
          listening.set_exception(server_error);
        } catch (...) {
        }
      }
    });
    const auto endpoint =
        "http://127.0.0.1:" + std::to_string(listening.get_future().get());
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
    require(response.value("ok", false) && response.at("state") == "queued",
            "HTTP query was not queued");
    const auto request_id = response.at("request_id").get<std::string>();
    nlohmann::json completed;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
      completed = history::http_request_status(endpoint, request_id);
      if (completed.value("state", std::string{}) == "complete")
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    require(completed.value("state", std::string{}) == "complete",
            "queued HTTP query did not complete");
    require(completed.at("result").at("result").at("kind") == "extract",
            "HTTP review result was not persisted");
    server.request_stop();
    server.join();
    if (server_error)
      std::rethrow_exception(server_error);
}
