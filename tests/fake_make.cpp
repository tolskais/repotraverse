#include "history/encoding.hpp"
#include "history/process.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
  std::string configured;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument.starts_with("CC="))
      configured = argument.substr(3);
  }
  const auto compiler_environment = history::environment_utf8("CC");
  const auto compiler =
      configured.empty()
          ? compiler_environment.value_or(std::string{})
          : configured;
  if (compiler.empty()) {
    std::cerr << "CC is not set\n";
    return 2;
  }
  std::ofstream response("flags.rsp");
  response << "-I include -DREPOTRAVERSE_EXPERIMENT=1\n";
  response.close();
  const auto capability = history::run_process({compiler, "-E", "-dM", "-"});
  if (capability.exit_code != 0)
    return capability.exit_code;
  const auto result =
      history::run_process({compiler, "@flags.rsp", "-Wp,-MD,obj/sample.d",
                            "-c", "src/sample.cpp", "-o", "obj/sample.o"});
  std::cout << result.output;
  std::cerr << result.error;
  return result.exit_code;
}
