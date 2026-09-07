void __fastcall Detour_IRFSub1(void *p1, void *p2, void *p3, void *p4,
                                 void *p5, void *p6, void *p7, void *p8,
                                 void *p9, void *p10, void *p11, void *p12) {
  if (Original_IRFSub1) {
    Original_IRFSub1(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12);
  }
}
void __fastcall Detour_IRFSub2(void *p1, void *p2, void *p3, void *p4,
                                 void *p5, void *p6, void *p7, void *p8) {
  if (Original_IRFSub2) {
    Original_IRFSub2(p1, p2, p3, p4, p5, p6, p7, p8);
  }
}

// Get captured screen coords for a given slcIdx
bool TextHook_GetIntroScreenCoord(int slcIdx, float &outX, float &outY,
                                  float &outXScale, float &outYScale) {
  std::lock_guard<std::mutex> lock(g_screenCoordMutex);
  auto it = g_introScreenCoords.find(slcIdx);
  if (it == g_introScreenCoords.end()) return false;
  DWORD age = GetTickCount() - it->second.tick;
  if (age > 3000) {
    g_introScreenCoords.erase(it);
    return false;
  }
  outX = it->second.x;
  outY = it->second.y;
  outXScale = it->second.xScale;
  outYScale = it->second.yScale;
  return true;
}

// Helper: intro overlay keys include classic *_INTROSCREEN_* lines and a
// small set of standalone center-card keys that still use the intro renderer.
static bool IsIntroOverlayKey(const std::string &key) {
  return ClassifyHudKey(key) == HUD_KEY_INTRO;
}
static std::string ExtractIntroSessionTag(const std::string &key) {
  size_t pos = key.find("_INTROSCREEN");
  if (pos == std::string::npos) {
    if (ClassifyHudKey(key) == HUD_KEY_INTRO) {
      size_t prefix = key.find('_');
      if (prefix != std::string::npos) {
        return key.substr(0, prefix);
      }
    }
    return key;
  }
  return key.substr(0, pos);
}

// Helper: Extract display order from INTROSCREEN key suffix
static int ExtractIntroOrder(const std::string &key) {
  // Patterns: *_INTROSCREEN_LINE_N, *_INTROSCREEN_LINEN
  // Special: *_INTROSCREEN_YEARSLATER, *_INTROSCREEN_12_YEARS, etc.

  // Standalone / irregular center-card keys that should share the exact same
  // center-card lane and effect path as YEARSLATER.
  if (key == "CARRIER_3DAYS" ||
      key == "ODIN_INTROSCREEN_LINE_0" ||
      key == "SKYWAY_INTROSCREEN_TIMEBEFORE" ||
      key == "DEER_HUNT_JEEP_INTROLINE_1" ||
      key == "LAS_VEGAS_INTRO_TIME1" ||
      key == "LAS_VEGAS_INTRO_TIME2" ||
      key == "LAS_VEGAS_INTRO_TIME3" ||
      key == "LAS_VEGAS_INTRO_TIME4" ||
      key == "LAS_VEGAS_INTRO_TIME5") {
    return 50;
  }

  // Check for LINE_N pattern (e.g., BLACK_ICE_INTROSCREEN_LINE_3)
  auto pos = key.find("_LINE_");
  if (pos != std::string::npos) {
    std::string numStr = key.substr(pos + 6);
    try { return std::stoi(numStr); } catch (...) { return 99; }
  }

  // Check for LINEN without underscore (e.g., CARRIER_INTROSCREEN_LINE3)
  pos = key.find("_LINE");
  if (pos != std::string::npos) {
    std::string rest = key.substr(pos + 5);
    if (!rest.empty() && rest[0] >= '0' && rest[0] <= '9') {
      try { return std::stoi(rest); } catch (...) { return 99; }
    }
  }

  // Special suffixes
  if (key.find("YEARSLATER") != std::string::npos) return 50;
  if (key.find("12_YEARS") != std::string::npos) return 51;

  // Standard intro layout suffixes (non-center-card).
  // These are regular mission info lines that should use atlas 0.
  // Covers _TITLE, _LOC, _TIME, _NAME, _PLAYERNAME, _LEVELNAME,
  // _DATE, _LOCATION, and any other unrecognized suffix.
  {
    auto endsWith = [&](const char *suffix) -> bool {
      size_t len = strlen(suffix);
      return key.size() >= len &&
             key.compare(key.size() - len, len, suffix) == 0;
    };
    if (endsWith("_INTROSCREEN_TITLE") || endsWith("_INTROSCREEN_LEVELNAME"))
      return 0;
    if (endsWith("_INTROSCREEN_TIME") || endsWith("_INTROSCREEN_DATE"))
      return 2;
    if (endsWith("_INTROSCREEN_LOC") || endsWith("_INTROSCREEN_LOCATION"))
      return 3;
    // Character/player name is equivalent to LINE_3..4 in other missions
    // (NOT isTitle — needs typewriter reveal and is eligible for drift).
    if (endsWith("_INTROSCREEN_NAME") || endsWith("_INTROSCREEN_PLAYERNAME"))
      return 4;
  }

  // Safe default: treat any remaining unrecognized INTROSCREEN suffix as a
  // regular layout line (order < 50), NOT a center card.  Only explicitly
  // matched patterns above (YEARSLATER, 12_YEARS, hardcoded list) get >= 50.
  return 10;
}

// Called by FSHook when INTROSCREEN key is loaded via DB_FindXAssetHeader
void TextHook_OnIntroKey(const char *key) {
  if (!key) return;
  EnsureTranslationsLoaded();

  std::string keyStr(key);
  if (!IsIntroOverlayKey(keyStr)) return;

  auto itKor = g_KeyToKorean.find(keyStr);
  if (itKor == g_KeyToKorean.end()) {
    if (kVerboseRuntimeLogs) LogToFile("[TextHook] INTRO key not in Korean map: " + keyStr);
    return;
  }

  std::string korean = itKor->second;
  int order = ExtractIntroOrder(keyStr);

  DWORD now = GetTickCount();

  {
    std::lock_guard<std::mutex> lock(g_IntroOverlayMutex);
    std::string sessionTag = ExtractIntroSessionTag(keyStr);
    bool tagChanged = g_IntroOverlayActive && !g_IntroOverlayTag.empty() &&
                      (g_IntroOverlayTag != sessionTag);

    // Start a fresh session when:
    // 1) first intro key arrives (!g_IntroOverlayActive),
    // 2) mission tag changes (new mission).
    // No special restart/pause detection — native staleness handles ESC pause,
    // and same-mission restarts just update existing lines in place.
    if (!g_IntroOverlayActive || tagChanged) {
      if (tagChanged) {
        if (kVerboseRuntimeLogs) LogToFile("[TextHook] INTRO session reset tag=" + sessionTag);
      }
      g_IntroOverlayLines.clear();
      g_IntroOverlayStartTime = now;
      g_IntroOverlayActive = true;
      g_IntroOverlayTag = sessionTag;
      {
        std::lock_guard<std::mutex> nativeLock(g_nativeIntroMutex);
        g_nativeIntroMap.clear();
      }
      {
        std::lock_guard<std::mutex> coordLock(g_screenCoordMutex);
        g_introScreenCoords.clear();
      }
    }

    // Check if this key already exists (update instead of duplicate)
    bool found = false;
    for (auto &line : g_IntroOverlayLines) {
      if (line.english == keyStr) {
        line.text = korean;
        line.order = order;
        // Lock slcIdx once assigned (don't overwrite - prevents position swap)
        if (line.slcIdx < 0 && g_currentIntroSlcIdx >= 0) {
          line.slcIdx = g_currentIntroSlcIdx;
          if (kVerboseRuntimeLogs) {
            char slcBuf[64];
            sprintf_s(slcBuf, " -> slc=0x%X", g_currentIntroSlcIdx);
            LogToFile("[TextHook] INTRO slcIdx assigned: " + keyStr + slcBuf);
          }
        }
        found = true;
        break;
      }
    }

    if (!found) {
      IntroOverlayLine line;
      line.text = korean;
      line.english = keyStr; // Store key name for tracking
      line.order = order;
      line.slcIdx = g_currentIntroSlcIdx; // From Detour_IntroRenderFn context
      if (g_currentIntroSlcIdx >= 0 && kVerboseRuntimeLogs) {
        char slcBuf[64];
        sprintf_s(slcBuf, " -> slc=0x%X (%d)", g_currentIntroSlcIdx, g_currentIntroSlcIdx);
        LogToFile("[TextHook] INTRO new line: " + keyStr + slcBuf);
      }
      g_IntroOverlayLines.push_back(line);
    }

    // Sort by order
    std::sort(g_IntroOverlayLines.begin(), g_IntroOverlayLines.end(),
              [](const IntroOverlayLine &a, const IntroOverlayLine &b) {
                return a.order < b.order;
              });

    g_IntroOverlayLastUpdate = now;
  }

  if (kEnableRuntimeTextCapture) {
    LogToFile("[TextHook] INTRO queued: " + keyStr + " order=" +
              std::to_string(order) + " kor=\"" + Utf8Preview(korean, 40) +
              "\"");
  }
}

// Get current intro overlay snapshot (called by D3D11Hook for rendering)
IntroOverlaySnapshot TextHook_GetIntroOverlaySnapshot() {
  std::lock_guard<std::mutex> lock(g_IntroOverlayMutex);

  IntroOverlaySnapshot snap;
  DWORD now = GetTickCount();

  // Check if intro has timed out
  if (g_IntroOverlayActive &&
      (now - g_IntroOverlayLastUpdate) > INTRO_OVERLAY_TIMEOUT) {
    g_IntroOverlayActive = false;
    g_IntroOverlayLines.clear();
    g_IntroOverlayTag.clear();
  }

  snap.lines = g_IntroOverlayLines;
  snap.startTime = g_IntroOverlayStartTime;
  snap.lastUpdate = g_IntroOverlayLastUpdate;
  snap.active = g_IntroOverlayActive;

  return snap;
}

// =============================================================================
// IntroTextLayout Detour (0x3F3DE0) - Captures intro/chyron text + layout
// =============================================================================
void *Detour_IntroTextLayout(const char *text, void *layoutTable, int flags) {

  static uintptr_t s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  uintptr_t retAddr = (uintptr_t)_ReturnAddress();
  uintptr_t callerOffset = retAddr - s_moduleBase;

  // --- Soft guard state transition ---
  // When soft guard is ON (SLC disabled after ui_play_credits), IntroTextLayout
  // is the arbiter: CREDITS_* key → upgrade to full guard (credits confirmed),
  // non-CREDITS_* key → cancel soft guard (gameplay confirmed).
  {
    extern std::atomic<bool> g_creditsSoftGuard;
    extern std::atomic<bool> g_creditsGuardActive;
    if (g_creditsSoftGuard.load(std::memory_order_relaxed)) {
      if (text && text[0] == 'C' && text[1] == 'R' &&
          strncmp(text, "CREDITS_", 8) == 0) {
        // Credits rendering confirmed → upgrade to full guard
        g_creditsSoftGuard.store(false, std::memory_order_relaxed);
        g_creditsGuardActive.store(true, std::memory_order_relaxed);
        TextHook_SuspendHooksForCredits();
        LogToFile("[CREDITS-GUARD] HARD — CREDITS_* in IntroTextLayout (soft→full)");
        return Original_IntroTextLayout
                   ? Original_IntroTextLayout(text, layoutTable, flags)
                   : nullptr;
      } else {
        // Non-credits key → gameplay started, cancel soft guard
        g_creditsSoftGuard.store(false, std::memory_order_relaxed);
        TextHook_SoftResumeSLC();
        LogToFile("[CREDITS-GUARD] SOFT-OFF — non-credits IntroTextLayout key");
        // fall through to normal processing
      }
    }
  }

  // Credits guard: CREDITS_* keys are only used during the credits screen.
  // Skip all processing (logging, snapshot, hint/objective capture) to prevent
  // HUD false positives and per-frame processing flood.
  // Always call SuspendHooksForCredits() — it's idempotent (checks
  // g_hooksSuspendedForCredits internally).  This covers the case where
  // IMPROUDOFYOU1111 set g_creditsGuardActive=true without suspending hooks.
  if (text && text[0] == 'C' && text[1] == 'R' &&
      strncmp(text, "CREDITS_", 8) == 0) {
    extern std::atomic<bool> g_creditsGuardActive;
    if (!g_creditsGuardActive.load(std::memory_order_relaxed)) {
      g_creditsGuardActive.store(true, std::memory_order_relaxed);
    }
    TextHook_SuspendHooksForCredits();
    return Original_IntroTextLayout
               ? Original_IntroTextLayout(text, layoutTable, flags)
               : nullptr;
  }

  const int introSlc =
      (g_currentIntroSlcIdx >= 0) ? g_currentIntroSlcIdx : g_lastIntroSlcIdx;

  // Deduplicate: only log when text or caller changes
  static char s_lastText[256] = {};
  static uintptr_t s_lastCaller = 0;
  static int s_uniqueCount = 0;
  static int s_repeatCount = 0;
  static int s_totalCount = 0;

  s_totalCount++;

  bool isNewEntry = false;
  const char *txt = text ? text : "(null)";
  const std::string txtStr = text ? std::string(text) : std::string();
  if (callerOffset != s_lastCaller || strncmp(txt, s_lastText, 255) != 0) {
    isNewEntry = true;
    s_lastCaller = callerOffset;
    strncpy_s(s_lastText, txt, 255);
    if (s_repeatCount > 1) {
      char rbuf[256];
      sprintf_s(rbuf, "[INTROTEXT] (repeated %d times)", s_repeatCount);
      LogToFile(rbuf);
    }
    s_repeatCount = 0;
  }
  s_repeatCount++;

  HudScriptCountdownMatch timerMatch{};
  const bool looksCountdownTimer =
      (!txtStr.empty() && TryResolveHudScriptCountdownText(txtStr, timerMatch));
  if (looksCountdownTimer && isNewEntry) {
    char tbuf[768];
    sprintf_s(
        tbuf,
        "[INTROTEXT-TIMER] caller=+0x%llX slc=0x%X flags=%d full=%d key=%s text=\"%.200s\" table=0x%p",
        (unsigned long long)callerOffset, (unsigned int)(introSlc >= 0 ? introSlc : 0),
        flags, 1, timerMatch.key.c_str(), txt,
        layoutTable);
    LogToFile(tbuf);
  }

  if (isNewEntry && s_uniqueCount < 200) {
    s_uniqueCount++;

    char buf[768];
    sprintf_s(buf,
              "[INTROTEXT] #%d total=%d caller=0x%llX "
              "text=\"%.200s\" table=0x%p flags=%d",
              s_uniqueCount, s_totalCount,
              (unsigned long long)callerOffset,
              txt, layoutTable, flags);
    LogToFile(buf);

    // Dump layout table (first time only - it's a constant at 0x14071B840)
    static bool s_tableDumped = false;
    if (!s_tableDumped && layoutTable && !IsBadReadPtr(layoutTable, 128)) {
      s_tableDumped = true;
      volatile uint32_t *dw = (volatile uint32_t *)layoutTable;
      sprintf_s(buf,
                "[INTROTEXT] table +00: %08X %08X %08X %08X %08X %08X %08X %08X "
                "%08X %08X %08X %08X %08X %08X %08X %08X",
                dw[0], dw[1], dw[2], dw[3], dw[4], dw[5], dw[6], dw[7],
                dw[8], dw[9], dw[10], dw[11], dw[12], dw[13], dw[14], dw[15]);
      LogToFile(buf);
      volatile float *fp = (volatile float *)layoutTable;
      sprintf_s(buf,
                "[INTROTEXT] table floats: %.3f %.3f %.3f %.3f "
                "%.3f %.3f %.3f %.3f",
                fp[0], fp[1], fp[2], fp[3],
                fp[4], fp[5], fp[6], fp[7]);
      LogToFile(buf);

      // Second row
      sprintf_s(buf,
                "[INTROTEXT] table +40: %08X %08X %08X %08X %08X %08X %08X %08X "
                "%08X %08X %08X %08X %08X %08X %08X %08X",
                dw[16], dw[17], dw[18], dw[19], dw[20], dw[21], dw[22], dw[23],
                dw[24], dw[25], dw[26], dw[27], dw[28], dw[29], dw[30], dw[31]);
      LogToFile(buf);
    }
  }

  // NOTE: CODEDUMP done - found 1024-byte struct array at module+0x057F8D70
  //   return pointer points INTO this array (stride 0x400)
  //   Now dump the return struct to find render params (x,y,alpha,scale)

  // --- HUDHINT English suppression ---
  // When called from CG_DrawHudElem for a HUDHINT key with Korean translation,
  // pass " " to the original so nothing visible renders.  This does NOT modify
  // x/y/alpha/color, so CgDrawCapture and the Korean renderer are unaffected.
  {
    const uintptr_t cgElem =
        g_CGDrawHudElem_ElemPtr.load(std::memory_order_acquire);
    if (cgElem != 0 && text && text[0] != '\0') {
      EnsureTranslationsLoaded();
      std::string keyUpper = ToUpper(std::string(text));
      auto itKr = g_KeyToKorean.find(keyUpper);
      if (itKr != g_KeyToKorean.end() && !itKr->second.empty()) {
        const HudKeyType kt = ClassifyHudKey(keyUpper);
        if (kt == HUD_KEY_HINT || kt == HUD_KEY_OBJ_HUD_HINT) {
          static const char kBlank[] = " ";
          void *blankResult = Original_IntroTextLayout
                                  ? Original_IntroTextLayout(kBlank, layoutTable, flags)
                                  : nullptr;
          return blankResult;
        }
      }
    }
  }

  // Call original function
  void *result = Original_IntroTextLayout
                     ? Original_IntroTextLayout(text, layoutTable, flags)
                     : nullptr;

  // Log return value briefly
  if (isNewEntry && s_uniqueCount <= 10) {
    char buf[512];
    uintptr_t retOff = (uintptr_t)result - s_moduleBase;
    sprintf_s(buf, "[INTROTEXT] return=0x%p (module+0x%llX)",
              result, (unsigned long long)retOff);
    LogToFile(buf);

    // Extended table dump: +80 to +FF (one-time, constants might extend)
    static bool s_tableExtDumped = false;
    if (!s_tableExtDumped && layoutTable) {
      s_tableExtDumped = true;
      uintptr_t tbl = (uintptr_t)layoutTable;
      if (!IsBadReadPtr((void *)(tbl + 0x80), 128)) {
        volatile uint32_t *dw = (volatile uint32_t *)(tbl + 0x80);
        volatile float *fp = (volatile float *)(tbl + 0x80);
        sprintf_s(buf,
                  "[INTROTEXT] table +80: %08X %08X %08X %08X %08X %08X %08X %08X "
                  "%08X %08X %08X %08X %08X %08X %08X %08X",
                  dw[0], dw[1], dw[2], dw[3], dw[4], dw[5], dw[6], dw[7],
                  dw[8], dw[9], dw[10], dw[11], dw[12], dw[13], dw[14], dw[15]);
        LogToFile(buf);
        sprintf_s(buf,
                  "[INTROTEXT] table +80f: %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
                  fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6], fp[7]);
        LogToFile(buf);
        sprintf_s(buf,
                  "[INTROTEXT] table +A0f: %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
                  fp[8], fp[9], fp[10], fp[11], fp[12], fp[13], fp[14], fp[15]);
        LogToFile(buf);
      }
    }
  }

  return result;
}

