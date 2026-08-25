#include "history/encoding.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace history {
namespace {

std::string bytes(const std::u8string &value) {
  return {value.begin(), value.end()};
}

std::u8string u8bytes(std::string_view value) {
  return {value.begin(), value.end()};
}

bool continuation(unsigned char value) { return (value & 0xc0U) == 0x80U; }

std::size_t utf8_sequence(std::string_view value, std::size_t offset) {
  const auto first = static_cast<unsigned char>(value[offset]);
  if (first < 0x80U)
    return 1;
  std::size_t length = 0;
  std::uint32_t codepoint = 0;
  if (first >= 0xc2U && first <= 0xdfU) {
    length = 2;
    codepoint = first & 0x1fU;
  } else if (first >= 0xe0U && first <= 0xefU) {
    length = 3;
    codepoint = first & 0x0fU;
  } else if (first >= 0xf0U && first <= 0xf4U) {
    length = 4;
    codepoint = first & 0x07U;
  } else {
    return 0;
  }
  if (offset + length > value.size())
    return 0;
  for (std::size_t index = 1; index < length; ++index) {
    const auto next = static_cast<unsigned char>(value[offset + index]);
    if (!continuation(next))
      return 0;
    codepoint = (codepoint << 6U) | (next & 0x3fU);
  }
  if ((length == 3 && codepoint < 0x800U) ||
      (length == 4 && codepoint < 0x10000U) ||
      (codepoint >= 0xd800U && codepoint <= 0xdfffU) || codepoint > 0x10ffffU)
    return 0;
  return length;
}

void append_utf8(std::string &output, std::uint32_t value) {
  if (value <= 0x7fU) {
    output.push_back(static_cast<char>(value));
  } else if (value <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (value >> 6U)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
  } else if (value <= 0xffffU) {
    output.push_back(static_cast<char>(0xe0U | (value >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
  } else {
    output.push_back(static_cast<char>(0xf0U | (value >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
  }
}

std::string decode_utf16(std::string_view input, bool little_endian) {
  if (input.size() % 2 != 0)
    throw std::runtime_error("UTF-16 input has an incomplete code unit");
  const auto unit = [&](std::size_t offset) {
    const auto first = static_cast<unsigned char>(input[offset]);
    const auto second = static_cast<unsigned char>(input[offset + 1]);
    return static_cast<std::uint16_t>(little_endian ? first | (second << 8U)
                                                    : (first << 8U) | second);
  };
  std::string output;
  for (std::size_t offset = 0; offset < input.size(); offset += 2) {
    std::uint32_t value = unit(offset);
    if (value >= 0xd800U && value <= 0xdbffU) {
      if (offset + 3 >= input.size())
        throw std::runtime_error("UTF-16 input has an incomplete surrogate");
      const auto low = unit(offset + 2);
      if (low < 0xdc00U || low > 0xdfffU)
        throw std::runtime_error("UTF-16 input has an invalid surrogate");
      value = 0x10000U + ((value - 0xd800U) << 10U) + (low - 0xdc00U);
      offset += 2;
    } else if (value >= 0xdc00U && value <= 0xdfffU) {
      throw std::runtime_error("UTF-16 input has an invalid surrogate");
    }
    append_utf8(output, value);
  }
  return output;
}

} // namespace

std::filesystem::path path_from_utf8(std::string_view value) {
  return std::filesystem::path(u8bytes(value));
}

std::string path_to_utf8(const std::filesystem::path &path) {
  return bytes(path.u8string());
}

std::string generic_path_to_utf8(const std::filesystem::path &path) {
  return bytes(path.generic_u8string());
}

bool valid_utf8(std::string_view value) {
  for (std::size_t offset = 0; offset < value.size();) {
    const auto length = utf8_sequence(value, offset);
    if (length == 0)
      return false;
    offset += length;
  }
  return true;
}

void require_utf8(std::string_view value, std::string_view role) {
  if (!valid_utf8(value))
    throw std::runtime_error(std::string(role) + " is not valid UTF-8");
}

std::string utf8_lossy(std::string_view value) {
  std::string output;
  for (std::size_t offset = 0; offset < value.size();) {
    const auto length = utf8_sequence(value, offset);
    if (length == 0) {
      output += "\xef\xbf\xbd";
      ++offset;
    } else {
      output.append(value.substr(offset, length));
      offset += length;
    }
  }
  return output;
}

DecodedText decode_text(std::string_view input, bool allow_windows_legacy) {
  if (input.starts_with("\xef\xbb\xbf")) {
    input.remove_prefix(3);
    if (!valid_utf8(input))
      throw std::runtime_error("UTF-8 input is malformed");
    return {std::string(input), "utf-8-bom"};
  }
  if (input.starts_with("\xff\xfe"))
    return {decode_utf16(input.substr(2), true), "utf-16le"};
  if (input.starts_with("\xfe\xff"))
    return {decode_utf16(input.substr(2), false), "utf-16be"};
  if (valid_utf8(input))
    return {std::string(input), "utf-8"};
#ifdef _WIN32
  if (allow_windows_legacy) {
    if (input.empty())
      return {{}, "windows-acp:" + std::to_string(GetACP())};
    const auto count =
        MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0)
      throw std::runtime_error("legacy Windows text is malformed");
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, input.data(),
                        static_cast<int>(input.size()), wide.data(), count);
    return {wide_to_utf8(wide), "windows-acp:" + std::to_string(GetACP())};
  }
#else
  (void)allow_windows_legacy;
#endif
  throw std::runtime_error(
      "text input is not valid UTF-8 or BOM-marked UTF-16");
}

DecodedText read_text_file(const std::filesystem::path &path,
                           bool allow_windows_legacy) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot open text file: " + path_to_utf8(path));
  const std::string content{std::istreambuf_iterator<char>(input), {}};
  return decode_text(content, allow_windows_legacy);
}

#ifdef _WIN32
std::wstring utf8_to_wide(std::string_view value) {
  if (value.empty())
    return {};
  const auto count =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (count <= 0)
    throw std::runtime_error("input is not valid UTF-8");
  std::wstring output(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), output.data(), count);
  return output;
}

std::string wide_to_utf8(std::wstring_view value) {
  if (value.empty())
    return {};
  const auto count = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (count <= 0)
    throw std::runtime_error("UTF-16 input is malformed");
  std::string output(static_cast<std::size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), output.data(), count,
                      nullptr, nullptr);
  return output;
}

std::optional<std::string> environment_utf8(std::string_view name) {
  const auto wide_name = utf8_to_wide(name);
  SetLastError(ERROR_SUCCESS);
  const auto required = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
  if (required == 0) {
    if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
      return std::nullopt;
    return std::string{};
  }
  std::wstring value(required, L'\0');
  const auto written =
      GetEnvironmentVariableW(wide_name.c_str(), value.data(), required);
  if (written == 0 || written >= required)
    throw std::runtime_error("cannot read process environment");
  value.resize(written);
  return wide_to_utf8(value);
}
#else
std::optional<std::string> environment_utf8(std::string_view name) {
  const std::string owned_name{name};
  if (const auto *value = std::getenv(owned_name.c_str()))
    return std::string{value};
  return std::nullopt;
}
#endif

} // namespace history
