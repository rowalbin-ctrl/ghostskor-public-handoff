struct OSAuthStatusProducerEvidence {
  ObjectiveTextCapture capture{};
  int laneSlot = 0;
  uint64_t producerInstanceSig = 0;
};

static std::mutex g_OSAuthSnapshotMutex;
static std::unordered_map<uint64_t, ObjectiveTextCapture>
    g_OSAuthObjectiveTextByInstance;
static std::unordered_map<uint64_t, ObjectiveVisualCapture>
    g_OSAuthObjectiveVisualByInstance;
static std::unordered_map<uint64_t, ObjectiveVisualCapture>
    g_OSAuthStatusVisualByInstance;
static std::deque<OSAuthStatusProducerEvidence> g_OSAuthStatusProducerEvents;
static std::vector<ObjectiveStatusNativeLine> g_OSAuthSnapshotLines;
static std::atomic<unsigned int> g_OSAuthSeq{1};
static unsigned long g_OSAuthLastSnapshotTick = 0;
static unsigned long g_OSAuthLastResetTickSeen = 0;
// Version counter: bumped by every producer/visual write.  ReadAuthoritativeObjectiveSnapshot
// skips the expensive deep-copy + join when the version hasn't changed, reducing per-frame
// cost from O(map_size) hash-map copies to O(1) for unchanged frames.
static std::atomic<uint64_t> g_OSAuthWriteVersion{0};
static uint64_t g_OSAuthLastReadVersion = 0;

static bool TextHook_OSRev_IsEnabled() {
#if GHOSTSKOR_OSREV_ONLY
  return RuntimeFlags_ObjectiveStatusEnableOsRevOnly();
#else
  return false;
#endif
}

static float TextHook_OSRev_Clamp01(float v) {
  if (v < 0.0f) {
    return 0.0f;
  }
  if (v > 1.0f) {
    return 1.0f;
  }
  return v;
}

static bool TextHook_OSAuth_IsEnabled() {
  return TextHook_OSRev_IsEnabled();
}

static bool TextHook_OSAuth_IsVisualLessSaveStatusKey(const std::string &key) {
  return key == "EXE_GAMESAVED" || key == "CGAME_NOW_SAVING";
}

static ObjectiveInstanceId TextHook_OSAuth_MakeInstanceId(uint32_t cfgIndex,
                                                          int fxBirth,
                                                          uint64_t elemPtrOrIndex) {
  ObjectiveInstanceId id{};
  if (fxBirth <= 0 || elemPtrOrIndex == 0) {
    return id;
  }
  id.elemPtrOrIndex = elemPtrOrIndex;
  id.fxBirth = fxBirth;
  id.cfgIndex = cfgIndex;
  id.valid = true;
  return id;
}

static uint64_t
TextHook_OSAuth_MakeInstanceKey(const ObjectiveInstanceId &instanceId) {
  if (!instanceId.valid) {
    return 0;
  }
  uint64_t key = instanceId.elemPtrOrIndex;
  key ^= (uint64_t)(uint32_t)instanceId.fxBirth + 0x9E3779B97F4A7C15ull +
         (key << 6) + (key >> 2);
  return (key != 0) ? key : 1ull;
}

static uint32_t TextHook_OSAuth_PackColor(float r, float g, float b, float a) {
  auto toByte = [](float v) -> uint32_t {
    if (!std::isfinite(v)) {
      return 255u;
    }
    if (v < 0.0f) {
      v = 0.0f;
    } else if (v > 1.0f) {
      v = 1.0f;
    }
    return (uint32_t)(v * 255.0f + 0.5f);
  };
  return (toByte(a) << 24) | (toByte(b) << 16) | (toByte(g) << 8) | toByte(r);
}

static unsigned long g_OSAuthLastPruneTick = 0;

