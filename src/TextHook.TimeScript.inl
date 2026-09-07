struct TimeScriptHudSnapshot {
  uintptr_t rbx = 0;
  float x = 0.0f;
  float y = 0.0f;
  float docX = 0.0f;
  float docY = 0.0f;
  float legacyX = 0.0f;
  float legacyY = 0.0f;
  bool usedLegacyXY = false;
  float fontScale = 0.0f;
  float timerValue = 0.0f;
  uint32_t horzAlign = 0;
  uint32_t vertAlign = 0;
  uint32_t colorPacked = 0;
  uintptr_t textPtr = 0;
};

struct TimeScriptHookContext {
  bool active = false;
  bool colorSuppressed = false;
  bool layoutReady = false;
  uintptr_t textPtr = 0;
  uint32_t originalColor = 0;
  unsigned int callerOffset = 0;
  std::string rawText;
  HudScriptCountdownMatch match;
  TimeScriptHudSnapshot snap;
  float renderX = 0.0f;
  float renderY = 0.0f;
  float renderScale = 1.0f;
  float renderFontHeight = 78.0f;
};

struct TimeScriptFallbackScanRange {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  bool initialized = false;
};

struct TimeScriptIncrementalScanState {
  uintptr_t cursor = 0;
  DWORD lastScanTick = 0;
};

struct TimeScriptIarCandidateState {
  std::string key;
  std::string rawText;
  std::string sourceTag;
  uintptr_t stringAddr = 0;
  uintptr_t rootBase = 0;
  int score = 0;
  DWORD lastSeenTick = 0;
  DWORD lastLogTick = 0;
  bool active = false;
};

struct TimeScriptRuntimeState {
  std::mutex mutex;
  TimeScriptRenderSnapshot renderSnapshot;
  TimeScriptIarCandidateState iarCandidate;
  TimeScriptFallbackScanRange fallbackScanRange;
  uintptr_t fallbackCachedHudPtr = 0;
  uintptr_t fallbackCachedTextPtr = 0;
  TimeScriptIncrementalScanState fallbackTextScanState;
  TimeScriptIncrementalScanState fallbackHudScanState;
  DWORD directOwnerTick = 0;
  DWORD iarFallbackOwnerTick = 0;
  // Last known good position from IAR-DIRECT path (reused by FALLBACK
  // when NativeIntroRender coordinates are off-screen).
  float lastGoodX = 0;
  float lastGoodY = 0;
  float lastGoodScale = 0;
  float lastGoodFontHeight = 0;
  float lastGoodColor[4] = {0.8f, 1.0f, 0.8f, 1.0f};
  int   lastGoodForceAlign = 0;
  bool  lastGoodValid = false;
};

static thread_local TimeScriptHookContext g_TimeScriptHookContext;
static TimeScriptRuntimeState g_TimeScriptRuntimeState{};
static constexpr DWORD kTimeScriptSnapshotTtlMs = 500;

static bool TimeScript_ShouldInvalidateExpiryJumpLocked(
    const std::string &key, float proposedSeconds, const char *sourceTag) {
  if (!std::isfinite(proposedSeconds)) {
    return false;
  }

  const TimeScriptRenderSnapshot &cur = g_TimeScriptRuntimeState.renderSnapshot;
  if (!cur.active || cur.key.empty() || cur.key != key || !cur.timerComputed ||
      !std::isfinite(cur.timerSeconds)) {
    return false;
  }

  if (cur.timerSeconds > 3.5f) {
    return false;
  }
  if (proposedSeconds <= (cur.timerSeconds + 20.0f) || proposedSeconds < 60.0f) {
    return false;
  }

  char jbuf[384];
  sprintf_s(jbuf,
            "[TIMESCRIPT-EXPIRY-JUMP-SKIP] src=%s key=%s prev=%.1f next=%.1f",
            sourceTag ? sourceTag : "unknown", key.c_str(), cur.timerSeconds,
            proposedSeconds);
  LogToFile(jbuf);

  g_TimeScriptRuntimeState.renderSnapshot.active = false;
  g_TimeScriptRuntimeState.directOwnerTick = 0;
  g_TimeScriptRuntimeState.iarFallbackOwnerTick = 0;
  g_TimeScriptPersistSuppress.active = false;
  return true;
}

// TimeScriptPersistentSuppress and g_TimeScriptPersistSuppress are forward-
// declared in texthook.cpp (before TextHook.IntroRendering.inl) so that both
// IntroRendering.inl and this file can access them.

static void TimeScript_ResetRuntimeStateLocked() {
  g_TimeScriptRuntimeState.renderSnapshot = TimeScriptRenderSnapshot{};
  g_TimeScriptRuntimeState.iarCandidate = TimeScriptIarCandidateState{};
  g_TimeScriptRuntimeState.fallbackScanRange = TimeScriptFallbackScanRange{};
  g_TimeScriptRuntimeState.fallbackCachedHudPtr = 0;
  g_TimeScriptRuntimeState.fallbackCachedTextPtr = 0;
  g_TimeScriptRuntimeState.fallbackTextScanState =
      TimeScriptIncrementalScanState{};
  g_TimeScriptRuntimeState.fallbackHudScanState =
      TimeScriptIncrementalScanState{};
  g_TimeScriptRuntimeState.directOwnerTick = 0;
  g_TimeScriptRuntimeState.iarFallbackOwnerTick = 0;
}

// Restore persistent alpha suppression if the countdown element is no longer
// active.  Called at the start of each PreDrawHook frame and on explicit reset.
static void TimeScript_RestorePersistentSuppressIfStale() {
  if (!g_TimeScriptPersistSuppress.active) {
    return;
  }
  const DWORD now = GetTickCount();
  // If no suppression refresh for 500ms, the countdown has ended.
  // Since CG_DrawHudElem hook handles visual suppression (alpha=0) and
  // we no longer modify HudElem fields directly, no restoration is needed.
  // Just deactivate — CG_UpdateHudElem naturally restores fields next frame.
  if ((now - g_TimeScriptPersistSuppress.lastSuppressTick) > 500) {
    static DWORD s_lastRestoreLog = 0;
    if ((now - s_lastRestoreLog) > 900) {
      s_lastRestoreLog = now;
      char buf[256];
      sprintf_s(buf,
                "[TIMESCRIPT-SUPPRESS-END] elemPtr=0x%llX age=%lu",
                (unsigned long long)g_TimeScriptPersistSuppress.elemPtr,
                (unsigned long)(now - g_TimeScriptPersistSuppress.lastSuppressTick));
      LogToFile(buf);
    }
    g_TimeScriptPersistSuppress = TimeScriptPersistentSuppress{};
  }
}

void TextHook_TimeScriptReset() {
  // No HudElem field restoration needed — CG_DrawHudElem alpha=0 is the
  // only modification, and CG_UpdateHudElem naturally restores each frame.
  if (g_TimeScriptPersistSuppress.active) {
    g_TimeScriptPersistSuppress = TimeScriptPersistentSuppress{};
  }
  {
    std::lock_guard<std::mutex> lock(g_TimeScriptRuntimeState.mutex);
    TimeScript_ResetRuntimeStateLocked();
  }
  g_TimeScriptHookContext = TimeScriptHookContext{};
}

static bool TimeScript_TryBuildIarFallbackSnapshot(
    int slcIdx, const std::string &key, const std::string &rawText,
    TimeScriptRenderSnapshot &outSnap) {
  outSnap = TimeScriptRenderSnapshot{};
  if (!IsTimeScriptManagedCountdownKeyName(key) || rawText.empty()) {
    return false;
  }

  EnsureTranslationsLoaded();
  auto itEng = g_KeyToEnglish.find(key);
  auto itKor = g_KeyToKorean.find(key);
  if (itEng == g_KeyToEnglish.end() || itKor == g_KeyToKorean.end() ||
      itEng->second.empty() || itKor->second.empty()) {
    return false;
  }

  const std::string rawNorm = ToUpperAscii(TrimSpaces(StripColorCodes(rawText)));
  const std::string engNorm =
      ToUpperAscii(TrimSpaces(StripColorCodes(itEng->second)));
  if (rawNorm.empty() || engNorm.empty() || rawNorm != engNorm) {
    return false;
  }

  NativeIntroRenderState nativeState{};
  if (slcIdx < 0 || !TextHook_GetNativeIntroRender(slcIdx, nativeState) ||
      !nativeState.valid) {
    return false;
  }

  IntroHudElemSnapshot snap{};
  snap.x = nativeState.x;
  snap.y = nativeState.y;
  snap.fontScale = nativeState.fontScale;
  snap.horzAlign = nativeState.horzAlign;
  snap.vertAlign = nativeState.vertAlign;

  if (!TryComputeIntroCountdownLayout(snap, outSnap.x, outSnap.y,
                                      outSnap.scale, outSnap.fontHeight)) {
    return false;
  }

  outSnap.key = key;
  outSnap.englishText = rawText;
  outSnap.renderText = itKor->second;
  outSnap.color[0] = (float)((nativeState.colorPacked >> 0) & 0xFF) / 255.0f;
  outSnap.color[1] = (float)((nativeState.colorPacked >> 8) & 0xFF) / 255.0f;
  outSnap.color[2] = (float)((nativeState.colorPacked >> 16) & 0xFF) / 255.0f;
  outSnap.color[3] = (float)((nativeState.colorPacked >> 24) & 0xFF) / 255.0f;
  if (outSnap.color[3] <= 0.01f) {
    outSnap.color[3] = 1.0f;
  }
  outSnap.forceAlign =
      (snap.horzAlign == 2) ? 1 : ((snap.horzAlign == 1) ? 2 : 0);
  outSnap.yIsTopOfText = true;
  outSnap.noTenths = (key == "SATFARM_TIME_IMACT");
  outSnap.lastSeenTick = GetTickCount();
  outSnap.active = true;

  const float engWidth = KoreanRenderer::MeasureTextWidthEx(
      rawText, outSnap.fontHeight, outSnap.scale, 1);
  const float korWidth = KoreanRenderer::MeasureTextWidthEx(
      outSnap.renderText, outSnap.fontHeight, outSnap.scale, 1);
  if (engWidth > 1.0f && korWidth > engWidth * 0.98f) {
    const float fit = (engWidth / korWidth) * 0.96f;
    if (std::isfinite(fit) && fit > 0.35f && fit < 1.0f) {
      outSnap.scale *= fit;
    }
  }

  return true;
}

// Forward declaration — defined further below in this file.
static bool TryComputeTimeScriptLayout(const TimeScriptHudSnapshot &snap,
                                       float &outX, float &outY,
                                       float &outScale, float &outFontHeight);

