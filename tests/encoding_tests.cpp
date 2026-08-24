#include "history/catalog.hpp"
#include "history/encoding.hpp"
#include "history/process.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace

int main() {
  try {
    const std::string value = "한글 경로/파일 🚀.json";
    const auto path = history::path_from_utf8(value);
    require(history::generic_path_to_utf8(path) == value,
            "UTF-8 filesystem path did not round-trip");
    require(history::valid_utf8(value), "valid UTF-8 was rejected");
    require(!history::valid_utf8("\xff"), "invalid UTF-8 was accepted");
    require(history::utf8_lossy("a\xffz") == "a\xef\xbf\xbdz",
            "lossy diagnostic conversion is incorrect");

    require(history::decode_text("\xef\xbb\xbf한글").encoding == "utf-8-bom",
            "UTF-8 BOM was not detected");
    const std::string utf16le{"\xff\xfe\x5c\xd5\x00\xae", 6};
    const std::string utf16be{"\xfe\xff\xd5\x5c\xae\x00", 6};
    require(history::decode_text(utf16le).text == "한글",
            "UTF-16LE was not decoded");
    require(history::decode_text(utf16be).text == "한글",
            "UTF-16BE was not decoded");
    bool rejected = false;
    try {
      (void)history::decode_text("\xff");
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    require(rejected, "ambiguous non-UTF-8 text was not rejected");

    const auto root = std::filesystem::temp_directory_path() /
                      history::path_from_utf8("repotraverse-한글-catalog");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    {
      history::Catalog catalog(root);
      require(std::filesystem::exists(root / "catalog.sqlite3"),
              "SQLite catalog was not created below a Unicode path");
    }

#ifdef _WIN32
    auto long_root = root;
    while (history::path_to_utf8(long_root).size() < 300)
      long_root /= history::path_from_utf8(
          "매우 긴 Windows 저장소 디렉터리 이름");
    std::filesystem::create_directories(long_root);
    const auto long_file = long_root / history::path_from_utf8("소스 파일.cpp");
    {
      std::ofstream output(long_file, std::ios::binary);
      require(static_cast<bool>(output),
              "long-path-aware manifest did not enable file creation");
      output << "int main() { return 0; }\n";
    }
    require(std::filesystem::exists(long_file),
            "file below a 260+ character path was not created");
#endif

    const auto repository = root / history::path_from_utf8("프로젝트");
    const auto capture = root / history::path_from_utf8("캡처 결과");
    std::filesystem::create_directories(repository);
    const auto response = repository / history::path_from_utf8("응답.rsp");
    {
      std::ofstream output(response, std::ios::binary);
      const std::string utf16_response{
          "\xff\xfe\x2d\x00\x44\x00\x54\x00\x45\x00\x53\x00\x54\x00", 16};
      output.write(utf16_response.data(),
                   static_cast<std::streamsize>(utf16_response.size()));
    }
    history::ProcessOptions probe;
    probe.working_directory = repository;
    probe.environment = {
        {"REPOTRAVERSE_CAPTURE_DIRECTORY", history::path_to_utf8(capture)},
        {"REPOTRAVERSE_CAPTURE_REPOSITORY", history::path_to_utf8(repository)},
        {"REPOTRAVERSE_CAPTURE_CONFIGURATION", "한글 구성"},
        {"REPOTRAVERSE_CAPTURE_REVISION", "0123456789abcdef"},
        {"REPOTRAVERSE_CAPTURE_TOOLCHAIN", "armclang6"}};
    const auto probed =
        history::run_process({PROBE_PATH, "@응답.rsp", "-c", "소스.cpp", "-o",
                              "결과.o", "-MF", "의존.d"},
                             probe);
    require(probed.exit_code == 0, "compiler probe failed below Unicode paths");
    std::filesystem::path captured_record;
    for (const auto &entry : std::filesystem::directory_iterator(capture))
      captured_record = entry.path();
    require(!captured_record.empty(),
            "compiler probe did not persist a record");
    const auto record =
        nlohmann::json::parse(history::read_text_file(captured_record).text);
    require(record.at("configuration") == "한글 구성",
            "Unicode probe environment did not round-trip");
    require(record.at("translation_unit") == "소스.cpp",
            "Unicode compiler argument did not round-trip");
    require(record.at("response_file_encodings").at("응답.rsp") == "utf-16le",
            "response-file encoding metadata was not recorded");
    std::filesystem::remove_all(root, ignored);
    std::cout << "encoding tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
