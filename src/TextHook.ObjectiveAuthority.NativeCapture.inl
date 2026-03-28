// =============================================================================
// Objective Authority: native producer capture only
// =============================================================================

static bool ObjRev_IsPlausibleAuthorityCfg(uint32_t cfgIdx) {
  return (cfgIdx > 0 && cfgIdx <= 0xFFFFu);
}

static bool ObjRev_IsTrustedStatusCfgLane(uint32_t cfgIdx,
                                          unsigned long maxAgeMs = 12000) {
  if (!ObjRev_IsPlausibleAuthorityCfg(cfgIdx)) {
    return false;
  }
  return TextHook_IsTrustedObjectiveStatusCfgForKey(
             cfgIdx, "GAME_OBJECTIVESUPDATED", maxAgeMs) ||
         TextHook_IsTrustedObjectiveStatusCfgForKey(
             cfgIdx, "GAME_OBJECTIVECOMPLETED", maxAgeMs) ||
         TextHook_IsTrustedObjectiveStatusCfgForKey(
             cfgIdx, "EXE_GAMESAVED", maxAgeMs) ||
         TextHook_IsTrustedObjectiveStatusCfgForKey(
             cfgIdx, "CGAME_NOW_SAVING", maxAgeMs) ||
         TextHook_IsTrustedObjectiveStatusCfgForKey(
             cfgIdx, "OBJECTIVE_FAILED", maxAgeMs);
}

static void ObjRev_LogAuthoritySkip(const char *reason, const std::string &key,
                                    uint32_t cfgIdx, uint32_t slcIdx, int fxBirth,
                                    int elemIndex, uintptr_t callerOffset) {
  static std::unordered_map<uint64_t, unsigned long> s_authSkipLogTick;
  const unsigned long now = GetTickCount();
  const uint64_t sig =
      ((uint64_t)cfgIdx << 32) ^
      ((uint64_t)(uint16_t)(elemIndex & 0xFFFF) << 16) ^
      (uint64_t)(uint16_t)(fxBirth & 0xFFFF);
  auto it = s_authSkipLogTick.find(sig);
  if (it != s_authSkipLogTick.end() && (now - it->second) <= 1200) {
    return;
  }
  s_authSkipLogTick[sig] = now;
  char sbuf[512];
  sprintf_s(
      sbuf,
      "[AUTH-TEXT-SKIP] reason=%s key=%s cfg=0x%X slc=0x%X fxB=%d elem=%d caller=+0x%llX",
      reason ? reason : "unknown", key.empty() ? "(none)" : key.c_str(), cfgIdx,
      slcIdx, fxBirth, elemIndex, (unsigned long long)callerOffset);
  LogToFile(sbuf);
}