bool TextHook_TimeScriptOnIntroActualRenderCandidate(
    const char *key, const char *rawText, const char *sourceTag,
    uintptr_t stringAddr, uintptr_t rootBase, int score, int slcIdx) {
  const std::string keyStr = key ? key : "";
  if (!IsTimeScriptManagedCountdownKeyName(keyStr)) {
    return false;
  }

  const DWORD now = GetTickCount();
  std::lock_guard<std::mutex> lock(g_TimeScriptRuntimeState.mutex);
  TimeScriptIarCandidateState &cand = g_TimeScriptRuntimeState.iarCandidate;
  cand.key = keyStr;
  cand.rawText = rawText ? rawText : "";
  cand.sourceTag = sourceTag ? sourceTag : "";
  cand.stringAddr = stringAddr;
  cand.rootBase = rootBase;
  cand.score = score;
  cand.lastSeenTick = now;
  cand.active = true;

  if ((now - cand.lastLogTick) > 900) {
    cand.lastLogTick = now;
    char buf[512];
    sprintf_s(
        buf,
        "[TIMESCRIPT-IAR-CAND] key=%s score=%d src=%s str=0x%llX root=0x%llX raw=\"%.96s\"",
        cand.key.c_str(), cand.score, cand.sourceTag.c_str(),
        (unsigned long long)cand.stringAddr, (unsigned long long)cand.rootBase,
        Utf8Preview(cand.rawText, 96).c_str());
    LogToFile(buf);
  }

  // If directOwner snapshot exists and is fresh, refresh its TTL, update
  // timer value from HudElem, and re-suppress.
  const bool hasFreshSnapshot =
      (g_TimeScriptRuntimeState.directOwnerTick != 0 &&
       (now - g_TimeScriptRuntimeState.directOwnerTick) <=
           kTimeScriptSnapshotTtlMs);
  const bool hasStaleSnapshot =
      (!hasFreshSnapshot &&
       g_TimeScriptRuntimeState.directOwnerTick != 0 &&
       g_TimeScriptRuntimeState.renderSnapshot.active &&
       !g_TimeScriptRuntimeState.renderSnapshot.key.empty());
  if (hasFreshSnapshot || hasStaleSnapshot) {
    // Stale-timer detection: if timerSeconds hasn't changed for >2s,
    // the cached elemPtr is likely stale (map transition moved the timer
    // to a new HudElem).  Invalidate the snapshot so IAR-DIRECT re-scans.
    {
      static float s_prevTimer = -1.0f;
      static DWORD s_timerChangeTick = 0;
      float curTimer =
          g_TimeScriptRuntimeState.renderSnapshot.timerSeconds;
      if (std::abs(curTimer - s_prevTimer) > 0.05f) {
        s_prevTimer = curTimer;
        s_timerChangeTick = now;
      } else if (s_timerChangeTick != 0 &&
                 (now - s_timerChangeTick) > 2000 && curTimer > 1.0f) {
        // Timer frozen for >2s while supposedly active → force re-scan.
        g_TimeScriptRuntimeState.renderSnapshot.active = false;
        g_TimeScriptRuntimeState.directOwnerTick = 0;
        g_TimeScriptPersistSuppress.active = false;
        s_prevTimer = -1.0f;
        s_timerChangeTick = 0;
        static DWORD s_lastStaleLog = 0;
        if ((now - s_lastStaleLog) > 2000) {
          s_lastStaleLog = now;
          char _sb[256];
          sprintf_s(_sb,
                    "[TIMESCRIPT-STALE-INVALIDATE] timer=%.1f frozen "
                    "for >2s, forcing re-scan",
                    curTimer);
          LogToFile(_sb);
        }
        // Don't return false — fall through to IAR-DIRECT path below.
        goto iar_direct_rescan;
      }
    }

    g_TimeScriptRuntimeState.renderSnapshot.lastSeenTick = now;
    g_TimeScriptRuntimeState.directOwnerTick = now;

    // Update timer from HudElem every frame.
    // Visual suppression is handled by CG_DrawHudElem hook (alpha=0).
    // We do NOT modify any HudElem fields here — just read time for
    // timer computation.  This preserves typewriter sounds.
    if (g_TimeScriptPersistSuppress.active &&
        g_TimeScriptPersistSuppress.elemPtr != 0) {
      const uintptr_t ep = g_TimeScriptPersistSuppress.elemPtr;

      // Read time field (don't zero it — CG_DrawHudElem handles suppress)
      uint32_t timeFieldU = 0;
      TryReadIntroU32(ep, 0x78, timeFieldU);
      if ((int)timeFieldU > 30000) {
        g_TimeScriptPersistSuppress.cachedTimeField = timeFieldU;
      }
      int effTime = (int)timeFieldU;
      if (effTime <= 0 && g_TimeScriptPersistSuppress.cachedTimeField > 0) {
        effTime = (int)g_TimeScriptPersistSuppress.cachedTimeField;
      }

      // Update timer seconds using inline game time probe
      int gameTime = TextHook_ReadSPGameTime();
      if (gameTime <= 0 && effTime > 30000) {
        static uintptr_t s_ttlGTAddr = 0;
        static bool s_ttlGTValid = false;
        if (s_ttlGTValid && s_ttlGTAddr != 0) {
          uint32_t rv = 0;
          if (TryReadIntroU32(s_ttlGTAddr, 0, rv) &&
              (int)rv > 1000 && (int)rv < 10000000) {
            int rem = effTime - (int)rv;
            if (rem >= 0 && rem <= 700000) {
              gameTime = (int)rv;
            }
          }
        }
        if (gameTime <= 0) {
          static const uintptr_t s_modBase = (uintptr_t)GetModuleHandleA(NULL);
          const uintptr_t kGTOffsets[] = {
              IW6Offsets::Profile::Rva_1473528, IW6Offsets::Profile::Rva_147352C, IW6Offsets::Profile::Rva_1473520, IW6Offsets::Profile::Rva_1473524,
              IW6Offsets::Profile::Rva_147ED00, IW6Offsets::Profile::Rva_147ED04, IW6Offsets::Profile::Rva_147ED08,
          };
          for (size_t ci = 0;
               ci < sizeof(kGTOffsets) / sizeof(kGTOffsets[0]); ++ci) {
            uintptr_t addr = s_modBase + kGTOffsets[ci];
            uint32_t rv = 0;
            if (TryReadIntroU32(addr, 0, rv) && (int)rv > 1000 &&
                (int)rv < 10000000) {
              int rem = effTime - (int)rv;
              if (rem >= 0 && rem <= 700000) {
                s_ttlGTAddr = addr;
                s_ttlGTValid = true;
                gameTime = (int)rv;
                break;
              }
            }
          }
        }
      }
      if (gameTime > 0 && effTime > 0) {
        const float proposedSeconds = (float)(std::max)(0, effTime - gameTime) / 1000.0f;
        if (!TimeScript_ShouldInvalidateExpiryJumpLocked(
                g_TimeScriptRuntimeState.renderSnapshot.key, proposedSeconds,
                "ttl")) {
          g_TimeScriptRuntimeState.renderSnapshot.timerSeconds =
              proposedSeconds;
        }
        g_TimeScriptRuntimeState.renderSnapshot.timerComputed = true;
      }
      g_TimeScriptPersistSuppress.lastSuppressTick = now;
    }

    static DWORD s_lastHeartbeatLog = 0;
    if ((now - s_lastHeartbeatLog) > 900) {
      s_lastHeartbeatLog = now;
      char hbuf[384];
      sprintf_s(hbuf,
                "[TIMESCRIPT-IAR-TTL] key=%s timer=%.1f fresh=%d",
                g_TimeScriptRuntimeState.renderSnapshot.key.c_str(),
                g_TimeScriptRuntimeState.renderSnapshot.timerSeconds,
                hasFreshSnapshot ? 1 : 0);
      LogToFile(hbuf);
    }
    return false;
  }

iar_direct_rescan:
  TimeScriptRenderSnapshot fallbackSnap{};
  if (!TimeScript_TryBuildIarFallbackSnapshot(slcIdx, keyStr, cand.rawText,
                                              fallbackSnap)) {
    static DWORD s_lastLayoutMissLog = 0;
    if ((now - s_lastLayoutMissLog) > 900) {
      s_lastLayoutMissLog = now;
      char lbuf[384];
      sprintf_s(
          lbuf,
          "[TIMESCRIPT-IAR-LAYOUT-MISS] key=%s slc=0x%X score=%d src=%s raw=\"%.96s\"",
          keyStr.c_str(), (unsigned int)(slcIdx >= 0 ? slcIdx : 0), score,
          cand.sourceTag.c_str(), Utf8Preview(cand.rawText, 96).c_str());
      LogToFile(lbuf);
    }

    // --- IAR-direct: create snapshot using known HudElem layout ---
    // The NativeIntroRender table has no entry for countdown timers (IntroRenderFn
    // is not called for them).  When IAR detects "Powerdown in" / "Time to Exfil"
    // but no HudElem-scanner snapshot exists yet, use the observed HudElem layout
    // to create an immediate snapshot so Korean text appears without delay.
    //
    // Guard: rootBase must be within the HudElem array.  The IAR context
    // pointer (renderCtx) can contain stale localization key references from
    // other missions — e.g. CLOCKWORK_POWERDOWN appearing during SKYWAY.
    // Without this check the IAR-direct path would create a false snapshot
    // and write suppression fields to a non-HudElem structure.
    if (IsTimeScriptManagedCountdownKeyName(keyStr)) {
      const uintptr_t modBase = (uintptr_t)GetModuleHandleA(NULL);
      const uintptr_t hudArrayStart =
          modBase + IW6Offsets::HudElem_Array_SP;
      const uintptr_t hudArrayEnd =
          hudArrayStart +
          (uintptr_t)2048 * IW6Offsets::HudElem_Array_Stride_SP;

      // Resolve the actual HudElem pointer.  If rootBase from IAR is already
      // inside the HudElem array, use it directly.  Otherwise scan the array
      // for an element whose label field (+0x40) matches the expected SLC.
      // SATFARM_TIME_IMACT's IAR rootBase points to a render buffer (not a
      // HudElem), so this scan is necessary to find the real HudElem for
      // suppress + timer value reading.
      uintptr_t elemPtr = 0;
      if (rootBase >= hudArrayStart && rootBase < hudArrayEnd) {
        elemPtr = rootBase;
      } else {
        // Determine target SLC for this key
        uint32_t targetLabel = 0;
        if (keyStr == "CLOCKWORK_POWERDOWN") targetLabel = 0x23;
        else if (keyStr == "CLOCKWORK_EXFIL") targetLabel = 0x23;
        else if (keyStr == "SATFARM_TIME_IMACT") targetLabel = 0x3C;

        if (targetLabel != 0) {
          const int stride = IW6Offsets::HudElem_Array_Stride_SP;
          for (int i = 0; i < 2048; ++i) {
            uintptr_t e = hudArrayStart + (uintptr_t)i * (uintptr_t)stride;
            uint32_t label = 0;
            if (TryReadIntroU32(e, 0x40, label) && label == targetLabel) {
              elemPtr = e;
              break;
            }
          }
          // SATFARM_TIME_IMACT: cfg changes 0x3C → 0x3B after the map
          // transition (second section).  If primary scan found nothing,
          // try 0x3B without strict type guard — during cutscene
          // transitions, the timer type changes from TIMER_DOWN(5) to
          // TEXT(1) or TIMER_UP(4), so requiring type==5 would fail.
          // 0x3B is a high configstring index unique to SATFARM, so
          // false matches are unlikely.
          if (elemPtr == 0 && keyStr == "SATFARM_TIME_IMACT") {
            for (int i = 0; i < 2048; ++i) {
              uintptr_t e = hudArrayStart + (uintptr_t)i * (uintptr_t)stride;
              uint32_t label = 0;
              if (TryReadIntroU32(e, 0x40, label) && label == 0x3B) {
                elemPtr = e;
                break;
              }
            }
          }
        }
        static DWORD s_lastScanLog = 0;
        if ((now - s_lastScanLog) > 2000) {
          s_lastScanLog = now;
          char rbuf[384];
          sprintf_s(rbuf,
                    "[TIMESCRIPT-IAR-DIRECT-SCAN] key=%s rootBase=0x%llX "
                    "not in HudElem array, scanned → elemPtr=0x%llX",
                    keyStr.c_str(), (unsigned long long)rootBase,
                    (unsigned long long)elemPtr);
          LogToFile(rbuf);
        }
        // Fallback 1: try persist-suppress's cached elemPtr.
        if (elemPtr == 0 && g_TimeScriptPersistSuppress.elemPtr != 0) {
          uintptr_t cachedElem = g_TimeScriptPersistSuppress.elemPtr;
          if (cachedElem >= hudArrayStart && cachedElem < hudArrayEnd) {
            uint32_t cachedType = 0, cachedTime = 0;
            TryReadIntroU32(cachedElem, 0x00, cachedType);
            TryReadIntroU32(cachedElem, 0x78, cachedTime);
            if (IsHudElemTimerType(cachedType) && (int)cachedTime > 30000) {
              elemPtr = cachedElem;
            }
          }
        }
        // Fallback 2: broad scan for ANY timer-type HudElem with
        // time > 30000.  Handles cases where label changed AND
        // persist suppress expired (e.g., CLOCKWORK_EXFIL after
        // POWERDOWN ends, or mid-mission continue).
        if (elemPtr == 0) {
          const int stride = IW6Offsets::HudElem_Array_Stride_SP;
          uint32_t bestTime = 0;
          for (int i = 0; i < 2048; ++i) {
            uintptr_t e = hudArrayStart + (uintptr_t)i * (uintptr_t)stride;
            uint32_t eType = 0, eTime = 0;
            if (!TryReadIntroU32(e, 0x00, eType)) continue;
            if (!IsHudElemTimerType(eType)) continue;
            if (!TryReadIntroU32(e, 0x78, eTime)) continue;
            if ((int)eTime > 30000 && eTime > bestTime) {
              bestTime = eTime;
              elemPtr = e;
            }
          }
        }
        if (elemPtr == 0) {
          return false;
        }
      }
      EnsureTranslationsLoaded();
      auto itKor = g_KeyToKorean.find(keyStr);
      if (itKor != g_KeyToKorean.end() && !itKor->second.empty()) {
        // Observed HudElem layout for countdown timers:
        //   x=-250.0, y=100.0, fontScale=1.60, alignScreen=0x31
        //   horiz=3(RIGHT) → mapped=2, vert=1(TOP) → mapped=0
        //   color=0xFFCCFFCC
        TimeScriptHudSnapshot snap{};
        snap.x = -250.0f;
        snap.y = 100.0f;
        snap.fontScale = 1.60f;
        snap.horzAlign = 2;  // RIGHT
        snap.vertAlign = 0;  // TOP

        float outX = 0, outY = 0, outScale = 1.0f, outFontHeight = 48.0f;
        if (TryComputeTimeScriptLayout(snap, outX, outY, outScale,
                                        outFontHeight)) {
          TimeScriptRenderSnapshot renderSnap{};
          renderSnap.key = keyStr;
          renderSnap.englishText = cand.rawText;
          renderSnap.renderText = itKor->second;
          renderSnap.color[0] = 0.8f;   // 0xCC / 255
          renderSnap.color[1] = 1.0f;   // 0xFF / 255
          renderSnap.color[2] = 0.8f;   // 0xCC / 255
          renderSnap.color[3] = 1.0f;   // 0xFF / 255
          renderSnap.x = outX;
          renderSnap.y = outY;
          renderSnap.scale = outScale;
          renderSnap.fontHeight = outFontHeight;
          renderSnap.forceAlign = 1;     // RIGHT-aligned text
          renderSnap.yIsTopOfText = true;
          renderSnap.noTenths = (keyStr == "SATFARM_TIME_IMACT");
          renderSnap.lastSeenTick = now;
          renderSnap.active = true;

          // Read HudElem fields before suppress.  We suppress with
          // fontScale=0, alpha=0, time=0 but keep type intact.  Zeroing
          // type causes the game to reclaim the HudElem slot (label
          // cleared), making subsequent label-based scans fail.
          uint32_t iarTimeFieldU = 0;
          uint32_t iarType = 0, iarColor = 0, iarFsBits = 0;
          TryReadIntroU32(elemPtr, 0x78, iarTimeFieldU);
          TryReadIntroU32(elemPtr, 0x00, iarType);
          TryReadIntroU32(elemPtr, 0x30, iarColor);
          TryReadIntroU32(elemPtr, 0x14, iarFsBits);

          // Cache time field (only overwrite with real non-zero values)
          if ((int)iarTimeFieldU > 30000) {
            g_TimeScriptPersistSuppress.cachedTimeField = iarTimeFieldU;
          }
          int effectiveTimeField = (int)iarTimeFieldU;
          if (effectiveTimeField <= 0 &&
              g_TimeScriptPersistSuppress.cachedTimeField > 0) {
            effectiveTimeField = (int)g_TimeScriptPersistSuppress.cachedTimeField;
          }

          // Inline game time probe (same as StoreSnapshotFromHudElemTimer)
          {
            int gameTime = TextHook_ReadSPGameTime();

            if (gameTime <= 0 && effectiveTimeField > 30000) {
              static uintptr_t s_iarInlineGTAddr = 0;
              static bool s_iarInlineGTValid = false;
              if (s_iarInlineGTValid && s_iarInlineGTAddr != 0) {
                uint32_t rv = 0;
                if (TryReadIntroU32(s_iarInlineGTAddr, 0, rv) &&
                    (int)rv > 1000 && (int)rv < 10000000) {
                  int rem = effectiveTimeField - (int)rv;
                  if (rem >= 1000 && rem <= 700000) {
                    gameTime = (int)rv;
                  }
                }
              }
              if (gameTime <= 0) {
                const uintptr_t kGTOffsets[] = {
                    IW6Offsets::Profile::Rva_1473528, IW6Offsets::Profile::Rva_147352C, IW6Offsets::Profile::Rva_1473520, IW6Offsets::Profile::Rva_1473524,
                    IW6Offsets::Profile::Rva_147ED00, IW6Offsets::Profile::Rva_147ED04, IW6Offsets::Profile::Rva_147ED08,
                    IW6Offsets::Profile::Rva_1478000, IW6Offsets::Profile::Rva_1478004, IW6Offsets::Profile::Rva_1478008,
                    IW6Offsets::Profile::Rva_15B0000, IW6Offsets::Profile::Rva_15B0004, IW6Offsets::Profile::Rva_15B0008,
                    IW6Offsets::Profile::Rva_3C91500, IW6Offsets::Profile::Rva_3C91504, IW6Offsets::Profile::Rva_3C91508, IW6Offsets::Profile::Rva_3C914F8, IW6Offsets::Profile::Rva_3C914FC,
                    IW6Offsets::Profile::Rva_1550000, IW6Offsets::Profile::Rva_1550004, IW6Offsets::Profile::Rva_1560000,
                    IW6Offsets::Profile::Rva_43F4B60, IW6Offsets::Profile::Rva_43F4B64, IW6Offsets::Profile::Rva_43F4B68, IW6Offsets::Profile::Rva_43F4B70,
                };
                for (size_t ci = 0;
                     ci < sizeof(kGTOffsets) / sizeof(kGTOffsets[0]); ++ci) {
                  uintptr_t addr = modBase + kGTOffsets[ci];
                  uint32_t rv = 0;
                  if (TryReadIntroU32(addr, 0, rv) && (int)rv > 1000 &&
                      (int)rv < 10000000) {
                    int rem = effectiveTimeField - (int)rv;
                    if (rem >= 1000 && rem <= 700000) {
                      s_iarInlineGTAddr = addr;
                      s_iarInlineGTValid = true;
                      gameTime = (int)rv;
                      char gtbuf[256];
                      sprintf_s(gtbuf,
                          "[TIMESCRIPT-IAR-GAMETIME-INLINE] offset=+0x%llX "
                          "val=%d rem=%.1fs",
                          (unsigned long long)kGTOffsets[ci], (int)rv,
                          rem / 1000.0f);
                      LogToFile(gtbuf);
                      break;
                    }
                  }
                }
              }
            }

            if (gameTime > 0 && effectiveTimeField > 0) {
              renderSnap.timerSeconds =
                  (float)(std::max)(0, effectiveTimeField - gameTime) / 1000.0f;
              renderSnap.timerComputed = true;
            }
          }

          g_TimeScriptRuntimeState.renderSnapshot = std::move(renderSnap);
          g_TimeScriptRuntimeState.directOwnerTick = now;
          g_TimeScriptRuntimeState.iarFallbackOwnerTick = 0;

          // Save last known good position for FALLBACK reuse after
          // mission transitions (when NativeIntroRender has bad coords).
          {
            const auto &s = g_TimeScriptRuntimeState.renderSnapshot;
            g_TimeScriptRuntimeState.lastGoodX = s.x;
            g_TimeScriptRuntimeState.lastGoodY = s.y;
            g_TimeScriptRuntimeState.lastGoodScale = s.scale;
            g_TimeScriptRuntimeState.lastGoodFontHeight = s.fontHeight;
            memcpy(g_TimeScriptRuntimeState.lastGoodColor, s.color, sizeof(s.color));
            g_TimeScriptRuntimeState.lastGoodForceAlign = s.forceAlign;
            g_TimeScriptRuntimeState.lastGoodValid = true;
          }

          // Register for CG_DrawHudElem x=9999 suppression.
          // No HudElem field writes here — CG_DrawHudElem hook handles
          // visual suppression, preserving typewriter sounds.
          {
            // Cooldown: don't re-register a slot that CG_DrawHudElem
            // just deactivated due to slot reuse.
            const bool inCooldown =
                (g_TimeScriptPersistSuppress.deactivatedElemPtr == elemPtr &&
                 (now - g_TimeScriptPersistSuppress.deactivatedTick) < 5000);
            if (!inCooldown) {
              if (!g_TimeScriptPersistSuppress.active ||
                  g_TimeScriptPersistSuppress.elemPtr != elemPtr) {
                g_TimeScriptPersistSuppress.elemPtr = elemPtr;
                g_TimeScriptPersistSuppress.originalFontScaleBits = iarFsBits;
                g_TimeScriptPersistSuppress.originalType = iarType;
                g_TimeScriptPersistSuppress.originalColor = iarColor;
                // Cache label + text SLC for slot-reuse detection.
                uint32_t iarLabel = 0, iarTextSlc = 0;
                TryReadIntroU32(elemPtr, 0x40, iarLabel);
                TryReadIntroU32(elemPtr, 0x84, iarTextSlc);
                g_TimeScriptPersistSuppress.originalLabel = iarLabel;
                g_TimeScriptPersistSuppress.originalTextSlc = iarTextSlc;
              }
              g_TimeScriptPersistSuppress.rbx = elemPtr;
              g_TimeScriptPersistSuppress.lastSuppressTick = now;
              g_TimeScriptPersistSuppress.active = true;
            }
          }

          static DWORD s_lastDirectLog = 0;
          if ((now - s_lastDirectLog) > 900) {
            s_lastDirectLog = now;
            char dbuf[512];
            sprintf_s(
                dbuf,
                "[TIMESCRIPT-IAR-DIRECT] key=%s x=%.1f y=%.1f scale=%.3f "
                "fontH=%.1f elemPtr=0x%llX time=%d eff=%d timer=%.1f",
                keyStr.c_str(), outX, outY, outScale, outFontHeight,
                (unsigned long long)elemPtr, (int)iarTimeFieldU,
                effectiveTimeField,
                g_TimeScriptRuntimeState.renderSnapshot.timerSeconds);
            LogToFile(dbuf);
          }
          return true;
        }
      }
    }
    return false;
  }

  g_TimeScriptRuntimeState.renderSnapshot = std::move(fallbackSnap);
  g_TimeScriptRuntimeState.iarFallbackOwnerTick =
      g_TimeScriptRuntimeState.renderSnapshot.lastSeenTick;

  // If FALLBACK coordinates are off-screen, recover position.
  // Priority: (1) last known good from IAR-DIRECT, (2) hardcoded layout
  // matching the standard timescript HudElem properties.
  {
    auto &snap = g_TimeScriptRuntimeState.renderSnapshot;
    // Timescript renders at top-right (~x=1337, y=261 on 1920x1080).
    // Any position clearly outside the visible area is wrong.
    const bool posOffScreen =
        (snap.x < -100.0f || snap.x > 3000.0f ||
         snap.y < -100.0f || snap.y > 2000.0f);
    if (posOffScreen) {
      if (g_TimeScriptRuntimeState.lastGoodValid) {
        snap.x = g_TimeScriptRuntimeState.lastGoodX;
        snap.y = g_TimeScriptRuntimeState.lastGoodY;
        snap.scale = g_TimeScriptRuntimeState.lastGoodScale;
        snap.fontHeight = g_TimeScriptRuntimeState.lastGoodFontHeight;
        memcpy(snap.color, g_TimeScriptRuntimeState.lastGoodColor,
               sizeof(snap.color));
        snap.forceAlign = g_TimeScriptRuntimeState.lastGoodForceAlign;
      } else {
        // No prior direct path (e.g., mid-mission continue).  Use the
        // same hardcoded layout as IAR-DIRECT (observed HudElem coords).
        TimeScriptHudSnapshot hcSnap{};
        hcSnap.x = -250.0f;
        hcSnap.y = 100.0f;
        hcSnap.fontScale = 1.60f;
        hcSnap.horzAlign = 2;   // RIGHT
        hcSnap.vertAlign = 0;   // TOP
        float outX = 0, outY = 0, outScale = 1.0f, outFontH = 48.0f;
        if (TryComputeTimeScriptLayout(hcSnap, outX, outY, outScale, outFontH)) {
          snap.x = outX;
          snap.y = outY;
          snap.scale = outScale;
          snap.fontHeight = outFontH;
          snap.color[0] = 0.8f;
          snap.color[1] = 1.0f;
          snap.color[2] = 0.8f;
          snap.color[3] = 1.0f;
          snap.forceAlign = 1;  // RIGHT
        }
      }
    }
  }

  // Register persist suppress for the FALLBACK path too.  IAR skipOriginal
  // hides the label text, but x=9999 also hides any associated timer
  // rendering from the same CG_DrawHudElem call.
  {
    const uintptr_t canonicalElem = TextHook_GetCGDrawHudElemPtr();
    if (canonicalElem != 0) {
      if (!g_TimeScriptPersistSuppress.active ||
          g_TimeScriptPersistSuppress.elemPtr != canonicalElem) {
        uint32_t origType = 0;
        TryReadIntroU32(canonicalElem, 0x00, origType);
        g_TimeScriptPersistSuppress.elemPtr = canonicalElem;
        g_TimeScriptPersistSuppress.originalType = origType;
      }
      g_TimeScriptPersistSuppress.lastSuppressTick = now;
      g_TimeScriptPersistSuppress.active = true;
    }
  }

  static DWORD s_lastFallbackAcceptLog = 0;
  if ((now - s_lastFallbackAcceptLog) > 900) {
    s_lastFallbackAcceptLog = now;
    char abuf[512];
    sprintf_s(
        abuf,
        "[TIMESCRIPT-IAR-FALLBACK] key=%s slc=0x%X x=%.1f y=%.1f scale=%.3f fontH=%.1f raw=\"%.96s\"",
        keyStr.c_str(), (unsigned int)(slcIdx >= 0 ? slcIdx : 0),
        g_TimeScriptRuntimeState.renderSnapshot.x,
        g_TimeScriptRuntimeState.renderSnapshot.y,
        g_TimeScriptRuntimeState.renderSnapshot.scale,
        g_TimeScriptRuntimeState.renderSnapshot.fontHeight,
        Utf8Preview(cand.rawText, 96).c_str());
    LogToFile(abuf);
  }
  return true;
}

