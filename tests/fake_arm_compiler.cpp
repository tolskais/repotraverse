#include <filesystem>
#include <fstream>
#include <string>

namespace {
void touch(const std::filesystem::path &path, std::string_view contents = {}) {
  if (path.empty())
    return;
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << contents;
}
} // namespace

int main(int argc, char **argv) {
  std::filesystem::path output, dependency;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if ((argument == "-o" || argument == "--output") && index + 1 < argc)
      output = argv[++index];
    else if (argument.starts_with("--output="))
      output = argument.substr(9);
    else if (argument.starts_with("--depend="))
      dependency = argument.substr(9);
    else if (argument.starts_with("-Wp,-MD,"))
      dependency = argument.substr(8);
  }
  touch(output);
  touch(dependency,
        output.generic_string() + ": src/sample.cpp include/config.hpp\n");
  return 0;
}