static void ObjRev_RecordAuthoritativeObjectiveCapture(
    const std::string &key, uint32_t cfgText, uint32_t cfgLabel,
    uint32_t slcIndex, int fxBirth, uintptr_t elemPtr, uintptr_t callerOffset,
    bool publishOverlayEvent) {
  const uint32_t cfgIndex = ObjRev_IsPlausibleAuthorityCfg(cfgText)
                                ? cfgText
                                : (ObjRev_IsPlausibleAuthorityCfg(cfgLabel) ? cfgLabel
                                                                           : 0u);
  const int elemIndex = ObjRev_ResolveHudElemArrayIndex(elemPtr);

  if (key.empty() || !ObjRev_LooksLikeLocalizationKey(key.c_str())) {
    ObjRev_LogAuthoritySkip("invalid_key", key, cfgIndex, slcIndex, fxBirth,
                            elemIndex, callerOffset);
    return;
  }
  if (!ObjRev_IsPlausibleAuthorityCfg(cfgIndex)) {
    ObjRev_LogAuthoritySkip("invalid_cfg", key, cfgIndex, slcIndex, fxBirth,
                            elemIndex, callerOffset);
    return;
  }
  if (IsObjectiveStatusMessageKeyName(key)) {
    ObjRev_LogAuthoritySkip("status_key", key, cfgIndex, slcIndex, fxBirth,
                            elemIndex, callerOffset);
    return;
  }
  if (ObjRev_IsTrustedStatusCfgLane(cfgIndex)) {
    ObjRev_LogAuthoritySkip("status_cfg_lane", key, cfgIndex, slcIndex,
                            fxBirth, elemIndex, callerOffset);
    ObjRev_InvalidateAuthoritativeCfg(cfgIndex, "status_cfg_lane");
    return;
  }
  if (!ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset)) {
    ObjRev_LogAuthoritySkip("non_producer_caller", key, cfgIndex, slcIndex,
                            fxBirth, elemIndex, callerOffset);
    return;
  }
  if (fxBirth <= 0) {
    ObjRev_LogAuthoritySkip("missing_fxBirth", key, cfgIndex, slcIndex, fxBirth,
                            elemIndex, callerOffset);
    return;
  }
  if (elemIndex < 0) {
    ObjRev_LogAuthoritySkip("missing_elem_index", key, cfgIndex, slcIndex,
                            fxBirth, elemIndex, callerOffset);
    return;
  }

  AuthoritativeObjectiveTextEvent ev{};
  ev.cfgIndex = cfgIndex;
  ev.slcIndex = slcIndex;
  ev.key = key;
  ev.caller = (unsigned int)callerOffset;
  ev.thread = GetCurrentThreadId();
  ev.tick = GetTickCount();
  ev.frameStamp = ev.tick;
  ev.fxBirth = fxBirth;
  ev.elemIndex = elemIndex;

  {
    std::lock_guard<std::mutex> lock(g_objAuthoritativeCaptureMutex);
    g_objAuthoritativeByInstance[ObjRev_MakeAuthoritativeInstanceKey(
        ev.cfgIndex, ev.fxBirth, ev.elemIndex)] = ev;

    if (g_objAuthoritativeByInstance.size() > 4096) {
      for (auto it = g_objAuthoritativeByInstance.begin();
           it != g_objAuthoritativeByInstance.end();) {
        if ((ev.tick - it->second.tick) > (10 * 60 * 1000)) {
          it = g_objAuthoritativeByInstance.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  TextHook_OSAuth_RecordObjectiveProducerCapture(
      ev.key, ev.cfgIndex, ev.slcIndex, ev.fxBirth, ev.elemIndex, ev.caller,
      0u);

  if (publishOverlayEvent) {
    OverlayDrawEvent evt{};
    evt.frameId = 0;
    evt.tick = ev.tick;
    evt.source = OVERLAY_SOURCE_SLC;
    evt.callerOffset = ev.caller;
    evt.key = ev.key;
    evt.english = ev.key;
    evt.x = 0.0f;
    evt.y = 0.0f;
    evt.sx = 1.0f;
    evt.sy = 1.0f;
    evt.fontH = 0.0f;
    evt.alpha = 1.0f;
    evt.style = 0;
    evt.virtualW = 0;
    evt.virtualH = 0;
    evt.flags = ((unsigned int)(unsigned short)ev.elemIndex) << 16;
    evt.fxBirth = ev.fxBirth;
    evt.fxLetter = 0;
    evt.serverTime = 0;
    evt.channelHint = DetermineObjectiveChannelForKey(ev.key, false);
    TextHook_PublishOverlayEvent(evt);
  }

  static std::unordered_map<uint64_t, unsigned long> s_authLogTick;
  const uint64_t logSig = ObjRev_MakeAuthoritativeInstanceKey(
      ev.cfgIndex, ev.fxBirth, ev.elemIndex);
  auto itLog = s_authLogTick.find(logSig);
  if (itLog == s_authLogTick.end() || (ev.tick - itLog->second) > 1200) {
    s_authLogTick[logSig] = ev.tick;
    char buf[384];
    sprintf_s(buf,
              "[AUTH-TEXT-CAP] key=%s cfg=0x%X slc=0x%X fxB=%d elem=%d caller=+0x%X thr=%u",
              ev.key.c_str(), ev.cfgIndex, ev.slcIndex, ev.fxBirth, ev.elemIndex,
              ev.caller, ev.thread);
    LogToFile(buf);
  }
}

static void ObjRev_RecordAuthoritativeObjectiveCapture(
    const std::string &key, uint32_t cfgText, uint32_t cfgLabel, int fxBirth,
    uintptr_t elemPtr, uintptr_t callerOffset, bool publishOverlayEvent) {
  ObjRev_RecordAuthoritativeObjectiveCapture(
      key, cfgText, cfgLabel, 0, fxBirth, elemPtr, callerOffset,
      publishOverlayEvent);
}

static void ObjRev_InvalidateAuthoritativeInstance(uint32_t cfgIndex,
                                                   int fxBirth, int elemIndex,
                                                   const char *reason) {
  if (!ObjRev_IsPlausibleAuthorityCfg(cfgIndex) || fxBirth <= 0 ||
      elemIndex < 0) {
    return;
  }

  bool removed = false;
  {
    std::lock_guard<std::mutex> lock(g_objAuthoritativeCaptureMutex);
    auto it = g_objAuthoritativeByInstance.find(
        ObjRev_MakeAuthoritativeInstanceKey(cfgIndex, fxBirth, elemIndex));
    if (it != g_objAuthoritativeByInstance.end()) {
      g_objAuthoritativeByInstance.erase(it);
      removed = true;
    }
  }

  if (!removed) {
    return;
  }

  static std::unordered_map<uint64_t, unsigned long>
      s_authInvalidateInstLogTick;
  const unsigned long now = GetTickCount();
  const uint64_t logSig =
      ObjRev_MakeAuthoritativeInstanceKey(cfgIndex, fxBirth, elemIndex);
  auto itLog = s_authInvalidateInstLogTick.find(logSig);
  if (itLog == s_authInvalidateInstLogTick.end() ||
      (now - itLog->second) > 1000) {
    s_authInvalidateInstLogTick[logSig] = now;
    char ibuf[320];
    sprintf_s(
        ibuf,
        "[AUTH-INSTANCE-INVALIDATE] cfg=0x%X fxB=%d elem=%d reason=%s",
        cfgIndex, fxBirth, elemIndex, reason ? reason : "unknown");
    LogToFile(ibuf);
  }
}

static void ObjRev_InvalidateAuthoritativeCfg(uint32_t cfgIndex,
                                              const char *reason) {
  if (cfgIndex == 0) {
    return;
  }

  unsigned int removedInstance = 0;
  {
    std::lock_guard<std::mutex> lock(g_objAuthoritativeCaptureMutex);
    for (auto it = g_objAuthoritativeByInstance.begin();
         it != g_objAuthoritativeByInstance.end();) {
      if (it->second.cfgIndex == cfgIndex) {
        it = g_objAuthoritativeByInstance.erase(it);
        ++removedInstance;
      } else {
        ++it;
      }
    }
  }

  if (removedInstance == 0) {
    return;
  }

  static std::unordered_map<uint32_t, unsigned long> s_authInvalidateLogTick;
  const unsigned long now = GetTickCount();
  auto itLog = s_authInvalidateLogTick.find(cfgIndex);
  if (itLog == s_authInvalidateLogTick.end() ||
      (now - itLog->second) > 1000) {
    s_authInvalidateLogTick[cfgIndex] = now;
    char ibuf[320];
    sprintf_s(ibuf,
              "[AUTH-CFG-INVALIDATE] cfg=0x%X rmInst=%u reason=%s",
              cfgIndex, removedInstance, reason ? reason : "unknown");
    LogToFile(ibuf);
  }
}

static bool ObjRev_GetAuthoritativeObjectiveKey(uint32_t cfgIndex, int fxBirth,
                                                int elemIndex,
                                                std::string &outKey,
                                                const char **outSource) {
  outKey.clear();
  if (!ObjRev_IsPlausibleAuthorityCfg(cfgIndex) || fxBirth <= 0 ||
      elemIndex < 0) {
    return false;
  }

  const unsigned long now = GetTickCount();
  const unsigned long kInstanceTtlMs = 10 * 60 * 1000;
  AuthoritativeObjectiveTextEvent best{};
  bool found = false;

  std::lock_guard<std::mutex> lock(g_objAuthoritativeCaptureMutex);

  for (auto it = g_objAuthoritativeByInstance.begin();
       it != g_objAuthoritativeByInstance.end();) {
    if ((now - it->second.tick) > kInstanceTtlMs) {
      it = g_objAuthoritativeByInstance.erase(it);
    } else {
      ++it;
    }
  }

  auto it = g_objAuthoritativeByInstance.find(
      ObjRev_MakeAuthoritativeInstanceKey(cfgIndex, fxBirth, elemIndex));
  if (it != g_objAuthoritativeByInstance.end()) {
    best = it->second;
    found = true;
  }

  if (!found || best.key.empty() ||
      !ObjRev_LooksLikeLocalizationKey(best.key.c_str()) ||
      IsObjectiveStatusMessageKeyName(best.key)) {
    return false;
  }

  outKey = best.key;
  if (outSource) {
    *outSource = "native_exact";
  }
  return true;
}

static void ObjRev_EnqueuePendingObjectiveAuthorityEvent(
    const std::string &key, uint32_t slcIndex, uintptr_t callerOffset,
    uint32_t ordinal) {
  constexpr unsigned long kPendingAuthorityRetentionMs = 20000;
  if (key.empty() || !ObjRev_LooksLikeLocalizationKey(key.c_str()) ||
      IsObjectiveStatusMessageKeyName(key) ||
      !ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset)) {
    return;
  }

  PendingObjectiveAuthorityEvent ev{};
  ev.key = key;
  ev.slcIndex = slcIndex;
  ev.caller = (unsigned int)callerOffset;
  ev.thread = GetCurrentThreadId();
  ev.tick = GetTickCount();
  ev.ordinal = (ordinal > 0 && ordinal <= 16) ? ordinal : 0;

  bool pushed = false;
  size_t queuedSize = 0;
  {
    std::lock_guard<std::mutex> lock(g_objPendingAuthorityMutex);
    for (auto it = g_objPendingAuthorityEvents.begin();
         it != g_objPendingAuthorityEvents.end();) {
      if ((ev.tick - it->tick) > kPendingAuthorityRetentionMs) {
        it = g_objPendingAuthorityEvents.erase(it);
      } else {
        ++it;
      }
    }

    for (auto it = g_objPendingAuthorityEvents.rbegin();
         it != g_objPendingAuthorityEvents.rend(); ++it) {
      if (it->key == ev.key && it->caller == ev.caller &&
          it->slcIndex == ev.slcIndex && (ev.tick - it->tick) <= 1200) {
        it->tick = ev.tick;
        it->thread = ev.thread;
        if (ev.ordinal > 0) {
          it->ordinal = ev.ordinal;
        }
        return;
      }
    }

    g_objPendingAuthorityEvents.push_back(ev);
    pushed = true;
    while (g_objPendingAuthorityEvents.size() > 64) {
      g_objPendingAuthorityEvents.pop_front();
    }
    queuedSize = g_objPendingAuthorityEvents.size();
  }

  if (pushed) {
    static std::unordered_map<std::string, unsigned long> s_pendingLogTick;
    auto itLog = s_pendingLogTick.find(ev.key);
    if (itLog == s_pendingLogTick.end() || (ev.tick - itLog->second) > 1200) {
      s_pendingLogTick[ev.key] = ev.tick;
      char pbuf[320];
      sprintf_s(
          pbuf,
          "[AUTH-PENDING-QUEUE] key=%s slc=0x%X ord=%u caller=+0x%X size=%zu",
          ev.key.c_str(), ev.slcIndex, ev.ordinal, ev.caller, queuedSize);
      LogToFile(pbuf);
    }
  }
}

static bool ObjRev_ConsumePendingObjectiveAuthorityForLane(
    uint32_t cfgIndex, int fxBirth, int elemIndex, uintptr_t elemPtr, float laneY,
    uint32_t *outSlcIndex) {
  constexpr unsigned long kPendingAuthorityRetentionMs = 20000;
  if (outSlcIndex) {
    *outSlcIndex = 0;
  }
  if (!ObjRev_IsPlausibleAuthorityCfg(cfgIndex) || fxBirth <= 0 || elemIndex < 0 ||
      elemPtr <= 0x10000 || elemPtr >= 0x7FFFFFFFFFFF) {
    return false;
  }
  if (ObjRev_IsTrustedStatusCfgLane(cfgIndex)) {
    static std::unordered_map<uint32_t, unsigned long> s_pendingStatusCfgSkipLog;
    const unsigned long nowSkip = GetTickCount();
    auto itSkip = s_pendingStatusCfgSkipLog.find(cfgIndex);
    if (itSkip == s_pendingStatusCfgSkipLog.end() ||
        (nowSkip - itSkip->second) > 1200) {
      s_pendingStatusCfgSkipLog[cfgIndex] = nowSkip;
      char sbuf[256];
      sprintf_s(sbuf,
                "[AUTH-PENDING-SKIP-STATUSCFG] cfg=0x%X idx=%d fxB=%d",
                cfgIndex, elemIndex, fxBirth);
      LogToFile(sbuf);
    }
    return false;
  }

  const unsigned long now = GetTickCount();
  const int laneSlot =
      std::isfinite(laneY) ? (int)lroundf((laneY - 10.0f) / 20.0f) : -1;

  PendingObjectiveAuthorityEvent chosen{};
  int chosenScore = -1000000;
  size_t chosenIndex = (size_t)-1;

  {
    std::lock_guard<std::mutex> lock(g_objPendingAuthorityMutex);
    for (auto it = g_objPendingAuthorityEvents.begin();
         it != g_objPendingAuthorityEvents.end();) {
      if ((now - it->tick) > kPendingAuthorityRetentionMs) {
        it = g_objPendingAuthorityEvents.erase(it);
      } else {
        ++it;
      }
    }

    for (size_t idx = 0; idx < g_objPendingAuthorityEvents.size(); ++idx) {
      const PendingObjectiveAuthorityEvent &cand = g_objPendingAuthorityEvents[idx];
      if (cand.key.empty() || !ObjRev_LooksLikeLocalizationKey(cand.key.c_str()) ||
          IsObjectiveStatusMessageKeyName(cand.key) ||
          !ObjRev_IsObjectiveAuthoritySlcCaller(cand.caller) ||
          cand.ordinal == 0 || cand.ordinal > 16) {
        continue;
      }

      int score = 0;
      const unsigned long age = (now >= cand.tick) ? (now - cand.tick) : 0;
      if (age > kPendingAuthorityRetentionMs) {
        continue;
      }
      if (age <= 4000) {
        score += (80 - (int)(age / 80));
      } else {
        const int tailPenalty = (int)((age - 4000) / 400);
        score += (std::max)(0, 28 - tailPenalty);
      }

      if (cand.slcIndex > 1) {
        score += 18;
      }

      if (laneSlot >= 0 && laneSlot <= 8) {
        const int ordSlot = (std::max)(0, (std::min)(8, (int)cand.ordinal - 1));
        const int delta = abs(ordSlot - laneSlot);
        if (delta == 0) {
          score += 220;
        } else if (delta == 1) {
          score += 50;
        } else if (delta == 2) {
          // Objective/status lane transitions can shift by 2 slots when a
          // status entry temporarily occupies the top lane.
          score += 8;
        } else {
          score -= 180;
        }
      }

      if (score > chosenScore) {
        chosenScore = score;
        chosenIndex = idx;
        chosen = cand;
      }
    }

    if (chosenIndex == (size_t)-1 || chosenScore < 60) {
      return false;
    }
    // Keep the event in queue for a short window so neighboring lane instances
    // in the same transition can consume the same producer sample.
    g_objPendingAuthorityEvents[chosenIndex].tick = now;
  }

  ObjRev_RecordAuthoritativeObjectiveCapture(chosen.key, cfgIndex, 0,
                                             chosen.slcIndex, fxBirth, elemPtr,
                                             chosen.caller, false);

  std::string verifyKey;
  if (!ObjRev_GetAuthoritativeObjectiveKey(cfgIndex, fxBirth, elemIndex, verifyKey,
                                           nullptr) ||
      verifyKey.empty()) {
    ObjRev_EnqueuePendingObjectiveAuthorityEvent(
        chosen.key, chosen.slcIndex, (uintptr_t)chosen.caller, chosen.ordinal);
    return false;
  }

  if (outSlcIndex) {
    *outSlcIndex = chosen.slcIndex;
  }

  static std::unordered_map<uint64_t, unsigned long> s_pendingConsumeLogTick;
  const uint64_t sig = ObjRev_MakeAuthoritativeInstanceKey(cfgIndex, fxBirth,
                                                           elemIndex);
  auto itLog = s_pendingConsumeLogTick.find(sig);
  if (itLog == s_pendingConsumeLogTick.end() || (now - itLog->second) > 1200) {
    s_pendingConsumeLogTick[sig] = now;
    char cbuf[384];
    sprintf_s(
        cbuf,
        "[AUTH-PENDING-CONSUME] key=%s cfg=0x%X slc=0x%X idx=%d fxB=%d ord=%u score=%d",
        chosen.key.c_str(), cfgIndex, chosen.slcIndex, elemIndex, fxBirth,
        chosen.ordinal, chosenScore);
    LogToFile(cbuf);
  }

  return true;
}

static void ObjRev_ClearPendingObjectiveAuthorityEvents() {
  std::lock_guard<std::mutex> lock(g_objPendingAuthorityMutex);
  g_objPendingAuthorityEvents.clear();
}
