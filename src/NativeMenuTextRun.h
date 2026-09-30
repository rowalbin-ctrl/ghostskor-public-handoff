#pragma once
#include <memory>
#include <string>
#include <cstring>

// One engine element may emit several visual lines. A complete translation
// belongs to that element invocation, not to each suffix passed to AddCmd.
struct NativeMenuTextRun {
  std::shared_ptr<const std::string> source;
  bool joinedLiterals = false;
  bool claimed = false;
  explicit NativeMenuTextRun(std::shared_ptr<const std::string> text, bool joined = false)
      : source(std::move(text)), joinedLiterals(joined) {}
  // LUI passes localized literal spans as 0x1F text 0x1E. Concatenated
  // messages lose these boundaries before the engine emits wrapped lines.
  // Only decode a complete sequence of two or more literal spans. Ordinary
  // labels, unresolved keys, and mixed control/binding streams stay untouched.
  static std::string DecodeLiteralJoin(const char *encoded) {
    if (!encoded || static_cast<unsigned char>(*encoded) != 0x1F) return {};
    const char *firstClose = std::strchr(encoded + 1, '\x1e');
    if (!firstClose || static_cast<unsigned char>(firstClose[1]) != 0x1F) return {};
    std::string joined;
    unsigned spans = 0;
    const auto *p = reinterpret_cast<const unsigned char*>(encoded);
    while (*p) {
      if (*p++ != 0x1F) return {};
      while (*p && *p != 0x1E) {
        if (*p < 0x20 && *p != '\n' && *p != '\r' && *p != '\t') return {};
        joined.push_back(static_cast<char>(*p++));
      }
      if (*p++ != 0x1E) return {};
      ++spans;
    }
    return spans >= 2 ? joined : std::string{};
  }
  bool IsMultiline() const {
    return source && source->find_first_of("\r\n") != std::string::npos;
  }
  bool Claim() { if (claimed) return false; claimed = true; return true; }
};
