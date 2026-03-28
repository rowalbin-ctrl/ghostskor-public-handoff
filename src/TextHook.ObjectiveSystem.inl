static bool HasObjectiveSuffixOnlyKey(const std::string &key) {
  if (key.rfind("MENU_", 0) == 0) {
    return false;
  }
  if (key.size() > 10 &&
      key.compare(key.size() - 10, 10, "_OBJECTIVE") == 0) {
    return true;
  }
  if (key.size() > 4 && key.compare(key.size() - 4, 4, "_OBJ") == 0) {
    return true;
  }
  return false;
}
static bool IsObjectiveUpdateKeyName(const std::string &key) {
  return (key.find("OBJECTIVESUPDATED") != std::string::npos ||
          key.find("OBJECTIVEUPDATED") != std::string::npos ||
          key.find("OBJECTIVECOMPLETED") != std::string::npos ||
          key.find("OBJECTIVEFAILED") != std::string::npos ||
          key.find("OBJECTIVES_UPDATED") != std::string::npos ||
          key.find("OBJECTIVE_UPDATED") != std::string::npos ||
          key.find("OBJECTIVE_COMPLETED") != std::string::npos ||
          key.find("OBJECTIVE_FAILED") != std::string::npos);
}
static bool IsObjectiveListBootstrapKeyName(const std::string &key) {
  if (key.empty()) {
    return false;
  }
  if (key == "EXE_GAMESAVED" || key == "CGAME_NOW_SAVING" ||
      key == "GAME_OBJECTIVESUPDATED" ||
      IsObjectiveUpdateKeyName(key)) {
    return false;
  }
  if (ObjRev_IsObjectiveListKeyStrict(key)) {
    return true;
  }
  if (key.find("_OBJ_") != std::string::npos ||
      key.find("_OBJECTIVE_") != std::string::npos ||
      HasObjectiveSuffixOnlyKey(key)) {
    return true;
  }
  if (key.rfind("GAME_OBJECTIVE", 0) == 0 ||
      key.rfind("CGAME_OBJECTIVE", 0) == 0 ||
      key.rfind("CORNERED_OBJ_", 0) == 0 ||
      key.rfind("CORNERED_OBJECTIVE_", 0) == 0) {
    return true;
  }
  return false;
}
static bool IsObjectiveOverlayKeyName(const std::string &key) {
  if (key.empty())
    return false;
  if (key == "EXE_GAMESAVED" || key == "CGAME_NOW_SAVING" ||
      key == "CGAME_MISSIONOBJECTIVES")
    return true;
  return IsObjectiveUpdateKeyName(key) || ObjRev_IsObjectiveListKeyStrict(key);
}
static bool IsObjectiveStatusMessageKeyName(const std::string &key) {
  if (key == "EXE_GAMESAVED" || key == "CGAME_NOW_SAVING") {
    return true;
  }
  // Status allowlist expanded to cover update/completed/failed variants.
  return IsObjectiveUpdateKeyName(key);
}
static const char *GetObjectiveStatusKoreanFallback(const std::string &key) {
  if (key == "EXE_GAMESAVED") {
    return "게임 저장됨";
  }
  if (key == "CGAME_NOW_SAVING") {
    return "저장 중...";
  }
  if (key == "GAME_OBJECTIVESUPDATED") {
    return "목표 업데이트.";
  }
  if (key == "GAME_OBJECTIVECOMPLETED") {
    return "목표 완료.";
  }
  if (key == "GAME_OBJECTIVEFAILED") {
    return "목표 실패.";
  }
  return "";
}
static ObjectiveChannel DetermineObjectiveChannelForKey(
    const std::string &key, bool pauseMenuLikely) {
  (void)pauseMenuLikely;
  if (key == "EXE_GAMESAVED") {
    return OBJ_CHANNEL_GAME_SAVED;
  }
  if (key == "CGAME_NOW_SAVING") {
    return OBJ_CHANNEL_SAVE_PROGRESS;
  }

  if (key == "CGAME_MISSIONOBJECTIVES") {
    return OBJ_CHANNEL_PAUSE_HEADER;
  }

  if (IsObjectiveUpdateKeyName(key)) {
    return OBJ_CHANNEL_GAMEPLAY_UPDATE;
  }
  if (ObjRev_IsObjectiveListKeyStrict(key)) {
    return OBJ_CHANNEL_GAMEPLAY_LIST;
  }

  return OBJ_CHANNEL_NONE;
}
static ObjectiveChannel
DetermineObjectiveChannelForOwnerdraw(unsigned short ownerdrawId,
                                      ObjectiveChannel fallback) {
  switch (ownerdrawId) {
  case 99:
    return OBJ_CHANNEL_PAUSE_HEADER;
  case 100:
    // ownerdraw 100 is shared by gameplay objective lines and pause objective list.
    // Keep key-derived channel as the primary truth to avoid misclassifying
    // gameplay objectives into pause-only layout/timing.
    if (fallback != OBJ_CHANNEL_NONE) {
      return fallback;
    }
    return OBJ_CHANNEL_GAMEPLAY_LIST;
  case 111:
    if (fallback == OBJ_CHANNEL_SAVE_PROGRESS)
      return OBJ_CHANNEL_SAVE_PROGRESS;
    return OBJ_CHANNEL_GAME_SAVED;
  default:
    return fallback;
  }
}
static uint64_t NextObjectiveRuntimeInstanceId() {
  return ObjectiveRuntime::NextRuntimeInstanceId();
}
static uint32_t NextObjectiveGeneration() {
  return ObjectiveRuntime::NextGeneration();
}

struct ObjectiveStatusCfgTrustSlot {
  uint32_t cfgIdx = 0;
  unsigned long tick = 0;
  unsigned int hits = 0;
};

struct ObjectiveStatusCfgTrustEntry {
  ObjectiveStatusCfgTrustSlot primary{};
  ObjectiveStatusCfgTrustSlot secondary{};
};

static std::unordered_map<std::string, ObjectiveStatusCfgTrustEntry>
    g_ObjectiveStatusCfgTrustByKey;
static std::mutex g_ObjectiveStatusCfgTrustMutex;
static std::unordered_map<std::string, DWORD> g_ObjectiveStatusEventLogTick;
static std::unordered_map<std::string, DWORD> g_ObjectiveStatusSaveLastPushTick;
static std::unordered_map<std::string, DWORD> g_ObjectiveStatusProbePushTick;
static std::mutex g_ObjectiveStatusProbePushTickMutex;

static bool TextHook_IsPlausibleStatusCfgToken(unsigned int token) {
  return (token > 0 && token <= 0xFFFFu);
}

static void TextHook_PruneObjectiveStatusCfgTrustLocked(unsigned long now) {
  constexpr DWORD kStatusCfgTrustRetentionMs = 180000;
  for (auto it = g_ObjectiveStatusCfgTrustByKey.begin();
       it != g_ObjectiveStatusCfgTrustByKey.end();) {
    auto pruneSlot = [&](ObjectiveStatusCfgTrustSlot &slot) {
      if (slot.cfgIdx == 0) {
        return;
      }
      const unsigned long age = (now >= slot.tick) ? (now - slot.tick) : 0;
      if (age > kStatusCfgTrustRetentionMs) {
        slot = ObjectiveStatusCfgTrustSlot{};
      }
    };
    pruneSlot(it->second.primary);
    pruneSlot(it->second.secondary);
    if (it->second.primary.cfgIdx == 0 && it->second.secondary.cfgIdx == 0) {
      it = g_ObjectiveStatusCfgTrustByKey.erase(it);
    } else {
      ++it;
    }
  }
}

static void TextHook_RecordObjectiveStatusCfgTrust(const std::string &key,
                                                   unsigned int sourceToken,
                                                   unsigned long now,
                                                   const char *sourceTag) {
  if (!IsObjectiveStatusMessageKeyName(key) ||
      !TextHook_IsPlausibleStatusCfgToken(sourceToken)) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_ObjectiveStatusCfgTrustMutex);
  TextHook_PruneObjectiveStatusCfgTrustLocked(now);
  auto &entry = g_ObjectiveStatusCfgTrustByKey[key];
  ObjectiveStatusCfgTrustSlot *slot = nullptr;
  if (entry.primary.cfgIdx == sourceToken && sourceToken != 0) {
    slot = &entry.primary;
  } else if (entry.secondary.cfgIdx == sourceToken && sourceToken != 0) {
    slot = &entry.secondary;
  } else if (entry.primary.cfgIdx == 0) {
    slot = &entry.primary;
  } else if (entry.secondary.cfgIdx == 0) {
    slot = &entry.secondary;
  } else if (entry.primary.tick <= entry.secondary.tick) {
    slot = &entry.primary;
  } else {
    slot = &entry.secondary;
  }

  const uint32_t prevCfg = slot->cfgIdx;
  slot->cfgIdx = sourceToken;
  slot->tick = now;
  slot->hits += 1;

  static std::unordered_map<std::string, DWORD> s_statusCfgTrustLogTick;
  const std::string logSig = key + "|" + std::to_string(sourceToken);
  auto itLog = s_statusCfgTrustLogTick.find(logSig);
  const bool cfgChanged = (prevCfg != 0 && prevCfg != sourceToken);
  if (cfgChanged || itLog == s_statusCfgTrustLogTick.end() ||
      (now - itLog->second) > 2500) {
    s_statusCfgTrustLogTick[logSig] = now;
    char tbuf[320];
    sprintf_s(tbuf,
              "[STATUS-CFG-TRUST] key=%s cfg=0x%X hits=%u prev=0x%X src=%s",
              key.c_str(), sourceToken, slot->hits, prevCfg,
              (sourceTag && sourceTag[0]) ? sourceTag : "unknown");
    LogToFile(tbuf);
  }
}

static bool TextHook_IsTrustedObjectiveStatusCfgForKey(
    uint32_t cfgIdx, const std::string &statusKey, unsigned long maxAgeMs = 180000) {
  if (cfgIdx == 0 || statusKey.empty() ||
      !IsObjectiveStatusMessageKeyName(statusKey)) {
    return false;
  }
  const unsigned long now = GetTickCount();
  std::lock_guard<std::mutex> lock(g_ObjectiveStatusCfgTrustMutex);
  TextHook_PruneObjectiveStatusCfgTrustLocked(now);
  auto it = g_ObjectiveStatusCfgTrustByKey.find(statusKey);
  if (it == g_ObjectiveStatusCfgTrustByKey.end()) {
    return false;
  }
  auto slotMatches = [&](const ObjectiveStatusCfgTrustSlot &slot) {
    if (slot.cfgIdx == 0 || slot.cfgIdx != cfgIdx) {
      return false;
    }
    const unsigned long age = (now >= slot.tick) ? (now - slot.tick) : 0;
    return age <= maxAgeMs;
  };
  return slotMatches(it->second.primary) || slotMatches(it->second.secondary);
}

static uint32_t
TextHook_GetTrustedObjectiveStatusCfgForKey(const std::string &statusKey,
                                            unsigned long maxAgeMs = 180000) {
  if (statusKey.empty() || !IsObjectiveStatusMessageKeyName(statusKey)) {
    return 0;
  }
  const unsigned long now = GetTickCount();
  std::lock_guard<std::mutex> lock(g_ObjectiveStatusCfgTrustMutex);
  TextHook_PruneObjectiveStatusCfgTrustLocked(now);
  auto it = g_ObjectiveStatusCfgTrustByKey.find(statusKey);
  if (it == g_ObjectiveStatusCfgTrustByKey.end()) {
    return 0;
  }
  const ObjectiveStatusCfgTrustSlot *best = nullptr;
  auto tryPick = [&](const ObjectiveStatusCfgTrustSlot &slot) {
    if (slot.cfgIdx == 0) {
      return;
    }
    const unsigned long age = (now >= slot.tick) ? (now - slot.tick) : 0;
    if (age > maxAgeMs) {
      return;
    }
    if (!best || slot.tick > best->tick) {
      best = &slot;
    }
  };
  tryPick(it->second.primary);
  tryPick(it->second.secondary);
  return best ? best->cfgIdx : 0;
}