bool TextHook_GetTimeScriptRenderSnapshot(TimeScriptRenderSnapshot &outState) {
  outState = TimeScriptRenderSnapshot{};

  std::lock_guard<std::mutex> lock(g_TimeScriptRuntimeState.mutex);
  const TimeScriptRenderSnapshot &snap = g_TimeScriptRuntimeState.renderSnapshot;
  if (!snap.active || snap.renderText.empty()) {
    return false;
  }

  const DWORD now = GetTickCount();
  if (snap.lastSeenTick == 0 || (now - snap.lastSeenTick) > kTimeScriptSnapshotTtlMs) {
    return false;
  }
  const bool directOwnerLive =
      (g_TimeScriptRuntimeState.directOwnerTick != 0 &&
       (now - g_TimeScriptRuntimeState.directOwnerTick) <=
           kTimeScriptSnapshotTtlMs);
  const bool iarOwnerLive =
      (g_TimeScriptRuntimeState.iarFallbackOwnerTick != 0 &&
       (now - g_TimeScriptRuntimeState.iarFallbackOwnerTick) <=
           kTimeScriptSnapshotTtlMs);
  if (!directOwnerLive && !iarOwnerLive) {
    return false;
  }
  if (!std::isfinite(snap.x) || !std::isfinite(snap.y) ||
      !std::isfinite(snap.scale) || !std::isfinite(snap.fontHeight) ||
      snap.color[3] <= 0.01f) {
    return false;
  }

  outState = snap;
  return true;
}

