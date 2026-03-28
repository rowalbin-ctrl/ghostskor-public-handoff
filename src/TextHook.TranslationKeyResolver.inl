static const char *StoreOverlayTranslatedString(const std::string &translated) {
  int idx = g_TranslatedIndex.fetch_add(1) % MAX_TRANSLATED_STRINGS;
  strncpy(g_TranslatedStrings[idx], translated.c_str(), MAX_TRANSLATED_LEN - 1);
  g_TranslatedStrings[idx][MAX_TRANSLATED_LEN - 1] = '\0';
  return g_TranslatedStrings[idx];
}
static std::string ToUpperOverlay(const std::string &s) {
  return ToUpperAscii(s);
}
static std::string StripColorCodesOverlay(const std::string &s) {
  return StripColorCodes(s);
}
static std::string StripLookupControlChars(const std::string &s) {
  if (s.empty()) {
    return s;
  }
  std::string out;
  out.reserve(s.size());
  for (unsigned char uc : s) {
    if (uc < 0x20) {
      if (uc == '\n' || uc == '\r' || uc == '\t') {
        out.push_back((char)uc);
      }
      continue;
    }
    out.push_back((char)uc);
  }
  return out;
}
static std::string NormalizeBindingPlaceholderVariants(const std::string &s) {
  if (s.empty()) {
    return s;
  }
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if ((i + 2) < s.size() && s[i] == '$' && s[i + 1] == '$' &&
        std::isdigit((unsigned char)s[i + 2])) {
      out.push_back('&');
      out.push_back('&');
      i += 2;
      while (i < s.size() && std::isdigit((unsigned char)s[i])) {
        out.push_back(s[i]);
        ++i;
      }
      if (i < s.size()) {
        --i;
      }
      continue;
    }
    out.push_back(s[i]);
  }
  return out;
}
static std::string CanonicalizeRuntimeEnglish(const std::string &s) {
  std::string clean =
      NormalizeBindingPlaceholderVariants(
          StripLookupControlChars(StripColorCodesOverlay(s)));
  return ToUpperOverlay(NormalizeWhitespaceAscii(clean, true, true));
}
static std::string StripTrailingPunctuationToken(const std::string &canonical) {
  std::string out = canonical;
  while (!out.empty()) {
    char c = out.back();
    if (c == '.' || c == '!' || c == '?' || c == ';' || c == ':') {
      out.pop_back();
      continue;
    }
    break;
  }
  if (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}
static bool ResolveRuntimeEnglishKeyInternal(const std::string &english,
                                             std::string &outKey) {
  std::string canonical = CanonicalizeRuntimeEnglish(english);
  if (canonical.empty()) {
    return false;
  }

  std::shared_lock<std::shared_mutex> lock(g_RuntimeEnglishMutex);

  auto it = g_RuntimeEnglishToKey.find(canonical);
  if (it != g_RuntimeEnglishToKey.end() && !it->second.empty()) {
    outKey = it->second;
    return true;
  }

  std::string loose = StripTrailingPunctuationToken(canonical);
  if (!loose.empty() && loose != canonical) {
    it = g_RuntimeEnglishToKey.find(loose);
    if (it != g_RuntimeEnglishToKey.end() && !it->second.empty()) {
      outKey = it->second;
      return true;
    }
  }

  size_t bestLen = 0;
  for (const auto &kv : g_RuntimeBindingTemplateToKey) {
    const std::string &sig = kv.first;
    size_t sep = sig.find('\x1F');
    if (sep == std::string::npos) {
      continue;
    }
    // Use direct ranges instead of substr() to avoid 2 heap allocations
    // per iteration — critical for failed-resolve hot paths (weapon names).
    const size_t beforeLen = sep;
    const size_t afterStart = sep + 1;
    const size_t afterLen = sig.size() - afterStart;
    if (beforeLen > 0 &&
        (canonical.size() < beforeLen ||
         canonical.compare(0, beforeLen, sig, 0, beforeLen) != 0)) {
      continue;
    }
    const size_t searchFrom = beforeLen;
    size_t posAfter = std::string::npos;
    if (afterLen == 0) {
      posAfter = canonical.size();
    } else {
      posAfter =
          canonical.find(sig.c_str() + afterStart, searchFrom, afterLen);
      if (posAfter == std::string::npos) {
        continue;
      }
    }
    if (posAfter < searchFrom) {
      continue;
    }
    const size_t dynamicLen = posAfter - searchFrom;
    if (dynamicLen > 40) {
      continue;
    }
    const size_t score = beforeLen + afterLen;
    if (score > bestLen && !kv.second.empty()) {
      bestLen = score;
      outKey = kv.second;
    }
  }

  return !outKey.empty();
}

// Register overlay translation (called from FSHook)
extern "C" void TextHook_RegisterOverlay(const char *english,
                                         const char *korean) {
  if (!english || !korean)
    return;
  std::string engClean = StripColorCodesOverlay(std::string(english));
  std::string engUpper = ToUpperOverlay(engClean);
  if (engUpper.empty())
    return;

  std::unique_lock<std::shared_mutex> lock(g_OverlayMapMutex);
  if (g_EngToKorOverlayMap.find(engUpper) == g_EngToKorOverlayMap.end()) {
    g_EngToKorOverlayMap[engUpper] = korean;
  }
}
void TextHook_RegisterRuntimeEnglishKey(const char *key,
                                        const char *englishValue) {
  if (!key || !englishValue) {
    return;
  }
  std::string keyStr = TrimSpaces(std::string(key));
  std::string rawEng = TrimSpaces(std::string(englishValue));
  if (keyStr.empty() || rawEng.empty()) {
    return;
  }

  std::string canonical = CanonicalizeRuntimeEnglish(rawEng);
  if (canonical.empty()) {
    return;
  }
  const DWORD now = GetTickCount();
  std::string loose = StripTrailingPunctuationToken(canonical);
  std::string upperRaw = ToUpperOverlay(StripColorCodesOverlay(rawEng));
  auto hasBindingPlaceholder = [](const std::string &text) -> bool {
    return text.find("[{+") != std::string::npos ||
           text.find('{') != std::string::npos ||
           text.find("&&") != std::string::npos ||
           text.find("$$") != std::string::npos;
  };
  const bool incomingHasBindingPlaceholder = hasBindingPlaceholder(rawEng);

  std::unique_lock<std::shared_mutex> lock(g_RuntimeEnglishMutex);

  auto remember = [&](const std::string &canon) {
    if (canon.empty()) {
      return;
    }
    auto it = g_RuntimeEnglishToKey.find(canon);
    if (it == g_RuntimeEnglishToKey.end() || it->second.empty()) {
      g_RuntimeEnglishToKey[canon] = keyStr;
    }
  };

  remember(canonical);
  if (!loose.empty() && loose != canonical) {
    remember(loose);
  }
  auto itExistingEng = g_RuntimeKeyToEnglish.find(keyStr);
  const bool keepExistingResolvedEnglish =
      incomingHasBindingPlaceholder &&
      itExistingEng != g_RuntimeKeyToEnglish.end() &&
      !itExistingEng->second.empty() &&
      !hasBindingPlaceholder(itExistingEng->second);
  if (!keepExistingResolvedEnglish) {
    g_RuntimeKeyToEnglish[keyStr] = rawEng;
  }
  g_RuntimeKeyLastSeenTick[keyStr] = now;
  if (g_RuntimeKeyLastSeenTick.size() > 2048) {
    for (auto it = g_RuntimeKeyLastSeenTick.begin();
         it != g_RuntimeKeyLastSeenTick.end();) {
      const DWORD seen = it->second;
      if (now >= seen && (now - seen) > 45000) {
        it = g_RuntimeKeyLastSeenTick.erase(it);
      } else {
        ++it;
      }
    }
  }

  size_t bindStart = upperRaw.find("[{+");
  while (bindStart != std::string::npos) {
    size_t bindEnd = upperRaw.find("}]", bindStart + 3);
    if (bindEnd == std::string::npos) {
      break;
    }

    std::string before = CanonicalizeRuntimeEnglish(upperRaw.substr(0, bindStart));
    std::string after =
        CanonicalizeRuntimeEnglish(upperRaw.substr(bindEnd + 2));
    std::string sig = before;
    sig.push_back('\x1F');
    sig += after;
    if (!before.empty() || !after.empty()) {
      auto itT = g_RuntimeBindingTemplateToKey.find(sig);
      if (itT == g_RuntimeBindingTemplateToKey.end() || itT->second.empty()) {
        g_RuntimeBindingTemplateToKey[sig] = keyStr;
      }
    }

    bindStart = upperRaw.find("[{+", bindEnd + 2);
  }
  bindStart = upperRaw.find('{');
  while (bindStart != std::string::npos) {
    if (bindStart > 0 && upperRaw[bindStart - 1] == '[') {
      bindStart = upperRaw.find('{', bindStart + 1);
      continue;
    }
    size_t bindEnd = upperRaw.find('}', bindStart + 1);
    if (bindEnd == std::string::npos) {
      break;
    }

    std::string before = CanonicalizeRuntimeEnglish(upperRaw.substr(0, bindStart));
    std::string after =
        CanonicalizeRuntimeEnglish(upperRaw.substr(bindEnd + 1));
    std::string sig = before;
    sig.push_back('\x1F');
    sig += after;
    if (!before.empty() || !after.empty()) {
      auto itT = g_RuntimeBindingTemplateToKey.find(sig);
      if (itT == g_RuntimeBindingTemplateToKey.end() || itT->second.empty()) {
        g_RuntimeBindingTemplateToKey[sig] = keyStr;
      }
    }

    bindStart = upperRaw.find('{', bindEnd + 1);
  }
}

bool TextHook_GetRuntimeEnglishForKey(const std::string &key,
                                      std::string &outEnglish) {
  outEnglish.clear();
  if (key.empty()) {
    return false;
  }
  std::shared_lock<std::shared_mutex> lock(g_RuntimeEnglishMutex);
  auto it = g_RuntimeKeyToEnglish.find(key);
  if (it == g_RuntimeKeyToEnglish.end() || it->second.empty()) {
    return false;
  }
  outEnglish = it->second;
  return true;
}

bool TextHook_GetRuntimeEnglishForKeyRecent(const std::string &key,
                                            std::string &outEnglish,
                                            unsigned long maxAgeMs) {
  outEnglish.clear();
  if (key.empty()) {
    return false;
  }
  const DWORD now = GetTickCount();
  std::shared_lock<std::shared_mutex> lock(g_RuntimeEnglishMutex);
  auto itEng = g_RuntimeKeyToEnglish.find(key);
  if (itEng == g_RuntimeKeyToEnglish.end() || itEng->second.empty()) {
    return false;
  }
  auto itTick = g_RuntimeKeyLastSeenTick.find(key);
  if (itTick == g_RuntimeKeyLastSeenTick.end()) {
    return false;
  }
  const DWORD seen = itTick->second;
  if (seen == 0 || now < seen || (now - seen) > maxAgeMs) {
    return false;
  }
  outEnglish = itEng->second;
  return true;
}

// Phase 6: Find Korean translation — primary path is g_EnglishToKey → g_KeyToKorean.
// Falls back to g_EngToKorOverlayMap (append-only runtime cache) for variants
// not in the startup-loaded chain.
static bool FindLocalizedTemplatePlaceholder(const std::string &text,
                                             size_t from,
                                             size_t &outStart,
                                             size_t &outEnd) {
  outStart = std::string::npos;
  outEnd = std::string::npos;
  size_t pAmp = text.find("&&", from);
  size_t pDol = text.find("$$", from);
  size_t p = std::string::npos;
  if (pAmp == std::string::npos) {
    p = pDol;
  } else if (pDol == std::string::npos) {
    p = pAmp;
  } else {
    p = (pAmp < pDol) ? pAmp : pDol;
  }
  if (p == std::string::npos) {
    return false;
  }

  size_t end = p + 2;
  while (end < text.size() &&
         std::isdigit((unsigned char)text[end])) {
    ++end;
  }
  if (end <= (p + 2)) {
    return false;
  }

  outStart = p;
  outEnd = end;
  return true;
}

static std::string ResolveLocalizedTemplateArgumentFragment(
    const std::string &fragment) {
  std::string clean = TrimSpaces(StripLookupControlChars(
      StripColorCodesOverlay(fragment)));
  if (clean.empty()) {
    return clean;
  }

  std::string dynamicKey;
  const std::string upper = ToUpperOverlay(clean);
  const bool allowGameplayBindingFallback =
      !TextHook_IsPauseMenuLikely() && !TextHook_IsFrontendMenuLikely();
  const bool bindingLikeFragment =
      (upper.find("MOUSE") != std::string::npos ||
       upper.find(" OR ") != std::string::npos || upper == "SPACE" ||
       upper == "CTRL" || upper == "SHIFT" || upper == "ALT" ||
       upper == "TAB" || upper == "ENTER" || upper == "ESC");
  if (allowGameplayBindingFallback && bindingLikeFragment) {
    std::string inferredBinding =
        BindingResolver::ExtractKeyFromEnglish("Press " + clean + " to");
    if (!inferredBinding.empty()) {
      return inferredBinding;
    }
  }

  auto itDirect = g_EnglishToKey.find(upper);
  if (itDirect != g_EnglishToKey.end()) {
    dynamicKey = itDirect->second;
  } else {
    ResolveRuntimeEnglishKeyInternal(clean, dynamicKey);
  }

  if (!dynamicKey.empty()) {
    auto itKor = g_KeyToKorean.find(dynamicKey);
    if (itKor != g_KeyToKorean.end() && !itKor->second.empty()) {
      return itKor->second;
    }
  }

  return clean;
}

bool TextHook_ResolveLocalizedTemplateArgs(const std::string &key,
                                           const std::string &runtimeEnglish,
                                           std::string &inOutText) {
  if (key.empty() || runtimeEnglish.empty() || inOutText.empty()) {
    return false;
  }
  if (inOutText.find("&&") == std::string::npos &&
      inOutText.find("$$") == std::string::npos) {
    return false;
  }

  auto itEng = g_KeyToEnglish.find(key);
  if (itEng == g_KeyToEnglish.end() || itEng->second.empty()) {
    return false;
  }

  const std::string templateEnglish =
      StripLookupControlChars(StripColorCodesOverlay(itEng->second));
  const std::string runtimeText =
      StripLookupControlChars(StripColorCodesOverlay(runtimeEnglish));
  if (templateEnglish.empty() || runtimeText.empty()) {
    return false;
  }

  std::unordered_map<std::string, std::string> replacements;
  size_t templatePos = 0;
  size_t runtimePos = 0;
  while (templatePos < templateEnglish.size()) {
    size_t tokenStart = std::string::npos;
    size_t tokenEnd = std::string::npos;
    if (!FindLocalizedTemplatePlaceholder(templateEnglish, templatePos,
                                          tokenStart, tokenEnd)) {
      break;
    }

    const std::string prefix =
        templateEnglish.substr(templatePos, tokenStart - templatePos);
    if (!prefix.empty()) {
      size_t prefixPos = runtimeText.find(prefix, runtimePos);
      if (prefixPos == std::string::npos) {
        break;
      }
      runtimePos = prefixPos + prefix.size();
    }

    size_t nextTokenStart = std::string::npos;
    size_t nextTokenEnd = std::string::npos;
    const bool hasNext = FindLocalizedTemplatePlaceholder(
        templateEnglish, tokenEnd, nextTokenStart, nextTokenEnd);
    const std::string suffix =
        hasNext ? templateEnglish.substr(tokenEnd, nextTokenStart - tokenEnd)
                : templateEnglish.substr(tokenEnd);
    const std::string token =
        templateEnglish.substr(tokenStart, tokenEnd - tokenStart);

    size_t captureEnd = runtimeText.size();
    if (!suffix.empty()) {
      captureEnd = runtimeText.find(suffix, runtimePos);
      if (captureEnd == std::string::npos || captureEnd < runtimePos) {
        break;
      }
    }

    std::string fragment = runtimeText.substr(runtimePos, captureEnd - runtimePos);
    fragment = ResolveLocalizedTemplateArgumentFragment(fragment);
    if (!fragment.empty()) {
      replacements[token] = fragment;
    }

    runtimePos = captureEnd;
    templatePos = tokenEnd;
  }

  if (replacements.empty()) {
    return false;
  }

  bool changed = false;
  for (const auto &kv : replacements) {
    const std::string &token = kv.first;
    const std::string &replacement = kv.second;
    size_t pos = 0;
    while ((pos = inOutText.find(token, pos)) != std::string::npos) {
      inOutText.replace(pos, token.size(), replacement);
      pos += replacement.size();
      changed = true;
    }
  }

  return changed;
}

static const char *FindOverlayTranslation(const char *english) {
  if (!english)
    return nullptr;
  std::string engClean = StripColorCodesOverlay(std::string(english));
  std::string engUpper = ToUpperOverlay(engClean);
  if (engUpper.empty())
    return nullptr;

  // Primary path: g_EnglishToKey → g_KeyToKorean (startup-loaded, read-only)
  {
    auto itKey = g_EnglishToKey.find(engUpper);
    if (itKey != g_EnglishToKey.end()) {
      auto itKor = g_KeyToKorean.find(itKey->second);
      if (itKor != g_KeyToKorean.end() && !itKor->second.empty()) {
        return StoreOverlayTranslatedString(itKor->second);
      }
    }
  }

  // Runtime key lookup: use observed LOCALIZE key↔english pairs.
  // This avoids text-specific hardcoding for mission/esc/object prompts.
  std::string runtimeKey;
  if (ResolveRuntimeEnglishKeyInternal(engClean, runtimeKey)) {
    auto itKor = g_KeyToKorean.find(runtimeKey);
    if (itKor != g_KeyToKorean.end() && !itKor->second.empty()) {
      return StoreOverlayTranslatedString(itKor->second);
    }
  }

  // Fallback: g_EngToKorOverlayMap (runtime-discovered variants, append-only)
  {
    std::shared_lock<std::shared_mutex> lock(g_OverlayMapMutex);
    auto it = g_EngToKorOverlayMap.find(engUpper);
    if (it != g_EngToKorOverlayMap.end()) {
      return StoreOverlayTranslatedString(it->second);
    }
  }
  return nullptr;
}
static void EnsureTranslationsLoaded() {
  // call_once provides thread-safety if multiple hooks race to load JSON.
  std::call_once(g_LoadOnce, LoadTranslations);
}

// Helper: To Upper

static std::string ToUpper(const std::string &s) {
  return ToUpperAscii(s);
}

// Helper: Normalize string for matching (removes \n, \r, collapses spaces,
// trims)
static std::string NormalizeForMatching(const std::string &s) {
  return NormalizeWhitespaceAscii(s, false, false);
}
static std::string NormalizeEnglishKey(const std::string &s) {
  std::string clean = StripColorCodes(s);
  std::string norm = NormalizeForMatching(clean);
  return ToUpper(norm);
}
static bool ResolveKeyFromEnglishInternal(const std::string &english,
                                          std::string &outKey) {
  if (english.empty())
    return false;

  // Per-text result cache: menu text is the same every frame, so cache the
  // resolution result to avoid 6+ string allocations and 3+ map lookups per
  // call.  Cache is cleared every 2 seconds or when it exceeds 512 entries.
  {
    struct CachedResolve {
      std::string key;
      bool found;
    };
    static std::unordered_map<std::string, CachedResolve> s_resolveCache;
    static DWORD s_resolveCacheTick = 0;
    DWORD now = GetTickCount();
    if ((now - s_resolveCacheTick) > 2000 || s_resolveCache.size() > 512) {
      s_resolveCache.clear();
      s_resolveCacheTick = now;
    }
    auto cit = s_resolveCache.find(english);
    if (cit != s_resolveCache.end()) {
      if (cit->second.found) {
        outKey = cit->second.key;
        return true;
      }
      return false;
    }
    // Cache miss — compute below, then store result before returning.
    // We use a lambda-style approach: store outKey at every return path.
    // To avoid modifying every return, we wrap the rest in a helper lambda.
    auto resolveAndCache = [&]() -> bool {
  std::string cleaned =
      NormalizeBindingPlaceholderVariants(
          StripLookupControlChars(StripColorCodes(english)));
  std::string cleanedTrim = TrimSpaces(cleaned);

  // Direct key match (string_t may already be a key)
  auto itKey = g_KeyToKorean.find(cleanedTrim);
  if (itKey != g_KeyToKorean.end()) {
    outKey = cleanedTrim;
    return true;
  }
  if (!cleanedTrim.empty() && cleanedTrim[0] == '@') {
    std::string keyStr = cleanedTrim.substr(1);
    auto itKey2 = g_KeyToKorean.find(keyStr);
    if (itKey2 != g_KeyToKorean.end()) {
      outKey = keyStr;
      return true;
    }
  }
  std::string upper = ToUpper(cleanedTrim.empty() ? cleaned : cleanedTrim);
  auto it = g_EnglishToKey.find(upper);
  if (it != g_EnglishToKey.end()) {
    outKey = it->second;
    return true;
  }

  std::string norm = ToUpper(NormalizeForMatching(cleaned));
  if (norm != upper) {
    it = g_EnglishToKey.find(norm);
    if (it != g_EnglishToKey.end()) {
      outKey = it->second;
      return true;
    }
  }

  // Runtime key lookup: LOCALIZE key↔english pairs observed from DB path.
  const bool allowGameplayEnglishFallback =
      !TextHook_IsPauseMenuLikely() && !TextHook_IsFrontendMenuLikely();
  const auto looksLikeThrowBackPrompt = [](const std::string &probe) -> bool {
    return probe.find("THROW BACK") != std::string::npos;
  };
  if (allowGameplayEnglishFallback &&
      (looksLikeThrowBackPrompt(upper) || looksLikeThrowBackPrompt(norm))) {
    outKey = "PLATFORM_THROWBACKGRENADE";
    return true;
  }

  const auto looksLikeNoAmmoPrompt = [](const std::string &probe) -> bool {
    return (probe.rfind("NO ", 0) == 0 &&
            probe.find(" AMMO REMAINING") != std::string::npos);
  };
  if (allowGameplayEnglishFallback &&
      (looksLikeNoAmmoPrompt(upper) || looksLikeNoAmmoPrompt(norm))) {
    outKey = "WEAPON_NO_WEAPON_AMMO";
    return true;
  }

  if (ResolveRuntimeEnglishKeyInternal(cleaned, outKey)) {
    return true;
  }

  // Binding template matching: handles [{+command}] ??actual key name
  // e.g., "PRESS E TO TOGGLE ZOOM." matches template before="PRESS " after=" TO TOGGLE ZOOM."
  {
    size_t bestMatchLen = 0;
    for (const auto &tmpl : g_BindingTemplates) {
      bool beforeOk = tmpl.before.empty() ||
                      upper.rfind(tmpl.before, 0) == 0 ||
                      norm.rfind(tmpl.before, 0) == 0;
      if (!beforeOk) continue;
      bool afterOk = tmpl.after.empty();
      if (!afterOk) {
        // Allow composite prompts (e.g., "PRESS E TO ..., HOLD ...").
        // The dynamic placeholder gap should stay short.
        auto MatchAfter = [&](const std::string &haystack,
                              const std::string &needle) -> bool {
          if (needle.empty()) {
            return true;
          }
          size_t searchFrom = tmpl.before.size();
          size_t afterPos = haystack.find(needle, searchFrom);
          return (afterPos != std::string::npos &&
                  afterPos >= tmpl.before.size() &&
                  (afterPos - tmpl.before.size()) <= 40);
        };

        if (MatchAfter(upper, tmpl.after) || MatchAfter(norm, tmpl.after)) {
          afterOk = true;
        } else {
          // Runtime variants often drop trailing punctuation.
          const std::string looseNeedle = StripTrailingPunctuationToken(tmpl.after);
          if (!looseNeedle.empty() && looseNeedle != tmpl.after) {
            if (MatchAfter(upper, looseNeedle) || MatchAfter(norm, looseNeedle)) {
              afterOk = true;
            }
          }
        }
      }
      if (beforeOk && afterOk) {
        size_t matchLen = tmpl.before.size() + tmpl.after.size();
        if (matchLen > bestMatchLen) {
          bestMatchLen = matchLen;
          outKey = tmpl.key;
        }
      }
    }
    if (bestMatchLen > 0) return true;
  }

  // Prefix fallback (long strings)
  size_t bestLen = 0;
  for (const auto &entry : g_PrefixToKey) {
    const std::string &prefix = entry.first;
    if (upper.rfind(prefix, 0) == 0 && prefix.size() > bestLen) {
      bestLen = prefix.size();
      outKey = entry.second;
    } else if (norm.rfind(prefix, 0) == 0 && prefix.size() > bestLen) {
      bestLen = prefix.size();
      outKey = entry.second;
    }
  }
  return bestLen > 0;
    }; // end resolveAndCache lambda
    bool result = resolveAndCache();
    s_resolveCache[english] = {outKey, result};
    return result;
  } // end cache block
}
bool TextHook_ResolveKeyFromEnglish(const std::string &english,
                                    std::string &outKey) {
  return ResolveKeyFromEnglishInternal(english, outKey);
}
bool TextHook_GetLocalizedKeyText(const std::string &key,
                                  std::string &outKorean,
                                  std::string &outEnglish) {
  if (key.empty()) {
    return false;
  }
  EnsureTranslationsLoaded();
  auto itKor = g_KeyToKorean.find(key);
  if (itKor == g_KeyToKorean.end() || itKor->second.empty()) {
    return false;
  }
  outKorean = itKor->second;
  auto itEng = g_KeyToEnglish.find(key);
  outEnglish = (itEng != g_KeyToEnglish.end()) ? itEng->second : key;
  return true;
}

// Load Translations from JSON

static void LoadTranslations() {
  if (g_MapLoaded.load()) {
    return;
  }

  if (!TranslationStore::EnsureLoaded()) {
    LogToFile("[TextHook] ERROR: TranslationStore not ready.");
    return;
  }

  const auto &keyToEnglish = TranslationStore::KeyToEnglish();
  const auto &keyToKorean = TranslationStore::KeyToKorean();

  for (const auto &element : keyToEnglish) {
    const std::string &key = element.first;
    const std::string &val = element.second;

    if (val.empty()) {
      continue;
    }

    g_KeyToEnglish[key] = val;
    g_EnglishToKey[ToUpper(val)] = key;

    std::string normKey = NormalizeEnglishKey(val);
    if (!normKey.empty() && g_EnglishToKey.find(normKey) == g_EnglishToKey.end()) {
      g_EnglishToKey[normKey] = key;
    }

    if (val.size() >= 30) {
      std::vector<size_t> prefixLengths = {30, 35, 40, 45, 50, 55, 60};

      for (size_t targetLen : prefixLengths) {
        if (targetLen >= val.size()) {
          continue;
        }

        size_t maxSearch = (std::min)(targetLen + 3, val.size());
        size_t spacePos = val.rfind(' ', maxSearch);

        size_t prefixLen = targetLen;
        if (spacePos != std::string::npos && spacePos > targetLen - 10) {
          prefixLen = spacePos + 1;
        }

        if (prefixLen > 0 && prefixLen < val.size()) {
          std::string prefix = val.substr(0, prefixLen);
          g_PrefixToKey[ToUpper(prefix)] = key;

          std::string suffix = val.substr(prefixLen);
          size_t start = suffix.find_first_not_of(' ');
          if (start != std::string::npos && start > 0) {
            suffix = suffix.substr(start);
          }
          if (!suffix.empty() && suffix.size() >= 3) {
            g_SuffixToSuppress.insert(ToUpper(suffix));
          }
        }
      }
    }

    const bool colonTailPrefix =
        (val.size() >= 2 &&
         val.compare(val.size() - 2, 2, ": ") == 0) ||
        (!val.empty() && val.back() == ':');
    if (val.size() >= 8 && val.size() <= 64 && colonTailPrefix) {
      std::string prefix = ToUpper(val);
      if (g_PrefixToKey.find(prefix) == g_PrefixToKey.end()) {
        g_PrefixToKey[prefix] = key;
      }
      std::string normPrefix = NormalizeEnglishKey(val);
      if (!normPrefix.empty() &&
          g_PrefixToKey.find(normPrefix) == g_PrefixToKey.end()) {
        g_PrefixToKey[normPrefix] = key;
      }
    }

    size_t nlPos = std::string::npos;
    size_t literalN = val.find("\\n");
    size_t literalR = val.find("\\r");
    size_t realN = val.find('\n');
    size_t realR = val.find('\r');

    if (literalN != std::string::npos)
      nlPos = literalN;
    if (literalR != std::string::npos &&
        (nlPos == std::string::npos || literalR < nlPos))
      nlPos = literalR;
    if (realN != std::string::npos &&
        (nlPos == std::string::npos || realN < nlPos))
      nlPos = realN;
    if (realR != std::string::npos &&
        (nlPos == std::string::npos || realR < nlPos))
      nlPos = realR;

    if (nlPos != std::string::npos && nlPos > 0) {
      std::string firstLine = val.substr(0, nlPos);
      if (!firstLine.empty()) {
        g_EnglishToKey[ToUpper(firstLine)] = key;
      }
    }
  }

  g_BindingTemplates.clear();
  std::unordered_set<std::string> tmplSeen;

  for (const auto &kv : g_KeyToEnglish) {
    const std::string &engVal = kv.second;
    if (engVal.empty()) {
      continue;
    }

    auto AddTemplate = [&](size_t tokenStart, size_t tokenEnd) {
      if (tokenEnd <= tokenStart || tokenStart > engVal.size() ||
          tokenEnd > engVal.size()) {
        return;
      }
      std::string rawBefore = engVal.substr(0, tokenStart);
      std::string rawAfter = engVal.substr(tokenEnd);
      std::string before = TrimSpaces(ToUpper(StripColorCodes(rawBefore)));
      std::string after = TrimSpaces(ToUpper(StripColorCodes(rawAfter)));
      if (before.size() < 2 && after.size() < 2) {
        return;
      }

      std::string sig = kv.first + "|" + before + "|" + after;
      if (!tmplSeen.insert(sig).second) {
        return;
      }

      BindingTemplate tmpl;
      tmpl.before = before;
      tmpl.after = after;
      tmpl.key = kv.first;
      g_BindingTemplates.push_back(tmpl);
    };

    size_t searchPos = 0;
    while (true) {
      size_t bindStart = engVal.find("[{+", searchPos);
      if (bindStart == std::string::npos)
        break;
      size_t bindEnd = engVal.find("}]", bindStart);
      if (bindEnd == std::string::npos)
        break;
      AddTemplate(bindStart, bindEnd + 2);
      searchPos = bindEnd + 2;
    }

    searchPos = 0;
    while (true) {
      size_t bindStart = engVal.find('{', searchPos);
      if (bindStart == std::string::npos)
        break;
      if (bindStart > 0 && engVal[bindStart - 1] == '[') {
        searchPos = bindStart + 1;
        continue;
      }
      size_t bindEnd = engVal.find('}', bindStart + 1);
      if (bindEnd == std::string::npos)
        break;
      AddTemplate(bindStart, bindEnd + 1);
      searchPos = bindEnd + 1;
    }

    searchPos = 0;
    while (true) {
      size_t ampPos = engVal.find("&&", searchPos);
      if (ampPos == std::string::npos)
        break;
      size_t digitPos = ampPos + 2;
      while (digitPos < engVal.size() &&
             std::isdigit((unsigned char)engVal[digitPos])) {
        digitPos++;
      }
      if (digitPos > (ampPos + 2)) {
        AddTemplate(ampPos, digitPos);
        searchPos = digitPos;
      } else {
        searchPos = ampPos + 2;
      }
    }
  }

  LogToFile("[TextHook] Loaded " + std::to_string(g_EnglishToKey.size()) +
            " English keys, " + std::to_string(g_BindingTemplates.size()) +
            " binding templates.");

  for (const auto &element : keyToKorean) {
    g_KeyToKorean[element.first] = element.second;
  }

  LogToFile("[TextHook] Loaded " + std::to_string(g_KeyToKorean.size()) +
            " Korean keys.");

  {
    int videoSubPreloaded = 0;

    for (const auto &korEntry : g_KeyToKorean) {
      const std::string &key = korEntry.first;
      const std::string &korean = korEntry.second;

      if (key.find("VIDSUBTITLES_") != 0 &&
          key.find("INTROSCREEN") == std::string::npos) {
        continue;
      }

      auto engIt = g_KeyToEnglish.find(key);
      if (engIt == g_KeyToEnglish.end() || engIt->second.empty()) {
        continue;
      }

      g_EngToKorOverlayMap[ToUpper(engIt->second)] = korean;
      videoSubPreloaded++;
    }

    LogToFile("[TextHook] Pre-loaded " + std::to_string(videoSubPreloaded) +
              " video subtitle translations into overlay map.");
  }

  g_MapLoaded = true;
}

// Helper: Strip Quake-style color codes (^1, ^2, etc.)
static std::string StripColorCodes(const std::string &s) {
  std::string ret;
  ret.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '^' && i + 1 < s.size()) {
      char next = s[i + 1];
      // Check for color codes 0-9, a-z, A-Z (COD specific)
      if ((next >= '0' && next <= '9') || (next >= 'a' && next <= 'z') ||
          (next >= 'A' && next <= 'Z')) {
        i++; // Skip both caret and code
        continue;
      }
    }
    ret.push_back(s[i]);
  }
  return ret;
}