unsigned int TextHook_RecordObjectiveStatusEvent(const std::string &key,
                                                 unsigned int callerOffset,
                                                 const char *sourceTag,
                                                 uint64_t instanceSig) {
  if (!IsObjectiveStatusMessageKeyName(key)) {
    return 0;
  }

  EnsureTranslationsLoaded();

  std::string korean;
  std::string english;
  TextHook_GetLocalizedKeyText(key, korean, english);
  if (korean.empty()) {
    const char *fallback = GetObjectiveStatusKoreanFallback(key);
    if (fallback && fallback[0]) {
      korean = fallback;
    }
  }
  if (korean.empty()) {
    return 0;
  }
  if (english.empty()) {
    auto itEng = g_KeyToEnglish.find(key);
    english = (itEng != g_KeyToEnglish.end()) ? itEng->second : key;
  }

  const DWORD now = GetTickCount();
  TextHook_RecordObjectiveStatusCfgTrust(key, callerOffset, now, sourceTag);
  constexpr DWORD kStatusEventRetentionMs = 20000;
  // Legacy callers can re-pulse identical key lookups for a short window.
  constexpr DWORD kStatusLegacyDedupMs = 800;
  constexpr DWORD kStatusCfgDedupMs = 700;
  constexpr DWORD kStatusInstanceDedupMs = 1800;
  constexpr DWORD kStatusUpdateInstanceDedupMs = 5200;
  // Save-status producers can linger slightly longer than the visible 5s
  // envelope through fallback/render teardown. Keep dedup beyond that window
  // so the same save event cannot mint a fresh seq after fade-out.
  constexpr DWORD kStatusSaveInstanceDedupMs = 7000;
  constexpr size_t kStatusEventMax = 32;
  const uint32_t expectedSourceCfg =
      TextHook_IsPlausibleStatusCfgToken(callerOffset) ? (uint32_t)callerOffset
                                                        : 0u;
  const bool isSaveStatusKey =
      (key == "EXE_GAMESAVED" || key == "CGAME_NOW_SAVING");
  const bool isUpdateStatusKey =
      (key == "GAME_OBJECTIVESUPDATED" || key == "GAME_OBJECTIVECOMPLETED" ||
       key == "GAME_OBJECTIVEFAILED");
  const DWORD sameInstanceDedupWindow =
      isSaveStatusKey ? kStatusSaveInstanceDedupMs
                      : (isUpdateStatusKey ? kStatusUpdateInstanceDedupMs
                                           : kStatusInstanceDedupMs);

  unsigned int resultSequence = 0;
  ObjectiveRuntime::WithStatusState([&](ObjectiveRuntime::StatusState &state) {
    auto latestSeqForKeyLocked = [&]() -> unsigned int {
      for (auto rit = state.events.rbegin(); rit != state.events.rend(); ++rit) {
        if (rit->key == key) {
          return rit->sequence;
        }
      }
      return 0;
    };
    for (auto it = state.events.begin(); it != state.events.end();) {
      if (now >= it->triggerTick &&
          (now - it->triggerTick) > kStatusEventRetentionMs) {
        it = state.events.erase(it);
      } else {
        ++it;
      }
    }
    if (isSaveStatusKey) {
      for (auto it = g_ObjectiveStatusSaveLastPushTick.begin();
           it != g_ObjectiveStatusSaveLastPushTick.end();) {
        if ((now - it->second) > 20000) {
          it = g_ObjectiveStatusSaveLastPushTick.erase(it);
        } else {
          ++it;
        }
      }
      auto itSave = g_ObjectiveStatusSaveLastPushTick.find(key);
      if (itSave != g_ObjectiveStatusSaveLastPushTick.end()) {
        const DWORD age = (now >= itSave->second) ? (now - itSave->second) : 0;
        if (age <= kStatusSaveInstanceDedupMs) {
          resultSequence = latestSeqForKeyLocked();
          return;
        }
      }
    }

    for (auto it = state.events.rbegin(); it != state.events.rend(); ++it) {
      if (it->key != key) {
        continue;
      }
      const DWORD age =
          (now >= it->triggerTick) ? (now - it->triggerTick) : 0;
      if (instanceSig != 0) {
        if (it->sourceInstanceSig == instanceSig &&
            age <= sameInstanceDedupWindow &&
            (expectedSourceCfg == 0 || it->sourceCfg == expectedSourceCfg)) {
          resultSequence = it->sequence;
          return;
        }
        if (expectedSourceCfg != 0 && it->sourceCfg == expectedSourceCfg) {
          const DWORD cfgDedupWindow =
              isSaveStatusKey ? 2000 : kStatusCfgDedupMs;
          if (age <= cfgDedupWindow) {
            resultSequence = it->sequence;
            return;
          }
        }
        continue;
      }
      if (age <= kStatusLegacyDedupMs) {
        resultSequence = it->sequence;
        return;
      }
      break;
    }

    ObjectiveStatusEvent ev{};
    ev.key = key;
    ev.korean = korean;
    ev.english = english;
    ev.triggerTick = now;
    // Ensure save events survive staleBeforeReset pruning in
    // GetStatusEventsSnapshot.  A checkpoint save can race with
    // SetRuntimeResetTick() — if resetTick >= triggerTick, the event
    // would be immediately pruned as stale.
    if (isSaveStatusKey) {
      const unsigned long curResetTick =
          ObjectiveRuntime::GetRuntimeResetTick();
      if (curResetTick != 0 && ev.triggerTick <= curResetTick) {
        ev.triggerTick = curResetTick + 1;
      }
    }
    ev.callerOffset = callerOffset;
    ev.sourceCfg = expectedSourceCfg;
    if (ev.sourceCfg == 0) {
      ev.sourceCfg = TextHook_GetTrustedObjectiveStatusCfgForKey(key, 1200);
    }
    ev.sourceInstanceSig = instanceSig;
    unsigned int seq = state.sequence++;
    if (seq == 0) {
      seq = state.sequence++;
    }
    ev.sequence = seq;
    state.events.push_back(ev);
    while (state.events.size() > kStatusEventMax) {
      state.events.pop_front();
    }
    if (isSaveStatusKey) {
      g_ObjectiveStatusSaveLastPushTick[key] = now;
    }

    std::string logKey = ev.key + "|" + std::to_string(ev.sequence);
    auto itLog = g_ObjectiveStatusEventLogTick.find(logKey);
    if (itLog == g_ObjectiveStatusEventLogTick.end() ||
        (now - itLog->second) > 1500) {
      g_ObjectiveStatusEventLogTick[logKey] = now;
      char sbuf[512];
      sprintf_s(sbuf,
                "[STATUS-EVT-PUSH] key=%s seq=%u caller=+0x%X cfg=0x%X isig=0x%llX src=%s kor=\"%.64s\"",
                ev.key.c_str(), ev.sequence, callerOffset, ev.sourceCfg,
                (unsigned long long)ev.sourceInstanceSig,
                sourceTag ? sourceTag : "unknown", ev.korean.c_str());
      LogToFile(sbuf);
    }
    resultSequence = ev.sequence;
  });
  return resultSequence;
}

struct ObjectiveStatusProbeSample {
  std::string key;
  unsigned long tick = 0;
  unsigned int callerOffset = 0;
  uint32_t slcValue = 0;
  bool hasRenderParams = false;
};

static std::deque<ObjectiveStatusProbeSample> g_ObjectiveStatusProbeSamples;
static std::mutex g_ObjectiveStatusProbeMutex;

static void TextHook_RecordObjectiveStatusProbe(const std::string &key,
                                                unsigned int callerOffset,
                                                uint32_t slcValue,
                                                bool hasRenderParams) {
  if (!IsObjectiveStatusMessageKeyName(key)) {
    return;
  }

  const DWORD now = GetTickCount();
  constexpr DWORD kProbeRetentionMs = 1400;
  constexpr size_t kProbeMax = 64;

  std::unique_lock<std::mutex> lock(g_ObjectiveStatusProbeMutex);
  for (auto it = g_ObjectiveStatusProbeSamples.begin();
       it != g_ObjectiveStatusProbeSamples.end();) {
    const DWORD age = (now >= it->tick) ? (now - it->tick) : 0;
    if (age > kProbeRetentionMs) {
      it = g_ObjectiveStatusProbeSamples.erase(it);
    } else {
      ++it;
    }
  }

  bool merged = false;
  if (!g_ObjectiveStatusProbeSamples.empty()) {
    ObjectiveStatusProbeSample &last = g_ObjectiveStatusProbeSamples.back();
    const bool sameIdentity =
        (last.key == key && last.callerOffset == callerOffset &&
         last.slcValue == slcValue);
    const DWORD delta = (now >= last.tick) ? (now - last.tick) : 0;
    if (sameIdentity && delta <= 80) {
      last.tick = now;
      last.hasRenderParams = last.hasRenderParams || hasRenderParams;
      merged = true;
    }
  }
  if (!merged) {
    ObjectiveStatusProbeSample sample{};
    sample.key = key;
    sample.tick = now;
    sample.callerOffset = callerOffset;
    sample.slcValue = slcValue;
    sample.hasRenderParams = hasRenderParams;
    g_ObjectiveStatusProbeSamples.push_back(std::move(sample));
    while (g_ObjectiveStatusProbeSamples.size() > kProbeMax) {
      g_ObjectiveStatusProbeSamples.pop_front();
    }
  }

  static std::unordered_map<std::string, DWORD> s_statusProbeLogTick;
  std::string sig = key + "|" + std::to_string(callerOffset);
  auto itLog = s_statusProbeLogTick.find(sig);
  if (itLog == s_statusProbeLogTick.end() || (now - itLog->second) > 500) {
    s_statusProbeLogTick[sig] = now;
    char pbuf[256];
    sprintf_s(
        pbuf,
        "[STATUS-PROBE] key=%s caller=+0x%X slc=0x%X rp=%d",
        key.c_str(), callerOffset, (unsigned int)slcValue,
        hasRenderParams ? 1 : 0);
    LogToFile(pbuf);
  }

  if (key == "GAME_OBJECTIVECOMPLETED") {
    static std::unordered_map<std::string, DWORD> s_statusCompletedProbeLogTick;
    std::string csig = std::to_string(callerOffset) + "|" +
                       std::to_string((unsigned int)slcValue);
    auto itComp = s_statusCompletedProbeLogTick.find(csig);
    if (itComp == s_statusCompletedProbeLogTick.end() ||
        (now - itComp->second) > 400) {
      s_statusCompletedProbeLogTick[csig] = now;
      char cbuf[256];
      sprintf_s(cbuf,
                "[STATUS-PROBE-COMPLETE] key=%s caller=+0x%X slc=0x%X rp=%d",
                key.c_str(), callerOffset, (unsigned int)slcValue,
                hasRenderParams ? 1 : 0);
      LogToFile(cbuf);
    }
  }
  const bool statusProbeRenderable = hasRenderParams;
  if (statusProbeRenderable) {
    lock.unlock();
    static std::unordered_map<std::string, DWORD> s_statusProbeRenderableLogTick;
    const std::string logSig =
        key + "|" + std::to_string(callerOffset) + "|" +
        std::to_string((unsigned int)slcValue);
    auto itLog = s_statusProbeRenderableLogTick.find(logSig);
    if (itLog == s_statusProbeRenderableLogTick.end() ||
        (now - itLog->second) > 900) {
      s_statusProbeRenderableLogTick[logSig] = now;
      const uint32_t trustedCfg =
          TextHook_GetTrustedObjectiveStatusCfgForKey(key, 2500u);
      char pbuf[320];
      sprintf_s(pbuf,
                "[STATUS-PROBE-READY] key=%s caller=+0x%X cfg=0x%X slc=0x%X rp=1",
                key.c_str(), callerOffset, trustedCfg, (unsigned int)slcValue);
      LogToFile(pbuf);
    }
  }

  // For non-save status keys (GAME_OBJECTIVESUPDATED, COMPLETED, FAILED),
  // also record an event in the status event queue.  The OSAUTH native path
  // has its probe fallback disabled, and these keys don't produce status
  // producers, so the event queue + the direct rendering block is the only
  // way to get them on screen.  The event dedup window (5200ms for update
  // keys) prevents duplicate events from per-frame probe calls.
  if (IsObjectiveUpdateKeyName(key)) {
    if (lock.owns_lock()) lock.unlock();
    const uint64_t instanceSig =
        ((uint64_t)(callerOffset & 0xFFFFu) << 16) ^
        (uint64_t)(slcValue & 0x00FFFFFFu);
    TextHook_RecordObjectiveStatusEvent(key, callerOffset,
                                        "status_probe_direct", instanceSig);
  }
}

bool TextHook_GetRecentObjectiveStatusProbe(unsigned long maxAgeMs,
                                            unsigned int requiredCallerOffset,
                                            std::string &outKey,
                                            uint32_t *outSlc,
                                            bool *outHasRenderParams) {
  outKey.clear();
  if (outSlc) {
    *outSlc = 0;
  }
  if (outHasRenderParams) {
    *outHasRenderParams = false;
  }

  const DWORD now = GetTickCount();
  std::lock_guard<std::mutex> lock(g_ObjectiveStatusProbeMutex);
  const ObjectiveStatusProbeSample *bestAny = nullptr;
  const ObjectiveStatusProbeSample *bestWithParams = nullptr;
  for (auto it = g_ObjectiveStatusProbeSamples.rbegin();
       it != g_ObjectiveStatusProbeSamples.rend(); ++it) {
    if (requiredCallerOffset != 0 &&
        it->callerOffset != requiredCallerOffset) {
      continue;
    }
    const DWORD age = (now >= it->tick) ? (now - it->tick) : 0;
    if (age > maxAgeMs) {
      continue;
    }
    if (!bestAny) {
      bestAny = &(*it);
    }
    if (it->hasRenderParams) {
      bestWithParams = &(*it);
      break;
    }
  }

  const ObjectiveStatusProbeSample *picked =
      bestWithParams ? bestWithParams : bestAny;
  if (!picked) {
    return false;
  }

  outKey = picked->key;
  if (outSlc) {
    *outSlc = picked->slcValue;
  }
  if (outHasRenderParams) {
    *outHasRenderParams = picked->hasRenderParams;
  }
  return true;
}

