#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace history {

// Narrow strings crossing Repotraverse interfaces are UTF-8. Filesystem paths
// remain in the platform-native representation.
std::filesystem::path path_from_utf8(std::string_view value);
std::string path_to_utf8(const std::filesystem::path &path);
std::string generic_path_to_utf8(const std::filesystem::path &path);

bool valid_utf8(std::string_view value);
void require_utf8(std::string_view value, std::string_view role);
std::string utf8_lossy(std::string_view value);

struct DecodedText {
  std::string text;
  std::string encoding;
};

DecodedText decode_text(std::string_view bytes,
                        bool allow_windows_legacy = false);
DecodedText read_text_file(const std::filesystem::path &path,
                           bool allow_windows_legacy = false);

#ifdef _WIN32
std::wstring utf8_to_wide(std::string_view value);
std::string wide_to_utf8(std::wstring_view value);
std::optional<std::string> environment_utf8(std::string_view name);
#endif

} // namespace history
