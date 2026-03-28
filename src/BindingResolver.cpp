#include "BindingResolver.h"
#include "TextHook.h"
#include "IW6Offsets.h"
#include "Utils.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace BindingResolver {

static std::unordered_map<std::string, std::string> g_BindCmdToKey;
static std::unordered_map<std::string, std::vector<std::string>> g_BindCmdToKeys;
static bool g_BindLoadAttempted = false;
static std::mutex g_BindMutex;

// ── Gamepad detection via XInput ─────────────────────────────────────
static bool          g_gpadDetected      = false;
static unsigned long g_gpadLastCheckTick = 0;

typedef DWORD (WINAPI *XInputGetState_t)(DWORD, void *);
static XInputGetState_t s_XInputGetState  = nullptr;
static bool             s_xinputResolved  = false;

static bool IsGamepadActive() {
  const unsigned long now = GetTickCount();
  if ((now - g_gpadLastCheckTick) < 500) return g_gpadDetected;
  g_gpadLastCheckTick = now;

  if (!s_xinputResolved) {
    s_xinputResolved = true;
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (!h) h = LoadLibraryA("xinput1_3.dll");
    if (!h) h = LoadLibraryA("xinput9_1_0.dll");
    if (h) s_XInputGetState = (XInputGetState_t)GetProcAddress(h, "XInputGetState");
  }

  if (!s_XInputGetState) { g_gpadDetected = false; return false; }

  // XINPUT_STATE: 16 bytes (dwPacketNumber + XINPUT_GAMEPAD)
  unsigned char state[16] = {};
  bool connected = (s_XInputGetState(0, state) == 0); // ERROR_SUCCESS
  g_gpadDetected = connected;
  return connected;
}

// Default Xbox-style gamepad bindings for IW6/Ghosts
static const std::unordered_map<std::string, std::string> &GetGamepadDefaults() {
  static const std::unordered_map<std::string, std::string> kPad = {
      {"+attack", "RT"},
      {"+speed_throw", "LT"},
      {"+toggleads_throw", "LT"},
      {"+activate", "X"},
      {"+use", "X"},
      {"+reload", "X"},
      {"+usereload", "X"},
      {"+gostand", "A"},
      {"+jump", "A"},
      {"+melee_zoom", "RS"},
      {"+melee", "RS"},
      {"+frag", "RB"},
      {"+smoke", "LB"},
      {"+breath_sprint", "LS"},
      {"+togglecrouch", "B"},
      {"+stance", "B"},
      {"+actionslot1", "D-Pad Up"},
      {"+actionslot2", "D-Pad Down"},
      {"+actionslot3", "D-Pad Left"},
      {"+actionslot4", "D-Pad Right"},
  };
  return kPad;
}

std::string ExtractKeyFromEnglish(const std::string &english);

static std::string ToLowerAscii(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back((char)std::tolower((unsigned char)c));
  }
  return out;
}

static std::string ToUpperAscii(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back((char)std::toupper((unsigned char)c));
  }
  return out;
}

static std::string NormalizeCmd(const std::string &cmd) {
  std::string t = TrimAscii(cmd);
  t = ToLowerAscii(t);
  std::string out;
  out.reserve(t.size());
  bool inSpace = false;
  for (char c : t) {
    if (std::isspace((unsigned char)c)) {
      if (!inSpace) {
        out.push_back(' ');
        inSpace = true;
      }
      continue;
    }
    inSpace = false;
    out.push_back(c);
  }
  // Normalize actionslot spacing: "+actionslot 1" -> "+actionslot1"
  if (out.rfind("+actionslot ", 0) == 0 && out.size() > 12) {
    char c = out[12];
    if (c >= '0' && c <= '9') {
      out.erase(11, 1); // remove space after "actionslot"
    }
  }
  return out;
}

static void ParseBindLine(const std::string &line) {
  std::string lower = ToLowerAscii(line);
  if (!(lower.rfind("bind ", 0) == 0 || lower.rfind("bind2 ", 0) == 0))
    return;

  size_t keyStart = lower.find(' ');
  if (keyStart == std::string::npos)
    return;
  size_t keyEnd = lower.find(' ', keyStart + 1);
  if (keyEnd == std::string::npos)
    return;
  std::string key = TrimAscii(line.substr(keyStart + 1, keyEnd - keyStart - 1));
  size_t q1 = line.find('"', keyEnd);
  if (q1 == std::string::npos)
    return;
  size_t q2 = line.find('"', q1 + 1);
  if (q2 == std::string::npos || q2 <= q1 + 1)
    return;

  std::string cmd = line.substr(q1 + 1, q2 - q1 - 1);
  std::string norm = NormalizeCmd(cmd);
  if (norm.empty())
    return;

  std::string keyUpper = ToUpperAscii(TrimAscii(key));
  if (keyUpper.empty())
    return;

  // Prefer the most recently observed file entry. Later files in the load
  // order should override earlier defaults/stale configs.
  g_BindCmdToKey[norm] = keyUpper;

  auto &keys = g_BindCmdToKeys[norm];
  for (auto it = keys.begin(); it != keys.end(); ++it) {
    if (ToUpperAscii(*it) == keyUpper) {
      keys.erase(it);
      break;
    }
  }
  keys.push_back(keyUpper);
}

void InvalidateBindings() {
  std::lock_guard<std::mutex> lock(g_BindMutex);
  if (!g_BindLoadAttempted)
    return;
  g_BindLoadAttempted = false;
}

static void LoadBindingsIfNeeded() {
  {
    std::lock_guard<std::mutex> lock(g_BindMutex);
    if (g_BindLoadAttempted)
      return;
    g_BindLoadAttempted = true;
  }

  std::string gameDir = GetModuleDirPath(nullptr);
  if (gameDir.empty())
    return;

  std::string paths[] = {
      gameDir + "\\players\\config.cfg",
      gameDir + "\\players\\config_mp.cfg",
      gameDir + "\\players2\\config.cfg",
      gameDir + "\\players2\\config_mp.cfg",
      gameDir + "\\players\\keys.cfg",
      gameDir + "\\players\\keys_mp.cfg",
      gameDir + "\\players2\\keys.cfg",
      gameDir + "\\players2\\keys_mp.cfg",
  };

  for (const auto &path : paths) {
    std::string fileText;
    if (!ReadTextUtf8(path, fileText))
      continue;

    std::istringstream f(fileText);
    std::string line;
    while (std::getline(f, line)) {
      ParseBindLine(line);
    }
  }

}