bool TextHook_GetRecentObjectiveStatusProbeByKey(
    unsigned long maxAgeMs, const char *requiredKey,
    unsigned int requiredCallerOffset, std::string &outKey, uint32_t *outSlc,
    bool *outHasRenderParams, unsigned int *outCallerOffset) {
  outKey.clear();
  if (outSlc) {
    *outSlc = 0;
  }
  if (outHasRenderParams) {
    *outHasRenderParams = false;
  }
  if (outCallerOffset) {
    *outCallerOffset = 0;
  }

  if (!requiredKey || !requiredKey[0]) {
    return false;
  }

  const DWORD now = GetTickCount();
  std::lock_guard<std::mutex> lock(g_ObjectiveStatusProbeMutex);
  const ObjectiveStatusProbeSample *bestAny = nullptr;
  const ObjectiveStatusProbeSample *bestWithParams = nullptr;
  for (auto it = g_ObjectiveStatusProbeSamples.rbegin();
       it != g_ObjectiveStatusProbeSamples.rend(); ++it) {
    if (it->key != requiredKey) {
      continue;
    }
    if (requiredCallerOffset != 0 &&
        it->callerOffset != requiredCallerOffset) {
      continue;
    }
    const DWORD age = (now >= it->tick) ? (now - it->tick) : 0;
    if (age > maxAgeMs) {
      continue;
    }
    if (!bestAny) {
      bestAny = &(*it);
    }
    if (it->hasRenderParams) {
      bestWithParams = &(*it);
      break;
    }
  }

  const ObjectiveStatusProbeSample *picked =
      bestWithParams ? bestWithParams : bestAny;
  if (!picked) {
    return false;
  }

  outKey = picked->key;
  if (outSlc) {
    *outSlc = picked->slcValue;
  }
  if (outHasRenderParams) {
    *outHasRenderParams = picked->hasRenderParams;
  }
  if (outCallerOffset) {
    *outCallerOffset = picked->callerOffset;
  }
  return true;
}

std::vector<ObjectiveStatusEvent> TextHook_GetObjectiveStatusEvents() {
  const DWORD now = GetTickCount();
  const unsigned long resetTick = TextHook_GetObjectiveRuntimeResetTick();
  constexpr DWORD kStatusEventRetentionMs = 20000;
  return ObjectiveRuntime::GetStatusEventsSnapshot(now, resetTick,
                                                   kStatusEventRetentionMs);
}

void TextHook_ClearObjectiveStatusEvents() {
  size_t prevCount = 0;
  unsigned int prevSeq = 0;
  ObjectiveRuntime::ClearStatusState(prevCount, prevSeq);
  g_ObjectiveStatusEventLogTick.clear();
  g_ObjectiveStatusSaveLastPushTick.clear();
  {
    std::lock_guard<std::mutex> lock(g_ObjectiveStatusProbeMutex);
    g_ObjectiveStatusProbeSamples.clear();
  }
  {
    std::lock_guard<std::mutex> lock(g_ObjectiveStatusCfgTrustMutex);
    g_ObjectiveStatusCfgTrustByKey.clear();
  }
  {
    std::lock_guard<std::mutex> lk(g_ObjectiveStatusProbePushTickMutex);
    g_ObjectiveStatusProbePushTick.clear();
  }
  const bool isNoopReset = (prevCount == 0 && prevSeq <= 1);
  if (!isNoopReset) {
    char cbuf[192];
    sprintf_s(cbuf, "[STATUS-RESET] events=%u prevSeq=%u",
              (unsigned int)prevCount, prevSeq);
    LogToFile(cbuf);
  }
}