static void TextHook_OSAuth_PruneLocked(unsigned long now) {
  // Throttle: prune at most once per 500ms to avoid O(N) scan on every
  // mutex acquisition.  Writers and readers call this on every lock, so
  // without throttling the cost scales with (active_objectives * 2 + 1)
  // per frame — the primary source of frame drops with 2-3+ objectives.
  if (g_OSAuthLastPruneTick != 0 && (now - g_OSAuthLastPruneTick) < 500) {
    return;
  }
  g_OSAuthLastPruneTick = now;

  constexpr unsigned long kObjectiveProdTtlMs = 10 * 60 * 1000;
  // FIX: visual TTL was 180s — stale visuals lingered as ghost subtitles
  // for 3 minutes after HudElem destruction.  Detour refreshes captureTick
  // every frame (~16ms), so 2s of no refresh = HudElem is gone.
  // Direct HudElem memory polling is now the primary liveness authority.
  // These TTLs are safety-net fallbacks only — polling removes dead visuals
  // immediately, so these should rarely trigger.  Kept generous to avoid
  // premature expiry during loading screens or cutscenes where the detour
  // may not fire.
  constexpr unsigned long kObjectiveVisualTtlMs = 60000;   // 60s safety net
  constexpr unsigned long kStatusVisualTtlMs = 60000;      // 60s safety net
  constexpr unsigned long kStatusProdTtlMs = 60000;        // 60s safety net
  constexpr unsigned long kSnapshotLineTtlMs = 60000;      // 60s safety net

  for (auto it = g_OSAuthObjectiveTextByInstance.begin();
       it != g_OSAuthObjectiveTextByInstance.end();) {
    const unsigned long age =
        (now >= it->second.captureTick) ? (now - it->second.captureTick) : 0;
    if (age > kObjectiveProdTtlMs) {
      it = g_OSAuthObjectiveTextByInstance.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = g_OSAuthStatusProducerEvents.begin();
       it != g_OSAuthStatusProducerEvents.end();) {
    const unsigned long age =
        (now >= it->capture.captureTick) ? (now - it->capture.captureTick) : 0;
    if (age > kStatusProdTtlMs) {
      it = g_OSAuthStatusProducerEvents.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = g_OSAuthObjectiveVisualByInstance.begin();
       it != g_OSAuthObjectiveVisualByInstance.end();) {
    const unsigned long age =
        (now >= it->second.captureTick) ? (now - it->second.captureTick) : 0;
    if (age > kObjectiveVisualTtlMs) {
      it = g_OSAuthObjectiveVisualByInstance.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = g_OSAuthStatusVisualByInstance.begin();
       it != g_OSAuthStatusVisualByInstance.end();) {
    const unsigned long age =
        (now >= it->second.captureTick) ? (now - it->second.captureTick) : 0;
    if (age > kStatusVisualTtlMs) {
      it = g_OSAuthStatusVisualByInstance.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = g_OSAuthSnapshotLines.begin();
       it != g_OSAuthSnapshotLines.end();) {
    const unsigned long age =
        (now >= it->lifeStart) ? (now - it->lifeStart) : 0;
    if (age > kSnapshotLineTtlMs) {
      it = g_OSAuthSnapshotLines.erase(it);
    } else {
      ++it;
    }
  }
}

static void TextHook_OSAuth_RecordObjectiveProducerCapture(
    const std::string &key, uint32_t cfgIndex, uint32_t slcIndex, int fxBirth,
    uint64_t elemPtrOrIndex, unsigned int callerOffset,
    unsigned int calleeOffset) {
  if (!TextHook_OSAuth_IsEnabled() || key.empty() ||
      !TextHook_OSAuth_IsObjectiveCaptureCandidateKey(key)) {
    return;
  }

  ObjectiveTextCapture capture{};
  capture.instanceId =
      TextHook_OSAuth_MakeInstanceId(cfgIndex, fxBirth, elemPtrOrIndex);
  if (!capture.instanceId.valid) {
    return;
  }
  capture.channel = OBJSTAT_NATIVE_OBJECTIVE;
  capture.key = key;
  capture.rawText = key;
  capture.slcIndex = slcIndex;
  capture.callerRva = callerOffset;
  capture.calleeRva = calleeOffset;
  capture.threadId = GetCurrentThreadId();
  capture.captureTick = GetTickCount();

  {
    std::lock_guard<std::mutex> lock(g_OSAuthSnapshotMutex);
    TextHook_OSAuth_PruneLocked(capture.captureTick);
    const uint64_t instanceKey = TextHook_OSAuth_MakeInstanceKey(capture.instanceId);
    g_OSAuthObjectiveTextByInstance[instanceKey] = capture;
    g_OSAuthWriteVersion.fetch_add(1, std::memory_order_release);

    if (kVerboseRuntimeLogs) {
      static std::unordered_map<uint64_t, unsigned long> s_osauthProdLogTick;
      auto itLog = s_osauthProdLogTick.find(instanceKey);
      if (itLog == s_osauthProdLogTick.end() ||
          (capture.captureTick - itLog->second) > 1000) {
        s_osauthProdLogTick[instanceKey] = capture.captureTick;
        char pbuf[384];
        sprintf_s(
            pbuf,
            "[OSAUTH-PROD] ch=obj key=%s cfg=0x%X slc=0x%X fxB=%d elem=%llu caller=+0x%X callee=+0x%X",
            capture.key.c_str(), capture.instanceId.cfgIndex, capture.slcIndex,
            capture.instanceId.fxBirth,
            (unsigned long long)capture.instanceId.elemPtrOrIndex, capture.callerRva,
            capture.calleeRva);
        LogToFile(pbuf);
      }
    }
  }
}

static void TextHook_OSAuth_RecordStatusProducerCapture(
    const std::string &key, uint32_t cfgIndex, uint32_t slcIndex, int laneSlot,
    unsigned int callerOffset, uint64_t producerInstanceSig, int fxBirth,
    uint64_t elemPtrOrIndex, unsigned int calleeOffset) {
  if (!TextHook_OSAuth_IsEnabled() || key.empty() ||
      !IsObjectiveStatusMessageKeyName(key)) {
    return;
  }

  OSAuthStatusProducerEvidence evidence{};
  evidence.capture.channel = OBJSTAT_NATIVE_STATUS;
  evidence.capture.key = key;
  evidence.capture.rawText = key;
  evidence.capture.slcIndex = slcIndex;
  evidence.capture.callerRva = callerOffset;
  evidence.capture.calleeRva = calleeOffset;
  evidence.capture.threadId = GetCurrentThreadId();
  evidence.capture.captureTick = GetTickCount();
  evidence.capture.instanceId =
      TextHook_OSAuth_MakeInstanceId(cfgIndex, fxBirth, elemPtrOrIndex);
  if (!evidence.capture.instanceId.valid) {
    evidence.capture.instanceId.cfgIndex = cfgIndex;
    evidence.capture.instanceId.fxBirth = fxBirth;
    evidence.capture.instanceId.elemPtrOrIndex = elemPtrOrIndex;
  }
  const bool exactJoin = evidence.capture.instanceId.valid;
  const bool allowVisualLessSynthetic =
      TextHook_OSAuth_IsVisualLessSaveStatusKey(key);
  if (!exactJoin && !allowVisualLessSynthetic) {
    if (kVerboseRuntimeLogs) {
      static std::unordered_map<std::string, unsigned long>
          s_osauthStatusProdRejectLogTick;
      const std::string logSig =
          key + "|" + std::to_string(callerOffset) + "|" +
          std::to_string((unsigned int)cfgIndex) + "|" +
          std::to_string((unsigned int)slcIndex);
      auto itLog = s_osauthStatusProdRejectLogTick.find(logSig);
      if (itLog == s_osauthStatusProdRejectLogTick.end() ||
          (evidence.capture.captureTick - itLog->second) > 1200) {
        s_osauthStatusProdRejectLogTick[logSig] = evidence.capture.captureTick;
        char pbuf[384];
        sprintf_s(
            pbuf,
            "[TOPLEFT-STATUS-REJECT] key=%s cfg=0x%X slc=0x%X lane=%d caller=+0x%X reason=non_exact",
            key.c_str(), cfgIndex, slcIndex, laneSlot, callerOffset);
        LogToFile(pbuf);
      }
    }
    return;
  }
  evidence.laneSlot = laneSlot;
  evidence.producerInstanceSig =
      (producerInstanceSig != 0)
          ? producerInstanceSig
          : TextHook_OSAuth_MakeInstanceKey(evidence.capture.instanceId);

  bool shouldLog = false;
  {
    std::lock_guard<std::mutex> lock(g_OSAuthSnapshotMutex);
    TextHook_OSAuth_PruneLocked(evidence.capture.captureTick);

    const uint64_t evidenceInstanceKey =
        TextHook_OSAuth_MakeInstanceKey(evidence.capture.instanceId);
    bool merged = false;
    for (auto it = g_OSAuthStatusProducerEvents.rbegin();
         it != g_OSAuthStatusProducerEvents.rend(); ++it) {
      if (it->capture.key != evidence.capture.key) {
        continue;
      }
      if (evidenceInstanceKey != 0) {
        const uint64_t existingInstanceKey =
            TextHook_OSAuth_MakeInstanceKey(it->capture.instanceId);
        if (existingInstanceKey != evidenceInstanceKey) {
          continue;
        }
      } else if (it->capture.instanceId.cfgIndex !=
                 evidence.capture.instanceId.cfgIndex) {
        continue;
      }
      const unsigned long age =
          (evidence.capture.captureTick >= it->capture.captureTick)
              ? (evidence.capture.captureTick - it->capture.captureTick)
              : 0;
      if (age > 120) {
        break;
      }
      it->capture.captureTick = evidence.capture.captureTick;
      it->capture.callerRva = evidence.capture.callerRva;
      it->capture.calleeRva = evidence.capture.calleeRva;
      it->capture.slcIndex = evidence.capture.slcIndex;
      if (evidence.capture.instanceId.valid) {
        it->capture.instanceId = evidence.capture.instanceId;
      } else if (evidence.capture.instanceId.cfgIndex != 0) {
        it->capture.instanceId.cfgIndex = evidence.capture.instanceId.cfgIndex;
      }
      it->laneSlot = evidence.laneSlot;
      if (evidence.producerInstanceSig != 0) {
        it->producerInstanceSig = evidence.producerInstanceSig;
      }
      merged = true;
      break;
    }
    g_OSAuthWriteVersion.fetch_add(1, std::memory_order_release);
    if (!merged) {
      g_OSAuthStatusProducerEvents.push_back(evidence);
      while (g_OSAuthStatusProducerEvents.size() > 64) {
        g_OSAuthStatusProducerEvents.pop_front();
      }
    }

    if (kVerboseRuntimeLogs) {
      static std::unordered_map<std::string, unsigned long> s_osauthStatusProdLogTick;
      const std::string logSig =
          evidence.capture.key + "|" +
          std::to_string((unsigned int)(evidence.capture.instanceId.cfgIndex)) +
          "|" +
          std::to_string((unsigned int)(evidence.capture.instanceId.fxBirth)) +
          "|" +
          std::to_string(
              (unsigned int)(evidence.capture.instanceId.elemPtrOrIndex & 0xFFFFFFFFu)) +
          "|" +
          std::to_string((unsigned int)(evidence.producerInstanceSig & 0xFFFFFFFFu));
      auto itLog = s_osauthStatusProdLogTick.find(logSig);
      if (itLog == s_osauthStatusProdLogTick.end() ||
          (evidence.capture.captureTick - itLog->second) > 800) {
        s_osauthStatusProdLogTick[logSig] = evidence.capture.captureTick;
        char pbuf[384];
        sprintf_s(
            pbuf,
            "[OSAUTH-PROD] ch=status key=%s cfg=0x%X slc=0x%X lane=%d caller=+0x%X isig=0x%llX",
            evidence.capture.key.c_str(), evidence.capture.instanceId.cfgIndex,
            evidence.capture.slcIndex, evidence.laneSlot, evidence.capture.callerRva,
            (unsigned long long)evidence.producerInstanceSig);
        LogToFile(pbuf);
      }
    }
  }
}

static bool TextHook_OSRev_IsObjectiveStatusKey(const std::string &key) {
  if (key.empty()) {
    return false;
  }
  if (IsObjectiveStatusMessageKeyName(key)) {
    return true;
  }
  return TextHook_OSRev_IsNativeObjectiveCandidateKey(key);
}

static std::string TextHook_OSRev_NormalizeRawText(const std::string &text) {
  return TrimSpaces(StripColorCodes(text));
}

static bool TextHook_OSRev_LooksLikePromptFragment(const std::string &text) {
  const std::string cleaned = TrimSpaces(StripColorCodes(text));
  if (cleaned.empty()) {
    return false;
  }
  if (LooksLikePressHoldPrompt(cleaned)) {
    return true;
  }

  const std::string upper = ToUpperAscii(cleaned);
  std::string compact;
  compact.reserve(upper.size());
  for (unsigned char ch : upper) {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
      compact.push_back((char)ch);
    }
  }
  if (compact.empty()) {
    return false;
  }

  const bool hasActivate = (compact.find("ACTIVATE") != std::string::npos);
  const bool hasTruncatedActivate =
      (compact.find("CTIVATE") != std::string::npos ||
       compact.find("TIVATE") != std::string::npos ||
       compact.find("IVATE") != std::string::npos);
  const bool hasToggle = (compact.find("TOGGLE") != std::string::npos);
  const bool hasZoom = (compact.find("ZOOM") != std::string::npos);
  const bool hasScanner = (compact.find("OPTICSCANNER") != std::string::npos ||
                           compact.find("PTICSCANNER") != std::string::npos ||
                           compact.find("SCANNER") != std::string::npos);
  const bool hasPromptVerb =
      hasActivate || hasTruncatedActivate || hasToggle || hasZoom ||
      (compact.find("PRESS") != std::string::npos) ||
      (compact.find("RESS") == 0) ||
      (compact.find("HOLD") != std::string::npos);

  if ((hasScanner && hasPromptVerb) ||
      compact.find("TOGGLEZOOM") != std::string::npos ||
      compact.find("OGGLEZOOM") != std::string::npos ||
      compact.find("TOACTIVATE") != std::string::npos ||
      compact.find("OACTIVATE") != std::string::npos ||
      compact.find("TOTOGGLE") != std::string::npos ||
      compact.find("OTOGGLE") != std::string::npos) {
    return true;
  }

  if ((hasActivate || hasTruncatedActivate || hasToggle || hasZoom) &&
      (compact.rfind("TO", 0) == 0 || compact.rfind("O", 0) == 0 ||
       compact.rfind("PRESS", 0) == 0 || compact.rfind("RESS", 0) == 0 ||
       compact.rfind("HOLD", 0) == 0)) {
    return true;
  }
  return false;
}

static bool TextHook_OSRev_IsPromptLikeLocalizedKey(
    const std::string &key) {
  if (key.empty() || IsObjectiveStatusMessageKeyName(key)) {
    return false;
  }
  if (key.rfind("PLATFORM_", 0) == 0 || key.rfind("SCRIPT_HINT_", 0) == 0 ||
      key.rfind("SCRIPT_PLATFORM_HINT_", 0) == 0) {
    return true;
  }

  std::string korean;
  std::string english;
  if (!TextHook_GetLocalizedKeyText(key, korean, english)) {
    return false;
  }
  return (!english.empty() && TextHook_OSRev_LooksLikePromptFragment(english)) ||
         (!korean.empty() && TextHook_OSRev_LooksLikePromptFragment(korean));
}

static bool TextHook_OSRev_IsNativeObjectiveCandidateKey(
    const std::string &key) {
  if (key.empty() || IsObjectiveStatusMessageKeyName(key)) {
    return false;
  }
  if (key.rfind("MENU_", 0) == 0 || key.rfind("LUA_MENU_", 0) == 0 ||
      key.rfind("PLATFORM_", 0) == 0 || key.rfind("MP_", 0) == 0 ||
      key.rfind("SUBTITLE_", 0) == 0 ||
      key.rfind("SCRIPT_HINT_", 0) == 0 ||
      key.rfind("SCRIPT_PLATFORM_HINT_", 0) == 0 ||
      key.rfind("DEADQUOTE_", 0) == 0 ||
      key.rfind("CORNERED_", 0) == 0 && key.find("_OBJ_") == std::string::npos) {
    return false;
  }

  const bool looksLikeObjectiveKey =
      (key.find("_OBJ_") != std::string::npos ||
       key.rfind("SCRIPT_OBJ_", 0) == 0);
  if (!looksLikeObjectiveKey) {
    return false;
  }

  std::string korean;
  std::string english;
  if (!TextHook_GetLocalizedKeyText(key, korean, english)) {
    return false;
  }
  return !TextHook_OSRev_IsPromptLikeLocalizedKey(key);
}

static bool
TextHook_OSAuth_IsObjectiveCaptureCandidateKey(const std::string &key) {
  if (TextHook_OSRev_IsNativeObjectiveCandidateKey(key)) {
    return true;
  }
  if (key.empty() || IsObjectiveStatusMessageKeyName(key)) {
    return false;
  }
  if (key.rfind("MENU_", 0) == 0 || key.rfind("LUA_MENU_", 0) == 0 ||
      key.rfind("PLATFORM_", 0) == 0 || key.rfind("MP_", 0) == 0 ||
      key.rfind("SUBTITLE_", 0) == 0 ||
      key.rfind("SCRIPT_HINT_", 0) == 0 ||
      key.rfind("SCRIPT_PLATFORM_HINT_", 0) == 0 ||
      key.rfind("DEADQUOTE_", 0) == 0 ||
      (key.rfind("CORNERED_", 0) == 0 &&
       key.find("_OBJ_") == std::string::npos)) {
    return false;
  }

  const std::string upper = ToUpperAscii(key);
  if (upper.find("INTROSCREEN") != std::string::npos) {
    return false;
  }

  std::string korean;
  std::string english;
  if (!TextHook_GetLocalizedKeyText(key, korean, english)) {
    return false;
  }
  if (korean.empty() && english.empty()) {
    return false;
  }
  return !TextHook_OSRev_IsPromptLikeLocalizedKey(key);
}

static bool TextHook_OSRev_TryResolveAnyLocalizedKeyFromRaw(
    const std::string &rawText, std::string &outKey) {
  outKey.clear();
  const std::string trimmed = TextHook_OSRev_NormalizeRawText(rawText);
  if (trimmed.empty()) {
    return false;
  }
  if (LooksLikeHudLocalizationKey(trimmed)) {
    outKey = trimmed;
    return true;
  }
  const std::string norm = ObjRev_SanitizeEnglishForKeyResolve(trimmed);
  if (norm.empty()) {
    return false;
  }
  return ResolveKeyFromEnglishInternal(norm, outKey);
}

static bool TextHook_OSRev_ResolveKeyFromText(const std::string &text,
                                              std::string &outKey) {
  outKey.clear();
  if (text.empty()) {
    return false;
  }
  std::string trimmed = TrimSpaces(StripColorCodes(text));
  if (trimmed.empty()) {
    return false;
  }
  if (LooksLikeHudLocalizationKey(trimmed)) {
    outKey = trimmed;
    return TextHook_OSRev_IsObjectiveStatusKey(outKey);
  }

  std::string norm = ObjRev_SanitizeEnglishForKeyResolve(trimmed);
  if (norm.empty()) {
    norm = trimmed;
  }
  std::string resolved;
  if (ResolveKeyFromEnglishInternal(norm, resolved) &&
      TextHook_OSRev_IsObjectiveStatusKey(resolved)) {
    outKey = resolved;
    return true;
  }

  // Fallback for already-localized status strings.
  auto normalizeLoose = [](std::string s) -> std::string {
    s = TrimSpaces(StripColorCodes(s));
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
      if (std::isspace(ch)) {
        continue;
      }
      if (ch == '.' || ch == '!' || ch == '?' || ch == ',' || ch == ':') {
        continue;
      }
      out.push_back((char)ch);
    }
    return out;
  };

  const std::string targetNorm = normalizeLoose(trimmed);
  if (targetNorm.empty()) {
    return false;
  }

  static const char *kStatusKeys[] = {
      "GAME_OBJECTIVESUPDATED", "GAME_OBJECTIVECOMPLETED",
      "GAME_OBJECTIVEFAILED",
      "EXE_GAMESAVED", "CGAME_NOW_SAVING"};
  for (const char *candidateKey : kStatusKeys) {
    std::string kor;
    std::string eng;
    if (!TextHook_GetLocalizedKeyText(candidateKey, kor, eng)) {
      continue;
    }
    if (!kor.empty() && normalizeLoose(kor) == targetNorm) {
      outKey = candidateKey;
      return true;
    }
    if (!eng.empty() && normalizeLoose(eng) == targetNorm) {
      outKey = candidateKey;
      return true;
    }
    const char *fallback = GetObjectiveStatusKoreanFallback(candidateKey);
    if (fallback && fallback[0] &&
        normalizeLoose(std::string(fallback)) == targetNorm) {
      outKey = candidateKey;
      return true;
    }
  }
  return false;
}

static int TextHook_OSRev_LaneFromNativeY(float yVirtual) {
  int slot = (int)lroundf((yVirtual - 10.0f) / 20.0f);
  if (slot < -8) {
    slot = -8;
  }
  if (slot > 12) {
    slot = 12;
  }
  return slot;
}

static ObjectiveStatusNativeChannel
TextHook_OSRev_ChannelFromKey(const std::string &key) {
  return IsObjectiveStatusMessageKeyName(key) ? OBJSTAT_NATIVE_STATUS
                                              : OBJSTAT_NATIVE_OBJECTIVE;
}

static void
TextHook_OSAuth_RecordVisualCapture(const ObjectiveVisualCapture &capture) {
  if (!TextHook_OSAuth_IsEnabled() || !capture.instanceId.valid ||
      capture.key.empty()) {
    return;
  }

  const uint64_t instanceKey = TextHook_OSAuth_MakeInstanceKey(capture.instanceId);
  if (instanceKey == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_OSAuthSnapshotMutex);
  TextHook_OSAuth_PruneLocked(capture.captureTick);
  auto &targetMap =
      (capture.channel == OBJSTAT_NATIVE_STATUS) ? g_OSAuthStatusVisualByInstance
                                                 : g_OSAuthObjectiveVisualByInstance;
  auto it = targetMap.find(instanceKey);
  if (it == targetMap.end()) {
    targetMap[instanceKey] = capture;
    g_OSAuthWriteVersion.fetch_add(1, std::memory_order_release);
    return;
  }

  bool replace = false;
  const bool introVisualPair =
      (capture.calleeRva == 0x1F1740u && it->second.calleeRva == 0x1F1740u);
  const DWORD introDeltaMs =
      (capture.captureTick >= it->second.captureTick)
          ? (capture.captureTick - it->second.captureTick)
          : (it->second.captureTick - capture.captureTick);
  if (introVisualPair && introDeltaMs <= 40) {
    const float scaleDelta = capture.scale - it->second.scale;
    if (scaleDelta > 0.01f) {
      replace = true;
    } else if (scaleDelta < -0.01f) {
      replace = false;
    } else if (capture.alpha + 0.03f >= it->second.alpha) {
      replace = (capture.captureTick >= it->second.captureTick);
    }
  } else {
    replace =
        (capture.captureTick > it->second.captureTick) ||
        (capture.captureTick == it->second.captureTick &&
         capture.alpha >= it->second.alpha);
  }
  if (replace) {
    targetMap[instanceKey] = capture;
    g_OSAuthWriteVersion.fetch_add(1, std::memory_order_release);
  }

  if (kVerboseRuntimeLogs && capture.channel == OBJSTAT_NATIVE_OBJECTIVE) {
    static std::unordered_map<uint64_t, unsigned long> s_osauthVisCapLogTick;
    auto itLog = s_osauthVisCapLogTick.find(instanceKey);
    if (itLog == s_osauthVisCapLogTick.end() ||
        (capture.captureTick - itLog->second) > 1200) {
      s_osauthVisCapLogTick[instanceKey] = capture.captureTick;
      char vbuf[448];
      sprintf_s(
          vbuf,
          "[OSAUTH-VIS-CAP] ch=obj key=%s cfg=0x%X fxB=%d elem=%llu lane=%d x=%.1f y=%.1f a=%.2f",
          capture.key.c_str(), capture.instanceId.cfgIndex,
          capture.instanceId.fxBirth,
          (unsigned long long)capture.instanceId.elemPtrOrIndex,
          capture.laneSlot, capture.x, capture.y, capture.alpha);
      LogToFile(vbuf);
    }
  }
}

static void TextHook_OSAuth_RecordIntroCapture(
    const std::string &key, const std::string &rawText, uint32_t cfgIndex,
    uint32_t slcIndex, int fxBirth, int fxLetter, uint64_t elemPtrOrIndex,
    float x, float y, float scale, float fontHeight, uint32_t colorPacked,
    unsigned short virtualW, unsigned short virtualH, unsigned int callerOffset,
    unsigned int calleeOffset) {
  if (!TextHook_OSAuth_IsEnabled() || key.empty() ||
      (!IsObjectiveStatusMessageKeyName(key) &&
       !TextHook_OSAuth_IsObjectiveCaptureCandidateKey(key))) {
    return;
  }

  ObjectiveVisualCapture capture{};
  capture.instanceId =
      TextHook_OSAuth_MakeInstanceId(cfgIndex, fxBirth, elemPtrOrIndex);
  if (!capture.instanceId.valid) {
    return;
  }

  capture.channel = TextHook_OSRev_ChannelFromKey(key);
  capture.laneSlot = TextHook_OSRev_LaneFromNativeY(y);
  capture.key = key;
  capture.rawText = rawText.empty() ? key : rawText;
  capture.x = x;
  capture.y = y;
  capture.scale = (scale > 0.02f && scale < 10.0f) ? scale : 1.25f;
  capture.fontHeight =
      (fontHeight > 5.0f && fontHeight < 200.0f) ? fontHeight : 78.0f;
  capture.alpha =
      TextHook_OSRev_Clamp01((float)((colorPacked >> 24) & 0xFF) / 255.0f);
  capture.colorPacked = colorPacked;
  capture.virtualW = virtualW;
  capture.virtualH = virtualH;
  capture.fxLetterTime = (fxLetter > 0 && fxLetter < 2000) ? fxLetter : 0;
  capture.callerRva = callerOffset;
  capture.calleeRva = calleeOffset;
  capture.captureTick = GetTickCount();

  TextHook_OSAuth_RecordVisualCapture(capture);

  if (capture.channel == OBJSTAT_NATIVE_STATUS) {
    const uint64_t instanceKey = TextHook_OSAuth_MakeInstanceKey(capture.instanceId);
    TextHook_OSAuth_RecordStatusProducerCapture(
        key, cfgIndex, slcIndex, capture.laneSlot, callerOffset, instanceKey,
        fxBirth, elemPtrOrIndex, calleeOffset);
    return;
  }

  TextHook_OSAuth_RecordObjectiveProducerCapture(key, cfgIndex, slcIndex,
                                                 fxBirth, elemPtrOrIndex,
                                                 callerOffset, calleeOffset);
}

static std::string TextHook_OSRev_ResolveDisplayText(const std::string &key) {
  std::string korean;
  std::string english;
  if (TextHook_GetLocalizedKeyText(key, korean, english) && !korean.empty()) {
    return StripColorCodes(korean);
  }
  if (IsObjectiveStatusMessageKeyName(key)) {
    const char *fallback = GetObjectiveStatusKoreanFallback(key);
    if (fallback && fallback[0]) {
      return fallback;
    }
  }
  return key;
}

static std::string TextHook_OSRev_ResolveDisplayText(const std::string &key,
                                                     const std::string &rawText) {
  if (!key.empty()) {
    const std::string display = TextHook_OSRev_ResolveDisplayText(key);
    if (!display.empty() && display != key) {
      return display;
    }
  }

  std::string mappedKey;
  if (TextHook_OSRev_TryResolveAnyLocalizedKeyFromRaw(rawText, mappedKey)) {
    std::string korean;
    std::string english;
    if (TextHook_GetLocalizedKeyText(mappedKey, korean, english) &&
        !korean.empty()) {
      return StripColorCodes(korean);
    }
  }

  return TextHook_OSRev_NormalizeRawText(rawText);
}

// OSREV data path removed — OSAUTH is the sole authority.
// Stub kept for callers that still reference this signature.
static void TextHook_OSRev_RecordProducerSample(const char *, unsigned int,
                                                unsigned int, uintptr_t,
                                                uintptr_t) {}

// OSREV chain probe removed — OSAUTH is the sole authority.
static void TextHook_OSRev_RecordChainProbe(const char *, unsigned int,
                                            uintptr_t, uintptr_t, uintptr_t,
                                            uintptr_t, uintptr_t, uintptr_t) {}

// OSREV draw sample removed — OSAUTH is the sole authority.
static void TextHook_OSRev_RecordDrawSample(const std::string &,
                                            const std::string &,
                                            const HudNativeEntry &,
                                            unsigned int, unsigned int) {}

// OSREV resolve draw sample removed — OSAUTH is the sole authority.
static void TextHook_OSRev_RecordResolveDrawSample(const char *, unsigned int,
                                                   unsigned int, float, float,
                                                   float, float, float,
                                                   uintptr_t, uintptr_t,
                                                   unsigned int, unsigned int) {}

// --- Direct HudElem memory polling ---
// Instead of guessing when a HudElem disappears via TTL, read its memory
// directly to determine liveness and current alpha.
enum class HudElemPollResult {
  Alive,       // Element is active with same identity
  Dead,        // Element type=0 (freed) or memory unreadable
  Reused,      // Slot reused by different element (fxBirthTime changed)
};

struct HudElemPollData {
  HudElemPollResult result = HudElemPollResult::Dead;
  float liveAlpha = 0.0f;          // Current alpha from engine memory
  uint32_t liveColorPacked = 0;    // Current full color from engine memory
};

static HudElemPollData TextHook_OSAuth_PollHudElem(uintptr_t elemPtrOrIndex,
                                                    int expectedFxBirth) {
  HudElemPollData data{};

  if (elemPtrOrIndex == 0) {
    return data; // Dead — null
  }

  // elemPtrOrIndex may be a raw pointer OR an array index (0..1023).
  // If it looks like an index (small value), convert to pointer using
  // the known HudElem array base address.
  uintptr_t elemPtr = elemPtrOrIndex;
  if (elemPtrOrIndex < 0x10000) {
    static uintptr_t s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
    const uintptr_t arrayBase = s_moduleBase + IW6Offsets::HudElem_Array_SP;
    elemPtr = arrayBase + elemPtrOrIndex * IW6Offsets::HudElem_Array_Stride_SP;
  }

  if (elemPtr < 0x10000 || elemPtr >= 0x7FFFFFFFFFFF) {
    return data; // Dead — invalid pointer after conversion
  }

  // Check if memory is readable (0xA8 = HudElem stride)
  if (IsBadReadPtr((void *)elemPtr, 0xA8) != 0) {
    return data; // Dead — memory no longer accessible
  }

  // Read type field at offset 0x00
  uint32_t type = 0;
  if (!SafeReadVolatile(elemPtr + 0x00, &type)) {
    return data; // Dead — read failed
  }
  if (type == 0) {
    return data; // Dead — element freed (type=FREE)
  }

  // Read fxBirthTime at offset 0x90 to detect slot reuse
  int fxBirth = 0;
  if (!SafeReadVolatile(elemPtr + 0x90, &fxBirth)) {
    return data; // Dead — read failed
  }
  if (expectedFxBirth > 0 && fxBirth != expectedFxBirth) {
    data.result = HudElemPollResult::Reused;
    return data; // Slot reused by different element
  }

  // Element is alive — read live color/alpha
  uint32_t colorPacked = 0;
  if (SafeReadVolatile(elemPtr + 0x30, &colorPacked)) {
    data.liveColorPacked = colorPacked;
    data.liveAlpha = (float)((colorPacked >> 24) & 0xFF) / 255.0f;
  }

  data.result = HudElemPollResult::Alive;
  return data;
}

void TextHook_ReadAuthoritativeObjectiveSnapshot() {
  if (!TextHook_OSAuth_IsEnabled()) {
    return;
  }

  const unsigned long now = GetTickCount();

  // Version gate: skip the expensive deep-copy + join if no producer/visual
  // writes occurred since last rebuild.  This turns per-frame cost from
  // O(map_size) to O(1) for unchanged frames — the dominant case at 60fps.
  const uint64_t currentWriteVer = g_OSAuthWriteVersion.load(std::memory_order_acquire);
  if (currentWriteVer == g_OSAuthLastReadVersion &&
      g_OSAuthLastSnapshotTick != 0 &&
      (now - g_OSAuthLastSnapshotTick) < 16) {
    // Data unchanged and snapshot is fresh — nothing to do.
    return;
  }
  g_OSAuthLastReadVersion = currentWriteVer;

  std::unordered_map<uint64_t, ObjectiveTextCapture> objectiveTextByInstance;
  std::unordered_map<uint64_t, ObjectiveVisualCapture> objectiveVisualStore;
  std::unordered_map<uint64_t, ObjectiveVisualCapture> statusVisualStore;
  std::deque<OSAuthStatusProducerEvidence> statusProducerEvents;
  std::vector<ObjectiveStatusNativeLine> previousSnapshotLines;
  {
    std::lock_guard<std::mutex> lock(g_OSAuthSnapshotMutex);
    const unsigned long resetTick = TextHook_GetObjectiveRuntimeResetTick();
    if (resetTick != 0 && resetTick != g_OSAuthLastResetTickSeen) {
      g_OSAuthObjectiveTextByInstance.clear();
      g_OSAuthObjectiveVisualByInstance.clear();
      g_OSAuthStatusVisualByInstance.clear();
      g_OSAuthStatusProducerEvents.clear();
      g_OSAuthSnapshotLines.clear();
      // Clear probe caches so config keys can be re-resolved after map restart
      ObjRev_ClearProbeAndBiasCaches();
    }
    g_OSAuthLastResetTickSeen = resetTick;
    TextHook_OSAuth_PruneLocked(now);
    objectiveTextByInstance = g_OSAuthObjectiveTextByInstance;
    objectiveVisualStore = g_OSAuthObjectiveVisualByInstance;
    statusVisualStore = g_OSAuthStatusVisualByInstance;
    statusProducerEvents = g_OSAuthStatusProducerEvents;
    previousSnapshotLines = g_OSAuthSnapshotLines;

    // Diagnostic: log status producer deque state at copy time
    if (kVerboseRuntimeLogs) {
      static unsigned long s_lastStatusDiagTick = 0;
      if ((now - s_lastStatusDiagTick) > 2000 &&
          !g_OSAuthStatusProducerEvents.empty()) {
        s_lastStatusDiagTick = now;
        const auto &first = g_OSAuthStatusProducerEvents.front();
        const auto &last = g_OSAuthStatusProducerEvents.back();
        const uint64_t firstIK =
            TextHook_OSAuth_MakeInstanceKey(first.capture.instanceId);
        const uint64_t lastIK =
            TextHook_OSAuth_MakeInstanceKey(last.capture.instanceId);
        char dbuf[512];
        sprintf_s(dbuf,
                  "[OSAUTH-DIAG-STATPROD] dequeSize=%u copySize=%u "
                  "first={key=%s valid=%d ik=0x%llX age=%lu} "
                  "last={key=%s valid=%d ik=0x%llX age=%lu}",
                  (unsigned int)g_OSAuthStatusProducerEvents.size(),
                  (unsigned int)statusProducerEvents.size(),
                  first.capture.key.c_str(), (int)first.capture.instanceId.valid,
                  (unsigned long long)firstIK,
                  (now >= first.capture.captureTick)
                      ? (now - first.capture.captureTick) : 0,
                  last.capture.key.c_str(), (int)last.capture.instanceId.valid,
                  (unsigned long long)lastIK,
                  (now >= last.capture.captureTick)
                      ? (now - last.capture.captureTick) : 0);
        LogToFile(dbuf);
      }
    }
  }

  struct VisualCandidate {
    ObjectiveVisualCapture capture{};
    unsigned long lastSeen = 0;
  };

  auto shouldPreferVisual = [](const VisualCandidate &dst,
                               const VisualCandidate &cand) {
    if (cand.lastSeen != dst.lastSeen) {
      return cand.lastSeen > dst.lastSeen;
    }
    if (cand.capture.alpha != dst.capture.alpha) {
      return cand.capture.alpha > dst.capture.alpha;
    }
    return cand.capture.laneSlot < dst.capture.laneSlot;
  };

  auto logDrop = [&](const char *reason, const std::string &key,
                     uint32_t cfgIndex, const ObjectiveInstanceId &instanceId,
                     unsigned int callerRva) {
    if (!kVerboseRuntimeLogs) return;
    static std::unordered_map<std::string, unsigned long> s_osauthDropLogTick;
    const std::string sig =
        std::string(reason ? reason : "unknown") + "|" + key + "|" +
        std::to_string(cfgIndex) + "|" +
        std::to_string((unsigned int)(instanceId.elemPtrOrIndex & 0xFFFFu)) + "|" +
        std::to_string((unsigned int)callerRva);
    auto itLog = s_osauthDropLogTick.find(sig);
    if (itLog != s_osauthDropLogTick.end() && (now - itLog->second) <= 1200) {
      return;
    }
    s_osauthDropLogTick[sig] = now;
    char dbuf[384];
    sprintf_s(
        dbuf,
        "[OSAUTH-DROP] reason=%s key=%s cfg=0x%X fxB=%d elem=%llu caller=+0x%X",
        reason ? reason : "unknown", key.empty() ? "(none)" : key.c_str(),
        cfgIndex, instanceId.fxBirth,
        (unsigned long long)instanceId.elemPtrOrIndex, callerRva);
    LogToFile(dbuf);
  };

  auto logVisual = [&](const VisualCandidate &cand) {
    if (!kVerboseRuntimeLogs) return;
    static std::unordered_map<uint64_t, unsigned long> s_osauthVisLogTick;
    const uint64_t sig = TextHook_OSAuth_MakeInstanceKey(cand.capture.instanceId) ^
                         ((uint64_t)(unsigned int)cand.capture.channel << 60);
    auto itLog = s_osauthVisLogTick.find(sig);
    if (itLog != s_osauthVisLogTick.end() && (now - itLog->second) <= 1000) {
      return;
    }
    s_osauthVisLogTick[sig] = now;
    char vbuf[448];
    sprintf_s(
        vbuf,
        "[OSAUTH-VIS] ch=%s key=%s cfg=0x%X fxB=%d elem=%llu lane=%d x=%.1f y=%.1f a=%.2f src=%s",
        cand.capture.channel == OBJSTAT_NATIVE_STATUS ? "status" : "obj",
        cand.capture.key.c_str(), cand.capture.instanceId.cfgIndex,
        cand.capture.instanceId.fxBirth,
        (unsigned long long)cand.capture.instanceId.elemPtrOrIndex,
        cand.capture.laneSlot, cand.capture.x, cand.capture.y,
        cand.capture.alpha, "intro");
    LogToFile(vbuf);
  };

  auto logJoinCand = [&](const char *kind, const std::string &key,
                         uint32_t cfgIndex, size_t count,
                         unsigned int callerRva) {
    if (!kVerboseRuntimeLogs) return;
    static std::unordered_map<std::string, unsigned long> s_osauthJoinCandTick;
    const std::string sig =
        std::string(kind ? kind : "?") + "|" + key + "|" +
        std::to_string(cfgIndex) + "|" + std::to_string(callerRva);
    auto itLog = s_osauthJoinCandTick.find(sig);
    if (itLog != s_osauthJoinCandTick.end() && (now - itLog->second) <= 900) {
      return;
    }
    s_osauthJoinCandTick[sig] = now;
    char cbuf[320];
    sprintf_s(cbuf,
              "[OSAUTH-JOIN-CAND] kind=%s key=%s cfg=0x%X cand=%u caller=+0x%X",
              kind ? kind : "?", key.c_str(), cfgIndex, (unsigned int)count,
              callerRva);
    LogToFile(cbuf);
  };

  auto logJoinProof = [&](const char *kind, const ObjectiveStatusNativeLine &line,
                          const ObjectiveInstanceId &instanceId,
                          unsigned int visualCaller) {
    if (!kVerboseRuntimeLogs) return;
    static std::unordered_map<std::string, unsigned long> s_osauthJoinLogTick;
    const std::string sig =
        std::string(kind ? kind : "?") + "|" + line.key + "|" +
        std::to_string(line.producerCallerRva) + "|" +
        std::to_string((unsigned int)(instanceId.elemPtrOrIndex & 0xFFFFu));
    auto itLog = s_osauthJoinLogTick.find(sig);
    if (itLog != s_osauthJoinLogTick.end() && (now - itLog->second) <= 900) {
      return;
    }
    s_osauthJoinLogTick[sig] = now;

    char jbuf[448];
    sprintf_s(
        jbuf,
        "[OSAUTH-JOIN] kind=%s key=%s cfg=0x%X fxB=%d elem=%llu lane=%d prod=+0x%X vis=+0x%X",
        kind ? kind : "?", line.key.c_str(), instanceId.cfgIndex,
        instanceId.fxBirth, (unsigned long long)instanceId.elemPtrOrIndex,
        line.laneSlot, line.producerCallerRva, visualCaller);
    LogToFile(jbuf);

    char pbuf[448];
    sprintf_s(
        pbuf,
        "[OSAUTH-PROOF] kind=%s key=%s seq=%u prod=+0x%X draw=+0x%X cfg=0x%X fxB=%d elem=%llu",
        kind ? kind : "?", line.key.c_str(), line.sourceSeq,
        line.producerCallerRva, line.drawCallerRva, instanceId.cfgIndex,
        instanceId.fxBirth, (unsigned long long)instanceId.elemPtrOrIndex);
    LogToFile(pbuf);
  };

  // logStatusProbeFallback removed — probe fallback disabled (FSHook-direct).

  auto makeSnapshotLineInstanceKey = [](const ObjectiveStatusNativeLine &line)
      -> uint64_t {
    if (line.instanceSig != 0) {
      return line.instanceSig;
    }
    if (line.sourceElemPtrOrIndex == 0 || line.fxBirthTime <= 0) {
      return 0;
    }
    ObjectiveInstanceId id{};
    id.elemPtrOrIndex = line.sourceElemPtrOrIndex;
    id.fxBirth = line.fxBirthTime;
    id.cfgIndex = line.cfgIndex;
    id.valid = true;
    return TextHook_OSAuth_MakeInstanceKey(id);
  };

  auto snapshotLineSourceName = [](const ObjectiveStatusNativeLine &line)
      -> const char * {
    if (line.channel == OBJSTAT_NATIVE_STATUS) {
      if ((line.authorityFlags & OBJSTAT_LINE_STATUS_PROBE_FALLBACK) != 0u) {
        return "status_probe";
      }
      if ((line.authorityFlags & OBJSTAT_LINE_HAS_VISUAL) != 0u) {
        return "status_visual";
      }
      return "status";
    }
    if ((line.authorityFlags & OBJSTAT_LINE_HAS_VISUAL) != 0u) {
      return "obj_visual";
    }
    return "obj";
  };

  auto snapshotLineDiagSig = [&](const ObjectiveStatusNativeLine &line)
      -> std::string {
    if (line.channel == OBJSTAT_NATIVE_STATUS) {
      if (line.sourceSeq != 0) {
        return std::string("status|") + line.key + "|" +
               std::to_string(line.sourceSeq);
      }
      return std::string("status|") + line.key + "|lane|" +
             std::to_string(line.laneSlot) + "|cfg|" +
             std::to_string(line.cfgIndex);
    }
    const uint64_t instanceKey = makeSnapshotLineInstanceKey(line);
    if (instanceKey != 0) {
      return std::string("obj|") + line.key + "|ik|" +
             std::to_string(instanceKey);
    }
    return std::string("obj|") + line.key + "|lane|" +
           std::to_string(line.laneSlot) + "|cfg|" +
           std::to_string(line.cfgIndex);
  };

  auto logLineLifecycle = [&](const char *phase,
                              const ObjectiveStatusNativeLine &line,
                              unsigned long ageMs,
                              const ObjectiveStatusNativeLine *prevLine) {
    if (!kVerboseRuntimeLogs) {
      return;
    }
    static std::unordered_map<std::string, unsigned long>
        s_osauthLineLifeLogTick;
    const std::string sig =
        std::string(phase ? phase : "?") + "|" + snapshotLineDiagSig(line);
    auto itLog = s_osauthLineLifeLogTick.find(sig);
    if (itLog != s_osauthLineLifeLogTick.end() &&
        (now - itLog->second) <= 250) {
      return;
    }
    s_osauthLineLifeLogTick[sig] = now;

    const ObjectiveStatusNativeLine *baseline =
        (prevLine != nullptr) ? prevLine : &line;
    char lbuf[640];
    sprintf_s(
        lbuf,
        "[OSAUTH-LINE-%s] ch=%s src=%s key=%s cfg=0x%X lane=%d seq=%u elem=%llu "
        "fxB=%d age=%lums x=%.1f y=%.1f a=%.2f life=%lu..%lu prevSrc=%s "
        "prevLane=%d prevA=%.2f",
        phase ? phase : "?", line.channel == OBJSTAT_NATIVE_STATUS ? "status"
                                                                   : "obj",
        snapshotLineSourceName(line), line.key.c_str(),
        (unsigned int)line.cfgIndex, line.laneSlot, line.sourceSeq,
        (unsigned long long)line.sourceElemPtrOrIndex, line.fxBirthTime, ageMs,
        line.x, line.y, line.alpha, (unsigned long)line.lifeStart,
        (unsigned long)line.lifeEnd,
        (prevLine != nullptr) ? snapshotLineSourceName(*prevLine) : "-",
        baseline->laneSlot, baseline->alpha);
    LogToFile(lbuf);
  };

  std::unordered_map<uint64_t, VisualCandidate> objectiveVisualByInstance;
  std::unordered_map<uint64_t, VisualCandidate> statusVisualByInstance;
  std::unordered_set<uint64_t> hiddenObjectiveVisualInstances;
  const size_t totalVisualStoreSize =
      objectiveVisualStore.size() + statusVisualStore.size();
  objectiveVisualByInstance.reserve(objectiveVisualStore.size() + 8);
  statusVisualByInstance.reserve(statusVisualStore.size() + 8);
  hiddenObjectiveVisualInstances.reserve(totalVisualStoreSize + 8);

  // Collect instance keys to remove after polling (dead/reused HudElems).
  // We do NOT remove inside the lock — the pruning pass below handles it.
  std::vector<uint64_t> deadObjectiveVisualKeys;
  std::vector<uint64_t> deadStatusVisualKeys;
  std::vector<uint64_t> staleObjectiveProducerKeys;

  size_t visualCount = 0;
  auto processVisualStore =
      [&](const std::unordered_map<uint64_t, ObjectiveVisualCapture> &store,
          ObjectiveStatusNativeChannel expectedChannel,
          std::unordered_map<uint64_t, VisualCandidate> &outByInstance,
          std::vector<uint64_t> &deadKeys) {
        for (const auto &kv : store) {
          const ObjectiveVisualCapture &stored = kv.second;
          if (!stored.instanceId.valid || stored.key.empty()) {
            continue;
          }

          const uintptr_t elemPtr = (uintptr_t)stored.instanceId.elemPtrOrIndex;
          const int expectedFxBirth = stored.instanceId.fxBirth;
          HudElemPollData poll =
              TextHook_OSAuth_PollHudElem(elemPtr, expectedFxBirth);

          const unsigned long captureAge =
              (now >= stored.captureTick) ? (now - stored.captureTick) : 0;
          if (poll.result == HudElemPollResult::Reused) {
            deadKeys.push_back(kv.first);
            continue;
          }
          if (poll.result == HudElemPollResult::Dead) {
            constexpr unsigned long kObjectiveDeadVisualGraceMs = 0UL;
            const bool keepObjectiveGrace =
                (stored.channel == OBJSTAT_NATIVE_OBJECTIVE) &&
                (captureAge <= kObjectiveDeadVisualGraceMs);
            if (!keepObjectiveGrace) {
              deadKeys.push_back(kv.first);
              continue;
            }
          }

          const bool isStatus =
              (expectedChannel == OBJSTAT_NATIVE_STATUS) &&
              IsObjectiveStatusMessageKeyName(stored.key);
          const bool isObjective =
              (expectedChannel == OBJSTAT_NATIVE_OBJECTIVE) &&
              TextHook_OSAuth_IsObjectiveCaptureCandidateKey(stored.key);
          if (!isStatus && !isObjective) {
            continue;
          }

          const uint64_t instanceKey =
              TextHook_OSAuth_MakeInstanceKey(stored.instanceId);
          if (instanceKey == 0) {
            continue;
          }

          VisualCandidate cand{};
          cand.capture = stored;
          cand.lastSeen = stored.captureTick;

          ++visualCount;
          logVisual(cand);

          auto it = outByInstance.find(instanceKey);
          if (it == outByInstance.end() || shouldPreferVisual(it->second, cand)) {
            outByInstance[instanceKey] = cand;
          }
        }
      };
  processVisualStore(objectiveVisualStore, OBJSTAT_NATIVE_OBJECTIVE,
                     objectiveVisualByInstance, deadObjectiveVisualKeys);
  processVisualStore(statusVisualStore, OBJSTAT_NATIVE_STATUS,
                     statusVisualByInstance, deadStatusVisualKeys);

  // Remove dead/reused visuals from the global store so they don't accumulate.
  if (!deadObjectiveVisualKeys.empty() || !deadStatusVisualKeys.empty()) {
    std::lock_guard<std::mutex> lock(g_OSAuthSnapshotMutex);
    for (const uint64_t &dk : deadObjectiveVisualKeys) {
      g_OSAuthObjectiveVisualByInstance.erase(dk);
    }
    for (const uint64_t &dk : deadStatusVisualKeys) {
      g_OSAuthStatusVisualByInstance.erase(dk);
    }
    g_OSAuthWriteVersion.fetch_add(1, std::memory_order_release);
  }
  if (!staleObjectiveProducerKeys.empty()) {
    std::lock_guard<std::mutex> lock(g_OSAuthSnapshotMutex);
    for (const uint64_t &dk : staleObjectiveProducerKeys) {
      g_OSAuthObjectiveTextByInstance.erase(dk);
    }
    g_OSAuthWriteVersion.fetch_add(1, std::memory_order_release);
  }

  std::vector<ObjectiveStatusNativeLine> acceptedLines;
  acceptedLines.reserve(objectiveTextByInstance.size() +
                        statusProducerEvents.size() + 4);
  std::unordered_set<std::string> acceptedStatusKeys;
  acceptedStatusKeys.reserve(statusProducerEvents.size() + 8);

  for (const auto &kv : objectiveTextByInstance) {
    const uint64_t instanceKey = kv.first;
    const ObjectiveTextCapture &capture = kv.second;
    auto itVis = objectiveVisualByInstance.find(instanceKey);
    if (itVis == objectiveVisualByInstance.end()) {
      staleObjectiveProducerKeys.push_back(instanceKey);
      const char *dropReason =
          (hiddenObjectiveVisualInstances.find(instanceKey) !=
           hiddenObjectiveVisualInstances.end())
              ? "obj_hidden_visual"
              : "obj_no_visual";
      logDrop(dropReason, capture.key, capture.instanceId.cfgIndex,
              capture.instanceId, capture.callerRva);
      continue;
    }

    const VisualCandidate &vis = itVis->second;
    logJoinCand("obj", capture.key, capture.instanceId.cfgIndex, 1,
                capture.callerRva);
    if (!vis.capture.key.empty() && vis.capture.key != capture.key) {
      logDrop("obj_key_mismatch", capture.key, capture.instanceId.cfgIndex,
              capture.instanceId, vis.capture.callerRva);
      continue;
    }
    if (capture.instanceId.cfgIndex != 0 && vis.capture.instanceId.cfgIndex != 0 &&
        capture.instanceId.cfgIndex != vis.capture.instanceId.cfgIndex) {
      logDrop("obj_cfg_mismatch", capture.key, capture.instanceId.cfgIndex,
              capture.instanceId, vis.capture.callerRva);
      continue;
    }

    ObjectiveStatusNativeLine line{};
    line.channel = OBJSTAT_NATIVE_OBJECTIVE;
    line.laneSlot = vis.capture.laneSlot;
    line.key = capture.key;
    line.text = TextHook_OSRev_ResolveDisplayText(capture.key, vis.capture.rawText);
    line.cfgIndex = capture.instanceId.cfgIndex;
    line.sourceElemPtrOrIndex = capture.instanceId.elemPtrOrIndex;
    line.fxBirthTime = capture.instanceId.fxBirth;
    line.fxLetterTime = vis.capture.fxLetterTime;
    line.x = vis.capture.x;
    line.y = vis.capture.y;
    line.scale = vis.capture.scale;
    line.fontHeight = vis.capture.fontHeight;
    line.alpha = vis.capture.alpha;
    line.colorPacked = vis.capture.colorPacked;
    line.virtualW = vis.capture.virtualW;
    line.virtualH = vis.capture.virtualH;
    line.lifeStart = vis.capture.captureTick;
    line.lifeEnd = now + 60000UL;  // generous — polling removes dead visuals
    line.authorityFlags = OBJSTAT_LINE_HAS_VISUAL;
    line.instanceSig = instanceKey;
    line.ownerKind = OBJSTAT_OWNER_NATIVE_JOIN;
    line.authorityScore = 320;
    line.producerCallerRva = capture.callerRva;
    line.producerCalleeRva = capture.calleeRva;
    line.drawCallerRva = vis.capture.callerRva;
    line.drawCalleeRva = vis.capture.calleeRva;
    acceptedLines.push_back(line);
    logJoinProof("obj", line, capture.instanceId, vis.capture.callerRva);
  }

  for (const auto &kv : objectiveVisualByInstance) {
    const uint64_t instanceKey = kv.first;
    if (objectiveTextByInstance.find(instanceKey) != objectiveTextByInstance.end()) {
      continue;
    }

    const VisualCandidate &vis = kv.second;
    if (vis.capture.key.empty() ||
        !TextHook_OSAuth_IsObjectiveCaptureCandidateKey(vis.capture.key)) {
      continue;
    }

    ObjectiveStatusNativeLine line{};
    line.channel = OBJSTAT_NATIVE_OBJECTIVE;
    line.ownerKind = OBJSTAT_OWNER_NATIVE_VISUAL_ONLY;
    line.laneSlot = vis.capture.laneSlot;
    line.key = vis.capture.key;
    line.text = TextHook_OSRev_ResolveDisplayText(vis.capture.key,
                                                  vis.capture.rawText);
    line.instanceSig = instanceKey;
    line.cfgIndex = vis.capture.instanceId.cfgIndex;
    line.sourceElemPtrOrIndex = vis.capture.instanceId.elemPtrOrIndex;
    line.fxBirthTime = vis.capture.instanceId.fxBirth;
    line.fxLetterTime = vis.capture.fxLetterTime;
    line.x = vis.capture.x;
    line.y = vis.capture.y;
    line.scale = vis.capture.scale;
    line.fontHeight = vis.capture.fontHeight;
    line.alpha = vis.capture.alpha;
    line.colorPacked = vis.capture.colorPacked;
    line.virtualW = vis.capture.virtualW;
    line.virtualH = vis.capture.virtualH;
    line.lifeStart = vis.capture.captureTick;
    line.lifeEnd = now + 60000UL;
    line.authorityFlags = OBJSTAT_LINE_HAS_VISUAL;
    line.authorityScore = 220;
    line.drawCallerRva = vis.capture.callerRva;
    line.drawCalleeRva = vis.capture.calleeRva;
    acceptedLines.push_back(line);
    logJoinCand("obj_visual", line.key, line.cfgIndex, 1,
                vis.capture.callerRva);
    logJoinProof("obj_visual", line, vis.capture.instanceId,
                 vis.capture.callerRva);
  }

  std::unordered_map<uint64_t, OSAuthStatusProducerEvidence>
      latestStatusProducerByInstance;
  latestStatusProducerByInstance.reserve(statusProducerEvents.size() + 4);
  for (const auto &ev : statusProducerEvents) {
    const uint64_t instanceKey = TextHook_OSAuth_MakeInstanceKey(ev.capture.instanceId);
    const uint64_t producerKey =
        (instanceKey != 0) ? instanceKey : ev.producerInstanceSig;
    if (producerKey == 0) {
      ObjectiveInstanceId invalid{};
      invalid.cfgIndex = ev.capture.instanceId.cfgIndex;
      invalid.fxBirth = ev.capture.instanceId.fxBirth;
      invalid.elemPtrOrIndex = ev.capture.instanceId.elemPtrOrIndex;
      logDrop("status_no_identity", ev.capture.key, ev.capture.instanceId.cfgIndex,
              invalid,
              ev.capture.callerRva);
      continue;
    }
    auto it = latestStatusProducerByInstance.find(producerKey);
    if (it == latestStatusProducerByInstance.end() ||
        ev.capture.captureTick > it->second.capture.captureTick) {
      latestStatusProducerByInstance[producerKey] = ev;
    }
  }

  constexpr unsigned long kStatusVisualFreshMs = 1000UL;
  constexpr unsigned long kStatusProducerMaxRenderMs = 6500UL;
  for (const auto &kv : latestStatusProducerByInstance) {
    const uint64_t producerKey = kv.first;
    const OSAuthStatusProducerEvidence &producer = kv.second;
    const uint64_t visualInstanceKey =
        TextHook_OSAuth_MakeInstanceKey(producer.capture.instanceId);
    logJoinCand("status", producer.capture.key,
                producer.capture.instanceId.cfgIndex,
                statusVisualByInstance.find(visualInstanceKey) ==
                        statusVisualByInstance.end()
                    ? 0u
                    : 1u,
                producer.capture.callerRva);
    auto itVis = statusVisualByInstance.find(visualInstanceKey);
    bool hasVisual = (itVis != statusVisualByInstance.end());
    bool visualKeyOk = true;
    bool visualCfgOk = true;
    const unsigned long producerAge =
        (now >= producer.capture.captureTick)
            ? (now - producer.capture.captureTick)
            : 0;
    unsigned long visualAge = 0;

    if (hasVisual) {
      const VisualCandidate &picked = itVis->second;
      visualAge =
          (now >= picked.capture.captureTick)
              ? (now - picked.capture.captureTick)
              : 0;
      if (!picked.capture.key.empty() && picked.capture.key != producer.capture.key) {
        logDrop("status_key_mismatch", producer.capture.key,
                producer.capture.instanceId.cfgIndex, producer.capture.instanceId,
                picked.capture.callerRva);
        visualKeyOk = false;
      }
      if (visualKeyOk &&
          producer.capture.instanceId.cfgIndex != 0 &&
          picked.capture.instanceId.cfgIndex != 0 &&
          producer.capture.instanceId.cfgIndex != picked.capture.instanceId.cfgIndex) {
        logDrop("status_cfg_mismatch", producer.capture.key,
                producer.capture.instanceId.cfgIndex, producer.capture.instanceId,
                producer.capture.callerRva);
        visualCfgOk = false;
      }
    }

    bool useVisual = hasVisual && visualKeyOk && visualCfgOk &&
                     visualAge <= kStatusVisualFreshMs;
    if (!useVisual && producerAge > kStatusProducerMaxRenderMs) {
      logDrop("status_stale_producer", producer.capture.key,
              producer.capture.instanceId.cfgIndex, producer.capture.instanceId,
              producer.capture.callerRva);
      continue;
    }
    if (!useVisual) {
      logDrop("status_no_visual", producer.capture.key,
              producer.capture.instanceId.cfgIndex, producer.capture.instanceId,
              producer.capture.callerRva);
      continue;
    }

    ObjectiveStatusNativeLine line{};
    line.channel = OBJSTAT_NATIVE_STATUS;
    line.key = producer.capture.key;
    line.text =
        TextHook_OSRev_ResolveDisplayText(producer.capture.key,
            useVisual ? itVis->second.capture.rawText : std::string(""));
    const ObjectiveInstanceId &producerId = producer.capture.instanceId;
    const ObjectiveInstanceId &joinedId =
        (useVisual && itVis->second.capture.instanceId.valid)
            ? itVis->second.capture.instanceId
            : producerId;
    line.cfgIndex =
        (joinedId.cfgIndex != 0) ? joinedId.cfgIndex : producerId.cfgIndex;
    line.instanceSig = producerKey;
    line.sourceElemPtrOrIndex =
        (joinedId.elemPtrOrIndex != 0) ? joinedId.elemPtrOrIndex
                                       : producerId.elemPtrOrIndex;
    line.fxBirthTime =
        (joinedId.fxBirth > 0) ? joinedId.fxBirth : producerId.fxBirth;
    // Status messages should NOT have typewriter, so leave fxLetterTime = 0.

    line.authorityFlags = OBJSTAT_LINE_HAS_VISUAL;
    line.ownerKind = OBJSTAT_OWNER_NATIVE_JOIN;
    line.authorityScore = 300;
    const VisualCandidate &picked = itVis->second;
    line.laneSlot = picked.capture.laneSlot;
    line.x = picked.capture.x;
    line.y = picked.capture.y;
    line.scale = picked.capture.scale;
    line.fontHeight = picked.capture.fontHeight;
    line.alpha = picked.capture.alpha;
    line.colorPacked = picked.capture.colorPacked;
    line.virtualW = picked.capture.virtualW;
    line.virtualH = picked.capture.virtualH;

    line.lifeStart = producer.capture.captureTick;
    line.lifeEnd = producer.capture.captureTick + kStatusProducerMaxRenderMs;

    // Cache joined status seqs to prevent per-frame re-emission.
    // Without this, the same producer triggers TextHook_RecordObjectiveStatusEvent
    // every frame, and after the dedup window (5.2s) expires, a new seq is generated
    // which restarts the renderer's alpha timer.
    struct StatusJoinEntry {
      unsigned int seq;
      DWORD firstJoinTick;
      std::string key;
    };
    static std::unordered_map<uint64_t, StatusJoinEntry> s_statusJoinedSeq;
    // Clear on game reset
    {
      static unsigned long s_statusJoinResetTickSeen = 0;
      const unsigned long sjResetTick = TextHook_GetObjectiveRuntimeResetTick();
      if (sjResetTick != 0 && sjResetTick != s_statusJoinResetTickSeen) {
        s_statusJoinResetTickSeen = sjResetTick;
        s_statusJoinedSeq.clear();
      }
    }
    // Expire old entries.
    // FIX: was 6s — if detour kept firing beyond 6s, new seq was minted on
    // the next rebuild, which restarted the renderer's alpha timer.
    // Use 300s to match the renderer's alpha map expiry.
    for (auto itJ = s_statusJoinedSeq.begin(); itJ != s_statusJoinedSeq.end();) {
      if ((now - itJ->second.firstJoinTick) > 300000) {
        itJ = s_statusJoinedSeq.erase(itJ);
      } else {
        ++itJ;
      }
    }

    auto itJoined = s_statusJoinedSeq.find(producerKey);
    unsigned int cachedSeq = 0;
    if (itJoined != s_statusJoinedSeq.end() &&
        itJoined->second.key == producer.capture.key) {
      cachedSeq = itJoined->second.seq;
    }

    if (cachedSeq != 0) {
      line.sourceSeq = cachedSeq;
    } else {
      const unsigned int cfgForSeq =
          (itVis->second.capture.instanceId.cfgIndex != 0)
              ? itVis->second.capture.instanceId.cfgIndex
              : producer.capture.instanceId.cfgIndex;
      const unsigned int pushedSeq = TextHook_RecordObjectiveStatusEvent(
          producer.capture.key,
          cfgForSeq,
          "osauth_join",
          producerKey);
      line.sourceSeq =
          (pushedSeq != 0) ? pushedSeq : g_OSAuthSeq.fetch_add(1);
      s_statusJoinedSeq[producerKey] = {line.sourceSeq, now, producer.capture.key};
    }
    line.producerCallerRva = producer.capture.callerRva;
    line.producerCalleeRva = producer.capture.calleeRva;
    line.drawCallerRva = itVis->second.capture.callerRva;
    line.drawCalleeRva = itVis->second.capture.calleeRva;
    acceptedLines.push_back(line);
    acceptedStatusKeys.insert(line.key);
    logJoinProof("status", line, itVis->second.capture.instanceId,
                 itVis->second.capture.callerRva);
  }

  // STATUS PROBE FALLBACK — DISABLED.
  // EXE_GAMESAVED and CGAME_NOW_SAVING are now emitted directly by FSHook
  // when DB_FindXAssetHeader fires (exactly once per real save event).
  // The probe-based detection was unreliable: the game's persistent HudElem
  // kept its SLC text index pointing to "Game Saved" for 4+ minutes after
  // alpha→0, causing infinite ghost message spawn-despawn cycles.
  // The FSHook-direct path eliminates ghosts entirely because
  // DB_FindXAssetHeader only fires for genuine engine save events.
  auto candidateLines = std::move(acceptedLines);
  acceptedLines.clear();
  acceptedLines.reserve(candidateLines.size() + 4);
  auto shouldPreferCandidate = [](const ObjectiveStatusNativeLine &dst,
                                  const ObjectiveStatusNativeLine &cand) {
    if (cand.authorityScore != dst.authorityScore) {
      return cand.authorityScore > dst.authorityScore;
    }
    const bool candHasVisual =
        (cand.authorityFlags & OBJSTAT_LINE_HAS_VISUAL) != 0u;
    const bool dstHasVisual =
        (dst.authorityFlags & OBJSTAT_LINE_HAS_VISUAL) != 0u;
    if (candHasVisual != dstHasVisual) {
      return candHasVisual;
    }
    if (cand.sourceElemPtrOrIndex != dst.sourceElemPtrOrIndex) {
      return cand.sourceElemPtrOrIndex != 0;
    }
    if (cand.alpha != dst.alpha) {
      return cand.alpha > dst.alpha;
    }
    if (cand.lifeStart != dst.lifeStart) {
      return cand.lifeStart > dst.lifeStart;
    }
    // Stable tiebreaker: prefer lower lane slot to prevent line jumping
    // when multiple HudElems share the same key.
    if (cand.laneSlot != dst.laneSlot) {
      return cand.laneSlot < dst.laneSlot;
    }
    return false;
  };

  ObjectiveRuntime::WithTopLeftObjectiveRuntimeState(
      [&](ObjectiveRuntime::TopLeftObjectiveRuntimeState &state) {
        for (auto it = state.owners.begin(); it != state.owners.end();) {
          bool erase = false;
          if (it->second.ownerKind == OBJSTAT_OWNER_STATUS_PROBE) {
            erase = (it->second.line.lifeEnd != 0 &&
                     now > it->second.line.lifeEnd);
          } else {
            const unsigned long expiryMs =
                (it->second.channel == OBJSTAT_NATIVE_OBJECTIVE) ? 24UL
                                                                 : 250UL;
            const unsigned long age =
                (it->second.lastSeenTick != 0 && now >= it->second.lastSeenTick)
                    ? (now - it->second.lastSeenTick)
                    : 0;
            erase = (it->second.lastSeenTick != 0 && age > expiryMs);
          }

          if (erase) {
            if (it->second.channel == OBJSTAT_NATIVE_STATUS &&
                it->second.ownerKind != OBJSTAT_OWNER_STATUS_PROBE &&
                !it->second.key.empty()) {
              state.recentNativeStatusKeySeenTick[it->second.key] =
                  (it->second.lastSeenTick != 0) ? it->second.lastSeenTick : now;
            }
            it = state.owners.erase(it);
          } else {
            ++it;
          }
        }
        for (auto itRecent = state.recentNativeStatusKeySeenTick.begin();
             itRecent != state.recentNativeStatusKeySeenTick.end();) {
          const unsigned long age =
              (now >= itRecent->second) ? (now - itRecent->second) : 0;
          if (age > 10000UL) {
            itRecent = state.recentNativeStatusKeySeenTick.erase(itRecent);
          } else {
            ++itRecent;
          }
        }

        std::unordered_map<uint64_t, ObjectiveStatusNativeLine> bestByInstance;
        bestByInstance.reserve(candidateLines.size() + 4);
        for (ObjectiveStatusNativeLine cand : candidateLines) {
          if (cand.instanceSig == 0 ||
              (cand.channel == OBJSTAT_NATIVE_OBJECTIVE && cand.key.empty())) {
            continue;
          }
          auto itBest = bestByInstance.find(cand.instanceSig);
          if (itBest == bestByInstance.end() ||
              shouldPreferCandidate(itBest->second, cand)) {
            bestByInstance[cand.instanceSig] = std::move(cand);
          }
        }

        // Per-key dedup for status AND objective lines — multiple producer
        // instances for the same key cause "tangling" (simultaneous rendering
        // with independent alpha timers).  Keep only the best instance per key
        // within each channel.
        {
          // key -> best instanceSig, separately for status and objective
          std::unordered_map<std::string, uint64_t> bestStatusByKey;
          std::unordered_map<std::string, uint64_t> bestObjByKey;
          for (const auto &kv : bestByInstance) {
            if (kv.second.key.empty()) continue;
            auto *bestMap = (kv.second.channel == OBJSTAT_NATIVE_STATUS)
                                ? &bestStatusByKey
                                : (kv.second.channel == OBJSTAT_NATIVE_OBJECTIVE)
                                      ? &bestObjByKey
                                      : nullptr;
            if (!bestMap) continue;
            auto itBest = bestMap->find(kv.second.key);
            if (itBest == bestMap->end()) {
              (*bestMap)[kv.second.key] = kv.first;
            } else {
              auto itPrev = bestByInstance.find(itBest->second);
              if (itPrev != bestByInstance.end() &&
                  shouldPreferCandidate(itPrev->second, kv.second)) {
                (*bestMap)[kv.second.key] = kv.first;
              }
            }
          }
          for (auto it = bestByInstance.begin(); it != bestByInstance.end();) {
            if (!it->second.key.empty()) {
              const auto *bestMap =
                  (it->second.channel == OBJSTAT_NATIVE_STATUS)
                      ? &bestStatusByKey
                      : (it->second.channel == OBJSTAT_NATIVE_OBJECTIVE)
                            ? &bestObjByKey
                            : nullptr;
              if (bestMap) {
                auto itBest = bestMap->find(it->second.key);
                if (itBest != bestMap->end() &&
                    itBest->second != it->first) {
                  it = bestByInstance.erase(it);
                  continue;
                }
              }
            }
            ++it;
          }
        }

        std::vector<uint64_t> removeAfterAdoption;
        removeAfterAdoption.reserve(4);
        for (auto &kv : bestByInstance) {
          const uint64_t instanceSig = kv.first;
          ObjectiveStatusNativeLine cand = kv.second;
          auto itOwner = state.owners.find(instanceSig);
          if (itOwner != state.owners.end() &&
              itOwner->second.channel != cand.channel) {
            continue;
          }

          if (itOwner == state.owners.end() &&
              cand.channel == OBJSTAT_NATIVE_STATUS &&
              cand.ownerKind != OBJSTAT_OWNER_STATUS_PROBE) {
            for (const auto &ownerKv : state.owners) {
              const ObjectiveStatusNativeOwner &probeOwner = ownerKv.second;
              if (probeOwner.channel != OBJSTAT_NATIVE_STATUS ||
                  probeOwner.ownerKind != OBJSTAT_OWNER_STATUS_PROBE ||
                  probeOwner.key != cand.key) {
                continue;
              }
              if (probeOwner.sourceSeq != 0) {
                cand.sourceSeq = probeOwner.sourceSeq;
              }
              if (probeOwner.line.lifeStart != 0) {
                cand.lifeStart = probeOwner.line.lifeStart;
              }
              removeAfterAdoption.push_back(ownerKv.first);
              break;
            }
          }

          ObjectiveStatusNativeOwner owner{};
          if (itOwner != state.owners.end()) {
            owner = itOwner->second;
          } else {
            owner.instanceSig = instanceSig;
            owner.channel = cand.channel;
            owner.firstSeenTick =
                (cand.lifeStart != 0) ? cand.lifeStart : now;
            owner.resetGeneration = state.generation;
          }

          if (cand.channel == OBJSTAT_NATIVE_STATUS && owner.sourceSeq != 0) {
            cand.sourceSeq = owner.sourceSeq;
          }
          if (cand.lifeStart == 0) {
            cand.lifeStart =
                (owner.line.lifeStart != 0) ? owner.line.lifeStart : now;
          } else if (owner.line.lifeStart != 0 && owner.line.lifeStart < cand.lifeStart) {
            cand.lifeStart = owner.line.lifeStart;
          }

          cand.instanceSig = instanceSig;
          cand.ownerKind = (cand.ownerKind != OBJSTAT_OWNER_NONE)
                               ? cand.ownerKind
                               : owner.ownerKind;
          cand.authorityFlags |= OBJSTAT_LINE_OWNER_LOCKED;
          cand.authorityScore =
              (std::max)(cand.authorityScore, owner.authorityScore);

          owner.instanceSig = instanceSig;
          owner.channel = cand.channel;
          owner.ownerKind = cand.ownerKind;
          owner.key = cand.key;
          owner.laneSlot = cand.laneSlot;
          owner.cfgIndex = cand.cfgIndex;
          owner.fxBirthTime = cand.fxBirthTime;
          owner.elemPtrOrIndex = cand.sourceElemPtrOrIndex;
          owner.authorityScore = cand.authorityScore;
          owner.lastSeenTick = now;
          owner.resetGeneration = state.generation;
          owner.line = cand;
          owner.line.instanceSig = instanceSig;
          owner.line.ownerKind = owner.ownerKind;
          owner.line.authorityScore = owner.authorityScore;
          owner.line.authorityFlags |= OBJSTAT_LINE_OWNER_LOCKED;
          if (cand.channel == OBJSTAT_NATIVE_STATUS) {
            owner.sourceSeq = (cand.sourceSeq != 0) ? cand.sourceSeq
                                                    : owner.sourceSeq;
            owner.line.sourceSeq = owner.sourceSeq;
            if (cand.ownerKind != OBJSTAT_OWNER_STATUS_PROBE &&
                !cand.key.empty()) {
              state.recentNativeStatusKeySeenTick[cand.key] = now;
            }
          }

          // Evict previous owners for the same key+channel to prevent tangling.
          // Only one owner per key should exist at a time within each channel.
          if (!cand.key.empty() &&
              (cand.channel == OBJSTAT_NATIVE_STATUS ||
               cand.channel == OBJSTAT_NATIVE_OBJECTIVE)) {
            for (auto it = state.owners.begin();
                 it != state.owners.end();) {
              if (it->first != instanceSig &&
                  it->second.channel == cand.channel &&
                  it->second.key == cand.key) {
                it = state.owners.erase(it);
              } else {
                ++it;
              }
            }
          }
          state.owners[instanceSig] = owner;
        }

        for (const uint64_t removeSig : removeAfterAdoption) {
          auto itOwner = state.owners.find(removeSig);
          if (itOwner != state.owners.end() &&
              itOwner->second.ownerKind == OBJSTAT_OWNER_STATUS_PROBE) {
            state.owners.erase(itOwner);
          }
        }

        while (state.owners.size() > 64) {
          auto itOldest = state.owners.end();
          for (auto it = state.owners.begin(); it != state.owners.end(); ++it) {
            if (itOldest == state.owners.end() ||
                it->second.lastSeenTick < itOldest->second.lastSeenTick) {
              itOldest = it;
            }
          }
          if (itOldest == state.owners.end()) {
            break;
          }
          state.owners.erase(itOldest);
        }

        for (const auto &ownerKv : state.owners) {
          if (ownerKv.second.line.key.empty()) {
            continue;
          }
          acceptedLines.push_back(ownerKv.second.line);
        }
      });

  std::stable_sort(acceptedLines.begin(), acceptedLines.end(),
                   [](const ObjectiveStatusNativeLine &a,
                      const ObjectiveStatusNativeLine &b) {
                     if (a.channel != b.channel) {
                       return a.channel == OBJSTAT_NATIVE_STATUS;
                     }
                     if (a.laneSlot != b.laneSlot) {
                       return a.laneSlot < b.laneSlot;
                     }
                     if (a.instanceSig != b.instanceSig) {
                       return a.instanceSig < b.instanceSig;
                     }
                     return a.key < b.key;
                   });

  if (kVerboseRuntimeLogs) {
    std::unordered_map<std::string, ObjectiveStatusNativeLine> prevLineBySig;
    std::unordered_map<std::string, ObjectiveStatusNativeLine> currLineBySig;
    prevLineBySig.reserve(previousSnapshotLines.size() + 4);
    currLineBySig.reserve(acceptedLines.size() + 4);

    for (const ObjectiveStatusNativeLine &line : previousSnapshotLines) {
      if (line.key.empty()) {
        continue;
      }
      prevLineBySig[snapshotLineDiagSig(line)] = line;
    }
    for (const ObjectiveStatusNativeLine &line : acceptedLines) {
      if (line.key.empty()) {
        continue;
      }
      currLineBySig[snapshotLineDiagSig(line)] = line;
    }

    for (const auto &kv : currLineBySig) {
      const ObjectiveStatusNativeLine &line = kv.second;
      auto itPrev = prevLineBySig.find(kv.first);
      if (itPrev == prevLineBySig.end()) {
        logLineLifecycle("SPAWN", line, 0, nullptr);
        continue;
      }
      const ObjectiveStatusNativeLine &prev = itPrev->second;
      if (line.laneSlot != prev.laneSlot ||
          line.authorityFlags != prev.authorityFlags ||
          line.sourceElemPtrOrIndex != prev.sourceElemPtrOrIndex ||
          line.fxBirthTime != prev.fxBirthTime) {
        const unsigned long ageMs =
            (now >= line.lifeStart) ? (now - line.lifeStart) : 0;
        logLineLifecycle("MORPH", line, ageMs, &prev);
      }
    }

    for (const auto &kv : prevLineBySig) {
      auto itCurr = currLineBySig.find(kv.first);
      if (itCurr != currLineBySig.end()) {
        continue;
      }
      const ObjectiveStatusNativeLine &line = kv.second;
      const unsigned long ageMs =
          (now >= line.lifeStart) ? (now - line.lifeStart) : 0;
      logLineLifecycle("DESPAWN", line, ageMs, nullptr);
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_OSAuthSnapshotMutex);
    g_OSAuthSnapshotLines = acceptedLines;
    g_OSAuthLastSnapshotTick = now;
  }

  if (kVerboseRuntimeLogs) {
    static unsigned long s_lastStructLogTick = 0;
    if ((now - s_lastStructLogTick) > 1200) {
      s_lastStructLogTick = now;
      char sbuf[256];
      sprintf_s(sbuf,
                "[OSAUTH-STRUCT] objProd=%u statusProd=%u vis=%u lines=%u tick=%lu",
                (unsigned int)objectiveTextByInstance.size(),
                (unsigned int)latestStatusProducerByInstance.size(),
                (unsigned int)visualCount, (unsigned int)acceptedLines.size(),
                (unsigned long)now);
      LogToFile(sbuf);
    }
  }
}

std::vector<ObjectiveStatusNativeLine>
TextHook_GetAuthoritativeObjectiveSnapshot() {
  std::vector<ObjectiveStatusNativeLine> out;
  if (!TextHook_OSAuth_IsEnabled()) {
    return out;
  }

  const unsigned long now = GetTickCount();
  std::lock_guard<std::mutex> lock(g_OSAuthSnapshotMutex);
  TextHook_OSAuth_PruneLocked(now);
  out = g_OSAuthSnapshotLines;
  return out;
}

// OSREV snapshot functions removed — OSAUTH is the sole authority.