static std::string PrettyBindKey(const std::string &key) {
  std::string upper;
  upper.reserve(key.size());
  for (char c : key) {
    upper.push_back((char)std::toupper((unsigned char)c));
  }
  if (upper == "MWHEELUP")
    return "WHEEL UP";
  if (upper == "MWHEELDOWN")
    return "WHEEL DOWN";
  if (upper == "MOUSE1")
    return "MOUSE1";
  if (upper == "MOUSE2")
    return "MOUSE2";
  if (upper == "MOUSE3")
    return "MOUSE3";
  if (upper == "SPACE")
    return "SPACE";
  return upper;
}

static std::string ComposeBindingDisplay(const std::vector<std::string> &keys) {
  std::vector<std::string> pretty;
  pretty.reserve(keys.size());
  for (const std::string &k : keys) {
    std::string p = PrettyBindKey(k);
    if (p.empty()) {
      continue;
    }
    bool dup = false;
    std::string pUpper = ToUpperAscii(p);
    for (const std::string &ex : pretty) {
      if (ToUpperAscii(ex) == pUpper) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      pretty.push_back(p);
    }
  }
  if (pretty.empty()) {
    return "";
  }
  if (pretty.size() == 1) {
    return pretty[0];
  }
  return pretty[0] + " OR " + pretty[1];
}

static std::string CollapseSpacesAscii(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  bool prevSpace = false;
  for (char c : s) {
    if (std::isspace((unsigned char)c)) {
      if (!prevSpace) {
        out.push_back(' ');
        prevSpace = true;
      }
      continue;
    }
    prevSpace = false;
    out.push_back(c);
  }
  return TrimAscii(out);
}

static std::string StripLocalizedKeyDecorations(const std::string &s) {
  std::string out = TrimAscii(s);
  auto StripTrailingBracketPair = [&](char open, char close) {
    while (!out.empty() && out.back() == close) {
      size_t openPos = out.find_last_of(open);
      if (openPos == std::string::npos) {
        break;
      }
      bool onlyTrailing = true;
      for (size_t i = openPos; i < out.size(); ++i) {
        const char c = out[i];
        if (c == close || c == open || std::isspace((unsigned char)c)) {
          continue;
        }
        onlyTrailing = false;
        break;
      }
      if (!onlyTrailing) {
        break;
      }
      out.erase(openPos);
      out = TrimAscii(out);
    }
  };

  StripTrailingBracketPair('(', ')');
  StripTrailingBracketPair('[', ']');
  return out;
}

static bool IsFunctionKeyToken(const std::string &upper) {
  if (upper.size() < 2 || upper.size() > 4 || upper[0] != 'F') {
    return false;
  }
  for (size_t i = 1; i < upper.size(); ++i) {
    if (!std::isdigit((unsigned char)upper[i])) {
      return false;
    }
  }
  return true;
}

static std::string NormalizeBindingTokenUpper(const std::string &token) {
  std::string upper = ToUpperAscii(CollapseSpacesAscii(token));
  if (upper.empty()) {
    return upper;
  }

  if (upper.size() >= 2 &&
      ((upper.front() == '[' && upper.back() == ']') ||
       (upper.front() == '(' && upper.back() == ')'))) {
    upper = TrimAscii(upper.substr(1, upper.size() - 2));
  }

  if (upper.rfind("KEY ", 0) == 0) {
    std::string rest = TrimAscii(upper.substr(4));
    for (char &c : rest) {
      if (c == ' ') {
        c = '_';
      }
    }
    if (!rest.empty()) {
      upper = "KEY_" + rest;
    }
  }

  if (upper == "KEY_ESC")
    return "KEY_ESCAPE";
  if (upper == "KEY_DELETE")
    return "KEY_DEL";
  if (upper == "KEY_INSERT")
    return "KEY_INS";
  if (upper == "KEY_PAGEUP")
    return "KEY_PGUP";
  if (upper == "KEY_PAGEDOWN")
    return "KEY_PGDN";

  if (upper == "OR")
    return "KEY_OR";
  if (upper == "ESCAPE")
    return "ESC";
  if (upper == "DELETE")
    return "DEL";
  if (upper == "INSERT")
    return "INS";
  if (upper == "PAGE UP")
    return "PGUP";
  if (upper == "PAGE DOWN")
    return "PGDN";
  if (upper == "PAGEUP")
    return "PGUP";
  if (upper == "PAGEDOWN")
    return "PGDN";
  if (upper == "SPACEBAR")
    return "SPACE";
  if (upper == "RETURN")
    return "ENTER";

  if (upper == "LEFT MOUSE" || upper == "MOUSE LEFT" ||
      upper == "LEFTMOUSE" || upper == "LMB" || upper == "MOUSE 1")
    return "MOUSE1";
  if (upper == "RIGHT MOUSE" || upper == "MOUSE RIGHT" ||
      upper == "RIGHTMOUSE" || upper == "RMB" || upper == "MOUSE 2")
    return "MOUSE2";
  if (upper == "MIDDLE MOUSE" || upper == "MOUSE MIDDLE" ||
      upper == "MIDDLEMOUSE" || upper == "MMB" || upper == "MOUSE 3")
    return "MOUSE3";
  if (upper == "WHEEL UP" || upper == "MOUSE WHEEL UP")
    return "MWHEELUP";
  if (upper == "WHEEL DOWN" || upper == "MOUSE WHEEL DOWN")
    return "MWHEELDOWN";

  if (upper.rfind("MOUSE ", 0) == 0 && upper.size() > 6) {
    std::string tail = TrimAscii(upper.substr(6));
    bool allDigit = !tail.empty();
    for (char c : tail) {
      if (!std::isdigit((unsigned char)c)) {
        allDigit = false;
        break;
      }
    }
    if (allDigit) {
      return "MOUSE" + tail;
    }
  }

  return upper;
}

static bool IsLikelyBindingToken(const std::string &tokenUpper) {
  if (tokenUpper.empty()) {
    return false;
  }
  if (tokenUpper.rfind("KEY_", 0) == 0) {
    return true;
  }
  if (tokenUpper == "SPACE" || tokenUpper == "CTRL" || tokenUpper == "SHIFT" ||
      tokenUpper == "ALT" || tokenUpper == "TAB" || tokenUpper == "ENTER" ||
      tokenUpper == "ESC" || tokenUpper == "INS" || tokenUpper == "DEL" ||
      tokenUpper == "PGUP" || tokenUpper == "PGDN" || tokenUpper == "HOME" ||
      tokenUpper == "END" || tokenUpper == "MWHEELUP" ||
      tokenUpper == "MWHEELDOWN") {
    return true;
  }
  if (tokenUpper.rfind("MOUSE", 0) == 0) {
    return true;
  }
  if (tokenUpper.size() == 1 &&
      std::isalnum((unsigned char)tokenUpper[0])) {
    return true;
  }
  if (IsFunctionKeyToken(tokenUpper)) {
    return true;
  }
  if (tokenUpper.find(' ') == std::string::npos) {
    bool allTokenChars = true;
    for (char c : tokenUpper) {
      if (!std::isalnum((unsigned char)c) && c != '_' && c != '+' &&
          c != '-') {
        allTokenChars = false;
        break;
      }
    }
    if (allTokenChars) {
      return true;
    }
  }
  return false;
}