// Capture objective native layout directly from SLC caller-state (RBP/RDI).
// This is the only path that sees mission objective ownerdraw placement in real
// time, so we prefer it over static fallback positioning.
static bool TryCaptureObjectiveNativeFromSlc(const std::string &key,
                                             uintptr_t callerOffset,
                                             uint32_t slcValue) {
  if (RuntimeFlags_ObjectiveStatusEnableOsRevOnly()) {
    return false;
  }
  if (key.empty())
    return false;
  const bool isObjectiveAuthorityCaller =
      ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset);
  const bool isLocalizationKey = ObjRev_LooksLikeLocalizationKey(key.c_str());
  const bool isObjectiveOverlayKey = IsObjectiveOverlayKeyName(key);
  const bool isStrictObjectiveKey = ObjRev_IsObjectiveListKeyStrict(key);
  const bool isBootstrapObjectiveKey =
      (isLocalizationKey && IsObjectiveListBootstrapKeyName(key));
  const bool objectiveCaptureCandidate =
      (isBootstrapObjectiveKey || isObjectiveOverlayKey || isStrictObjectiveKey);
  if (isObjectiveAuthorityCaller) {
    if (!isLocalizationKey) {
      return false;
    }
    if (IsObjectiveStatusMessageKeyName(key)) {
      return false;
    }
    // Keep authority caller strict against noise keys, but do not require only
    // bootstrap keys. Objective-list keys discovered at runtime are valid.
    if (!objectiveCaptureCandidate) {
      return false;
    }
  }
  if (!isObjectiveAuthorityCaller && !objectiveCaptureCandidate)
    return false;

  auto BuildScanLagDiag = [&](DWORD nowTick, char *out, size_t outCap) {
    if (!out || outCap == 0) {
      return;
    }
    unsigned int tcCaller = 0;
    uint32_t tcQuery = 0;
    uint32_t tcType = 0;
    uint32_t tcOrdinal = 0;
    long tcAge = -1;
    if (g_objTypeChkTlsCtx.tick != 0 && nowTick >= g_objTypeChkTlsCtx.tick) {
      tcAge = (long)(nowTick - g_objTypeChkTlsCtx.tick);
      tcCaller = (unsigned int)g_objTypeChkTlsCtx.callerOffset;
      tcQuery = g_objTypeChkTlsCtx.queryIdx;
      tcType = g_objTypeChkTlsCtx.resultType;
      tcOrdinal = g_objTypeChkTlsCtx.ordinal;
    }

    unsigned int tlsCaller = 0;
    uint32_t tlsQuery = 0;
    long tlsAge = -1;
    if (g_objCfgSlcTlsCtx.tick != 0 && nowTick >= g_objCfgSlcTlsCtx.tick) {
      if (slcValue == 0 || g_objCfgSlcTlsCtx.slcIdx == slcValue) {
        tlsAge = (long)(nowTick - g_objCfgSlcTlsCtx.tick);
        tlsCaller = (unsigned int)g_objCfgSlcTlsCtx.callerOffset;
        tlsQuery = g_objCfgSlcTlsCtx.queryIdx;
      }
    }

    ObjCfgSlcTlsContext ringHit{};
    unsigned int ringCaller = 0;
    uint32_t ringQuery = 0;
    long ringAge = -1;
    if (slcValue != 0 &&
        ObjRev_FindRecentCfgSlcTlsForSlc(slcValue, nowTick, 2500, ringHit) &&
        ringHit.tick != 0 && nowTick >= ringHit.tick) {
      ringAge = (long)(nowTick - ringHit.tick);
      ringCaller = (unsigned int)ringHit.callerOffset;
      ringQuery = ringHit.queryIdx;
    }

    sprintf_s(
        out, outCap,
        "lag(tc=%ldms c=+0x%X q=0x%X t=%u ord=%u tls=%ldms c=+0x%X q=0x%X ring=%ldms c=+0x%X q=0x%X)",
        tcAge, tcCaller, tcQuery, (unsigned int)tcType, (unsigned int)tcOrdinal,
        tlsAge, tlsCaller, tlsQuery, ringAge, ringCaller, ringQuery);
  };

  auto LogFailThrottled = [&](const char *reason, uintptr_t rbpVal = 0) {
    // Non-authority paths can probe objective-like keys (e.g., status/hint
    // consumers). Keep objective-fail telemetry exclusive to authority caller.
    if (!isObjectiveAuthorityCaller) {
      return;
    }
    if (reason && strcmp(reason, "bridge_ptrscan_miss") == 0) {
      // bridge miss during pending-join bootstrap is noisy and not actionable.
      return;
    }
    static std::unordered_map<std::string, DWORD> s_objNativeFailLog;
    static std::mutex s_objNativeFailLogMutex;
    DWORD nowFail = GetTickCount();
    std::string sig = std::to_string((unsigned long long)callerOffset) + "|" +
                      (reason ? reason : "unknown");
    bool shouldLog = false;
    {
      std::lock_guard<std::mutex> lk(s_objNativeFailLogMutex);
      auto it = s_objNativeFailLog.find(sig);
      if (it == s_objNativeFailLog.end() || (nowFail - it->second) > 8000) {
        s_objNativeFailLog[sig] = nowFail;
        shouldLog = true;
      }
      if (s_objNativeFailLog.size() > 192) {
        for (auto it2 = s_objNativeFailLog.begin();
             it2 != s_objNativeFailLog.end();) {
          if ((nowFail - it2->second) > 30000) {
            it2 = s_objNativeFailLog.erase(it2);
          } else {
            ++it2;
          }
        }
      }
    }
    if (shouldLog) {
      char lagBuf[320];
      lagBuf[0] = '\0';
      BuildScanLagDiag(nowFail, lagBuf, sizeof(lagBuf));
      char fbuf[768];
      sprintf_s(fbuf,
                "[OBJ-NATIVE-SLC-FAIL] key=%s caller=+0x%llX slc=0x%X reason=%s rbp=0x%llX %s",
                key.c_str(), (unsigned long long)callerOffset,
                (unsigned int)slcValue, reason ? reason : "unknown",
                (unsigned long long)rbpVal, lagBuf);
      LogToFile(fbuf);
    }
  };

  uintptr_t rbp = g_SLC_CallerRegs.rbp;

  auto IsFinite = [](float v) -> bool {
    return (v == v) && std::isfinite(v);
  };
  auto IsPlausibleXY = [&](float v) -> bool {
    return IsFinite(v) && v > -4096.0f && v < 4096.0f;
  };
  auto IsPlausibleScale = [&](float v) -> bool {
    return IsFinite(v) && v > 0.02f && v < 10.0f;
  };

  auto ReadF32Safe = [&](uintptr_t addr, float &out) -> bool {
    if (addr <= 0x10000 || addr >= 0x7FFFFFFFFFFF)
      return false;
    __try {
      out = *(volatile float *)addr;
      return std::isfinite(out) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
    }
  };
  auto ReadU32SafeLocal = [&](uintptr_t addr, uint32_t &out) -> bool {
    if (addr <= 0x10000 || addr >= 0x7FFFFFFFFFFF)
      return false;
    __try {
      out = *(volatile uint32_t *)addr;
      return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
    }
  };
  auto IsReadable = [&](uintptr_t p, size_t bytes) -> bool {
    return (p > 0x10000 && p < 0x7FFFFFFFFFFF && IsBadReadPtr((void *)p, bytes) == 0);
  };
  const auto IsPlausibleCfgIndex = [](uint32_t v) -> bool {
    return (v > 0 && v <= 0xFFFFu);
  };
  auto TryBridgeFromElemPtr = [&](uintptr_t elemPtr, const char *src,
                                  int srcOff) -> bool {
    if (!isObjectiveAuthorityCaller || slcValue == 0) {
      return false;
    }
    if (!IsReadable(elemPtr, 0xA8)) {
      return false;
    }
    uint32_t etype = 0, labelCfg = 0, textCfg = 0, eFlags = 0;
    uint32_t fxBirthU = 0, fxLetterU = 0;
    float ex = 0.0f, ey = 0.0f, efs = 1.0f;
    if (!ReadU32SafeLocal(elemPtr + 0x00, etype) || etype != 1) {
      return false;
    }
    if (!ReadF32Safe(elemPtr + 0x04, ex) || !ReadF32Safe(elemPtr + 0x08, ey) ||
        !ReadF32Safe(elemPtr + 0x14, efs)) {
      return false;
    }
    ReadU32SafeLocal(elemPtr + 0x80, labelCfg);
    ReadU32SafeLocal(elemPtr + 0x84, textCfg);
    ReadU32SafeLocal(elemPtr + 0x90, fxBirthU);
    ReadU32SafeLocal(elemPtr + 0x94, fxLetterU);
    ReadU32SafeLocal(elemPtr + 0xA4, eFlags);

    const uint32_t cfgIndex = IsPlausibleCfgIndex(textCfg)
                                  ? textCfg
                                  : (IsPlausibleCfgIndex(labelCfg) ? labelCfg : 0u);
    if (cfgIndex == 0) {
      return false;
    }
    std::string cfgDirectKey = ObjRev_ResolveConfigKey(cfgIndex, true, true);
    if (cfgDirectKey.empty()) {
      cfgDirectKey = ObjRev_ResolveConfigKey(cfgIndex, false, true);
    }
    if (!cfgDirectKey.empty() &&
        ObjRev_LooksLikeLocalizationKey(cfgDirectKey.c_str())) {
      bool ignoreCfgDirectMismatch = false;
      if (IsObjectiveStatusMessageKeyName(cfgDirectKey)) {
        // cfg->key map can briefly lag during slot churn. If current key is a
        // strict objective key, do not hard-fail on transient status aliasing.
        if (IsObjectiveStatusMessageKeyName(key)) {
          LogFailThrottled("cfg_status_key");
          return false;
        }
        ignoreCfgDirectMismatch = true;
      }
      if (!ignoreCfgDirectMismatch && cfgDirectKey != key) {
        LogFailThrottled("cfg_key_mismatch");
        return false;
      }
    }

    const int elemIndex = ObjRev_ResolveHudElemArrayIndex(elemPtr);
    if (elemIndex < 0) {
      return false;
    }
    const int fxBirth = (int)fxBirthU;

    {
      std::lock_guard<std::mutex> lk(g_slcToKeyMutex);
      g_slcToKey[slcValue] = key;
      if (g_slcToKey.size() > 16384) {
        g_slcToKey.clear();
      }
    }
    ObjRev_RecordDirectCfgSlc(cfgIndex, 0, slcValue, callerOffset);
    ObjRev_RecordAuthoritativeObjectiveCapture(
        key, textCfg, labelCfg, slcValue, fxBirth, elemPtr, callerOffset,
        true);

    // Pre-emptive suppression: move x offscreen NOW (SLC hook runs on game
    // thread BEFORE CG_DrawHudElem) so the very first render frame doesn't
    // flash the English text.  ex was already read above (line 916).
    {
      uint32_t origXBits = 0;
      memcpy(&origXBits, &ex, sizeof(uint32_t));
      TextHook_ObjSuppressElem(elemPtr, origXBits);
      static const float kOff = 9999.0f;
      uint32_t offBits;
      memcpy(&offBits, &kOff, sizeof(uint32_t));
      ObjSuppress_SafeWrite(elemPtr + 0x04, offBits);
    }

    static std::unordered_map<uint64_t, DWORD> s_bridgeLogTick;
    const DWORD nowBridge = GetTickCount();
    const uint64_t sig = ((uint64_t)cfgIndex << 32) ^ (uint64_t)slcValue;
    auto itBridge = s_bridgeLogTick.find(sig);
    if (itBridge == s_bridgeLogTick.end() ||
        (nowBridge - itBridge->second) > 1200) {
      s_bridgeLogTick[sig] = nowBridge;
      char lagBuf[320];
      lagBuf[0] = '\0';
      BuildScanLagDiag(nowBridge, lagBuf, sizeof(lagBuf));
      char bbuf[768];
      sprintf_s(
          bbuf,
          "[OBJ-SLC-CFG-BRIDGE] key=%s slc=0x%X cfg=0x%X idx=%d fxB=%d "
          "ptr=0x%llX x=%.2f y=%.2f fs=%.3f flags=0x%X src=%s%+d %s",
          key.c_str(), (unsigned int)slcValue, (unsigned int)cfgIndex,
          elemIndex, fxBirth, (unsigned long long)elemPtr, ex, ey, efs, eFlags,
          src ? src : "?", srcOff, lagBuf);
      LogToFile(bbuf);
    }
    return true;
  };

  if (isObjectiveAuthorityCaller && slcValue > 0) {
    const DWORD nowBridgeProbe = GetTickCount();
    const uint32_t producerOrdinal =
        (uint32_t)(g_SLC_CallerRegs.rdi & 0xFFFFFFFFu);
    const bool hasRecentTypeChkCtx =
        (g_objTypeChkTlsCtx.tick != 0 && nowBridgeProbe >= g_objTypeChkTlsCtx.tick &&
         (nowBridgeProbe - g_objTypeChkTlsCtx.tick) <= 128 &&
         ObjRev_IsObjectiveAuthorityTypeCheckCaller(
             g_objTypeChkTlsCtx.callerOffset));
    // Deterministic bridge attempt:
    // resolve cfg from a real HudElem pointer reachable from objective-call
    // context pointers. We only accept exact HudElem layout matches.
    const struct ProbeBase {
      const char *name;
      uintptr_t ptr;
    } probeBases[] = {
        {"rsi", g_SLC_CallerRegs.rsi},
        {"rdx", g_SLC_CallerRegs.rdx},
        {"r8", g_SLC_CallerRegs.r8},
        {"r9", g_SLC_CallerRegs.r9},
        {"rdi", g_SLC_CallerRegs.rdi},
        {"r12", g_SLC_CallerRegs.r12},
        {"rsp", g_SLC_CallerRegs.rsp},
        {"rbp", g_SLC_CallerRegs.rbp},
        {"tc.r12", hasRecentTypeChkCtx ? g_objTypeChkTlsCtx.r12 : 0},
        {"tc.rsi", hasRecentTypeChkCtx ? g_objTypeChkTlsCtx.rsi : 0},
        {"tc.rbp", hasRecentTypeChkCtx ? g_objTypeChkTlsCtx.rbp : 0},
        {"tc.rsp", hasRecentTypeChkCtx ? g_objTypeChkTlsCtx.rsp : 0},
    };
    for (const auto &pb : probeBases) {
      if (pb.ptr == 0) {
        continue;
      }
      if (TryBridgeFromElemPtr(pb.ptr, pb.name, 0)) {
        return true;
      }
      if (!IsReadable(pb.ptr, 0x200)) {
        continue;
      }
      for (int off = -0x200; off <= 0x300; off += 8) {
        const intptr_t ai = (intptr_t)pb.ptr + (intptr_t)off;
        if (ai <= 0) {
          continue;
        }
        const uintptr_t a = (uintptr_t)ai;
        if (!IsReadable(a, sizeof(uintptr_t))) {
          continue;
        }
        const uintptr_t q = *(volatile uintptr_t *)a;
        if (TryBridgeFromElemPtr(q, pb.name, off)) {
          return true;
        }
        // Second-level pointer chain:
        // objective context frequently stores nested pointers before the actual
        // hudelem_s pointer appears.
        if (!IsReadable(q, 0x180)) {
          continue;
        }
        for (int off2 = -0x100; off2 <= 0x180; off2 += 8) {
          const intptr_t bi = (intptr_t)q + (intptr_t)off2;
          if (bi <= 0) {
            continue;
          }
          const uintptr_t b = (uintptr_t)bi;
          if (!IsReadable(b, sizeof(uintptr_t))) {
            continue;
          }
          const uintptr_t q2 = *(volatile uintptr_t *)b;
          if (TryBridgeFromElemPtr(q2, pb.name, off + off2)) {
            return true;
          }
        }
      }
    }

    // Secondary deterministic bridge:
    // when direct pointer-chain probing misses, recover cfg from the same
    // objective SLC handoff and resolve the live HudElem pointer by cfg match.
    // This still routes through TryBridgeFromElemPtr() so authority creation
    // remains bound to exact HudElem layout and instance identity.
    auto TryBridgeFromCfgHandoff = [&]() -> bool {
      const DWORD nowJoin = GetTickCount();
      uint32_t handoffQuery = 0;
      uintptr_t handoffCaller = 0;
      const char *handoffSrc = "none";
      DWORD handoffAgeMs = 0xFFFFFFFFu;

      if (g_objCfgSlcTlsCtx.slcIdx == slcValue &&
          g_objCfgSlcTlsCtx.queryIdx != 0 &&
          g_objCfgSlcTlsCtx.tick != 0 &&
          nowJoin >= g_objCfgSlcTlsCtx.tick &&
          (nowJoin - g_objCfgSlcTlsCtx.tick) <= 96) {
        handoffQuery = g_objCfgSlcTlsCtx.queryIdx;
        handoffCaller = g_objCfgSlcTlsCtx.callerOffset;
        handoffSrc = "tls";
        handoffAgeMs = nowJoin - g_objCfgSlcTlsCtx.tick;
      }
      if (handoffQuery == 0) {
        std::lock_guard<std::mutex> lk(g_objCfgSlcRawMutex);
        auto itRaw = g_objCfgSlcRawBySlc.find(slcValue);
        if (itRaw != g_objCfgSlcRawBySlc.end() &&
            itRaw->second.queryIdx != 0 &&
            nowJoin >= itRaw->second.tick &&
            (nowJoin - itRaw->second.tick) <= 500) {
          handoffQuery = itRaw->second.queryIdx;
          handoffCaller = itRaw->second.callerOffset;
          handoffSrc = "map";
          handoffAgeMs = nowJoin - itRaw->second.tick;
        }
      }
      if (handoffQuery == 0) {
        ObjCfgSlcTlsContext ringHit{};
        if (ObjRev_FindRecentCfgSlcTlsForSlc(slcValue, nowJoin, 320, ringHit) &&
            ringHit.queryIdx != 0) {
          handoffQuery = ringHit.queryIdx;
          handoffCaller = ringHit.callerOffset;
          handoffSrc = "tls_ring";
          handoffAgeMs = (nowJoin >= ringHit.tick) ? (nowJoin - ringHit.tick) : 0;
        }
      }
      if (handoffQuery == 0) {
        return false;
      }

      uint32_t cfgIndex = 0;
      uint32_t cfgBias = 0;
      const uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
      if (moduleBase != 0 && handoffCaller != 0) {
        const uintptr_t cfgRetAddr = moduleBase + handoffCaller;
        ObjRev_DecodeCfgBaseFromCaller(handoffQuery, cfgRetAddr, cfgIndex,
                                       cfgBias);
      }
      if (cfgIndex == 0) {
        uint32_t objectiveBias = 0;
        if (ObjRev_ResolveObjectiveBias(objectiveBias) &&
            handoffQuery > objectiveBias) {
          const uint32_t guessCfg = handoffQuery - objectiveBias;
          if (IsPlausibleCfgIndex(guessCfg)) {
            cfgIndex = guessCfg;
            cfgBias = objectiveBias;
          }
        }
      }
      if (cfgIndex == 0) {
        return false;
      }

      uintptr_t hudBase = 0;
      int hudStride = 0;
      int hudCount = 0;
      {
        std::lock_guard<std::mutex> lock(g_hudElemArrayMutex);
        if ((g_hudElemArray.valid || g_hudElemArray.tentative) &&
            g_hudElemArray.baseAddr != 0 && g_hudElemArray.stride > 0 &&
            g_hudElemArray.count > 0) {
          hudBase = g_hudElemArray.baseAddr;
          hudStride = g_hudElemArray.stride;
          hudCount = g_hudElemArray.count;
        }
      }
      if (hudBase == 0 || hudStride <= 0 || hudCount <= 0) {
        return false;
      }

      uintptr_t bestElem = 0;
      int bestIdx = -1;
      int bestScore = -100000;
      float bestX = 0.0f;
      float bestY = 0.0f;
      float bestFs = 1.0f;
      uint32_t bestFlags = 0;
      uint32_t bestFxBirth = 0;
      for (int idx = 0; idx < hudCount; ++idx) {
        const uintptr_t elem = hudBase + (uintptr_t)idx * (uintptr_t)hudStride;
        if (!IsReadable(elem, 0xA8)) {
          continue;
        }
        uint32_t etype = 0, labelCfg = 0, textCfg = 0, fxBirthU = 0, flags = 0;
        if (!ReadU32SafeLocal(elem + 0x00, etype) || etype != 1) {
          continue;
        }
        ReadU32SafeLocal(elem + 0x80, labelCfg);
        ReadU32SafeLocal(elem + 0x84, textCfg);
        ReadU32SafeLocal(elem + 0x90, fxBirthU);
        ReadU32SafeLocal(elem + 0xA4, flags);
        const uint32_t elemCfg = IsPlausibleCfgIndex(textCfg)
                                     ? textCfg
                                     : (IsPlausibleCfgIndex(labelCfg) ? labelCfg
                                                                      : 0u);
        if (elemCfg != cfgIndex) {
          continue;
        }

        float ex = 0.0f, ey = 0.0f, efs = 1.0f;
        const bool hasXY =
            ReadF32Safe(elem + 0x04, ex) && ReadF32Safe(elem + 0x08, ey);
        ReadF32Safe(elem + 0x14, efs);

        int score = 0;
        if (fxBirthU > 0) {
          score += 24;
        } else {
          score -= 16;
        }
        if (hasXY && IsPlausibleXY(ex) && IsPlausibleXY(ey)) {
          score += 6;
          if (ex > -16.0f && ex < 96.0f) {
            score += 6;
          }
          if (ey > -220.0f && ey < 220.0f) {
            score += 8;
          }
        }
        if (IsPlausibleScale(efs)) {
          score += 2;
        }
        if ((flags & 0x5u) != 0) {
          score += 2;
        }

        if (score > bestScore) {
          bestScore = score;
          bestElem = elem;
          bestIdx = idx;
          bestX = ex;
          bestY = ey;
          bestFs = efs;
          bestFlags = flags;
          bestFxBirth = fxBirthU;
        }
      }
      if (bestElem == 0) {
        static std::unordered_map<uint64_t, DWORD> s_cfgScanMissLogTick;
        const uint64_t missSig =
            ((uint64_t)(cfgIndex & 0xFFFFu) << 32) ^
            (uint64_t)(handoffQuery & 0xFFFFFFFFu);
        auto itMiss = s_cfgScanMissLogTick.find(missSig);
        if (itMiss == s_cfgScanMissLogTick.end() ||
            (nowJoin - itMiss->second) > 1500) {
          s_cfgScanMissLogTick[missSig] = nowJoin;
          char lagBuf[320];
          lagBuf[0] = '\0';
          BuildScanLagDiag(nowJoin, lagBuf, sizeof(lagBuf));
          char mbuf[768];
          sprintf_s(
              mbuf,
              "[OBJ-SLC-CFGSCAN-MISS] key=%s slc=0x%X cfg=0x%X q=0x%X src=%s caller=+0x%llX age=%lums hudBase=0x%llX stride=0x%X count=%d %s",
              key.c_str(), (unsigned int)slcValue, (unsigned int)cfgIndex,
              (unsigned int)handoffQuery, handoffSrc,
              (unsigned long long)handoffCaller,
              (unsigned long)((handoffAgeMs == 0xFFFFFFFFu) ? 0u : handoffAgeMs),
              (unsigned long long)hudBase, (unsigned int)hudStride, hudCount,
              lagBuf);
          LogToFile(mbuf);
        }
        return false;
      }

      if (TryBridgeFromElemPtr(bestElem, "cfg_scan", bestIdx)) {
        static std::unordered_map<uint64_t, DWORD> s_cfgScanLogTick;
        const uint64_t sig = ((uint64_t)cfgIndex << 32) ^ (uint64_t)slcValue;
        auto it = s_cfgScanLogTick.find(sig);
        if (it == s_cfgScanLogTick.end() || (nowJoin - it->second) > 1500) {
          s_cfgScanLogTick[sig] = nowJoin;
          char lagBuf[320];
          lagBuf[0] = '\0';
          BuildScanLagDiag(nowJoin, lagBuf, sizeof(lagBuf));
          char cbuf[768];
          sprintf_s(
              cbuf,
              "[OBJ-SLC-CFGSCAN-BRIDGE] key=%s slc=0x%X cfg=0x%X q=0x%X bias=0x%X idx=%d src=%s caller=+0x%llX age=%lums x=%.1f y=%.1f fs=%.2f fxB=%u flags=0x%X score=%d %s",
              key.c_str(), (unsigned int)slcValue, (unsigned int)cfgIndex,
              (unsigned int)handoffQuery, (unsigned int)cfgBias, bestIdx,
              handoffSrc, (unsigned long long)handoffCaller,
              (unsigned long)((handoffAgeMs == 0xFFFFFFFFu) ? 0u : handoffAgeMs),
              bestX, bestY, bestFs, (unsigned int)bestFxBirth,
              (unsigned int)bestFlags, bestScore, lagBuf);
          LogToFile(cbuf);
        }
        return true;
      }
      return false;
    };
    if (TryBridgeFromCfgHandoff()) {
      return true;
    }

    // Tertiary deterministic bridge:
    // use objective producer ordinal (RDI: 1-based lane slot) to select a
    // plausible live objective lane element, then route through exact bridge.
    auto TryBridgeFromOrdinalLane = [&]() -> bool {
      const uint32_t ord = (uint32_t)(g_SLC_CallerRegs.rdi & 0xFFFFFFFFu);
      if (ord == 0 || ord > 16) {
        return false;
      }

      uintptr_t hudBase = 0;
      int hudStride = 0;
      int hudCount = 0;
      {
        std::lock_guard<std::mutex> lock(g_hudElemArrayMutex);
        if ((g_hudElemArray.valid || g_hudElemArray.tentative) &&
            g_hudElemArray.baseAddr != 0 && g_hudElemArray.stride > 0 &&
            g_hudElemArray.count > 0) {
          hudBase = g_hudElemArray.baseAddr;
          hudStride = g_hudElemArray.stride;
          hudCount = g_hudElemArray.count;
        }
      }
      if (hudBase == 0 || hudStride <= 0 || hudCount <= 0) {
        return false;
      }

      struct LaneCand {
        uintptr_t elem = 0;
        int idx = -1;
        uint32_t cfg = 0;
        uint32_t flags = 0;
        int fxBirth = 0;
        float x = 0.0f;
        float y = 0.0f;
        float fs = 1.0f;
        int score = -1000000;
      };
      LaneCand best{};
      const float expectedY = 10.0f + 20.0f * (float)(ord - 1u);
      for (int idx = 0; idx < hudCount; ++idx) {
        const uintptr_t elem = hudBase + (uintptr_t)idx * (uintptr_t)hudStride;
        if (!IsReadable(elem, 0xA8)) {
          continue;
        }
        uint32_t etype = 0, cfgLabel = 0, cfgText = 0, flags = 0;
        uint32_t fxBirthU = 0;
        float ex = 0.0f, ey = 0.0f, efs = 1.0f;
        if (!ReadU32SafeLocal(elem + 0x00, etype) || etype != 1) {
          continue;
        }
        if (!ReadF32Safe(elem + 0x04, ex) || !ReadF32Safe(elem + 0x08, ey) ||
            !ReadF32Safe(elem + 0x14, efs)) {
          continue;
        }
        ReadU32SafeLocal(elem + 0x80, cfgLabel);
        ReadU32SafeLocal(elem + 0x84, cfgText);
        ReadU32SafeLocal(elem + 0x90, fxBirthU);
        ReadU32SafeLocal(elem + 0xA4, flags);
        const uint32_t cfg =
            IsPlausibleCfgIndex(cfgText)
                ? cfgText
                : (IsPlausibleCfgIndex(cfgLabel) ? cfgLabel : 0u);
        if (cfg == 0 || fxBirthU == 0) {
          continue;
        }
        if (ex < -32.0f || ex > 96.0f || ey < -200.0f || ey > 220.0f) {
          continue;
        }
        if ((flags & 0x5u) == 0) {
          continue;
        }

        int score = 0;
        const float dy = fabsf(ey - expectedY);
        const float dx = fabsf(ex - 6.0f);
        score += (int)(400.0f - (dy * 10.0f));
        score += (int)(120.0f - (dx * 8.0f));
        if (efs > 0.6f && efs < 2.2f) {
          score += 32;
        }
        if (dy <= 3.5f) {
          score += 100;
        }
        if (score > best.score) {
          best.elem = elem;
          best.idx = idx;
          best.cfg = cfg;
          best.flags = flags;
          best.fxBirth = (int)fxBirthU;
          best.x = ex;
          best.y = ey;
          best.fs = efs;
          best.score = score;
        }
      }
      if (best.elem == 0 || best.idx < 0) {
        static std::unordered_map<uint64_t, DWORD> s_ordMissLogTick;
        const DWORD nowOrdMiss = GetTickCount();
        const uint64_t missSig =
            ((uint64_t)(ord & 0xFFFFu) << 32) ^ (uint64_t)(slcValue & 0xFFFFFFFFu);
        auto itMiss = s_ordMissLogTick.find(missSig);
        if (itMiss == s_ordMissLogTick.end() ||
            (nowOrdMiss - itMiss->second) > 1500) {
          s_ordMissLogTick[missSig] = nowOrdMiss;
          char lagBuf[320];
          lagBuf[0] = '\0';
          BuildScanLagDiag(nowOrdMiss, lagBuf, sizeof(lagBuf));
          char mbuf[640];
          sprintf_s(
              mbuf,
              "[OBJ-SLC-ORD-MISS] key=%s slc=0x%X ord=%u expectedY=%.1f hudBase=0x%llX stride=0x%X count=%d %s",
              key.c_str(), (unsigned int)slcValue, (unsigned int)ord, expectedY,
              (unsigned long long)hudBase, (unsigned int)hudStride, hudCount,
              lagBuf);
          LogToFile(mbuf);
        }
        return false;
      }

      if (TryBridgeFromElemPtr(best.elem, "ord_scan", (int)ord)) {
        static std::unordered_map<uint64_t, DWORD> s_ordBridgeLogTick;
        const DWORD nowOrd = GetTickCount();
        const uint64_t sig =
            ((uint64_t)(best.cfg & 0xFFFFu) << 32) ^ (uint64_t)(ord & 0xFFFFu);
        auto it = s_ordBridgeLogTick.find(sig);
        if (it == s_ordBridgeLogTick.end() || (nowOrd - it->second) > 1200) {
          s_ordBridgeLogTick[sig] = nowOrd;
          char lagBuf[320];
          lagBuf[0] = '\0';
          BuildScanLagDiag(nowOrd, lagBuf, sizeof(lagBuf));
          char obuf[768];
          sprintf_s(
              obuf,
              "[OBJ-SLC-ORD-BRIDGE] key=%s slc=0x%X ord=%u cfg=0x%X idx=%d x=%.1f y=%.1f fs=%.2f flags=0x%X score=%d %s",
              key.c_str(), (unsigned int)slcValue, (unsigned int)ord,
              (unsigned int)best.cfg, best.idx, best.x, best.y, best.fs,
              (unsigned int)best.flags, best.score, lagBuf);
          LogToFile(obuf);
        }
        return true;
      }
      return false;
    };
    if (TryBridgeFromOrdinalLane()) {
      return true;
    }

    // One-shot reverse telemetry:
    // if bridge fails, locate where this slcValue appears inside live HudElem
    // blobs to recover the true field link between objective SLC event and
    // runtime HudElem instance.
    {
      static std::unordered_map<uint32_t, DWORD> s_slcFootprintLogTick;
      const DWORD nowFp = GetTickCount();
      auto itFp = s_slcFootprintLogTick.find(slcValue);
      if (itFp == s_slcFootprintLogTick.end() ||
          (nowFp - itFp->second) > 15000) {
        s_slcFootprintLogTick[slcValue] = nowFp;
        uintptr_t hudBase = 0;
        int hudStride = 0;
        int hudCount = 0;
        {
          std::lock_guard<std::mutex> lock(g_hudElemArrayMutex);
          if ((g_hudElemArray.valid || g_hudElemArray.tentative) &&
              g_hudElemArray.baseAddr != 0 && g_hudElemArray.stride > 0 &&
              g_hudElemArray.count > 0) {
            hudBase = g_hudElemArray.baseAddr;
            hudStride = g_hudElemArray.stride;
            hudCount = g_hudElemArray.count;
          }
        }
        int fpHits = 0;
        if (hudBase != 0 && hudStride > 0 && hudCount > 0) {
          for (int idx = 0; idx < hudCount && fpHits < 32; ++idx) {
            const uintptr_t elem = hudBase + (uintptr_t)idx * (uintptr_t)hudStride;
            if (!IsReadable(elem, 0xA8)) {
              continue;
            }
            uint32_t etype = 0, cfgLabel = 0, cfgText = 0;
            ReadU32SafeLocal(elem + 0x00, etype);
            ReadU32SafeLocal(elem + 0x80, cfgLabel);
            ReadU32SafeLocal(elem + 0x84, cfgText);
            float ex = 0.0f, ey = 0.0f, efs = 1.0f;
            ReadF32Safe(elem + 0x04, ex);
            ReadF32Safe(elem + 0x08, ey);
            ReadF32Safe(elem + 0x14, efs);

            for (int off = 0; off <= 0xA4; off += 4) {
              uint32_t v = 0;
              if (!ReadU32SafeLocal(elem + (uintptr_t)off, v)) {
                continue;
              }
              if (v != slcValue) {
                continue;
              }
              char fbuf[384];
              sprintf_s(
                  fbuf,
                  "[OBJ-SLC-FOOTPRINT] slc=0x%X key=%s idx=%d elem=0x%llX off=0x%X "
                  "cfgText=0x%X cfgLabel=0x%X type=%u x=%.1f y=%.1f fs=%.2f",
                  (unsigned int)slcValue, key.c_str(), idx,
                  (unsigned long long)elem, off, cfgText, cfgLabel, etype, ex, ey,
                  efs);
              LogToFile(fbuf);
              fpHits += 1;
              break;
            }
          }
        }
        if (fpHits == 0) {
          char mbuf[192];
          sprintf_s(mbuf,
                    "[OBJ-SLC-FOOTPRINT-MISS] slc=0x%X key=%s hudBase=0x%llX stride=0x%X count=%d",
                    (unsigned int)slcValue, key.c_str(),
                    (unsigned long long)hudBase, (unsigned int)hudStride, hudCount);
          LogToFile(mbuf);
        }
      }
    }
    ObjRev_EnqueuePendingObjectiveAuthorityEvent(
        key, slcValue, callerOffset, producerOrdinal);
    // When producer ordinal is valid, pending queue is the intended recovery
    // path for pre-bind timing. Treat pointer-scan miss as non-failure noise.
    const bool hasValidPendingOrdinal =
        (producerOrdinal > 0 && producerOrdinal <= 16);
    if (!hasValidPendingOrdinal) {
      LogFailThrottled("bridge_ptrscan_miss", rbp);
    }
    // Hard guardrail:
    // On authoritative objective caller, never fall through to generic
    // register candidate scan after bridge miss. That path can pick rsp noise
    // (x~2.8,y~0) and produces tiny top-left ghost text.
    return false;
  }

  struct RegBase {
    const char *name;
    uintptr_t ptr;
  };
  const RegBase regs[] = {
      {"rsp", g_SLC_CallerRegs.rsp}, {"rbp", g_SLC_CallerRegs.rbp},
      {"rdi", g_SLC_CallerRegs.rdi},
      {"rsi", g_SLC_CallerRegs.rsi}, {"rbx", g_SLC_CallerRegs.rbx},
      {"r12", g_SLC_CallerRegs.r12}, {"r13", g_SLC_CallerRegs.r13},
      {"r14", g_SLC_CallerRegs.r14}, {"r15", g_SLC_CallerRegs.r15},
  };
  const int offsets[] = {0x00, 0x04, 0x08, 0x0C, 0x10, 0x14,
                         0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C,
                         0x30, 0x34, 0x38, 0x3C, 0x40};

  struct BestCandidate {
    bool valid = false;
    const char *reg = nullptr;
    uintptr_t base = 0;
    int off = 0;
    int score = -999;
    float x = 0.0f;
    float y = 0.0f;
    float sx = 1.0f;
    float sy = 1.0f;
  } best;

  // Primary reverse probe: objective SLC path often carries hudelem_s-like data
  // in RDI. Prefer this over blind register scans when plausible.
  uintptr_t rdiElem = g_SLC_CallerRegs.rdi;
  if (IsReadable(rdiElem, 0xA0)) {
    float ex = 0.0f, ey = 0.0f, escale = 1.0f;
    uint32_t etype = 0, alignOrg = 0, alignScreen = 0;
    uint32_t fadeStart = 0, fadeTime = 0, fxBirth = 0, fxLetter = 0;
    uint32_t fxDecayStart = 0, fxDecayDur = 0;
    uint32_t labelCfg = 0, textCfg = 0;
    const bool hasXY =
        ReadF32Safe(rdiElem + 0x04, ex) && ReadF32Safe(rdiElem + 0x08, ey);
    ReadF32Safe(rdiElem + 0x14, escale);
    ReadU32SafeLocal(rdiElem + 0x00, etype);
    ReadU32SafeLocal(rdiElem + 0x28, alignOrg);
    ReadU32SafeLocal(rdiElem + 0x2C, alignScreen);
    ReadU32SafeLocal(rdiElem + 0x38, fadeStart);
    ReadU32SafeLocal(rdiElem + 0x3C, fadeTime);
    ReadU32SafeLocal(rdiElem + 0x90, fxBirth);
    ReadU32SafeLocal(rdiElem + 0x94, fxLetter);
    ReadU32SafeLocal(rdiElem + 0x98, fxDecayStart);
    ReadU32SafeLocal(rdiElem + 0x9C, fxDecayDur);
    ReadU32SafeLocal(rdiElem + 0x80, labelCfg);
    ReadU32SafeLocal(rdiElem + 0x84, textCfg);

    const bool hasPlausibleCfg =
        IsPlausibleCfgIndex(textCfg) || IsPlausibleCfgIndex(labelCfg);
    const bool hasPlausibleType = (etype >= 1 && etype <= 13);
    const bool hasPlausibleXY =
        hasXY && IsPlausibleXY(ex) && IsPlausibleXY(ey);
    const bool canCaptureAuthoritative =
        hasPlausibleCfg && hasPlausibleType && hasPlausibleXY;

    if (!canCaptureAuthoritative) {
      static std::unordered_map<std::string, DWORD> s_authSkipLogTick;
      const DWORD nowAuthSkip = GetTickCount();
      std::string sigAuthSkip =
          key + "|" + std::to_string((unsigned long long)callerOffset);
      auto itAuthSkip = s_authSkipLogTick.find(sigAuthSkip);
      if (itAuthSkip == s_authSkipLogTick.end() ||
          (nowAuthSkip - itAuthSkip->second) > 1200) {
        s_authSkipLogTick[sigAuthSkip] = nowAuthSkip;
        const char *reason = !hasPlausibleCfg
                                 ? "cfg"
                                 : (!hasPlausibleType ? "type" : "xy");
        char abuf[320];
        sprintf_s(abuf,
                  "[AUTH-TEXT-SKIP] key=%s caller=+0x%llX reason=%s type=%u "
                  "hasXY=%d cfgText=0x%X cfgLabel=0x%X",
                  key.c_str(), (unsigned long long)callerOffset, reason,
                  (unsigned int)etype, hasXY ? 1 : 0, textCfg, labelCfg);
        LogToFile(abuf);
      }
    }

    if ((textCfg > 0 && textCfg <= 0xFFFFu) ||
        (labelCfg > 0 && labelCfg <= 0xFFFFu)) {
      static std::unordered_map<std::string, DWORD> s_objNativeCfgMapLog;
      DWORD nowCfg = GetTickCount();
      std::string sigCfg = key + "|" + std::to_string((unsigned int)textCfg) +
                           "|" + std::to_string((unsigned int)labelCfg);
      auto itCfg = s_objNativeCfgMapLog.find(sigCfg);
      if (itCfg == s_objNativeCfgMapLog.end() || (nowCfg - itCfg->second) > 1200) {
        s_objNativeCfgMapLog[sigCfg] = nowCfg;
        char cbuf[320];
        sprintf_s(cbuf,
                  "[OBJ-NATIVE-CFGMAP] key=%s caller=+0x%llX cfgText=0x%X cfgLabel=0x%X",
                  key.c_str(), (unsigned long long)callerOffset, textCfg, labelCfg);
        LogToFile(cbuf);
      }
    }

    static std::unordered_map<std::string, DWORD> s_objNativeRdiRawLog;
    DWORD nowRdiRaw = GetTickCount();
    std::string sigRdiRaw =
        key + "|" + std::to_string((unsigned long long)callerOffset);
    auto itRdiRaw = s_objNativeRdiRawLog.find(sigRdiRaw);
    if (itRdiRaw == s_objNativeRdiRawLog.end() ||
        (nowRdiRaw - itRdiRaw->second) > 700) {
      s_objNativeRdiRawLog[sigRdiRaw] = nowRdiRaw;
      uint32_t d04 = 0, d08 = 0, d0C = 0, d10 = 0, d14 = 0, d18 = 0, d1C = 0,
               d20 = 0, d24 = 0, d30 = 0, d34 = 0, d40 = 0;
      ReadU32SafeLocal(rdiElem + 0x04, d04);
      ReadU32SafeLocal(rdiElem + 0x08, d08);
      ReadU32SafeLocal(rdiElem + 0x0C, d0C);
      ReadU32SafeLocal(rdiElem + 0x10, d10);
      ReadU32SafeLocal(rdiElem + 0x14, d14);
      ReadU32SafeLocal(rdiElem + 0x18, d18);
      ReadU32SafeLocal(rdiElem + 0x1C, d1C);
      ReadU32SafeLocal(rdiElem + 0x20, d20);
      ReadU32SafeLocal(rdiElem + 0x24, d24);
      ReadU32SafeLocal(rdiElem + 0x30, d30);
      ReadU32SafeLocal(rdiElem + 0x34, d34);
      ReadU32SafeLocal(rdiElem + 0x40, d40);
      char rrbuf[768];
      sprintf_s(
          rrbuf,
          "[OBJ-NATIVE-SLC-RDI-RAW] key=%s caller=+0x%llX elem=0x%llX hasXY=%d "
          "type=%u x=%.2f y=%.2f fs=%.3f ao=%u as=%u fade=%u/%u fx=%u,%u,%u,%u "
          "d04=%u d08=%u d0c=%u d10=%u d14=%u d18=%u d1c=%u d20=%u d24=%u "
          "d30=%u d34=%u d40=%u",
          key.c_str(), (unsigned long long)callerOffset,
          (unsigned long long)rdiElem, hasXY ? 1 : 0, (unsigned int)etype, ex, ey,
          escale, (unsigned int)alignOrg, (unsigned int)alignScreen,
          (unsigned int)fadeStart, (unsigned int)fadeTime, (unsigned int)fxBirth,
          (unsigned int)fxLetter, (unsigned int)fxDecayStart,
          (unsigned int)fxDecayDur, (unsigned int)d04, (unsigned int)d08,
          (unsigned int)d0C, (unsigned int)d10, (unsigned int)d14,
          (unsigned int)d18, (unsigned int)d1C, (unsigned int)d20,
          (unsigned int)d24, (unsigned int)d30, (unsigned int)d34,
          (unsigned int)d40);
      LogToFile(rrbuf);
    }

    if (hasXY && IsPlausibleXY(ex) && IsPlausibleXY(ey)) {
      int rdiScore = 0;
      if (etype >= 1 && etype <= 13) {
        rdiScore += 8;
      }
      if (ex > -96.0f && ex < 640.0f && ey > -220.0f && ey < 380.0f) {
        rdiScore += 8;
      }
      if (IsPlausibleScale(escale)) {
        rdiScore += 3;
      } else {
        escale = 1.0f;
      }
      if ((fabsf(ex) + fabsf(ey)) > 0.01f) {
        rdiScore += 2;
      }

      if (!best.valid || rdiScore > best.score) {
        best.valid = true;
        best.reg = "rdi.elem";
        best.base = rdiElem;
        best.off = 0x04;
        best.score = rdiScore;
        best.x = ex;
        best.y = ey;
        best.sx = escale;
        best.sy = escale;
      }

      static std::unordered_map<std::string, DWORD> s_objNativeRdiLog;
      DWORD nowRdi = GetTickCount();
      std::string sigRdi =
          key + "|" + std::to_string((unsigned long long)callerOffset);
      auto itRdi = s_objNativeRdiLog.find(sigRdi);
      if (itRdi == s_objNativeRdiLog.end() || (nowRdi - itRdi->second) > 1000) {
        s_objNativeRdiLog[sigRdi] = nowRdi;
        char rbuf[512];
        sprintf_s(
            rbuf,
            "[OBJ-NATIVE-SLC-RDI] key=%s caller=+0x%llX elem=0x%llX type=%u x=%.2f "
            "y=%.2f scale=%.3f ao=%u as=%u fade=%u/%u fx=%u,%u,%u,%u",
            key.c_str(), (unsigned long long)callerOffset,
            (unsigned long long)rdiElem, (unsigned int)etype, ex, ey, escale,
            (unsigned int)alignOrg, (unsigned int)alignScreen,
            (unsigned int)fadeStart, (unsigned int)fadeTime, (unsigned int)fxBirth,
            (unsigned int)fxLetter, (unsigned int)fxDecayStart,
            (unsigned int)fxDecayDur);
        LogToFile(rbuf);
      }
    }
  }

  for (const auto &rb : regs) {
    if (!IsReadable(rb.ptr, 0x50))
      continue;

    for (int off : offsets) {
      float cx = 0.0f, cy = 0.0f, csx = 1.0f, csy = 1.0f;
      if (!ReadF32Safe(rb.ptr + off + 0x00, cx) ||
          !ReadF32Safe(rb.ptr + off + 0x04, cy)) {
        continue;
      }
      if (!IsPlausibleXY(cx) || !IsPlausibleXY(cy)) {
        continue;
      }

      int score = 0;
      // Broad sanity window for IW virtual/screen coords.
      if (cx > -1024.0f && cx < 4096.0f && cy > -1024.0f && cy < 4096.0f) {
        score += 2;
      } else {
        continue;
      }
      // Objective track usually lives in top-left-ish area.
      if (cx > -32.0f && cx < 520.0f && cy > -180.0f && cy < 320.0f) {
        score += 3;
      }
      if ((fabsf(cx) + fabsf(cy)) > 1.0f) {
        score += 1;
      } else {
        score -= 3;
      }

      if (ReadF32Safe(rb.ptr + off + 0x08, csx) &&
          ReadF32Safe(rb.ptr + off + 0x0C, csy) && IsPlausibleScale(csx) &&
          IsPlausibleScale(csy)) {
        score += 2;
        if (fabsf(csx - csy) < 0.35f) {
          score += 1;
        }
      } else {
        csx = 1.0f;
        csy = 1.0f;
      }

      if (!best.valid || score > best.score) {
        best.valid = true;
        best.reg = rb.name;
        best.base = rb.ptr;
        best.off = off;
        best.score = score;
        best.x = cx;
        best.y = cy;
        best.sx = csx;
        best.sy = csy;
      }
    }
  }

  if (!best.valid || best.score < 5) {
    static std::unordered_map<std::string, DWORD> s_objNativeRegsLog;
    DWORD nowRegs = GetTickCount();
    std::string sig = key + "|" + std::to_string((unsigned long long)callerOffset);
    auto itRegs = s_objNativeRegsLog.find(sig);
    if (itRegs == s_objNativeRegsLog.end() || (nowRegs - itRegs->second) > 2500) {
      s_objNativeRegsLog[sig] = nowRegs;
      const uintptr_t bestPtr =
          (best.valid && best.base != 0) ? (best.base + (uintptr_t)best.off) : 0;
      char lagBuf[320];
      lagBuf[0] = '\0';
      BuildScanLagDiag(nowRegs, lagBuf, sizeof(lagBuf));
      char rbuf[1024];
      sprintf_s(
          rbuf,
          "[OBJ-NATIVE-SLC-REGS] key=%s caller=+0x%llX rsp=0x%llX rbp=0x%llX rdi=0x%llX "
          "rsi=0x%llX rbx=0x%llX r12=0x%llX r13=0x%llX r14=0x%llX r15=0x%llX "
          "best=%s base=0x%llX off=0x%X ptr=0x%llX score=%d x=%.2f y=%.2f sx=%.3f sy=%.3f %s",
          key.c_str(), (unsigned long long)callerOffset,
          (unsigned long long)g_SLC_CallerRegs.rsp,
          (unsigned long long)g_SLC_CallerRegs.rbp,
          (unsigned long long)g_SLC_CallerRegs.rdi,
          (unsigned long long)g_SLC_CallerRegs.rsi,
          (unsigned long long)g_SLC_CallerRegs.rbx,
          (unsigned long long)g_SLC_CallerRegs.r12,
          (unsigned long long)g_SLC_CallerRegs.r13,
          (unsigned long long)g_SLC_CallerRegs.r14,
          (unsigned long long)g_SLC_CallerRegs.r15, best.reg ? best.reg : "?",
          (unsigned long long)best.base, (unsigned int)best.off,
          (unsigned long long)bestPtr, best.score, best.x, best.y, best.sx,
          best.sy, lagBuf);
      LogToFile(rbuf);
    }
    LogFailThrottled("scan_no_candidate", rbp);
    return false;
  }

  float x = best.x;
  float y = best.y;
  float sx = best.sx;
  float sy = best.sy;
  if (!IsPlausibleScale(sx))
    sx = 1.0f;
  if (!IsPlausibleScale(sy))
    sy = sx;

  static std::unordered_map<std::string, DWORD> s_objNativeCandLog;
  DWORD nowCand = GetTickCount();
  std::string sigCand = key + "|" + std::to_string((unsigned long long)callerOffset);
  auto itCand = s_objNativeCandLog.find(sigCand);
  if (itCand == s_objNativeCandLog.end() || (nowCand - itCand->second) > 1000) {
    s_objNativeCandLog[sigCand] = nowCand;
    const uintptr_t candPtr = best.base + (uintptr_t)best.off;
    char lagBuf[320];
    lagBuf[0] = '\0';
    BuildScanLagDiag(nowCand, lagBuf, sizeof(lagBuf));
    char cbuf[768];
    sprintf_s(cbuf,
              "[OBJ-NATIVE-SLC-CAND] key=%s caller=+0x%llX reg=%s base=0x%llX off=0x%X ptr=0x%llX score=%d "
              "x=%.2f y=%.2f sx=%.3f sy=%.3f %s",
              key.c_str(), (unsigned long long)callerOffset,
              best.reg ? best.reg : "?", (unsigned long long)best.base,
              (unsigned int)best.off, (unsigned long long)candPtr, best.score,
              x, y, sx, sy, lagBuf);
    LogToFile(cbuf);
  }

  HudNativeEntry entry{};
  entry.key = key;
  entry.x = x;
  entry.y = y;
  entry.xScale = sx;
  entry.yScale = sy;
  entry.fontHeight = 78.0f;
  entry.style = 0;
  entry.lastUpdate = GetTickCount();
  entry.source = HUD_NATIVE_RENDER;
  entry.sourceToken = NATIVE_HUD_SOURCE_BRIDGE;
  entry.callerOffset = (unsigned int)callerOffset;
  entry.objectiveLike = true;
  entry.objectiveChannel = (unsigned char)DetermineObjectiveChannelForKey(
      key, TextHook_IsPauseMenuLikely());
  // Prefer IW objective virtual-space when values look like virtual coords.
  if (x < 700.0f && y < 520.0f) {
    entry.virtualW = 0;
    entry.virtualH = 480;
  } else {
    entry.virtualW = 0;
    entry.virtualH = 0;
  }
  entry.valid = !(fabsf(x) < 0.001f && fabsf(y) < 0.001f);
  if (!entry.valid) {
    LogFailThrottled("candidate_zero_xy", rbp);
    return false;
  }

  float fallbackColor[4];
  DetermineHudColor(key, fallbackColor);
  entry.color[0] = fallbackColor[0];
  entry.color[1] = fallbackColor[1];
  entry.color[2] = fallbackColor[2];
  entry.color[3] = fallbackColor[3];

  // Best-effort: hudelem_s carries packed RGBA at +0x30 on this call path.
  uintptr_t elem = g_SLC_CallerRegs.rdi;
  if (elem > 0x10000 && elem < 0x7FFFFFFFFFFF &&
      IsBadReadPtr((void *)elem, 0x34) == 0) {
    uint32_t packed = *(volatile uint32_t *)(elem + 0x30);
    float r = (float)((packed >> 0) & 0xFF) / 255.0f;
    float g = (float)((packed >> 8) & 0xFF) / 255.0f;
    float b = (float)((packed >> 16) & 0xFF) / 255.0f;
    float a = (float)((packed >> 24) & 0xFF) / 255.0f;
    if (IsFinite(r) && IsFinite(g) && IsFinite(b) && IsFinite(a) &&
        r >= 0.0f && r <= 1.0f && g >= 0.0f && g <= 1.0f && b >= 0.0f &&
        b <= 1.0f && a >= 0.0f && a <= 1.0f) {
      entry.color[0] = r;
      entry.color[1] = g;
      entry.color[2] = b;
      entry.color[3] = a;
    }

    float fontScale = *(volatile float *)(elem + 0x14);
    if (IsPlausibleScale(fontScale)) {
      entry.xScale = fontScale;
      entry.yScale = fontScale;
    }
  }

  TextHook_UpdateHudNativeEntry(entry);

  static std::unordered_map<std::string, DWORD> s_objNativeDataLog;
  DWORD nowData = GetTickCount();
  std::string sig = key + "|" + std::to_string((unsigned long long)callerOffset);
  auto itData = s_objNativeDataLog.find(sig);
  if (itData == s_objNativeDataLog.end() || (nowData - itData->second) > 1000) {
    s_objNativeDataLog[sig] = nowData;
    if (s_objNativeDataLog.size() > 192) {
      for (auto it2 = s_objNativeDataLog.begin(); it2 != s_objNativeDataLog.end();) {
        if ((nowData - it2->second) > 30000) {
          it2 = s_objNativeDataLog.erase(it2);
        } else {
          ++it2;
        }
      }
    }
    char lagBuf[320];
    lagBuf[0] = '\0';
    BuildScanLagDiag(nowData, lagBuf, sizeof(lagBuf));
    char sbuf[768];
    sprintf_s(sbuf,
              "[OBJ-NATIVE-SLC-DATA] key=%s caller=+0x%llX x=%.2f y=%.2f sx=%.3f "
              "sy=%.3f vw=%u vh=%u a=%.2f elem=0x%llX %s",
              key.c_str(), (unsigned long long)callerOffset, entry.x, entry.y,
              entry.xScale, entry.yScale, entry.virtualW, entry.virtualH,
              entry.color[3], (unsigned long long)elem, lagBuf);
    LogToFile(sbuf);
  }
  return true;
}

