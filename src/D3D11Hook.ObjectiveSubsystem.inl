static const char *ObjectiveChannelName(ObjectiveChannel ch) {
  switch (ch) {
  case OBJ_CHANNEL_GAMEPLAY_LIST:
    return "gameplay_list";
  case OBJ_CHANNEL_GAMEPLAY_UPDATE:
    return "gameplay_update";
  case OBJ_CHANNEL_PAUSE_LIST:
    return "pause_list";
  case OBJ_CHANNEL_PAUSE_HEADER:
    return "pause_header";
  case OBJ_CHANNEL_GAME_SAVED:
    return "game_saved";
  case OBJ_CHANNEL_SAVE_PROGRESS:
    return "save_progress";
  default:
    return "none";
  }
}

static bool ReadF32Safe(uintptr_t addr, float &out) {
  __try {
    out = *(volatile float *)addr;
    return std::isfinite(out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool ReadU32Safe(uintptr_t addr, uint32_t &out) {
  __try {
    out = *(volatile uint32_t *)addr;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool ReadU64Safe(uintptr_t addr, uint64_t &out) {
  __try {
    out = *(volatile uint64_t *)addr;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static unsigned short OwnerdrawForObjectiveChannel(ObjectiveChannel ch) {
  switch (ch) {
  case OBJ_CHANNEL_PAUSE_HEADER:
    return 99;
  case OBJ_CHANNEL_PAUSE_LIST:
  case OBJ_CHANNEL_GAMEPLAY_LIST:
  case OBJ_CHANNEL_GAMEPLAY_UPDATE:
    return 100;
  case OBJ_CHANNEL_GAME_SAVED:
  case OBJ_CHANNEL_SAVE_PROGRESS:
    return 111;
  default:
    return 0;
  }
}

static bool IsObjectiveUpdateTokenForProbe(const std::string &key) {
  return (key.find("OBJECTIVESUPDATED") != std::string::npos ||
          key.find("OBJECTIVEUPDATED") != std::string::npos ||
          key.find("OBJECTIVECOMPLETED") != std::string::npos ||
          key.find("OBJECTIVEFAILED") != std::string::npos ||
          key.find("OBJECTIVES_UPDATED") != std::string::npos ||
          key.find("OBJECTIVE_UPDATED") != std::string::npos ||
          key.find("OBJECTIVE_COMPLETED") != std::string::npos ||
          key.find("OBJECTIVE_FAILED") != std::string::npos);
}

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

static bool IsObjectiveStatusMessageKey(const std::string &key) {
  if (key == "EXE_GAMESAVED" || key == "CGAME_NOW_SAVING") {
    return true;
  }
  return IsObjectiveUpdateTokenForProbe(key);
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

static float StatusQuickPulseAlphaScale(const std::string &key,
                                        unsigned int sequence,
                                        DWORD nowTick) {
  const DWORD bucket = nowTick / 50u;  // GSC quick_pulse updates every 0.05s
  uint32_t state = 2166136261u;
  for (unsigned char ch : key) {
    state ^= (uint32_t)ch;
    state *= 16777619u;
  }
  state ^= sequence + 0x9E3779B9u;
  state *= 16777619u;
  state ^= bucket + 0x85EBCA6Bu;
  state ^= state >> 16;
  state *= 0x7FEB352Du;
  state ^= state >> 15;
  state *= 0x846CA68Bu;
  state ^= state >> 16;
  const float unit = (float)(state & 0xFFFFu) / 65535.0f;
  return 0.7f + (0.3f * unit);
}

static ObjectiveChannel ProbeObjectiveChannelFromKey(const std::string &key) {
  if (key == "EXE_GAMESAVED") {
    return OBJ_CHANNEL_GAME_SAVED;
  }
  if (key == "CGAME_NOW_SAVING") {
    return OBJ_CHANNEL_SAVE_PROGRESS;
  }
  if (key == "CGAME_MISSIONOBJECTIVES") {
    return OBJ_CHANNEL_PAUSE_HEADER;
  }
  if (IsObjectiveUpdateTokenForProbe(key)) {
    return OBJ_CHANNEL_GAMEPLAY_UPDATE;
  }
  if (key.rfind("CORNERED_OBJ_", 0) == 0 ||
      key.rfind("CORNERED_OBJECTIVE_", 0) == 0 ||
      key.rfind("GAME_OBJECTIVE", 0) == 0 ||
      key.rfind("CGAME_OBJECTIVE", 0) == 0 ||
      key.find("MISSIONOBJECTIVE") != std::string::npos ||
      key.find("MISSION_OBJECTIVE") != std::string::npos ||
      key.find("_OBJ_") != std::string::npos ||
      key.find("_OBJECTIVE_") != std::string::npos ||
      HasObjectiveSuffixOnlyKey(key)) {
    return OBJ_CHANNEL_GAMEPLAY_LIST;
  }
  return OBJ_CHANNEL_NONE;
}

static unsigned short OwnerdrawForObjectiveKey(const std::string &key,
                                               ObjectiveChannel hintedChannel) {
  ObjectiveChannel ch = hintedChannel;
  if (ch == OBJ_CHANNEL_NONE) {
    ch = ProbeObjectiveChannelFromKey(key);
  }
  return OwnerdrawForObjectiveChannel(ch);
}

struct ObjectiveProbeSeed {
  std::string key;
  ObjectiveChannel channel = OBJ_CHANNEL_NONE;
  unsigned short ownerdrawId = 0;
  bool hasNativePos = false;
  float nativeX = 0.0f;
  float nativeY = 0.0f;
  unsigned short nativeVirtualW = 0;
  unsigned short nativeVirtualH = 0;
  float nativeAlpha = 1.0f;
  float scale = 1.0f;
  unsigned int callerOffset = 0;
  DWORD tick = 0;
};

static void UpsertObjectiveProbeSeed(
    std::unordered_map<std::string, ObjectiveProbeSeed> &seeds,
    const ObjectiveProbeSeed &seed) {
  if (seed.key.empty()) {
    return;
  }
  auto it = seeds.find(seed.key);
  if (it == seeds.end()) {
    seeds.emplace(seed.key, seed);
    return;
  }
  ObjectiveProbeSeed &dst = it->second;
  if (seed.tick >= dst.tick) {
    // Keep stronger native-pos evidence even if a later observed-only seed arrives.
    if (!seed.hasNativePos && dst.hasNativePos) {
      ObjectiveProbeSeed merged = seed;
      merged.hasNativePos = true;
      merged.nativeX = dst.nativeX;
      merged.nativeY = dst.nativeY;
      merged.nativeVirtualW = dst.nativeVirtualW;
      merged.nativeVirtualH = dst.nativeVirtualH;
      merged.nativeAlpha = dst.nativeAlpha;
      if (merged.callerOffset == 0) {
        merged.callerOffset = dst.callerOffset;
      }
      dst = merged;
    } else {
      dst = seed;
    }
    return;
  }
  if (dst.callerOffset == 0 && seed.callerOffset != 0) {
    dst.callerOffset = seed.callerOffset;
  }
  if (dst.ownerdrawId == 0 && seed.ownerdrawId != 0) {
    dst.ownerdrawId = seed.ownerdrawId;
  }
  if (!dst.hasNativePos && seed.hasNativePos) {
    dst.hasNativePos = true;
    dst.nativeX = seed.nativeX;
    dst.nativeY = seed.nativeY;
    dst.nativeVirtualW = seed.nativeVirtualW;
    dst.nativeVirtualH = seed.nativeVirtualH;
    dst.nativeAlpha = seed.nativeAlpha;
  }
}

// File-scope GMW lane state ??shared between probe and render code.
// Updated in ProbeObjectiveConsumerCandidates, read in render loop.
static DWORD g_GmwLaneActiveTick = 0;
static int g_GmwLastActiveWin = 0;

// Synthesized window alpha ??mirrors native Con_DrawMessageWindow behavior.
// Native engine computes fadeIn/fadeOut at RENDER TIME (NOT stored in GMW memory).
// GMW alpha field is BINARY (1.0 or 0.0, no intermediate values).
// We replicate: fadeIn=750ms (con_gameMsgWindow2FadeIn=0.75),
//               fadeOut=500ms (con_gameMsgWindow2FadeOut=0.5).
static DWORD g_GmwActivationTick = 0;   // when hasLane went 0??
static DWORD g_GmwDeactivationTick = 0; // when hasLane went 1??
static float g_GmwSynthAlpha = 0.0f;    // computed alpha [0..1]
static bool  g_GmwPrevHasLane = false;
static DWORD g_GmwFadeoutDoneTick = 0;  // when synth alpha reached 0 after deactivation

// D3D11Hook_SignalMissionRestart — moved to ObjectiveRendererUnified.inl
// void D3D11Hook_SignalMissionRestart() { }

static constexpr float kGmwFadeInMs  = 750.0f;
static constexpr float kGmwFadeOutMs = 500.0f;

static void ProbeObjectiveConsumerCandidates(
    const std::vector<ObjectiveOverlayEntry> &objectiveEntries,
    const std::vector<HudNativeEntry> &hudNativeEntries,
    const std::vector<ObjectiveKeyObservation> &objectiveObservedKeys, DWORD now,
    float safeOffX, float safeOffY, float scaleF) {
  static uintptr_t s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  if (!s_moduleBase) {
    return;
  }

  std::unordered_map<std::string, ObjectiveProbeSeed> seeds;
  seeds.reserve(objectiveEntries.size() + hudNativeEntries.size());
  auto IsLaneDrivenChannel = [](ObjectiveChannel ch) -> bool {
    return (ch == OBJ_CHANNEL_GAMEPLAY_UPDATE ||
            ch == OBJ_CHANNEL_GAMEPLAY_LIST ||
            ch == OBJ_CHANNEL_GAME_SAVED ||
            ch == OBJ_CHANNEL_SAVE_PROGRESS);
  };

  for (const auto &e : objectiveEntries) {
    if (e.key.empty()) {
      continue;
    }
    if (e.consumerOwned) {
      // Avoid feedback loop: consumer-owned entries should not become probe seeds.
      continue;
    }
    // Game Saved/Save Progress entries stay visible longer in native (~3-5s),
    // so extend the probe seed age window for them.
    DWORD seedMaxAgeMs = 1200;
    if (e.channel == OBJ_CHANNEL_GAME_SAVED ||
        e.channel == OBJ_CHANNEL_SAVE_PROGRESS) {
      seedMaxAgeMs = 5000;
    }
    if ((now - e.lastSeen) > seedMaxAgeMs) {
      continue;
    }
    ObjectiveProbeSeed seed{};
    seed.key = e.key;
    seed.channel =
        (e.channel != OBJ_CHANNEL_NONE) ? e.channel : ProbeObjectiveChannelFromKey(e.key);
    seed.ownerdrawId = OwnerdrawForObjectiveKey(seed.key, seed.channel);
    seed.scale =
        (std::isfinite(e.scale) && e.scale > 0.02f && e.scale < 8.0f) ? e.scale : 1.0f;
    seed.callerOffset = e.callerOffset;
    const bool laneDriven = IsLaneDrivenChannel(seed.channel);
    if (e.nativeValid && !laneDriven) {
      seed.hasNativePos = true;
      seed.nativeX = e.nativeX;
      seed.nativeY = e.nativeY;
      seed.nativeVirtualW = e.nativeVirtualW;
      seed.nativeVirtualH = e.nativeVirtualH;
      seed.nativeAlpha = Clamp01(e.color[3]);
    }
    seed.tick = e.lastSeen;
    UpsertObjectiveProbeSeed(seeds, seed);
  }

  for (const auto &n : hudNativeEntries) {
    if (!n.objectiveLike || n.key.empty()) {
      continue;
    }
    if (!n.valid) {
      continue;
    }
    if ((now - n.lastUpdate) > 1200) {
      continue;
    }
    ObjectiveChannel ch = OBJ_CHANNEL_NONE;
    if (n.objectiveChannel >= OBJ_CHANNEL_GAMEPLAY_LIST &&
        n.objectiveChannel <= OBJ_CHANNEL_SAVE_PROGRESS) {
      ch = (ObjectiveChannel)n.objectiveChannel;
    } else {
      ch = ProbeObjectiveChannelFromKey(n.key);
    }
    if (ch == OBJ_CHANNEL_NONE) {
      continue;
    }
    if (IsLaneDrivenChannel(ch)) {
      // Gameplay objective/save channels are lane-driven from gameMsgWindow.
      // Raw hudNative positions in these channels are noisy (camera-coupled in
      // some caller paths), so don't seed them as consumer-truth positions.
      static std::unordered_map<std::string, DWORD> s_laneNativeSkipLog;
      std::string lk =
          n.key + "|" + std::to_string((unsigned int)n.callerOffset);
      auto itL = s_laneNativeSkipLog.find(lk);
      if (itL == s_laneNativeSkipLog.end() || (now - itL->second) > 900) {
        s_laneNativeSkipLog[lk] = now;
        char nbuf[360];
        sprintf_s(nbuf,
                  "[OBJ-CONS-CAND-NATIVE-LANE] key=%s ch=%s x=%.2f y=%.2f "
                  "a=%.2f vw=%u vh=%u caller=+0x%X",
                  n.key.c_str(), ObjectiveChannelName(ch), n.x, n.y, n.color[3],
                  n.virtualW, n.virtualH, n.callerOffset);
        LogToFile(nbuf);
      }
      continue;
    }
    float scale = 1.0f;
    const bool xOk = std::isfinite(n.xScale) && n.xScale > 0.02f && n.xScale < 8.0f;
    const bool yOk = std::isfinite(n.yScale) && n.yScale > 0.02f && n.yScale < 8.0f;
    if (xOk && yOk) {
      scale = (n.xScale + n.yScale) * 0.5f;
    } else if (yOk) {
      scale = n.yScale;
    } else if (xOk) {
      scale = n.xScale;
    }

    ObjectiveProbeSeed seed{};
    seed.key = n.key;
    seed.channel = ch;
    seed.ownerdrawId = OwnerdrawForObjectiveKey(seed.key, ch);
    seed.hasNativePos = true;
    seed.nativeX = n.x;
    seed.nativeY = n.y;
    if (!std::isfinite(seed.nativeX) || !std::isfinite(seed.nativeY) ||
        (fabsf(seed.nativeX) < 0.001f && fabsf(seed.nativeY) < 0.001f)) {
      continue;
    }
    seed.nativeVirtualW = n.virtualW;
    seed.nativeVirtualH = n.virtualH;
    seed.nativeAlpha = Clamp01(n.color[3]);
    seed.scale = scale;
    seed.callerOffset = n.callerOffset;
    seed.tick = n.lastUpdate;
    UpsertObjectiveProbeSeed(seeds, seed);
  }

  for (const auto &obs : objectiveObservedKeys) {
    if (obs.key.empty()) {
      continue;
    }
    // CGAME_MISSIONOBJECTIVES is heavily emitted outside true pause rendering.
    // Treat it as a pause-only signal; suppress it while gameplay objective
    // traffic is active.
    if (obs.key == "CGAME_MISSIONOBJECTIVES") {
      bool gameplayTrafficRecent = false;
      for (const auto &obs2 : objectiveObservedKeys) {
        if (obs2.key.empty() || obs2.key == "CGAME_MISSIONOBJECTIVES") {
          continue;
        }
        ObjectiveChannel ch2 =
            (obs2.channel != OBJ_CHANNEL_NONE)
                ? obs2.channel
                : ProbeObjectiveChannelFromKey(obs2.key);
        if (ch2 == OBJ_CHANNEL_GAMEPLAY_UPDATE ||
            ch2 == OBJ_CHANNEL_GAMEPLAY_LIST) {
          if ((now - obs2.lastSeen) <= 700) {
            gameplayTrafficRecent = true;
            break;
          }
        }
      }
      if (gameplayTrafficRecent || !TextHook_IsPauseMenuLikely()) {
        continue;
      }
    }
    DWORD obsMaxAge = 1500;
    if (obs.key == "EXE_GAMESAVED" || obs.key == "CGAME_NOW_SAVING") {
      obsMaxAge = 450;
    }
    if ((now - obs.lastSeen) > obsMaxAge) {
      continue;
    }
    ObjectiveChannel ch =
        (obs.channel != OBJ_CHANNEL_NONE) ? obs.channel : ProbeObjectiveChannelFromKey(obs.key);
    if (ch == OBJ_CHANNEL_NONE) {
      continue;
    }
    ObjectiveProbeSeed seed{};
    seed.key = obs.key;
    seed.channel = ch;
    seed.ownerdrawId = OwnerdrawForObjectiveKey(obs.key, ch);
    seed.scale = 1.0f;
    seed.callerOffset = 0;
    seed.tick = obs.lastSeen;
    UpsertObjectiveProbeSeed(seeds, seed);
  }

  if (seeds.empty()) {
    return;
  }

  static const uintptr_t kCandidates[] = {0x2006A0, 0x200780, 0x200B30,
                                          0x234180, 0x234FA0, 0x23E452};
  static std::unordered_set<uintptr_t> s_loggedCandidate;
  for (uintptr_t off : kCandidates) {
    if (s_loggedCandidate.find(off) != s_loggedCandidate.end()) {
      continue;
    }
    uint32_t b0 = 0;
    if (!ReadU32Safe(s_moduleBase + off, b0)) {
      continue;
    }
    s_loggedCandidate.insert(off);
    char cbuf[160];
    sprintf_s(cbuf, "[OBJ-CONS-CAND] fn=+0x%llX dword0=0x%08X",
              (unsigned long long)off, b0);
    LogToFile(cbuf);
  }

  // Runtime consumer memory probe (gameMsgWindow family).
  // When stable lane hints are visible, publish as consumer samples.
  constexpr uintptr_t kGmwBaseOff = 0x17CF880;
  constexpr uintptr_t kGmwStride = 0xC6C8;
  constexpr int kGmwWindowCount = 5;
  struct GmwWindowState {
    uintptr_t addr = 0;
    float x = 0.0f;
    float y = 0.0f;
    float a = 1.0f;
    uint32_t d28 = 0;
    uint32_t d2c = 0;
    bool active = false;
  };
  GmwWindowState gmwWin[kGmwWindowCount];
  int selectedWin = 0;
  int bestWin = -1;
  float bestScore = 0.0f;
  for (int i = 0; i < kGmwWindowCount; ++i) {
    uintptr_t addr = s_moduleBase + kGmwBaseOff + (uintptr_t)i * kGmwStride;
    gmwWin[i].addr = addr;
    ReadF32Safe(addr + 0x5C, gmwWin[i].x);
    ReadF32Safe(addr + 0x34, gmwWin[i].y);
    ReadU32Safe(addr + 0x28, gmwWin[i].d28);
    ReadU32Safe(addr + 0x2C, gmwWin[i].d2c);
    float alphaCand = 1.0f;
    if (ReadF32Safe(addr + 0x60, alphaCand) && alphaCand >= 0.0f &&
        alphaCand <= 1.2f) {
      gmwWin[i].a = Clamp01(alphaCand);
    }
    gmwWin[i].active = (gmwWin[i].x > 2.0f && gmwWin[i].x < 160.0f &&
                        gmwWin[i].y >= 0.0f && gmwWin[i].y < 64.0f &&
                        gmwWin[i].d2c > 0 && gmwWin[i].d2c <= 64);
    if (gmwWin[i].active) {
      float score =
          fabsf(gmwWin[i].x - 28.6f) + fabsf(gmwWin[i].y - 20.6f);
      if (bestWin < 0 || score < bestScore) {
        bestWin = i;
        bestScore = score;
      }
    }
  }
  if (bestWin >= 0) {
    selectedWin = bestWin;
    g_GmwLastActiveWin = bestWin;  // remember which window was active
  }
  uintptr_t gmw0 = gmwWin[selectedWin].addr;
  uintptr_t gmwSelectedOff = kGmwBaseOff + (uintptr_t)selectedWin * kGmwStride;
  constexpr uintptr_t kLineTableOff = 0x17F24E0;
  uintptr_t lineBase = s_moduleBase + kLineTableOff;
  float laneX = 0.0f;
  float laneY = 0.0f;
  float laneA = 1.0f;
  bool hasLane = gmwWin[selectedWin].active;
  laneX = gmwWin[selectedWin].x;
  laneY = gmwWin[selectedWin].y;
  laneA = gmwWin[selectedWin].a;

  // Synthesized window alpha ??replicates native Con_DrawMessageWindow fade.
  // GMW alpha in memory is BINARY (1.0??.0 in one frame). Native renderer
  // computes fadeIn/fadeOut itself. We must do the same.
  {
    if (hasLane && !g_GmwPrevHasLane) {
      // Debounce: if lane was only deactivated briefly (<100ms), treat as
      // continuous activation ??do NOT reset g_GmwActivationTick. This
      // prevents synthAlpha from dropping to 0 on brief GMW lane bounces.
      DWORD deactivationDuration = (g_GmwDeactivationTick != 0)
                                       ? (now - g_GmwDeactivationTick)
                                       : 0xFFFFFFFF;
      if (deactivationDuration > 100) {
        g_GmwActivationTick = now;
      }
      // Log GMW reactivation for diagnostic purposes.
      if (kVerboseRuntimeLogs) {
        if (g_GmwDeactivationTick != 0) {
          char rbuf[256];
          sprintf_s(rbuf,
              "[OBJ-GMW-REACTIVATE] hasLane 0->1, deactivated=%lums%s (log only)",
              (unsigned long)deactivationDuration,
              deactivationDuration <= 100 ? " (DEBOUNCED)" : "");
          LogToFile(rbuf);
        } else {
          LogToFile("[OBJ-GMW-REACTIVATE] hasLane 0->1, first activation (log only)");
        }
      }
    }
    if (!hasLane && g_GmwPrevHasLane) {
      g_GmwDeactivationTick = now;
    }
    g_GmwPrevHasLane = hasLane;

    float prevSynth = g_GmwSynthAlpha;
    if (hasLane) {
      // FadeIn: ramp up over 750ms from activation
      float elapsed = (float)(now - g_GmwActivationTick);
      float target = (elapsed >= kGmwFadeInMs) ? 1.0f : elapsed / kGmwFadeInMs;
      g_GmwSynthAlpha = target;
    } else if (g_GmwDeactivationTick != 0) {
      // FadeOut: ramp down over 500ms from deactivation.
      // But debounce: if deactivated < 100ms, keep alpha at previous level.
      float deactElapsed = (float)(now - g_GmwDeactivationTick);
      if (deactElapsed < 100.0f) {
        // Brief bounce ??hold previous alpha, don't fade out yet
      } else {
        float elapsed = deactElapsed;
        g_GmwSynthAlpha = (elapsed >= kGmwFadeOutMs)
                               ? 0.0f
                               : 1.0f - elapsed / kGmwFadeOutMs;
      }
    }
    if (g_GmwSynthAlpha < 0.0f) g_GmwSynthAlpha = 0.0f;
    if (g_GmwSynthAlpha > 1.0f) g_GmwSynthAlpha = 1.0f;
    // Track when fadeout completes ??entries refreshed before this are ghosts.
    // NOTE: Do NOT set g_bMissionRestarted here. When hasLane transitions 0??,
    // synthAlpha resets to 0 (elapsed=0 in fadeIn), which falsely triggers this
    // check. Restart detection is handled solely by hasLane 0?? with duration guard.
    if (prevSynth > 0.005f && g_GmwSynthAlpha <= 0.005f) {
      g_GmwFadeoutDoneTick = now;
    }
  }
  float laneSpacing = 12.0f;
  for (int i = 0; i < 4; ++i) {
    float f10 = 0.0f;
    if (!ReadF32Safe(lineBase + (uintptr_t)i * 0x100 + 0x10, f10)) {
      continue;
    }
    if (std::isfinite(f10) && f10 > 8.0f && f10 < 40.0f) {
      laneSpacing = (std::max)(laneSpacing, f10);
    }
  }

  static DWORD s_lastGmwLog = 0;
  // NOTE: lane tick stored in file-scope g_GmwLaneActiveTick (shared with render)
  DWORD &s_lastLaneActiveTick = g_GmwLaneActiveTick;
  static float s_lastLaneX = 0.0f;
  static float s_lastLaneY = 0.0f;
  static float s_lastLaneA = 1.0f;
  static float s_lastLaneSpacing = 12.0f;
  if (hasLane) {
    s_lastLaneActiveTick = now;
    s_lastLaneX = laneX;
    s_lastLaneY = laneY;
    s_lastLaneA = laneA;
    s_lastLaneSpacing = laneSpacing;
  }

  // Log GMW lane transitions with synth alpha (diagnostic only)
  if (kVerboseRuntimeLogs) {
    static bool s_wasHasLane2 = false;
    if (hasLane != s_wasHasLane2) {
      char tbuf[256];
      sprintf_s(tbuf,
                "[GMW-LANE] %s synthA=%.3f d2c=%u w=%d",
                hasLane ? "ACTIVATED" : "DEACTIVATED",
                g_GmwSynthAlpha, gmwWin[selectedWin].d2c, selectedWin);
      LogToFile(tbuf);
      s_wasHasLane2 = hasLane;
    }
    // Track d2c changes on selected window (detect objective lifecycle)
    static uint32_t s_prevD2c = 0;
    static float s_prevAlpha = 1.0f;
    uint32_t curD2c = gmwWin[selectedWin].d2c;
    float curAlpha = gmwWin[selectedWin].a;
    if (curD2c != s_prevD2c || fabsf(curAlpha - s_prevAlpha) > 0.01f) {
      char dbuf[256];
      sprintf_s(dbuf,
                "[GMW-D2C-CHANGE] w=%d d2c=%u->%u a=%.3f->%.3f hasLane=%d synthA=%.3f",
                selectedWin, s_prevD2c, curD2c, s_prevAlpha, curAlpha,
                hasLane ? 1 : 0, g_GmwSynthAlpha);
      LogToFile(dbuf);
      s_prevD2c = curD2c;
      s_prevAlpha = curAlpha;
    }
  }

  if (kVerboseRuntimeLogs && (now - s_lastGmwLog) > 800) {
    s_lastGmwLog = now;
    for (int i = 0; i < kGmwWindowCount; ++i) {
      uintptr_t off = kGmwBaseOff + (uintptr_t)i * kGmwStride;
      char wbuf[300];
      sprintf_s(wbuf,
                "[OBJ-GMW-WIN] idx=%d off=+0x%llX x=%.2f y=%.2f d28=%u d2c=%u "
                "a=%.2f active=%d sel=%d sp=%.2f",
                i, (unsigned long long)off, gmwWin[i].x, gmwWin[i].y,
                gmwWin[i].d28, gmwWin[i].d2c, gmwWin[i].a,
                gmwWin[i].active ? 1 : 0, (i == selectedWin) ? 1 : 0,
                laneSpacing);
      LogToFile(wbuf);
    }
    char gbuf[288];
    sprintf_s(gbuf,
              "[OBJ-CONS-CHAIN] src=gmw w=%d off=+0x%llX x=%.2f y=%.2f d28=%u "
              "d2c=%u active=%d sp=%.2f",
              selectedWin, (unsigned long long)gmwSelectedOff, laneX, laneY,
              gmwWin[selectedWin].d28, gmwWin[selectedWin].d2c,
              hasLane ? 1 : 0, laneSpacing);
    LogToFile(gbuf);
  }

  auto TryReadAsciiPreview = [](uintptr_t ptr, std::string &out) -> bool {
    out.clear();
    if (ptr <= 0x10000 || ptr >= 0x7FFFFFFFFFFF) {
      return false;
    }
    __try {
      int printable = 0;
      for (int i = 0; i < 96; ++i) {
        char c = *(volatile char *)(ptr + i);
        if (c == '\0') {
          break;
        }
        unsigned char uc = (unsigned char)c;
        if (uc >= 0x20 && uc <= 0x7E) {
          out.push_back(c);
          printable++;
          continue;
        }
        if (c == '\n' || c == '\r' || c == '\t') {
          out.push_back(' ');
          continue;
        }
        // Non-ASCII blobs are common in these pools; reject to avoid noise.
        out.clear();
        return false;
      }
      if (printable < 4) {
        out.clear();
        return false;
      }
      return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      out.clear();
      return false;
    }
  };
  auto TryReadUtf16Preview = [](uintptr_t ptr, std::string &out) -> bool {
    out.clear();
    if (ptr <= 0x10000 || ptr >= 0x7FFFFFFFFFFF) {
      return false;
    }
    __try {
      int printable = 0;
      for (int i = 0; i < 96; ++i) {
        wchar_t wc = *(volatile wchar_t *)(ptr + i * sizeof(wchar_t));
        if (wc == L'\0') {
          break;
        }
        if (wc >= 0x20 && wc <= 0x7E) {
          out.push_back((char)wc);
          printable++;
          continue;
        }
        if (wc == L'\n' || wc == L'\r' || wc == L'\t') {
          out.push_back(' ');
          continue;
        }
        // Allow Hangul blocks but map to placeholder to prove UTF-16 text lane.
        if (wc >= 0xAC00 && wc <= 0xD7A3) {
          out.push_back('?');
          printable++;
          continue;
        }
        out.clear();
        return false;
      }
      if (printable < 4) {
        out.clear();
        return false;
      }
      return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      out.clear();
      return false;
    }
  };

  // Memory reversing probe: dump gameMsgWindow internals and line tables
  // while objective lane is active to identify true consumer fields.
  // Gated behind kVerboseRuntimeLogs — pure diagnostic, not needed for rendering.
  static DWORD s_lastGmwDetailLog = 0;
  if (kVerboseRuntimeLogs && (now - s_lastGmwDetailLog) > 1400) {
    s_lastGmwDetailLog = now;

    uint32_t u28 = 0, u2c = 0, u30 = 0, u34 = 0, u38 = 0, u3c = 0, u5c = 0,
             u60 = 0, u70 = 0, u74 = 0;
    ReadU32Safe(gmw0 + 0x28, u28);
    ReadU32Safe(gmw0 + 0x2C, u2c);
    ReadU32Safe(gmw0 + 0x30, u30);
    ReadU32Safe(gmw0 + 0x34, u34);
    ReadU32Safe(gmw0 + 0x38, u38);
    ReadU32Safe(gmw0 + 0x3C, u3c);
    ReadU32Safe(gmw0 + 0x5C, u5c);
    ReadU32Safe(gmw0 + 0x60, u60);
    ReadU32Safe(gmw0 + 0x70, u70);
    ReadU32Safe(gmw0 + 0x74, u74);

    float f34 = 0.0f, f38 = 0.0f, f3c = 0.0f, f40 = 0.0f, f44 = 0.0f,
          f48 = 0.0f, f4c = 0.0f, f50 = 0.0f, f54 = 0.0f, f58 = 0.0f,
          f5c = 0.0f, f60 = 0.0f, f64 = 0.0f, f68 = 0.0f, f6c = 0.0f,
          f70 = 0.0f, f74 = 0.0f;
    ReadF32Safe(gmw0 + 0x34, f34);
    ReadF32Safe(gmw0 + 0x38, f38);
    ReadF32Safe(gmw0 + 0x3C, f3c);
    ReadF32Safe(gmw0 + 0x40, f40);
    ReadF32Safe(gmw0 + 0x44, f44);
    ReadF32Safe(gmw0 + 0x48, f48);
    ReadF32Safe(gmw0 + 0x4C, f4c);
    ReadF32Safe(gmw0 + 0x50, f50);
    ReadF32Safe(gmw0 + 0x54, f54);
    ReadF32Safe(gmw0 + 0x58, f58);
    ReadF32Safe(gmw0 + 0x5C, f5c);
    ReadF32Safe(gmw0 + 0x60, f60);
    ReadF32Safe(gmw0 + 0x64, f64);
    ReadF32Safe(gmw0 + 0x68, f68);
    ReadF32Safe(gmw0 + 0x6C, f6c);
    ReadF32Safe(gmw0 + 0x70, f70);
    ReadF32Safe(gmw0 + 0x74, f74);

    uint64_t p00 = 0, p08 = 0, p10 = 0, p18 = 0;
    ReadU64Safe(gmw0 + 0x00, p00);
    ReadU64Safe(gmw0 + 0x08, p08);
    ReadU64Safe(gmw0 + 0x10, p10);
    ReadU64Safe(gmw0 + 0x18, p18);

    char m0[512];
    sprintf_s(
        m0,
        "[OBJ-GMW-SCAN] w=%d off=+0x%llX p00=0x%llX p08=0x%llX p10=0x%llX p18=0x%llX "
        "u28=%u u2c=%u u30=%u u34=%u u38=%u u3c=%u u5c=%u u60=%u u70=%u u74=%u "
        "f34=%.2f f38=%.2f f3c=%.2f f5c=%.2f f60=%.2f f70=%.2f f74=%.2f",
        selectedWin, (unsigned long long)gmwSelectedOff, (unsigned long long)p00,
        (unsigned long long)p08, (unsigned long long)p10, (unsigned long long)p18,
        u28, u2c, u30, u34, u38, u3c, u5c, u60, u70, u74, f34, f38, f3c, f5c, f60,
        f70, f74);
    LogToFile(m0);

    std::string txtPreview;
    if (TryReadAsciiPreview((uintptr_t)p00, txtPreview) ||
        TryReadUtf16Preview((uintptr_t)p00, txtPreview)) {
      LogToFile(std::string("[OBJ-GMW-TXT] p00=\"") + txtPreview + "\"");
    }
    if (TryReadAsciiPreview((uintptr_t)p08, txtPreview) ||
        TryReadUtf16Preview((uintptr_t)p08, txtPreview)) {
      LogToFile(std::string("[OBJ-GMW-TXT] p08=\"") + txtPreview + "\"");
    }
    if (TryReadAsciiPreview((uintptr_t)p10, txtPreview) ||
        TryReadUtf16Preview((uintptr_t)p10, txtPreview)) {
      LogToFile(std::string("[OBJ-GMW-TXT] p10=\"") + txtPreview + "\"");
    }
    if (TryReadAsciiPreview((uintptr_t)p18, txtPreview) ||
        TryReadUtf16Preview((uintptr_t)p18, txtPreview)) {
      LogToFile(std::string("[OBJ-GMW-TXT] p18=\"") + txtPreview + "\"");
    }

    for (int i = 0; i < 4; ++i) {
      uintptr_t slot = lineBase + (uintptr_t)i * 0x100;
      uint32_t d0 = 0, d4 = 0, d8 = 0, dc = 0;
      uint32_t d14 = 0, d18 = 0, d1c = 0, d20 = 0, d24 = 0, d28 = 0, d2c = 0,
               d30 = 0, d34 = 0;
      float fx4 = 0.0f, fx8 = 0.0f, fxc = 0.0f, fx10 = 0.0f;
      float fx14 = 0.0f, fx18 = 0.0f, fx1c = 0.0f, fx20 = 0.0f, fx24 = 0.0f,
            fx28 = 0.0f, fx2c = 0.0f, fx30 = 0.0f, fx34 = 0.0f;
      ReadU32Safe(slot + 0x00, d0);
      ReadU32Safe(slot + 0x04, d4);
      ReadU32Safe(slot + 0x08, d8);
      ReadU32Safe(slot + 0x0C, dc);
      ReadU32Safe(slot + 0x14, d14);
      ReadU32Safe(slot + 0x18, d18);
      ReadU32Safe(slot + 0x1C, d1c);
      ReadU32Safe(slot + 0x20, d20);
      ReadU32Safe(slot + 0x24, d24);
      ReadU32Safe(slot + 0x28, d28);
      ReadU32Safe(slot + 0x2C, d2c);
      ReadU32Safe(slot + 0x30, d30);
      ReadU32Safe(slot + 0x34, d34);
      ReadF32Safe(slot + 0x04, fx4);
      ReadF32Safe(slot + 0x08, fx8);
      ReadF32Safe(slot + 0x0C, fxc);
      ReadF32Safe(slot + 0x10, fx10);
      ReadF32Safe(slot + 0x14, fx14);
      ReadF32Safe(slot + 0x18, fx18);
      ReadF32Safe(slot + 0x1C, fx1c);
      ReadF32Safe(slot + 0x20, fx20);
      ReadF32Safe(slot + 0x24, fx24);
      ReadF32Safe(slot + 0x28, fx28);
      ReadF32Safe(slot + 0x2C, fx2c);
      ReadF32Safe(slot + 0x30, fx30);
      ReadF32Safe(slot + 0x34, fx34);
      char lbuf[320];
      sprintf_s(lbuf,
                "[OBJ-GMW-LINE] off=+0x%llX i=%d d0=%u d4=%u d8=%u dc=%u "
                "f4=%.2f f8=%.2f fc=%.2f f10=%.2f",
                (unsigned long long)kLineTableOff, i, d0, d4, d8, dc, fx4, fx8,
                fxc, fx10);
      LogToFile(lbuf);
      char lbuf2[640];
      sprintf_s(
          lbuf2,
          "[OBJ-GMW-LINE-X] off=+0x%llX i=%d d14=%u d18=%u d1c=%u d20=%u d24=%u "
          "d28=%u d2c=%u d30=%u d34=%u f14=%.2f f18=%.2f f1c=%.2f f20=%.2f "
          "f24=%.2f f28=%.2f f2c=%.2f f30=%.2f f34=%.2f",
          (unsigned long long)kLineTableOff, i, d14, d18, d1c, d20, d24, d28,
          d2c, d30, d34, fx14, fx18, fx1c, fx20, fx24, fx28, fx2c, fx30, fx34);
      LogToFile(lbuf2);

      int linePtrHits = 0;
      for (uintptr_t poff = 0; poff <= 0xB0; poff += 8) {
        uint64_t pq = 0;
        if (!ReadU64Safe(slot + poff, pq)) {
          continue;
        }
        std::string lineTxt;
        if (!TryReadAsciiPreview((uintptr_t)pq, lineTxt) &&
            !TryReadUtf16Preview((uintptr_t)pq, lineTxt)) {
          continue;
        }
        char p2[384];
        sprintf_s(p2,
                  "[OBJ-GMW-LINE-PTR] off=+0x%llX i=%d ptrOff=+0x%llX ptr=0x%llX txt=%s",
                  (unsigned long long)kLineTableOff, i, (unsigned long long)poff,
                  (unsigned long long)pq, lineTxt.substr(0, 96).c_str());
        LogToFile(p2);
        linePtrHits++;
        if (linePtrHits >= 3) {
          break;
        }
      }
    }

    int ptrHitCount = 0;
    for (uintptr_t off = 0; off <= 0x120; off += 8) {
      uint64_t q = 0;
      if (!ReadU64Safe(gmw0 + off, q)) {
        continue;
      }
      std::string ptrTxt;
      if (!TryReadAsciiPreview((uintptr_t)q, ptrTxt) &&
          !TryReadUtf16Preview((uintptr_t)q, ptrTxt)) {
        continue;
      }
      char pbuf[360];
      sprintf_s(pbuf, "[OBJ-GMW-PTRSCAN] w=%d off=+0x%llX ptrOff=+0x%llX ptr=0x%llX txt=%s",
                selectedWin, (unsigned long long)gmwSelectedOff,
                (unsigned long long)off, (unsigned long long)q,
                Utf8Preview(ptrTxt, 80).c_str());
      LogToFile(pbuf);
      ptrHitCount++;
      if (ptrHitCount >= 4) {
        break;
      }
    }

    // Per-line timestamp probing removed ??GMW alpha is binary,
    // native fade is computed at render time. We use synthesized alpha instead.
  }
  // GMW offset search scan — pure diagnostic (16K SEH reads per invocation).
  // Gated behind kVerboseRuntimeLogs.
  static DWORD s_lastGmwSearchLog = 0;
  if (kVerboseRuntimeLogs && !hasLane && (now - s_lastGmwSearchLog) > 600) {
    s_lastGmwSearchLog = now;
    struct GmwCand {
      intptr_t delta;
      uintptr_t off;
      float x;
      float y;
      uint32_t d28;
      uint32_t d2c;
      float score;
    };
    std::vector<GmwCand> cands;
    cands.reserve(128);
    const bool hasLaneTarget =
        (s_lastLaneActiveTick != 0) && ((now - s_lastLaneActiveTick) <= 12000);
    const float targetX = hasLaneTarget ? s_lastLaneX : 32.0f;
    const float targetY = hasLaneTarget ? s_lastLaneY : 24.0f;
    for (intptr_t delta = -0x20000; delta <= 0x20000; delta += 0x10) {
      uintptr_t b = (uintptr_t)((intptr_t)gmw0 + delta);
      float cx = 0.0f, cy = 0.0f;
      uint32_t d28 = 0, d2c = 0;
      if (!ReadF32Safe(b + 0x5C, cx) || !ReadF32Safe(b + 0x34, cy) ||
          !ReadU32Safe(b + 0x28, d28) || !ReadU32Safe(b + 0x2C, d2c)) {
        continue;
      }
      if (!std::isfinite(cx) || !std::isfinite(cy)) {
        continue;
      }
      if (d2c == 0 || d2c > 64) {
        continue;
      }
      if (cx < -64.0f || cx > 240.0f || cy < -96.0f || cy > 140.0f) {
        continue;
      }
      GmwCand c{};
      c.delta = delta;
      c.off = kGmwBaseOff + (uintptr_t)((intptr_t)selectedWin * (intptr_t)kGmwStride + delta);
      c.x = cx;
      c.y = cy;
      c.d28 = d28;
      c.d2c = d2c;
      c.score = fabsf(cx - targetX) + fabsf(cy - targetY);
      if (d2c != 17) {
        c.score += 8.0f;
      }
      if (cx < 0.0f || cy < 0.0f) {
        c.score += 5.0f;
      }
      cands.push_back(c);
    }
    if (!cands.empty()) {
      std::sort(cands.begin(), cands.end(),
                [](const GmwCand &a, const GmwCand &b) {
                  return a.score < b.score;
                });
      for (size_t i = 0; i < cands.size() && i < 8; ++i) {
        const GmwCand &c = cands[i];
        char sbuf[320];
        sprintf_s(
            sbuf,
            "[OBJ-GMW-SEARCH] w=%d base=+0x%llX off=+0x%llX delta=%+lld x=%.2f "
            "y=%.2f d28=%u d2c=%u score=%.2f",
            selectedWin, (unsigned long long)gmwSelectedOff,
            (unsigned long long)c.off, (long long)c.delta, c.x, c.y, c.d28,
            c.d2c, c.score);
        LogToFile(sbuf);
      }
    } else {
      char sbuf[200];
      sprintf_s(sbuf,
                "[OBJ-GMW-SEARCH] w=%d off=+0x%llX no_candidates x=%.2f y=%.2f",
                selectedWin, (unsigned long long)gmwSelectedOff, laneX, laneY);
      LogToFile(sbuf);
    }
  }

  // Render state diagnostic dump — gated behind kVerboseRuntimeLogs.
  static DWORD s_lastRstateLog = 0;
  if (kVerboseRuntimeLogs && (now - s_lastRstateLog) > 1600) {
    s_lastRstateLog = now;
    constexpr uintptr_t kRStateAOff = 0x1640E38;
    constexpr uintptr_t kRStateBOff = 0x1640F0C;
    uintptr_t rsA = s_moduleBase + kRStateAOff;
    uintptr_t rsB = s_moduleBase + kRStateBOff;
    float a[8] = {0}, b[8] = {0};
    for (int i = 0; i < 8; ++i) {
      ReadF32Safe(rsA + (uintptr_t)i * 4, a[i]);
      ReadF32Safe(rsB + (uintptr_t)i * 4, b[i]);
    }
    char rbuf[512];
    sprintf_s(rbuf,
              "[OBJ-RSTATE] a=+0x%llX %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f "
              "b=+0x%llX %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f",
              (unsigned long long)kRStateAOff, a[0], a[1], a[2], a[3], a[4],
              a[5], a[6], a[7], (unsigned long long)kRStateBOff, b[0], b[1],
              b[2], b[3], b[4], b[5], b[6], b[7]);
    LogToFile(rbuf);
  }

  static std::unordered_map<std::string, DWORD> s_lastSampleLog;
  static std::unordered_map<std::string, DWORD> s_lastLaneMissLog;
  static std::unordered_map<std::string, DWORD> s_lastStaleLaneLog;
  for (const auto &kv : seeds) {
    const ObjectiveProbeSeed &seed = kv.second;
    const bool haveLaneStale =
        (!hasLane && s_lastLaneActiveTick != 0 &&
         (now - s_lastLaneActiveTick) <= 200);
    const bool laneRecent = hasLane || haveLaneStale;
    const bool laneUsingStale = haveLaneStale;
    // If we do not have a stale-valid snapshot, keep raw gmw probe values
    // (including negatives) for diagnostics instead of silently zeroing.
    const float laneUseX = hasLane ? laneX : (haveLaneStale ? s_lastLaneX : laneX);
    const float laneUseY = hasLane ? laneY : (haveLaneStale ? s_lastLaneY : laneY);
    const float laneUseA = hasLane ? laneA : (haveLaneStale ? s_lastLaneA : laneA);
    const float laneUseSpacing =
        hasLane ? laneSpacing : (haveLaneStale ? s_lastLaneSpacing : laneSpacing);
    const bool laneDrivenChannel =
        (seed.channel == OBJ_CHANNEL_GAMEPLAY_UPDATE ||
         seed.channel == OBJ_CHANNEL_GAMEPLAY_LIST ||
         seed.channel == OBJ_CHANNEL_GAME_SAVED ||
         seed.channel == OBJ_CHANNEL_SAVE_PROGRESS);
    ObjectiveConsumerSample sample{};
    sample.key = seed.key;
    sample.ownerdrawId = seed.ownerdrawId;
    sample.scale = seed.scale;
    const char *srcTag = "observed";
    bool isGmwSample = false;
    float sampleSpacing = 0.0f;
    if (seed.hasNativePos) {
      sample.x = seed.nativeX;
      sample.y = seed.nativeY;
      sample.virtualW = seed.nativeVirtualW;
      sample.virtualH = seed.nativeVirtualH;
      srcTag = "native";
    } else if (laneRecent &&
               (seed.channel == OBJ_CHANNEL_GAMEPLAY_UPDATE ||
                seed.channel == OBJ_CHANNEL_GAMEPLAY_LIST ||
                seed.channel == OBJ_CHANNEL_GAME_SAVED ||
                seed.channel == OBJ_CHANNEL_SAVE_PROGRESS)) {
      // GMW lane is active: use it ONLY for timing (alpha/active detection).
      // Do NOT use gmw probe values for position ??they are internal window
      // state, not HudElem virtual coordinates. Leave x/y/callerOffset at 0
      // so posTrusted=false and the fallback path handles positioning with
      // known GSC coordinates + ScreenPlacement.
      sample.x = 0.0f;
      sample.y = 0.0f;
      sample.virtualW = 0;
      sample.virtualH = 0;
      sample.callerOffset = 0;
      if (seed.channel == OBJ_CHANNEL_GAMEPLAY_UPDATE ||
          seed.channel == OBJ_CHANNEL_GAMEPLAY_LIST) {
        sample.scale = 1.30f;
      } else if (seed.channel == OBJ_CHANNEL_GAME_SAVED ||
                 seed.channel == OBJ_CHANNEL_SAVE_PROGRESS) {
        sample.scale = 1.30f;
      }
      srcTag = laneUsingStale ? "gmw_stale" : "gmw";
      isGmwSample = true;
      if (laneUsingStale) {
        auto itLs = s_lastStaleLaneLog.find(seed.key);
        if (itLs == s_lastStaleLaneLog.end() || (now - itLs->second) > 1200) {
          s_lastStaleLaneLog[seed.key] = now;
          char sbuf[320];
          sprintf_s(
              sbuf,
              "[OBJ-CONS-GMW-STALE] key=%s ch=%s staleMs=%lu x=%.2f y=%.2f "
              "sp=%.2f",
              seed.key.c_str(), ObjectiveChannelName(seed.channel),
              (unsigned long)(now - s_lastLaneActiveTick), laneUseX, laneUseY,
              laneUseSpacing);
          LogToFile(sbuf);
        }
      }
    } else {
      if (laneDrivenChannel && !laneRecent) {
        auto itLm = s_lastLaneMissLog.find(seed.key);
        if (itLm == s_lastLaneMissLog.end() || (now - itLm->second) > 1200) {
          s_lastLaneMissLog[seed.key] = now;
          char lmbuf[320];
          sprintf_s(
              lmbuf,
              "[OBJ-CONS-LANE-MISS] key=%s ch=%s laneRecent=0 obsTick=%lu "
              "gmwX=%.2f gmwY=%.2f",
              seed.key.c_str(), ObjectiveChannelName(seed.channel),
              (unsigned long)seed.tick, laneUseX, laneUseY);
          LogToFile(lmbuf);
        }
      }
      sample.x = 0.0f;
      sample.y = 0.0f;
      sample.virtualW = 0;
      sample.virtualH = 0;
      // Skip positionless samples for lane-driven channels: they clobber
      // good gmw-based entries with nativeValid=false x=0 y=0.
      if (laneDrivenChannel) {
        continue;
      }
    }
    if (seed.hasNativePos) {
      sample.alpha = seed.nativeAlpha;
    } else if (seed.channel == OBJ_CHANNEL_GAME_SAVED ||
               seed.channel == OBJ_CHANNEL_SAVE_PROGRESS) {
      sample.alpha = laneRecent ? laneUseA : 1.0f;
    } else if (isGmwSample) {
      sample.alpha = laneUseA;
    } else {
      sample.alpha = 1.0f;
    }
    sample.visibleChars = -1;
    sample.totalChars = -1;
    if (seed.hasNativePos) {
      sample.callerOffset = seed.callerOffset;
    } else if (sample.callerOffset == 0) {
      sample.callerOffset = 0;
    }
    DWORD sampleTick = now;
    if (!seed.hasNativePos && !isGmwSample) {
      sampleTick = (seed.tick != 0) ? seed.tick : now;
    }
    sample.frameId = (unsigned int)(sampleTick / 16);
    sample.tick = sampleTick;
    TextHook_OnObjectiveConsumerSample(sample);

    auto it = s_lastSampleLog.find(seed.key);
    if (it == s_lastSampleLog.end() || (now - it->second) > 600) {
      s_lastSampleLog[seed.key] = now;
      char sbuf[360];
      sprintf_s(
          sbuf,
          "[OBJ-CONS-SAMPLE] key=%s od=%u x=%.2f y=%.2f a=%.2f frame=%u src=%s "
          "vw=%u vh=%u caller=+0x%X tick=%lu sp=%.2f",
          seed.key.c_str(), sample.ownerdrawId, sample.x, sample.y, sample.alpha,
          sample.frameId, srcTag, sample.virtualW, sample.virtualH,
          sample.callerOffset, (unsigned long)sample.tick, sampleSpacing);
      LogToFile(sbuf);
    }
  }

  // === GMW lane keep-alive === DISABLED for gameplay objectives.
  // The renderAge + s_objRenderCache lifecycle system now manages objective
  // display timing. KEEPALIVE created consumer samples via
  // TextHook_OnObjectiveConsumerSample, but non-consumer entries (from FSHook)
  // are skipped in that function, causing it to CREATE duplicate entries with
  // different runtimeInstanceId ??slot jumps ??position jumps (OBJ-POS-JUMP).
  // Keeping this disabled prevents Issues 1 (text below English) and 2
  // (3-objective display corruption).
}

static bool LooksLikeGameplayPromptEnglish(const std::string &english) {
  if (english.empty())
    return false;
  std::string t = TrimAscii(StripColorCodesSimple(english));
  if (t.empty())
    return false;
  std::string u;
  u.reserve(t.size());
  for (char c : t) {
    u.push_back((char)std::toupper((unsigned char)c));
  }
  if (u.rfind("PRESS", 0) == 0 || u.rfind("HOLD", 0) == 0 ||
      u.rfind("TAP", 0) == 0 || u.rfind("USE", 0) == 0) {
    return true;
  }
  const bool hasVerb = (u.find("PRESS ") != std::string::npos ||
                        u.find("HOLD ") != std::string::npos ||
                        u.find("TAP ") != std::string::npos ||
                        u.find("USE ") != std::string::npos);
  if (hasVerb && u.find(" TO ") != std::string::npos) {
    return true;
  }
  if (u.find("[{+") != std::string::npos || u.find("&&") != std::string::npos) {
    return true;
  }
  return false;
}

static bool IsCorneredScannerStatusKey(const std::string &key) {
  if (key == "CORNERED_READY" || key == "CORNERED_NO" ||
      key == "CORNERED_MATCH" || key == "CORNERED_DATA") {
    return true;
  }
  if (key.find("SCANNING") != std::string::npos ||
      key.find("IDENTIFIED") != std::string::npos ||
      key.find("DEGREE_SYMBOL") != std::string::npos) {
    return true;
  }
  return false;
}

static bool IsCorneredBinocularActionHintKey(const std::string &key) {
  return (key.rfind("CORNERED_BINOCULARS_", 0) == 0 &&
          key.find("_HINT") != std::string::npos);
}

static bool IsHudRuntimeTailJoinKey(const std::string &key) {
  return (key == "PLATFORM_SWAPWEAPONS" ||
          key == "PLATFORM_SWAPWEAPONSGAMEPAD" ||
          key == "PLATFORM_PICKUPNEWWEAPON" ||
          key == "PLATFORM_PICKUPNEWWEAPONGAMEPAD");
}

static bool IsTransientObjectiveChannel(ObjectiveChannel ch) {
  return (ch == OBJ_CHANNEL_GAMEPLAY_UPDATE ||
          ch == OBJ_CHANNEL_GAME_SAVED ||
          ch == OBJ_CHANNEL_SAVE_PROGRESS);
}

static bool IsNativeObjectiveRenderChannel(ObjectiveChannel ch) {
  return (ch == OBJ_CHANNEL_GAMEPLAY_LIST ||
          ch == OBJ_CHANNEL_GAMEPLAY_UPDATE ||
          ch == OBJ_CHANNEL_GAME_SAVED ||
          ch == OBJ_CHANNEL_SAVE_PROGRESS);
}

static void RenderNativeObjectivesOnly(IDXGISwapChain *pSwapChain, DWORD now,
                                       bool suppressHudOverlay,
                                       bool hudPauseMenu,
                                       bool objectiveHiddenByGate,
                                       const std::vector<HudNativeEntry> &hudNativeEntriesFrame) {
#if GHOSTSKOR_OBJECTIVE_REVERSE
  if (RuntimeFlags_ObjectiveStatusEnableOsRevOnly()) {
    (void)hudNativeEntriesFrame;
    if (suppressHudOverlay) {
      return;
    }

    TextHook_ReadAuthoritativeObjectiveSnapshot();
    std::vector<ObjectiveStatusNativeLine> nativeLines =
        TextHook_GetAuthoritativeObjectiveSnapshot();
    if (nativeLines.empty()) {
      return;
    }
    TextHook_RunHudElemArrayScan();
    TextHook_ReadLiveHudElems();
    const std::vector<NativeObjectiveItem> liveObjectiveItems =
        TextHook_GetNativeObjectiveItems();

    UpdateScreenPlacement();
    if (!g_FrameSwapChainDescValid) {
      return;
    }
    const DXGI_SWAP_CHAIN_DESC &objScDesc = g_FrameSwapChainDesc;
    const float screenWidth = (float)objScDesc.BufferDesc.Width;
    const float screenHeight = (float)objScDesc.BufferDesc.Height;
    const float resMul = g_ActiveArea.height / 1080.0f;
    const float scaleX = g_ActiveArea.width / 1280.0f;
    const float scaleY = g_ActiveArea.height / 720.0f;
    const float scaleF = g_ActiveArea.height / 480.0f;
    const float objScaleX = s_scrPlaceValid ? s_scrPlaceScale[0] : scaleF;
    const float objScaleY = s_scrPlaceValid ? s_scrPlaceScale[1] : scaleF;
    EngineSafeArea sa = TextHook_GetEngineSafeArea();
    const float safeOffX =
        (sa.x > 1.0f && sa.x < screenWidth * 0.5f) ? sa.x : (24.0f * scaleX);
    const float safeOffY =
        (sa.y > 1.0f && sa.y < screenHeight * 0.5f) ? sa.y : (24.0f * scaleY);
    const float objSafeX = s_scrPlaceValid ? s_scrPlaceViewMin[0] : safeOffX;
    const float objSafeY = s_scrPlaceValid ? s_scrPlaceViewMin[1] : safeOffY;
    const float objectiveGateAlpha = objectiveHiddenByGate ? 0.0f : 1.0f;
    auto LaneSlotFromNativeY = [](float y) -> int {
      int slot = (int)lroundf((y - 10.0f) / 20.0f);
      if (slot < -8) slot = -8;
      if (slot > 12) slot = 12;
      return slot;
    };
    auto FindLiveObjectiveItem =
        [&](const ObjectiveStatusNativeLine &line) -> const NativeObjectiveItem * {
      const NativeObjectiveItem *best = nullptr;
      int bestScore = -1000000;
      for (const auto &item : liveObjectiveItems) {
        int score = 0;
        bool matchedIdentity = false;
        if (line.instanceSig != 0 && item.instanceSig != 0) {
          if (item.instanceSig == line.instanceSig) {
            score += 24;
            matchedIdentity = true;
          } else if (line.cfgIndex == 0 && line.sourceElemPtrOrIndex == 0 &&
                     line.fxBirthTime <= 0 && line.key.empty()) {
            continue;
          }
        }
        if (!line.key.empty()) {
          if (item.key.empty()) {
            if (line.cfgIndex == 0 && line.sourceElemPtrOrIndex == 0 &&
                line.fxBirthTime <= 0) {
              continue;
            }
          } else if (item.key != line.key) {
            if (line.cfgIndex == 0 && line.sourceElemPtrOrIndex == 0 &&
                line.fxBirthTime <= 0) {
              continue;
            }
          } else {
            score += 12;
            if (item.validKey) {
              score += 2;
            }
            matchedIdentity = true;
          }
        }
        if (line.cfgIndex != 0) {
          if (item.cfgIndex != 0 && item.cfgIndex != line.cfgIndex) {
            continue;
          }
          if (item.cfgIndex == line.cfgIndex) {
            score += 10;
            matchedIdentity = true;
          }
        }
        if (line.fxBirthTime > 0) {
          if (item.fxBirthTime > 0 && item.fxBirthTime != line.fxBirthTime) {
            continue;
          }
          if (item.fxBirthTime == line.fxBirthTime) {
            score += 6;
            matchedIdentity = true;
          }
        }
        if (line.sourceElemPtrOrIndex != 0) {
          if (item.rawPtr != 0 &&
              line.sourceElemPtrOrIndex == (uint64_t)item.rawPtr) {
            score += 14;
            matchedIdentity = true;
          } else if (line.sourceElemPtrOrIndex ==
                     (uint64_t)(uint32_t)item.arrayIndex) {
            score += 8;
            matchedIdentity = true;
          }
        }
        if (!matchedIdentity) {
          continue;
        }
        if (LaneSlotFromNativeY(item.y) == line.laneSlot) {
          score += 2;
        }
        if (item.alpha > 0.01f) {
          score += 1;
        }
        if (score > bestScore) {
          best = &item;
          bestScore = score;
        }
      }
      return (bestScore >= 10) ? best : nullptr;
    };
    auto StatusLineSig = [](const ObjectiveStatusNativeLine &line) -> std::string {
      if (line.sourceSeq != 0) {
        return line.key + "|" + std::to_string(line.sourceSeq);
      }
      return line.key + "|lane|" + std::to_string(line.laneSlot) + "|cfg|" +
             std::to_string(line.cfgIndex);
    };

    // Time-based alpha envelope for status messages.
    // HudElem +0x30 alpha is fadeovertime TARGET (instantly 0 on fade start),
    // so we manage status display lifetime ourselves.
    struct StatusAlphaEntry {
      DWORD firstSeen = 0;
      DWORD lastSeen = 0;
      unsigned int seq = 0;
    };
    static std::unordered_map<std::string, StatusAlphaEntry> s_statusAlphaMap;
    // Clear status alpha map on game reset so status messages get fresh timers
    {
      static unsigned long s_statusAlphaResetTickSeen = 0;
      const unsigned long statusAlphaResetTick = TextHook_GetObjectiveRuntimeResetTick();
      if (statusAlphaResetTick != 0 && statusAlphaResetTick != s_statusAlphaResetTickSeen) {
        s_statusAlphaResetTickSeen = statusAlphaResetTick;
        s_statusAlphaMap.clear();
      }
    }
    struct StatusEnvelopeTiming {
      float fadeInMs;
      float holdMs;
      float fadeOutMs;
    };
    auto StatusEnvelopeForKey = [](const std::string &key) -> StatusEnvelopeTiming {
      // Match the legacy status event timings so osauth/native status does not
      // outlive the original engine path for update/completed/save messages.
      if (key == "EXE_GAMESAVED" || key == "CGAME_NOW_SAVING") {
        return {750.0f, 3250.0f, 500.0f};  // 4.5s total
      }
      if (key == "GAME_OBJECTIVESUPDATED" || key == "GAME_OBJECTIVECOMPLETED" ||
          key == "GAME_OBJECTIVEFAILED") {
        return {750.0f, 2750.0f, 600.0f};  // 4.1s total
      }
      return {750.0f, 3250.0f, 500.0f};
    };
    std::unordered_map<std::string, float> statusEnvelopeAlphaBySig;
    statusEnvelopeAlphaBySig.reserve(nativeLines.size() + 4);

    // Keep the alpha state until the source line disappears. If a stale source
    // keeps emitting the same key/lane after its fade has completed, erasing by
    // age alone would re-register it as a fresh message and create a ghost
    // reappearance cycle.
    constexpr DWORD kStatusAlphaIdleExpiryMs = 2000u;
    for (auto it = s_statusAlphaMap.begin(); it != s_statusAlphaMap.end();) {
      const DWORD idleAge =
          (it->second.lastSeen != 0 && now >= it->second.lastSeen)
              ? (now - it->second.lastSeen)
              : 0;
      if (it->second.lastSeen != 0 && idleAge > kStatusAlphaIdleExpiryMs) {
        it = s_statusAlphaMap.erase(it);
      } else {
        ++it;
      }
    }

    // Register new status lines and compute time-based alpha
    for (auto &line : nativeLines) {
      if (line.channel != OBJSTAT_NATIVE_STATUS) {
        continue;
      }
      // Timer ownership must follow the status event sequence, not the lane.
      // The same native status can temporarily fall back from its visual lane
      // to lane -1 and then recover, and that lane flip must not restart the
      // alpha timer as a fresh ghost message.
      const std::string statusSig = StatusLineSig(line);
      auto itS = s_statusAlphaMap.find(statusSig);
      if (itS == s_statusAlphaMap.end()) {
        s_statusAlphaMap[statusSig] = {now, now, line.sourceSeq};
        itS = s_statusAlphaMap.find(statusSig);
      } else {
        if (itS->second.seq != line.sourceSeq) {
          itS->second.firstSeen = now;
          itS->second.seq = line.sourceSeq;
        }
        itS->second.lastSeen = now;
      }
      const StatusEnvelopeTiming timing = StatusEnvelopeForKey(line.key);
      const float kStatusFadeInMs = timing.fadeInMs;
      const float kStatusHoldMs = timing.holdMs;
      const float kStatusFadeOutMs = timing.fadeOutMs;
      const float kStatusTotalMs =
          kStatusFadeInMs + kStatusHoldMs + kStatusFadeOutMs;
      const float ageMs = (float)(now - itS->second.firstSeen);
      float envAlpha;
      if (ageMs < kStatusFadeInMs) {
        envAlpha = ageMs / kStatusFadeInMs;
      } else if (ageMs < kStatusFadeInMs + kStatusHoldMs) {
        envAlpha = 1.0f;
      } else if (ageMs < kStatusTotalMs) {
        envAlpha = 1.0f - (ageMs - kStatusFadeInMs - kStatusHoldMs) / kStatusFadeOutMs;
      } else {
        envAlpha = 0.0f;
      }
      statusEnvelopeAlphaBySig[statusSig] = Clamp01(envAlpha);
    }

    std::unordered_set<int> occupiedStatusLaneSlots;
    occupiedStatusLaneSlots.reserve(8);
    for (const auto &line : nativeLines) {
      if (line.channel != OBJSTAT_NATIVE_STATUS) {
        continue;
      }
      const std::string statusSig = StatusLineSig(line);
      const auto itEnv = statusEnvelopeAlphaBySig.find(statusSig);
      const float occupiedAlpha =
          (itEnv != statusEnvelopeAlphaBySig.end()) ? itEnv->second
                                                    : Clamp01(line.alpha);
      const float gate =
          hudPauseMenu ? 0.0f : (objectiveGateAlpha * occupiedAlpha);
      if (gate > 0.01f) {
        occupiedStatusLaneSlots.insert(line.laneSlot);
      }
    }

    auto osauthLineSourceName = [](const ObjectiveStatusNativeLine &line)
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
    auto osauthLineRenderSig = [](const ObjectiveStatusNativeLine &line)
        -> std::string {
      if (line.instanceSig != 0) {
        return line.key + "|owner|" +
               std::to_string((unsigned long long)line.instanceSig);
      }
      if (line.channel == OBJSTAT_NATIVE_STATUS) {
        if (line.sourceSeq != 0) {
          return line.key + "|" + std::to_string(line.sourceSeq);
        }
        return line.key + "|lane|" + std::to_string(line.laneSlot) + "|cfg|" +
               std::to_string(line.cfgIndex);
      }
      if (line.sourceElemPtrOrIndex != 0 && line.fxBirthTime > 0) {
        return line.key + "|elem|" +
               std::to_string((unsigned long long)line.sourceElemPtrOrIndex) +
               "|fx|" + std::to_string(line.fxBirthTime);
      }
      return line.key + "|lane|" + std::to_string(line.laneSlot) + "|cfg|" +
             std::to_string(line.cfgIndex);
    };
    struct ObjLiveDiagState {
      bool hadLive = false;
      unsigned long lastSeenTick = 0;
    };
    static std::unordered_map<std::string, ObjLiveDiagState> s_objLiveDiagState;
    for (auto it = s_objLiveDiagState.begin(); it != s_objLiveDiagState.end();) {
      const unsigned long age =
          (now >= it->second.lastSeenTick) ? (now - it->second.lastSeenTick) : 0;
      if (it->second.lastSeenTick != 0 && age > 15000UL) {
        it = s_objLiveDiagState.erase(it);
      } else {
        ++it;
      }
    }

    static std::unordered_map<std::string, DWORD> s_osrevRenderLogTick;
    // Per-key dedup — multiple producer instances for the same key can create
    // separate native lines with independent alpha timers, causing them to
    // render simultaneously ("tangling").
    std::unordered_set<std::string> renderedStatusKeys;
    std::unordered_set<std::string> renderedObjectiveKeys;
    for (const auto &line : nativeLines) {
      if (line.text.empty()) {
        continue;
      }
      if (line.channel == OBJSTAT_NATIVE_OBJECTIVE &&
          occupiedStatusLaneSlots.find(line.laneSlot) !=
              occupiedStatusLaneSlots.end()) {
        continue;
      }
      if (!line.key.empty()) {
        if (line.channel == OBJSTAT_NATIVE_STATUS &&
            renderedStatusKeys.count(line.key)) {
          continue;
        }
        if (line.channel == OBJSTAT_NATIVE_OBJECTIVE &&
            renderedObjectiveKeys.count(line.key)) {
          continue;
        }
        // NOTE: insert moved to AFTER alpha check — see below KoreanRenderer::QueueText
      }

      float gateAlpha = objectiveGateAlpha;
      if (line.channel == OBJSTAT_NATIVE_STATUS && hudPauseMenu) {
        gateAlpha = 0.0f;
      }

      const NativeObjectiveItem *liveObjective = FindLiveObjectiveItem(line);
      if (line.channel == OBJSTAT_NATIVE_OBJECTIVE) {
        const std::string liveSig = osauthLineRenderSig(line);
        ObjLiveDiagState &diag = s_objLiveDiagState[liveSig];
        const bool hasLiveObjective = (liveObjective != nullptr);
        const bool stateKnown = (diag.lastSeenTick != 0);
        if (!stateKnown || diag.hadLive != hasLiveObjective) {
          char dbuf[640];
          const unsigned long lineAge =
              (now >= line.lifeStart) ? (now - line.lifeStart) : 0;
          sprintf_s(
              dbuf,
              "[OBJ-LIVE-%s] key=%s src=%s lane=%d cfg=0x%X elem=%llu fxB=%d "
              "lineAge=%lums alpha=%.2f live=%d liveAlpha=%.2f server=%d "
              "letter=%d x=%.1f y=%.1f",
              hasLiveObjective ? (stateKnown ? "RESTORE" : "BIND")
                               : (stateKnown ? "MISS" : "MISS-SPAWN"),
              line.key.c_str(), osauthLineSourceName(line), line.laneSlot,
              (unsigned int)line.cfgIndex,
              (unsigned long long)line.sourceElemPtrOrIndex, line.fxBirthTime,
              lineAge, line.alpha, hasLiveObjective ? 1 : 0,
              hasLiveObjective ? liveObjective->alpha : 0.0f,
              hasLiveObjective ? liveObjective->serverTime : 0,
              hasLiveObjective ? liveObjective->fxLetterTime : 0, line.x,
              line.y);
          LogToFile(dbuf);
        }
        diag.hadLive = hasLiveObjective;
        diag.lastSeenTick = now;
      }
      std::string renderText = line.text;
      float x = line.x;
      float y = line.y;
      if (line.virtualW != 0 || line.virtualH != 0) {
        x = objSafeX + (line.x * objScaleX);
        y = objSafeY + (line.y * objScaleY);
      }
      const float fontHeight =
          ((line.fontHeight > 5.0f && line.fontHeight < 200.0f)
              ? line.fontHeight
              : 78.0f) * resMul;
      float scale =
          (line.scale > 0.02f && line.scale < 10.0f) ? line.scale : 1.45f;
      uint32_t colorPacked = line.colorPacked;
      float lineAlpha = Clamp01(line.alpha);
      const bool statusHasVisual =
          (line.channel == OBJSTAT_NATIVE_STATUS) &&
          ((line.authorityFlags & OBJSTAT_LINE_HAS_VISUAL) != 0u);
      const bool statusSyntheticOnly =
          (line.channel == OBJSTAT_NATIVE_STATUS) &&
          ((line.authorityFlags & OBJSTAT_LINE_STATUS_PROBE_FALLBACK) != 0u);
      const std::string statusSig =
          (line.channel == OBJSTAT_NATIVE_STATUS) ? StatusLineSig(line)
                                                  : std::string();
      const auto itStatusEnv =
          (line.channel == OBJSTAT_NATIVE_STATUS)
              ? statusEnvelopeAlphaBySig.find(statusSig)
              : statusEnvelopeAlphaBySig.end();
      const float statusEnvAlpha =
          (itStatusEnv != statusEnvelopeAlphaBySig.end()) ? itStatusEnv->second
                                                          : Clamp01(line.alpha);
      const char *renderClockSrc =
          (line.channel == OBJSTAT_NATIVE_OBJECTIVE) ? "osauth_cap" : "osauth";
      int rawVisibleChars = -1;
      if (liveObjective != nullptr) {
        x = objSafeX + (liveObjective->x * objScaleX);
        y = objSafeY + (liveObjective->y * objScaleY);
        if (liveObjective->fontScale > 0.02f && liveObjective->fontScale < 10.0f) {
          scale = liveObjective->fontScale;
        }
        colorPacked = liveObjective->colorPacked;
        if (line.channel == OBJSTAT_NATIVE_STATUS) {
          lineAlpha = statusHasVisual ? Clamp01(line.alpha) : statusEnvAlpha;
          renderClockSrc = statusHasVisual ? "osauth_live_status_visual"
                                           : "osauth_live_status";
        } else {
          lineAlpha = Clamp01(liveObjective->alpha);
          renderClockSrc = "osauth_live";
        }

        ObjectiveChannel ch = ProbeObjectiveChannelFromKey(line.key);
        if (ch == OBJ_CHANNEL_NONE) {
          ch = (line.y <= -20.0f) ? OBJ_CHANNEL_GAMEPLAY_UPDATE
                                  : OBJ_CHANNEL_GAMEPLAY_LIST;
        }
        const size_t totalChars = CountUtf8Chars(line.text);
        if (ch == OBJ_CHANNEL_GAMEPLAY_LIST &&
            liveObjective->fxLetterTime > 0 && liveObjective->fxBirthTime > 0 &&
            totalChars > 0) {
          const int typeClockUsed = liveObjective->serverTime;
          const bool nativeClockPlausible =
              (typeClockUsed > 0 && typeClockUsed >= liveObjective->fxBirthTime &&
               (typeClockUsed - liveObjective->fxBirthTime) <= 20000);
          if (nativeClockPlausible) {
            // Scale fxLetterTime so Korean typewriter finishes at the same
            // time as the English one.  English has more characters, so
            // the per-character interval must be stretched for Korean.
            int scaledLetterTime = liveObjective->fxLetterTime;
            {
              std::string korLookup, engLookup;
              if (!line.key.empty() &&
                  TextHook_GetLocalizedKeyText(line.key, korLookup,
                                               engLookup) &&
                  !engLookup.empty()) {
                const size_t engChars = CountUtf8Chars(engLookup);
                if (engChars > totalChars && totalChars > 0) {
                  scaledLetterTime =
                      (int)((float)liveObjective->fxLetterTime *
                            (float)engChars / (float)totalChars);
                  if (scaledLetterTime <= 0) scaledLetterTime = 1;
                }
              }
            }
            rawVisibleChars =
                (typeClockUsed - liveObjective->fxBirthTime) /
                scaledLetterTime;
            if (rawVisibleChars <= 0) {
              continue;
            }
            if ((size_t)rawVisibleChars < totalChars) {
              renderText =
                  Utf8PrefixByChars(line.text, (size_t)rawVisibleChars);
            }
            renderClockSrc = "osauth_live_native";
          }
        }
      } else if (line.channel == OBJSTAT_NATIVE_STATUS) {
        lineAlpha = statusHasVisual ? Clamp01(line.alpha) : statusEnvAlpha;
        if (statusSyntheticOnly) {
          renderClockSrc = "osauth_status_probe";
        }
      } else if (line.channel == OBJSTAT_NATIVE_OBJECTIVE) {
        // Fallback typewriter when FindLiveObjectiveItem fails but the
        // OSAUTH snapshot carries valid typewriter parameters from the
        // intro capture.  Uses wall-clock age as the typewriter clock
        // because the native serverTime is not available without a live
        // HudElem match.
        ObjectiveChannel ch = ProbeObjectiveChannelFromKey(line.key);
        if (ch == OBJ_CHANNEL_NONE) {
          ch = (line.y <= -20.0f) ? OBJ_CHANNEL_GAMEPLAY_UPDATE
                                  : OBJ_CHANNEL_GAMEPLAY_LIST;
        }
        if (ch == OBJ_CHANNEL_GAMEPLAY_LIST && line.fxLetterTime > 0 &&
            line.fxBirthTime > 0 && line.lifeStart > 0) {
          const size_t totalChars = CountUtf8Chars(line.text);
          if (totalChars > 0) {
            const unsigned long lineAge =
                (now >= line.lifeStart) ? (now - line.lifeStart) : 0;
            // Scale fxLetterTime for Korean char count (same as native path).
            unsigned long scaledLetterTime = (unsigned long)line.fxLetterTime;
            {
              std::string korLookup, engLookup;
              if (!line.key.empty() &&
                  TextHook_GetLocalizedKeyText(line.key, korLookup,
                                               engLookup) &&
                  !engLookup.empty()) {
                const size_t engChars = CountUtf8Chars(engLookup);
                if (engChars > totalChars && totalChars > 0) {
                  scaledLetterTime =
                      (unsigned long)((float)line.fxLetterTime *
                                      (float)engChars / (float)totalChars);
                  if (scaledLetterTime == 0) scaledLetterTime = 1;
                }
              }
            }
            rawVisibleChars = (int)(lineAge / scaledLetterTime);
            if (rawVisibleChars <= 0) {
              continue;  // still zero visible chars — skip this frame
            }
            if ((size_t)rawVisibleChars < totalChars) {
              renderText =
                  Utf8PrefixByChars(line.text, (size_t)rawVisibleChars);
            }
            renderClockSrc = "osauth_fallback_typewriter";
          }
        }
      }
      if (line.channel == OBJSTAT_NATIVE_STATUS && statusSyntheticOnly &&
          !statusHasVisual && liveObjective == nullptr) {
        lineAlpha = Clamp01(
            lineAlpha * StatusQuickPulseAlphaScale(line.key, line.sourceSeq, now));
      }

      // Synthetic typewriter for objectives: works regardless of whether
      // the native fxLetterTime-based paths above produced a result.
      // If rawVisibleChars is still -1, the native typewriter did NOT fire
      // (fxLetterTime == 0, FindLiveObjectiveItem failed, timing already
      // complete, etc.).  Apply a wall-clock typewriter so Korean text
      // reveals character by character matching the game's visual timing.
      if (rawVisibleChars == -1 && line.channel == OBJSTAT_NATIVE_OBJECTIVE) {
        ObjectiveChannel synthCh = ProbeObjectiveChannelFromKey(line.key);
        if (synthCh == OBJ_CHANNEL_NONE) {
          synthCh = (line.y <= -20.0f) ? OBJ_CHANNEL_GAMEPLAY_UPDATE
                                       : OBJ_CHANNEL_GAMEPLAY_LIST;
        }
        if (synthCh == OBJ_CHANNEL_GAMEPLAY_LIST) {
          const size_t synthTotalChars = CountUtf8Chars(renderText);
          if (synthTotalChars > 0) {
            // Track first-seen per unique line to drive the typewriter clock.
            struct ObjTypewriterEntry {
              DWORD firstSeen = 0;
              uint64_t instanceSig = 0;
              int fxBirth = 0;
            };
            static std::unordered_map<std::string, ObjTypewriterEntry>
                s_objTypewriter;
            // Prune old entries
            for (auto it = s_objTypewriter.begin();
                 it != s_objTypewriter.end();) {
              if ((now - it->second.firstSeen) > 30000) {
                it = s_objTypewriter.erase(it);
              } else {
                ++it;
              }
            }
            // Clear on game reset
            {
              static unsigned long s_objTypeResetTick = 0;
              const unsigned long typeResetTick =
                  TextHook_GetObjectiveRuntimeResetTick();
              if (typeResetTick != 0 &&
                  typeResetTick != s_objTypeResetTick) {
                s_objTypeResetTick = typeResetTick;
                s_objTypewriter.clear();
              }
            }

            const std::string typeKey =
                line.key + "|" +
                std::to_string((unsigned long long)line.instanceSig);
            auto itType = s_objTypewriter.find(typeKey);
            if (itType == s_objTypewriter.end()) {
              s_objTypewriter[typeKey] = {now, line.instanceSig,
                                          line.fxBirthTime};
              itType = s_objTypewriter.find(typeKey);
            } else if (itType->second.fxBirth != line.fxBirthTime) {
              // New typewriter instance (different fxBirthTime) — restart.
              itType->second = {now, line.instanceSig, line.fxBirthTime};
            }

            const DWORD typeElapsed =
                (now >= itType->second.firstSeen)
                    ? (now - itType->second.firstSeen)
                    : 0;
            // Compute per-character interval scaled to match English duration.
            // Default: ~1.2s total if no English text available.
            unsigned long synthLetterTime = 1200UL / synthTotalChars;
            {
              std::string korLookup, engLookup;
              if (!line.key.empty() &&
                  TextHook_GetLocalizedKeyText(line.key, korLookup,
                                               engLookup) &&
                  !engLookup.empty()) {
                const size_t engChars = CountUtf8Chars(engLookup);
                if (engChars > 0) {
                  // Use 50ms per English char as baseline if fxLetterTime is 0.
                  const unsigned long baseLetter =
                      (line.fxLetterTime > 0) ? (unsigned long)line.fxLetterTime
                                              : 50UL;
                  // total English duration → same duration for Korean
                  synthLetterTime =
                      (baseLetter * engChars) / synthTotalChars;
                }
              }
            }
            if (synthLetterTime == 0) synthLetterTime = 1;

            const int synthVisible =
                (int)(typeElapsed / synthLetterTime);
            if (synthVisible <= 0) {
              continue;  // typewriter not started yet — skip this frame
            }
            if ((size_t)synthVisible < synthTotalChars) {
              renderText =
                  Utf8PrefixByChars(line.text, (size_t)synthVisible);
            }
            rawVisibleChars = synthVisible;
            renderClockSrc = "synthetic_typewriter";
          }
        }
      }

      const float outAlpha = lineAlpha * gateAlpha;
      if (outAlpha <= 0.01f || renderText.empty()) {
        continue;
      }
      float color[4] = {
          (float)((colorPacked >> 0) & 0xFF) / 255.0f,
          (float)((colorPacked >> 8) & 0xFF) / 255.0f,
          (float)((colorPacked >> 16) & 0xFF) / 255.0f,
          outAlpha};
      const float width =
          KoreanRenderer::MeasureTextWidthEx(renderText, fontHeight, scale);

      KoreanRenderer::QueueText(renderText, x, y, scale, color, fontHeight, 0,
                                width, false, false, 1.5f, false, false, true,
                                0, screenWidth * 0.86f, true, true);

      // Mark this key as rendered — AFTER alpha check passed and text was
      // actually queued, so stale lines with alpha=0 don't block valid ones.
      if (!line.key.empty()) {
        if (line.channel == OBJSTAT_NATIVE_STATUS)
          renderedStatusKeys.insert(line.key);
        else if (line.channel == OBJSTAT_NATIVE_OBJECTIVE)
          renderedObjectiveKeys.insert(line.key);
      }

      if (kVerboseRuntimeLogs) {
        const std::string logSig =
            line.key + "|" + std::to_string(line.laneSlot) + "|" +
            std::to_string((unsigned int)line.channel);
        auto itLog = s_osrevRenderLogTick.find(logSig);
        if (itLog == s_osrevRenderLogTick.end() || (now - itLog->second) > 600) {
          s_osrevRenderLogTick[logSig] = now;
          if (line.channel == OBJSTAT_NATIVE_STATUS) {
            const std::string statusLogSig = StatusLineSig(line);
            auto itSA = s_statusAlphaMap.find(statusLogSig);
            const float statusAgeS = itSA != s_statusAlphaMap.end()
                ? (float)(now - itSA->second.firstSeen) / 1000.0f : 0.0f;
            char sbuf[448];
            sprintf_s(
                sbuf,
                "[STATUS-MSG-RENDER] key=%s seq=%u x=%.1f y=%.1f age=%.1fs a=%.2f lane=%d src=osauth lineSrc=%s",
                line.key.c_str(), line.sourceSeq, x, y, statusAgeS, outAlpha,
                line.laneSlot, osauthLineSourceName(line));
            LogToFile(sbuf);
          } else {
            char obuf[448];
            sprintf_s(
                obuf,
                "[OBJ-LATCH-RENDER] key=%s lane=%d x=%.1f y=%.1f a=%.2f vc=%d clk=%s lineSrc=%s",
                line.key.c_str(), line.laneSlot, x, y, outAlpha,
                rawVisibleChars, renderClockSrc, osauthLineSourceName(line));
            LogToFile(obuf);
          }
        }
      }
    }

    // ---- Status event rendering from event queue ----
    // Renders ALL status messages (save, objective updated/completed/failed)
    // directly from the status event queue with self-contained timing.
    // The OSAUTH native path does not produce lines for these keys because
    // the probe fallback is disabled and no status producers fire.
    {
      std::vector<ObjectiveStatusEvent> statusEvents =
          TextHook_GetObjectiveStatusEvents();
      struct StatusEventAlpha {
        DWORD firstSeen = 0;
        DWORD lastSeen = 0;
        unsigned int seq = 0;
      };
      static std::unordered_map<std::string, StatusEventAlpha> s_statusEventAlpha;
      {
        static unsigned long s_statusEventResetTick = 0;
        const unsigned long statusResetTick =
            TextHook_GetObjectiveRuntimeResetTick();
        if (statusResetTick != 0 && statusResetTick != s_statusEventResetTick) {
          s_statusEventResetTick = statusResetTick;
          s_statusEventAlpha.clear();
        }
      }
      for (auto it = s_statusEventAlpha.begin();
           it != s_statusEventAlpha.end();) {
        const DWORD idle =
            (it->second.lastSeen != 0 && now >= it->second.lastSeen)
                ? (now - it->second.lastSeen)
                : 0;
        if (it->second.lastSeen != 0 && idle > 2000) {
          it = s_statusEventAlpha.erase(it);
        } else {
          ++it;
        }
      }

      for (const auto &ev : statusEvents) {
        if (ev.korean.empty()) {
          continue;
        }
        // Skip if the OSAUTH native path already rendered this key
        // (can happen when HudElem position drifts and the join succeeds).
        if (renderedStatusKeys.count(ev.key)) {
          continue;
        }

        // Use the key|seq composite for independent alpha tracking per event
        const std::string alphaSig =
            ev.key + "|" + std::to_string(ev.sequence);
        auto itAlpha = s_statusEventAlpha.find(alphaSig);
        if (itAlpha == s_statusEventAlpha.end()) {
          s_statusEventAlpha[alphaSig] = {now, now, ev.sequence};
          itAlpha = s_statusEventAlpha.find(alphaSig);
        } else {
          itAlpha->second.lastSeen = now;
        }

        // Timing: save keys get longer hold, update keys are briefer
        const bool isSaveKey =
            (ev.key == "EXE_GAMESAVED" || ev.key == "CGAME_NOW_SAVING");
        const float fadeInMs = isSaveKey ? 750.0f : 200.0f;
        const float holdMs = isSaveKey ? 3250.0f : 2800.0f;
        const float fadeOutMs = 500.0f;
        const float totalMs = fadeInMs + holdMs + fadeOutMs;
        const float ageMs = (float)(now - itAlpha->second.firstSeen);
        float envAlpha;
        if (ageMs < fadeInMs) {
          envAlpha = ageMs / fadeInMs;
        } else if (ageMs < fadeInMs + holdMs) {
          envAlpha = 1.0f;
        } else if (ageMs < totalMs) {
          envAlpha =
              1.0f - (ageMs - fadeInMs - holdMs) / fadeOutMs;
        } else {
          envAlpha = 0.0f;
        }
        envAlpha = Clamp01(envAlpha);
        if (hudPauseMenu) {
          envAlpha = 0.0f;
        }
        const float statusAlpha = envAlpha * objectiveGateAlpha;
        if (statusAlpha <= 0.01f) {
          continue;
        }

        const float statusX = objSafeX + (6.0f * objScaleX);
        // First status key at y=10, second at y=26 (if multiple active)
        const float statusY = objSafeY + (10.0f * objScaleY);
        constexpr float statusFontH = 78.0f;
        constexpr float statusScale = 1.25f;
        float statusColor[4] = {1.0f, 1.0f, 1.0f, statusAlpha};

        std::string statusRenderText = ev.korean;
        for (char &c : statusRenderText) {
          if (c == '\n' || c == '\r')
            c = ' ';
        }
        const float statusWidth = KoreanRenderer::MeasureTextWidthEx(
            statusRenderText, statusFontH, statusScale);
        KoreanRenderer::QueueText(statusRenderText, statusX, statusY,
                                  statusScale, statusColor, statusFontH, 0,
                                  statusWidth, false, false, 1.5f, false,
                                  false, true, 0, screenWidth * 0.86f, true,
                                  true);
        renderedStatusKeys.insert(ev.key);

        if (kVerboseRuntimeLogs) {
          static std::unordered_map<std::string, DWORD> s_statusEvRenderLogTick;
          auto itLog = s_statusEvRenderLogTick.find(ev.key);
          if (itLog == s_statusEvRenderLogTick.end() ||
              (now - itLog->second) > 600) {
            s_statusEvRenderLogTick[ev.key] = now;
            char sbuf[384];
            sprintf_s(sbuf,
                      "[STATUS-MSG-RENDER] key=%s seq=%u x=%.1f y=%.1f "
                      "age=%.1fs a=%.2f src=event_queue",
                      ev.key.c_str(), ev.sequence, statusX, statusY,
                      ageMs / 1000.0f, statusAlpha);
            LogToFile(sbuf);
          }
        }
      }
    }

    return;
  }

  // ?? Cache structures ??????????????????????????????????????????????????
  // Instance cache: (cfgText << 32 | fxBirthTime) ??key. ABSOLUTE PRIORITY.
  // Lane cache: slot ??(key, fxBirthTime). Second priority (same birth only).
  // INVARIANT: Once an instanceId is mapped to a key, it NEVER changes.
  struct LaneTypewriterCache {
    std::string key;
    int fxBirthTime = 0;
    DWORD startClock = 0;
    DWORD tick = 0;
    bool recoveredSeed = false;
  };
  struct ObjectiveRenderLatchEntry {
    std::string key;
    std::string text;
    ObjectiveChannel ch = OBJ_CHANNEL_NONE;
    int laneSlot = 0;
    int fxBirthTime = 0;
    int fxLetterTime = 0;
    float x = 0.0f;
    float y = 0.0f;
    float scale = 1.25f;
    uint32_t colorPacked = 0xFFFFFFFFu;
    DWORD lastSeenTick = 0;
    DWORD typeStartClock = 0;
    bool fullText = false;
    bool valid = false;
  };
  static std::unordered_map<int, LaneTypewriterCache> s_laneTypewriterCache;
  static std::unordered_map<std::string, ObjectiveRenderLatchEntry>
      s_objectiveRenderLatchByKey;
  static const DWORD kLaneKeyCacheMs = 180000;
  static DWORD s_pauseStartTick = 0;
  static DWORD s_pauseAccumMs = 0;
  static unsigned long s_lastObjectiveResetTick = 0;
  auto LaneSlotFromY = [](float y) -> int {
    int slot = (int)lroundf((y - 10.0f) / 20.0f);
    // Preserve negative lanes (e.g. y=-68 notification lane => slot -4).
    // Clamping to [0..8] mixes notification with objective lane 0 and causes
    // key swaps like "Game Saved" <-> objective update.
    if (slot < -8) slot = -8;
    if (slot > 12) slot = 12;
    return slot;
  };

  // ?? Per-frame setup ???????????????????????????????????????????????????
  UpdateScreenPlacement();
  TextHook_RunHudElemArrayScan();
  TextHook_ReadLiveHudElems();

  std::vector<NativeObjectiveItem> nativeItems = TextHook_GetNativeObjectiveItems();

  // ?? Front menu detection (multi-frame debounce) ???????????????????????
  // ?? Mission restart detection ?????????????????????????????????????????
  const unsigned long objectiveResetTick = TextHook_GetObjectiveRuntimeResetTick();
  if (objectiveResetTick != 0 && objectiveResetTick != s_lastObjectiveResetTick) {
    s_lastObjectiveResetTick = objectiveResetTick;
    s_laneTypewriterCache.clear();
    s_objectiveRenderLatchByKey.clear();
    s_pauseStartTick = 0;
    s_pauseAccumMs = 0;
  }

  // ?? Cache expiry ??????????????????????????????????????????????????????
  for (auto it = s_laneTypewriterCache.begin(); it != s_laneTypewriterCache.end();) {
    if ((now - it->second.tick) > kLaneKeyCacheMs)
      it = s_laneTypewriterCache.erase(it);
    else
      ++it;
  }

  // ?? Pause accumulation ????????????????????????????????????????????????
  if (hudPauseMenu) {
    if (s_pauseStartTick == 0) s_pauseStartTick = now;
  } else if (s_pauseStartTick != 0) {
    s_pauseAccumMs += (now - s_pauseStartTick);
    s_pauseStartTick = 0;
  }
  DWORD gameplayClockNow = now - s_pauseAccumMs;
  if (hudPauseMenu && s_pauseStartTick != 0) {
    gameplayClockNow = s_pauseStartTick - s_pauseAccumMs;
  }
  const float objectiveGateAlpha = objectiveHiddenByGate ? 0.0f : 1.0f;

  if (objectiveGateAlpha <= 0.0f) {
    static DWORD s_lastObjectiveGateHideLog = 0;
    if ((now - s_lastObjectiveGateHideLog) > 800) {
      s_lastObjectiveGateHideLog = now;
      char gbuf[256];
      sprintf_s(gbuf,
                "[OBJ-GATE-HIDE] items=%d pause=%d suppress=%d hidden=%d",
                (int)nativeItems.size(), hudPauseMenu ? 1 : 0,
                suppressHudOverlay ? 1 : 0, objectiveHiddenByGate ? 1 : 0);
      LogToFile(gbuf);
    }
  }

  // ?? Screen geometry ???????????????????????????????????????????????????
  if (!g_FrameSwapChainDescValid) return;
  const DXGI_SWAP_CHAIN_DESC &objScDesc = g_FrameSwapChainDesc;
  const float screenWidth = (float)objScDesc.BufferDesc.Width;
  const float screenHeight = (float)objScDesc.BufferDesc.Height;
  const float resMul2 = g_ActiveArea.height / 1080.0f;
  const float scaleX = g_ActiveArea.width / 1280.0f;
  const float scaleY = g_ActiveArea.height / 720.0f;
  const float scaleF = g_ActiveArea.height / 480.0f;
  const float objScaleX = s_scrPlaceValid ? s_scrPlaceScale[0] : scaleF;
  const float objScaleY = s_scrPlaceValid ? s_scrPlaceScale[1] : scaleF;
  EngineSafeArea sa = TextHook_GetEngineSafeArea();
  const float safeOffX = (sa.x > 1.0f && sa.x < screenWidth * 0.5f) ? sa.x : (24.0f * scaleX);
  const float safeOffY = (sa.y > 1.0f && sa.y < screenHeight * 0.5f) ? sa.y : (24.0f * scaleY);
  const float objSafeX = s_scrPlaceValid ? s_scrPlaceViewMin[0] : safeOffX;
  const float objSafeY = s_scrPlaceValid ? s_scrPlaceViewMin[1] : safeOffY;
  const float kObjBaseFontH = 78.0f * resMul2;
  const std::vector<LiveHudScriptCountdown> liveCountdownsFrame =
      TextHook_GetLiveHudScriptCountdowns();

  if (!hudPauseMenu && !liveCountdownsFrame.empty()) {
    static std::unordered_map<std::string, DWORD> s_liveCountdownRenderLogTick;
    auto ComputeCountdownPos = [&](const LiveHudScriptCountdown &countdown,
                                   float &outX, float &outY) {
      switch (countdown.horzAlign) {
        case 1:
          outX = screenWidth * 0.5f + countdown.x * scaleF;
          break;
        case 2:
          outX = (screenWidth - objSafeX) + countdown.x * scaleF;
          break;
        default:
          outX = objSafeX + countdown.x * scaleF;
          break;
      }
      switch (countdown.vertAlign) {
        case 1:
          outY = screenHeight * 0.5f + countdown.y * scaleF;
          break;
        case 2:
          outY = (screenHeight - objSafeY) + countdown.y * scaleF;
          break;
        default:
          outY = objSafeY + countdown.y * scaleF;
          break;
      }
    };

    for (const auto &countdown : liveCountdownsFrame) {
      if (!countdown.active || countdown.renderText.empty() ||
          countdown.key == "CLOCKWORK_POWERDOWN") {
        continue;
      }

      float renderX = 0.0f;
      float renderY = 0.0f;
      ComputeCountdownPos(countdown, renderX, renderY);
      if (!std::isfinite(renderX) || !std::isfinite(renderY)) {
        continue;
      }

      float color[4] = {
          (float)((countdown.colorPacked >> 0) & 0xFF) / 255.0f,
          (float)((countdown.colorPacked >> 8) & 0xFF) / 255.0f,
          (float)((countdown.colorPacked >> 16) & 0xFF) / 255.0f,
          Clamp01(countdown.alpha)};
      if (color[3] <= 0.01f) {
        color[3] = 1.0f;
      }
      const bool forceWhiteCountdown =
          (countdown.key == "CLOCKWORK_POWERDOWN");
      if (forceWhiteCountdown) {
        color[0] = 1.0f;
        color[1] = 1.0f;
        color[2] = 1.0f;
      }

      const float renderScale =
          (countdown.fontScale > 0.05f && countdown.fontScale < 5.0f)
              ? (countdown.fontScale * 1.3f)
              : 1.0f;
      const float englishWidth = KoreanRenderer::MeasureTextWidthEx(
          countdown.englishText.empty() ? countdown.rawText : countdown.englishText,
          kObjBaseFontH, renderScale, 1);

      DrawStylePatch stylePatch{};
      stylePatch.applyGlow = !forceWhiteCountdown;
      stylePatch.glowR = 0.30f;
      stylePatch.glowG = 0.60f;
      stylePatch.glowB = 0.30f;
      stylePatch.glowA = color[3];

      KoreanRenderer::QueueText(
          countdown.renderText, renderX, renderY, renderScale, color,
          kObjBaseFontH, 0, englishWidth, false, false, renderScale, false,
          false, true, 0, 0.0f, false, true, 1, &stylePatch);

      const std::string logSig =
          countdown.key + "|" + std::to_string((unsigned long long)countdown.rawPtr);
      auto itLog = s_liveCountdownRenderLogTick.find(logSig);
      if (itLog == s_liveCountdownRenderLogTick.end() ||
          (now - itLog->second) > 900) {
        s_liveCountdownRenderLogTick[logSig] = now;
        char tbuf[512];
        sprintf_s(
            tbuf,
            "[HUD-TIMER-LIVE-RENDER] key=%s x=%.1f y=%.1f scale=%.3f fs=%.2f align=%u/%u timer=%.3f text=\"%.96s\"",
            countdown.key.c_str(), renderX, renderY, renderScale,
            countdown.fontScale, countdown.horzAlign, countdown.vertAlign,
            countdown.timerValue, countdown.renderText.c_str());
        LogToFile(tbuf);
      }
    }
  }

  // ?? Sort native items by Y-lane ???????????????????????????????????????
  std::stable_sort(nativeItems.begin(), nativeItems.end(),
                   [&](const NativeObjectiveItem &a, const NativeObjectiveItem &b) {
                     int laneA = LaneSlotFromY(a.y);
                     int laneB = LaneSlotFromY(b.y);
                     if (laneA != laneB) return laneA < laneB;
                     return a.arrayIndex < b.arrayIndex;
                   });

  // Keep one authoritative native item per lane to avoid duplicate rendering
  // when engine emits shadow/clone HudElems in the same slot.
  {
    struct LanePick {
      NativeObjectiveItem item{};
      float score = -99999.0f;
      bool set = false;
    };
    std::unordered_map<int, LanePick> bestByLane;
    bestByLane.reserve(nativeItems.size() + 4);
    for (const auto &ni : nativeItems) {
      const int slot = LaneSlotFromY(ni.y);
      float score = Clamp01(ni.alpha) * 100.0f;
      if (ni.flags == 0x7) score += 40.0f;
      else if (ni.flags == 0x5) score += 30.0f;
      if (ni.cfgIndex != 0) score += 20.0f;
      if (ni.fxBirthTime > 0) score += 10.0f;

      auto &pick = bestByLane[slot];
      if (!pick.set || score > pick.score ||
          (fabsf(score - pick.score) <= 0.001f &&
           ni.arrayIndex < pick.item.arrayIndex)) {
        pick.item = ni;
        pick.score = score;
        pick.set = true;
      }
    }

    if (bestByLane.size() < nativeItems.size()) {
      std::vector<NativeObjectiveItem> uniqueItems;
      uniqueItems.reserve(bestByLane.size());
      for (const auto &kv : bestByLane) {
        if (kv.second.set) uniqueItems.push_back(kv.second.item);
      }
      std::stable_sort(uniqueItems.begin(), uniqueItems.end(),
                       [&](const NativeObjectiveItem &a,
                           const NativeObjectiveItem &b) {
                         int laneA = LaneSlotFromY(a.y);
                         int laneB = LaneSlotFromY(b.y);
                         if (laneA != laneB) return laneA < laneB;
                         return a.arrayIndex < b.arrayIndex;
                       });
      static DWORD s_lastLaneDedupLog = 0;
      if ((now - s_lastLaneDedupLog) > 1200) {
        s_lastLaneDedupLog = now;
        char dbuf[160];
        sprintf_s(dbuf, "[OBJ-LANE-DEDUP] in=%d out=%d",
                  (int)nativeItems.size(), (int)uniqueItems.size());
        LogToFile(dbuf);
      }
      nativeItems.swap(uniqueItems);
    }
  }

  // ?? Build FSHook fallback pool (all gameplay-renderable, firstSeen asc) ??
  struct FsHookCandidate {
    std::string key;
    ObjectiveChannel ch = OBJ_CHANNEL_NONE;
    unsigned long firstSeen = 0;
    unsigned long lastSeen = 0;
  };
  std::vector<FsHookCandidate> fsPool;

  // ?? Resolve: instance cache ??lane cache ??FSHook pool ????????????????
  struct LaneResolvedItem {
    const NativeObjectiveItem *item = nullptr;
    int laneSlot = 0;
    bool resolved = false;
    std::string key;
    ObjectiveChannel ch = OBJ_CHANNEL_NONE;
    const char *keySrc = "none";
    unsigned long fsFirstSeen = 0;  // FSHook detection time (for typewriter catchup)
  };
  std::vector<LaneResolvedItem> laneResolved;
  laneResolved.reserve(nativeItems.size());
  std::unordered_set<std::string> usedKeys;
  auto IsNotificationLane = [](const NativeObjectiveItem &item) -> bool {
    if (item.flags == 0x7) {
      return true;
    }
    return (item.y <= -20.0f && item.y >= -140.0f &&
            item.fontScale >= 1.50f && item.fontScale <= 2.60f);
  };
  auto IsListStyleLane = [&](const NativeObjectiveItem &item) -> bool {
    if (IsNotificationLane(item)) {
      return false;
    }
    if (item.flags == 0x5) {
      return true;
    }
    return (item.y >= -5.0f && item.y <= 105.0f &&
            item.fontScale >= 1.10f && item.fontScale <= 1.40f);
  };

  for (size_t i = 0; i < nativeItems.size(); ++i) {
    const auto &item = nativeItems[i];
    const bool notificationLane = IsNotificationLane(item);
    LaneResolvedItem r{};
    r.item = &item;
    r.laneSlot = LaneSlotFromY(item.y);

    // Priority 0: Native cfg/slc-resolved key from HudElem reverse.
    // This is the authoritative path when reverse key extraction succeeds.
    if (item.validKey && !item.key.empty() &&
        usedKeys.find(item.key) == usedKeys.end()) {
      HudKeyType nativeKeyType = ClassifyHudKey(item.key);
      if (nativeKeyType == HUD_KEY_HINT ||
          nativeKeyType == HUD_KEY_OBJ_HUD_HINT) {
        continue;
      }
      ObjectiveChannel nativeCh = ProbeObjectiveChannelFromKey(item.key);
      if (nativeCh == OBJ_CHANNEL_NONE) {
        nativeCh = notificationLane ? OBJ_CHANNEL_GAMEPLAY_UPDATE
                                    : OBJ_CHANNEL_GAMEPLAY_LIST;
      }
      if (IsNativeObjectiveRenderChannel(nativeCh)) {
        std::string tmpKor;
        std::string tmpEng;
        if (TextHook_GetLocalizedKeyText(item.key, tmpKor, tmpEng) &&
            !tmpKor.empty()) {
          r.key = item.key;
          r.ch = nativeCh;
          r.keySrc = "native_cfg";
          r.resolved = true;
          // Try to grab fsFirstSeen from FSHook pool for typewriter timing
          for (const auto &cand : fsPool) {
            if (cand.key == item.key && cand.firstSeen != 0) {
              r.fsFirstSeen = cand.firstSeen;
              break;
            }
          }
          usedKeys.insert(r.key);
          laneResolved.push_back(std::move(r));
          continue;
        }
      }
    }
    laneResolved.push_back(std::move(r));
    continue;
  }

  // ?? Diagnostic: unresolved lanes ??????????????????????????????????????
  {
    static std::unordered_map<int, DWORD> s_objUnresolvedLogTick;
    static std::unordered_map<int, DWORD> s_objUnresolvedNonLaneLogTick;
    static std::unordered_map<uint64_t, DWORD> s_objUnresolvedFirstSeenTick;
    bool hasRecentStatusEvent = false;
    {
      std::vector<ObjectiveStatusEvent> statusEvents =
          TextHook_GetObjectiveStatusEvents();
      for (const auto &ev : statusEvents) {
        if (ev.key.empty()) {
          continue;
        }
        const DWORD ageMs =
            (now >= ev.triggerTick) ? (now - ev.triggerTick)
                                    : (ev.triggerTick - now);
        if (ageMs <= 3500) {
          hasRecentStatusEvent = true;
          break;
        }
      }
    }
    for (const auto &r : laneResolved) {
      if (r.resolved || !r.item) continue;
      if (hasRecentStatusEvent) {
        const bool listLaneBand =
            (r.laneSlot >= 0 && r.laneSlot <= 2 && r.item->y >= -5.0f &&
             r.item->y <= 70.0f && r.item->fontScale >= 1.10f &&
             r.item->fontScale <= 1.40f);
        const bool notificationBand =
            (r.item->y <= -20.0f && r.item->y >= -140.0f &&
             r.item->fontScale >= 1.50f);
        if (listLaneBand || notificationBand) {
          continue;
        }
      }
      if (r.item->fxBirthTime <= 0) {
        continue;
      }
      const uint64_t unresolvedSig =
          ((uint64_t)r.item->cfgIndex << 32) ^
          ((uint64_t)(uint32_t)r.item->fxBirthTime << 8) ^
          (uint64_t)(uint8_t)(r.laneSlot & 0xFF);
      auto itFirstSeen = s_objUnresolvedFirstSeenTick.find(unresolvedSig);
      if (itFirstSeen == s_objUnresolvedFirstSeenTick.end()) {
        s_objUnresolvedFirstSeenTick[unresolvedSig] = now;
        continue;
      }
      if (now >= itFirstSeen->second && (now - itFirstSeen->second) < 30000) {
        continue;
      }
      if (s_objUnresolvedFirstSeenTick.size() > 8192) {
        for (auto itf = s_objUnresolvedFirstSeenTick.begin();
             itf != s_objUnresolvedFirstSeenTick.end();) {
          if (now >= itf->second && (now - itf->second) > 120000) {
            itf = s_objUnresolvedFirstSeenTick.erase(itf);
          } else {
            ++itf;
          }
        }
      }
      if (r.laneSlot < 0) {
        const int nonLaneKey =
            (r.item->arrayIndex << 8) | ((r.laneSlot & 0xFF) ^ 0x80);
        auto itNonLane = s_objUnresolvedNonLaneLogTick.find(nonLaneKey);
        if (itNonLane == s_objUnresolvedNonLaneLogTick.end() ||
            (now - itNonLane->second) > 1000) {
          s_objUnresolvedNonLaneLogTick[nonLaneKey] = now;
          char nbuf[384];
          sprintf_s(
              nbuf,
              "[OBJ-UNRESOLVED-NONLANE] idx=%d lane=%d cfg=0x%X x=%.1f y=%.1f "
              "fxB=%d fxL=%d fsPool=%d",
              r.item->arrayIndex, r.laneSlot, r.item->cfgIndex, r.item->x,
              r.item->y, r.item->fxBirthTime, r.item->fxLetterTime,
              (int)fsPool.size());
          LogToFile(nbuf);
        }
        continue;
      }
      const int slotKey = (r.item->arrayIndex << 8) | (r.laneSlot & 0xFF);
      auto it = s_objUnresolvedLogTick.find(slotKey);
      if (it != s_objUnresolvedLogTick.end() && (now - it->second) <= 1000) continue;
      s_objUnresolvedLogTick[slotKey] = now;
      char ubuf[384];
      sprintf_s(ubuf,
                "[OBJ-UNRESOLVED] idx=%d lane=%d cfg=0x%X x=%.1f y=%.1f "
                "fxB=%d fxL=%d fsPool=%d",
                r.item->arrayIndex, r.laneSlot, r.item->cfgIndex,
                r.item->x, r.item->y, r.item->fxBirthTime, r.item->fxLetterTime,
                (int)fsPool.size());
      LogToFile(ubuf);
    }
  }

  // ?? Render resolved lanes + update caches ?????????????????????????????
  static std::unordered_map<std::string, DWORD> s_nativeRenderLogTick;
  static std::unordered_map<std::string, DWORD> s_nativeNoLocLogTick;
  static std::unordered_map<std::string, DWORD> s_nativeTypeSkipLogTick;
  static std::unordered_map<std::string, DWORD> s_nativeGateSkipLogTick;
  static std::unordered_map<std::string, DWORD> s_nativeStatusSkipLogTick;
  struct StatusNativeAnchor {
    float x = 0.0f;
    float y = 0.0f;
    float fontScale = 1.25f;
    float alpha = 0.0f;
    uint32_t colorPacked = 0xFFFFFFFF;
    bool valid = false;
  };
  std::unordered_map<std::string, StatusNativeAnchor> statusNativeAnchorByKey;
  statusNativeAnchorByKey.reserve(8);
  std::unordered_set<std::string> renderedObjectiveKeys;
  renderedObjectiveKeys.reserve(laneResolved.size() + 8);

  for (const auto &r : laneResolved) {
    if (!r.item || !r.resolved || r.key.empty()) continue;
    const auto &item = *r.item;
    const int laneSlot = r.laneSlot;
    const std::string &resolvedKey = r.key;
    ObjectiveChannel ch = r.ch;
    const char *keySrc = r.keySrc;
    if (IsObjectiveStatusMessageKey(resolvedKey)) {
      StatusNativeAnchor anchor{};
      anchor.x = objSafeX + (item.x * objScaleX);
      anchor.y = objSafeY + (item.y * objScaleY);
      anchor.fontScale =
          (item.fontScale > 0.05f && item.fontScale < 5.0f)
              ? item.fontScale
              : 1.25f;
      anchor.alpha = Clamp01(item.alpha) * objectiveGateAlpha;
      anchor.colorPacked = item.colorPacked;
      anchor.valid = std::isfinite(anchor.x) && std::isfinite(anchor.y);
      if (anchor.valid) {
        auto itAnchor = statusNativeAnchorByKey.find(resolvedKey);
        if (itAnchor == statusNativeAnchorByKey.end() ||
            anchor.alpha >= itAnchor->second.alpha) {
          statusNativeAnchorByKey[resolvedKey] = anchor;
        }
      }

      std::string slogKey =
          resolvedKey + "|" + std::to_string(item.cfgIndex) + "|" +
          std::to_string(item.arrayIndex);
      auto itSkip = s_nativeStatusSkipLogTick.find(slogKey);
      if (itSkip == s_nativeStatusSkipLogTick.end() ||
          (now - itSkip->second) > 1000) {
        s_nativeStatusSkipLogTick[slogKey] = now;
        char sbuf[320];
        sprintf_s(sbuf,
                  "[OBJ-NATIVE-SKIP-STATUS] key=%s idx=%d cfg=0x%X src=%s",
                  resolvedKey.c_str(), item.arrayIndex, item.cfgIndex, keySrc);
        LogToFile(sbuf);
      }
      continue;
    }


    // ?? Localization lookup ?????????????????????????????????????????
    std::string korean, english;
    if (!TextHook_GetLocalizedKeyText(resolvedKey, korean, english)) {
      std::string nlogKey = resolvedKey + "|" + std::to_string(item.cfgIndex);
      auto itNoLoc = s_nativeNoLocLogTick.find(nlogKey);
      if (itNoLoc == s_nativeNoLocLogTick.end() || (now - itNoLoc->second) > 1200) {
        s_nativeNoLocLogTick[nlogKey] = now;
        char lbuf[384];
        sprintf_s(lbuf, "[OBJ-NATIVE-SKIP-NOLOC] key=%s src=%s idx=%d cfg=0x%X",
                  resolvedKey.c_str(), keySrc, item.arrayIndex, item.cfgIndex);
        LogToFile(lbuf);
      }
      continue;
    }

    std::string displayText =
        BindingResolver::ResolveBindingsInText(StripColorCodesSimple(korean),
                                               StripColorCodesSimple(english));
    if (displayText.empty()) continue;
    for (char &c : displayText) {
      if (c == '\n' || c == '\r') c = ' ';
    }

    // Decouple objective list rendering from transient HudElem frame noise.
    // Use resolved key as activation signal and render from a latched state.
    if (ch == OBJ_CHANNEL_GAMEPLAY_LIST) {
      ObjectiveRenderLatchEntry &lat = s_objectiveRenderLatchByKey[resolvedKey];
      const bool newLatch = !lat.valid;
      const int prevFxBirth = lat.fxBirthTime;
      const bool newInstance =
          (!newLatch && prevFxBirth > 0 && item.fxBirthTime > 0 &&
           prevFxBirth != item.fxBirthTime);
      const bool resetLatchPlacement = (newLatch || newInstance);
      lat.valid = true;
      lat.key = resolvedKey;
      lat.text = displayText;
      lat.ch = ch;
      if (resetLatchPlacement || lat.laneSlot < 0) {
        lat.laneSlot = laneSlot;
      }
      lat.fxBirthTime = item.fxBirthTime;
      lat.fxLetterTime = item.fxLetterTime;
      lat.scale = (item.fontScale > 0.05f && item.fontScale < 5.0f)
                      ? item.fontScale
                      : 1.25f;
      lat.x = objSafeX + (item.x * objScaleX);
      const float liveY = objSafeY + (item.y * objScaleY);
      if (resetLatchPlacement || !std::isfinite(lat.y)) {
        if (lat.laneSlot >= 0) {
          const float slotY =
              objSafeY + ((10.0f + 20.0f * (float)lat.laneSlot) * objScaleY);
          lat.y = slotY;
        } else {
          lat.y = liveY;
        }
      }
      if (item.colorPacked != 0) {
        lat.colorPacked = item.colorPacked;
      }
      if (resetLatchPlacement || lat.typeStartClock == 0) {
        const bool recoveredKey = (strcmp(keySrc, "instance_cache") == 0 ||
                                   strcmp(keySrc, "lane_cache") == 0 ||
                                   (strcmp(keySrc, "native_cfg") == 0 &&
                                    r.fsFirstSeen == 0));
        if (recoveredKey) {
          lat.fullText = true;
          lat.typeStartClock = gameplayClockNow;
        } else {
          DWORD catchup = 0;
          if (r.fsFirstSeen != 0 && now > r.fsFirstSeen) {
            catchup = now - (DWORD)r.fsFirstSeen;
            if (catchup > 8000) {
              catchup = 8000;
            }
          }
          lat.fullText = false;
          lat.typeStartClock =
              (gameplayClockNow >= catchup) ? (gameplayClockNow - catchup) : 0;
        }
      }
      lat.lastSeenTick = now;
      renderedObjectiveKeys.insert(resolvedKey);
      continue;
    }

    // ?? Typewriter ??????????????????????????????????????????????????
    std::string renderText = displayText;
    int rawVisibleChars = -1;
    int typeClockUsed = item.serverTime;
    const char *typeClockSrc = "native";
    const size_t totalChars = CountUtf8Chars(displayText);
    if (item.fxLetterTime > 0 && item.fxBirthTime > 0 && totalChars > 0) {
      const bool allowTypewriter = (ch == OBJ_CHANNEL_GAMEPLAY_LIST);
      if (!allowTypewriter) {
        // Update/save notifications should appear immediately, not typewriter-delayed.
        rawVisibleChars = (int)totalChars;
        typeClockSrc = "full_nonlist";
      } else {
        const bool nativeClockPlausible =
            (typeClockUsed > 0 && typeClockUsed >= item.fxBirthTime &&
             (typeClockUsed - item.fxBirthTime) <= 20000);
        if (nativeClockPlausible) {
          rawVisibleChars = (typeClockUsed - item.fxBirthTime) / item.fxLetterTime;
        } else {
          LaneTypewriterCache &tw = s_laneTypewriterCache[laneSlot];
          const bool twUnseeded =
              (tw.fxBirthTime != item.fxBirthTime || tw.startClock == 0 ||
               tw.key != resolvedKey);
          if (twUnseeded) {
            const bool recoveredKey = (strcmp(keySrc, "instance_cache") == 0 ||
                                       strcmp(keySrc, "lane_cache") == 0 ||
                                       (strcmp(keySrc, "native_cfg") == 0 && r.fsFirstSeen == 0));
            DWORD catchup = 0;
            if (recoveredKey) {
              rawVisibleChars = (int)totalChars;
              // Recovered lanes should stay fully visible; avoid clock rewind
              // that causes full_cache -> local visible-char jumps.
              typeClockSrc = "full_cache";
              tw.recoveredSeed = true;
            } else {
              // Use FSHook firstSeen for typewriter catchup: the key was
              // detected by FSHook before the HudElem was matched, so the
              // typewriter should account for that elapsed time.
              if (r.fsFirstSeen != 0 && now > r.fsFirstSeen) {
                catchup = now - (DWORD)r.fsFirstSeen;
                if (catchup > 8000) catchup = 8000; // safety cap
              }
              typeClockSrc = "local";
              rawVisibleChars = (int)(catchup / (DWORD)item.fxLetterTime);
              typeClockUsed = item.fxBirthTime + (int)catchup;
              tw.recoveredSeed = false;
            }
            tw.key = resolvedKey;
            tw.fxBirthTime = item.fxBirthTime;
            tw.startClock = tw.recoveredSeed
                                ? gameplayClockNow
                                : ((gameplayClockNow >= catchup)
                                       ? (gameplayClockNow - catchup)
                                       : 0);
            tw.tick = now;
          } else {
            tw.tick = now;
            if (tw.recoveredSeed) {
              rawVisibleChars = (int)totalChars;
              typeClockSrc = "full_cache_hold";
            } else {
              DWORD elapsed = (gameplayClockNow >= tw.startClock)
                                  ? (gameplayClockNow - tw.startClock)
                                  : 0;
              rawVisibleChars = (int)(elapsed / (DWORD)item.fxLetterTime);
              typeClockUsed = item.fxBirthTime + (int)elapsed;
              typeClockSrc = "local";
            }
          }
        }
      }
      if (rawVisibleChars <= 0) {
        std::string tlogKey = resolvedKey + "|" + std::to_string(item.cfgIndex) +
                              "|" + std::to_string(item.arrayIndex);
        auto itType = s_nativeTypeSkipLogTick.find(tlogKey);
        if (itType == s_nativeTypeSkipLogTick.end() ||
            (now - itType->second) > 800) {
          s_nativeTypeSkipLogTick[tlogKey] = now;
          char tbuf[512];
          sprintf_s(tbuf,
                    "[OBJ-RENDER-SKIP-TYPE] key=%s idx=%d cfg=0x%X ch=%s "
                    "vc=%d tc=%d st=%d fxB=%d fxL=%d clk=%s gclk=%lu",
                    resolvedKey.c_str(), item.arrayIndex, item.cfgIndex,
                    ObjectiveChannelName(ch), rawVisibleChars, (int)totalChars,
                    item.serverTime, item.fxBirthTime, item.fxLetterTime,
                    typeClockSrc, (unsigned long)gameplayClockNow);
          LogToFile(tbuf);
        }
        continue;
      }
      size_t visibleChars = (size_t)rawVisibleChars;
      if (visibleChars < totalChars) {
        renderText = Utf8PrefixByChars(displayText, visibleChars);
      }
    }
    if (renderText.empty()) continue;

    // ?? Position + color from HudElem (native authority) ????????????
    const float x = objSafeX + (item.x * objScaleX);
    const float y = objSafeY + (item.y * objScaleY);
    const float fontHeight = kObjBaseFontH;
    const float scale = (item.fontScale > 0.05f && item.fontScale < 5.0f) ? item.fontScale : 1.25f;
    float color[4] = {0.0f, 0.0f, 0.0f, Clamp01(item.alpha) * objectiveGateAlpha};
    color[0] = (float)((item.colorPacked >> 0) & 0xFF) / 255.0f;
    color[1] = (float)((item.colorPacked >> 8) & 0xFF) / 255.0f;
    color[2] = (float)((item.colorPacked >> 16) & 0xFF) / 255.0f;
    if (color[3] <= 0.01f) {
      std::string glogKey = resolvedKey + "|" + std::to_string(item.cfgIndex) +
                            "|" + std::to_string(item.arrayIndex);
      auto itGate = s_nativeGateSkipLogTick.find(glogKey);
      if (itGate == s_nativeGateSkipLogTick.end() ||
          (now - itGate->second) > 800) {
        s_nativeGateSkipLogTick[glogKey] = now;
        char gbuf[512];
        sprintf_s(gbuf,
                  "[OBJ-RENDER-SKIP-GATE] key=%s idx=%d cfg=0x%X itemA=%.2f "
                  "gate=%.2f outA=%.2f pause=%d suppress=%d hidden=%d",
                  resolvedKey.c_str(), item.arrayIndex, item.cfgIndex,
                  Clamp01(item.alpha), objectiveGateAlpha, color[3],
                  hudPauseMenu ? 1 : 0, suppressHudOverlay ? 1 : 0,
                  objectiveHiddenByGate ? 1 : 0);
        LogToFile(gbuf);
      }
      continue;
    }

    // ?? Render ??????????????????????????????????????????????????????
    float width = KoreanRenderer::MeasureTextWidthEx(renderText, fontHeight, scale);
    const float maxWidthPx = screenWidth * 0.86f;
    KoreanRenderer::QueueText(
        renderText, x, y, scale, color, fontHeight, 0, width,
        false, false, 1.5f, false, false, true, 0, maxWidthPx,
        true, /*yIsTopOfText=*/true);
    renderedObjectiveKeys.insert(resolvedKey);

    // ?? Diagnostic log (throttled) ??????????????????????????????????
    std::string logKey = resolvedKey + "|" + std::to_string(item.cfgIndex) + "|" +
                         std::to_string(item.arrayIndex);
    auto itLog = s_nativeRenderLogTick.find(logKey);
    if (itLog == s_nativeRenderLogTick.end() || (now - itLog->second) > 1200) {
      s_nativeRenderLogTick[logKey] = now;
      char rbuf[512];
      sprintf_s(rbuf,
                "[OBJ-REVERSE] LIVE-RENDER key=%s ch=%s src=%s idx=%d cfg=0x%X "
                "x=%.1f y=%.1f a=%.2f st=%d fxB=%d fxL=%d vc=%d clk=%s",
                resolvedKey.c_str(), ObjectiveChannelName(ch), keySrc, item.arrayIndex,
                item.cfgIndex, x, y, color[3], item.serverTime,
                item.fxBirthTime, item.fxLetterTime, rawVisibleChars, typeClockSrc);
      LogToFile(rbuf);
    }
  }

  const std::vector<LiveHudElem> liveHudElemsFrame = TextHook_GetLiveHudElems();
  std::vector<ObjectiveStatusEvent> statusEventsFrame =
      TextHook_GetObjectiveStatusEvents();
  std::unordered_set<int> occupiedStatusLaneSlots;
  occupiedStatusLaneSlots.reserve(8);
  auto MarkStatusLaneByY = [&](float laneY) {
    if (!std::isfinite(laneY)) {
      return;
    }
    if (laneY < -120.0f || laneY > 140.0f) {
      return;
    }
    occupiedStatusLaneSlots.insert(LaneSlotFromY(laneY));
  };
  auto MarkStatusLaneByKey = [&](const std::string &statusKey) {
    if (statusKey == "EXE_GAMESAVED" || statusKey == "GAME_OBJECTIVESUPDATED") {
      occupiedStatusLaneSlots.insert(0);
      return;
    }
    if (statusKey == "CGAME_NOW_SAVING") {
      occupiedStatusLaneSlots.insert(1);
      return;
    }
    if (statusKey == "GAME_OBJECTIVECOMPLETED") {
      occupiedStatusLaneSlots.insert(3);
      return;
    }
    if (statusKey == "GAME_OBJECTIVEFAILED") {
      occupiedStatusLaneSlots.insert(4);
      return;
    }
  };
  auto StatusTimingForLaneGate = [](const std::string &key) -> std::pair<DWORD, DWORD> {
    if (key == "EXE_GAMESAVED" || key == "CGAME_NOW_SAVING") {
      return {4000u, 500u};
    }
    if (key == "GAME_OBJECTIVESUPDATED" || key == "GAME_OBJECTIVECOMPLETED" ||
        key == "GAME_OBJECTIVEFAILED") {
      return {3500u, 600u};
    }
    return {3500u, 500u};
  };
  for (const auto &le : liveHudElemsFrame) {
    if (!le.active || le.key.empty() || !IsObjectiveStatusMessageKey(le.key)) {
      continue;
    }
    MarkStatusLaneByY(le.y);
    MarkStatusLaneByKey(le.key);
  }
  for (const auto &n : hudNativeEntriesFrame) {
    if (!n.valid || n.key.empty() || !IsObjectiveStatusMessageKey(n.key)) {
      continue;
    }
    const DWORD ageMs = (now >= n.lastUpdate) ? (now - n.lastUpdate) : 0;
    if (ageMs > 700) {
      continue;
    }
    MarkStatusLaneByY(n.y);
    MarkStatusLaneByKey(n.key);
  }
  for (const auto &ev : statusEventsFrame) {
    auto timing = StatusTimingForLaneGate(ev.key);
    const DWORD holdMs = timing.first;
    const DWORD fadeMs = timing.second;
    const DWORD ageMs = (now >= ev.triggerTick) ? (now - ev.triggerTick) : 0;
    if (ageMs <= holdMs + fadeMs) {
      MarkStatusLaneByKey(ev.key);
    }
  }

  // Render objective list from latched states so transient HudElem slot noise
  // does not directly shake on-screen timing/position.
  {
    static std::unordered_map<std::string, DWORD> s_latchedRenderLogTick;
    constexpr DWORD kLatchMissHoldMs = 220;
    constexpr DWORD kLatchMissFadeMs = 280;
    constexpr DWORD kLatchPruneMs = 1400;

    std::vector<ObjectiveRenderLatchEntry> latchedItems;
    latchedItems.reserve(s_objectiveRenderLatchByKey.size());
    for (auto it = s_objectiveRenderLatchByKey.begin();
         it != s_objectiveRenderLatchByKey.end();) {
      const ObjectiveRenderLatchEntry &lat = it->second;
      if (!lat.valid || lat.key.empty()) {
        it = s_objectiveRenderLatchByKey.erase(it);
        continue;
      }
      const DWORD missAge =
          (now >= lat.lastSeenTick) ? (now - lat.lastSeenTick) : 0;
      if (missAge > kLatchPruneMs) {
        it = s_objectiveRenderLatchByKey.erase(it);
        continue;
      }
      latchedItems.push_back(lat);
      ++it;
    }

    std::stable_sort(latchedItems.begin(), latchedItems.end(),
                     [](const ObjectiveRenderLatchEntry &a,
                        const ObjectiveRenderLatchEntry &b) {
                       if (a.laneSlot != b.laneSlot) {
                         return a.laneSlot < b.laneSlot;
                       }
                       return a.key < b.key;
                     });

    for (const auto &lat : latchedItems) {
      if (lat.text.empty()) {
        continue;
      }
      if (occupiedStatusLaneSlots.find(lat.laneSlot) !=
          occupiedStatusLaneSlots.end()) {
        static std::unordered_map<std::string, DWORD> s_latchedStatusSkipLogTick;
        const std::string skipSig =
            lat.key + "|" + std::to_string(lat.laneSlot);
        auto itSkip = s_latchedStatusSkipLogTick.find(skipSig);
        if (itSkip == s_latchedStatusSkipLogTick.end() ||
            (now - itSkip->second) > 900) {
          s_latchedStatusSkipLogTick[skipSig] = now;
          char sbuf[256];
          sprintf_s(sbuf,
                    "[OBJ-LATCH-SKIP-STATUS-LANE] key=%s lane=%d lastSeen=%lu",
                    lat.key.c_str(), lat.laneSlot,
                    (unsigned long)lat.lastSeenTick);
          LogToFile(sbuf);
        }
        continue;
      }

      std::string renderText = lat.text;
      const size_t totalChars = CountUtf8Chars(lat.text);
      int rawVisibleChars = (int)totalChars;
      const char *clockSrc = "latched_full";
      if (lat.fxLetterTime > 0 && totalChars > 0 && !lat.fullText) {
        const DWORD elapsed = (gameplayClockNow >= lat.typeStartClock)
                                  ? (gameplayClockNow - lat.typeStartClock)
                                  : 0;
        rawVisibleChars = (int)(elapsed / (DWORD)lat.fxLetterTime);
        clockSrc = "latched_local";
      }
      if (rawVisibleChars <= 0) {
        continue;
      }
      if ((size_t)rawVisibleChars < totalChars) {
        renderText = Utf8PrefixByChars(lat.text, (size_t)rawVisibleChars);
      }
      if (renderText.empty()) {
        continue;
      }

      const DWORD missAge =
          (now >= lat.lastSeenTick) ? (now - lat.lastSeenTick) : 0;
      float lifeAlpha = 1.0f;
      if (missAge > kLatchMissHoldMs) {
        const DWORD fadeAge = missAge - kLatchMissHoldMs;
        if (fadeAge >= kLatchMissFadeMs) {
          lifeAlpha = 0.0f;
        } else {
          lifeAlpha = 1.0f - ((float)fadeAge / (float)kLatchMissFadeMs);
        }
      }
      const float alpha = Clamp01(objectiveGateAlpha * lifeAlpha);
      if (alpha <= 0.01f) {
        continue;
      }

      float color[4] = {
          (float)((lat.colorPacked >> 0) & 0xFF) / 255.0f,
          (float)((lat.colorPacked >> 8) & 0xFF) / 255.0f,
          (float)((lat.colorPacked >> 16) & 0xFF) / 255.0f,
          alpha};
      const float width =
          KoreanRenderer::MeasureTextWidthEx(renderText, kObjBaseFontH, lat.scale);
      KoreanRenderer::QueueText(renderText, lat.x, lat.y, lat.scale, color,
                                kObjBaseFontH, 0, width, false, false, 1.5f,
                                false, false, true, 0, screenWidth * 0.86f,
                                true, true);

      const std::string logKey = lat.key + "|" + std::to_string(lat.laneSlot);
      auto itLog = s_latchedRenderLogTick.find(logKey);
      if (itLog == s_latchedRenderLogTick.end() ||
          (now - itLog->second) > 1200) {
        s_latchedRenderLogTick[logKey] = now;
        char lbuf[512];
        sprintf_s(lbuf,
                  "[OBJ-LATCH-RENDER] key=%s lane=%d x=%.1f y=%.1f a=%.2f "
                  "vc=%d clk=%s miss=%lu",
                  lat.key.c_str(), lat.laneSlot, lat.x, lat.y, alpha,
                  rawVisibleChars, clockSrc, (unsigned long)missAge);
        LogToFile(lbuf);
      }
    }
  }

  // Status messages:
  // - Text/timing authority comes from ObjectiveStatusEvent queue.
  // - Live/HudNative visuals are optional geometry/alpha anchors only.
  {
    static std::unordered_map<std::string, DWORD> s_statusRenderLogTick;
    static std::unordered_map<std::string, DWORD> s_statusLiveHudElemLogTick;
    const float statusGateAlpha = hudPauseMenu ? 0.0f : 1.0f;

    struct LiveStatusVisual {
      float x = 0.0f;
      float y = 0.0f;
      float scale = 1.25f;
      float alpha = 1.0f;
      uint32_t colorPacked = 0xFFFFFFFF;
      uint32_t cfgIndex = 0;
      int fxBirthTime = 0;
      int fxLetterTime = 0;
      bool fromLive = false;
      bool valid = false;
    };

    std::unordered_map<std::string, LiveStatusVisual> visualByKey;
    std::unordered_map<std::string, LiveStatusVisual> visualByKeyCfg;
    struct LiveCfgStatusOwner {
      std::string key;
      LiveStatusVisual vis{};
      bool valid = false;
    };
    std::unordered_map<uint32_t, LiveCfgStatusOwner> activeStatusOwnerByCfg;
    visualByKey.reserve(8);
    visualByKeyCfg.reserve(8);
    activeStatusOwnerByCfg.reserve(8);
    auto MakeCfgVisualSig = [](const std::string &key,
                               uint32_t cfgIndex) -> std::string {
      return key + "|" + std::to_string((unsigned int)cfgIndex);
    };
    auto ShouldPreferStatusVisual = [](const LiveStatusVisual &dst,
                                       const LiveStatusVisual &cand) {
      const bool preferBySource = (cand.fromLive && !dst.fromLive);
      const bool preferByLane =
          (std::isfinite(cand.y) && std::isfinite(dst.y) &&
           (cand.y < dst.y - 0.5f));
      const bool sameLane =
          (std::isfinite(cand.y) && std::isfinite(dst.y) &&
           (fabsf(cand.y - dst.y) <= 0.5f));
      const bool preferByStrength =
          (sameLane &&
           ((cand.alpha > dst.alpha + 0.01f) ||
            (cand.fxBirthTime > dst.fxBirthTime)));
      return (preferBySource || preferByLane || preferByStrength);
    };
    auto UpsertVisualMap = [&](std::unordered_map<std::string, LiveStatusVisual> &dstMap,
                               const std::string &mapKey,
                               const LiveStatusVisual &cand) {
      auto it = dstMap.find(mapKey);
      if (it == dstMap.end()) {
        dstMap[mapKey] = cand;
        return;
      }
      LiveStatusVisual &dst = it->second;
      if (ShouldPreferStatusVisual(dst, cand)) {
        dst = cand;
      }
    };
    auto UpsertVisual = [&](const std::string &key,
                            const LiveStatusVisual &cand) {
      if (key.empty() || !cand.valid) {
        return;
      }
      UpsertVisualMap(visualByKey, key, cand);
      if (cand.cfgIndex != 0) {
        UpsertVisualMap(visualByKeyCfg, MakeCfgVisualSig(key, cand.cfgIndex),
                        cand);
      }
    };

    const std::vector<LiveHudElem> &liveHudElems = liveHudElemsFrame;
    for (const auto &le : liveHudElems) {
      if (!le.active || le.key.empty() || !IsObjectiveStatusMessageKey(le.key)) {
        continue;
      }
      const bool isSaveStatusKey =
          (le.key == "EXE_GAMESAVED" || le.key == "CGAME_NOW_SAVING");
      if (!le.objectiveLane && !isSaveStatusKey) {
        continue;
      }
      if (!std::isfinite(le.x) || !std::isfinite(le.y) ||
          !std::isfinite(le.fontScale) || !std::isfinite(le.alpha)) {
        continue;
      }
      const DWORD nowLiveLog = GetTickCount();
      const std::string liveSig =
          le.key + "|" + std::to_string((unsigned int)le.cfgIndex) + "|" +
          std::to_string(le.arrayIndex) + "|" +
          std::to_string((unsigned int)le.rawTextSlc) + "|" +
          std::to_string((unsigned int)le.rawLabelSlc);
      auto itLiveLog = s_statusLiveHudElemLogTick.find(liveSig);
      if (itLiveLog == s_statusLiveHudElemLogTick.end() ||
          (nowLiveLog - itLiveLog->second) > 800) {
        s_statusLiveHudElemLogTick[liveSig] = nowLiveLog;
        char lbuf[512];
        sprintf_s(
            lbuf,
            "[STATUS-LIVE-HUDELEM] key=%s cfg=0x%X slc=0x%X ptr=0x%llX idx=%d obj=%d x=%.1f y=%.1f fs=%.2f a=%.2f flags=0x%X textSlc=0x%X labelSlc=0x%X",
            le.key.c_str(), le.cfgIndex, le.slcIndex,
            (unsigned long long)le.rawPtr, le.arrayIndex,
            le.objectiveLane ? 1 : 0, le.x, le.y, le.fontScale, le.alpha,
            (unsigned int)le.flags, le.rawTextSlc, le.rawLabelSlc);
        LogToFile(lbuf);
      }
      LiveStatusVisual cand{};
      cand.x = objSafeX + (le.x * objScaleX);
      cand.y = objSafeY + (le.y * objScaleY);
      cand.scale =
          (le.fontScale > 0.05f && le.fontScale < 5.0f) ? le.fontScale : 1.25f;
      cand.alpha = Clamp01(le.alpha);
      cand.colorPacked = le.color;
      cand.cfgIndex = le.cfgIndex;
      cand.fxBirthTime = le.fxBirthTime;
      cand.fxLetterTime = le.fxLetterTime;
      cand.fromLive = true;
      cand.valid = std::isfinite(cand.x) && std::isfinite(cand.y);
      UpsertVisual(le.key, cand);
      if (cand.cfgIndex != 0) {
        LiveCfgStatusOwner &owner = activeStatusOwnerByCfg[cand.cfgIndex];
        if (!owner.valid || ShouldPreferStatusVisual(owner.vis, cand)) {
          owner.key = le.key;
          owner.vis = cand;
          owner.valid = true;
        }
      }
    }

    auto PackColorToABGR = [](float r, float g, float b, float a) -> uint32_t {
      auto ToByte = [](float v) -> uint32_t {
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
      const uint32_t rr = ToByte(r);
      const uint32_t gg = ToByte(g);
      const uint32_t bb = ToByte(b);
      const uint32_t aa = ToByte(a);
      return (aa << 24) | (bb << 16) | (gg << 8) | rr;
    };

    for (const auto &n : hudNativeEntriesFrame) {
      if (!n.valid || n.key.empty() || !IsObjectiveStatusMessageKey(n.key)) {
        continue;
      }
      const DWORD ageMs = (now >= n.lastUpdate) ? (now - n.lastUpdate) : 0;
      if (ageMs > 700) {
        continue;
      }
      if (!std::isfinite(n.x) || !std::isfinite(n.y)) {
        continue;
      }
      LiveStatusVisual cand{};
      cand.x = n.x;
      cand.y = n.y;
      if (n.virtualW != 0 || n.virtualH != 0) {
        cand.x = objSafeX + (n.x * objScaleX);
        cand.y = objSafeY + (n.y * objScaleY);
      }
      if (std::isfinite(n.yScale) && n.yScale > 0.05f && n.yScale < 5.0f) {
        cand.scale = n.yScale;
      } else if (std::isfinite(n.xScale) && n.xScale > 0.05f &&
                 n.xScale < 5.0f) {
        cand.scale = n.xScale;
      } else {
        cand.scale = 1.25f;
      }
      cand.alpha = Clamp01(n.color[3]);
      cand.colorPacked =
          PackColorToABGR(n.color[0], n.color[1], n.color[2], n.color[3]);
      cand.cfgIndex = 0;
      cand.fromLive = false;
      cand.valid = std::isfinite(cand.x) && std::isfinite(cand.y);
      UpsertVisual(n.key, cand);
    }

    std::vector<ObjectiveStatusEvent> statusEvents = statusEventsFrame;
    // Diagnostic: log status event queue state for save events
    {
      static DWORD s_statusEvtDiagLogTick = 0;
      const DWORD diagNow = GetTickCount();
      if ((diagNow - s_statusEvtDiagLogTick) > 600) {
        for (const auto &diagEv : statusEvents) {
          if (diagEv.key == "EXE_GAMESAVED" || diagEv.key == "CGAME_NOW_SAVING") {
            const DWORD diagAge = (diagNow >= diagEv.triggerTick)
                                      ? (diagNow - diagEv.triggerTick)
                                      : 0;
            char dbuf[384];
            sprintf_s(dbuf,
                      "[STATUS-EVT-DIAG] key=%s seq=%u age=%lums cfg=0x%X "
                      "isig=0x%llX kor=\"%.32s\"",
                      diagEv.key.c_str(), diagEv.sequence, diagAge,
                      diagEv.sourceCfg,
                      (unsigned long long)diagEv.sourceInstanceSig,
                      diagEv.korean.c_str());
            LogToFile(dbuf);
            s_statusEvtDiagLogTick = diagNow;
          }
        }
      }
    }
    if (!statusEvents.empty()) {
      static bool s_statusPauseActive = false;
      static DWORD s_statusPauseStart = 0;
      static std::unordered_map<unsigned int, DWORD> s_statusPauseShiftBySeq;
      static std::unordered_map<unsigned int, DWORD> s_statusFirstVisualTickBySeq;
      static std::unordered_map<unsigned int, DWORD> s_statusLastLiveVisualTickBySeq;
      static std::unordered_map<unsigned int, LiveStatusVisual> s_statusLastVisualBySeq;
      static unsigned long s_statusResetTickSeen = 0;

      const unsigned long statusResetTick = TextHook_GetObjectiveRuntimeResetTick();
      if (statusResetTick != 0 && statusResetTick != s_statusResetTickSeen) {
        s_statusResetTickSeen = statusResetTick;
        s_statusPauseActive = false;
        s_statusPauseStart = 0;
        s_statusRenderLogTick.clear();
        s_statusPauseShiftBySeq.clear();
        s_statusFirstVisualTickBySeq.clear();
        s_statusLastLiveVisualTickBySeq.clear();
        s_statusLastVisualBySeq.clear();
      }

      if (hudPauseMenu) {
        if (!s_statusPauseActive) {
          s_statusPauseActive = true;
          s_statusPauseStart = now;
        }
      } else if (s_statusPauseActive) {
        const DWORD pauseDelta =
            (now >= s_statusPauseStart) ? (now - s_statusPauseStart) : 0;
        for (auto &kv : s_statusPauseShiftBySeq) {
          kv.second += pauseDelta;
        }
        s_statusPauseActive = false;
      }

      std::unordered_map<unsigned int, bool> liveSeq;
      liveSeq.reserve(statusEvents.size() + 4);
      for (const auto &ev : statusEvents) {
        liveSeq[ev.sequence] = true;
        if (s_statusPauseShiftBySeq.find(ev.sequence) ==
            s_statusPauseShiftBySeq.end()) {
          s_statusPauseShiftBySeq[ev.sequence] = 0;
        }
      }
      for (auto it = s_statusPauseShiftBySeq.begin();
           it != s_statusPauseShiftBySeq.end();) {
        if (liveSeq.find(it->first) == liveSeq.end()) {
          it = s_statusPauseShiftBySeq.erase(it);
        } else {
          ++it;
        }
      }
      for (auto it = s_statusFirstVisualTickBySeq.begin();
           it != s_statusFirstVisualTickBySeq.end();) {
        if (liveSeq.find(it->first) == liveSeq.end()) {
          it = s_statusFirstVisualTickBySeq.erase(it);
        } else {
          ++it;
        }
      }
      for (auto it = s_statusLastLiveVisualTickBySeq.begin();
           it != s_statusLastLiveVisualTickBySeq.end();) {
        if (liveSeq.find(it->first) == liveSeq.end()) {
          it = s_statusLastLiveVisualTickBySeq.erase(it);
        } else {
          ++it;
        }
      }
      for (auto it = s_statusLastVisualBySeq.begin();
           it != s_statusLastVisualBySeq.end();) {
        if (liveSeq.find(it->first) == liveSeq.end()) {
          it = s_statusLastVisualBySeq.erase(it);
        } else {
          ++it;
        }
      }

      const DWORD renderNow = s_statusPauseActive ? s_statusPauseStart : now;
      auto StatusTimingForKey = [](const std::string &key) -> std::pair<DWORD, DWORD> {
        if (key == "EXE_GAMESAVED" || key == "CGAME_NOW_SAVING") {
          return {3500u, 500u};
        }
        if (key == "GAME_OBJECTIVESUPDATED" || key == "GAME_OBJECTIVECOMPLETED") {
          return {3000u, 600u};
        }
        return {3000u, 500u};
      };

      struct ActiveStatusEvent {
        ObjectiveStatusEvent ev;
        DWORD startTick = 0;
        DWORD ageMs = 0;
        DWORD holdMs = 0;
        DWORD fadeMs = 0;
      };
      std::vector<ActiveStatusEvent> activeEvents;
      activeEvents.reserve(statusEvents.size());
      for (const auto &ev : statusEvents) {
        auto timing = StatusTimingForKey(ev.key);
        const DWORD holdMs = timing.first;
        const DWORD fadeMs = timing.second;
        const DWORD shiftMs = s_statusPauseShiftBySeq[ev.sequence];
        const uint64_t start64 = (uint64_t)ev.triggerTick + (uint64_t)shiftMs;
        const DWORD startTick = (start64 > 0xFFFFFFFFull)
                                    ? 0xFFFFFFFFu
                                    : (DWORD)start64;
        const DWORD ageMs = (renderNow >= startTick) ? (renderNow - startTick) : 0;
        ActiveStatusEvent aev{};
        aev.ev = ev;
        aev.startTick = startTick;
        aev.ageMs = ageMs;
        aev.holdMs = holdMs;
        aev.fadeMs = fadeMs;
        activeEvents.push_back(std::move(aev));
      }

      std::stable_sort(activeEvents.begin(), activeEvents.end(),
                       [](const ActiveStatusEvent &a,
                          const ActiveStatusEvent &b) {
                         if (a.ev.triggerTick != b.ev.triggerTick) {
                           return a.ev.triggerTick < b.ev.triggerTick;
                         }
                         return a.ev.sequence < b.ev.sequence;
                       });
      {
        // Keep only the newest live event per key. Rendering multiple active
        // entries for the same status key causes false line stacking.
        std::unordered_map<std::string, size_t> latestIndexByKey;
        latestIndexByKey.reserve(activeEvents.size());
        for (size_t idx = 0; idx < activeEvents.size(); ++idx) {
          if (!activeEvents[idx].ev.key.empty()) {
            latestIndexByKey[activeEvents[idx].ev.key] = idx;
          }
        }
        std::vector<ActiveStatusEvent> dedupedEvents;
        dedupedEvents.reserve(activeEvents.size());
        for (size_t idx = 0; idx < activeEvents.size(); ++idx) {
          const auto &aev = activeEvents[idx];
          auto itLatest = latestIndexByKey.find(aev.ev.key);
          if (itLatest != latestIndexByKey.end() && itLatest->second != idx) {
            continue;
          }
          dedupedEvents.push_back(aev);
        }
        activeEvents.swap(dedupedEvents);
      }
      if (activeEvents.size() > 3) {
        activeEvents.erase(activeEvents.begin(),
                           activeEvents.begin() + (activeEvents.size() - 3));
      }

      struct OrderedStatusVisual {
        std::string key;
        LiveStatusVisual vis;
      };
      std::vector<OrderedStatusVisual> orderedVisuals;
      orderedVisuals.reserve(visualByKey.size());
      for (const auto &kv : visualByKey) {
        if (kv.second.valid) {
          orderedVisuals.push_back({kv.first, kv.second});
        }
      }
      std::stable_sort(orderedVisuals.begin(), orderedVisuals.end(),
                       [](const OrderedStatusVisual &a,
                          const OrderedStatusVisual &b) {
                         return a.vis.y < b.vis.y;
                       });
      size_t nextVisual = 0;
      std::unordered_set<std::string> activeEventKeys;
      activeEventKeys.reserve(activeEvents.size());
      for (const auto &aev : activeEvents) {
        if (!aev.ev.key.empty()) {
          activeEventKeys.insert(aev.ev.key);
        }
      }

      const float fallbackX = objSafeX + (6.0f * objScaleX);
      const float fallbackY = objSafeY + (10.0f * objScaleY);
      for (size_t ei = 0; ei < activeEvents.size(); ++ei) {
        const ActiveStatusEvent &aev = activeEvents[ei];
        const ObjectiveStatusEvent &ev = aev.ev;
        const bool isSaveStatusRenderKey =
            (ev.key == "EXE_GAMESAVED" || ev.key == "CGAME_NOW_SAVING");
        if (ev.sourceCfg != 0) {
          auto itOwner = activeStatusOwnerByCfg.find(ev.sourceCfg);
          if (itOwner != activeStatusOwnerByCfg.end() && itOwner->second.valid &&
              !itOwner->second.key.empty() && itOwner->second.key != ev.key) {
            continue;
          }
        }

        std::string statusText = ev.korean;
        std::string localizedEng = ev.english;
        const char *statusTextSrc = "event_queue";
        if (statusText.empty()) {
          if (!TextHook_GetLocalizedKeyText(ev.key, statusText, localizedEng) ||
              statusText.empty()) {
            const char *fixedKor = GetObjectiveStatusKoreanFallback(ev.key);
            if (fixedKor && fixedKor[0]) {
              statusText = fixedKor;
              statusTextSrc = "fixed";
            }
          }
        }
        if (statusText.empty()) {
          continue;
        }
        for (char &c : statusText) {
          if (c == '\n' || c == '\r') {
            c = ' ';
          }
        }

        LiveStatusVisual vis{};
        bool hasVisual = false;
        const char *statusPosSrc = "event_default";
        const char *statusVisualSrc = "event_queue";
        // Save/update status messages keep their own render lanes; visual anchors
        // are safe to consume for color/alpha fidelity.
        const bool blockHudNativeStatusVisual = false;
        if (!isSaveStatusRenderKey && ev.sourceCfg != 0) {
          auto itCfgVisual =
              visualByKeyCfg.find(MakeCfgVisualSig(ev.key, ev.sourceCfg));
          if (itCfgVisual != visualByKeyCfg.end() && itCfgVisual->second.valid &&
              (!blockHudNativeStatusVisual || itCfgVisual->second.fromLive)) {
            vis = itCfgVisual->second;
            hasVisual = true;
            statusPosSrc = vis.fromLive ? "live_lane" : "hud_native";
            statusVisualSrc = "event+key_cfg_visual";
          }
        }
        auto itKeyVisual = visualByKey.find(ev.key);
        if (!isSaveStatusRenderKey && !hasVisual &&
            itKeyVisual != visualByKey.end() && itKeyVisual->second.valid &&
            (!blockHudNativeStatusVisual || itKeyVisual->second.fromLive)) {
          vis = itKeyVisual->second;
          hasVisual = true;
          statusPosSrc = vis.fromLive ? "live_lane" : "hud_native";
          statusVisualSrc = "event+key_visual";
        }
        if (!isSaveStatusRenderKey && !hasVisual && !blockHudNativeStatusVisual) {
          auto itNativeAnchor = statusNativeAnchorByKey.find(ev.key);
          if (itNativeAnchor != statusNativeAnchorByKey.end() &&
              itNativeAnchor->second.valid) {
            const StatusNativeAnchor &anchor = itNativeAnchor->second;
            vis.x = anchor.x;
            vis.y = anchor.y;
            vis.scale = anchor.fontScale;
            vis.alpha = anchor.alpha;
            vis.colorPacked = anchor.colorPacked;
            vis.cfgIndex = ev.sourceCfg;
            vis.fromLive = false;
            vis.valid = true;
            hasVisual = true;
            statusPosSrc = "hud_native";
            statusVisualSrc = "event+native_anchor";
          }
        }
        if (!isSaveStatusRenderKey && !hasVisual && ev.sourceCfg != 0 &&
            nextVisual < orderedVisuals.size()) {
          // Do not borrow visuals owned by another active status key in this
          // frame; otherwise concurrent status events stack onto the same lane.
          while (nextVisual < orderedVisuals.size()) {
            const OrderedStatusVisual &cand = orderedVisuals[nextVisual++];
            const bool ownedByOtherActiveKey =
                !cand.key.empty() && cand.key != ev.key &&
                activeEventKeys.find(cand.key) != activeEventKeys.end();
            if (ownedByOtherActiveKey) {
              continue;
            }
            const bool cfgMismatch =
                (cand.vis.cfgIndex != 0 && cand.vis.cfgIndex != ev.sourceCfg);
            if (cfgMismatch) {
              continue;
            }
            if (blockHudNativeStatusVisual && !cand.vis.fromLive) {
              continue;
            }
            vis = cand.vis;
            hasVisual = true;
            statusPosSrc = vis.fromLive ? "live_lane" : "hud_native";
            statusVisualSrc = "event+ordered_visual";
            break;
          }
        }
        if (!isSaveStatusRenderKey && hasVisual) {
          s_statusLastVisualBySeq[ev.sequence] = vis;
        } else if (!isSaveStatusRenderKey) {
          auto itLastVis = s_statusLastVisualBySeq.find(ev.sequence);
          if (itLastVis != s_statusLastVisualBySeq.end() && itLastVis->second.valid) {
            vis = itLastVis->second;
            hasVisual = true;
            statusPosSrc = "status_seq_cache";
            statusVisualSrc = "event+seq_visual_cache";
          }
        }

        std::string renderStatusText = statusText;
        float statusX = hasVisual ? vis.x : fallbackX;
        float statusY = hasVisual ? vis.y : fallbackY;
        float statusFontH = kObjBaseFontH;
        float statusScale = hasVisual ? vis.scale : 1.25f;
        const bool forceDedicatedStatusLane =
            (ev.key == "EXE_GAMESAVED" || ev.key == "CGAME_NOW_SAVING");
        auto GetDedicatedStatusLaneY = [&](const std::string &statusKey) -> float {
          if (statusKey == "EXE_GAMESAVED") {
            return objSafeY + (10.0f * objScaleY);
          }
          if (statusKey == "CGAME_NOW_SAVING") {
            return objSafeY + (26.0f * objScaleY);
          }
          if (statusKey == "GAME_OBJECTIVESUPDATED") {
            return objSafeY + (48.0f * objScaleY);
          }
          if (statusKey == "GAME_OBJECTIVECOMPLETED") {
            return objSafeY + (68.0f * objScaleY);
          }
          if (statusKey == "GAME_OBJECTIVEFAILED") {
            return objSafeY + (88.0f * objScaleY);
          }
          return objSafeY + (88.0f * objScaleY);
        };
        if (forceDedicatedStatusLane) {
          // Status channel owns its render lane; objective list lanes must not
          // be reused by status text.
          statusX = objSafeX + (6.0f * objScaleX);
          statusY = GetDedicatedStatusLaneY(ev.key);
          statusPosSrc = "status_dedicated_lane";
        }
        if (ev.key == "GAME_OBJECTIVESUPDATED") {
          // Keep OBJECTIVESUPDATED as transient overlay above objective list.
          statusY = objSafeY + (12.0f * objScaleY);
          statusScale = 1.18f;
          statusPosSrc = "transient_overlay";
        }
        DWORD effectiveStartTick = aev.startTick;
        auto itFirst = s_statusFirstVisualTickBySeq.find(ev.sequence);
        if (itFirst == s_statusFirstVisualTickBySeq.end()) {
          s_statusFirstVisualTickBySeq[ev.sequence] = renderNow;
          itFirst = s_statusFirstVisualTickBySeq.find(ev.sequence);
        }
        if (itFirst != s_statusFirstVisualTickBySeq.end() &&
            itFirst->second > effectiveStartTick) {
          effectiveStartTick = itFirst->second;
        }
        const DWORD effectiveAgeMs =
            (renderNow >= effectiveStartTick) ? (renderNow - effectiveStartTick) : 0;
        const bool hasLiveVisual = (hasVisual && vis.fromLive && vis.alpha > 0.01f);
        if (hasLiveVisual) {
          s_statusLastLiveVisualTickBySeq[ev.sequence] = renderNow;
        }
        bool keepAliveByRecentLiveVisual = false;
        auto itLiveTick = s_statusLastLiveVisualTickBySeq.find(ev.sequence);
        if (itLiveTick != s_statusLastLiveVisualTickBySeq.end()) {
          const DWORD liveAge =
              (renderNow >= itLiveTick->second) ? (renderNow - itLiveTick->second) : 0;
          const bool allowLiveKeepAlive =
              (ev.key == "EXE_GAMESAVED" || ev.key == "CGAME_NOW_SAVING");
          keepAliveByRecentLiveVisual = allowLiveKeepAlive && (liveAge <= 420);
        }
        if (effectiveAgeMs > aev.holdMs + aev.fadeMs &&
            !keepAliveByRecentLiveVisual) {
          continue;
        }
        // Status text lifetime/alpha authority is the event queue timing.
        // Live lane visuals provide geometry/typewriter anchors only.
        float baseAlpha = 1.0f;
        float lifeAlpha = 1.0f;
        if (effectiveAgeMs > aev.holdMs && aev.fadeMs > 0) {
          lifeAlpha =
              1.0f - (float)(effectiveAgeMs - aev.holdMs) / (float)aev.fadeMs;
        }
        lifeAlpha = Clamp01(lifeAlpha);
        float alpha = Clamp01(baseAlpha * lifeAlpha);
        alpha = Clamp01(alpha *
                        StatusQuickPulseAlphaScale(ev.key, ev.sequence, renderNow));
        alpha = Clamp01(alpha) * statusGateAlpha;
        uint32_t colorPacked = hasVisual ? vis.colorPacked : 0xFFFFFFFFu;
        float color[4] = {
            (float)((colorPacked >> 0) & 0xFF) / 255.0f,
            (float)((colorPacked >> 8) & 0xFF) / 255.0f,
            (float)((colorPacked >> 16) & 0xFF) / 255.0f,
            alpha};

        if (hasVisual && vis.fromLive && vis.fxLetterTime > 0) {
          const size_t totalChars = CountUtf8Chars(statusText);
          int rawVisibleChars = (int)totalChars;
          DWORD elapsed = 0;
          const DWORD fxBirth =
              (vis.fxBirthTime > 0) ? (DWORD)vis.fxBirthTime : 0;
          if (fxBirth != 0 && gameplayClockNow > fxBirth) {
            elapsed = gameplayClockNow - fxBirth;
          }
          rawVisibleChars = (int)(elapsed / (DWORD)vis.fxLetterTime);
          if (IsObjectiveStatusMessageKey(ev.key)) {
            // Keep status typewriter, but avoid initial blank when gameplay
            // clock and event queue clock are briefly out of sync.
            const int queueVisibleChars =
                (int)(effectiveAgeMs / (DWORD)vis.fxLetterTime);
            if (queueVisibleChars > rawVisibleChars) {
              rawVisibleChars = queueVisibleChars;
            }
            if (rawVisibleChars <= 0 && effectiveAgeMs >= 120) {
              rawVisibleChars = 1;
            }
          }
          if (rawVisibleChars <= 0) {
            renderStatusText.clear();
          } else if ((size_t)rawVisibleChars < totalChars) {
            renderStatusText =
                Utf8PrefixByChars(statusText, (size_t)rawVisibleChars);
          }
        }

        if (alpha <= 0.01f || renderStatusText.empty()) {
          if (ev.key == "EXE_GAMESAVED" || ev.key == "CGAME_NOW_SAVING") {
            static DWORD s_saveSkipLogTick = 0;
            if ((now - s_saveSkipLogTick) > 600) {
              s_saveSkipLogTick = now;
              char dbuf[256];
              sprintf_s(dbuf,
                        "[STATUS-EVT-SKIP-RENDER] key=%s seq=%u alpha=%.3f "
                        "textEmpty=%d age=%lu",
                        ev.key.c_str(), ev.sequence, alpha,
                        renderStatusText.empty() ? 1 : 0,
                        (unsigned long)effectiveAgeMs);
              LogToFile(dbuf);
            }
          }
          continue;
        }
        const float width =
            KoreanRenderer::MeasureTextWidthEx(renderStatusText, statusFontH,
                                               statusScale);
        KoreanRenderer::QueueText(
            renderStatusText, statusX, statusY, statusScale, color, statusFontH,
            0, width, false, false, 1.5f, false, false, true, 0,
            screenWidth * 0.86f, true, true);

        const float ageSec = (float)effectiveAgeMs / 1000.0f;
        std::string logKey =
            ev.key + "|" + std::to_string(ev.sequence) + "|" +
            std::to_string((int)statusY);
        auto itLog = s_statusRenderLogTick.find(logKey);
        if (itLog == s_statusRenderLogTick.end() ||
            (now - itLog->second) > 800) {
          s_statusRenderLogTick[logKey] = now;
          char sbuf[640];
          sprintf_s(
              sbuf,
              "[STATUS-MSG-RENDER] key=%s seq=%u age=%.2fs alpha=%.2f x=%.1f y=%.1f sx=%.2f fh=%.1f src=%s pos=%s vis=%s ecfg=0x%X vcfg=0x%X text=\"%.64s\"",
              ev.key.c_str(), ev.sequence, ageSec, alpha, statusX, statusY,
              statusScale, statusFontH, statusTextSrc, statusPosSrc,
              statusVisualSrc, ev.sourceCfg, hasVisual ? vis.cfgIndex : 0u,
              renderStatusText.c_str());
          LogToFile(sbuf);
        }
      }
    }
  }
#else
  (void)pSwapChain;
  (void)now;
  (void)suppressHudOverlay;
  (void)hudPauseMenu;
  (void)objectiveHiddenByGate;
#endif
}

// =========================================================================