static std::string ToLocalizationKeyName(const std::string &tokenUpper) {
  if (tokenUpper.empty()) {
    return "";
  }
  if (tokenUpper.rfind("KEY_", 0) == 0) {
    return tokenUpper;
  }
  if (tokenUpper == "ESC")
    return "KEY_ESCAPE";
  if (tokenUpper == "DEL")
    return "KEY_DEL";
  if (tokenUpper == "INS")
    return "KEY_INS";
  if (tokenUpper == "PGUP")
    return "KEY_PGUP";
  if (tokenUpper == "PGDN")
    return "KEY_PGDN";
  if (tokenUpper == "SPACE")
    return "KEY_SPACE";
  if (tokenUpper == "ENTER")
    return "KEY_ENTER";
  if (tokenUpper == "OR")
    return "KEY_OR";
  if (!IsLikelyBindingToken(tokenUpper)) {
    return "";
  }
  return "KEY_" + tokenUpper;
}

static std::string LocalizeBindDisplayToken(const std::string &token,
                                            bool preserveSingleAsciiAlpha =
                                                false) {
  std::string trimmed = TrimAscii(token);
  if (trimmed.empty()) {
    return trimmed;
  }

  std::string upper = NormalizeBindingTokenUpper(trimmed);
  if (upper.empty()) {
    return trimmed;
  }

  if (preserveSingleAsciiAlpha && upper.size() == 1 &&
      std::isalpha((unsigned char)upper[0])) {
    return upper;
  }
  if (preserveSingleAsciiAlpha && upper.rfind("KEY_", 0) == 0 &&
      upper.size() == 5) {
    unsigned char kc = (unsigned char)upper[4];
    if (std::isalnum(kc)) {
      return std::string(1, (char)kc);
    }
  }

  // Keybind value column must not depend on localization table quality for
  // common mouse/or tokens. Force stable Korean labels here.
  if (upper == "MOUSE1" || upper == "KEY_MOUSE1") {
    return "\xEB\xA7\x88\xEC\x9A\xB0\xEC\x8A\xA4 \xEC\x99\xBC\xEC\xAA\xBD"; // 마우스 왼쪽
  }
  if (upper == "MOUSE2" || upper == "KEY_MOUSE2") {
    return "\xEB\xA7\x88\xEC\x9A\xB0\xEC\x8A\xA4 \xEC\x98\xA4\xEB\xA5\xB8\xEC\xAA\xBD"; // 마우스 오른쪽
  }
  if (upper == "MOUSE3" || upper == "KEY_MOUSE3") {
    return "\xEB\xA7\x88\xEC\x9A\xB0\xEC\x8A\xA4 \xEA\xB0\x80\xEC\x9A\xB4\xEB\x8D\xB0"; // 마우스 가운데
  }
  if (upper == "MOUSE") {
    return "\xEB\xA7\x88\xEC\x9A\xB0\xEC\x8A\xA4"; // 마우스
  }
  if (upper == "KEY_OR" || upper == "OR") {
    return "\xEB\x98\x90\xEB\x8A\x94"; // 또는
  }
  if (upper == "SPACE" || upper == "KEY_SPACE") {
    return "SPACE";
  }

  std::string keyName = ToLocalizationKeyName(upper);
  if (!keyName.empty()) {
    std::string localized;
    std::string english;
    if (TextHook_GetLocalizedKeyText(keyName, localized, english) &&
        !localized.empty()) {
      localized = StripLocalizedKeyDecorations(localized);
      if (!localized.empty()) {
        return localized;
      }
    }
  }

  if (upper.rfind("KEY_", 0) == 0 && upper.size() > 4) {
    return upper.substr(4);
  }

  if (IsLikelyBindingToken(upper)) {
    return upper;
  }
  return trimmed;
}

static bool SplitBindingCompositeBy(const std::string &text,
                                    const std::string &delimiterUpper,
                                    std::vector<std::string> &outParts) {
  outParts.clear();
  if (text.empty() || delimiterUpper.empty()) {
    return false;
  }

  std::string upper = ToUpperAscii(text);
  size_t pos = 0;
  while (true) {
    size_t d = upper.find(delimiterUpper, pos);
    if (d == std::string::npos) {
      std::string tail = TrimAscii(text.substr(pos));
      if (!tail.empty()) {
        outParts.push_back(tail);
      }
      break;
    }
    std::string part = TrimAscii(text.substr(pos, d - pos));
    if (part.empty()) {
      outParts.clear();
      return false;
    }
    outParts.push_back(part);
    pos = d + delimiterUpper.size();
  }
  return outParts.size() >= 2;
}

static bool IsDirectionalMouseWordUpper(const std::string &upper) {
  return (upper == "LEFT" || upper == "RIGHT" || upper == "MIDDLE");
}

static void NormalizeCompositeMouseParts(std::vector<std::string> &parts) {
  bool hasMouseToken = false;
  for (const std::string &part : parts) {
    const std::string upper = ToUpperAscii(CollapseSpacesAscii(part));
    if (upper.find("MOUSE") != std::string::npos) {
      hasMouseToken = true;
      break;
    }
  }
  if (!hasMouseToken) {
    return;
  }

  for (std::string &part : parts) {
    const std::string upper = ToUpperAscii(CollapseSpacesAscii(part));
    if (IsDirectionalMouseWordUpper(upper)) {
      part = TrimAscii(part) + " MOUSE";
    }
  }
}