// =============================================================================
// IntroRenderFn Detour (0x1F1740) - Captures render context (R8/arg3)
// Flow: SLC_Wrapper ??IntroTextLayout ??JMP 0x416AE0(type, text, ctx, count)
// =============================================================================
// Per-slcIdx native HudElem render states (updated per-frame from RBX struct)
// NOTE: g_nativeIntroMap and g_nativeIntroMutex are defined above (near HDT560 hook)

bool TextHook_GetNativeIntroRender(int slcIdx, NativeIntroRenderState &out) {
  std::lock_guard<std::mutex> lock(g_nativeIntroMutex);
  auto it = g_nativeIntroMap.find((uint32_t)slcIdx);
  if (it == g_nativeIntroMap.end()) return false;
  DWORD now = GetTickCount();
  DWORD age = now - it->second.lastTick;
  if (age > 10000) { // cleanup after 10s (covers full intro + drift window)
    g_nativeIntroMap.erase(it);
    return false;
  }
  out = it->second;
  return true;
}
std::vector<NativeIntroRenderState> TextHook_GetAllNativeIntroStates() {
  std::lock_guard<std::mutex> lock(g_nativeIntroMutex);
  std::vector<NativeIntroRenderState> result;
  DWORD now = GetTickCount();
  for (auto it = g_nativeIntroMap.begin(); it != g_nativeIntroMap.end(); ) {
    if (now - it->second.lastTick > 10000) {
      it = g_nativeIntroMap.erase(it);
    } else {
      result.push_back(it->second);
      ++it;
    }
  }
  return result;
}

// Read engine's safe area pixel offset from base+0x706058.
// Discovered via disassembly: IntroRenderFn's coordinate transform uses
// ADDSS XMM0, [RIP+offset] pointing to this address for both X and Y margins.
EngineSafeArea TextHook_GetEngineSafeArea() {
  // Historical values were constant 0.5 / 0.6 floats in .rdata, not mutable
  // screen placement. Real safe-area values come from ScrPlace in D3D11Hook.
  return EngineSafeArea{0.5f, 0.6f};
}

// POD snapshot for intro HudElem reads. Filled through SEH-safe helper so
// Detour_IntroRenderFn does not directly dereference transient/stale RBX.
struct IntroHudElemSnapshot {
  uintptr_t rbx = 0;
  uint32_t type = 0;
  float x = 0.0f;
  float y = 0.0f;
  float fontScale = 0.0f;
  float timerValue = 0.0f;
  uint32_t horzAlign = 0;
  uint32_t vertAlign = 0;
  uint32_t colorPacked = 0;
  int elemTime = 0;
  int fxBirthTime = 0;
  int fxLetterTime = 0;
  int flags = 0;
  uint32_t labelSlc = 0;
  uint32_t textSlc = 0;
  uintptr_t textPtr = 0;
};

struct IntroDetourSharedContext {
  IntroHudElemSnapshot snap{};
  unsigned int callerOffset = 0;
  int type = 0;
  int slcIndex = 0;
  void *renderCtx = nullptr;
  int count = 0;
};

struct IntroCountdownOverlayState {
  bool resolved = false;
  bool alphaSuppressed = false;
  uint32_t originalColor = 0;
  unsigned int callerOffset = 0;
  IntroHudElemSnapshot snap{};
  HudScriptCountdownMatch match{};
  std::string rawText;
  std::string sourceTag;
};

static bool IsIntroCountdownElemType(uint32_t type) {
  return type == 1 || (type >= 0x5 && type <= 0xC);
}

static bool LooksLikeIntroCountdownShape(uint32_t type, float x, float y,
                                         float fontScale, float timerValue,
                                         uint32_t horzAlign,
                                         uint32_t vertAlign) {
  if (!IsIntroCountdownElemType(type)) {
    return false;
  }
  if (!(fontScale >= 0.45f && fontScale <= 3.20f)) {
    return false;
  }
  if (!(timerValue > 0.05f && timerValue < 600.0f)) {
    return false;
  }

  const bool rightAnchoredUpper =
      ((horzAlign == 2 || horzAlign == 1) && x >= -80.0f && x <= 300.0f &&
       y >= -180.0f && y <= 180.0f);
  const bool upperRightVirtual =
      (x >= 100.0f && x <= 760.0f && y >= 0.0f && y <= 340.0f);
  const bool clockworkPowerdownLane =
      (horzAlign == 2 && vertAlign == 0 && x >= -340.0f && x <= -140.0f &&
       y >= 60.0f && y <= 140.0f && fontScale >= 1.20f &&
       fontScale <= 2.10f);
  return rightAnchoredUpper || upperRightVirtual || clockworkPowerdownLane;
}

static bool IsObjectiveReverseLane(const IntroHudElemSnapshot &snap) {
  return snap.type == 1 && snap.x > 5.5f && snap.x < 6.5f && snap.y >= 9.0f &&
         snap.y <= 91.0f && snap.fontScale > 1.20f && snap.fontScale < 1.30f;
}

static bool IsTopLeftBootstrapStatusKey(const std::string &key) {
  return key == "GAME_OBJECTIVESUPDATED" ||
         key == "GAME_OBJECTIVECOMPLETED" ||
         key == "GAME_OBJECTIVEFAILED";
}

static std::pair<DWORD, DWORD>
GetTopLeftBootstrapStatusTiming(const std::string &key) {
  if (key == "EXE_GAMESAVED" || key == "CGAME_NOW_SAVING") {
    return {3500u, 500u};
  }
  if (key == "GAME_OBJECTIVESUPDATED" || key == "GAME_OBJECTIVECOMPLETED") {
    return {3000u, 600u};
  }
  return {3000u, 500u};
}

static std::vector<std::string>
GetActiveTopLeftBootstrapStatusKeys(unsigned long now) {
  std::vector<ObjectiveStatusEvent> statusEvents =
      TextHook_GetObjectiveStatusEvents();
  struct ActiveStatusEvent {
    ObjectiveStatusEvent ev;
    DWORD startTick = 0;
    DWORD holdMs = 0;
    DWORD fadeMs = 0;
  };
  std::vector<ActiveStatusEvent> activeEvents;
  activeEvents.reserve(statusEvents.size());
  for (const auto &ev : statusEvents) {
    if (!IsTopLeftBootstrapStatusKey(ev.key)) {
      continue;
    }
    const auto timing = GetTopLeftBootstrapStatusTiming(ev.key);
    const DWORD holdMs = timing.first;
    const DWORD fadeMs = timing.second;
    const DWORD startTick = ev.triggerTick;
    const DWORD ageMs = (now >= startTick) ? (now - startTick) : 0;
    if (ageMs > holdMs + fadeMs) {
      continue;
    }
    ActiveStatusEvent aev{};
    aev.ev = ev;
    aev.startTick = startTick;
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
    std::unordered_map<std::string, size_t> latestIndexByKey;
    latestIndexByKey.reserve(activeEvents.size());
    for (size_t idx = 0; idx < activeEvents.size(); ++idx) {
      if (!activeEvents[idx].ev.key.empty()) {
        latestIndexByKey[activeEvents[idx].ev.key] = idx;
      }
    }
    std::vector<ActiveStatusEvent> deduped;
    deduped.reserve(activeEvents.size());
    for (size_t idx = 0; idx < activeEvents.size(); ++idx) {
      const auto &aev = activeEvents[idx];
      auto itLatest = latestIndexByKey.find(aev.ev.key);
      if (itLatest != latestIndexByKey.end() && itLatest->second != idx) {
        continue;
      }
      deduped.push_back(aev);
    }
    activeEvents.swap(deduped);
  }
  if (activeEvents.size() > 4) {
    activeEvents.erase(activeEvents.begin(),
                       activeEvents.begin() + (activeEvents.size() - 4));
  }

  std::vector<std::string> keys;
  keys.reserve(activeEvents.size());
  for (const auto &aev : activeEvents) {
    if (!aev.ev.key.empty()) {
      keys.push_back(aev.ev.key);
    }
  }
  return keys;
}

