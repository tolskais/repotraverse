#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "history/process.hpp"

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
}

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--sleep") {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "--output") {
    std::cout << std::string(1024 * 1024, 'x');
    return 0;
  }
  try {
    history::ProcessOptions timeout;
    timeout.timeout = std::chrono::milliseconds(100);
    const auto timed = history::run_process({argv[0], "--sleep"}, timeout);
    require(timed.timed_out, "process deadline was not enforced");

    history::ProcessOptions bounded;
    bounded.timeout = std::chrono::seconds(5);
    bounded.max_output_bytes = 4096;
    const auto oversized = history::run_process({argv[0], "--output"}, bounded);
    require(oversized.output_truncated, "process output limit was not enforced");
    require(oversized.output.size() <= bounded.max_output_bytes,
            "bounded process output exceeded its limit");
    std::cout << "process tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