std::string LocalizeBindingDisplayText(const std::string &text,
                                       bool preserveSingleAsciiAlpha) {
  std::string normalized = CollapseSpacesAscii(text);
  if (normalized.empty()) {
    return normalized;
  }

  std::vector<std::string> parts;
  bool isComposite = SplitBindingCompositeBy(normalized, " KEY_OR ", parts);
  if (!isComposite) {
    isComposite = SplitBindingCompositeBy(normalized, " OR ", parts);
  }
  if (!isComposite) {
    isComposite = SplitBindingCompositeBy(normalized, " \xEB\x98\x90\xEB\x8A\x94 ", parts); // " 또는 "
  }

  if (isComposite) {
    NormalizeCompositeMouseParts(parts);
    bool allLikelyBinding = true;
    for (const std::string &p : parts) {
      if (!IsLikelyBindingToken(NormalizeBindingTokenUpper(p))) {
        allLikelyBinding = false;
        break;
      }
    }
    if (allLikelyBinding) {
      std::string joinOr = LocalizeBindDisplayToken("KEY_OR");
      if (joinOr.empty()) {
        joinOr = "\xEB\x98\x90\xEB\x8A\x94"; // "또는"
      }
      std::string out;
      for (size_t i = 0; i < parts.size(); ++i) {
        std::string localizedPart =
            LocalizeBindDisplayToken(parts[i], preserveSingleAsciiAlpha);
        if (localizedPart.empty()) {
          localizedPart = TrimAscii(parts[i]);
        }
        if (i > 0) {
          out.append(" ");
          out.append(joinOr);
          out.append(" ");
        }
        out.append(localizedPart);
      }
      return out;
    }
  }

  return LocalizeBindDisplayToken(normalized, preserveSingleAsciiAlpha);
}

// IW6 command aliases: game uses different internal names for the same action.
// e.g., +changezoom in localize strings maps to +melee_zoom in keys.cfg.
static std::string ResolveAlias(const std::string &norm) {
  // Map: localize placeholder command ??actual keys.cfg command
  static const std::unordered_map<std::string, std::string> kAliases = {
      {"+changezoom", "+melee_zoom"},
      {"+melee", "+melee_zoom"},
      {"+sprint", "+breath_sprint"},
      {"+holdbreath", "+breath_sprint"},
      {"+throw", "+frag"},
      {"+stance", "+togglecrouch"},
      {"+prone", "toggleprone"},
      {"+jump", "+gostand"},
      {"+use", "+activate"},
      {"+attack2", "+speed_throw"},
      {"+toggleads_throw", "+speed_throw"},
  };
  auto it = kAliases.find(norm);
  return (it != kAliases.end()) ? it->second : "";
}

// IW6 Ghosts button control-char mapping (from localize.json LUA_MENU_PAD_*)
// Returns the control char (1–23) for a given game command, or '\0'.
static char GetGamepadButtonChar(const std::string &norm) {
  // Face buttons
  if (norm == "+gostand"  || norm == "+jump")                 return '\x01'; // A
  if (norm == "+togglecrouch" || norm == "+stance")           return '\x02'; // B
  if (norm == "+activate" || norm == "+use" || norm == "+reload" || norm == "+usereload") return '\x03'; // X
  if (norm == "weapnext"  || norm == "weapprev")              return '\x04'; // Y
  // Shoulder buttons
  if (norm == "+smoke")                                        return '\x05'; // LB
  if (norm == "+frag")                                         return '\x06'; // RB
  // Start/Back
  // (14=Start, 15=Back — rarely used in hints; omit for now)
  // Sticks
  if (norm == "+breath_sprint")                               return '\x10'; // LS click
  if (norm == "+melee_zoom" || norm == "+melee")              return '\x11'; // RS click
  // Triggers
  if (norm == "+speed_throw" || norm == "+toggleads_throw")   return '\x12'; // LT
  if (norm == "+attack" || norm == "+attack2")                return '\x13'; // RT
  // D-Pad
  if (norm == "+actionslot1")                                  return '\x14'; // D-Up
  if (norm == "+actionslot2")                                  return '\x15'; // D-Down
  if (norm == "+actionslot3")                                  return '\x16'; // D-Left
  if (norm == "+actionslot4")                                  return '\x17'; // D-Right
  return '\0';
}

std::string ResolveBindingDisplay(const std::string &command) {
  LoadBindingsIfNeeded();
  std::string norm = NormalizeCmd(command);
  if (norm.empty())
    return "";

  // Gamepad mode: return the native button control char so KoreanRenderer
  // can render the actual button glyph image inline.
  if (IsGamepadActive()) {
    char ch = GetGamepadButtonChar(norm);
    if (ch == '\0') {
      // Try alias lookup for unmapped commands
      std::string alias = ResolveAlias(norm);
      if (!alias.empty()) ch = GetGamepadButtonChar(alias);
    }
    if (ch != '\0')
      return std::string(1, ch);
  }

  {
    std::lock_guard<std::mutex> lock(g_BindMutex);

    // 1. Direct lookup
    auto itMulti = g_BindCmdToKeys.find(norm);
    if (itMulti != g_BindCmdToKeys.end() && !itMulti->second.empty()) {
      std::string composite = ComposeBindingDisplay(itMulti->second);
      if (!composite.empty()) {
        return composite;
      }
    }
    auto it = g_BindCmdToKey.find(norm);
    if (it != g_BindCmdToKey.end())
      return PrettyBindKey(it->second);

    // 2. Try alias (e.g., +changezoom ??+melee_zoom)
    std::string alias = ResolveAlias(norm);
    if (!alias.empty()) {
      auto itMulti2 = g_BindCmdToKeys.find(alias);
      if (itMulti2 != g_BindCmdToKeys.end() && !itMulti2->second.empty()) {
        std::string composite = ComposeBindingDisplay(itMulti2->second);
        if (!composite.empty()) {
          return composite;
        }
      }
      auto it2 = g_BindCmdToKey.find(alias);
      if (it2 != g_BindCmdToKey.end())
        return PrettyBindKey(it2->second);
    }

    // 3. Reverse alias: search all aliases for this command
    //    (in case the alias itself is what's in the localize string)
    for (const auto &bind : g_BindCmdToKeys) {
      std::string bindAlias = ResolveAlias(bind.first);
      if (!bindAlias.empty() && bindAlias == norm) {
        std::string composite = ComposeBindingDisplay(bind.second);
        if (!composite.empty()) {
          return composite;
        }
      }
    }
    for (const auto &bind : g_BindCmdToKey) {
      std::string bindAlias = ResolveAlias(bind.first);
      if (!bindAlias.empty() && bindAlias == norm) {
        return PrettyBindKey(bind.second);
      }
    }
  }

  // 4. Hardcoded fallbacks (only when keys.cfg is completely empty/unreadable)
  static const std::unordered_map<std::string, std::string> kFallback = {
      {"+attack", "MOUSE1"},
      {"+activate", "F"},
      {"+reload", "R"},
      {"+gostand", "SPACE"},
      {"+frag", "G"},
      {"+smoke", "Q"},
      {"+actionslot1", "1"},
      {"+actionslot2", "2"},
      {"+actionslot3", "3"},
      {"+actionslot4", "4"},
      {"+speed_throw", "MOUSE2"},
      {"+toggleads_throw", "MOUSE2"},
      {"+melee_zoom", "E"},
      {"+breath_sprint", "SHIFT"},
      {"+togglecrouch", "C"},
  };
  auto it2 = kFallback.find(norm);
  if (it2 != kFallback.end())
    return it2->second;

  // 5. Try alias for fallback too
  std::string alias = ResolveAlias(norm);
  if (!alias.empty()) {
    auto it3 = kFallback.find(alias);
    if (it3 != kFallback.end())
      return it3->second;
  }

  return "";
}