static bool TryReadIntroU32(uintptr_t base, uintptr_t off, uint32_t &out) {
  if (!(base > 0x10000 && base < 0x7FFFFFFFFFFF)) {
    return false;
  }
  if (IsBadReadPtr((void *)(base + off), sizeof(uint32_t)) != 0) {
    return false;
  }
  __try {
    out = *(volatile uint32_t *)(base + off);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool TryReadIntroU64(uintptr_t base, uintptr_t off, uintptr_t &out) {
  if (!(base > 0x10000 && base < 0x7FFFFFFFFFFF)) {
    return false;
  }
  if (IsBadReadPtr((void *)(base + off), sizeof(uintptr_t)) != 0) {
    return false;
  }
  __try {
    out = *(volatile uintptr_t *)(base + off);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool TryReadIntroFloat(uintptr_t base, uintptr_t off, float &out) {
  if (!(base > 0x10000 && base < 0x7FFFFFFFFFFF)) {
    return false;
  }
  if (IsBadReadPtr((void *)(base + off), sizeof(float)) != 0) {
    return false;
  }
  __try {
    out = *(volatile float *)(base + off);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool TryReadIntroByte(uintptr_t addr, unsigned char &out) {
  out = 0;
  if (!(addr > 0x10000 && addr < 0x7FFFFFFFFFFF)) {
    return false;
  }
  if (IsBadReadPtr((void *)addr, sizeof(unsigned char)) != 0) {
    return false;
  }
  __try {
    out = *(volatile unsigned char *)addr;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool TryWriteIntroByte(uintptr_t addr, unsigned char value) {
  if (!(addr > 0x10000 && addr < 0x7FFFFFFFFFFF)) {
    return false;
  }
  __try {
    DWORD oldProtect = 0;
    const BOOL protectOk = VirtualProtect((void *)addr, sizeof(unsigned char),
                                          PAGE_EXECUTE_READWRITE, &oldProtect);
    *(volatile unsigned char *)addr = value;
    if (protectOk) {
      DWORD restoreProtect = 0;
      VirtualProtect((void *)addr, sizeof(unsigned char), oldProtect,
                     &restoreProtect);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool TryWriteIntroU32(uintptr_t base, uintptr_t off, uint32_t value) {
  if (!(base > 0x10000 && base < 0x7FFFFFFFFFFF)) {
    return false;
  }
  if (IsBadWritePtr((void *)(base + off), sizeof(uint32_t)) != 0) {
    return false;
  }
  __try {
    *(volatile uint32_t *)(base + off) = value;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool IsIntroTopLeftStatusLane(const IntroHudElemSnapshot &snap,
                                     unsigned int callerOffset) {
  if (snap.type != 1) {
    return false;
  }
  if (callerOffset != IW6Offsets::Profile::Rva_2316DB) {
    return false;
  }
  if (snap.horzAlign != 0 || snap.vertAlign != 0) {
    return false;
  }
  if (!(snap.x >= -0.5f && snap.x <= 8.5f)) {
    return false;
  }
  if (!(snap.y >= 8.0f && snap.y <= 55.0f)) {
    return false;
  }
  if (!(snap.fontScale >= 1.18f && snap.fontScale <= 1.35f)) {
    return false;
  }
  return true;
}

static bool IsLikelyIntroCountdownFallbackLane(
    const IntroHudElemSnapshot &snap, unsigned int callerOffset) {
  if (!IsIntroCountdownElemType(snap.type)) {
    return false;
  }
  if (IsIntroTopLeftStatusLane(snap, callerOffset)) {
    return false;
  }
  if (!LooksLikeIntroCountdownShape(snap.type, snap.x, snap.y, snap.fontScale,
                                    snap.timerValue, snap.horzAlign,
                                    snap.vertAlign)) {
    return false;
  }
  if (callerOffset == IW6Offsets::Profile::Rva_2316DB || callerOffset == IW6Offsets::Profile::Rva_2B1453 ||
      callerOffset == IW6Offsets::Profile::Rva_232562) {
    return true;
  }
  return (snap.horzAlign == 2 || snap.x >= 180.0f);
}

static bool TryComputeIntroCountdownLayout(const IntroHudElemSnapshot &snap,
                                           float &outX, float &outY,
                                           float &outScale,
                                           float &outFontHeight) {
  outX = 0.0f;
  outY = 0.0f;
  outScale = 0.0f;
  outFontHeight = 0.0f;

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
      (snap.horzAlign == 2 && snap.vertAlign == 0 &&
       snap.x >= -340.0f && snap.x <= -140.0f && snap.y >= 60.0f &&
       snap.y <= 140.0f && snap.fontScale >= 1.20f &&
       snap.fontScale <= 2.10f);
  if (clockworkPowerdownLane) {
    // Clockwork powerdown uses a larger native HudElem fontScale than the
    // common countdown lanes; reusing the generic 78 * 1.3 path renders it
    // roughly 2x too large and spills into the number slot.
    outFontHeight = 60.0f;
    outScale = snap.fontScale * 0.85f;
  } else {
    outFontHeight = 78.0f;
    outScale = snap.fontScale * 1.3f;
  }
  return std::isfinite(outX) && std::isfinite(outY) &&
         std::isfinite(outScale) && outScale > 0.01f;
}

static bool TryResolveIntroHudScriptCountdown(
    const IntroHudElemSnapshot &snap, void *renderCtx,
    unsigned int callerOffset, HudScriptCountdownMatch &outMatch,
    std::string &outRawText, std::string &outSourceTag) {
  outMatch = {};
  outRawText.clear();
  outSourceTag.clear();

  const bool fallbackLane =
      IsLikelyIntroCountdownFallbackLane(snap, callerOffset);
  const bool snapLooksCountdown = LooksLikeIntroCountdownShape(
      snap.type, snap.x, snap.y, snap.fontScale, snap.timerValue,
      snap.horzAlign, snap.vertAlign);
  const bool allowDirectCountdownTextOnSnap = snapLooksCountdown || fallbackLane;

  auto tryCandidate = [&](const std::string &rawText,
                          const char *sourceTag) -> bool {
    if (rawText.empty()) {
      return false;
    }

    HudScriptCountdownMatch match;
    if (TryResolveHudScriptCountdownText(rawText, match)) {
      if (IsTimeScriptManagedCountdownKeyName(match.key)) {
        return false;
      }
      if (!allowDirectCountdownTextOnSnap) {
        return false;
      }
      outMatch = std::move(match);
      outRawText = rawText;
      outSourceTag = sourceTag ? sourceTag : "intro_unknown";
      return true;
    }

    return false;
  };

  std::string textPreview;
  if (snap.textPtr > 0x10000 && snap.textPtr < 0x7FFFFFFFFFFF &&
      TryReadCStringPreview((const char *)snap.textPtr, 160, textPreview) &&
      tryCandidate(textPreview, "intro_text_ptr")) {
    return true;
  }

  const uintptr_t ctx = (uintptr_t)renderCtx;
  if (ctx > 0x10000 && ctx < 0x7FFFFFFFFFFF) {
    for (int off = -32; off <= 96; ++off) {
      const uintptr_t p = ctx + (intptr_t)off;
      if (!(p > 0x10000 && p < 0x7FFFFFFFFFFF)) {
        continue;
      }
      std::string rawInline;
      if (TryReadCStringPreview((const char *)p, 160, rawInline) &&
          tryCandidate(rawInline, "intro_ctx_inline")) {
        return true;
      }
    }

    static const std::array<int, 8> kCtxPtrOffsets = {0x00, 0x08, 0x10, 0x18,
                                                       0x20, 0x28, 0x30, 0x38};
    for (int off : kCtxPtrOffsets) {
      uintptr_t value = 0;
      if (!TryReadIntroU64(ctx, (uintptr_t)off, value) ||
          !(value > 0x10000 && value < 0x7FFFFFFFFFFF)) {
        continue;
      }
      std::string rawPtr;
      if (TryReadCStringPreview((const char *)value, 160, rawPtr) &&
          tryCandidate(rawPtr, "intro_ctx_ptr")) {
        return true;
      }
    }
  }

  static const std::array<int, 5> kElemPtrOffsets = {0x40, 0x48, 0x50, 0x58,
                                                      0x60};
  for (int off : kElemPtrOffsets) {
    uintptr_t value = 0;
    if (!TryReadIntroU64(snap.rbx, (uintptr_t)off, value) ||
        !(value > 0x10000 && value < 0x7FFFFFFFFFFF)) {
      continue;
    }
    std::string rawPtr;
    if (TryReadCStringPreview((const char *)value, 160, rawPtr) &&
        tryCandidate(rawPtr, "intro_elem_ptr")) {
      return true;
    }
  }
  return false;
}

struct IntroActualRenderProbeHit {
  bool matched = false;
  bool keyOnly = false;
  bool tailOnly = false;
  HudScriptCountdownMatch match;
  std::string rawText;
  std::string key;
  std::string sourceTag;
  uintptr_t rootBase = 0;
  uintptr_t stringAddr = 0;
  int offset = 0;
  int score = -1;
};

static void ConsiderIntroActualRenderTextCandidate(
    const std::string &rawText, const std::string &sourceTag,
    uintptr_t rootBase, uintptr_t stringAddr, int offset, int baseScore,
    IntroActualRenderProbeHit &best) {
  if (rawText.empty()) {
    return;
  }

  IntroActualRenderProbeHit cand;
  cand.rawText = rawText;
  cand.sourceTag = sourceTag;
  cand.rootBase = rootBase;
  cand.stringAddr = stringAddr;
  cand.offset = offset;

  HudScriptCountdownMatch directMatch{};
  if (TryResolveHudScriptCountdownText(rawText, directMatch)) {
    cand.matched = true;
    cand.match = std::move(directMatch);
    cand.key = cand.match.key;
    cand.score = baseScore + 360;
  } else {
    std::string keyOnly;
    if (TryResolveHudScriptCountdownKeyFromRawText(rawText, keyOnly)) {
      cand.keyOnly = true;
      cand.key = std::move(keyOnly);
      cand.score = baseScore + 220;
    }
  }

  if (cand.score < 0) {
    return;
  }

  if (cand.score > best.score ||
      (cand.score == best.score && cand.rawText.size() > best.rawText.size())) {
    best = std::move(cand);
  }
}

static void ProbeIntroActualRenderRoot(const char *rootTag, uintptr_t rootBase,
                                       IntroActualRenderProbeHit &best) {
  if (!(rootBase > 0x10000 && rootBase < 0x7FFFFFFFFFFF)) {
    return;
  }

  const std::string rootPrefix = rootTag ? rootTag : "iar";
  std::string rawText;
  if (TryReadCStringPreview((const char *)rootBase, 160, rawText)) {
    ConsiderIntroActualRenderTextCandidate(rawText, rootPrefix + "_base",
                                           rootBase, rootBase, 0, 260, best);
  }

  static const std::array<int, 12> kInlineOffsets = {
      0x08, 0x10, 0x18, 0x20, 0x28, 0x30,
      0x38, 0x40, 0x48, 0x50, 0x60, 0x70};
  for (int off : kInlineOffsets) {
    const uintptr_t strAddr = rootBase + (uintptr_t)off;
    if (!(strAddr > 0x10000 && strAddr < 0x7FFFFFFFFFFF)) {
      continue;
    }
    rawText.clear();
    if (TryReadCStringPreview((const char *)strAddr, 160, rawText)) {
      ConsiderIntroActualRenderTextCandidate(rawText, rootPrefix + "_inline",
                                             rootBase, strAddr, off, 180, best);
    }
  }

  static const std::array<int, 16> kPtrOffsets = {
      0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38,
      0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78};
  for (int off : kPtrOffsets) {
    uintptr_t ptrValue = 0;
    if (!TryReadIntroU64(rootBase, (uintptr_t)off, ptrValue) ||
        !(ptrValue > 0x10000 && ptrValue < 0x7FFFFFFFFFFF)) {
      continue;
    }
    rawText.clear();
    if (TryReadCStringPreview((const char *)ptrValue, 160, rawText)) {
      ConsiderIntroActualRenderTextCandidate(rawText, rootPrefix + "_ptr",
                                             rootBase, ptrValue, off, 240, best);
    }
  }
}

static bool TryResolveIntroActualRenderProbe(int type, void *layoutResult,
                                             void *renderCtx, int count,
                                             IntroActualRenderProbeHit &outHit) {
  (void)type;
  outHit = IntroActualRenderProbeHit{};
  outHit.score = -1;

  if (count < 0 || count > 4096) {
    return false;
  }
  if ((uintptr_t)layoutResult <= 0x10000 && (uintptr_t)renderCtx <= 0x10000) {
    return false;
  }

  // NOTE: Probe gate REMOVED.  The previous 120ms gate caused countdown
  // timer suppression to fail on ~87% of frames (7 out of 8 at 60fps),
  // producing persistent ghosting/flickering of the English label that
  // could not be eliminated.  Probing every IAR call is cheap (~58 memory
  // reads per call, <30 calls per frame) and eliminates the flicker.

  ProbeIntroActualRenderRoot("iar_layout", (uintptr_t)layoutResult, outHit);
  ProbeIntroActualRenderRoot("iar_ctx", (uintptr_t)renderCtx, outHit);
  return outHit.score >= 0;
}

static void LogIntroActualRenderProbeHit(int type, void *layoutResult,
                                         void *renderCtx, int count,
                                         const IntroActualRenderProbeHit &hit) {
  if (hit.score < 0) {
    return;
  }

  const DWORD now = GetTickCount();
  static std::unordered_map<std::string, DWORD> s_logTick;
  std::string sig = std::to_string(type) + "|" + std::to_string(count) + "|" +
                    hit.sourceTag + "|" + hit.key + "|" + hit.rawText;
  if (sig.size() > 224) {
    sig.resize(224);
  }
  auto it = s_logTick.find(sig);
  if (it != s_logTick.end() && (now - it->second) <= 900) {
    return;
  }
  s_logTick[sig] = now;

  auto readRoot = [](uintptr_t base, uintptr_t (&qwords)[2], float (&floats)[2]) {
    qwords[0] = 0;
    qwords[1] = 0;
    floats[0] = 0.0f;
    floats[1] = 0.0f;
    if (!(base > 0x10000 && base < 0x7FFFFFFFFFFF)) {
      return;
    }
    TryReadIntroU64(base, 0x00, qwords[0]);
    TryReadIntroU64(base, 0x08, qwords[1]);
    TryReadIntroFloat(base, 0x00, floats[0]);
    TryReadIntroFloat(base, 0x04, floats[1]);
  };

  uintptr_t layoutQ[2] = {};
  uintptr_t renderQ[2] = {};
  float layoutF[2] = {};
  float renderF[2] = {};
  readRoot((uintptr_t)layoutResult, layoutQ, layoutF);
  readRoot((uintptr_t)renderCtx, renderQ, renderF);

  const char *tag = hit.matched ? "[IAR-TIMER]" : "[IAR-CAND]";
  const std::string key =
      !hit.key.empty() ? hit.key : (hit.match.key.empty() ? "-" : hit.match.key);
  const std::string renderText =
      hit.matched ? Utf8Preview(hit.match.renderText, 120) : std::string();

  char buf[1400];
  sprintf_s(
      buf,
      "%s type=%d count=%d src=%s score=%d match=%d keyOnly=%d tail=%d key=%s "
      "root=0x%llX str=0x%llX off=0x%X layout=0x%llX ctx=0x%llX raw=\"%.120s\" "
      "render=\"%.120s\" lq0=0x%llX lq8=0x%llX cq0=0x%llX cq8=0x%llX "
      "lf0=%.3f lf4=%.3f cf0=%.3f cf4=%.3f",
      tag, type, count, hit.sourceTag.c_str(), hit.score, hit.matched ? 1 : 0,
      hit.keyOnly ? 1 : 0, hit.tailOnly ? 1 : 0, key.c_str(),
      (unsigned long long)hit.rootBase, (unsigned long long)hit.stringAddr,
      hit.offset, (unsigned long long)(uintptr_t)layoutResult,
      (unsigned long long)(uintptr_t)renderCtx,
      Utf8Preview(hit.rawText, 120).c_str(), renderText.c_str(),
      (unsigned long long)layoutQ[0], (unsigned long long)layoutQ[1],
      (unsigned long long)renderQ[0], (unsigned long long)renderQ[1], layoutF[0],
      layoutF[1], renderF[0], renderF[1]);
  LogToFile(buf);
}

struct IntroActualRenderPrefixOverlay {
  bool ready = false;
  int slcIdx = -1;
  HudScriptCountdownMatch match;
  float x = 0.0f;
  float y = 0.0f;
  float scale = 1.0f;
  float fontHeight = 42.0f;
  int forceAlign = 0;
  bool yIsTopOfText = false;
  float color[4] = {0.8f, 1.0f, 0.8f, 1.0f};
  uint32_t colorPacked = 0xFFFFFFFFu;
};

static bool TryPrepareIntroActualPrefixOverlay(
    const IntroActualRenderProbeHit &hit,
    IntroActualRenderPrefixOverlay &outOverlay) {
  outOverlay = IntroActualRenderPrefixOverlay{};
  if (!hit.keyOnly || hit.matched || hit.key.empty() || hit.rawText.empty()) {
    return false;
  }
  if (TextHook_IsPauseMenuLikely()) {
    return false;
  }

  EnsureTranslationsLoaded();
  auto itEng = g_KeyToEnglish.find(hit.key);
  auto itKor = g_KeyToKorean.find(hit.key);
  if (itEng == g_KeyToEnglish.end() || itKor == g_KeyToKorean.end() ||
      itEng->second.empty() || itKor->second.empty()) {
    return false;
  }

  const std::string rawNorm = ToUpperAscii(TrimSpaces(StripColorCodes(hit.rawText)));
  const std::string engNorm =
      ToUpperAscii(TrimSpaces(StripColorCodes(itEng->second)));
  if (rawNorm.empty() || engNorm.empty() || rawNorm != engNorm) {
    return false;
  }

  const int activeSlc =
      (g_currentIntroSlcIdx >= 0) ? g_currentIntroSlcIdx : g_lastIntroSlcIdx;
  IntroHudElemSnapshot snap{};
  bool haveLayoutSource = false;
  NativeIntroRenderState nativeState{};
  if (activeSlc >= 0 && TextHook_GetNativeIntroRender(activeSlc, nativeState) &&
      nativeState.valid) {
    snap.x = nativeState.x;
    snap.y = nativeState.y;
    snap.fontScale = nativeState.fontScale;
    snap.horzAlign = nativeState.horzAlign;
    snap.vertAlign = nativeState.vertAlign;
    outOverlay.color[0] =
        (float)((nativeState.colorPacked >> 0) & 0xFF) / 255.0f;
    outOverlay.color[1] =
        (float)((nativeState.colorPacked >> 8) & 0xFF) / 255.0f;
    outOverlay.color[2] =
        (float)((nativeState.colorPacked >> 16) & 0xFF) / 255.0f;
    outOverlay.color[3] =
        (float)((nativeState.colorPacked >> 24) & 0xFF) / 255.0f;
    outOverlay.colorPacked = nativeState.colorPacked;
    haveLayoutSource = true;
  }
  if (!haveLayoutSource) {
    return false;
  }

  if (!TryComputeIntroCountdownLayout(snap, outOverlay.x, outOverlay.y,
                                      outOverlay.scale,
                                      outOverlay.fontHeight)) {
    return false;
  }

  if (snap.horzAlign == 2) {
    outOverlay.forceAlign = 1;
  } else if (snap.horzAlign == 1) {
    outOverlay.forceAlign = 2;
  } else {
    outOverlay.forceAlign = 0;
  }
  outOverlay.yIsTopOfText = true;

  outOverlay.match.key = hit.key;
  outOverlay.match.englishPrefix = hit.rawText;
  outOverlay.match.koreanPrefix = itKor->second;
  outOverlay.match.tail.clear();
  outOverlay.match.renderText = itKor->second;
  outOverlay.slcIdx = (activeSlc >= 0) ? activeSlc : 0;
  if (outOverlay.color[3] <= 0.01f) {
    outOverlay.color[3] = 1.0f;
  }

  const float engWidth = KoreanRenderer::MeasureTextWidthEx(
      hit.rawText, outOverlay.fontHeight, outOverlay.scale, 1);
  const float korWidth = KoreanRenderer::MeasureTextWidthEx(
      outOverlay.match.renderText, outOverlay.fontHeight, outOverlay.scale, 1);
  if (engWidth > 1.0f && korWidth > engWidth * 0.98f) {
    const float fit = (engWidth / korWidth) * 0.96f;
    if (std::isfinite(fit) && fit > 0.35f && fit < 1.0f) {
      outOverlay.scale *= fit;
    }
  }

  outOverlay.ready = true;
  return true;
}

static void LogObjectiveReverseProbe(const IntroHudElemSnapshot &snap,
                                     unsigned int callerOffset,
                                     int detourType,
                                     void *renderCtx,
                                     int count) {
  if (!kVerboseRuntimeLogs) {
    return;
  }
  if (!IsObjectiveReverseLane(snap)) {
    return;
  }

  const unsigned long now = GetTickCount();
  const uint64_t probeSig =
      (((uint64_t)callerOffset & 0xFFFFFFull) << 40) ^
      (((uint64_t)(uint32_t)lroundf(snap.y) & 0xFFull) << 32) ^
      (((uint64_t)snap.textSlc & 0xFFFFull) << 8) ^
      (uint64_t)(snap.flags & 0xFF);
  static std::unordered_map<uint64_t, unsigned long> s_probeLogTick;
  auto itProbe = s_probeLogTick.find(probeSig);
  if (itProbe != s_probeLogTick.end() && (now - itProbe->second) <= 1500) {
    return;
  }
  s_probeLogTick[probeSig] = now;

  const int rawElemOffsets[] = {0x70, 0x74, 0x78, 0x7C, 0x80, 0x84, 0x88,
                                0x8C, 0x90, 0x94, 0x98, 0x9C, 0xA0, 0xA4};
  constexpr size_t kRawElemCount = sizeof(rawElemOffsets) / sizeof(rawElemOffsets[0]);
  uint32_t rawElem[kRawElemCount] = {};
  for (size_t i = 0; i < kRawElemCount; ++i) {
    TryReadIntroU32(snap.rbx, (uintptr_t)rawElemOffsets[i], rawElem[i]);
  }

  uintptr_t ctx = (uintptr_t)renderCtx;
  uintptr_t ctxQ[4] = {};
  if (ctx > 0x10000 && ctx < 0x7FFFFFFFFFFF) {
    TryReadIntroU64(ctx, 0x00, ctxQ[0]);
    TryReadIntroU64(ctx, 0x08, ctxQ[1]);
    TryReadIntroU64(ctx, 0x10, ctxQ[2]);
    TryReadIntroU64(ctx, 0x18, ctxQ[3]);
  }

  char rawBuf[768];
  sprintf_s(
      rawBuf,
      "[OBJ-REVERSE-PROBE] ptr=0x%llX caller=+0x%X argType=%d count=%d ctx=0x%llX "
      "o70=0x%X o74=0x%X o78=0x%X o7C=0x%X o80=0x%X o84=0x%X o88=0x%X o8C=0x%X "
      "o90=0x%X o94=0x%X o98=0x%X o9C=0x%X oA0=0x%X oA4=0x%X "
      "ctx0=0x%llX ctx8=0x%llX ctx10=0x%llX ctx18=0x%llX",
      (unsigned long long)snap.rbx, callerOffset, detourType, count,
      (unsigned long long)ctx, rawElem[0], rawElem[1], rawElem[2], rawElem[3],
      rawElem[4], rawElem[5], rawElem[6], rawElem[7], rawElem[8], rawElem[9],
      rawElem[10], rawElem[11], rawElem[12], rawElem[13],
      (unsigned long long)ctxQ[0], (unsigned long long)ctxQ[1],
      (unsigned long long)ctxQ[2], (unsigned long long)ctxQ[3]);
  LogToFile(rawBuf);

  const int candidateOffsets[] = {0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58,
                                  0x5C, 0x60, 0x64, 0x68, 0x6C, 0x70, 0x74,
                                  0x78, 0x7C, 0x80, 0x84, 0x88, 0x8C, 0x90,
                                  0x94, 0x98, 0x9C, 0xA0, 0xA4};
  size_t cfgHitCount = 0;
  for (int off : candidateOffsets) {
    if (cfgHitCount >= 6) {
      break;
    }
    uint32_t value = 0;
    if (!TryReadIntroU32(snap.rbx, (uintptr_t)off, value) || value == 0) {
      continue;
    }
    if (!ObjRev_IsPlausibleConfigIndex(value)) {
      continue;
    }
    std::string resolved = ObjRev_ResolveConfigKey(value, true, true);
    if (resolved.empty()) {
      resolved = ObjRev_ResolveConfigKey(value, true, false);
    }
    if (resolved.empty()) {
      continue;
    }
    char hitBuf[320];
    sprintf_s(hitBuf,
              "[OBJ-REVERSE-CFGHIT] ptr=0x%llX caller=+0x%X off=0x%X val=0x%X key=%s",
              (unsigned long long)snap.rbx, callerOffset, off, value,
              resolved.c_str());
    LogToFile(hitBuf);
    ++cfgHitCount;
  }

  auto logPointerStrings = [&](const char *tag, uintptr_t base,
                               std::initializer_list<int> offsets) {
    size_t ptrHitCount = 0;
    for (int off : offsets) {
      if (ptrHitCount >= 4) {
        break;
      }
      uintptr_t value = 0;
      if (!TryReadIntroU64(base, (uintptr_t)off, value) || value <= 0x10000 ||
          value >= 0x7FFFFFFFFFFF) {
        continue;
      }
      std::string preview;
      if (!TryReadCStringPreview((const char *)value, 96, preview) ||
          preview.empty()) {
        continue;
      }
      if (preview.size() > 96) {
        preview = Utf8Preview(preview, 96);
      }
      char pbuf[352];
      sprintf_s(pbuf,
                "[OBJ-REVERSE-PTR] ptr=0x%llX caller=+0x%X src=%s off=0x%X q=0x%llX text=\"%s\"",
                (unsigned long long)snap.rbx, callerOffset, tag, off,
                (unsigned long long)value, preview.c_str());
      LogToFile(pbuf);
      ++ptrHitCount;
    }
  };

  logPointerStrings("elem", snap.rbx, {0x40, 0x48, 0x50, 0x58, 0x60, 0x68,
                                       0x70, 0x78, 0x80, 0x88, 0x90, 0x98});
  if (ctx > 0x10000 && ctx < 0x7FFFFFFFFFFF) {
    logPointerStrings("ctx", ctx, {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30,
                                   0x38, 0x40});
  }
}

static bool TryResolveObjectiveRenderCtxText(void *renderCtx,
                                             std::string &outRawText,
                                             std::string &outKey,
                                             int &outOffset) {
  outRawText.clear();
  outKey.clear();
  outOffset = 0;

  const uintptr_t ctx = (uintptr_t)renderCtx;
  if (!(ctx > 0x10000 && ctx < 0x7FFFFFFFFFFF)) {
    return false;
  }

  struct Candidate {
    std::string rawText;
    std::string key;
    int offset = 0;
    int score = 0;
  };

  auto normalizeLoose = [](std::string s) -> std::string {
    s = TrimSpaces(StripColorCodes(s));
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
      if (std::isspace(ch)) {
        continue;
      }
      if (ch == '.' || ch == '!' || ch == '?' || ch == ',' || ch == ':' ||
          ch == '\'' || ch == '"' || ch == '^') {
        continue;
      }
      out.push_back((char)std::tolower(ch));
    }
    return out;
  };

  auto resolveObjectiveFragmentKey = [&](const std::string &rawText,
                                         std::string &resolvedKey) -> bool {
    resolvedKey.clear();
    const std::string targetNorm = normalizeLoose(rawText);
    if (targetNorm.size() < 8) {
      return false;
    }

    size_t bestScore = 0;
    auto consider = [&](const std::string &candidateKey,
                        const std::string &candidateText) {
      if (candidateText.empty()) {
        return;
      }
      if (!TextHook_OSRev_IsObjectiveStatusKey(candidateKey)) {
        return;
      }
      const std::string candidateNorm = normalizeLoose(candidateText);
      if (candidateNorm.size() < 8) {
        return;
      }
      if (candidateNorm.find(targetNorm) == std::string::npos &&
          targetNorm.find(candidateNorm) == std::string::npos) {
        return;
      }
      const size_t score =
          (candidateNorm.size() < targetNorm.size()) ? candidateNorm.size()
                                                     : targetNorm.size();
      if (score > bestScore) {
        bestScore = score;
        resolvedKey = candidateKey;
      }
    };

    EnsureTranslationsLoaded();
    for (const auto &kv : g_KeyToEnglish) {
      consider(kv.first, kv.second);
      auto itKor = g_KeyToKorean.find(kv.first);
      if (itKor != g_KeyToKorean.end()) {
        consider(kv.first, itKor->second);
      }
    }
    return !resolvedKey.empty();
  };

  Candidate best;
  for (int off = -24; off <= 48; ++off) {
    const uintptr_t p = ctx + (intptr_t)off;
    if (!(p > 0x10000 && p < 0x7FFFFFFFFFFF)) {
      continue;
    }

    std::string rawText;
    if (!TryReadCStringPreview((const char *)p, 160, rawText) || rawText.empty()) {
      continue;
    }

    int score = (int)rawText.size();
    const std::string rawLeadUpper =
        ToUpperAscii(TrimSpaces(StripColorCodes(rawText)));
    if (rawLeadUpper.rfind("PRESS ", 0) == 0 ||
        rawLeadUpper.rfind("HOLD ", 0) == 0 ||
        rawLeadUpper.rfind("TAP ", 0) == 0 ||
        rawLeadUpper.rfind("USE ", 0) == 0 ||
        rawLeadUpper.rfind("TIME TO ", 0) == 0) {
      score += 18;
    }
    if (rawLeadUpper.rfind("RESS ", 0) == 0 ||
        rawLeadUpper.rfind("OLD ", 0) == 0 ||
        rawLeadUpper.rfind("AP ", 0) == 0 ||
        rawLeadUpper.rfind("SE ", 0) == 0 ||
        rawLeadUpper.rfind("IME TO ", 0) == 0) {
      score -= 18;
    }
    std::string key;
    const bool resolvedKey =
        TextHook_OSRev_ResolveKeyFromText(rawText, key) ||
        resolveObjectiveFragmentKey(rawText, key);
    if (!resolvedKey) {
      if (TextHook_OSRev_LooksLikePromptFragment(rawText) ||
          rawText.size() < 12) {
        continue;
      }
      score -= 6;
    }
    if (IsObjectiveStatusMessageKeyName(key)) {
      score += 12;
    }
    if (TextHook_OSRev_IsNativeObjectiveCandidateKey(key)) {
      score += 14;
    }
    if (off <= 0) {
      score += 4;
    } else if (off <= 8) {
      score += 2;
    }

    if (score > best.score) {
      best.rawText = rawText;
      best.key = key;
      best.offset = off;
      best.score = score;
    }
  }

  if (best.score <= 0 || best.rawText.empty()) {
    return false;
  }

  outRawText = best.rawText;
  outKey = best.key;
  outOffset = best.offset;
  return true;
}

static bool TryBuildIntroHudElemSnapshotFromPtr(uintptr_t rbx, int slcIndex,
                                                IntroHudElemSnapshot &outSnap,
                                                int &outScore,
                                                std::string *outPreview =
                                                    nullptr) {
  outSnap = IntroHudElemSnapshot{};
  outScore = -1;

  if (!(rbx > 0x10000 && rbx < 0x7FFFFFFFFFFF)) {
    return false;
  }
  if (IsBadReadPtr((void *)rbx, 0xA8) != 0) {
    return false;
  }

  uint32_t elemType = 0;
  float elemX = 0.0f;
  float elemY = 0.0f;
  float elemFontScale = 0.0f;
  float elemTimerValue = 0.0f;
  uint32_t elemHorzAlign = 0;
  uint32_t elemVertAlign = 0;
  uint32_t elemColor = 0;
  uintptr_t elemTextPtr = 0;
  uint32_t elemLabelSlc = 0;
  uint32_t elemTextSlc = 0;
  uint32_t elemTimeU32 = 0;
  uint32_t elemFxBirthU32 = 0;
  uint32_t elemFxLetterU32 = 0;
  uint32_t elemFlagsU32 = 0;

  if (!TryReadIntroU32(rbx, 0x00, elemType) ||
      !TryReadIntroFloat(rbx, 0x04, elemX) ||
      !TryReadIntroFloat(rbx, 0x08, elemY) ||
      !TryReadIntroFloat(rbx, 0x14, elemFontScale) ||
      !TryReadIntroFloat(rbx, 0x1C, elemTimerValue) ||
      !TryReadIntroU32(rbx, 0x24, elemHorzAlign) ||
      !TryReadIntroU32(rbx, 0x28, elemVertAlign) ||
      !TryReadIntroU32(rbx, 0x30, elemColor) ||
      !TryReadIntroU64(rbx, 0x50, elemTextPtr) ||
      !TryReadIntroU32(rbx, 0x78, elemTimeU32) ||
      !TryReadIntroU32(rbx, 0x80, elemLabelSlc) ||
      !TryReadIntroU32(rbx, 0x84, elemTextSlc) ||
      !TryReadIntroU32(rbx, 0x90, elemFxBirthU32) ||
      !TryReadIntroU32(rbx, 0x94, elemFxLetterU32) ||
      !TryReadIntroU32(rbx, 0xA4, elemFlagsU32)) {
    return false;
  }

  const int elemTime = (int)elemTimeU32;
  const int elemFxBirthTime = (int)elemFxBirthU32;
  const int elemFxLetterTime = (int)elemFxLetterU32;
  const int elemFlags = (int)elemFlagsU32;

  if (!std::isfinite(elemX) || !std::isfinite(elemY) ||
      !std::isfinite(elemFontScale) || !std::isfinite(elemTimerValue)) {
    return false;
  }
  if (elemType > 0x20 || elemFontScale <= 0.01f || elemFontScale > 6.0f ||
      elemX < -4096.0f || elemX > 4096.0f || elemY < -4096.0f ||
      elemY > 4096.0f || elemHorzAlign > 5 || elemVertAlign > 5) {
    return false;
  }

  const bool slcMatches = (elemTextSlc == (uint32_t)slcIndex);
  const bool looksCountdown = LooksLikeIntroCountdownShape(
      elemType, elemX, elemY, elemFontScale, elemTimerValue, elemHorzAlign,
      elemVertAlign);

  std::string preview;
  if (elemTextPtr > 0x10000 && elemTextPtr < 0x7FFFFFFFFFFF) {
    TryReadCStringPreview((const char *)elemTextPtr, 160, preview);
  }

  bool previewLooksCountdown = false;
  HudScriptCountdownMatch previewMatch{};
  if (!preview.empty() && TryResolveHudScriptCountdownText(preview, previewMatch)) {
    previewLooksCountdown = true;
  }

  std::string cfgCountdownKey;
  const bool cfgLooksCountdown =
      TryResolveHudScriptCountdownKeyFromCfg(elemTextSlc, cfgCountdownKey) ||
      TryResolveHudScriptCountdownKeyFromCfg(elemLabelSlc, cfgCountdownKey);

  const std::string previewNorm =
      ToUpperAscii(TrimSpaces(StripColorCodes(preview)));
  const bool previewHasClockworkPowerdown =
      (previewNorm.find("POWERDOWN IN") != std::string::npos);

  int score = 0;
  if (slcMatches) {
    score += 120;
  }
  if (looksCountdown) {
    score += 180;
  }
  if (previewLooksCountdown) {
    score += 180;
  }
  if (cfgLooksCountdown) {
    score += 90;
  }
  if (previewHasClockworkPowerdown) {
    score += 220;
  }
  if (IsIntroCountdownElemType(elemType)) {
    score += 16;
  }
  if (elemTimerValue > 0.05f && elemTimerValue < 600.0f) {
    score += 18;
  }
  if (elemHorzAlign == 2 && elemVertAlign == 0) {
    score += 24;
  }

  const bool acceptable =
      slcMatches || looksCountdown || previewLooksCountdown ||
      cfgLooksCountdown || previewHasClockworkPowerdown;
  if (!acceptable) {
    return false;
  }

  outSnap.rbx = rbx;
  outSnap.type = elemType;
  outSnap.x = elemX;
  outSnap.y = elemY;
  outSnap.fontScale = elemFontScale;
  outSnap.timerValue = elemTimerValue;
  outSnap.horzAlign = elemHorzAlign;
  outSnap.vertAlign = elemVertAlign;
  outSnap.colorPacked = elemColor;
  outSnap.textPtr = elemTextPtr;
  outSnap.elemTime = elemTime;
  outSnap.fxBirthTime = elemFxBirthTime;
  outSnap.fxLetterTime = elemFxLetterTime;
  outSnap.flags = elemFlags;
  outSnap.labelSlc = elemLabelSlc;
  outSnap.textSlc = elemTextSlc;
  outScore = score;
  if (outPreview) {
    *outPreview = std::move(preview);
  }
  return true;
}

// Keep SEH isolated from C++ objects.
static bool TryReadIntroHudElemSnapshot(uintptr_t stackBase, int slcIndex,
                                        IntroHudElemSnapshot &outSnap) {
  if (!(stackBase > 0x10000 && stackBase < 0x7FFFFFFFFFFF)) {
    return false;
  }

  IntroHudElemSnapshot bestSnap{};
  std::string bestPreview;
  intptr_t bestSlotOff = 0;
  int fixedScore = -1;
  if (TryBuildIntroHudElemSnapshotFromPtr(
          *(volatile uintptr_t *)(stackBase + 0x78), slcIndex, bestSnap,
          fixedScore, &bestPreview)) {
    bestSlotOff = 0x78;
  }

  int bestScore = fixedScore;


  for (intptr_t slotOff = -0x40; slotOff <= 0x180;
       slotOff += (intptr_t)sizeof(uintptr_t)) {
    if (slotOff == 0x78) {
      continue;
    }
    const uintptr_t slotAddr = (uintptr_t)((intptr_t)stackBase + slotOff);
    if (!(slotAddr > 0x10000 && slotAddr < 0x7FFFFFFFFFFF)) {
      continue;
    }
    if (IsBadReadPtr((void *)slotAddr, sizeof(uintptr_t)) != 0) {
      continue;
    }

    uintptr_t candPtr = 0;
    if (!TryReadIntroU64(slotAddr, 0, candPtr)) {
      continue;
    }

    IntroHudElemSnapshot candSnap{};
    std::string candPreview;
    int candScore = -1;
    if (!TryBuildIntroHudElemSnapshotFromPtr(candPtr, slcIndex, candSnap,
                                             candScore, &candPreview)) {
      continue;
    }

    const bool candLooksCountdown = LooksLikeIntroCountdownShape(
        candSnap.type, candSnap.x, candSnap.y, candSnap.fontScale,
        candSnap.timerValue, candSnap.horzAlign, candSnap.vertAlign);
    const std::string candPreviewNorm =
        ToUpperAscii(TrimSpaces(StripColorCodes(candPreview)));
    const bool candHasClockworkPowerdown =
        (candPreviewNorm.find("POWERDOWN IN") != std::string::npos);
    if (candLooksCountdown || candHasClockworkPowerdown) {
#if GHOSTSKOR_RUNTIME_DIAG
      static std::unordered_map<uint64_t, DWORD> s_introStackCandLogTick;
      const DWORD now = GetTickCount();
      const uint64_t candSig =
          (((uint64_t)(uint32_t)slcIndex) << 40) ^
          (((uint64_t)(uint32_t)candSnap.textSlc) << 16) ^
          ((uint64_t)(uint16_t)(slotOff & 0xFFFF)) ^
          ((uint64_t)(candHasClockworkPowerdown ? 1u : 0u) << 56);
      auto itCand = s_introStackCandLogTick.find(candSig);
      if (itCand == s_introStackCandLogTick.end() ||
          (now - itCand->second) > 1200) {
        s_introStackCandLogTick[candSig] = now;
        char cbuf[768];
        sprintf_s(
            cbuf,
            "[INTRO-TIMER-STACK-CAND] slc=0x%X slot=%+lld score=%d ptr=0x%llX type=%u x=%.1f y=%.1f align=%u/%u fs=%.2f timer=%.3f text=0x%X label=0x%X preview=\"%.120s\"",
            (unsigned int)slcIndex, (long long)slotOff, candScore,
            (unsigned long long)candSnap.rbx, candSnap.type, candSnap.x,
            candSnap.y, candSnap.horzAlign, candSnap.vertAlign,
            candSnap.fontScale, candSnap.timerValue, candSnap.textSlc,
            candSnap.labelSlc, Utf8Preview(candPreview, 120).c_str());
        LogToFile(cbuf);
      }
#endif
    }

    if (candScore > bestScore) {
      bestScore = candScore;
      bestSnap = candSnap;
      bestPreview = std::move(candPreview);
      bestSlotOff = slotOff;
    }
  }

  if (bestScore < 0) {
    return false;
  }

  outSnap = bestSnap;

#if GHOSTSKOR_RUNTIME_DIAG
  static std::unordered_map<uint64_t, DWORD> s_introStackScanLogTick;
  const DWORD now = GetTickCount();
  const uint64_t sig =
      (((uint64_t)(uint32_t)slcIndex) << 32) ^
      (uint64_t)((uint32_t)bestSnap.textSlc ^ (uint32_t)bestSnap.labelSlc);
  auto it = s_introStackScanLogTick.find(sig);
  if (it == s_introStackScanLogTick.end() || (now - it->second) > 1200) {
    s_introStackScanLogTick[sig] = now;
    char sbuf[768];
    sprintf_s(
        sbuf,
        "[INTRO-TIMER-STACK] slc=0x%X slot=%+lld score=%d ptr=0x%llX type=%u x=%.1f y=%.1f align=%u/%u fs=%.2f timer=%.3f textPtr=0x%llX preview=\"%.120s\"",
        (unsigned int)slcIndex, (long long)bestSlotOff, bestScore,
        (unsigned long long)bestSnap.rbx, bestSnap.type, bestSnap.x, bestSnap.y,
        bestSnap.horzAlign, bestSnap.vertAlign, bestSnap.fontScale,
        bestSnap.timerValue, (unsigned long long)bestSnap.textPtr,
        Utf8Preview(bestPreview, 120).c_str());
    LogToFile(sbuf);
  }
#endif

  return true;
}

void Detour_IntroActualRender(int type, void *layoutResult, void *renderCtx,
                              int count) {

  IntroActualRenderProbeHit probeHit;
  IntroActualRenderPrefixOverlay prefixOverlay;
  bool skipOriginal = false;
  bool queuePrefix = false;
  // PERF: only probe when a countdown timer is active.
  // Without an active timer, the 58-read × 2-root memory scan is pure waste.
  const bool needIARProbe = g_TimeScriptPersistSuppress.active;
  if (needIARProbe &&
      TryResolveIntroActualRenderProbe(type, layoutResult, renderCtx, count,
                                       probeHit)) {
    LogIntroActualRenderProbeHit(type, layoutResult, renderCtx, count,
                                 probeHit);
    if (IsTimeScriptManagedCountdownKeyName(probeHit.key)) {
      // D3D11 overlay renders the full Korean countdown text.
      // Skip original IAR to suppress English rendering.
      skipOriginal = true;
      const int activeSlc =
          (g_currentIntroSlcIdx >= 0) ? g_currentIntroSlcIdx : g_lastIntroSlcIdx;
      TextHook_TimeScriptOnIntroActualRenderCandidate(
          probeHit.key.c_str(), probeHit.rawText.c_str(),
          probeHit.sourceTag.c_str(), probeHit.stringAddr, probeHit.rootBase,
          probeHit.score, activeSlc);
    } else if (TryPrepareIntroActualPrefixOverlay(probeHit, prefixOverlay)) {
        // Non-clockwork prefix-only IAR hits are standalone label draws.
        skipOriginal = true;

        static std::mutex s_prefixQueueMtx;
        static std::unordered_map<uint64_t, DWORD> s_prefixQueueTick;
        const DWORD nowQueue = GetTickCount();
        const uint64_t keyHash =
            (uint64_t)(std::hash<std::string>{}(prefixOverlay.match.key) &
                       0xFFFFFFFFull);
        const uint64_t queueSig =
            (((uint64_t)(uint32_t)prefixOverlay.slcIdx) << 32) ^ keyHash;
        {
          std::lock_guard<std::mutex> lock(s_prefixQueueMtx);
          auto itQueue = s_prefixQueueTick.find(queueSig);
          if (itQueue == s_prefixQueueTick.end() ||
              (nowQueue - itQueue->second) > 4) {
            s_prefixQueueTick[queueSig] = nowQueue;
            queuePrefix = true;
          }
          if (s_prefixQueueTick.size() > 64) {
            for (auto it = s_prefixQueueTick.begin();
                 it != s_prefixQueueTick.end();) {
              if ((nowQueue - it->second) > 2000) {
                it = s_prefixQueueTick.erase(it);
              } else {
                ++it;
              }
            }
          }
        }
    }
  }

  if (!skipOriginal && Original_IntroActualRender) {
    Original_IntroActualRender(type, layoutResult, renderCtx, count);
  }

  if (prefixOverlay.ready && queuePrefix) {
    QueueHudScriptCountdownOverlay(prefixOverlay.match, prefixOverlay.x,
                                   prefixOverlay.y, prefixOverlay.scale,
                                   prefixOverlay.fontHeight, 0,
                                   prefixOverlay.color,
                                   prefixOverlay.yIsTopOfText, IW6Offsets::IntroActualRender_SP,
                                   "IAR-PREFIX", prefixOverlay.forceAlign);

    static std::unordered_map<uint64_t, DWORD> s_prefixLogTick;
    const DWORD now = GetTickCount();
    const uint64_t keyHash =
        (uint64_t)(std::hash<std::string>{}(prefixOverlay.match.key) &
                   0xFFFFFFFFull);
    const uint64_t sig =
        ((uint64_t)(uint32_t)prefixOverlay.slcIdx << 32) ^ keyHash;
    auto it = s_prefixLogTick.find(sig);
    if (it == s_prefixLogTick.end() || (now - it->second) > 900) {
      s_prefixLogTick[sig] = now;
      char pbuf[640];
      sprintf_s(
          pbuf,
          "[IAR-PREFIX-QUEUE] slc=0x%X x=%.1f y=%.1f scale=%.3f fontH=%.1f "
          "align=%d top=%d skip=1 raw=\"%.96s\" kor=\"%.96s\"",
          (unsigned int)prefixOverlay.slcIdx, prefixOverlay.x, prefixOverlay.y,
          prefixOverlay.scale, prefixOverlay.fontHeight,
          prefixOverlay.forceAlign, prefixOverlay.yIsTopOfText ? 1 : 0,
          probeHit.rawText.c_str(), prefixOverlay.match.renderText.c_str());
      LogToFile(pbuf);
    }
  }
}

static bool TryBuildIntroDetourSharedContext(uintptr_t stackBase,
                                             uintptr_t retAddr, int type,
                                             int slcIndex, void *renderCtx,
                                             int count,
                                             IntroDetourSharedContext &outCtx) {
  outCtx = IntroDetourSharedContext{};
  if (!TryReadIntroHudElemSnapshot(stackBase, slcIndex, outCtx.snap)) {
    return false;
  }
  const uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  outCtx.callerOffset =
      (retAddr >= moduleBase) ? (unsigned int)(retAddr - moduleBase) : 0u;
  outCtx.type = type;
  outCtx.slcIndex = slcIndex;
  outCtx.renderCtx = renderCtx;
  outCtx.count = count;
  return true;
}

static void ProcessIntroCountdownSnapshot(
    const IntroDetourSharedContext &ctx, IntroCountdownOverlayState &outState) {
  outState = IntroCountdownOverlayState{};
  outState.callerOffset = ctx.callerOffset;

  outState.resolved = TryResolveIntroHudScriptCountdown(
      ctx.snap, ctx.renderCtx, ctx.callerOffset, outState.match, outState.rawText,
      outState.sourceTag);
  if (outState.resolved) {
    outState.snap = ctx.snap;
    outState.originalColor = ctx.snap.colorPacked;
    // Register persist suppress — CG_DrawHudElem x=9999 handles it.
    {
      const uintptr_t canonicalElem = TextHook_GetCGDrawHudElemPtr();
      const uintptr_t regElem =
          (canonicalElem != 0) ? canonicalElem : ctx.snap.rbx;
      if (!g_TimeScriptPersistSuppress.active ||
          g_TimeScriptPersistSuppress.elemPtr != regElem) {
        uint32_t origFsBits = 0, origType = 0;
        // Read from canonical elem (HudElem array), not ctx.snap.rbx
        // (IAR register — may point to render buffer, not HudElem).
        TryReadIntroU32(regElem, 0x14, origFsBits);
        TryReadIntroU32(regElem, 0x00, origType);
        g_TimeScriptPersistSuppress.elemPtr = regElem;
        g_TimeScriptPersistSuppress.originalFontScaleBits = origFsBits;
        g_TimeScriptPersistSuppress.originalType = origType;
      }
      g_TimeScriptPersistSuppress.rbx = ctx.snap.rbx;
      g_TimeScriptPersistSuppress.originalColor = ctx.snap.colorPacked;
      g_TimeScriptPersistSuppress.lastSuppressTick = GetTickCount();
      g_TimeScriptPersistSuppress.active = true;
      outState.alphaSuppressed = true;
    }

    static std::unordered_map<std::string, DWORD> s_introTimerHideLogTick;
    const DWORD nowHide = GetTickCount();
    const std::string hideSig =
        std::to_string((unsigned long long)ctx.callerOffset) + "|" +
        std::to_string((unsigned int)ctx.slcIndex) + "|" +
        outState.match.tail + "|" + outState.sourceTag;
    auto itHide = s_introTimerHideLogTick.find(hideSig);
    if (itHide == s_introTimerHideLogTick.end() ||
        (nowHide - itHide->second) > 900) {
      s_introTimerHideLogTick[hideSig] = nowHide;
      char hbuf[768];
      sprintf_s(
          hbuf,
          "[INTRO-TIMER-HIDE] caller=+0x%X slc=0x%X src=%s hidden=%d x=%.1f y=%.1f align=%u/%u fs=%.2f timer=%.3f color=0x%08X textPtr=0x%llX raw=\"%.120s\" render=\"%.120s\"",
          ctx.callerOffset, (unsigned int)ctx.slcIndex,
          outState.sourceTag.c_str(), outState.alphaSuppressed ? 1 : 0,
          ctx.snap.x, ctx.snap.y, ctx.snap.horzAlign, ctx.snap.vertAlign,
          ctx.snap.fontScale, ctx.snap.timerValue, outState.originalColor,
          (unsigned long long)ctx.snap.textPtr,
          Utf8Preview(outState.rawText, 120).c_str(),
          outState.match.renderText.c_str());
      LogToFile(hbuf);
    }
    return;
  }

  // --- Direct SLC fallback for managed countdown timers ---
  // Try multiple SLC sources: ctx.slcIndex (engine param), snap.textSlc (+0x84),
  // and +0x40 (label field).  For TEXT elements (type=1), the configstring for
  // "Powerdown in" is typically in the text field (+0x84) = ctx.slcIndex, while
  // +0x40 (label) may be 0.
  {
    // Collect candidate SLC values to try
    uint32_t candidateSlcs[3] = {0, 0, 0};
    int nCandidates = 0;
    // 1. Engine parameter (most reliable - this IS the configstring being rendered)
    if (ctx.slcIndex > 0 && ctx.slcIndex <= 0xFFFF) {
      candidateSlcs[nCandidates++] = (uint32_t)ctx.slcIndex;
    }
    // 2. snap.textSlc (from shared builder, +0x84 - correct offset)
    if (ctx.snap.textSlc > 0 && ctx.snap.textSlc <= 0xFFFF &&
        ctx.snap.textSlc != (uint32_t)ctx.slcIndex) {
      candidateSlcs[nCandidates++] = ctx.snap.textSlc;
    }
    // 3. Direct read of +0x40 (label field - correct for timer-type elements)
    uint32_t labelField40 = 0;
    if (ctx.snap.rbx != 0 &&
        TryReadIntroU32(ctx.snap.rbx, 0x40, labelField40) &&
        labelField40 > 0 && labelField40 <= 0xFFFF &&
        labelField40 != (uint32_t)ctx.slcIndex &&
        labelField40 != ctx.snap.textSlc) {
      candidateSlcs[nCandidates++] = labelField40;
    }

    for (int ci = 0; ci < nCandidates; ++ci) {
      std::string directKey;
      if (TryResolveHudScriptCountdownKeyFromCfg(candidateSlcs[ci], directKey) &&
          IsTimeScriptManagedCountdownKeyName(directKey)) {
        // Found a TimeScript-managed countdown key (CLOCKWORK_POWERDOWN,
        // CLOCKWORK_EXFIL, or SATFARM_TIME_IMACT).
        // Read correct alignment from +0x28/+0x2C (alignOrg/alignScreen).
        uint32_t correctAlignOrg = 0;
        uint32_t correctAlignScreen = 0;
        if (ctx.snap.rbx != 0) {
          TryReadIntroU32(ctx.snap.rbx, 0x28, correctAlignOrg);
          TryReadIntroU32(ctx.snap.rbx, 0x2C, correctAlignScreen);
        }
        uint32_t timeFieldU = 0, durationFieldU = 0;
        float valueField = 0.0f;
        if (ctx.snap.rbx != 0) {
          TryReadIntroU32(ctx.snap.rbx, 0x78, timeFieldU);
          TryReadIntroU32(ctx.snap.rbx, 0x7C, durationFieldU);
          TryReadIntroFloat(ctx.snap.rbx, 0x80, valueField);
        }
        TimeScript_StoreSnapshotFromHudElemTimer(
            directKey, ctx.snap.x, ctx.snap.y, ctx.snap.fontScale,
            correctAlignOrg, correctAlignScreen, ctx.snap.colorPacked,
            (int)timeFieldU, (int)durationFieldU, valueField,
            candidateSlcs[ci], ctx.snap.textSlc, ctx.snap.rbx);
        outState.resolved = true;
        outState.snap = ctx.snap;
        outState.originalColor = ctx.snap.colorPacked;
        outState.match.key = directKey;
        EnsureTranslationsLoaded();
        auto itK = g_KeyToKorean.find(directKey);
        if (itK != g_KeyToKorean.end()) {
          outState.match.renderText = itK->second;
        }
        outState.sourceTag = "ir_direct_slc";

        static DWORD s_lastDirectLabelLog = 0;
        const DWORD nowDL = GetTickCount();
        if ((nowDL - s_lastDirectLabelLog) > 900) {
          s_lastDirectLabelLog = nowDL;
          char dlbuf[512];
          sprintf_s(dlbuf,
                    "[INTRO-TIMER-DIRECT-SLC] key=%s slc=0x%X src=%d rbx=0x%llX "
                    "x=%.1f y=%.1f fs=%.2f ao=%u as=%u color=0x%08X "
                    "time=%d dur=%d val=%.3f caller=+0x%X",
                    directKey.c_str(), candidateSlcs[ci], ci,
                    (unsigned long long)ctx.snap.rbx,
                    ctx.snap.x, ctx.snap.y, ctx.snap.fontScale,
                    correctAlignOrg, correctAlignScreen, ctx.snap.colorPacked,
                    (int)timeFieldU, (int)durationFieldU, valueField, ctx.callerOffset);
          LogToFile(dlbuf);
        }

        // Register for CG_DrawHudElem x=9999 suppression.
        // Uses the canonical HudElem array pointer (from IdentifyHudElemPtr)
        // set by CG_DrawHudElem before calling original.  ctx.snap.rbx is
        // an IAR register value that may NOT be a valid HudElem array pointer,
        // so CG_DrawHudElem would never match it.
        {
          const uintptr_t canonicalElem = TextHook_GetCGDrawHudElemPtr();
          const uintptr_t regElem =
              (canonicalElem != 0) ? canonicalElem : ctx.snap.rbx;
          if (!g_TimeScriptPersistSuppress.active ||
              g_TimeScriptPersistSuppress.elemPtr != regElem) {
            uint32_t origFsBits = 0, origType = 0;
            // Read from canonical elem, not ctx.snap.rbx (IAR register).
            TryReadIntroU32(regElem, 0x14, origFsBits);
            TryReadIntroU32(regElem, 0x00, origType);
            g_TimeScriptPersistSuppress.elemPtr = regElem;
            g_TimeScriptPersistSuppress.originalFontScaleBits = origFsBits;
            g_TimeScriptPersistSuppress.originalType = origType;
          }
          g_TimeScriptPersistSuppress.rbx = ctx.snap.rbx;
          g_TimeScriptPersistSuppress.originalColor = ctx.snap.colorPacked;
          g_TimeScriptPersistSuppress.lastSuppressTick = GetTickCount();
          g_TimeScriptPersistSuppress.active = true;
          outState.alphaSuppressed = true;
        }
        return;
      }
    }
  }

  if (IsLikelyIntroCountdownFallbackLane(ctx.snap, ctx.callerOffset)) {
    static std::unordered_map<uint32_t, DWORD> s_introTimerProbeLogTick;
    const DWORD nowProbe = GetTickCount();
    auto itProbe = s_introTimerProbeLogTick.find((uint32_t)ctx.slcIndex);
    if (itProbe == s_introTimerProbeLogTick.end() ||
        (nowProbe - itProbe->second) > 900) {
      s_introTimerProbeLogTick[(uint32_t)ctx.slcIndex] = nowProbe;
      std::string textPtrPreview;
      TryReadCStringPreview((const char *)ctx.snap.textPtr, 160, textPtrPreview);
      char pbuf[768];
      sprintf_s(
          pbuf,
          "[INTRO-TIMER-PROBE] caller=+0x%X slc=0x%X x=%.1f y=%.1f align=%u/%u fs=%.2f timer=%.3f color=0x%08X flags=0x%X text=0x%X label=0x%X textPtr=0x%llX preview=\"%.120s\"",
          ctx.callerOffset, (unsigned int)ctx.slcIndex, ctx.snap.x, ctx.snap.y,
          ctx.snap.horzAlign, ctx.snap.vertAlign, ctx.snap.fontScale,
          ctx.snap.timerValue, ctx.snap.colorPacked, ctx.snap.flags,
          ctx.snap.textSlc, ctx.snap.labelSlc,
          (unsigned long long)ctx.snap.textPtr,
          Utf8Preview(textPtrPreview, 120).c_str());
      LogToFile(pbuf);
    }
    return;
  }

  if (LooksLikeIntroCountdownShape(ctx.snap.type, ctx.snap.x, ctx.snap.y,
                                   ctx.snap.fontScale, ctx.snap.timerValue,
                                   ctx.snap.horzAlign, ctx.snap.vertAlign)) {
    static std::unordered_map<uint32_t, DWORD> s_introTimerCandLogTick;
    const DWORD nowCand = GetTickCount();
    auto itCand = s_introTimerCandLogTick.find((uint32_t)ctx.slcIndex);
    if (itCand == s_introTimerCandLogTick.end() ||
        (nowCand - itCand->second) > 900) {
      s_introTimerCandLogTick[(uint32_t)ctx.slcIndex] = nowCand;
      std::string textPtrPreview;
      TryReadCStringPreview((const char *)ctx.snap.textPtr, 160, textPtrPreview);
      char cbuf[768];
      sprintf_s(
          cbuf,
          "[INTRO-TIMER-CAND] caller=+0x%X slc=0x%X type=%u x=%.1f y=%.1f align=%u/%u fs=%.2f timer=%.3f color=0x%08X flags=0x%X textPtr=0x%llX preview=\"%.120s\"",
          ctx.callerOffset, (unsigned int)ctx.slcIndex, ctx.snap.type,
          ctx.snap.x, ctx.snap.y, ctx.snap.horzAlign, ctx.snap.vertAlign,
          ctx.snap.fontScale, ctx.snap.timerValue, ctx.snap.colorPacked,
          ctx.snap.flags, (unsigned long long)ctx.snap.textPtr,
          Utf8Preview(textPtrPreview, 120).c_str());
      LogToFile(cbuf);
    }
  }
}

static void FinalizeIntroCountdownOverlay(int slcIndex,
                                          const IntroCountdownOverlayState &state) {
  if (state.alphaSuppressed) {
    TryWriteIntroU32(state.snap.rbx, 0x30, state.originalColor);
  }
  if (!state.resolved) {
    return;
  }

  float lineX = 0.0f;
  float lineY = 0.0f;
  float lineScale = 0.0f;
  float fontHeight = 0.0f;
  if (TryComputeIntroCountdownLayout(state.snap, lineX, lineY, lineScale,
                                     fontHeight)) {
    int forceAlign = 0;
    if (state.snap.horzAlign == 2) {
      forceAlign = 1;
    } else if (state.snap.horzAlign == 1) {
      forceAlign = 2;
    }
    const bool yIsTopOfText = (state.snap.vertAlign == 0);
    float color[4] = {
        (float)((state.originalColor >> 0) & 0xFF) / 255.0f,
        (float)((state.originalColor >> 8) & 0xFF) / 255.0f,
        (float)((state.originalColor >> 16) & 0xFF) / 255.0f,
        (float)((state.originalColor >> 24) & 0xFF) / 255.0f,
    };
    QueueHudScriptCountdownOverlay(
        state.match, lineX, lineY, lineScale, fontHeight, 0, color,
        yIsTopOfText, state.callerOffset,
        state.sourceTag.empty() ? "INTRO-TIMER" : state.sourceTag.c_str(),
        forceAlign);
    return;
  }

  static std::unordered_map<uint32_t, DWORD> s_introTimerLayoutMissTick;
  const DWORD nowLayoutMiss = GetTickCount();
  auto itLayoutMiss = s_introTimerLayoutMissTick.find((uint32_t)slcIndex);
  if (itLayoutMiss == s_introTimerLayoutMissTick.end() ||
      (nowLayoutMiss - itLayoutMiss->second) > 1500) {
    s_introTimerLayoutMissTick[(uint32_t)slcIndex] = nowLayoutMiss;
    char lbuf[512];
    sprintf_s(
        lbuf,
        "[INTRO-TIMER-LAYOUT-MISS] caller=+0x%X slc=0x%X x=%.1f y=%.1f align=%u/%u fs=%.2f timer=%.3f",
        state.callerOffset, (unsigned int)slcIndex, state.snap.x, state.snap.y,
        state.snap.horzAlign, state.snap.vertAlign, state.snap.fontScale,
        state.snap.timerValue);
    LogToFile(lbuf);
  }
}

void Detour_IntroRenderFn(int type, int slcIndex, void *renderCtx, int count) {

  // Credits guard: skip ALL processing (stack scan, CFG-REV, HUDHINT
  // promotion, objective capture) while credits are active.  Still call
  // original so the engine renders credits text normally.
  {
    extern std::atomic<bool> g_creditsGuardActive;
    if (g_creditsGuardActive.load(std::memory_order_relaxed)) {
      g_lastIntroSlcIdx = slcIndex;
      g_currentIntroSlcIdx = slcIndex;
      if (Original_IntroRenderFn) {
        Original_IntroRenderFn(type, slcIndex, renderCtx, count);
      }
      g_currentIntroSlcIdx = -1;
      return;
    }
  }

  IntroCountdownOverlayState introCountdownState;

  // Per-frame: read native render params from parent's RBX (HudElem struct)
  // RBX is saved at RSP+78 relative to _AddressOfReturnAddress() (confirmed)
  {
    uintptr_t stackBase = (uintptr_t)_AddressOfReturnAddress();
    const uintptr_t retAddr = (uintptr_t)_ReturnAddress();
    IntroDetourSharedContext ctx;
    if (TryBuildIntroDetourSharedContext(stackBase, retAddr, type, slcIndex,
                                         renderCtx, count, ctx)) {
      const IntroHudElemSnapshot &snap = ctx.snap;
      const unsigned int callerOffset = ctx.callerOffset;

      // Credits HudElem pattern detection: during end credits, many
      // HudElems at y=-120, flags=0x1, fs~1.35 are drawn every frame.
      // These are credits text lines (weapon models, staff names, etc.).
      // Skip all expensive processing (CFG-REV, timer stack scan, hint
      // promotion) to prevent false positives and performance issues.
      const bool isCreditsHudElem =
          (snap.y <= -115.0f && snap.flags == 0x1 &&
           snap.fontScale >= 1.28f && snap.fontScale <= 1.70f);
      if (isCreditsHudElem) {
        // Skip expensive processing for credits-like HudElems (performance).
        // Guard activation is NOT done here — IntroTextLayout CREDITS_* key
        // detection is the authoritative trigger.  isCreditsHudElem geometry
        // is too unreliable (false positives during normal gameplay).
        static DWORD s_lastCreditsPatternLog = 0;
        const DWORD nowCredits = GetTickCount();
        if ((nowCredits - s_lastCreditsPatternLog) > 2000) {
          s_lastCreditsPatternLog = nowCredits;
          char cbuf[256];
          sprintf_s(cbuf,
                    "[CREDITS-PATTERN] y=%.1f fs=%.2f flags=0x%X "
                    "text=0x%X label=0x%X — skip processing (no guard)",
                    snap.y, snap.fontScale, snap.flags,
                    snap.textSlc, snap.labelSlc);
          LogToFile(cbuf);
        }
      }

      // Timer-only path. Keep this isolated from the objective/status block so
      // timer work does not have to edit the authority logic below.
      if (!isCreditsHudElem)
        ProcessIntroCountdownSnapshot(ctx, introCountdownState);
#if GHOSTSKOR_OBJECTIVE_REVERSE
      // Objective/status-only path. This consumes the shared snapshot but
      // should not depend on timer ownership logic above.
      if (!isCreditsHudElem) {

        // Exact non-objective HUDHINT intake:
        // Some gameplay hints (e.g. DEER_HUNT_LASER_HINT) surface through the
        // +0x1F087D exact HudElem snapshot path rather than the mission-prompt
        // feeder. Promote those directly from the captured native struct via
        // the shared strict-HUDHINT helper so the geometry rules stay unified.
        TextHook_PromoteStrictHintSnapshotByCfg(
            snap.textSlc, snap.labelSlc, snap.x, snap.y, snap.fontScale,
            snap.horzAlign, snap.vertAlign, snap.colorPacked, snap.elemTime,
            snap.fxBirthTime, snap.fxLetterTime, snap.flags, callerOffset,
            "HUD-IR-NATIVE");

        // Phase 1A: capture valid HudElem pointer for array discovery.
        // Snapshot guarantees RBX readability for fields used below.
        {
          g_introHudElemPtr = snap.rbx;
          g_introHudElemTick = GetTickCount();
          g_introHudElemServerTime = snap.elemTime;

          // Immediate capture-time logging (throttled to 1/sec per slcIndex)
          if (kVerboseRuntimeLogs) {
            static std::unordered_map<uint32_t, unsigned long> s_captureLog;
            unsigned long capNow = GetTickCount();
            auto cit = s_captureLog.find((uint32_t)slcIndex);
            if (cit == s_captureLog.end() || (capNow - cit->second) > 1000) {
              s_captureLog[(uint32_t)slcIndex] = capNow;
              uint32_t cType = snap.type;
              float cX = snap.x;
              float cY = snap.y;
              float cFS = snap.fontScale;
              uint32_t cColor = snap.colorPacked;
              int cTime = snap.elemTime;
              int cFxBirth = snap.fxBirthTime;
              int cFxLetter = snap.fxLetterTime;
              int cFlags = snap.flags;
              float cAlpha = (float)((cColor >> 24) & 0xFF) / 255.0f;
              char cbuf[384];
              sprintf_s(cbuf,
                        "[OBJ-REVERSE-CAP] LIVE ptr=0x%llX type=%u x=%.1f "
                        "y=%.1f fs=%.2f a=%.2f color=0x%08X time=%d "
                        "fxB=%d fxL=%d flags=0x%X label=0x%X text=0x%X "
                        "caller=+0x%X argType=%d count=%d ctx=0x%llX",
                        (unsigned long long)snap.rbx, cType, cX, cY, cFS, cAlpha,
                        cColor, cTime, cFxBirth, cFxLetter, cFlags, snap.labelSlc,
                        snap.textSlc, callerOffset, type, count,
                        (unsigned long long)(uintptr_t)renderCtx);
              LogToFile(cbuf);
            }
          }
        }

        LogObjectiveReverseProbe(snap, callerOffset, type, renderCtx, count);

        // --- PERF gate: skip heavy resolve/authority pipeline when throttled ---
        if (RuntimeFlags_ObjectiveStatusEnableOsRevOnly()) {
          // HudElem.text is a configstring index, not an SLC index.
          // Feed OSREV from cfg->key resolution so the snapshot tracks the
          // native objective lane instead of the old SLC cache.
          // Resolve cfg→key.  The full 768-iteration probe is allowed here
          // because ObjRev_ProbeConfigToSlcBias now has negative caching:
          // cfgIndices that fail the probe are suppressed for 120 seconds,
          // so the expensive scan only fires once per new cfgIndex.
          auto resolveObjectiveCfgKey = [](uint32_t cfg) -> std::string {
            if (!ObjRev_IsPlausibleConfigIndex(cfg)) {
              return "";
            }
            std::string key = ObjRev_ResolveConfigKey(cfg, true, true);
            if (key.empty()) {
              key = ObjRev_ResolveConfigKey(cfg, true, false);
            }
            return key;
          };
          const bool objectiveLane = IsObjectiveReverseLane(snap);
          auto resolveAnyCfgKey = [&](uint32_t cfg) -> std::string {
            std::string key = resolveObjectiveCfgKey(cfg);
            if (key.empty() && ObjRev_IsPlausibleConfigIndex(cfg)) {
              key = ObjRev_ResolveConfigKey(cfg, false, true);
              if (key.empty()) {
                key = ObjRev_ResolveConfigKey(cfg, false, false);
              }
            }
            if (!key.empty()) {
              if (IsObjectiveStatusMessageKeyName(key)) {
                return key;
              }
              // Use the broad candidate check instead of the strict
              // _OBJ_-only check.  Many missions (DEER_HUNT, CLOCKWORK,
              // SATFARM, …) have objective keys without "_OBJ_" in the
              // name.  IsObjectiveCaptureCandidateKey applies the deny-list
              // (MENU_, SUBTITLE_, DEADQUOTE_, …) and localization/prompt
              // guards, which is sufficient to prevent false positives.
              // RecordIntroCapture has its own final validation as well.
              if (objectiveLane &&
                  TextHook_OSAuth_IsObjectiveCaptureCandidateKey(key)) {
                return key;
              }
              key.clear();
            }
            return key;
          };

          uint32_t osrevCfg = 0;
          std::string osrevRawText;
          std::string osrevKey;
          int osrevCtxTextOff = 0;
          // PERF: TryResolveObjectiveRenderCtxText is extremely expensive
          // (73 memory reads + full g_KeyToEnglish scan per readable offset).
          // Cache result by (textSlc, fxBirthTime) so it only resolves once
          // per unique objective instance instead of every frame.
          {
            struct CtxTextCacheEntry {
              std::string rawText;
              std::string key;
              int offset = 0;
              bool resolved = false;
              unsigned long tick = 0;
            };
            static std::unordered_map<uint64_t, CtxTextCacheEntry> s_ctxTextCache;
            // Clear on game reset so stale entries don't block new map's keys
            {
              static unsigned long s_ctxCacheResetTickSeen = 0;
              const unsigned long ctxResetTick = TextHook_GetObjectiveRuntimeResetTick();
              if (ctxResetTick != 0 && ctxResetTick != s_ctxCacheResetTickSeen) {
                s_ctxCacheResetTickSeen = ctxResetTick;
                s_ctxTextCache.clear();
              }
            }
            const uint64_t ctxCacheKey =
                ((uint64_t)snap.textSlc << 32) | (uint64_t)(uint32_t)snap.fxBirthTime;
            const unsigned long nowCtxCache = GetTickCount();
            auto itCache = s_ctxTextCache.find(ctxCacheKey);
            if (itCache != s_ctxTextCache.end() &&
                (nowCtxCache - itCache->second.tick) < 5000) {
              // Use cached result
              if (itCache->second.resolved) {
                osrevRawText = itCache->second.rawText;
                osrevKey = itCache->second.key;
                osrevCtxTextOff = itCache->second.offset;
              }
              itCache->second.tick = nowCtxCache;
            } else {
              // First time or stale — do the expensive resolve
              if (IsObjectiveReverseLane(snap)) {
                CtxTextCacheEntry entry;
                entry.tick = nowCtxCache;
                entry.resolved = TryResolveObjectiveRenderCtxText(
                    renderCtx, entry.rawText, entry.key, entry.offset);
                if (entry.resolved) {
                  osrevRawText = entry.rawText;
                  osrevKey = entry.key;
                  osrevCtxTextOff = entry.offset;
                }
                s_ctxTextCache[ctxCacheKey] = std::move(entry);
                // Prune old entries
                if (s_ctxTextCache.size() > 64) {
                  for (auto it = s_ctxTextCache.begin(); it != s_ctxTextCache.end();) {
                    if ((nowCtxCache - it->second.tick) > 30000)
                      it = s_ctxTextCache.erase(it);
                    else
                      ++it;
                  }
                }
              }
            }
          }
          if (!osrevRawText.empty()) {
            static std::unordered_map<std::string, unsigned long>
                s_osrevCtxTextLogTick;
            const unsigned long nowCtxText = GetTickCount();
            const std::string sig =
                (osrevKey.empty() ? osrevRawText : osrevKey) + "|" +
                std::to_string(osrevCtxTextOff);
            auto itCtxText = s_osrevCtxTextLogTick.find(sig);
            if (itCtxText == s_osrevCtxTextLogTick.end() ||
                (nowCtxText - itCtxText->second) > 1000) {
              s_osrevCtxTextLogTick[sig] = nowCtxText;
              char tbuf[384];
              if (!osrevKey.empty()) {
                sprintf_s(tbuf,
                          "[OBJ-CTX-TEXT] key=%s off=%d caller=+0x%X raw=\"%.160s\"",
                          osrevKey.c_str(), osrevCtxTextOff, callerOffset,
                          osrevRawText.c_str());
              } else {
                sprintf_s(tbuf,
                          "[OBJ-CTX-RAW] off=%d caller=+0x%X raw=\"%.160s\"",
                          osrevCtxTextOff, callerOffset, osrevRawText.c_str());
              }
              LogToFile(tbuf);
            }
          }

          if (osrevKey.empty()) {
            const std::string textObjectiveKey = resolveObjectiveCfgKey(snap.textSlc);
            if (!textObjectiveKey.empty()) {
              osrevKey = textObjectiveKey;
              osrevCfg = snap.textSlc;
            }
          }
          if (osrevKey.empty() && snap.labelSlc != snap.textSlc) {
            const std::string labelObjectiveKey =
                resolveObjectiveCfgKey(snap.labelSlc);
            if (!labelObjectiveKey.empty()) {
              osrevKey = labelObjectiveKey;
              osrevCfg = snap.labelSlc;
            }
          }
          const int osauthLaneSlot =
              (int)lroundf((snap.y - 10.0f) / 20.0f);
          std::string osauthKey;
          uint32_t osauthCfg = 0;
          auto pickLiveCfg = [&]() -> uint32_t {
            if (ObjRev_IsPlausibleConfigIndex(snap.textSlc)) {
              return snap.textSlc;
            }
            if (snap.labelSlc != snap.textSlc &&
                ObjRev_IsPlausibleConfigIndex(snap.labelSlc)) {
              return snap.labelSlc;
            }
            return 0;
          };
          std::string directObjectiveKey;
          uint32_t directObjectiveCfg = 0;
          auto trySelectDirectObjectiveCfg = [&](uint32_t cfg) {
            if (directObjectiveCfg != 0 || !ObjRev_IsPlausibleConfigIndex(cfg)) {
              return;
            }
            // Try strict (already-observed) resolution first.
            std::string cfgKey = resolveObjectiveCfgKey(cfg);
            // Fall back to the broad resolver so that non-standard keys
            // (DEER_HUNT_FINISH_PATROL, CLOCKWORK_ADVANCE_*, etc.) that
            // pass the deny-list + localization check can be accepted even
            // on their first appearance.
            if (cfgKey.empty()) {
              cfgKey = resolveAnyCfgKey(cfg);
              if (!cfgKey.empty() &&
                  (IsObjectiveStatusMessageKeyName(cfgKey) ||
                   !TextHook_OSAuth_IsObjectiveCaptureCandidateKey(cfgKey))) {
                cfgKey.clear();
              }
            }
            if (!cfgKey.empty()) {
              directObjectiveKey = cfgKey;
              directObjectiveCfg = cfg;
            }
          };
          if (!osrevKey.empty() &&
              TextHook_OSAuth_IsObjectiveCaptureCandidateKey(osrevKey) &&
              !IsObjectiveStatusMessageKeyName(osrevKey)) {
            directObjectiveKey = osrevKey;
            directObjectiveCfg = (osrevCfg != 0) ? osrevCfg : pickLiveCfg();
          }
          if (directObjectiveKey.empty()) {
            trySelectDirectObjectiveCfg(snap.textSlc);
          }
          if (directObjectiveKey.empty() && snap.labelSlc != snap.textSlc) {
            trySelectDirectObjectiveCfg(snap.labelSlc);
          }

          auto logBootstrapObjective = [&](const std::string &key, uint32_t cfg) {
            if (key.empty() || cfg == 0) {
              return;
            }
            static std::unordered_map<uint64_t, unsigned long>
                s_osauthBootstrapLogTick;
            const unsigned long nowBootstrap = GetTickCount();
            const uint64_t sig =
                ((uint64_t)(cfg & 0xFFFFu) << 32) ^
                (uint64_t)(uint32_t)snap.fxBirthTime;
            auto itBoot = s_osauthBootstrapLogTick.find(sig);
            if (itBoot == s_osauthBootstrapLogTick.end() ||
                (nowBootstrap - itBoot->second) > 1000) {
              s_osauthBootstrapLogTick[sig] = nowBootstrap;
              char bbuf[320];
              sprintf_s(
                  bbuf,
                  "[OSAUTH-BOOTSTRAP-OBJ] key=%s cfg=0x%X fxB=%d lane=%d x=%.1f y=%.1f",
                  key.c_str(), cfg, snap.fxBirthTime, osauthLaneSlot, snap.x,
                  snap.y);
              LogToFile(bbuf);
            }
          };

          auto logBootstrapStatus = [&](const std::string &key, uint32_t cfg) {
            if (key.empty() || cfg == 0) {
              return;
            }
            static std::unordered_map<uint64_t, unsigned long>
                s_osauthBootstrapStatusLogTick;
            const unsigned long nowBootstrap = GetTickCount();
            const uint64_t sig =
                (((uint64_t)(cfg & 0xFFFFu)) << 32) ^
                ((uint64_t)(uint32_t)snap.fxBirthTime) ^
                (uint64_t)(uint32_t)(osauthLaneSlot & 0xFF);
            auto itBoot = s_osauthBootstrapStatusLogTick.find(sig);
            if (itBoot == s_osauthBootstrapStatusLogTick.end() ||
                (nowBootstrap - itBoot->second) > 1000) {
              s_osauthBootstrapStatusLogTick[sig] = nowBootstrap;
              char bbuf[320];
              sprintf_s(
                  bbuf,
                  "[OSAUTH-BOOTSTRAP-STATUS] key=%s cfg=0x%X fxB=%d lane=%d x=%.1f y=%.1f",
                  key.c_str(), cfg, snap.fxBirthTime, osauthLaneSlot, snap.x,
                  snap.y);
              LogToFile(bbuf);
            }
          };

          auto tryBootstrapObjectiveKey = [&]() -> std::string {
            const unsigned int wantedOrdinal =
                (osauthLaneSlot >= 0) ? (unsigned int)(osauthLaneSlot + 1) : 0u;
            const auto recentObjectiveKeys = TextHook_GetRecentObjectiveKeys(1600);
            std::string bestOrdinalKey;
            unsigned long bestOrdinalTick = 0;
            std::string bestRecentKey;
            unsigned long bestRecentTick = 0;
            unsigned long secondRecentTick = 0;
            size_t candidateCount = 0;
            for (const auto &obs : recentObjectiveKeys) {
              if (obs.key.empty() ||
                  !TextHook_OSAuth_IsObjectiveCaptureCandidateKey(obs.key) ||
                  IsObjectiveStatusMessageKeyName(obs.key)) {
                continue;
              }
              if (obs.channel != OBJ_CHANNEL_GAMEPLAY_LIST &&
                  obs.channel != OBJ_CHANNEL_NONE) {
                continue;
              }
              ++candidateCount;
              if (wantedOrdinal != 0 && obs.ordinal == wantedOrdinal) {
                if (bestOrdinalKey.empty() || obs.lastSeen >= bestOrdinalTick) {
                  bestOrdinalKey = obs.key;
                  bestOrdinalTick = obs.lastSeen;
                }
              }
              if (bestRecentKey.empty() || obs.lastSeen >= bestRecentTick) {
                if (!bestRecentKey.empty()) {
                  secondRecentTick = bestRecentTick;
                }
                bestRecentKey = obs.key;
                bestRecentTick = obs.lastSeen;
              } else if (obs.lastSeen > secondRecentTick) {
                secondRecentTick = obs.lastSeen;
              }
            }
            if (!bestOrdinalKey.empty()) {
              return bestOrdinalKey;
            }
            if (candidateCount == 1) {
              return bestRecentKey;
            }
            const unsigned long nowObjectiveBootstrap = GetTickCount();
            const unsigned long bestRecentAge =
                (bestRecentTick != 0 && nowObjectiveBootstrap >= bestRecentTick)
                    ? (nowObjectiveBootstrap - bestRecentTick)
                    : 0;
            const unsigned long freshnessLead =
                (bestRecentTick >= secondRecentTick)
                    ? (bestRecentTick - secondRecentTick)
                    : 0;
            if (!bestRecentKey.empty() && bestRecentTick != 0 &&
                bestRecentAge <= 240 && freshnessLead >= 140) {
              return bestRecentKey;
            }
            return std::string();
          };

          auto tryBootstrapStatusKey = [&]() -> std::string {
            const unsigned long nowBootstrap = GetTickCount();
            const auto activeStatusKeys =
                GetActiveTopLeftBootstrapStatusKeys(nowBootstrap);
            if (osauthLaneSlot >= 0 &&
                (size_t)osauthLaneSlot < activeStatusKeys.size()) {
              return activeStatusKeys[(size_t)osauthLaneSlot];
            }
            if (osauthLaneSlot == 0) {
              std::string probeKey;
              if (TextHook_GetRecentObjectiveStatusProbe(300, 0, probeKey) &&
                  IsTopLeftBootstrapStatusKey(probeKey)) {
                return probeKey;
              }
            }
            return std::string();
          };

          if (objectiveLane) {
            const std::string bootstrapStatusKey = tryBootstrapStatusKey();
            const bool laneReservedForStatus = !bootstrapStatusKey.empty();
            std::string directStatusKey;
            uint32_t directStatusCfg = 0;
            auto trySelectDirectStatusCfg = [&](uint32_t cfg) {
              if (directStatusCfg != 0 || !ObjRev_IsPlausibleConfigIndex(cfg)) {
                return;
              }
              const std::string cfgKey = resolveAnyCfgKey(cfg);
              if (!cfgKey.empty() && IsTopLeftBootstrapStatusKey(cfgKey)) {
                directStatusKey = cfgKey;
                directStatusCfg = cfg;
              }
            };

            trySelectDirectStatusCfg(snap.textSlc);
            if (snap.labelSlc != snap.textSlc) {
              trySelectDirectStatusCfg(snap.labelSlc);
            }

            // Status lanes must be decided before any objective bootstrap.
            // If the HudElem's cfg directly resolves to a status key, that is
            // the strongest evidence of what the engine is currently displaying.
            // This must win on ALL lanes, not just lane 0 -- the previous
            // lane-0-only startup race guard left lanes 1+ unprotected, so
            // objective bootstrap could steal status instances on those lanes
            // and the owner lock would permanently block the status channel.
            if (!directStatusKey.empty() && directStatusCfg != 0) {
              // Direct status cfg is authoritative on any lane.
              osauthKey = directStatusKey;
              osauthCfg = directStatusCfg;
              logBootstrapStatus(osauthKey, osauthCfg);
            } else if (laneReservedForStatus) {
              // Lane is reserved by an active status event but cfg didn't
              // resolve to a status key (transient mismatch or HudElem reuse).
              // Block objective bootstrap from stealing this lane -- leave
              // osauthKey/osauthCfg empty so OSAUTH records the visual sample
              // without a key, preserving the lane for the status channel.
            }

            if (osauthCfg == 0 && !laneReservedForStatus) {
              if (!directObjectiveKey.empty()) {
                osauthKey = directObjectiveKey;
                osauthCfg = directObjectiveCfg;
              } else {
                const std::string bootstrapObjectiveKey =
                    tryBootstrapObjectiveKey();
                const uint32_t bootstrapCfg = pickLiveCfg();
                if (!bootstrapObjectiveKey.empty() && bootstrapCfg != 0) {
                  osauthKey = bootstrapObjectiveKey;
                  osauthCfg = bootstrapCfg;
                  logBootstrapObjective(osauthKey, osauthCfg);
                }
              }
            }
          } else {
            auto trySelectAuthCfg = [&](uint32_t cfg) {
              if (osauthCfg != 0 || !ObjRev_IsPlausibleConfigIndex(cfg)) {
                return;
              }
              const std::string cfgKey = resolveAnyCfgKey(cfg);
              if (!cfgKey.empty()) {
                osauthKey = cfgKey;
                osauthCfg = cfg;
              }
            };
            trySelectAuthCfg(snap.textSlc);
            if (snap.labelSlc != snap.textSlc) {
              trySelectAuthCfg(snap.labelSlc);
            }
          }
          if (!osauthKey.empty() && osauthCfg != 0 && snap.fxBirthTime > 0) {
            // Top-left authority uses exact native instance identity.
            // Intro capture must use the live HudElem pointer, not a transient
            // array slot index, otherwise slot 0 becomes an invalid instance
            // and the first objective never reaches OSAUTH.
            const uint64_t osauthElemPtrOrIndex = (uint64_t)snap.rbx;
            if (osauthElemPtrOrIndex != 0) {
              const std::string rawForAuth =
                  osrevRawText.empty() ? osauthKey : osrevRawText;
              TextHook_OSAuth_RecordIntroCapture(
                  osauthKey, rawForAuth, osauthCfg, (uint32_t)slcIndex,
                  snap.fxBirthTime, snap.fxLetterTime, osauthElemPtrOrIndex,
                  snap.x, snap.y, snap.fontScale, 78.0f, snap.colorPacked,
                  640, 480, callerOffset, IW6Offsets::IntroRender_SP);
            } else if (kVerboseRuntimeLogs) {
              static std::unordered_map<std::string, unsigned long>
                  s_osauthIntroSkipLogTick;
              const unsigned long nowSkip = GetTickCount();
              const std::string logSig =
                  osauthKey + "|" + std::to_string(osauthCfg) + "|" +
                  std::to_string((unsigned int)snap.fxBirthTime);
              auto itLog = s_osauthIntroSkipLogTick.find(logSig);
              if (itLog == s_osauthIntroSkipLogTick.end() ||
                  (nowSkip - itLog->second) > 1200) {
                s_osauthIntroSkipLogTick[logSig] = nowSkip;
                char sbuf[352];
                sprintf_s(
                    sbuf,
                    "[OSAUTH-INTRO-SKIP] key=%s cfg=0x%X fxB=%d reason=null_rbx",
                    osauthKey.c_str(), osauthCfg, snap.fxBirthTime);
                LogToFile(sbuf);
              }
            }
          }
          // OSREV draw sample feed removed — OSAUTH is the sole authority.
        }
      } // !isCreditsHudElem (GHOSTSKOR_OBJECTIVE_REVERSE block)
#endif
      if (!isCreditsHudElem) {
        if (RuntimeFlags_ObjectiveStatusEnableOsRevOnly()) {
          static bool s_loggedOsRevSlcDisabled = false;
          if (!s_loggedOsRevSlcDisabled) {
            s_loggedOsRevSlcDisabled = true;
            LogToFile("[OSREV] IntroRender cfg/slc authority disabled");
          }
        }
        const bool introObjectiveLane =
            IsObjectiveReverseLane(snap) && !introCountdownState.resolved;
        if (introObjectiveLane) {
          std::lock_guard<std::mutex> lock(g_nativeIntroMutex);
          g_nativeIntroMap.erase((uint32_t)slcIndex);
        } else {
          uint32_t c = snap.colorPacked;
          DWORD nowTick = GetTickCount();
          NativeIntroRenderState ns;
          ns.x = snap.x;
          ns.y = snap.y;
          ns.fontScale = snap.fontScale;
          ns.horzAlign = snap.horzAlign;
          ns.vertAlign = snap.vertAlign;
          ns.colorPacked = c;
          ns.r = (float)((c >>  0) & 0xFF) / 255.0f;
          ns.g = (float)((c >>  8) & 0xFF) / 255.0f;
          ns.b = (float)((c >> 16) & 0xFF) / 255.0f;
          ns.a = (float)((c >> 24) & 0xFF) / 255.0f;
          ns.slcIdx = (uint32_t)slcIndex;
          // Native typewriter fields (server time based)
          ns.elemTime = snap.elemTime;
          ns.fxBirthTime = snap.fxBirthTime;
          ns.fxLetterTime = snap.fxLetterTime;
          ns.lastTick = nowTick;
          ns.hasAlt = false;
          ns.altX = ns.altY = 0.0f;
          ns.altFontScale = 0.0f;
          ns.altR = ns.altG = ns.altB = ns.altA = 0.0f;
          ns.altTick = 0;
          ns.hasAlt2 = false;
          ns.alt2X = ns.alt2Y = 0.0f;
          ns.alt2FontScale = 0.0f;
          ns.alt2R = ns.alt2G = ns.alt2B = ns.alt2A = 0.0f;
          ns.alt2Tick = 0;
          ns.valid = true;
          {
            std::lock_guard<std::mutex> lock(g_nativeIntroMutex);
            auto existing = g_nativeIntroMap.find((uint32_t)slcIndex);
            // Preserve timestamps from existing entry.
            // When Detour_IntroRenderFn fires after an ESC pause, lastTick is
            // frozen during the pause → staleGap = exact pause duration.
            // Shift firstVisible forward by that gap so wall-clock typewriter
            // doesn't over-advance.
            DWORD staleGap = (existing != g_nativeIntroMap.end())
                ? (nowTick - existing->second.lastTick) : 0;
            if (existing != g_nativeIntroMap.end() && staleGap < 200) {
              // Normal frame-to-frame update (< 200ms gap)
              ns.firstSeen = existing->second.firstSeen;
              ns.firstVisible = existing->second.firstVisible;
            } else if (existing != g_nativeIntroMap.end()) {
              // Pause gap (≥ 200ms): shift firstVisible forward by gap.
              // No upper limit — ESC pause can last arbitrarily long.
              // Session change is handled by g_IntroOverlayActive reset,
              // not by staleGap timeout.
              ns.firstSeen = (staleGap > 2000) ? nowTick
                                                : existing->second.firstSeen;
              if (existing->second.firstVisible > 0) {
                ns.firstVisible = existing->second.firstVisible + staleGap;
              } else {
                ns.firstVisible = 0;
              }
            } else {
              // No existing entry: first appearance
              ns.firstSeen = nowTick;
              ns.firstVisible = 0;
            }
            // Track firstVisible: first time alpha exceeds visibility threshold
            if (ns.firstVisible == 0 && ns.a > 0.04f) {
              ns.firstVisible = nowTick;
            }
            // === cg.time memory scanner ===
            // Scans module's writable pages (256MB) for a 4-byte int that
            // monotonically increases (server time).  Lightweight: Phase 0
            // runs once (~5ms), filter phases run on subsequent frames.
            {
            // Production default: disabled (phase=6) to avoid intro hitch from
            // large writable-memory scan and because selected address was unreliable.
            static int s_scanPhase = 6;
            static uintptr_t s_cgTimeAddr = 0;
            static std::vector<uintptr_t> s_candidates;
            static std::vector<int> s_prevValues;
            static DWORD s_lastScanTick = 0;

            // DISABLED: scanner-selected address is unreliable (different
            // address each run, not verified to pause during ESC).
            // Using wall-clock + pause-gap compensation instead.
            // if (s_cgTimeAddr != 0) {
            //   ns.elemTime = *(volatile int *)s_cgTimeAddr;
            // }

            if (s_scanPhase == -1 && ns.fxBirthTime > 0) {
              s_scanPhase = 0;
              LogToFile("[CG_SCAN] Starting scan, fxBirth=" +
                        std::to_string(ns.fxBirthTime));
            }

            // Phase 0: collect from module's writable pages (256MB)
            if (s_scanPhase == 0 && ns.fxBirthTime > 0) {
              s_lastScanTick = nowTick;
              s_candidates.clear();
              s_prevValues.clear();
              int lo = (ns.fxBirthTime > 1000) ? (ns.fxBirthTime - 1000) : 0;
              int hi = ns.fxBirthTime + 600000;
              uintptr_t modBase = (uintptr_t)GetModuleHandleA(NULL);
              uintptr_t scanEnd = modBase + 0x10000000; // 256MB
              MEMORY_BASIC_INFORMATION mbi;
              uintptr_t addr = modBase;
              size_t bytesScanned = 0;
              while (addr < scanEnd) {
                if (!VirtualQuery((void *)addr, &mbi, sizeof(mbi))) break;
                uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                if (regionEnd > scanEnd) regionEnd = scanEnd;
                bool writable = (mbi.State == MEM_COMMIT) &&
                    (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
                bool noGuard = !(mbi.Protect & PAGE_GUARD);
                if (writable && noGuard) {
                  bytesScanned += (regionEnd - addr);
                  uintptr_t start = (addr + 3) & ~(uintptr_t)3;
                  for (uintptr_t p = start; p + 4 <= regionEnd; p += 4) {
                    int val = *(volatile int *)p;
                    if (val > lo && val < hi) {
                      s_candidates.push_back(p);
                      s_prevValues.push_back(val);
                    }
                  }
                }
                addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
              }
              char sbuf[256];
              sprintf_s(sbuf,
                "[CG_SCAN] Phase 0: %zu candidates (%zu MB scanned) "
                "range [%d,%d]",
                s_candidates.size(), bytesScanned/(1024*1024), lo, hi);
              LogToFile(sbuf);
              s_scanPhase = 1;
            }

            // Phase 1: diagnostic — log delta distribution of first 20
            if (s_scanPhase == 1 && (nowTick - s_lastScanTick) > 300) {
              s_lastScanTick = nowTick;
              int nChanged = 0, nSame = 0, nDecr = 0, nBig = 0;
              size_t diagCount = (s_candidates.size() < 20)
                  ? s_candidates.size() : 20;
              uintptr_t modBase = (uintptr_t)GetModuleHandleA(NULL);
              for (size_t i = 0; i < s_candidates.size(); i++) {
                int val = *(volatile int *)s_candidates[i];
                int delta = val - s_prevValues[i];
                if (delta == 0) nSame++;
                else if (delta < 0) nDecr++;
                else if (delta >= 5000) nBig++;
                else nChanged++;
                if (i < diagCount) {
                  char dbuf[256];
                  sprintf_s(dbuf,
                    "[CG_SCAN] DIAG [%zu] module+0x%llX prev=%d now=%d "
                    "delta=%d",
                    i, (unsigned long long)(s_candidates[i] - modBase),
                    s_prevValues[i], val, delta);
                  LogToFile(dbuf);
                }
              }
              char sbuf[256];
              sprintf_s(sbuf,
                "[CG_SCAN] Phase 1 diag: total=%zu same=%d changed=%d "
                "decr=%d big=%d (waited %ums)",
                s_candidates.size(), nSame, nChanged, nDecr, nBig,
                (unsigned)(nowTick - s_lastScanTick + 300));
              LogToFile(sbuf);

              // Now filter: keep ANY that changed positively (1..10000)
              std::vector<uintptr_t> survivors;
              std::vector<int> survivorPrev;
              for (size_t i = 0; i < s_candidates.size(); i++) {
                int val = *(volatile int *)s_candidates[i];
                int delta = val - s_prevValues[i];
                if (delta > 0 && delta < 10000) {
                  survivors.push_back(s_candidates[i]);
                  survivorPrev.push_back(val);
                }
              }
              sprintf_s(sbuf, "[CG_SCAN] Phase 1: %zu survivors",
                        survivors.size());
              LogToFile(sbuf);
              s_candidates = survivors;
              s_prevValues = survivorPrev;
              s_scanPhase = 2;
            }

            // Phases 2-4: strict filter (delta 50-2000, 300ms apart)
            if (s_scanPhase >= 2 && s_scanPhase <= 4 &&
                (nowTick - s_lastScanTick) > 300) {
              s_lastScanTick = nowTick;
              std::vector<uintptr_t> survivors;
              std::vector<int> survivorPrev;
              for (size_t i = 0; i < s_candidates.size(); i++) {
                int val = *(volatile int *)s_candidates[i];
                int delta = val - s_prevValues[i];
                if (delta >= 50 && delta < 2000) {
                  survivors.push_back(s_candidates[i]);
                  survivorPrev.push_back(val);
                }
              }
              char fbuf[128];
              sprintf_s(fbuf, "[CG_SCAN] Phase %d: %zu survivors (from %zu)",
                        s_scanPhase, survivors.size(), s_candidates.size());
              LogToFile(fbuf);
              s_candidates = survivors;
              s_prevValues = survivorPrev;
              s_scanPhase++;
            }

            // Phase 5: select
            if (s_scanPhase == 5) {
              uintptr_t modBase = (uintptr_t)GetModuleHandleA(NULL);
              for (size_t i = 0; i < s_candidates.size() && i < 20; i++) {
                int val = *(volatile int *)s_candidates[i];
                char cbuf[256];
                sprintf_s(cbuf,
                  "[CG_SCAN] FOUND module+0x%llX val=%d elapsed=%d",
                  (unsigned long long)(s_candidates[i] - modBase),
                  val, val - ns.fxBirthTime);
                LogToFile(cbuf);
              }
              if (!s_candidates.empty()) {
                s_cgTimeAddr = s_candidates[0];
                char fbuf[128];
                sprintf_s(fbuf, "[CG_SCAN] Selected module+0x%llX",
                  (unsigned long long)(s_cgTimeAddr -
                    (uintptr_t)GetModuleHandleA(NULL)));
                LogToFile(fbuf);
                ns.elemTime = *(volatile int *)s_cgTimeAddr;
              } else {
                LogToFile("[CG_SCAN] No survivors — wall-clock fallback");
              }
              s_scanPhase = 6;
            }
            }
            // Intro titles can emit multi-pass draw state for same slcIdx
            // (main + faint ghost/shadow) inside one frame window.
            // Main = higher fontScale (stable: dupe always fs=1.0, actual >= 1.25).
            // Alpha-based selection was unstable because quickPulse randomizes
            // both passes, causing main/alt to flip each frame → drift detection
            // failed (fontScale oscillated between 1.0 and 1.28).
            // 3-pass center-card draws can span several milliseconds; 2ms was
            // too tight and often collapsed into a single captured ghost pass.
            const DWORD kMultiPassWindowMs = 8;
            if (existing != g_nativeIntroMap.end() &&
                (nowTick - existing->second.lastTick) <= kMultiPassWindowMs) {
            float aDiff = existing->second.a - ns.a;
            if (aDiff < 0.0f) aDiff = -aDiff;
            float xDiff = existing->second.x - ns.x;
            if (xDiff < 0.0f) xDiff = -xDiff;
            float yDiff = existing->second.y - ns.y;
            if (yDiff < 0.0f) yDiff = -yDiff;
            float fsDiff = existing->second.fontScale - ns.fontScale;
            if (fsDiff < 0.0f) fsDiff = -fsDiff;

            bool meaningfulDiff =
                (aDiff > 0.03f) || (xDiff > 0.35f) || (yDiff > 0.35f) ||
                (fsDiff > 0.015f);

            if (meaningfulDiff) {
              auto isSamePass = [](float x1, float y1, float fs1, float a1,
                                   float x2, float y2, float fs2, float a2) {
                float dx = x1 - x2; if (dx < 0.0f) dx = -dx;
                float dy = y1 - y2; if (dy < 0.0f) dy = -dy;
                float dfs = fs1 - fs2; if (dfs < 0.0f) dfs = -dfs;
                float da = a1 - a2; if (da < 0.0f) da = -da;
                return (dx <= 0.35f) && (dy <= 0.35f) &&
                       (dfs <= 0.015f) && (da <= 0.08f);
              };

              NativeIntroRenderState merged = existing->second;
              const NativeIntroRenderState *mainPass = &existing->second;
              const NativeIntroRenderState *altPass = &ns;

              // Prefer higher fontScale (stable criterion — dupe is always 1.0)
              if (ns.fontScale > existing->second.fontScale + 0.01f) {
                mainPass = &ns;
                altPass = &existing->second;
              } else if (existing->second.fontScale <= ns.fontScale + 0.01f) {
                // Same fontScale — fall back to alpha
                if (ns.a > (existing->second.a + 0.02f)) {
                  mainPass = &ns;
                  altPass = &existing->second;
                }
              }

              merged = *mainPass;
              merged.firstSeen = existing->second.firstSeen;
              merged.firstVisible = existing->second.firstVisible;
              merged.lastTick = nowTick;
              merged.valid = true;
              merged.hasAlt = true;
              merged.altX = altPass->x;
              merged.altY = altPass->y;
              merged.altFontScale = altPass->fontScale;
              merged.altR = altPass->r;
              merged.altG = altPass->g;
              merged.altB = altPass->b;
              merged.altA = altPass->a;
              merged.altTick = nowTick;
              merged.hasAlt2 = false;
              merged.alt2X = merged.alt2Y = 0.0f;
              merged.alt2FontScale = 0.0f;
              merged.alt2R = merged.alt2G = merged.alt2B = merged.alt2A = 0.0f;
              merged.alt2Tick = 0;

              auto trySetAlt2 = [&](float x, float y, float fs,
                                    float r, float g, float b, float a,
                                    unsigned long tick) {
                if (merged.hasAlt2) return;
                if (isSamePass(x, y, fs, a,
                               merged.x, merged.y, merged.fontScale, merged.a)) {
                  return;
                }
                if (isSamePass(x, y, fs, a,
                               merged.altX, merged.altY,
                               merged.altFontScale, merged.altA)) {
                  return;
                }
                merged.hasAlt2 = true;
                merged.alt2X = x;
                merged.alt2Y = y;
                merged.alt2FontScale = fs;
                merged.alt2R = r;
                merged.alt2G = g;
                merged.alt2B = b;
                merged.alt2A = a;
                merged.alt2Tick = tick;
              };

              if (existing->second.hasAlt &&
                  (nowTick - existing->second.altTick) <= kMultiPassWindowMs) {
                trySetAlt2(existing->second.altX, existing->second.altY,
                           existing->second.altFontScale, existing->second.altR,
                           existing->second.altG, existing->second.altB,
                           existing->second.altA, existing->second.altTick);
              }
              if (existing->second.hasAlt2 &&
                  (nowTick - existing->second.alt2Tick) <= kMultiPassWindowMs) {
                trySetAlt2(existing->second.alt2X, existing->second.alt2Y,
                           existing->second.alt2FontScale, existing->second.alt2R,
                           existing->second.alt2G, existing->second.alt2B,
                           existing->second.alt2A, existing->second.alt2Tick);
              }

              if (aDiff > 0.20f) {
                static DWORD s_lastAltLog = 0;
                if (kEnableRuntimeTextCapture &&
                    (nowTick - s_lastAltLog > 600)) {
                  s_lastAltLog = nowTick;
                  char mbuf[256];
                  sprintf_s(mbuf,
                            "[TextHook] INTRO multipass slc=0x%X mainA=%.2f altA=%.2f alt2=%d mainX=%.1f altX=%.1f",
                            (unsigned)slcIndex, merged.a, merged.altA,
                            merged.hasAlt2 ? 1 : 0, merged.x, merged.altX);
                  LogToFile(mbuf);
                }
              }

              g_nativeIntroMap[(uint32_t)slcIndex] = merged;
            } else {
              // Same pass sampled twice in one frame window; just refresh.
              ns.firstSeen = existing->second.firstSeen;
              ns.firstVisible = existing->second.firstVisible;
              if (existing->second.hasAlt &&
                  (nowTick - existing->second.altTick) <= kMultiPassWindowMs) {
                ns.hasAlt = true;
                ns.altX = existing->second.altX;
                ns.altY = existing->second.altY;
                ns.altFontScale = existing->second.altFontScale;
                ns.altR = existing->second.altR;
                ns.altG = existing->second.altG;
                ns.altB = existing->second.altB;
                ns.altA = existing->second.altA;
                ns.altTick = existing->second.altTick;
              }
              if (existing->second.hasAlt2 &&
                  (nowTick - existing->second.alt2Tick) <= kMultiPassWindowMs) {
                ns.hasAlt2 = true;
                ns.alt2X = existing->second.alt2X;
                ns.alt2Y = existing->second.alt2Y;
                ns.alt2FontScale = existing->second.alt2FontScale;
                ns.alt2R = existing->second.alt2R;
                ns.alt2G = existing->second.alt2G;
                ns.alt2B = existing->second.alt2B;
                ns.alt2A = existing->second.alt2A;
                ns.alt2Tick = existing->second.alt2Tick;
              }
                g_nativeIntroMap[(uint32_t)slcIndex] = ns;
              }
            } else {
              g_nativeIntroMap[(uint32_t)slcIndex] = ns;
            }
          }
        }
      } // !isCreditsHudElem (NativeIntroRenderState block)
      }
    }

#if GHOSTSKOR_OBJECTIVE_REVERSE
  // Phase 2A: configstring?萸웕y mapping now happens in Detour_SL_ConvertToString
  // (HUD KEY NAME section) using g_currentIntroSlcIdx correlation.
  // The inline SLC formula was WRONG ??HudElem.text is a configstring index,
  // NOT an SLC index, so direct table lookup gives garbage.
#endif

  // Set g_lastIntroSlcIdx BEFORE calling original ??persists after we return.
  g_lastIntroSlcIdx = slcIndex;

  // Set context for TextHook_OnIntroKey (called during SLC resolution)
  g_currentIntroSlcIdx = slcIndex;

  // Call original (renders suppressed/blank text for intro, or timer digits)
  if (Original_IntroRenderFn) {
    Original_IntroRenderFn(type, slcIndex, renderCtx, count);
  }

  FinalizeIntroCountdownOverlay(slcIndex, introCountdownState);

  g_currentIntroSlcIdx = -1;

  // NOTE: g_lastIntroSlcIdx is NOT cleared here — it must survive until
  // the convergence code calls 0x3FE560 (which happens after we return)
}