// Helper: Check if string contains actual newline character
static bool ContainsNewline(const std::string &s) {
  for (char c : s) {
    if (c == '\n' || c == '\r')
      return true;
  }
  return false;
}

// Helper: Check if string contains Korean characters (UTF-8)
// Korean Hangul range in UTF-8:
// - Hangul Syllables: U+AC00 ~ U+D7AF (3-byte UTF-8: 0xEA 0xB0 0x80 ~ 0xED 0x9E
// 0xAF)
// - Hangul Jamo: U+1100 ~ U+11FF
// - Hangul Compatibility Jamo: U+3130 ~ U+318F
static bool ContainsKorean(const char *text) {
  if (!text)
    return false;

  const unsigned char *p = (const unsigned char *)text;
  while (*p) {
    // Check for 3-byte UTF-8 sequences (most Korean falls here)
    if ((*p & 0xF0) == 0xE0) {
      // 3-byte UTF-8
      if (p[1] && p[2]) {
        // Decode to check range
        unsigned int cp =
            ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        // Hangul Syllables: U+AC00 ~ U+D7AF
        if (cp >= 0xAC00 && cp <= 0xD7AF)
          return true;
        // Hangul Jamo Extended: U+1100 ~ U+11FF (actually 3-byte in some
        // encodings) Hangul Compatibility Jamo: U+3130 ~ U+318F
        if (cp >= 0x3130 && cp <= 0x318F)
          return true;
        p += 3;
        continue;
      }
    }
    p++;
  }
  return false;
}