static std::string ResolveActionTokenToBinding(const std::string &tokenUpper) {
  std::string normalized = tokenUpper;
  while (!normalized.empty() && normalized.front() == '+') {
    normalized.erase(normalized.begin());
  }
  if (normalized == "ATTACK") {
    return ResolveBindingDisplay("+attack");
  }
  if (normalized == "SPEED_THROW") {
    return ResolveBindingDisplay("+speed_throw");
  }
  if (normalized == "TOGGLEADS_THROW") {
    return ResolveBindingDisplay("+toggleads_throw");
  }
  if (normalized == "ACTIVATE" || normalized == "USE") {
    return ResolveBindingDisplay("+activate");
  }
  if (normalized == "FRAG" || normalized == "GRENADE") {
    return ResolveBindingDisplay("+frag");
  }
  if (normalized == "WEAPNEXT" || normalized == "NEXTWEAPON" ||
      normalized == "SWITCH_WEAPON") {
    return ResolveBindingDisplay("weapnext");
  }
  if (normalized == "WEAPPREV" || normalized == "PREVWEAPON") {
    return ResolveBindingDisplay("weapprev");
  }
  if (normalized.rfind("ACTIONSLOT", 0) == 0 && normalized.size() > 10) {
    std::string digit = normalized.substr(10);
    if (!digit.empty() &&
        std::all_of(digit.begin(), digit.end(), [](char c) {
          return std::isdigit((unsigned char)c) != 0;
        })) {
      return ResolveBindingDisplay("+actionslot " + digit);
    }
  }
  if (normalized == "THROW" || normalized == "THROWBACK" ||
      normalized == "THROW BACK" || normalized == "THROW_BACK" ||
      normalized == "THROW GRENADE" || normalized == "THROWGRENADE") {
    std::string r = ResolveBindingDisplay("+frag");
    if (!r.empty()) {
      return r;
    }
    r = ResolveBindingDisplay("+throw");
    if (!r.empty()) {
      return r;
    }
    r = ResolveBindingDisplay("+speed_throw");
    if (!r.empty()) {
      return r;
    }
    r = ResolveBindingDisplay("+toggleads_throw");
    if (!r.empty()) {
      return r;
    }
  }
  return "";
}

static bool ContainsUpperWord(const std::string &upper, const char *word) {
  if (upper.empty() || !word || !word[0]) {
    return false;
  }
  const std::string w(word);
  size_t pos = upper.find(w);
  while (pos != std::string::npos) {
    const size_t end = pos + w.size();
    const bool leftOk =
        (pos == 0) || !std::isalpha((unsigned char)upper[pos - 1]);
    const bool rightOk =
        (end >= upper.size()) || !std::isalpha((unsigned char)upper[end]);
    if (leftOk && rightOk) {
      return true;
    }
    pos = upper.find(w, pos + 1);
  }
  return false;
}

static std::string RemoveBracketedBindingDecorations(
    const std::string &input, const std::string &englishFallback) {
  if (input.find('[') == std::string::npos || input.find(']') == std::string::npos) {
    return input;
  }

  const std::string englishUpper = ToUpperAscii(englishFallback);
  const bool bindingContext =
      (!englishFallback.empty() &&
       (ContainsUpperWord(englishUpper, "PRESS") ||
        ContainsUpperWord(englishUpper, "HOLD") ||
        ContainsUpperWord(englishUpper, "TAP") ||
        ContainsUpperWord(englishUpper, "USE") ||
        englishUpper.find("THROW BACK") != std::string::npos ||
        englishUpper.find("[{") != std::string::npos ||
        englishUpper.find("&&") != std::string::npos ||
        englishUpper.find("$$") != std::string::npos));

  auto HasColorCodeLeft = [&](size_t idx) -> bool {
    size_t p = idx;
    while (p > 0 && std::isspace((unsigned char)input[p - 1])) {
      --p;
    }
    return (p >= 2 && input[p - 2] == '^' &&
            std::isdigit((unsigned char)input[p - 1]));
  };
  auto HasColorCodeRight = [&](size_t idx) -> bool {
    size_t p = idx + 1;
    while (p < input.size() && std::isspace((unsigned char)input[p])) {
      ++p;
    }
    return (p + 1 < input.size() && input[p] == '^' &&
            std::isdigit((unsigned char)input[p + 1]));
  };

  std::string out;
  out.reserve(input.size());
  size_t pos = 0;
  while (pos < input.size()) {
    size_t lb = input.find('[', pos);
    if (lb == std::string::npos) {
      out.append(input.substr(pos));
      break;
    }
    out.append(input.substr(pos, lb - pos));
    size_t rb = input.find(']', lb + 1);
    if (rb == std::string::npos) {
      out.append(input.substr(lb));
      break;
    }

    std::string token = TrimAscii(input.substr(lb + 1, rb - lb - 1));

    // Button glyph control chars (1–23): emit bare, no color wrapping.
    if (token.size() == 1 && (unsigned char)token[0] >= 1 &&
        (unsigned char)token[0] <= 23) {
      out.append(token);
      pos = rb + 1;
      continue;
    }

    // Game variable placeholders like "&&1 Remaining" / "&&1 남음" are NOT
    // key bindings — preserve brackets as-is, no color override.
    if (token.find("&&") != std::string::npos || token.find("$$") != std::string::npos) {
      out.append(input.substr(lb, rb - lb + 1));
      pos = rb + 1;
      continue;
    }

    // Already-localized tokens (contain non-ASCII / Korean text from Pass 1
    // of ResolveBindingsInText) must NOT be re-processed.  Output the token
    // directly — it already carries the correct localized binding display.
    {
      bool hasNonAscii = false;
      for (unsigned char c : token) {
        if (c >= 0x80) { hasNonAscii = true; break; }
      }
      if (hasNonAscii) {
        out.append(token);
        pos = rb + 1;
        continue;
      }
    }

    std::string tokenUpper = NormalizeBindingTokenUpper(token);
    std::string replacement;
    bool keyLikeToken = false;

    if (!tokenUpper.empty()) {
      std::string actionResolved = ResolveActionTokenToBinding(tokenUpper);
      if (!actionResolved.empty()) {
        replacement = LocalizeBindingDisplayText(actionResolved, true);
        keyLikeToken = true;
      }
      if (replacement.empty() &&
          (tokenUpper.find(" OR ") != std::string::npos ||
           IsLikelyBindingToken(tokenUpper) || tokenUpper.rfind("KEY_", 0) == 0)) {
        replacement = LocalizeBindingDisplayText(tokenUpper, true);
        keyLikeToken = true;
      }
    }

    if (replacement.empty() && bindingContext && !englishFallback.empty()) {
      std::string inferred = ExtractKeyFromEnglish(englishFallback);
      if (!inferred.empty()) {
        replacement = LocalizeBindingDisplayText(inferred, true);
        keyLikeToken = true;
      }
    }

    if (!replacement.empty()) {
      if ((bindingContext || keyLikeToken) && replacement.find('^') == std::string::npos) {
        if (!HasColorCodeLeft(lb) && !HasColorCodeRight(rb)) {
          replacement = "^3" + replacement + "^7";
        }
      }
      out.append(replacement);
    } else {
      std::string rawToken =
          token.empty() ? TrimAscii(input.substr(lb + 1, rb - lb - 1)) : token;
      if (!rawToken.empty() && (bindingContext || keyLikeToken)) {
        if (rawToken.find('^') == std::string::npos &&
            !HasColorCodeLeft(lb) && !HasColorCodeRight(rb)) {
          rawToken = "^3" + rawToken + "^7";
        }
        out.append(rawToken);
      } else if (!rawToken.empty()) {
        const bool tokenLooksLikeBinding =
            (!tokenUpper.empty() &&
             (tokenUpper.rfind("KEY_", 0) == 0 || IsLikelyBindingToken(tokenUpper)));
        if (tokenLooksLikeBinding) {
          out.append(rawToken);
        } else {
          out.append(input.substr(lb, rb - lb + 1));
        }
      } else {
        out.append(input.substr(lb, rb - lb + 1));
      }
    }

    // After emitting a binding key token, peek past any trailing color
    // codes (e.g. ^7) — if the very next visible character is non-ASCII
    // (Korean) and NOT whitespace, insert a space so the display reads
    // "SPACE 키를 눌러" instead of "SPACE키를 눌러".
    if (keyLikeToken || !replacement.empty()) {
      size_t peek = rb + 1;
      while (peek + 1 < input.size() && input[peek] == '^' &&
             std::isdigit((unsigned char)input[peek + 1])) {
        peek += 2;
      }
      if (peek < input.size() &&
          (unsigned char)input[peek] >= 0x80 &&
          !out.empty() && out.back() != ' ') {
        out.push_back(' ');
      }
    }

    pos = rb + 1;
  }

  return out;
}