static void RefreshObjectiveOverlayEntry(const std::string &key,
                                         ObjectiveChannel channel,
                                         const std::string *englishRuntime) {
  if (key.empty())
    return;

  if (channel == OBJ_CHANNEL_NONE) {
    channel = DetermineObjectiveChannelForKey(key, TextHook_IsPauseMenuLikely());
    if (channel == OBJ_CHANNEL_NONE)
      return;
  }

  auto itKor = g_KeyToKorean.find(key);
  std::string korean = (itKor != g_KeyToKorean.end()) ? itKor->second : "";
  if (korean.empty()) {
    return;
  }
  auto itEng = g_KeyToEnglish.find(key);
  std::string english = (itEng != g_KeyToEnglish.end()) ? itEng->second : key;
  if (englishRuntime && !englishRuntime->empty()) {
    std::string trimmed = TrimSpaces(*englishRuntime);
    if (!trimmed.empty()) {
      english = trimmed;
    }
  }

  ObjectiveRuntime::WithOverlayState(
      [&](ObjectiveRuntime::OverlayState &overlayState) {
        auto &objectiveEntries = overlayState.entries;
        const DWORD now = GetTickCount();
        constexpr DWORD kReuseWindowMs = 8000;

        ObjectiveOverlayEntry *target = nullptr;
        for (auto &entry : objectiveEntries) {
          if (entry.key != key || entry.channel != channel)
            continue;
          if ((now - entry.lastSeen) > kReuseWindowMs)
            continue;
          if (!target || entry.lastSeen > target->lastSeen) {
            target = &entry;
          }
        }

        if (target) {
          const DWORD ageFirst = now - (DWORD)target->firstSeen;
          if (ageFirst > 9500) {
            target->firstSeen = now;
            target->nativeClockStart = 0;
            target->nativeClockEnd = 0;
            target->consumerOwned = false;
            target->frameSeen = 0;
          }
          target->lastSeen = now;
          target->channel = channel;
          target->korean = korean;
          target->english = english;
          target->frameSeen += 1;
          return;
        }

        ObjectiveOverlayEntry entry{};
        entry.key = key;
        entry.korean = korean;
        entry.english = english;
        entry.channel = channel;
        DetermineHudColor(key, entry.color);
        entry.scale = DetermineHudScale(key);
        entry.fontHeight = 78.0f;
        entry.style = 0;
        entry.nativeX = 0.0f;
        entry.nativeY = 0.0f;
        entry.nativeXScale = entry.scale;
        entry.nativeYScale = entry.scale;
        entry.nativeVirtualW = 0;
        entry.nativeVirtualH = 0;
        entry.callerOffset = 0;
        entry.nativeSource = HUD_NATIVE_RENDER;
        entry.nativeValid = false;
        entry.consumerOwned = false;
        entry.ownerdrawId = 0;
        entry.visibleChars = -1;
        entry.totalChars = -1;
        entry.frameSeen = 1;
        entry.nativeClockStart = 0;
        entry.nativeClockEnd = 0;
        entry.runtimeInstanceId = NextObjectiveRuntimeInstanceId();
        entry.generation = NextObjectiveGeneration();
        entry.firstSeen = now;
        entry.lastSeen = now;
        entry.lastForceSeen = 0;

        if (objectiveEntries.size() >= OBJECTIVE_ENTRY_MAX) {
          auto oldest =
              std::min_element(objectiveEntries.begin(), objectiveEntries.end(),
                               [](const ObjectiveOverlayEntry &a,
                                  const ObjectiveOverlayEntry &b) {
                                 return a.lastSeen < b.lastSeen;
                               });
          if (oldest != objectiveEntries.end()) {
            objectiveEntries.erase(oldest);
          }
        }
        objectiveEntries.push_back(entry);
      });
}
void TextHook_OnObjectiveConsumerSample(const ObjectiveConsumerSample &sample) {
  if (sample.key.empty())
    return;

  EnsureTranslationsLoaded();

  auto itKor = g_KeyToKorean.find(sample.key);
  if (itKor == g_KeyToKorean.end()) {
    return;
  }

  ObjectiveChannel channel =
      DetermineObjectiveChannelForKey(sample.key, TextHook_IsPauseMenuLikely());
  channel = DetermineObjectiveChannelForOwnerdraw(sample.ownerdrawId, channel);
  if (channel == OBJ_CHANNEL_NONE) {
    return;
  }

  DWORD now = sample.tick ? sample.tick : GetTickCount();
  auto AgeSince = [now](DWORD stamp) -> DWORD {
    return (now >= stamp) ? (now - stamp) : 0;
  };
  std::string english = sample.key;
  auto itEng = g_KeyToEnglish.find(sample.key);
  if (itEng != g_KeyToEnglish.end()) {
    english = itEng->second;
  }

  float color[4];
  DetermineHudColor(sample.key, color);
  if (std::isfinite(sample.alpha)) {
    float a = sample.alpha;
    if (a < 0.0f)
      a = 0.0f;
    if (a > 1.0f)
      a = 1.0f;
    color[3] = a;
  }
  float scale = DetermineHudScale(sample.key);
  if (std::isfinite(sample.scale) && sample.scale > 0.02f && sample.scale < 8.0f) {
    scale = sample.scale;
  }
  bool posTrusted =
      std::isfinite(sample.x) && std::isfinite(sample.y) &&
      (sample.callerOffset != 0 || sample.virtualW != 0 || sample.virtualH != 0);
  if (posTrusted) {
    if (sample.x <= -1024.0f || sample.x >= 4096.0f || sample.y <= -1024.0f ||
        sample.y >= 4096.0f) {
      posTrusted = false;
    }
    if (fabsf(sample.x) < 0.001f && fabsf(sample.y) < 0.001f) {
      posTrusted = false;
    }
  }

  ObjectiveRuntime::WithOverlayState(
      [&](ObjectiveRuntime::OverlayState &overlayState) {
  auto &g_ObjectiveOverlay = overlayState.entries;

  constexpr DWORD kReuseWindowMs = 8000;
  ObjectiveOverlayEntry *target = nullptr;
  const ObjectiveOverlayEntry *donorNative = nullptr;
  ObjectiveOverlayEntry *nonConsumerTarget = nullptr;
  for (auto &e : g_ObjectiveOverlay) {
    if (e.key != sample.key)
      continue;
    if (e.channel != channel)
      continue;
    if (sample.ownerdrawId != 0 && e.ownerdrawId != 0 &&
        e.ownerdrawId != sample.ownerdrawId)
      continue;

    if (!e.consumerOwned) {
      // Track FSHook entry for potential merge (instead of creating duplicate)
      if (!nonConsumerTarget || e.lastSeen > nonConsumerTarget->lastSeen) {
        nonConsumerTarget = &e;
      }
      if (e.nativeValid &&
          (!donorNative || e.lastSeen > donorNative->lastSeen)) {
        donorNative = &e;
      }
      continue;
    }

    if (AgeSince(e.lastSeen) > kReuseWindowMs)
      continue;
    if (!target || e.lastSeen > target->lastSeen) {
      target = &e;
    }
  }

  auto ApplyDonorNative = [&](ObjectiveOverlayEntry &dst) {
    if (!donorNative || !donorNative->nativeValid)
      return;
    dst.nativeX = donorNative->nativeX;
    dst.nativeY = donorNative->nativeY;
    dst.nativeXScale = donorNative->nativeXScale;
    dst.nativeYScale = donorNative->nativeYScale;
    dst.nativeVirtualW = donorNative->nativeVirtualW;
    dst.nativeVirtualH = donorNative->nativeVirtualH;
    if (dst.callerOffset == 0) {
      dst.callerOffset = donorNative->callerOffset;
    }
    dst.nativeSource = donorNative->nativeSource;
    dst.nativeValid = true;
    if (dst.fontHeight <= 5.0f && donorNative->fontHeight > 5.0f) {
      dst.fontHeight = donorNative->fontHeight;
    }
  };

  if (!target) {
    // If a non-consumer (FSHook) entry already exists for this key+channel,
    // MERGE consumer data (visibleChars, position) into it instead of
    // creating a duplicate. FSHook keeps lifecycle ownership.
    if (nonConsumerTarget) {
      // Enrich with position if trusted
      if (posTrusted) {
        nonConsumerTarget->nativeX = sample.x;
        nonConsumerTarget->nativeY = sample.y;
        nonConsumerTarget->nativeXScale = scale;
        nonConsumerTarget->nativeYScale = scale;
        if (sample.virtualW != 0 || sample.virtualH != 0) {
          nonConsumerTarget->nativeVirtualW = sample.virtualW;
          nonConsumerTarget->nativeVirtualH = sample.virtualH;
        }
        nonConsumerTarget->callerOffset = sample.callerOffset;
        nonConsumerTarget->nativeValid = true;
        nonConsumerTarget->nativeSource = HUD_NATIVE_RENDER;
      } else if (!nonConsumerTarget->nativeValid) {
        ApplyDonorNative(*nonConsumerTarget);
      }
      // Enrich with typewriter data
      nonConsumerTarget->visibleChars =
          (sample.visibleChars >= 0 && sample.visibleChars <= 32767)
              ? (short)sample.visibleChars
              : nonConsumerTarget->visibleChars;
      nonConsumerTarget->totalChars =
          (sample.totalChars >= 0 && sample.totalChars <= 32767)
              ? (short)sample.totalChars
              : nonConsumerTarget->totalChars;
      // Update timing (but NOT firstSeen ??lifecycle stays FSHook-controlled)
      if (nonConsumerTarget->nativeClockStart == 0)
        nonConsumerTarget->nativeClockStart = now;
      if (now > nonConsumerTarget->nativeClockEnd)
        nonConsumerTarget->nativeClockEnd = now;
      if (now > nonConsumerTarget->lastSeen)
        nonConsumerTarget->lastSeen = now;
      if (sample.ownerdrawId != 0)
        nonConsumerTarget->ownerdrawId = sample.ownerdrawId;
      // Do NOT set consumerOwned=true ??FSHook keeps refreshing lastSeen
      static DWORD s_lastMergeLog = 0;
      if ((now - s_lastMergeLog) > 2000) {
        s_lastMergeLog = now;
        char mbuf[256];
        sprintf_s(mbuf, "[OBJ-CONS-MERGE] key=%s vis=%d/%d pos=(%.1f,%.1f)",
                  sample.key.c_str(), (int)nonConsumerTarget->visibleChars,
                  (int)nonConsumerTarget->totalChars,
                  nonConsumerTarget->nativeX, nonConsumerTarget->nativeY);
        LogToFile(mbuf);
      }
      return;
    }
    if (g_ObjectiveOverlay.size() >= OBJECTIVE_ENTRY_MAX) {
      auto oldest =
          std::min_element(g_ObjectiveOverlay.begin(), g_ObjectiveOverlay.end(),
                           [](const ObjectiveOverlayEntry &a,
                              const ObjectiveOverlayEntry &b) {
                             return a.lastSeen < b.lastSeen;
                           });
      if (oldest != g_ObjectiveOverlay.end()) {
        g_ObjectiveOverlay.erase(oldest);
      }
    }
    ObjectiveOverlayEntry created{};
    created.key = sample.key;
    created.korean = itKor->second;
    created.english = english;
    created.channel = channel;
    created.color[0] = color[0];
    created.color[1] = color[1];
    created.color[2] = color[2];
    created.color[3] = color[3];
    created.scale = scale;
    created.fontHeight = 78.0f;
    created.style = 0;
    created.nativeX = posTrusted ? sample.x : 0.0f;
    created.nativeY = posTrusted ? sample.y : 0.0f;
    created.nativeXScale = scale;
    created.nativeYScale = scale;
    created.nativeVirtualW = posTrusted ? sample.virtualW : 0;
    created.nativeVirtualH = posTrusted ? sample.virtualH : 0;
    created.callerOffset = posTrusted ? sample.callerOffset : 0;
    created.nativeSource = HUD_NATIVE_RENDER;
    created.nativeValid = posTrusted;
    if (!posTrusted) {
      ApplyDonorNative(created);
    }
    created.consumerOwned = true;
    created.ownerdrawId = sample.ownerdrawId;
    created.visibleChars =
        (sample.visibleChars >= 0 && sample.visibleChars <= 32767)
            ? (short)sample.visibleChars
            : (short)-1;
    created.totalChars =
        (sample.totalChars >= 0 && sample.totalChars <= 32767)
            ? (short)sample.totalChars
            : (short)-1;
    created.frameSeen = sample.frameId;
    created.nativeClockStart = now;
    created.nativeClockEnd = now;
    created.runtimeInstanceId = NextObjectiveRuntimeInstanceId();
    created.generation = NextObjectiveGeneration();
    created.firstSeen = now;
    created.lastSeen = now;
    g_ObjectiveOverlay.push_back(created);
    return;
  }

  target->korean = itKor->second;
  target->english = english;
  target->channel = channel;
  target->color[0] = color[0];
  target->color[1] = color[1];
  target->color[2] = color[2];
  target->color[3] = color[3];
  target->scale = scale;
  bool acceptPos = posTrusted;
  if (posTrusted && sample.callerOffset == 0 && target->callerOffset == 0 &&
      target->nativeValid) {
    float dx = fabsf(sample.x - target->nativeX);
    float dy = fabsf(sample.y - target->nativeY);
    if ((dx + dy) > 10.0f && AgeSince(target->lastSeen) <= 200) {
      acceptPos = false;
      static std::unordered_map<std::string, DWORD> s_objJitterDropLog;
      auto itJ = s_objJitterDropLog.find(sample.key);
      if (itJ == s_objJitterDropLog.end() || (now - itJ->second) > 1500) {
        s_objJitterDropLog[sample.key] = now;
        char jbuf[320];
        sprintf_s(jbuf,
                  "[OBJ-CONS-JITTER-DROP] key=%s dx=%.2f dy=%.2f old=(%.2f,%.2f) "
                  "new=(%.2f,%.2f)",
                  sample.key.c_str(), dx, dy, target->nativeX, target->nativeY,
                  sample.x, sample.y);
        LogToFile(jbuf);
      }
    }
  }
  if (acceptPos) {
    target->nativeX = sample.x;
    target->nativeY = sample.y;
    if (sample.virtualW != 0 || sample.virtualH != 0) {
      target->nativeVirtualW = sample.virtualW;
      target->nativeVirtualH = sample.virtualH;
    } else if (!target->nativeValid) {
      target->nativeVirtualW = 0;
      target->nativeVirtualH = 0;
    }
  }
  if (posTrusted || !target->nativeValid) {
    target->nativeXScale = scale;
    target->nativeYScale = scale;
  }
  if (posTrusted) {
    target->callerOffset = sample.callerOffset;
  }
  target->nativeSource = HUD_NATIVE_RENDER;
  if (posTrusted && acceptPos) {
    target->nativeValid = true;
  } else if (!target->nativeValid) {
    ApplyDonorNative(*target);
  }
  target->consumerOwned = true;
  if (sample.ownerdrawId != 0) {
    target->ownerdrawId = sample.ownerdrawId;
  }
  target->visibleChars =
      (sample.visibleChars >= 0 && sample.visibleChars <= 32767)
          ? (short)sample.visibleChars
          : (short)-1;
  target->totalChars =
      (sample.totalChars >= 0 && sample.totalChars <= 32767)
          ? (short)sample.totalChars
          : (short)-1;
  if (sample.frameId != 0) {
    target->frameSeen = sample.frameId;
  } else {
    target->frameSeen += 1;
  }
  if (target->nativeClockStart == 0) {
    target->nativeClockStart = now;
  }
  if (now > target->nativeClockEnd) {
    target->nativeClockEnd = now;
  }
  if (now > target->lastSeen) {
    target->lastSeen = now;
  }
      });
}
void TextHook_OnObjectiveLocalizeLookup(const char *key,
                                        const char *englishValue,
                                        bool loadingPhaseLikely,
                                        bool forceGameplayList) {
  if (!key || !key[0]) {
    return;
  }

  EnsureTranslationsLoaded();

  std::string keyStr(key);
  if (keyStr.find("INTROSCREEN") != std::string::npos) {
    return;
  }
  // Status notifications are rendered through a dedicated runtime event path.
  // Keeping them out of objective preload/overlay state avoids lane pollution.
  if (IsObjectiveStatusMessageKeyName(keyStr)) {
    return;
  }

  auto itKor = g_KeyToKorean.find(keyStr);
  if (itKor == g_KeyToKorean.end() || itKor->second.empty()) {
    return;
  }

  std::string englishRuntime;
  if (englishValue && englishValue[0]) {
    englishRuntime = TrimSpaces(englishValue);
  }
  (void)forceGameplayList;

  ObjectiveChannel channel =
      DetermineObjectiveChannelForKey(keyStr, TextHook_IsPauseMenuLikely());
  if (channel == OBJ_CHANNEL_NONE) {
    return;
  }
  // During active gameplay, native objective lanes are already authoritative.
  // Late FSHook localize lookups can resurrect dead objectives and create
  // ghost/preload overlays. Keep preload only for load-phase or explicit force.
  if (!loadingPhaseLikely && !forceGameplayList &&
      TextHook_HasRecentObjectiveLaneActivity(1200)) {
    return;
  }

  const std::string *engPtr = englishRuntime.empty() ? nullptr : &englishRuntime;
  RefreshObjectiveOverlayEntry(keyStr, channel, engPtr);

  const DWORD now = GetTickCount();
  ObjectiveRuntime::WithOverlayState(
      [&](ObjectiveRuntime::OverlayState &overlayState) {
    auto &g_ObjectiveOverlay = overlayState.entries;
    auto &g_ObjectiveKeyObservations = overlayState.keyObservations;
    auto &g_ObjectiveLoadStickyUntil = overlayState.loadStickyUntil;
    ObjectiveKeyObservation &obs = g_ObjectiveKeyObservations[keyStr];
    obs.key = keyStr;
    obs.channel = channel;
    obs.lastSeen = now;

    if (g_ObjectiveKeyObservations.size() > 256) {
      for (auto it = g_ObjectiveKeyObservations.begin();
           it != g_ObjectiveKeyObservations.end();) {
        if ((now - it->second.lastSeen) > 12000) {
          it = g_ObjectiveKeyObservations.erase(it);
        } else {
          ++it;
        }
      }
    }

    if (loadingPhaseLikely) {
      constexpr DWORD kLoadStickyMs = 90000;
      g_ObjectiveLoadStickyUntil[keyStr] = now + kLoadStickyMs;
    }

    // lastFSHookSeen should represent runtime (live) objective activity only.
    // DB load-phase lookups must not mark it, otherwise stale keys dominate
    // fallback selection and misbind active lanes.
    if (!forceGameplayList && !loadingPhaseLikely) {
      for (auto &e : g_ObjectiveOverlay) {
        if (e.key != keyStr) {
          continue;
        }
        if (channel != OBJ_CHANNEL_NONE && e.channel != channel) {
          continue;
        }
        e.lastFSHookSeen = now;
      }
    }
    if (forceGameplayList) {
      for (auto &e : g_ObjectiveOverlay) {
        if (e.key != keyStr) {
          continue;
        }
        if (channel != OBJ_CHANNEL_NONE && e.channel != channel) {
          continue;
        }
        e.lastForceSeen = now;
      }
    }
      });

  static std::mutex s_preloadLogMtx;
  static std::unordered_map<std::string, DWORD> s_preloadLogTick;
  std::string sig = keyStr + "|" + std::to_string((int)channel) + "|" +
                    (loadingPhaseLikely ? "1" : "0") + "|" +
                    (forceGameplayList ? "1" : "0");
  {
    std::lock_guard<std::mutex> lock(s_preloadLogMtx);
    auto it = s_preloadLogTick.find(sig);
    if (it == s_preloadLogTick.end() || (now - it->second) > 2000) {
      s_preloadLogTick[sig] = now;
      if (s_preloadLogTick.size() > 512) {
        for (auto it2 = s_preloadLogTick.begin();
             it2 != s_preloadLogTick.end();) {
          if ((now - it2->second) > 20000) {
            it2 = s_preloadLogTick.erase(it2);
          } else {
            ++it2;
          }
        }
      }

      char buf[512];
      sprintf_s(buf,
                "[OBJ-PRELOAD] key=%s ch=%d load=%d force=%d eng=\"%.96s\"",
                keyStr.c_str(), (int)channel, loadingPhaseLikely ? 1 : 0,
                forceGameplayList ? 1 : 0, englishRuntime.c_str());
      LogToFile(buf);
    }
  }
}