bool TextHook_HasTimeScriptTimerReplacement() {
  // Called for native HUD draws: avoid allocating/copying the text snapshot.
  std::lock_guard<std::mutex> lock(g_TimeScriptRuntimeState.mutex);
  const auto &snap = g_TimeScriptRuntimeState.renderSnapshot;
  const DWORD now = GetTickCount();
  const bool ownerLive =
      (g_TimeScriptRuntimeState.directOwnerTick != 0 &&
       now - g_TimeScriptRuntimeState.directOwnerTick <= kTimeScriptSnapshotTtlMs) ||
      (g_TimeScriptRuntimeState.iarFallbackOwnerTick != 0 &&
       now - g_TimeScriptRuntimeState.iarFallbackOwnerTick <= kTimeScriptSnapshotTtlMs);
  return ownerLive && snap.active && !snap.renderText.empty() &&
         snap.lastSeenTick != 0 && now - snap.lastSeenTick <= kTimeScriptSnapshotTtlMs &&
         snap.timerComputed && std::isfinite(snap.timerSeconds) &&
         snap.timerSeconds > 0.05f && std::isfinite(snap.x) &&
         std::isfinite(snap.y) && std::isfinite(snap.scale) &&
         std::isfinite(snap.fontHeight) && snap.color[3] > 0.01f;
}

// Scan HudElem array for a countdown timer (time > 30000) when the FALLBACK
// snapshot is active but timerComputed == false.  Updates the snapshot's
// timerSeconds and caches the time field for companion suppress.
void TextHook_TimeScriptDiscoverFallbackTimer() {
  std::lock_guard<std::mutex> lock(g_TimeScriptRuntimeState.mutex);
  auto &snap = g_TimeScriptRuntimeState.renderSnapshot;
  if (!snap.active || snap.timerComputed) return;

  // Strategy 1: read time field (+0x78) directly from persist-suppress HudElem.
  // The HudElem can be TIMER_DOWN type (type=5) with a valid time field even
  // when OBJ-REVERSE-CAP shows a different TEXT-type HudElem with time=0.
  {
    const uintptr_t elem = g_TimeScriptPersistSuppress.elemPtr;
    if (elem != 0 && g_TimeScriptPersistSuppress.active) {
      uint32_t timeField = 0;
      if (TryReadIntroU32(elem, 0x78, timeField) && (int)timeField > 30000) {
        // Cache for companion suppress.
        g_TimeScriptPersistSuppress.cachedTimeField = timeField;

        // Try known game time, then inline probe.
        int gameTime = TextHook_ReadSPGameTime();
        if (gameTime <= 0) {
          static uintptr_t s_modBase = (uintptr_t)GetModuleHandleA(NULL);
          static const uintptr_t kGTOffsets[] = {
              IW6Offsets::Profile::Rva_1473528, IW6Offsets::Profile::Rva_147352C, IW6Offsets::Profile::Rva_1473520, IW6Offsets::Profile::Rva_1473524,
              IW6Offsets::Profile::Rva_147ED00, IW6Offsets::Profile::Rva_147ED04, IW6Offsets::Profile::Rva_147ED08,
              IW6Offsets::Profile::Rva_1478000, IW6Offsets::Profile::Rva_1478004, IW6Offsets::Profile::Rva_1478008,
              IW6Offsets::Profile::Rva_15B0000, IW6Offsets::Profile::Rva_15B0004, IW6Offsets::Profile::Rva_15B0008,
              IW6Offsets::Profile::Rva_3C91500, IW6Offsets::Profile::Rva_3C91504, IW6Offsets::Profile::Rva_3C91508, IW6Offsets::Profile::Rva_3C914F8, IW6Offsets::Profile::Rva_3C914FC,
              IW6Offsets::Profile::Rva_1550000, IW6Offsets::Profile::Rva_1550004, IW6Offsets::Profile::Rva_1560000,
              IW6Offsets::Profile::Rva_43F4B60, IW6Offsets::Profile::Rva_43F4B64, IW6Offsets::Profile::Rva_43F4B68, IW6Offsets::Profile::Rva_43F4B70,
          };
          for (size_t ci = 0; ci < sizeof(kGTOffsets)/sizeof(kGTOffsets[0]); ++ci) {
            uintptr_t addr = s_modBase + kGTOffsets[ci];
            uint32_t rv = 0;
            if (TryReadIntroU32(addr, 0, rv) && (int)rv > 1000 &&
                (int)rv < 10000000) {
              int rem = (int)timeField - (int)rv;
              if (rem >= 1000 && rem <= 700000) {
                gameTime = (int)rv;
                break;
              }
            }
          }
        }
        if (gameTime > 0) {
          int remaining = (int)timeField - gameTime;
          if (remaining >= 1000 && remaining <= 700000) {
            snap.timerSeconds = (float)remaining / 1000.0f;
            snap.timerComputed = true;
            snap.lastSeenTick = GetTickCount();
            return;
          }
        }
      }
    }
  }

  // Strategy 2: scan all HudElems for time field (+0x78) > 30000.
  static uintptr_t s_modBase = (uintptr_t)GetModuleHandleA(NULL);
  const uintptr_t base = s_modBase + IW6Offsets::HudElem_Array_SP;
  const int stride = IW6Offsets::HudElem_Array_Stride_SP;

  uint32_t bestTimeField = 0;
  for (int i = 0; i < 2048; ++i) {
    uintptr_t elem = base + (uintptr_t)i * (uintptr_t)stride;
    uint32_t tf = 0;
    if (!TryReadIntroU32(elem, 0x78, tf)) continue;
    if ((int)tf > 30000 && tf > bestTimeField) bestTimeField = tf;
  }
  if (bestTimeField == 0) return;

  int gameTime = TextHook_ReadSPGameTime();
  if (gameTime <= 0) {
    static const uintptr_t kGTOffsets[] = {
        IW6Offsets::Profile::Rva_1473528, IW6Offsets::Profile::Rva_147352C, IW6Offsets::Profile::Rva_1473520, IW6Offsets::Profile::Rva_1473524,
        IW6Offsets::Profile::Rva_147ED00, IW6Offsets::Profile::Rva_147ED04, IW6Offsets::Profile::Rva_147ED08,
        IW6Offsets::Profile::Rva_1478000, IW6Offsets::Profile::Rva_1478004, IW6Offsets::Profile::Rva_1478008,
        IW6Offsets::Profile::Rva_15B0000, IW6Offsets::Profile::Rva_15B0004, IW6Offsets::Profile::Rva_15B0008,
        IW6Offsets::Profile::Rva_3C91500, IW6Offsets::Profile::Rva_3C91504, IW6Offsets::Profile::Rva_3C91508, IW6Offsets::Profile::Rva_3C914F8, IW6Offsets::Profile::Rva_3C914FC,
        IW6Offsets::Profile::Rva_1550000, IW6Offsets::Profile::Rva_1550004, IW6Offsets::Profile::Rva_1560000,
        IW6Offsets::Profile::Rva_43F4B60, IW6Offsets::Profile::Rva_43F4B64, IW6Offsets::Profile::Rva_43F4B68, IW6Offsets::Profile::Rva_43F4B70,
    };
    for (size_t ci = 0; ci < sizeof(kGTOffsets) / sizeof(kGTOffsets[0]); ++ci) {
      uintptr_t addr = s_modBase + kGTOffsets[ci];
      uint32_t rv = 0;
      if (TryReadIntroU32(addr, 0, rv) && (int)rv > 1000 &&
          (int)rv < 10000000) {
        int rem = (int)bestTimeField - (int)rv;
        if (rem >= 1000 && rem <= 700000) {
          gameTime = (int)rv;
          break;
        }
      }
    }
  }
  if (gameTime <= 0) return;

  int remaining = (int)bestTimeField - gameTime;
  if (remaining >= 1000 && remaining <= 700000) {
    snap.timerSeconds = (float)remaining / 1000.0f;
    snap.timerComputed = true;
    snap.lastSeenTick = GetTickCount();
    g_TimeScriptPersistSuppress.cachedTimeField = bestTimeField;
  }
}

// Called from R_AddCmdDrawText when a standalone timer tail (e.g., "5:33")
// is rendered inside a CG_DrawHudElem with active persist suppress.
// Parses the timer text and updates the FALLBACK snapshot's timerSeconds
// so the D3D11 overlay can display the correct countdown.
void TextHook_TimeScriptCaptureRenderedTimer(const std::string &timerTail,
                                           const std::string &expectedKey) {
  // Parse "M:SS" or "M:SS.T" format
  int mins = 0, secs = 0, tenths = 0;
  bool hasTenths = false;
  const char *p = timerTail.c_str();
  while (*p && std::isdigit((unsigned char)*p)) {
    mins = mins * 10 + (*p - '0');
    p++;
  }
  if (*p == ':') {
    p++;
    while (*p && std::isdigit((unsigned char)*p)) {
      secs = secs * 10 + (*p - '0');
      p++;
    }
    if (*p == '.') {
      p++;
      if (std::isdigit((unsigned char)*p)) {
        tenths = *p - '0';
        hasTenths = true;
      }
    }
  }
  float totalSeconds = (float)(mins * 60 + secs) + (hasTenths ? tenths / 10.0f : 0.0f);
  if (totalSeconds < 0.1f || totalSeconds > 36000.0f) return;

  std::lock_guard<std::mutex> lock(g_TimeScriptRuntimeState.mutex);
  TimeScriptRenderSnapshot &snap = g_TimeScriptRuntimeState.renderSnapshot;
  if (!snap.active || snap.key != expectedKey || expectedKey.empty()) return;
  // Every native draw is authoritative, including when an earlier frame
  // already populated the snapshot. A one-shot capture freezes the number.
  snap.timerSeconds = totalSeconds;
  snap.timerComputed = true;
  snap.lastSeenTick = GetTickCount();
}