static bool IsTokenChar(char c) {
  return std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '+';
}

static std::string ResolveBindingTokenDisplay(const std::string &bind,
                                              const std::string &englishFallback) {
  const std::string trimmed = TrimAscii(bind);
  if (trimmed.empty()) {
    return "";
  }

  const std::string normalizedUpper = NormalizeBindingTokenUpper(trimmed);
  std::string resolved = ResolveBindingDisplay(trimmed);
  if (resolved.empty() && !trimmed.empty() && trimmed.front() != '+') {
    resolved = ResolveBindingDisplay("+" + trimmed);
  }
  if (resolved.empty()) {
    resolved = ResolveActionTokenToBinding(normalizedUpper);
  }
  if (resolved.empty() && !englishFallback.empty()) {
    std::string inferred = ExtractKeyFromEnglish(englishFallback);
    if (!inferred.empty()) {
      resolved = inferred;
    }
  }
  if (!resolved.empty()) {
    resolved = LocalizeBindingDisplayText(resolved, true);
  }
  return resolved;
}

static std::string ResolveResidualBindingCommands(
    const std::string &text, const std::string &englishFallback) {
  if (text.empty()) {
    return text;
  }

  auto ResolveWrappedToken = [&](char open, char close, size_t start,
                                 size_t &nextPos, std::string &replacement) -> bool {
    if (text[start] != open) {
      return false;
    }
    const size_t end = text.find(close, start + 1);
    if (end == std::string::npos || end <= start + 1) {
      return false;
    }
    std::string token = TrimAscii(text.substr(start + 1, end - start - 1));
    if (token.empty()) {
      return false;
    }
    const std::string tokenUpper = NormalizeBindingTokenUpper(token);
    const bool candidate =
        (token.find('+') != std::string::npos) ||
        (!tokenUpper.empty() &&
         (IsLikelyBindingToken(tokenUpper) ||
          !ResolveActionTokenToBinding(tokenUpper).empty()));
    if (!candidate) {
      return false;
    }
    const std::string resolved =
        ResolveBindingTokenDisplay(token, englishFallback);
    if (resolved.empty()) {
      return false;
    }
    replacement = resolved;
    nextPos = end + 1;
    return true;
  };

  std::string out;
  out.reserve(text.size());
  size_t pos = 0;
  while (pos < text.size()) {
    std::string replacement;
    size_t nextPos = pos;

    if (ResolveWrappedToken('{', '}', pos, nextPos, replacement) ||
        ResolveWrappedToken('[', ']', pos, nextPos, replacement)) {
      out.append(replacement);
      pos = nextPos;
      continue;
    }

    if (text[pos] == '+') {
      const bool leftBoundary =
          (pos == 0 || !IsTokenChar(text[pos - 1]));
      if (leftBoundary) {
        size_t end = pos + 1;
        while (end < text.size() && IsTokenChar(text[end])) {
          ++end;
        }
        const std::string token = text.substr(pos, end - pos);
        const std::string resolved =
            ResolveBindingTokenDisplay(token, englishFallback);
        if (!resolved.empty()) {
          out.append(resolved);
          pos = end;
          continue;
        }
      }
    }

    out.push_back(text[pos]);
    ++pos;
  }
  return out;
}

static bool IsIgnoredPromptWord(const std::string &upper) {
  if (upper.empty())
    return true;
  return (upper == "TO" || upper == "THE" || upper == "FOR" ||
          upper == "AND" || upper == "OR" || upper == "A" ||
          upper == "AN" || upper == "KEY" || upper == "BUTTON" ||
          upper == "PRESS" || upper == "HOLD" || upper == "TAP" ||
          upper == "USE");
}

