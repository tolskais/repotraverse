#pragma once

#include <cstdint>
#include <map>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace history {

class Telemetry {
public:
  static Telemetry &instance();
  ~Telemetry();
  void configure(std::string endpoint, std::string service_name);
  void increment(const std::string &name, std::int64_t amount = 1);
  void gauge(const std::string &name, std::int64_t value);
  nlohmann::json snapshot() const;
  void log(const std::string &level, const std::string &event,
           nlohmann::json fields = {});
  void span(const std::string &name, std::int64_t duration_ms,
            nlohmann::json attributes = {});

private:
  mutable std::mutex mutex_;
  std::map<std::string, std::int64_t> counters_;
  std::map<std::string, std::int64_t> gauges_;
  std::string endpoint_;
  std::string service_name_{"repotraverse"};
  std::deque<nlohmann::json> pending_logs_;
  std::deque<nlohmann::json> pending_spans_;
  std::condition_variable_any changed_;
  std::jthread exporter_;
  void export_loop(std::stop_token stop);
};

} // namespace history
