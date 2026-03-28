#pragma once
#include <cstdint>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>


// Stub implementation of nlohmann::json for compilation without full library
namespace nlohmann {
struct json {
  std::map<std::string, std::string> data;

  // Find the next unescaped quote
  static size_t findUnescapedQuote(const std::string& s, size_t start) {
    for (size_t i = start; i < s.size(); ++i) {
      if (s[i] == '"') {
        // Count preceding backslashes
        size_t backslashes = 0;
        size_t j = i;
        while (j > 0 && s[j - 1] == '\\') {
          ++backslashes;
          --j;
        }
        // If even number of backslashes, quote is not escaped
        if (backslashes % 2 == 0) {
          return i;
        }
      }
    }
    return std::string::npos;
  }

  // Unescape a JSON string value
  static std::string unescape(const std::string& s) {
    std::string result;
    result.reserve(s.size());

    auto hexValue = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
      if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
      return -1;
    };

    auto parseHex4 = [&](size_t pos, uint32_t& out) -> bool {
      if (pos + 4 > s.size())
        return false;
      uint32_t v = 0;
      for (size_t k = 0; k < 4; ++k) {
        int hv = hexValue(s[pos + k]);
        if (hv < 0)
          return false;
        v = (v << 4) | static_cast<uint32_t>(hv);
      }
      out = v;
      return true;
    };

    auto appendUtf8 = [&](uint32_t cp) {
      if (cp <= 0x7F) {
        result.push_back(static_cast<char>(cp));
      } else if (cp <= 0x7FF) {
        result.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      } else if (cp <= 0xFFFF) {
        result.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      } else if (cp <= 0x10FFFF) {
        result.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
    };

    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) {
        char next = s[i + 1];
        if (next == '"' || next == '\\' || next == '/') {
          result += next;
          ++i;
          continue;
        } else if (next == 'n') {
          result += '\n';
          ++i;
          continue;
        } else if (next == 'r') {
          result += '\r';
          ++i;
          continue;
        } else if (next == 't') {
          result += '\t';
          ++i;
          continue;
        } else if (next == 'u') {
          uint32_t cp = 0;
          if (parseHex4(i + 2, cp)) {
            // Handle UTF-16 surrogate pairs when present.
            if (cp >= 0xD800 && cp <= 0xDBFF &&
                (i + 12 <= s.size()) &&
                s[i + 6] == '\\' && s[i + 7] == 'u') {
              uint32_t low = 0;
              if (parseHex4(i + 8, low) && low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                i += 11; // consumed: \uXXXX\uXXXX
                appendUtf8(cp);
                continue;
              }
            }
            i += 5; // consumed: \uXXXX
            appendUtf8(cp);
            continue;
          }
        }
      }
      result += s[i];
    }
    return result;
  }

  // Very basic parser for flat string-string JSON
  friend std::istream &operator>>(std::istream &is, json &j) {
    std::string content((std::istreambuf_iterator<char>(is)),
                        (std::istreambuf_iterator<char>()));
    // Simple manual parse for keys/values
    // "KEY": "VALUE"
    size_t pos = 0;
    while (true) {
      size_t quote1 = findUnescapedQuote(content, pos);
      if (quote1 == std::string::npos)
        break;
      size_t quote2 = findUnescapedQuote(content, quote1 + 1);
      if (quote2 == std::string::npos)
        break;
      std::string key = content.substr(quote1 + 1, quote2 - quote1 - 1);

      size_t colon = content.find(':', quote2);
      if (colon == std::string::npos)
        break;

      size_t quote3 = findUnescapedQuote(content, colon);
      if (quote3 == std::string::npos)
        break;
      size_t quote4 = findUnescapedQuote(content, quote3 + 1);
      if (quote4 == std::string::npos)
        break;
      std::string value = content.substr(quote3 + 1, quote4 - quote3 - 1);

      // Unescape the value
      j.data[key] = unescape(value);
      pos = quote4 + 1;
    }
    return is;
  }

  // Iterator support
  auto items() {
    return data; // Return copy of map for range-based loop (simplified)
  }
};
} // namespace nlohmann
