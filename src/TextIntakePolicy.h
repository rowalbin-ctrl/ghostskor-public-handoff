#pragma once
#include <cstddef>
#include <cstring>
#include <string_view>

namespace TextIntakePolicy {
inline bool StartsWith(const char* text, const char* prefix) {
  if (!text) return false;
  while (*prefix) if (*text++ != *prefix++) return false;
  return true;
}
// An exclusion applies to this string only. It never changes patch state.
inline bool IsUntranslatedCreditsKey(const char* text) {
  if (text && *text == '@') ++text;
  return StartsWith(text, "CREDITS_");
}
// Configstrings belong to an identified native HudElem. They are messages,
// not necessarily a single localization key: 4364C0 accepts 0x1E/0x1F
// localized/literal runs and 0x16 argument separators. Do not discard a
// native message just because it has already been resolved to English.
// Keep the inexpensive credits exclusion scoped to the message itself.
inline bool ShouldInspectNativeConfig(std::string_view text) {
  bool localized = true;
  size_t start = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    const unsigned char c = i == text.size() ? 0 : (unsigned char)text[i];
    if (c != 0 && c != 0x1E && c != 0x1F && c != 0x16) continue;
    auto run = text.substr(start, i - start);
    if (!run.empty()) {
      if (!localized) return true; // literal English can also have a translation
      if (run.front() == '@') run.remove_prefix(1);
      if (run.substr(0, 8) != "CREDITS_") return true;
    }
    if (c == 0) break;
    if (c == 0x1E) localized = true;
    if (c == 0x1F) localized = false;
    start = i + 1;
  }
  return false;
}
inline bool IsLocalizationKey(const char* text) {
  if (!text) return false;
  if (*text == '@') ++text;
  bool underscore = false, alpha = false;
  for (size_t i = 0; i < 128; ++i) {
    unsigned char c = (unsigned char)text[i];
    if (!c) return i >= 5 && underscore && alpha;
    if (c == '_') underscore = true;
    else if (c >= 'A' && c <= 'Z') alpha = true;
    else if (!(c >= '0' && c <= '9') && c != '.') return false;
  }
  return false;
}
inline bool HasHudPrefix(const char* text) {
  return StartsWith(text,"GAME_") || StartsWith(text,"CGAME_") ||
         StartsWith(text,"EXE_") || StartsWith(text,"SCRIPT_") ||
         StartsWith(text,"CORNERED_") || StartsWith(text,"HINT_") ||
         StartsWith(text,"OBJ_") || StartsWith(text,"PLATFORM_");
}
inline bool IsPrompt(const char* text) {
  if (!text) return false;
  // These are native binding markers and the English prompt leads already
  // accepted by the HUD path; ordinary staff names never need HUD discovery.
  return StartsWith(text,"PRESS ") || StartsWith(text,"Press ") ||
         StartsWith(text,"HOLD ") || StartsWith(text,"Hold ") ||
         StartsWith(text,"TAP ") || StartsWith(text,"Tap ") ||
         StartsWith(text,"[{+") || StartsWith(text,"&&");
}
inline bool ShouldInspectSlc(const char* text, bool hudContext, bool trustedCaller) {
  if (!text || !*text || IsUntranslatedCreditsKey(text)) return false;
  if (IsLocalizationKey(text))
    return HasHudPrefix(text) || hudContext || trustedCaller;
  return trustedCaller || (hudContext && IsPrompt(text));
}
}