std::string ExtractKeyFromEnglish(const std::string &english) {
  if (english.empty())
    return "";
  std::string lower = ToLowerAscii(english);
  const char *patterns[] = {"press ", "hold ", "tap ", "use "};
  size_t bestPos = std::string::npos;
  size_t bestLen = 0;
  for (const char *p : patterns) {
    size_t pos = lower.find(p);
    if (pos != std::string::npos) {
      if (bestPos == std::string::npos || pos < bestPos) {
        bestPos = pos;
        bestLen = strlen(p);
      }
    }
  }
  if (bestPos == std::string::npos) {
    auto UpperAscii = [](const std::string &s) -> std::string {
      std::string out;
      out.reserve(s.size());
      for (char c : s) {
        out.push_back((char)std::toupper((unsigned char)c));
      }
      return out;
    };
    const std::string upper = UpperAscii(english);
    const size_t throwBackPos = upper.find("THROW BACK");
    if (throwBackPos != std::string::npos) {
      std::string bindingLead = TrimAscii(english.substr(0, throwBackPos));
      const std::string bindingLeadUpper = UpperAscii(bindingLead);
      const size_t orPos = bindingLeadUpper.find(" OR ");
      if (orPos != std::string::npos) {
        bindingLead = TrimAscii(bindingLead.substr(0, orPos));
      }
      const std::string leadUpper = UpperAscii(bindingLead);
      if (leadUpper == "LEFT MOUSE")
        return "MOUSE1";
      if (leadUpper == "RIGHT MOUSE")
        return "MOUSE2";
      if (leadUpper == "MIDDLE MOUSE")
        return "MOUSE3";
      if (leadUpper == "SPACEBAR")
        return "SPACE";
      if (!leadUpper.empty() &&
          (leadUpper.size() == 1 ||
           leadUpper == "SPACE" || leadUpper == "CTRL" ||
           leadUpper == "SHIFT" || leadUpper == "ALT" ||
           leadUpper == "TAB" || leadUpper == "ENTER" ||
           leadUpper == "ESC" || leadUpper == "ESCAPE" ||
           leadUpper == "MOUSE1" || leadUpper == "MOUSE2" ||
           leadUpper == "MOUSE3")) {
        return (leadUpper == "ESCAPE") ? "ESC" : leadUpper;
      }
    }
    return "";
  }

  size_t i = bestPos + bestLen;
  auto SkipDecor = [&](size_t &p) {
    while (p < english.size()) {
      unsigned char c = (unsigned char)english[p];
      if (std::isspace(c) || c == '"' || c == '\'' ||
          c == '(' || c == ')' || c == '{' || c == '}' || c == '<' ||
          c == '>' || c == ',' || c == ':' || c == ';') {
        p++;
        continue;
      }
      // Skip IW color codes (^3, ^7, ...)
      if (c == '^' && (p + 1) < english.size()) {
        p += 2;
        continue;
      }
      break;
    }
  };
  auto ReadPromptToken = [&](size_t &p, std::string &token,
                             std::string &canonicalUpper) -> bool {
    token.clear();
    canonicalUpper.clear();
    SkipDecor(p);
    if (p >= english.size()) {
      return false;
    }

    size_t tokenEnd = p;
    if (english[p] == '[') {
      size_t rb = english.find(']', p + 1);
      if (rb != std::string::npos && rb > p + 1) {
        token = english.substr(p + 1, rb - (p + 1));
        tokenEnd = rb + 1;
      }
    }
    if (token.empty()) {
      size_t start = p;
      unsigned char firstCh = (unsigned char)english[p];
      if (firstCh >= 0x01 && firstCh <= 0x17) {
        // Game pre-substituted &&N with a gamepad button control char — treat as a
        // single-char token so the button glyph path can render it.
        tokenEnd = p + 1;
      } else {
        while (tokenEnd < english.size() && IsTokenChar(english[tokenEnd])) {
          tokenEnd++;
        }
      }
      if (tokenEnd > start) {
        token = english.substr(start, tokenEnd - start);
      }
    }
    p = tokenEnd;
    if (token.empty()) {
      return false;
    }

    canonicalUpper = NormalizeBindingTokenUpper(token);
    if (canonicalUpper.empty()) {
      canonicalUpper.reserve(token.size());
      for (char c : token) {
        canonicalUpper.push_back((char)std::toupper((unsigned char)c));
      }
    }
    return !canonicalUpper.empty();
  };

  for (int attempt = 0; attempt < 8; ++attempt) {
    SkipDecor(i);
    if (i >= english.size()) {
      return "";
    }
    if (i + 2 < english.size() &&
        ((english[i] == '&' && english[i + 1] == '&') ||
         (english[i] == '$' && english[i + 1] == '$'))) {
      i += 2;
      while (i < english.size() && std::isdigit((unsigned char)english[i])) {
        i++;
      }
      continue;
    }

    std::string token;
    std::string upper;
    if (!ReadPromptToken(i, token, upper)) {
      continue;
    }
    {
      size_t lookahead = i;
      std::string nextToken;
      std::string nextUpper;
      if (ReadPromptToken(lookahead, nextToken, nextUpper)) {
        const std::string combined =
            NormalizeBindingTokenUpper(token + " " + nextToken);
        if (combined == "MOUSE1" || combined == "MOUSE2" ||
            combined == "MOUSE3" || combined == "MWHEELUP" ||
            combined == "MWHEELDOWN") {
          return combined;
        }
      }
    }

    if (IsIgnoredPromptWord(upper)) {
      continue;
    }

    // Normalize common variants
    if (upper == "LEFTMOUSE")
      return "MOUSE1";
    if (upper == "RIGHTMOUSE")
      return "MOUSE2";
    if (upper == "MIDDLEMOUSE")
      return "MOUSE3";
    if (upper == "SPACEBAR")
      return "SPACE";
    if (upper == "LMB")
      return "MOUSE1";
    if (upper == "RMB")
      return "MOUSE2";
    if (upper == "MOUSE1" || upper == "MOUSE2" || upper == "MOUSE3")
      return upper;
    if (upper == "LEFT" || upper == "RIGHT" || upper == "MIDDLE" ||
        upper == "MOUSE") {
      continue;
    }
    if (upper == "SHIFT" || upper == "CTRL" || upper == "ALT" ||
        upper == "TAB" || upper == "ENTER" || upper == "ESC" ||
        upper == "ESCAPE") {
      return upper == "ESCAPE" ? "ESC" : upper;
    }
    if (upper.size() == 1 || (upper.size() <= 3 && upper[0] == 'F')) {
      return upper;
    }
    // Fallback for uncommon key names (e.g., MWHEELUP).
    if (upper.size() <= 24) {
      return upper;
    }
  }
  return "";
}