// Helper: Split string by \n or \r and return first non-empty line
static std::string GetFirstLine(const std::string &s) {
  // Find the first occurrence of either \n or \r
  size_t posN = s.find('\n');
  size_t posR = s.find('\r');

  size_t pos = std::string::npos;
  if (posN != std::string::npos && posR != std::string::npos) {
    pos = (posN < posR) ? posN : posR; // Use the earlier one
  } else if (posN != std::string::npos) {
    pos = posN;
  } else if (posR != std::string::npos) {
    pos = posR;
  }

  if (pos != std::string::npos && pos > 0) {
    return s.substr(0, pos);
  }
  return s;
}

// Helper: Remove \n and \r from string (for rendering)
static std::string StripNewlines(const std::string &s) {
  std::string result;
  result.reserve(s.size());
  for (char c : s) {
    if (c != '\n' && c != '\r') {
      result.push_back(c);
    }
  }
  return result;
}

// Find translation (returns nullptr if not found)
// IMPORTANT: Original lookup FIRST, then normalized fallback.
// This preserves "ESC ", "F1 " etc. which have trailing spaces.
static const char *FindTranslation(const char *english) {
  if (!english)
    return nullptr;

  // Lazy load
  EnsureTranslationsLoaded();

  std::string raw(english);
  std::string clean = StripColorCodes(raw);
  std::string upperEng = ToUpper(clean);
  const bool hasLineBreak =
      (clean.find('\n') != std::string::npos ||
       clean.find('\r') != std::string::npos);

  // 1. ORIGINAL lookup first (preserves trailing spaces, exact match)
  auto itKey = g_EnglishToKey.find(upperEng);
  if (itKey != g_EnglishToKey.end()) {
    std::string key = itKey->second;
    auto itKor = g_KeyToKorean.find(key);
    if (itKor != g_KeyToKorean.end()) {
      return itKor->second.c_str();
    }
  }

  // 2. Direct key lookup
  auto itDirect = g_KeyToKorean.find(clean);
  if (itDirect != g_KeyToKorean.end()) {
    return itDirect->second.c_str();
  }

  auto itDirectUpper = g_KeyToKorean.find(upperEng);
  if (itDirectUpper != g_KeyToKorean.end()) {
    return itDirectUpper->second.c_str();
  }

  const std::string upperTrim = ToUpper(TrimSpaces(clean));

  // Rorke Files DEADBOLT pages can arrive as paragraph fragments rather than
  // the full body blob. Route those variants to the canonical body key.
  auto LookupBodyByKey = [](const char *keyName) -> const char * {
    auto it = g_KeyToKorean.find(keyName);
    if (it != g_KeyToKorean.end() && !it->second.empty()) {
      return it->second.c_str();
    }
    return nullptr;
  };
  const bool hasDeadbolt08Id =
      (upperEng.rfind("CIA - SR21945500", 0) == 0);
  const bool looksDeadbolt08WholeBody =
      (hasLineBreak &&
       upperEng.find("A HEAVILY DAMAGED LAPTOP WAS RECOVERED") !=
           std::string::npos &&
       upperEng.find("CALLED \"THE FREEPORT\"") != std::string::npos);
  const bool looksDeadbolt08Body = hasDeadbolt08Id || looksDeadbolt08WholeBody;
  if (looksDeadbolt08Body) {
    if (const char *body08 = LookupBodyByKey("MENU_SP_VF_FILE_08_BODY")) {
      return body08;
    }
  }
  // Long-body fallback: resolve by normalized/prefix key pipeline without
  // touching short fragments. This is critical for VF file bodies that can
  // arrive with runtime spacing differences.
  if (hasLineBreak) {
    std::string resolvedKey;
    if (ResolveKeyFromEnglishInternal(clean, resolvedKey)) {
      auto itResolved = g_KeyToKorean.find(resolvedKey);
      if (itResolved != g_KeyToKorean.end() && !itResolved->second.empty()) {
        return itResolved->second.c_str();
      }
    }
  }

  // 2.5 Normalized direct lookup (collapse spaces/newlines)
  std::string normalized = NormalizeForMatching(clean);
  std::string normalizedUpper = ToUpper(normalized);
  // Save/Quit popup body sometimes arrives with runtime wraps/spaces.
  // Keep this strict (must include CHECKPOINT) to avoid mapping short fragments.
  if (!normalizedUpper.empty()) {
    const std::string quitWarnPrefix =
        "IF YOU QUIT NOW, YOU WILL LOSE ANY PROGRESS";
    if (normalizedUpper.rfind(quitWarnPrefix, 0) == 0 &&
        normalizedUpper.find("CHECKPOINT") != std::string::npos) {
      auto itQuitBody = g_KeyToKorean.find(
          "If you quit now, you will lose any progress since your last checkpoint.");
      if (itQuitBody != g_KeyToKorean.end() && !itQuitBody->second.empty()) {
        return itQuitBody->second.c_str();
      }
    }
  }

  if (normalized != clean) {
    auto itNorm = g_KeyToKorean.find(normalized);
    if (itNorm != g_KeyToKorean.end()) {
      return itNorm->second.c_str();
    }
    auto itNormUpper = g_KeyToKorean.find(normalizedUpper);
    if (itNormUpper != g_KeyToKorean.end()) {
      return itNormUpper->second.c_str();
    }
  }

  // 3. FALLBACK: If string contains \n or \r, try first line only
  // This handles "Resolution\n" -> search "Resolution"
  if (clean.find('\n') != std::string::npos ||
      clean.find('\r') != std::string::npos) {
    std::string firstLine = GetFirstLine(clean);

    if (!firstLine.empty() && firstLine != clean) {
      std::string firstLineUpper = ToUpper(firstLine);

      auto itKeyLine = g_EnglishToKey.find(firstLineUpper);
      if (itKeyLine != g_EnglishToKey.end()) {
        std::string key = itKeyLine->second;
        auto itKor = g_KeyToKorean.find(key);
        if (itKor != g_KeyToKorean.end()) {
          // Return Korean translation
          return itKor->second.c_str();
        }
      }

      // Also try direct key lookup with first line
      auto itDirectLine = g_KeyToKorean.find(firstLine);
      if (itDirectLine != g_KeyToKorean.end()) {
        return itDirectLine->second.c_str();
      }

      auto itDirectLineUpper = g_KeyToKorean.find(firstLineUpper);
      if (itDirectLineUpper != g_KeyToKorean.end()) {
        return itDirectLineUpper->second.c_str();
      }
    }
  }

  // 4. PREFIX MATCHING: For fragmented long texts (e.g., "Rank up, unlock new
  // weapons, perks, ") Game engine splits long descriptions across multiple
  // draw calls
  if (clean.size() >= 20) {
    auto itPrefix = g_PrefixToKey.find(upperEng);
    if (itPrefix != g_PrefixToKey.end()) {
      std::string key = itPrefix->second;
      const bool isBodyKey =
          (key.find("_BODY") != std::string::npos ||
           key.find("_FILE_") != std::string::npos);
      if (!(isBodyKey && !hasLineBreak)) {
        auto itKor = g_KeyToKorean.find(key);
        if (itKor != g_KeyToKorean.end()) {
          return itKor->second.c_str();
        }
      }
    }
  }

  // 5. OVERLAY MAP: For HUD translations from FSHook LOCALIZE
  const char *overlayResult = FindOverlayTranslation(english);
  if (overlayResult) {
    return overlayResult;
  }

  return nullptr;
}