static bool TryReadTimeScriptF32(uintptr_t base, uintptr_t off, float &out) {
  if (!(base > 0x10000 && base < 0x7FFFFFFFFFFF)) {
    return false;
  }
  if (IsBadReadPtr((void *)(base + off), sizeof(float)) != 0) {
    return false;
  }
  __try {
    out = *(volatile float *)(base + off);
    return std::isfinite(out) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool TryReadTimeScriptChar(uintptr_t addr, char &out) {
  if (!(addr > 0x10000 && addr < 0x7FFFFFFFFFFF)) {
    return false;
  }
  if (IsBadReadPtr((void *)addr, sizeof(char)) != 0) {
    return false;
  }
  __try {
    out = *(volatile char *)addr;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool TryWriteTimeScriptChar(uintptr_t addr, char value) {
  if (!(addr > 0x10000 && addr < 0x7FFFFFFFFFFF)) {
    return false;
  }
  if (IsBadWritePtr((void *)addr, sizeof(char)) != 0) {
    return false;
  }
  __try {
    *(volatile char *)addr = value;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool IsPlausibleTimeScriptCoord(float x, float y) {
  return std::isfinite(x) && std::isfinite(y) && x > -2048.0f &&
         x < 2048.0f && y > -1024.0f && y < 1024.0f;
}

static bool TryReadTimeScriptHudSnapshot(uintptr_t rbx,
                                         TimeScriptHudSnapshot &outSnap) {
  outSnap = {};
  if (!(rbx > 0x10000 && rbx < 0x7FFFFFFFFFFF)) {
    return false;
  }
  if (IsBadReadPtr((void *)rbx, 0x60) != 0) {
    return false;
  }

  float docX = 0.0f;
  float docY = 0.0f;
  float legacyX = 0.0f;
  float legacyY = 0.0f;
  float fontScale = 0.0f;
  float timerValue = 0.0f;
  uint32_t horzAlign = 0;
  uint32_t vertAlign = 0;
  uint32_t colorPacked = 0;
  uintptr_t textPtr = 0;

  const bool haveDocXY =
      TryReadTimeScriptF32(rbx, 0x0C, docX) && TryReadTimeScriptF32(rbx, 0x10, docY);
  const bool haveLegacyXY =
      TryReadTimeScriptF32(rbx, 0x04, legacyX) &&
      TryReadTimeScriptF32(rbx, 0x08, legacyY);
  if (!TryReadTimeScriptF32(rbx, 0x14, fontScale) ||
      !TryReadTimeScriptF32(rbx, 0x1C, timerValue) ||
      !TryReadIntroU32(rbx, 0x24, horzAlign) ||
      !TryReadIntroU32(rbx, 0x28, vertAlign) ||
      !TryReadIntroU32(rbx, 0x30, colorPacked) ||
      !TryReadIntroU64(rbx, 0x50, textPtr)) {
    return false;
  }

  if (!(textPtr > 0x10000 && textPtr < 0x7FFFFFFFFFFF)) {
    return false;
  }

  outSnap.rbx = rbx;
  outSnap.docX = docX;
  outSnap.docY = docY;
  outSnap.legacyX = legacyX;
  outSnap.legacyY = legacyY;
  outSnap.fontScale = fontScale;
  outSnap.timerValue = timerValue;
  outSnap.horzAlign = horzAlign;
  outSnap.vertAlign = vertAlign;
  outSnap.colorPacked = colorPacked;
  outSnap.textPtr = textPtr;

  if (haveDocXY && IsPlausibleTimeScriptCoord(docX, docY)) {
    outSnap.x = docX;
    outSnap.y = docY;
    outSnap.usedLegacyXY = false;
    return true;
  }
  if (haveLegacyXY && IsPlausibleTimeScriptCoord(legacyX, legacyY)) {
    outSnap.x = legacyX;
    outSnap.y = legacyY;
    outSnap.usedLegacyXY = true;
    return true;
  }
  return false;
}

static bool TryResolveTimeScriptCountdown(const TimeScriptHudSnapshot &snap,
                                          std::string &outRawText,
                                          HudScriptCountdownMatch &outMatch) {
  outRawText.clear();
  outMatch = {};

  if (!TryReadCStringPreview((const char *)snap.textPtr, 160, outRawText)) {
    return false;
  }

  if (TryResolveHudScriptCountdownText(outRawText, outMatch) &&
      IsTimeScriptManagedCountdownKeyName(outMatch.key)) {
    return true;
  }

  const std::string cleaned = ToUpperAscii(TrimSpaces(StripColorCodes(outRawText)));

  // Fallback: try to identify the key from partial English text content
  const char *fallbackKey = nullptr;
  if (cleaned.find("POWERDOWN") != std::string::npos) {
    fallbackKey = "CLOCKWORK_POWERDOWN";
  } else if (cleaned.find("EXFIL") != std::string::npos) {
    fallbackKey = "CLOCKWORK_EXFIL";
  } else if (cleaned.find("IMPACT") != std::string::npos) {
    fallbackKey = "SATFARM_TIME_IMACT";
  }
  if (!fallbackKey) {
    return false;
  }

  std::string tail;
  if (!FormatHudScriptCountdownTailFromSeconds(snap.timerValue, tail)) {
    return false;
  }

  return BuildHudScriptCountdownMatchForKeyTail(fallbackKey, tail, outMatch);
}

static bool TryComputeTimeScriptLayout(const TimeScriptHudSnapshot &snap,
                                       float &outX, float &outY,
                                       float &outScale,
                                       float &outFontHeight) {
  outX = 0.0f;
  outY = 0.0f;
  outScale = 1.0f;
  outFontHeight = 48.0f;

  const float screenWidth = GetScreenWidthApprox();
  const float screenHeight = GetScreenHeightApprox();
  if (!(screenWidth > 1.0f && screenHeight > 1.0f)) {
    return false;
  }

  const float scaleF = screenHeight / 480.0f;
  EngineSafeArea sa = TextHook_GetEngineSafeArea();
  const float safeAreaFrac = 0.035f;

  float safeOffX = sa.x;
  float safeOffY = (sa.y > 1.0f && sa.y < screenHeight * 0.5f) ? sa.y : sa.x;
  if (!(safeOffX > 0.0f && safeOffX < screenWidth * 0.5f)) {
    safeOffX = screenWidth * safeAreaFrac;
  }
  if (!(safeOffY > 0.0f && safeOffY < screenHeight * 0.5f)) {
    safeOffY = screenHeight * safeAreaFrac;
  }

  switch (snap.horzAlign) {
    case 1:
      outX = screenWidth * 0.5f + snap.x * scaleF;
      break;
    case 2:
      outX = (screenWidth - safeOffX) + snap.x * scaleF;
      break;
    default:
      outX = safeOffX + snap.x * scaleF;
      break;
  }

  switch (snap.vertAlign) {
    case 1:
      outY = screenHeight * 0.5f + snap.y * scaleF;
      break;
    case 2:
      outY = (screenHeight - safeOffY) + snap.y * scaleF;
      break;
    default:
      outY = safeOffY + snap.y * scaleF;
      break;
  }

  const bool clockworkPowerdownLane =
      (snap.horzAlign == 2 && snap.vertAlign == 0 && snap.x >= -340.0f &&
       snap.x <= -140.0f && snap.y >= 60.0f && snap.y <= 140.0f &&
       snap.fontScale >= 1.20f && snap.fontScale <= 2.10f);
  if (clockworkPowerdownLane) {
    outFontHeight = 72.0f;
    outScale = snap.fontScale * 1.10f;
    // Nudge Korean text down to align with engine's "Powerdown in" baseline.
    outY += 36.0f * (screenHeight / 1080.0f);
    // Nudge Korean text left to better align with English text position.
    outX -= 20.0f * (screenWidth / 1920.0f);
  } else {
    outScale = snap.fontScale * 1.10f;
  }
  return std::isfinite(outX) && std::isfinite(outY) &&
         std::isfinite(outScale) && outScale > 0.01f;
}

static void TimeScript_UpdateAuthoritativeRenderSnapshot(
    const TimeScriptHookContext &ctx) {
  TimeScriptRenderSnapshot snap{};
  snap.key = ctx.match.key;
  snap.englishText = ctx.rawText;
  snap.renderText = ctx.match.renderText;
  snap.x = ctx.renderX;
  snap.y = ctx.renderY;
  snap.scale = ctx.renderScale;
  snap.fontHeight = ctx.renderFontHeight;
  snap.color[0] = (float)((ctx.originalColor >> 0) & 0xFF) / 255.0f;
  snap.color[1] = (float)((ctx.originalColor >> 8) & 0xFF) / 255.0f;
  snap.color[2] = (float)((ctx.originalColor >> 16) & 0xFF) / 255.0f;
  snap.color[3] = (float)((ctx.originalColor >> 24) & 0xFF) / 255.0f;
  if (snap.color[3] <= 0.01f) {
    snap.color[3] = 1.0f;
  }
  snap.forceAlign =
      (ctx.snap.horzAlign == 2) ? 1 : ((ctx.snap.horzAlign == 1) ? 2 : 0);
  snap.yIsTopOfText = (ctx.snap.vertAlign == 0);
  snap.noTenths = (ctx.match.key == "SATFARM_TIME_IMACT");
  snap.timerSeconds = ctx.snap.timerValue;  // remaining countdown seconds from render struct +0x1C
  snap.lastSeenTick = GetTickCount();
  snap.active = true;

  std::lock_guard<std::mutex> lock(g_TimeScriptRuntimeState.mutex);
  g_TimeScriptRuntimeState.renderSnapshot = std::move(snap);
  g_TimeScriptRuntimeState.directOwnerTick =
      g_TimeScriptRuntimeState.renderSnapshot.lastSeenTick;
  g_TimeScriptRuntimeState.iarFallbackOwnerTick = 0;
}

// ---------------------------------------------------------------------------
// HudElem-scanner path: builds TimeScript snapshot from per-frame HudElem data
// Called from TextHook_ReadLiveHudElems when a timer-type element is detected
// with a label SLC resolving to CLOCKWORK_POWERDOWN or CLOCKWORK_EXFIL.
// ---------------------------------------------------------------------------
static void TimeScript_StoreSnapshotFromHudElemTimer(
    const std::string &key,
    float elemX, float elemY, float fontScale,
    uint32_t alignOrg, uint32_t alignScreen,
    uint32_t colorPacked,
    int timeField, int durationField, float valueField,
    uint32_t labelSlc, uint32_t textSlc, uintptr_t elemPtr) {
  const DWORD now = GetTickCount();

  EnsureTranslationsLoaded();
  auto itKor = g_KeyToKorean.find(key);
  auto itEng = g_KeyToEnglish.find(key);
  if (itKor == g_KeyToKorean.end() || itKor->second.empty()) {
    return;
  }

  // Map IW6 alignment to simplified horzAlign/vertAlign for layout.
  // alignOrg encodes element origin (text anchor point).
  // alignScreen encodes screen anchor: (horzScreenAlign << 4) | vertScreenAlign
  //   horiz: 0=SUBLEFT,1=LEFT,2=CENTER,3=RIGHT,4=FULL,5=NOSCALE,6=TO640,7=CENTER_SA
  //   vert:  0=SUBTOP,1=TOP,2=CENTER,3=BOTTOM,4=FULL,5=NOSCALE,6=TO480,7=CENTER_SA
  // Observed: countdown timer has alignScreen=0x31 → horiz=3(RIGHT), vert=1(TOP).
  const uint32_t horzScreenAlign = (alignScreen >> 4) & 0xF;
  const uint32_t vertScreenAlign = alignScreen & 0xF;
  // Map IW6 screen anchor to layout code: 0=LEFT, 1=CENTER, 2=RIGHT
  uint32_t mappedHorzAlign = 0;
  if (horzScreenAlign == 3) mappedHorzAlign = 2;      // RIGHT
  else if (horzScreenAlign == 2 || horzScreenAlign == 7) mappedHorzAlign = 1;  // CENTER
  uint32_t mappedVertAlign = 0;
  if (vertScreenAlign == 2 || vertScreenAlign == 7) mappedVertAlign = 1;      // CENTER
  else if (vertScreenAlign == 3) mappedVertAlign = 2;  // BOTTOM

  // When we zero fontScale for suppression, subsequent scanner frames read 0.
  // Use the saved original value so layout computation still works.
  float effectiveFontScale = fontScale;
  if (effectiveFontScale < 0.01f && g_TimeScriptPersistSuppress.active &&
      g_TimeScriptPersistSuppress.elemPtr == elemPtr &&
      g_TimeScriptPersistSuppress.originalFontScaleBits != 0) {
    memcpy(&effectiveFontScale, &g_TimeScriptPersistSuppress.originalFontScaleBits,
           sizeof(float));
  }

  // CG_DrawHudElem writes x=9999 for suppression.  If the scanner reads
  // the HudElem between the write and the engine's restore, elemX will be
  // 9999 and the computed outX will be off-screen (e.g. 24417).  Filter
  // these frames out: keep the previous snapshot's x/y instead.
  float effectiveX = elemX;
  float effectiveY = elemY;
  if (elemX > 5000.0f) {
    // Suppress-frame: use cached position from the active snapshot.
    std::lock_guard<std::mutex> lock(g_TimeScriptRuntimeState.mutex);
    const auto &prev = g_TimeScriptRuntimeState.renderSnapshot;
    if (prev.active && prev.key == key) {
      // Re-derive from the previous snapshot's output position — don't
      // feed 9999 into the layout math.  Just skip this snapshot update
      // entirely so the previous good position persists.
      return;
    }
  }

  TimeScriptHudSnapshot snap{};
  snap.x = effectiveX;
  snap.y = effectiveY;
  snap.fontScale = effectiveFontScale;
  snap.horzAlign = mappedHorzAlign;
  snap.vertAlign = mappedVertAlign;

  float outX = 0, outY = 0, outScale = 1.0f, outFontHeight = 48.0f;
  if (!TryComputeTimeScriptLayout(snap, outX, outY, outScale, outFontHeight)) {
    static DWORD s_lastLayoutFailLog = 0;
    if ((now - s_lastLayoutFailLog) > 900) {
      s_lastLayoutFailLog = now;
      char buf[384];
      sprintf_s(buf,
                "[HUDELEM-TIMER-LAYOUT-FAIL] key=%s x=%.1f y=%.1f fs=%.2f "
                "ao=%u as=%u mh=%u mv=%u",
                key.c_str(), elemX, elemY, fontScale, alignOrg, alignScreen,
                mappedHorzAlign, mappedVertAlign);
      LogToFile(buf);
    }
    return;
  }

  // Build render snapshot with Korean LABEL ONLY (no timer number).
  // The engine's own timer number rendering is left intact — it goes through
  // an unknown rendering path that we don't suppress.  The IAR hook
  // suppresses only the English label text, so the result is:
  //   Korean overlay: "전력 차단까지 "   (from D3D11)
  //   Engine render:  "1:07.7"          (from unknown path, unchanged)
  const std::string &korLabel = itKor->second;

  TimeScriptRenderSnapshot renderSnap{};
  renderSnap.key = key;
  renderSnap.englishText = itEng != g_KeyToEnglish.end() ? itEng->second : key;
  renderSnap.renderText = korLabel;
  renderSnap.x = outX;
  renderSnap.y = outY;
  renderSnap.scale = outScale;
  renderSnap.fontHeight = outFontHeight;
  renderSnap.color[0] = (float)((colorPacked >> 0) & 0xFF) / 255.0f;
  renderSnap.color[1] = (float)((colorPacked >> 8) & 0xFF) / 255.0f;
  renderSnap.color[2] = (float)((colorPacked >> 16) & 0xFF) / 255.0f;
  renderSnap.color[3] = (float)((colorPacked >> 24) & 0xFF) / 255.0f;
  if (renderSnap.color[3] <= 0.01f) {
    renderSnap.color[3] = 1.0f;
  }
  renderSnap.forceAlign =
      (mappedHorzAlign == 2) ? 1 : ((mappedHorzAlign == 1) ? 2 : 0);
  renderSnap.yIsTopOfText = true;
  renderSnap.noTenths = (key == "SATFARM_TIME_IMACT");
  // Compute timer from HudElem time field (+0x78 = absolute end time in
  // server ms) minus current SP game time.  valueField (+0x80) is NOT
  // remaining seconds for TIMER-type elements.
  //
  // NOTE: CG_DrawHudElem hook handles visual suppression (alpha=0).
  // We do NOT write to HudElem fields.  timeField may read 0 if the
  // engine hasn't updated it yet — fall back to cached value.
  int effectiveTimeField = timeField;
  if (effectiveTimeField <= 0 && g_TimeScriptPersistSuppress.active &&
      g_TimeScriptPersistSuppress.elemPtr == elemPtr &&
      g_TimeScriptPersistSuppress.cachedTimeField > 0) {
    effectiveTimeField = (int)g_TimeScriptPersistSuppress.cachedTimeField;
  }
  // Cache the time field (only overwrite with a real non-zero value)
  if (timeField > 30000 &&
      (g_TimeScriptPersistSuppress.elemPtr == elemPtr ||
       !g_TimeScriptPersistSuppress.active)) {
    g_TimeScriptPersistSuppress.cachedTimeField = (uint32_t)timeField;
  }
  {
    int gameTime = TextHook_ReadSPGameTime();

    // Inline game time probe: if TextHook_ReadSPGameTime() returns 0 (not yet
    // discovered by wide scan), try known candidate offsets directly.
    if (gameTime <= 0 && effectiveTimeField > 30000) {
      static uintptr_t s_inlineGTAddr = 0;
      static bool s_inlineGTValid = false;
      static uintptr_t s_gtModuleBase = 0;
      if (s_gtModuleBase == 0) {
        s_gtModuleBase = (uintptr_t)GetModuleHandleA(NULL);
      }
      if (s_inlineGTValid && s_inlineGTAddr != 0) {
        uint32_t rv = 0;
        if (TryReadIntroU32(s_inlineGTAddr, 0, rv) && (int)rv > 1000 &&
            (int)rv < 10000000) {
          int rem = effectiveTimeField - (int)rv;
          if (rem >= 1000 && rem <= 120000) {
            gameTime = (int)rv;
          }
        }
      }
      if (gameTime <= 0) {
        const uintptr_t kGTOffsets[] = {
            IW6Offsets::MPGlobals::serverTime,   // 0x647B280
            IW6Offsets::MPGlobals::cgArray,      // 0x176EC00
            IW6Offsets::Profile::Rva_176EC00 + 0x3388 + 0x3C7C,
            // Confirmed SP game time offset (from wide-scan discovery)
            IW6Offsets::Profile::Rva_1473528, IW6Offsets::Profile::Rva_147352C, IW6Offsets::Profile::Rva_1473520, IW6Offsets::Profile::Rva_1473524,
            IW6Offsets::Profile::Rva_147ED00, IW6Offsets::Profile::Rva_147ED04, IW6Offsets::Profile::Rva_147ED08,
            IW6Offsets::Profile::Rva_1478000, IW6Offsets::Profile::Rva_1478004, IW6Offsets::Profile::Rva_1478008,
            IW6Offsets::Profile::Rva_15B0000, IW6Offsets::Profile::Rva_15B0004, IW6Offsets::Profile::Rva_15B0008,
            IW6Offsets::Profile::Rva_3C91500, IW6Offsets::Profile::Rva_3C91504, IW6Offsets::Profile::Rva_3C91508, IW6Offsets::Profile::Rva_3C914F8, IW6Offsets::Profile::Rva_3C914FC,
            IW6Offsets::Profile::Rva_1550000, IW6Offsets::Profile::Rva_1550004, IW6Offsets::Profile::Rva_1560000,
            IW6Offsets::Profile::Rva_43F4B60, IW6Offsets::Profile::Rva_43F4B64, IW6Offsets::Profile::Rva_43F4B68, IW6Offsets::Profile::Rva_43F4B70,
        };
        for (size_t ci = 0;
             ci < sizeof(kGTOffsets) / sizeof(kGTOffsets[0]); ++ci) {
          uintptr_t addr = s_gtModuleBase + kGTOffsets[ci];
          uint32_t rv = 0;
          if (TryReadIntroU32(addr, 0, rv) && (int)rv > 1000 &&
              (int)rv < 10000000) {
            int rem = effectiveTimeField - (int)rv;
            if (rem >= 1000 && rem <= 120000) {
              s_inlineGTAddr = addr;
              s_inlineGTValid = true;
              gameTime = (int)rv;
              char gtbuf[256];
              sprintf_s(
                  gtbuf,
                  "[TIMESCRIPT-GAMETIME-INLINE] addr=0x%llX "
                  "offset=+0x%llX val=%d rem=%.1fs",
                  (unsigned long long)addr,
                  (unsigned long long)kGTOffsets[ci], (int)rv,
                  rem / 1000.0f);
              LogToFile(gtbuf);
              break;
            }
          }
        }
      }
    }

    if (gameTime > 0 && effectiveTimeField > 0) {
      const float proposedSeconds =
          (float)(std::max)(0, effectiveTimeField - gameTime) / 1000.0f;
      if (TimeScript_ShouldInvalidateExpiryJumpLocked(renderSnap.key,
                                                      proposedSeconds,
                                                      "scanner")) {
        return;
      }
      renderSnap.timerSeconds = proposedSeconds;
      renderSnap.timerComputed = true;
    } else if (valueField > 0.01f && valueField < 600.0f) {
      renderSnap.timerSeconds = valueField;  // fallback
    }
  }
  renderSnap.lastSeenTick = now;
  renderSnap.active = true;

  // Width-fit scaling removed: we now render the complete composite text
  // (Korean label + timer number) via D3D11 overlay, so matching the
  // English label width is no longer necessary.

  // Store snapshot for D3D11 overlay consumption
  {
    std::lock_guard<std::mutex> lock(g_TimeScriptRuntimeState.mutex);
    g_TimeScriptRuntimeState.renderSnapshot = std::move(renderSnap);
    g_TimeScriptRuntimeState.directOwnerTick = now;
    g_TimeScriptRuntimeState.iarFallbackOwnerTick = 0;
  }

  // Suppress engine rendering via CG_DrawHudElem x=9999 suppress.
  // No HudElem field writes — preserves typewriter sounds.
  {
    // Cooldown: if CG_DrawHudElem just deactivated this elemPtr due to
    // slot reuse, don't immediately re-register — the scanner may be
    // seeing stale timer type/label fields on the repurposed slot.
    const bool inCooldown =
        (g_TimeScriptPersistSuppress.deactivatedElemPtr == elemPtr &&
         (now - g_TimeScriptPersistSuppress.deactivatedTick) < 5000);
    if (!inCooldown) {
      if (!g_TimeScriptPersistSuppress.active ||
          g_TimeScriptPersistSuppress.elemPtr != elemPtr) {
        g_TimeScriptPersistSuppress.elemPtr = elemPtr;
        // Cache label + text SLC for slot-reuse detection in CG_DrawHudElem.
        uint32_t lbl = 0, txtSlc = 0;
        TryReadIntroU32(elemPtr, 0x40, lbl);
        TryReadIntroU32(elemPtr, 0x84, txtSlc);
        g_TimeScriptPersistSuppress.originalLabel = lbl;
        g_TimeScriptPersistSuppress.originalTextSlc = txtSlc;
      }
      g_TimeScriptPersistSuppress.lastSuppressTick = now;
      g_TimeScriptPersistSuppress.active = true;
    }
  }

  // Diagnostic logging
  static DWORD s_lastStoreLog = 0;
  if ((now - s_lastStoreLog) > 900) {
    s_lastStoreLog = now;
    char buf[640];
    sprintf_s(buf,
              "[HUDELEM-TIMER-SNAPSHOT] key=%s elem=0x%llX lbl=0x%X txt=0x%X "
              "x=%.1f y=%.1f fs=%.2f ao=%u as=%u mh=%u mv=%u "
              "time=%d dur=%d val=%.3f color=0x%08X "
              "outX=%.1f outY=%.1f scale=%.3f render=\"%.80s\"",
              key.c_str(), (unsigned long long)elemPtr,
              labelSlc, textSlc,
              elemX, elemY, fontScale, alignOrg, alignScreen,
              mappedHorzAlign, mappedVertAlign,
              timeField, durationField, valueField, colorPacked,
              outX, outY, outScale,
              korLabel.c_str());
    LogToFile(buf);
  }
}

static void TimeScript_PreDrawHook() {
  g_TimeScriptHookContext = TimeScriptHookContext{};

  // Check if a previous persistent alpha suppression should be restored
  // (countdown ended, element reused, etc.)
  TimeScript_RestorePersistentSuppressIfStale();

  if (TextHook_IsPauseMenuLikely() || TextHook_IsFrontendMenuLikely()) {
    return;
  }

  const uintptr_t rbx = g_TimeScriptCallerRegs.rbx;
  TimeScriptHudSnapshot snap;
  if (!TryReadTimeScriptHudSnapshot(rbx, snap)) {
    return;
  }

  std::string rawText;
  HudScriptCountdownMatch match;
  if (!TryResolveTimeScriptCountdown(snap, rawText, match)) {
    return;
  }
  if (!IsTimeScriptManagedCountdownKeyName(match.key)) {
    return;
  }

  TimeScriptHookContext ctx;
  ctx.snap = snap;
  ctx.match = match;
  ctx.rawText = rawText;
  ctx.textPtr = snap.textPtr;
  ctx.originalColor = snap.colorPacked;

  const uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  ctx.callerOffset = (g_TimeScriptCallerRegs.ret >= moduleBase)
                         ? (unsigned int)(g_TimeScriptCallerRegs.ret - moduleBase)
                         : 0u;
  ctx.layoutReady = TryComputeTimeScriptLayout(
      snap, ctx.renderX, ctx.renderY, ctx.renderScale, ctx.renderFontHeight);

  const DWORD now = GetTickCount();
  if (!ctx.layoutReady || ctx.match.renderText.empty()) {
    static DWORD s_lastLayoutMiss = 0;
    if ((now - s_lastLayoutMiss) > 900) {
      s_lastLayoutMiss = now;
      char buf[384];
      sprintf_s(
          buf,
          "[TIMESCRIPT-LAYOUT-MISS] caller=+0x%X rbx=0x%llX key=%s xy=%.1f,%.1f align=%u/%u fs=%.2f timer=%.3f",
          ctx.callerOffset, (unsigned long long)snap.rbx, match.key.c_str(),
          snap.x, snap.y, snap.horzAlign, snap.vertAlign, snap.fontScale,
          snap.timerValue);
      LogToFile(buf);
    }
    return;
  }
  ctx.active = true;

  // NOTE: We intentionally do NOT null the first character of the text string.
  // Nulling it makes the engine see an empty string, which causes the timer
  // number to shift left (loses the label's pixel width for positioning).
  // The SEH hook already returns same-width spaces for the label key, and
  // alpha suppression below makes the label invisible regardless of content.

  // Register for CG_DrawHudElem alpha=0 suppression.
  // No HudElem field writes — CG_DrawHudElem hook handles visual suppress.
  // This preserves typewriter sounds and animations.
  ctx.colorSuppressed = true;
  g_TimeScriptPersistSuppress.rbx = snap.rbx;
  g_TimeScriptPersistSuppress.originalColor = snap.colorPacked;
  g_TimeScriptPersistSuppress.lastSuppressTick = now;
  g_TimeScriptPersistSuppress.active = true;

  static std::unordered_map<std::string, DWORD> s_probeLogTick;
  const std::string sig =
      std::to_string((unsigned long long)snap.rbx) + "|" + match.tail;
  auto it = s_probeLogTick.find(sig);
  if (it == s_probeLogTick.end() || (now - it->second) > 900) {
    s_probeLogTick[sig] = now;
    char buf[640];
    sprintf_s(
        buf,
        "[TIMESCRIPT-HIDE] caller=+0x%X rbx=0x%llX textPtr=0x%llX hideA=%d persistAlpha=%d xy=%.1f,%.1f doc=%.1f,%.1f legacy=%.1f,%.1f useLegacy=%d align=%u/%u fs=%.2f timer=%.3f color=0x%08X raw=\"%.120s\" render=\"%.120s\"",
        ctx.callerOffset, (unsigned long long)snap.rbx,
        (unsigned long long)snap.textPtr, ctx.colorSuppressed ? 1 : 0,
        g_TimeScriptPersistSuppress.active ? 1 : 0,
        snap.x, snap.y, snap.docX, snap.docY,
        snap.legacyX, snap.legacyY, snap.usedLegacyXY ? 1 : 0, snap.horzAlign,
        snap.vertAlign, snap.fontScale, snap.timerValue, snap.colorPacked,
        rawText.c_str(), match.renderText.c_str());
    LogToFile(buf);
  }

  g_TimeScriptHookContext = std::move(ctx);
}

static void TimeScript_PostDrawHook() {
  TimeScriptHookContext ctx = std::move(g_TimeScriptHookContext);
  g_TimeScriptHookContext = TimeScriptHookContext{};

  if (!ctx.active) {
    return;
  }

  // NOTE: We intentionally do NOT restore text or alpha here.
  // - Text was never modified (no '\0' write), so no restore needed.
  // - Alpha is kept at 0 persistently to prevent flicker between frames.
  //   It will be restored when the countdown is no longer detected
  //   (see TimeScript_RestorePersistentSuppressIfStale below).

  if (!ctx.layoutReady || ctx.match.renderText.empty()) {
    return;
  }
  TimeScript_UpdateAuthoritativeRenderSnapshot(ctx);
}

static constexpr uintptr_t kTimeScriptFixedBufferAddr = IW6Offsets::Profile::Rva_49B00A8;

static bool TimeScript_TryResolveFallbackScanRange(
    TimeScriptFallbackScanRange &outRange);
static void TimeScript_PollFallbackDiagnostics();

static bool TimeScript_IsReadableProtect(DWORD protect) {
  const DWORD p = (protect & 0xFF);
  return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
         p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
         p == PAGE_EXECUTE_WRITECOPY;
}

static bool TimeScript_IsWritableProtect(DWORD protect) {
  const DWORD p = (protect & 0xFF);
  return p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
         p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

static bool TimeScript_IsCandidateRegion(const MEMORY_BASIC_INFORMATION &mbi,
                                         bool requireWritable) {
  if (mbi.State != MEM_COMMIT) {
    return false;
  }
  if (mbi.Protect & PAGE_NOACCESS) {
    return false;
  }
  if (mbi.Protect & PAGE_GUARD) {
    return false;
  }
  if (requireWritable) {
    return TimeScript_IsWritableProtect(mbi.Protect);
  }
  return TimeScript_IsReadableProtect(mbi.Protect);
}

static bool TimeScript_MatchPrefixInsensitive(const char *s,
                                              const char *prefix) {
  if (!s || !prefix) {
    return false;
  }
  for (; *prefix != '\0'; ++prefix, ++s) {
    const unsigned char a = (unsigned char)*s;
    const unsigned char b = (unsigned char)*prefix;
    if (a == 0) {
      return false;
    }
    if (std::tolower(a) != std::tolower(b)) {
      return false;
    }
  }
  return true;
}

static bool TimeScript_TryMatchManagedFallbackPrefix(const char *s,
                                                     const char *&outPrefix) {
  outPrefix = nullptr;
  if (!s) {
    return false;
  }

  static const char *kPrefixes[] = {
      "Powerdown in ",
      "Time to Exfil ",
      "Time to Impact: ",
  };
  for (const char *prefix : kPrefixes) {
    if (TimeScript_MatchPrefixInsensitive(s, prefix)) {
      outPrefix = prefix;
      return true;
    }
  }
  return false;
}

static bool TimeScript_TryResolveTextPtr(uintptr_t textPtr,
                                         std::string &outRawText,
                                         HudScriptCountdownMatch &outMatch) {
  outRawText.clear();
  outMatch = {};
  if (!(textPtr > 0x10000 && textPtr < 0x7FFFFFFFFFFF)) {
    return false;
  }
  if (!TryReadCStringPreview((const char *)textPtr, 160, outRawText)) {
    return false;
  }
  if (TryResolveHudScriptCountdownText(outRawText, outMatch) &&
      IsTimeScriptManagedCountdownKeyName(outMatch.key)) {
    return true;
  }

  std::string keyOnly;
  if (TryResolveHudScriptCountdownKeyFromRawText(outRawText, keyOnly) &&
      IsTimeScriptManagedCountdownKeyName(keyOnly)) {
    EnsureTranslationsLoaded();
    outMatch.key = keyOnly;
    auto itEng = g_KeyToEnglish.find(keyOnly);
    auto itKor = g_KeyToKorean.find(keyOnly);
    if (itEng != g_KeyToEnglish.end()) {
      outMatch.englishPrefix = StripColorCodes(itEng->second);
    }
    if (itKor != g_KeyToKorean.end()) {
      outMatch.koreanPrefix = itKor->second;
      outMatch.renderText = itKor->second;
    }
    return true;
  }

  return false;
}

static bool TimeScript_TryFindDynamicTextPtr(uintptr_t &outTextPtr,
                                             std::string &outRawText,
                                             HudScriptCountdownMatch &outMatch) {
  outTextPtr = 0;
  outRawText.clear();
  outMatch = {};

  TimeScriptFallbackScanRange scanRange;
  if (!TimeScript_TryResolveFallbackScanRange(scanRange)) {
    return false;
  }

  const DWORD now = GetTickCount();
  if ((now - g_TimeScriptRuntimeState.fallbackTextScanState.lastScanTick) < 16) {
    return false;
  }
  g_TimeScriptRuntimeState.fallbackTextScanState.lastScanTick = now;

  uintptr_t cursor = g_TimeScriptRuntimeState.fallbackTextScanState.cursor;
  if (cursor < scanRange.begin || cursor >= scanRange.end) {
    cursor = scanRange.begin;
  }

  constexpr size_t kTextScanBudgetBytes = 256u * 1024u;
  size_t budget = kTextScanBudgetBytes;
  const uintptr_t startCursor = cursor;
  bool wrapped = false;

  while (budget > 0 && scanRange.begin < scanRange.end) {
    if (cursor >= scanRange.end) {
      if (wrapped) {
        break;
      }
      cursor = scanRange.begin;
      wrapped = true;
      if (cursor >= startCursor) {
        break;
      }
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((LPCVOID)cursor, &mbi, sizeof(mbi)) == 0) {
      break;
    }

    const uintptr_t rawRegionBase = (uintptr_t)mbi.BaseAddress;
    const size_t regionSize = (size_t)mbi.RegionSize;
    const uintptr_t rawRegionEnd =
        rawRegionBase + (regionSize > 0 ? (uintptr_t)regionSize : 0x1000ull);
    const uintptr_t next =
        (rawRegionEnd > cursor) ? rawRegionEnd : (cursor + 0x1000ull);
    const uintptr_t regionBase =
        (std::max)(cursor, (std::max)(rawRegionBase, scanRange.begin));
    const uintptr_t regionEnd = (std::min)(rawRegionEnd, scanRange.end);

    if (!TimeScript_IsCandidateRegion(mbi, true) || regionEnd <= regionBase) {
      cursor = next;
      continue;
    }
    const size_t bytesAvailable = (size_t)(regionEnd - regionBase);
    if (bytesAvailable < 16) {
      cursor = next;
      continue;
    }

    const size_t scanBytes = (std::min)(bytesAvailable, budget);
    const char *bytes = (const char *)regionBase;
    for (size_t i = 0; i + 16 < scanBytes; ++i) {
      const unsigned char c = (unsigned char)bytes[i];
      if (std::tolower(c) != 'p' && std::tolower(c) != 't') {
        continue;
      }
      const char *matchedPrefix = nullptr;
      if (!TimeScript_TryMatchManagedFallbackPrefix(bytes + i, matchedPrefix)) {
        continue;
      }

      std::string rawText;
      HudScriptCountdownMatch match;
      const uintptr_t textPtr = regionBase + i;
      if (!TimeScript_TryResolveTextPtr(textPtr, rawText, match)) {
        continue;
      }

      outTextPtr = textPtr;
      outRawText = std::move(rawText);
      outMatch = std::move(match);
      g_TimeScriptRuntimeState.fallbackCachedTextPtr = outTextPtr;
      g_TimeScriptRuntimeState.fallbackTextScanState.cursor = outTextPtr;

      char buf[512];
      sprintf_s(
          buf,
          "[TIMESCRIPT-POLL-TEXTPTR] prefix=\"%s\" ptr=0x%llX region=0x%llX size=0x%llX raw=\"%.120s\"",
          matchedPrefix ? matchedPrefix : "?",
          (unsigned long long)outTextPtr, (unsigned long long)regionBase,
          (unsigned long long)regionSize, outRawText.c_str());
      LogToFile(buf);
      return true;
    }

    budget -= scanBytes;
    const uintptr_t consumedEnd = regionBase + (uintptr_t)scanBytes;
    cursor = (consumedEnd < regionEnd) ? consumedEnd : next;
    if (wrapped && cursor >= startCursor) {
      break;
    }
  }

  g_TimeScriptRuntimeState.fallbackTextScanState.cursor =
      (cursor >= scanRange.begin && cursor < scanRange.end) ? cursor
                                                            : scanRange.begin;

  static DWORD s_lastDynamicTextMissLogTick = 0;
  if ((now - s_lastDynamicTextMissLogTick) > 1500) {
    s_lastDynamicTextMissLogTick = now;
    char buf[192];
    sprintf_s(buf,
              "[TIMESCRIPT-POLL-TEXTPTR-MISS] prefix='managed_countdown' cursor=0x%llX",
              (unsigned long long)g_TimeScriptRuntimeState.fallbackTextScanState.cursor);
    LogToFile(buf);
  }

  return false;
}

static bool TimeScript_TryFindFallbackText(uintptr_t &outTextPtr,
                                           std::string &outRawText,
                                           HudScriptCountdownMatch &outMatch) {
  outTextPtr = 0;
  outRawText.clear();
  outMatch = {};

  if (TimeScript_TryResolveTextPtr(kTimeScriptFixedBufferAddr, outRawText,
                                   outMatch)) {
    outTextPtr = kTimeScriptFixedBufferAddr;
    g_TimeScriptRuntimeState.fallbackCachedTextPtr = outTextPtr;
    return true;
  }

  static DWORD s_lastBufferMissLogTick = 0;
  const DWORD nowMiss = GetTickCount();
  if ((nowMiss - s_lastBufferMissLogTick) > 1200) {
    s_lastBufferMissLogTick = nowMiss;
    char buf[160];
    sprintf_s(buf, "[TIMESCRIPT-POLL-BUFFER] readable=0 addr=0x%llX raw=\"\"",
              (unsigned long long)kTimeScriptFixedBufferAddr);
    LogToFile(buf);
  }

  if (g_TimeScriptRuntimeState.fallbackCachedTextPtr != 0) {
    if (TimeScript_TryResolveTextPtr(g_TimeScriptRuntimeState.fallbackCachedTextPtr,
                                     outRawText, outMatch)) {
      outTextPtr = g_TimeScriptRuntimeState.fallbackCachedTextPtr;
      return true;
    }
    g_TimeScriptRuntimeState.fallbackCachedTextPtr = 0;
  }

  return TimeScript_TryFindDynamicTextPtr(outTextPtr, outRawText, outMatch);
}

static bool TimeScript_TryResolveFallbackScanRange(
    TimeScriptFallbackScanRange &outRange) {
  outRange = {};

  if (g_TimeScriptRuntimeState.fallbackScanRange.initialized) {
    outRange = g_TimeScriptRuntimeState.fallbackScanRange;
    return outRange.begin < outRange.end;
  }

  const uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  if (moduleBase == 0) {
    g_TimeScriptRuntimeState.fallbackScanRange.initialized = true;
    return false;
  }

  const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)moduleBase;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    g_TimeScriptRuntimeState.fallbackScanRange.initialized = true;
    return false;
  }

  const IMAGE_NT_HEADERS64 *nt =
      (const IMAGE_NT_HEADERS64 *)(moduleBase + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    g_TimeScriptRuntimeState.fallbackScanRange.initialized = true;
    return false;
  }

  const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
  uintptr_t bestBegin = 0;
  uintptr_t bestEnd = 0;
  size_t bestSize = 0;
  bool foundNamedData = false;
  for (unsigned int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const DWORD sectionChars = sec[i].Characteristics;
    if ((sectionChars & IMAGE_SCN_MEM_WRITE) == 0) {
      continue;
    }

    const uintptr_t begin = moduleBase + sec[i].VirtualAddress;
    const size_t sectionSize = (std::max)((size_t)sec[i].Misc.VirtualSize,
                                          (size_t)sec[i].SizeOfRawData);
    if (sectionSize == 0) {
      continue;
    }

    const uintptr_t end = begin + sectionSize;
    if (end <= begin) {
      continue;
    }

    char name[9] = {};
    memcpy(name, sec[i].Name, 8);
    const bool isDataSection =
        (strncmp(name, ".data", 5) == 0 || strncmp(name, "data", 4) == 0);

    if (isDataSection) {
      if (!foundNamedData || sectionSize > bestSize) {
        bestBegin = begin;
        bestEnd = end;
        bestSize = sectionSize;
        foundNamedData = true;
      }
      continue;
    }

    if (!foundNamedData && sectionSize > bestSize) {
      bestBegin = begin;
      bestEnd = end;
      bestSize = sectionSize;
    }
  }

  g_TimeScriptRuntimeState.fallbackScanRange.begin = bestBegin;
  g_TimeScriptRuntimeState.fallbackScanRange.end = bestEnd;
  g_TimeScriptRuntimeState.fallbackScanRange.initialized = true;
  outRange = g_TimeScriptRuntimeState.fallbackScanRange;

  if (outRange.begin < outRange.end) {
    char buf[192];
    sprintf_s(buf,
              "[TIMESCRIPT-POLL-RANGE] begin=0x%llX end=0x%llX bytes=0x%llX",
              (unsigned long long)outRange.begin,
              (unsigned long long)outRange.end,
              (unsigned long long)(outRange.end - outRange.begin));
    LogToFile(buf);
  }

  return outRange.begin < outRange.end;
}
static float TimeScript_ScoreFallbackSnapshot(
    const TimeScriptHudSnapshot &snap) {
  float score =
      (float)((snap.colorPacked >> 24) & 0xFFu) / 255.0f * 100.0f;
  if (snap.horzAlign == 2) {
    score += 40.0f;
  } else if (snap.horzAlign == 1) {
    score += 16.0f;
  }
  if (snap.vertAlign == 0) {
    score += 18.0f;
  }
  if (snap.timerValue > 0.05f && snap.timerValue < 600.0f) {
    score += 16.0f;
  }
  if (snap.x >= 0.0f && snap.x <= 220.0f) {
    score += 12.0f;
  }
  if (snap.y >= -120.0f && snap.y <= 24.0f) {
    score += 12.0f;
  }
  if (snap.fontScale >= 0.35f && snap.fontScale <= 1.20f) {
    score += 10.0f;
  }
  if (snap.horzAlign == 2 && snap.vertAlign == 0 && snap.x >= -340.0f &&
      snap.x <= -140.0f && snap.y >= 60.0f && snap.y <= 140.0f &&
      snap.fontScale >= 1.20f && snap.fontScale <= 2.10f) {
    score += 72.0f;
  }
  if (!snap.usedLegacyXY) {
    score += 4.0f;
  }
  return score;
}

static bool TimeScript_TryResolveFallbackCandidate(
    uintptr_t hudPtr, uintptr_t expectedTextPtr, TimeScriptHudSnapshot &outSnap,
    std::string &outRawText,
    HudScriptCountdownMatch &outMatch) {
  outSnap = {};
  outRawText.clear();
  outMatch = {};

  TimeScriptHudSnapshot snap;
  if (!TryReadTimeScriptHudSnapshot(hudPtr, snap)) {
    return false;
  }
  if (snap.textPtr != expectedTextPtr) {
    return false;
  }

  std::string rawText;
  HudScriptCountdownMatch match;
  if (!TryResolveTimeScriptCountdown(snap, rawText, match) ||
      !IsTimeScriptManagedCountdownKeyName(match.key)) {
    return false;
  }

  outSnap = snap;
  outRawText = std::move(rawText);
  outMatch = std::move(match);
  return true;
}

static bool TimeScript_TryFindFallbackSnapshot(
    TimeScriptHudSnapshot &outSnap, std::string &outRawText,
    HudScriptCountdownMatch &outMatch) {
  outSnap = {};
  outRawText.clear();
  outMatch = {};

  uintptr_t textPtr = 0;
  std::string bufferRaw;
  HudScriptCountdownMatch bufferMatch;
  if (!TimeScript_TryFindFallbackText(textPtr, bufferRaw, bufferMatch)) {
    g_TimeScriptRuntimeState.fallbackCachedHudPtr = 0;
    return false;
  }

  if (g_TimeScriptRuntimeState.fallbackCachedHudPtr != 0) {
    if (TimeScript_TryResolveFallbackCandidate(g_TimeScriptRuntimeState.fallbackCachedHudPtr,
                                               textPtr, outSnap, outRawText,
                                               outMatch)) {
      return true;
    }
    g_TimeScriptRuntimeState.fallbackCachedHudPtr = 0;
  }

  TimeScriptFallbackScanRange scanRange;
  if (!TimeScript_TryResolveFallbackScanRange(scanRange)) {
    return false;
  }

  const DWORD now = GetTickCount();
  if ((now - g_TimeScriptRuntimeState.fallbackHudScanState.lastScanTick) < 16) {
    return false;
  }
  g_TimeScriptRuntimeState.fallbackHudScanState.lastScanTick = now;

  TimeScriptHudSnapshot bestSnap;
  std::string bestRawText;
  HudScriptCountdownMatch bestMatch;
  float bestScore = -100000.0f;
  uintptr_t cursor = g_TimeScriptRuntimeState.fallbackHudScanState.cursor;
  if (cursor < scanRange.begin || cursor >= scanRange.end) {
    cursor = scanRange.begin;
  }

  constexpr size_t kHudScanBudgetBytes = 256u * 1024u;
  size_t budget = kHudScanBudgetBytes;
  const uintptr_t startCursor = cursor;
  bool wrapped = false;
  while (budget > 0 && scanRange.begin < scanRange.end) {
    if (cursor >= scanRange.end) {
      if (wrapped) {
        break;
      }
      cursor = scanRange.begin;
      wrapped = true;
      if (cursor >= startCursor) {
        break;
      }
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((LPCVOID)cursor, &mbi, sizeof(mbi)) == 0) {
      break;
    }

    const uintptr_t rawRegionBase = (uintptr_t)mbi.BaseAddress;
    const size_t regionSize = (size_t)mbi.RegionSize;
    const uintptr_t rawRegionEnd =
        rawRegionBase + (regionSize > 0 ? (uintptr_t)regionSize : 0x1000ull);
    const uintptr_t next =
        (rawRegionEnd > cursor) ? rawRegionEnd : (cursor + 0x1000ull);
    const uintptr_t regionBase =
        (std::max)(cursor, (std::max)(rawRegionBase, scanRange.begin));
    const uintptr_t regionEnd = (std::min)(rawRegionEnd, scanRange.end);

    if (!TimeScript_IsCandidateRegion(mbi, true) || regionEnd <= regionBase) {
      cursor = next;
      continue;
    }
    const size_t bytesAvailable = (size_t)(regionEnd - regionBase);
    if (bytesAvailable < sizeof(uintptr_t)) {
      cursor = next;
      continue;
    }

    const size_t scanBytes = (std::min)(bytesAvailable, budget);
    const uintptr_t windowEnd = regionBase + (uintptr_t)scanBytes;
    const uintptr_t scanBase =
        (regionBase > (scanRange.begin + 0x50)) ? (regionBase - 0x50)
                                                : scanRange.begin;
    const uintptr_t scanBegin =
        (scanBase + (sizeof(uintptr_t) - 1)) & ~(sizeof(uintptr_t) - 1);
    const uintptr_t scanEnd = windowEnd;
    for (uintptr_t fieldAddr = scanBegin + 0x50;
         fieldAddr + sizeof(uintptr_t) <= scanEnd;
         fieldAddr += sizeof(uintptr_t)) {
      const uintptr_t candidateTextPtr = *(const uintptr_t *)fieldAddr;
      if (candidateTextPtr != textPtr) {
        continue;
      }

      const uintptr_t hudPtr = fieldAddr - 0x50;
      TimeScriptHudSnapshot snap;
      std::string rawText;
      HudScriptCountdownMatch match;
      if (!TimeScript_TryResolveFallbackCandidate(hudPtr, textPtr, snap,
                                                  rawText, match)) {
        continue;
      }

      const float score = TimeScript_ScoreFallbackSnapshot(snap);
      if (score > bestScore) {
        bestScore = score;
        bestSnap = snap;
        bestRawText = std::move(rawText);
        bestMatch = std::move(match);
      }
    }

    budget -= scanBytes;
    const uintptr_t consumedEnd = regionBase + (uintptr_t)scanBytes;
    cursor = (consumedEnd < regionEnd) ? consumedEnd : next;
    if (wrapped && cursor >= startCursor) {
      break;
    }
  }

  g_TimeScriptRuntimeState.fallbackHudScanState.cursor =
      (cursor >= scanRange.begin && cursor < scanRange.end) ? cursor
                                                            : scanRange.begin;

  if (bestScore <= -99999.0f) {
    static DWORD s_lastHudMissLogTick = 0;
    if ((now - s_lastHudMissLogTick) > 1200) {
      s_lastHudMissLogTick = now;
      char buf[384];
      sprintf_s(buf,
                "[TIMESCRIPT-POLL-HUDPTR-MISS] textPtr=0x%llX cursor=0x%llX raw=\"%.120s\"",
                (unsigned long long)textPtr,
                (unsigned long long)g_TimeScriptRuntimeState.fallbackHudScanState.cursor,
                bufferRaw.c_str());
      LogToFile(buf);
    }
    return false;
  }

  g_TimeScriptRuntimeState.fallbackCachedHudPtr = bestSnap.rbx;
  g_TimeScriptRuntimeState.fallbackHudScanState.cursor = bestSnap.rbx;
  outSnap = bestSnap;
  outRawText = std::move(bestRawText);
  outMatch = std::move(bestMatch);

  char buf[512];
  sprintf_s(
      buf,
      "[TIMESCRIPT-POLL-FOUND] rbx=0x%llX textPtr=0x%llX xy=%.1f,%.1f align=%u/%u fs=%.2f timer=%.3f color=0x%08X raw=\"%.96s\" render=\"%.96s\"",
      (unsigned long long)outSnap.rbx, (unsigned long long)outSnap.textPtr,
      outSnap.x, outSnap.y, outSnap.horzAlign, outSnap.vertAlign,
      outSnap.fontScale, outSnap.timerValue, outSnap.colorPacked,
      outRawText.c_str(), outMatch.renderText.c_str());
  LogToFile(buf);
  return true;
}

static void TimeScript_PollFallbackDiagnostics() {
  if (TextHook_IsPauseMenuLikely() || TextHook_IsFrontendMenuLikely()) {
    return;
  }

  // Fallback polling incrementally scans large executable/read-only regions to
  // recover a countdown when the dedicated hook path is unavailable. Once a
  // real managed countdown snapshot is already live, continuing that scan just
  // burns CPU for no benefit and can tank performance during SATFARM's post-
  // transition/tank section.
  TimeScriptRenderSnapshot activeSnap;
  if (TextHook_GetTimeScriptRenderSnapshot(activeSnap) &&
      IsTimeScriptManagedCountdownKeyName(activeSnap.key)) {
    return;
  }

  TimeScriptHudSnapshot snap;
  std::string rawText;
  HudScriptCountdownMatch match;
  if (!TimeScript_TryFindFallbackSnapshot(snap, rawText, match)) {
    return;
  }

  static std::unordered_map<std::string, DWORD> s_pollLogTick;
  const DWORD now = GetTickCount();
  const std::string sig =
      std::to_string((unsigned long long)snap.rbx) + "|" +
      match.tail;
  auto it = s_pollLogTick.find(sig);
  if (it == s_pollLogTick.end() || (now - it->second) > 800) {
    s_pollLogTick[sig] = now;
    char buf[512];
    sprintf_s(
        buf,
        "[TIMESCRIPT-POLL-CAND] key=%s rbx=0x%llX x=%.1f y=%.1f align=%u/%u fs=%.2f timer=%.3f text=\"%.96s\"",
        match.key.c_str(), (unsigned long long)snap.rbx, snap.x, snap.y,
        snap.horzAlign, snap.vertAlign, snap.fontScale, snap.timerValue,
        match.renderText.c_str());
    LogToFile(buf);
  }
}