std::string ResolveBindingsInText(const std::string &text,
                                  const std::string &englishFallback) {
  // Periodic gamepad state probe — logs on every state change.
  {
    static unsigned long s_lastGpadProbe = 0;
    static bool s_lastState = false;
    unsigned long now = GetTickCount();
    if ((now - s_lastGpadProbe) > 500) {
      s_lastGpadProbe = now;
      bool gpad = IsGamepadActive();
      if (gpad != s_lastState) {
        s_lastState = gpad;
        char buf[64];
        sprintf_s(buf, "[XINPUT] state → %s  tick=%lu", gpad ? "ON " : "OFF", now);
        LogToFile(buf);
      }
    }
  }
  if (text.find("[{") == std::string::npos &&
      text.find('{') == std::string::npos &&
      text.find("&&") == std::string::npos &&
      text.find("$$") == std::string::npos) {
    return ResolveResidualBindingCommands(
        RemoveBracketedBindingDecorations(text, englishFallback),
        englishFallback);
  }

  auto ResolveInlineBinding = [&](const std::string &bind) -> std::string {
    return ResolveBindingTokenDisplay(bind, englishFallback);
  };

  auto IsBraceBindingCandidate = [&](const std::string &bind) -> bool {
    std::string trimmed = TrimAscii(bind);
    if (trimmed.empty() || trimmed.size() > 32) {
      return false;
    }
    for (char c : trimmed) {
      if (!IsTokenChar(c) && !std::isspace((unsigned char)c)) {
        return false;
      }
    }
    return true;
  };

  // Pass 1: [{+command}] and {+command}/{command} placeholders
  std::string pass1;
  if (text.find("[{") != std::string::npos ||
      text.find('{') != std::string::npos) {
    pass1.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
      size_t startBracket = text.find("[{", pos);
      size_t startBrace = text.find('{', pos);
      bool bracketForm = false;
      size_t start = std::string::npos;
      if (startBracket != std::string::npos &&
          (startBrace == std::string::npos || startBracket <= startBrace)) {
        start = startBracket;
        bracketForm = true;
      } else if (startBrace != std::string::npos) {
        start = startBrace;
      }
      if (start == std::string::npos) {
        pass1.append(text.substr(pos));
        break;
      }

      pass1.append(text.substr(pos, start - pos));
      if (bracketForm) {
        size_t end = text.find("}]", start + 2);
        if (end == std::string::npos) {
          pass1.append(text.substr(start));
          break;
        }
        std::string bind = text.substr(start + 2, end - (start + 2));
        std::string resolved = ResolveInlineBinding(bind);
        // Button glyph control chars (1–23): output bare, no bracket wrapping.
        if (resolved.size() == 1 && (unsigned char)resolved[0] >= 1 &&
            (unsigned char)resolved[0] <= 23) {
          pass1.append(resolved);
        } else {
          pass1.append("[" + (!resolved.empty() ? resolved : TrimAscii(bind)) + "]");
        }
        pos = end + 2;
        continue;
      }

      size_t end = text.find('}', start + 1);
      if (end == std::string::npos) {
        pass1.append(text.substr(start));
        break;
      }
      std::string bind = text.substr(start + 1, end - (start + 1));
      if (!IsBraceBindingCandidate(bind)) {
        pass1.append(text.substr(start, end - start + 1));
        pos = end + 1;
        continue;
      }
      std::string resolved = ResolveInlineBinding(bind);
      pass1.append("[" + (!resolved.empty() ? resolved : TrimAscii(bind)) + "]");
      pos = end + 1;
    }
  } else {
    pass1 = text;
  }

  // Pass 2: &&1 / $$1 placeholders used by some PLATFORM_* hints.
  if (pass1.find("&&") == std::string::npos &&
      pass1.find("$$") == std::string::npos) {
    return ResolveResidualBindingCommands(
        RemoveBracketedBindingDecorations(pass1, englishFallback),
        englishFallback);
  }

  std::string inferred;
  if (!englishFallback.empty()) {
    inferred = ExtractKeyFromEnglish(englishFallback);
    if (!inferred.empty()) {
      inferred = LocalizeBindingDisplayText(inferred, true);
    }
  }
  if (inferred.empty()) {
    // Fallback: if pass1 already resolved another binding token in this line,
    // reuse it for &&n placeholders to avoid leaving raw &&1 on screen.
    size_t lb = pass1.find('[');
    while (lb != std::string::npos) {
      size_t rb = pass1.find(']', lb + 1);
      if (rb == std::string::npos || rb <= lb + 1) {
        break;
      }
      std::string token = TrimAscii(pass1.substr(lb + 1, rb - lb - 1));
      if (!token.empty() && token[0] != '+' && token[0] != '{' &&
          token.find("&&") == std::string::npos) {
        inferred = token;
        break;
      }
      lb = pass1.find('[', rb + 1);
    }
  }

  std::string out;
  out.reserve(pass1.size());
  size_t pos = 0;
  auto FindPlaceholder = [&](size_t from, size_t &tokenLen) -> size_t {
    tokenLen = 0;
    size_t pAmp = pass1.find("&&", from);
    size_t pDol = pass1.find("$$", from);
    size_t p = std::string::npos;
    if (pAmp == std::string::npos) {
      p = pDol;
      tokenLen = 2;
    } else if (pDol == std::string::npos) {
      p = pAmp;
      tokenLen = 2;
    } else {
      p = (pAmp < pDol) ? pAmp : pDol;
      tokenLen = 2;
    }
    return p;
  };
  while (true) {
    size_t placeholderLen = 0;
    size_t amp = FindPlaceholder(pos, placeholderLen);
    if (amp == std::string::npos || placeholderLen == 0) {
      out.append(pass1.substr(pos));
      break;
    }
    out.append(pass1.substr(pos, amp - pos));

    size_t end = amp + placeholderLen;
    while (end < pass1.size() &&
           std::isdigit((unsigned char)pass1[end])) {
      end++;
    }

    if (end > (amp + 2)) {
      if (!inferred.empty()) {
        out.append("[" + inferred + "]");
      } else {
        // Keep original token if we could not infer a key name.
        out.append(pass1.substr(amp, end - amp));
      }
      pos = end;
    } else {
      out.append(pass1.substr(amp, placeholderLen));
      pos = amp + placeholderLen;
    }
  }

  return ResolveResidualBindingCommands(
      RemoveBracketedBindingDecorations(out, englishFallback),
      englishFallback);
}

} // namespace BindingResolver
