#include "history/process.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>

int main() {
  const auto *compiler = std::getenv("CC");
  if (!compiler || !*compiler) {
    std::cerr << "CC is not set\n";
    return 2;
  }
  std::ofstream response("flags.rsp");
  response << "-I include -DREPOTRAVERSE_EXPERIMENT=1\n";
  response.close();
  const auto result = history::run_process(
      {compiler, "@flags.rsp", "--depend=obj/sample.d", "-c",
       "src/sample.cpp", "-o", "obj/sample.o"});
  std::cout << result.output;
  std::cerr << result.error;
  return result.exit_code;
}
