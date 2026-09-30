#pragma once
#include <string_view>

namespace NativeBindingPrompt {
// No mission/key whitelist: only bindings, color codes and whitespace.
// Leave their key names, pad materials and animation to the engine.
inline bool IsBindingOnly(std::string_view text) {
  bool binding = false;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = (unsigned char)text[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
    if (c == '^' && i + 1 < text.size() && text[i+1] >= '0' && text[i+1] <= '9') {
      i += 2; continue;
    }
    if (text.substr(i, 3) != "[{+") return false;
    const size_t end = text.find("}]", i + 3);
    if (end == std::string_view::npos || end == i + 3) return false;
    // Reject malformed/nested tokens; permit alternate command lists.
    for (size_t j = i + 3; j < end; ++j) {
      const unsigned char t = (unsigned char)text[j];
      if (!((t >= 'a' && t <= 'z') || (t >= 'A' && t <= 'Z') ||
            (t >= '0' && t <= '9') || t == '_' || t == '+' || t == ',')) return false;
    }
    binding = true;
    i = end + 2;
  }
  return binding;
}
inline bool IsUnchanged(std::string_view english, std::string_view korean) {
  return english == korean && IsBindingOnly(english);
}
}
