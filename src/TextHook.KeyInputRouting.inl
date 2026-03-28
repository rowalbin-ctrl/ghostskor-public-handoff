__declspec(noinline) static bool TryReadCStringPreview(const char *s,
                                                       size_t maxLen,
                                                       std::string &out) {
  out.clear();
  if (!s || maxLen < 4)
    return false;
  uintptr_t v = (uintptr_t)s;
  if (v < 0x10000 || v > 0x7FFFFFFFFFFF)
    return false;
  if (!IsSafeRead((void *)v, 4))
    return false;

  __try {
    for (size_t i = 0; i < maxLen; ++i) {
      char c = s[i];
      if (c == '\0')
        break;
      // Allow common IW color codes (^7) and brackets for bindings.
      unsigned char uc = (unsigned char)c;
      bool ok = (uc == '^' || uc == '[' || uc == ']' || uc == '{' || uc == '}' ||
                 uc == '+' || uc == '-' || uc == '_' || uc == ':' || uc == '.' ||
                 uc == ',' || uc == '\'' || uc == '\"' || uc == '/' || uc == '\\' ||
                 uc == '(' || uc == ')' || uc == '!' || uc == '?' || uc == '%' ||
                 uc == '#');
      if (!ok) {
        ok = (uc >= 32 && uc <= 126);
      }
      if (!ok) {
        // Reject strings with control/high bytes early.
        return false;
      }
      out.push_back(c);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }

  if (out.size() < 4)
    return false;
  return true;
}
static bool TryPeekFirstChar(const char *s, char &outC) {
  outC = '\0';
  if (!s)
    return false;
  uintptr_t v = (uintptr_t)s;
  if (v < 0x10000 || v > 0x7FFFFFFFFFFF)
    return false;
  if (!IsSafeRead((void *)v, 1))
    return false;
  __try {
    outC = s[0];
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
static bool LooksLikePressHoldPrompt(const std::string &text) {
  if (text.empty())
    return false;
  std::string cleaned = StripColorCodes(text);
  cleaned = TrimSpaces(cleaned);
  if (cleaned.size() < 4)
    return false;
  std::string upper = ToUpperAscii(cleaned);
  if (upper.rfind("PRESS", 0) == 0)
    return true;
  if (upper.rfind("HOLD", 0) == 0)
    return true;
  if (upper.rfind("TAP", 0) == 0)
    return true;
  if (upper.rfind("USE", 0) == 0)
    return true;
  // Some runtime variants prepend markers or include composite wording
  // ("Press and Hold ... to ..."). Treat these as prompts too.
  const bool hasVerb = (upper.find("PRESS ") != std::string::npos ||
                        upper.find("HOLD ") != std::string::npos ||
                        upper.find("TAP ") != std::string::npos ||
                        upper.find("USE ") != std::string::npos);
  if (hasVerb) {
    if (upper.find(" TO ") != std::string::npos ||
        upper.find("[{+") != std::string::npos ||
        upper.find("&&") != std::string::npos ||
        upper.find("$$") != std::string::npos) {
      return true;
    }
  }
  return false;
}
static bool HasBindingPlaceholderToken(const std::string &text) {
  return (text.find("[{+") != std::string::npos ||
          text.find("&&") != std::string::npos ||
          text.find("$$") != std::string::npos);
}
static bool IsExcludedHudAutoPrefix(const std::string &name) {
  if (name.empty())
    return true;
  return (name.rfind("MENU_", 0) == 0 ||
          name.rfind("LUA_MENU_", 0) == 0 ||
          name.rfind("MP_", 0) == 0 ||
          name.rfind("SUBTITLE_", 0) == 0 ||
          name.find("INTROSCREEN") != std::string::npos);
}
static void MaybeLogHudKeyAuto(const std::string &key, uintptr_t callerOffset,
                               const std::string &reason) {
  if (key.empty() || reason.empty()) {
    return;
  }

  static std::mutex s_autoLogMtx;
  static std::unordered_map<std::string, DWORD> s_autoLogTick;
  const DWORD now = GetTickCount();
  const std::string sig = key + "|" + std::to_string((unsigned long long)callerOffset) +
                          "|" + reason;
  bool shouldLog = false;
  {
    std::lock_guard<std::mutex> lk(s_autoLogMtx);
    auto it = s_autoLogTick.find(sig);
    if (it == s_autoLogTick.end() || (now - it->second) > 2500) {
      s_autoLogTick[sig] = now;
      shouldLog = true;
    }
    if (s_autoLogTick.size() > 512) {
      for (auto it = s_autoLogTick.begin(); it != s_autoLogTick.end();) {
        if ((now - it->second) > 30000) {
          it = s_autoLogTick.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  if (shouldLog) {
    char buf[512];
    sprintf_s(buf, "[HUD-KEY-AUTO] key=%s caller=+0x%llX reason=%s",
              key.c_str(), (unsigned long long)callerOffset, reason.c_str());
    LogToFile(buf);
  }
}
static bool LooksLikeHudLocalizationKey(const std::string &text) {
  if (text.empty())
    return false;
  std::string cleaned = TrimSpaces(StripColorCodes(text));
  if (cleaned.empty())
    return false;
  if (cleaned[0] == '@') {
    cleaned.erase(0, 1);
    cleaned = TrimSpaces(cleaned);
  }
  if (cleaned.size() < 5)
    return false;
  std::string upper = ToUpperAscii(cleaned);
  if (upper.find("INTROSCREEN") != std::string::npos)
    return false;
  if (upper.rfind("CORNERED_", 0) == 0 || upper.rfind("PLATFORM_", 0) == 0 ||
      upper.rfind("HINT_", 0) == 0 || upper.rfind("SCRIPT_HINT_", 0) == 0 ||
      upper.rfind("SCRIPT_PLATFORM_HINT_", 0) == 0 ||
      upper.rfind("GAME_OBJECTIVE", 0) == 0 ||
      upper.rfind("CGAME_OBJECTIVE", 0) == 0 ||
      upper.find("_HINT") != std::string::npos ||
      upper.find("_OBJ_") != std::string::npos ||
      upper.find("_OBJECTIVE_") != std::string::npos ||
      HasObjectiveSuffixOnlyKey(upper)) {
    return true;
  }

  // Generic key-shape fallback: allows mission-specific keys (e.g.
  // DEER_HUNT_*, JUNGLE_*) without hardcoded prefixes.
  if (upper.size() >= 6 && upper.size() <= 96 &&
      upper.find('_') != std::string::npos &&
      upper.find_first_of(" \t\r\n") == std::string::npos) {
    int alphaCount = 0;
    for (char c : upper) {
      const bool isAlpha = (c >= 'A' && c <= 'Z');
      const bool isDigit = (c >= '0' && c <= '9');
      if (isAlpha) {
        ++alphaCount;
      }
      if (!(isAlpha || isDigit || c == '_' || c == '.')) {
        return false;
      }
    }
    if (alphaCount >= 3) {
      return true;
    }
  }

  return false;
}
