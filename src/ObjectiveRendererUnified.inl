// ObjectiveRendererUnified.inl — Single-path objective/status renderer.
//
// INCLUDED FROM D3D11Hook.cpp (after D3D11Hook.ObjectiveSubsystem.inl)
//
// DATA SOURCES:
//   Objectives → TextHook_GetNativeObjectiveItems() [live HudElem scan]
//     - key (from SLC key map), x/y/alpha/color/fontScale (from live memory)
//     - serverTime/fxBirthTime/fxLetterTime (for typewriter)
//   Status     → g_objUnifiedStatusEvents [event queue from SLC + FSHook]
//     - Self-contained fade envelope (game alpha drops to 0 instantly)
//   Korean text → TextHook_GetLocalizedKeyText()
//
// WHY NOT SLC RDI CAPTURE:
//   - Objectives arrive as structured payloads (\state\N\str\KEY) at caller
//     0x204816, where RDI does NOT point to hudelem_s.
//   - RDI is valid only at callers 0x1F087D/0x1F1740, which the OSAUTH
//     system captures. But live HudElem scan already provides the same data.
//   - Status HudElems have alpha→0 instantly (fadeovertime target), so we
//     must use time-based envelope, not live alpha.
//
// RENDERING:
//   ONE loop for objectives (from NativeObjectiveItems)
//   ONE loop for status (from event queue)
//   ONE renderedKeys set — no double render possible.

// All headers available from parent TU (D3D11Hook.cpp).
// Utilities: CountUtf8Chars, Utf8PrefixByChars, StripColorCodesSimple, Clamp01

// Safe write to game memory (SEH-isolated, no C++ objects).
static __declspec(noinline) bool ObjUnified_SafeWriteU32(uintptr_t addr,
                                                          uint32_t value) {
  if (addr < 0x10000 || addr >= 0x7FFFFFFFFFFF) return false;
  if (IsBadWritePtr((void *)addr, sizeof(uint32_t)) != 0) return false;
  __try {
    *(volatile uint32_t *)addr = value;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// =============================================================================
// State
// =============================================================================

static std::mutex g_objUnifiedMutex;

// Status event queue (transient: "Objectives Updated", "Game Saved", etc.)
static std::deque<ObjUnifiedStatusEvent> g_objUnifiedStatusEvents;
static std::atomic<unsigned int> g_objUnifiedStatusSeq{1};

// Status envelope first-seen tracking (for fade-in/hold/fade-out timing).
struct ObjUnifiedStatusAlpha {
  DWORD firstSeen = 0;
  DWORD lastSeen = 0;
  unsigned int seq = 0;
};
static std::unordered_map<std::string, ObjUnifiedStatusAlpha> s_statusAlphaMap;

// Reset tick — events before this are stale.
static unsigned long g_objUnifiedResetTick = 0;

// Keys rendered by ObjUnified_Render() this frame — used by hint pipeline
// to suppress duplicate rendering of objectives as hints.
static std::unordered_set<std::string> g_objUnifiedRenderedKeys;

// Korean fallback text for status keys.
static const char *ObjUnified_GetKoreanFallback(const std::string &key) {
  if (key == "EXE_GAMESAVED")
    return "\xEA\xB2\x8C\xEC\x9E\x84 \xEC\xA0\x80\xEC\x9E\xA5\xEB\x90\xA8";
  if (key == "CGAME_NOW_SAVING")
    return "\xEC\xA0\x80\xEC\x9E\xA5 \xEC\xA4\x91...";
  if (key == "GAME_OBJECTIVESUPDATED")
    return "\xEB\xAA\xA9\xED\x91\x9C \xEC\x97\x85\xEB\x8D\xB0\xEC\x9D\xB4\xED\x8A\xB8.";
  if (key == "GAME_OBJECTIVECOMPLETED")
    return "\xEB\xAA\xA9\xED\x91\x9C \xEC\x99\x84\xEB\xA3\x8C.";
  if (key == "GAME_OBJECTIVEFAILED")
    return "\xEB\xAA\xA9\xED\x91\x9C \xEC\x8B\xA4\xED\x8C\xA8.";
  return "";
}

// =============================================================================
// CAPTURE: Called from SLC hook (status events only — objectives use live scan)
// =============================================================================

void ObjUnified_CaptureFromSLC(
    uintptr_t elemPtr,
    const std::string &key,
    uint32_t cfgIndex,
    uint32_t slcIndex,
    unsigned int callerOffset) {

  if (key.empty()) return;

  // Only capture STATUS keys here. Objective list items come from live scan.
  const HudKeyType keyType = ClassifyHudKey(key);
  if (keyType == HUD_KEY_OBJECTIVE_UPDATE ||
      keyType == HUD_KEY_GAME_SAVED ||
      keyType == HUD_KEY_NOW_SAVING) {
    ObjUnified_RecordStatusEvent(key, cfgIndex, "slc_capture");
  }
  // All other keys: ignored. Objectives are rendered from NativeObjectiveItems.
}

// =============================================================================
// STATUS EVENT RECORDING
// =============================================================================

unsigned int ObjUnified_RecordStatusEvent(
    const std::string &key,
    uint32_t sourceCfg,
    const char *sourceTag) {

  if (key.empty()) return 0;

  const HudKeyType keyType = ClassifyHudKey(key);
  const unsigned long now = GetTickCount();
  const unsigned int seq = g_objUnifiedStatusSeq.fetch_add(1);

  std::lock_guard<std::mutex> lock(g_objUnifiedMutex);

  // Dedup: same key within 5s → reuse existing event.
  for (const auto &ev : g_objUnifiedStatusEvents) {
    if (ev.key == key) {
      const unsigned long age =
          (now >= ev.startTick) ? (now - ev.startTick) : 0;
      if (age < 5000) {
        return ev.sequence;
      }
    }
  }

  // Prune old events (>20s).
  while (!g_objUnifiedStatusEvents.empty()) {
    const auto &front = g_objUnifiedStatusEvents.front();
    const unsigned long age =
        (now >= front.startTick) ? (now - front.startTick) : 0;
    if (age > 20000) {
      g_objUnifiedStatusEvents.pop_front();
    } else {
      break;
    }
  }

  ObjUnifiedStatusEvent ev;
  ev.key = key;
  ev.keyType = keyType;
  ev.sequence = seq;
  ev.startTick = now;
  ev.sourceCfg = sourceCfg;
  g_objUnifiedStatusEvents.push_back(ev);

  char buf[256];
  sprintf_s(buf,
            "[OBJ-UNIFIED-STATUS-EVENT] key=%s seq=%u cfg=0x%X src=%s",
            key.c_str(), seq, sourceCfg, sourceTag ? sourceTag : "?");
  LogToFile(buf);

  return seq;
}

// =============================================================================
// Status envelope timing
// =============================================================================

struct StatusEnvelopeTiming {
  float fadeInMs;
  float holdMs;
  float fadeOutMs;
};

static StatusEnvelopeTiming ObjUnified_StatusTiming(const std::string &key) {
  if (key == "EXE_GAMESAVED" || key == "CGAME_NOW_SAVING") {
    return {750.0f, 3250.0f, 500.0f};  // 4.5s total
  }
  // OBJECTIVESUPDATED, OBJECTIVECOMPLETED, OBJECTIVEFAILED
  return {750.0f, 2750.0f, 600.0f};  // 4.1s total
}

static float ObjUnified_StatusEnvelopeAlpha(float ageMs,
                                            const StatusEnvelopeTiming &t) {
  if (ageMs < t.fadeInMs) {
    return ageMs / t.fadeInMs;
  } else if (ageMs < t.fadeInMs + t.holdMs) {
    return 1.0f;
  } else if (ageMs < t.fadeInMs + t.holdMs + t.fadeOutMs) {
    return 1.0f - (ageMs - t.fadeInMs - t.holdMs) / t.fadeOutMs;
  }
  return 0.0f;
}

// =============================================================================
// RENDER: The ONE AND ONLY render path
// =============================================================================

void ObjUnified_Render(
    void *pSwapChainVoid,
    unsigned long now,
    bool suppressHudOverlay,
    bool hudPauseMenu,
    bool objectiveHiddenByGate) {

  g_objUnifiedRenderedKeys.clear();

  if (suppressHudOverlay) return;
  if (hudPauseMenu) return;
  if (objectiveHiddenByGate) return;

  // Expire stale suppressed elements (restores their latest cached native x).
  TextHook_ObjSuppressRestoreStale();
  // NOTE: TempRestore removed — x stays at 9999 permanently in memory.
  // The scanner reads cached live x from the suppress map instead,
  // eliminating the race window that caused English text flicker.

  IDXGISwapChain *pSwapChain = (IDXGISwapChain *)pSwapChainVoid;

  // ─── Run live HudElem scan ───────────────────────────────────────────
  TextHook_RunHudElemArrayScan();
  TextHook_ReadLiveHudElems();
  std::vector<NativeObjectiveItem> liveItems =
      TextHook_GetNativeObjectiveItems();

  // Diagnostic: log item count and validity (throttled).
  {
    static DWORD s_lastDiagTick = 0;
    if ((now - s_lastDiagTick) > 1500) {
      s_lastDiagTick = now;
      int validCount = 0;
      int objClassCount = 0;
      for (const auto &it : liveItems) {
        if (it.validKey) ++validCount;
        if (it.validKey && !it.key.empty()) {
          const HudKeyType kt = ClassifyHudKey(it.key);
          if (kt == HUD_KEY_OBJECTIVE_LIST || kt == HUD_KEY_OBJECTIVE_HEADER)
            ++objClassCount;
        }
      }
      char dbuf[256];
      sprintf_s(dbuf,
                "[OBJ-UNIFIED-DIAG] total=%d valid=%d objClass=%d",
                (int)liveItems.size(), validCount, objClassCount);
      LogToFile(dbuf);
      if (!liveItems.empty()) {
        for (size_t idx = 0; idx < liveItems.size() && idx < 6; ++idx) {
          const auto &it = liveItems[idx];
          char ibuf[384];
          sprintf_s(ibuf,
                    "[OBJ-UNIFIED-ITEM] [%d] key=%s valid=%d cfg=0x%X "
                    "x=%.1f y=%.1f a=%.2f fxB=%d",
                    (int)idx, it.key.empty() ? "(empty)" : it.key.c_str(),
                    it.validKey ? 1 : 0, it.cfgIndex, it.x, it.y, it.alpha,
                    it.fxBirthTime);
          LogToFile(ibuf);
        }
      }
    }
  }

  // ─── Screen coordinate setup ─────────────────────────────────────────
  UpdateScreenPlacement();
  if (!g_FrameSwapChainDescValid) return;
  const DXGI_SWAP_CHAIN_DESC &scDesc = g_FrameSwapChainDesc;

  const float screenWidth = (float)scDesc.BufferDesc.Width;
  const float screenHeight = (float)scDesc.BufferDesc.Height;
  if (screenWidth < 1.0f || screenHeight < 1.0f) return;

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

  // Helper: virtual coords → screen coords
  auto ToScreenX = [&](float vx) { return objSafeX + (vx * objScaleX); };
  auto ToScreenY = [&](float vy) { return objSafeY + (vy * objScaleY); };

  // Lane slot from native Y.
  auto LaneSlot = [](float y) -> int {
    int s = (int)lroundf((y - 10.0f) / 20.0f);
    if (s < -8) s = -8;
    if (s > 12) s = 12;
    return s;
  };

  // ─── STATUS ORACLE ─────────────────────────────────────────────────
  // ROOT FIX for wrong-line rendering and missing status Korean.
  //
  // Problem: statusLaneKey detection in texthook.cpp is unreliable.
  // When it fails, OSAUTH bridge assigns stale OBJECTIVE keys to what
  // is actually a STATUS HudElem (y~10, lane 0). Result: objective
  // Korean renders at the status position (wrong line).
  //
  // Fix: The SLC hook RELIABLY captures status keys into the event
  // queue. When a status event is active, the lowest-y HudElem with
  // alpha > 0 is showing the status message. Override its key.
  {
    std::lock_guard<std::mutex> slock(g_objUnifiedMutex);

    const int evtCount = (int)g_objUnifiedStatusEvents.size();

    // Find the OLDEST active status event (FIFO order).
    // The game displays status messages sequentially: OBJECTIVESUPDATED
    // appears first, then OBJECTIVECOMPLETED replaces it. The SLC hook
    // may capture both early (before the game displays the second one).
    // By using FIFO, we show each event for its envelope duration, then
    // advance to the next — matching the game's sequential display.
    const ObjUnifiedStatusEvent *activeStatus = nullptr;
    unsigned long activeAge = 0;
    unsigned long newestAge = 999999;
    for (auto it = g_objUnifiedStatusEvents.begin();
         it != g_objUnifiedStatusEvents.end(); ++it) {
      const unsigned long age =
          (now >= it->startTick) ? (now - it->startTick) : 0;
      if (newestAge > age) newestAge = age;
      // Each event has an envelope duration based on its type.
      const StatusEnvelopeTiming timing = ObjUnified_StatusTiming(it->key);
      const unsigned long displayDur =
          (unsigned long)(timing.fadeInMs + timing.holdMs + timing.fadeOutMs);
      if (age < displayDur && !activeStatus) {
        activeStatus = &(*it);
        activeAge = age;
      }
    }

    // ── Phantom filter helper ──
    // Real objective/status HudElems have x > 0 (typically 6.0+).
    // Phantom HudElems (timers, counters, icons) have x=0.0 and appear at
    // y=-68.0 (lane=-4, offscreen) or y=120.0 (lane=6, below objectives).
    // These pollute the "lowest-y" search and block the oracle for 90+ seconds.
    auto IsRealObjItem = [](const NativeObjectiveItem &it) -> bool {
      return it.x > 0.5f && it.y >= -1.0f;
    };

    // Diagnostic: log oracle state every 2s regardless of outcome.
    {
      static DWORD s_oracleDiagTick = 0;
      if ((now - s_oracleDiagTick) > 2000) {
        s_oracleDiagTick = now;
        float diagLowestY = 99999.0f;
        float diagLowestAlpha = 0.0f;
        int diagLowestLane = -99;
        std::string diagLowestKey;
        int realCount = 0;
        int phantomCount = 0;
        for (size_t i = 0; i < liveItems.size(); ++i) {
          if (!IsRealObjItem(liveItems[i])) { ++phantomCount; continue; }
          ++realCount;
          if (liveItems[i].y < diagLowestY) {
            diagLowestY = liveItems[i].y;
            diagLowestAlpha = liveItems[i].alpha;
            diagLowestLane = LaneSlot(liveItems[i].y);
            diagLowestKey = liveItems[i].key;
          }
        }
        char dbuf[512];
        sprintf_s(dbuf,
                  "[STATUS-ORACLE-DIAG] evts=%d newestAge=%lums "
                  "activeEvt=%s items=%d(real=%d,phantom=%d) "
                  "lowestY=%.1f lowestA=%.2f lowestLane=%d lowestKey=%s",
                  evtCount, newestAge,
                  activeStatus ? activeStatus->key.c_str() : "(none)",
                  (int)liveItems.size(), realCount, phantomCount,
                  diagLowestY, diagLowestAlpha,
                  diagLowestLane,
                  diagLowestKey.empty() ? "(empty)" : diagLowestKey.c_str());
        LogToFile(dbuf);
      }
    }

    if (activeStatus) {
      // Find the lowest-y REAL HudElem with alpha > 0.
      // Phantom items (x=0, y<0) are excluded.
      int bestIdx = -1;
      float bestY = 99999.0f;
      for (size_t i = 0; i < liveItems.size(); ++i) {
        if (!IsRealObjItem(liveItems[i])) continue;
        if (liveItems[i].alpha > 0.01f && liveItems[i].y < bestY) {
          bestY = liveItems[i].y;
          bestIdx = (int)i;
        }
      }

      if (bestIdx >= 0) {
        const int lane = LaneSlot(liveItems[bestIdx].y);
        // Only override at lane 0 (y ~ 10) — status always at top.
        if (lane == 0) {
          auto &target = liveItems[bestIdx];
          const HudKeyType curKt =
              (target.validKey && !target.key.empty())
                  ? ClassifyHudKey(target.key)
                  : HUD_KEY_UNKNOWN;
          const bool alreadyStatus =
              (curKt == HUD_KEY_OBJECTIVE_UPDATE ||
               curKt == HUD_KEY_GAME_SAVED ||
               curKt == HUD_KEY_NOW_SAVING);

          // Only override items with NO valid key (SLC cache miss).
          // If the direct resolution already gave a key (objective or status),
          // trust it — it reflects the game's CURRENT text, not stale events.
          if (!alreadyStatus && (!target.validKey || target.key.empty())) {
            const std::string oldKey = target.key;
            target.key = activeStatus->key;
            target.validKey = true;

            static DWORD s_oracleLogTick = 0;
            if ((now - s_oracleLogTick) > 1200) {
              s_oracleLogTick = now;
              char obuf[448];
              sprintf_s(obuf,
                        "[STATUS-ORACLE] Override lane-0: oldKey=%s -> "
                        "newKey=%s y=%.1f cfg=0x%X age=%lums",
                        oldKey.empty() ? "(empty)" : oldKey.c_str(),
                        activeStatus->key.c_str(), target.y,
                        target.cfgIndex, activeAge);
              LogToFile(obuf);
            }
          }
        } else {
          // Log: lane mismatch (not lane 0).
          static DWORD s_oracleLaneMissTick = 0;
          if ((now - s_oracleLaneMissTick) > 2000) {
            s_oracleLaneMissTick = now;
            char lbuf[256];
            sprintf_s(lbuf,
                      "[STATUS-ORACLE-MISS] lane=%d (not 0) y=%.1f "
                      "key=%s evt=%s",
                      lane, liveItems[bestIdx].y,
                      liveItems[bestIdx].key.empty()
                          ? "(empty)"
                          : liveItems[bestIdx].key.c_str(),
                      activeStatus->key.c_str());
            LogToFile(lbuf);
          }
        }
      } else {
        // Log: no real items with alpha > 0.
        static DWORD s_oracleNoItemTick = 0;
        if ((now - s_oracleNoItemTick) > 2000) {
          s_oracleNoItemTick = now;
          int realCnt = 0, phantomCnt = 0;
          for (size_t qi = 0; qi < liveItems.size(); ++qi) {
            if (IsRealObjItem(liveItems[qi])) ++realCnt;
            else ++phantomCnt;
          }
          char nbuf[320];
          sprintf_s(nbuf,
                    "[STATUS-ORACLE-MISS] no items (total=%d real=%d "
                    "phantom=%d) evt=%s age=%lums",
                    (int)liveItems.size(), realCnt, phantomCnt,
                    activeStatus->key.c_str(), activeAge);
          LogToFile(nbuf);
        }
      }
    }
  }

  // ─── SINGLE DEDUP SET ────────────────────────────────────────────────
  std::unordered_set<std::string> renderedKeys;
  renderedKeys.reserve(liveItems.size() + 8);

  // =====================================================================
  // UNIFIED RENDER: Objectives AND Status from live NativeObjectiveItems
  // =====================================================================
  //
  // ALL rendering goes through the live HudElem path. Status messages
  // now have their key assigned via STATUS-BRIDGE in texthook.cpp, so
  // they appear here with live position, alpha, and typewriter data.
  //
  // This eliminates:
  //   - Ghost subtitles (no HudElem → no render)
  //   - Wrong position (uses live HudElem y, matches English)
  //   - Status/objective overlap (game manages HudElem positions)
  //   - Timing mismatch (uses live alpha, matches English timing)
  //
  // Per-key dedup: same key, multiple HudElems → keep latched lane,
  // or highest alpha with lowest lane as tiebreaker.
  // Lane occupation: status items at a lane block objectives there.

  // Lane latch: remembers which lane a key previously rendered at.
  // Prevents visual jumping when the game reassigns HudElems.
  static std::unordered_map<std::string, int> s_keyLaneLatch;
  {
    static unsigned long s_latchResetTick = 0;
    const unsigned long rTick = TextHook_GetObjectiveRuntimeResetTick();
    if (rTick != 0 && rTick != s_latchResetTick) {
      s_latchResetTick = rTick;
      s_keyLaneLatch.clear();
    }
    // Prune entries for keys not in current live items.
    if (!s_keyLaneLatch.empty() && liveItems.empty()) {
      s_keyLaneLatch.clear();
    }
  }

  struct ObjCandidate {
    size_t itemIdx;
    int laneSlot;
    float alpha;
    bool isStatus;
  };
  std::unordered_map<std::string, ObjCandidate> bestByKey;
  bestByKey.reserve(liveItems.size());

  for (size_t i = 0; i < liveItems.size(); ++i) {
    const auto &item = liveItems[i];
    if (!item.validKey || item.key.empty()) continue;

    const HudKeyType kt = ClassifyHudKey(item.key);
    // Items in liveItems already passed isObjLane scoring (score≥100,
    // x≈6 y≈10-70 fs≈1.25 flags=0x5) — they ARE objectives even if
    // their key name doesn't match known _OBJ_ patterns (e.g.
    // CLOCKWORK_DISABLE_THE_SECURITY).  Only exclude types that
    // definitely don't belong in the objective overlay.
    // HUD_KEY_HINT: hint keys (e.g. SKYWAY_HINT_PICK_UP_GUN) may appear
    // in objective-lane HudElems, but they belong exclusively in the
    // HUDHINT renderer — rendering here causes double render.
    if (kt == HUD_KEY_EXCLUDE || kt == HUD_KEY_SUBTITLE ||
        kt == HUD_KEY_MENU || kt == HUD_KEY_INTRO ||
        kt == HUD_KEY_HINT) continue;
    // Center-screen gameplay prompts (y < -20) with generic OBJ_HUD_HINT
    // classification enter through isObjectiveNotificationLane but are NOT
    // objectives or status — they are movement tutorial hints (e.g.
    // DEER_HUNT_CROUCH_TOGGLE "Press C to crouch") with center-screen
    // alignment.  ToScreenY() on their negative virtual Y produces
    // offscreen output while native text gets suppressed via x=9999.
    // Let them fall through to the HUDHINT pipeline or render natively.
    if (kt == HUD_KEY_OBJ_HUD_HINT && item.y < -20.0f) continue;
    const bool isStat = (kt == HUD_KEY_OBJECTIVE_UPDATE ||
                         kt == HUD_KEY_GAME_SAVED ||
                         kt == HUD_KEY_NOW_SAVING);
    const bool isObj = !isStat;

    const int lane = LaneSlot(item.y);
    auto it = bestByKey.find(item.key);
    if (it == bestByKey.end()) {
      bestByKey[item.key] = {i, lane, item.alpha, isStat};
    } else {
      // Lane latch: prefer the previously-rendered lane if still valid.
      auto latchIt = s_keyLaneLatch.find(item.key);
      const int latchedLane =
          (latchIt != s_keyLaneLatch.end()) ? latchIt->second : -999;
      const bool currentIsLatched = (it->second.laneSlot == latchedLane);
      const bool newIsLatched = (lane == latchedLane);

      if (newIsLatched && !currentIsLatched && item.alpha > 0.01f) {
        // New candidate is at the latched lane, prefer it.
        it->second = {i, lane, item.alpha, isStat};
      } else if (!currentIsLatched && !newIsLatched) {
        // Neither is latched, use normal tiebreaker.
        if (item.alpha > it->second.alpha ||
            (item.alpha == it->second.alpha &&
             lane < it->second.laneSlot)) {
          it->second = {i, lane, item.alpha, isStat};
        }
      }
      // If current is latched and new is not, keep current.
    }
  }

  // Track which lanes are occupied by status items (Rule 4: no overlap).
  std::unordered_set<int> statusOccupiedLanes;
  for (const auto &kv : bestByKey) {
    if (kv.second.isStatus &&
        liveItems[kv.second.itemIdx].alpha > 0.01f) {
      statusOccupiedLanes.insert(kv.second.laneSlot);
    }
  }

  for (const auto &kv : bestByKey) {
    const auto &item = liveItems[kv.second.itemIdx];
    if (item.alpha <= 0.01f) continue;

    // Lane occupation: objectives skip lanes occupied by status (Rule 4).
    if (!kv.second.isStatus &&
        statusOccupiedLanes.count(kv.second.laneSlot)) {
      continue;
    }

    // Korean text lookup.
    std::string korean, english;
    bool gotText = TextHook_GetLocalizedKeyText(item.key, korean, english);
    if (!gotText || korean.empty()) {
      // Status keys may not be in translation store — use fallback.
      const char *fb = ObjUnified_GetKoreanFallback(item.key);
      if (fb[0]) {
        korean = fb;
        gotText = true;
      }
    }
    if (!gotText || korean.empty()) continue;

    std::string displayText =
        BindingResolver::ResolveBindingsInText(
            StripColorCodesSimple(korean),
            StripColorCodesSimple(english));
    if (displayText.empty()) continue;

    // Remove newlines.
    for (char &c : displayText) {
      if (c == '\n' || c == '\r') c = ' ';
    }

    // ── Game variable substitution (&&1, &&2, ...) ──
    // Priority 1: Read +0x80 (value field) from live HudElem memory (native)
    // Priority 2: R_AddCmdDrawText prefix matching (LUI / fallback)
    if (displayText.find("&&") != std::string::npos) {
      for (int pi = 1; pi <= 3; ++pi) {
        std::string placeholder = "&&" + std::to_string(pi);
        size_t ppos = displayText.find(placeholder);
        if (ppos == std::string::npos) continue;
        int resolvedVal = -1;
        // Read live configstring raw text — the game script writes
        // formatted text (with number) directly to the configstring.
        if (pi == 1 && item.cfgIndex != 0) {
          extern std::string TextHook_ReadLiveConfigString(uint32_t cfgIdx);
          std::string rawCfg = TextHook_ReadLiveConfigString(item.cfgIndex);
          if (!rawCfg.empty()) {
            size_t bracket = rawCfg.find("[ ");
            if (bracket != std::string::npos) {
              size_t ns = bracket + 2;
              while (ns < rawCfg.size() && rawCfg[ns] == ' ') ns++;
              size_t ne = ns;
              while (ne < rawCfg.size() && rawCfg[ne] >= '0' &&
                     rawCfg[ne] <= '9') ne++;
              if (ne > ns) {
                resolvedVal = atoi(rawCfg.substr(ns, ne - ns).c_str());
              }
            }
          }
        }
        if (resolvedVal >= 0) {
          displayText.replace(ppos, placeholder.size(),
                              std::to_string(resolvedVal));
        } else {
          displayText.erase(ppos, placeholder.size());
        }
      }
    }

    // ── Typewriter effect ──
    // Uses game server clock: serverTime (current) vs fxBirthTime (start).
    // fxLetterTime = ms per character.  Use the original English speed directly
    // so Korean text appears at the same per-character pace as the native text.
    int rawVisibleChars = -1;
    const size_t totalKorChars = CountUtf8Chars(displayText);
    if (item.fxLetterTime > 0 && item.fxBirthTime > 0 && totalKorChars > 0) {
      const bool clockPlausible =
          (item.serverTime > 0 &&
           item.serverTime >= item.fxBirthTime &&
           (item.serverTime - item.fxBirthTime) <= 20000);

      if (clockPlausible) {
        rawVisibleChars =
            (item.serverTime - item.fxBirthTime) / item.fxLetterTime;
        if (rawVisibleChars <= 0) {
          continue;  // typewriter not started yet
        }
        if ((size_t)rawVisibleChars < totalKorChars) {
          displayText =
              Utf8PrefixByChars(displayText, (size_t)rawVisibleChars);
        }
      }
    }

    // Status items: now resolved directly via live_status_slc_direct, so
    // the "writes wrong text with typewriter then clears" bug is fixed at
    // the key resolution level.  Let status messages fall through to the
    // synthetic typewriter below for correct character-by-character effect.

    // Synthetic typewriter fallback: if native clock didn't fire but we have
    // fxLetterTime, use a wall-clock based typewriter from first-seen time.
    if (rawVisibleChars == -1 && item.fxLetterTime > 0 && totalKorChars > 0) {
      struct SynthTypeEntry {
        DWORD firstSeen;
        int fxBirth;
      };
      static std::unordered_map<std::string, SynthTypeEntry> s_synthType;
      {
        static unsigned long s_synthResetTick = 0;
        const unsigned long rTick2 = TextHook_GetObjectiveRuntimeResetTick();
        if (rTick2 != 0 && rTick2 != s_synthResetTick) {
          s_synthResetTick = rTick2;
          s_synthType.clear();
        }
      }
      for (auto it2 = s_synthType.begin(); it2 != s_synthType.end();) {
        if ((now - it2->second.firstSeen) > 30000)
          it2 = s_synthType.erase(it2);
        else
          ++it2;
      }

      const std::string typeKey =
          item.key + "|" + std::to_string(item.fxBirthTime);
      auto itT = s_synthType.find(typeKey);
      if (itT == s_synthType.end()) {
        s_synthType[typeKey] = {now, item.fxBirthTime};
        itT = s_synthType.find(typeKey);
      } else if (itT->second.fxBirth != item.fxBirthTime) {
        itT->second = {now, item.fxBirthTime};
      }

      const DWORD elapsed =
          (now >= itT->second.firstSeen) ? (now - itT->second.firstSeen) : 0;
      const unsigned long synthLetterTime =
          (item.fxLetterTime > 0) ? (unsigned long)item.fxLetterTime : 50;

      const int synthVisible = (int)(elapsed / synthLetterTime);
      if (synthVisible <= 0) continue;
      if ((size_t)synthVisible < totalKorChars) {
        displayText = Utf8PrefixByChars(displayText, (size_t)synthVisible);
      }
      rawVisibleChars = synthVisible;
    }

    if (displayText.empty()) continue;

    // ── Status lane displacement guard ──
    // When a status item (GAME_OBJECTIVESUPDATED etc.) is pushed to
    // lane >= 5 with oversized scale (>= 1.5), it was displaced by
    // another status occupying lane 0.  Suppress to avoid rendering
    // at an unexpected position (e.g. left-center of screen).
    if (kv.second.isStatus && kv.second.laneSlot >= 5 && item.fontScale >= 1.5f) {
      continue;
    }

    // ── Compute screen coordinates from live HudElem ──
    float sx = ToScreenX(item.x);
    float sy = ToScreenY(item.y);
    const float scale =
        (item.fontScale > 0.02f && item.fontScale < 10.0f)
            ? item.fontScale : 1.25f;
    const float resMul = screenHeight / 1080.0f;
    const float fontH = 78.0f * resMul;

    // Color from live HudElem (same for objective and status — Rule 2).
    const float alpha = Clamp01(item.alpha);
    float color[4] = {
        (float)((item.colorPacked >> 0) & 0xFF) / 255.0f,
        (float)((item.colorPacked >> 8) & 0xFF) / 255.0f,
        (float)((item.colorPacked >> 16) & 0xFF) / 255.0f,
        alpha};

    const float width =
        KoreanRenderer::MeasureTextWidthEx(displayText, fontH, scale);
    KoreanRenderer::QueueText(
        displayText, sx, sy, scale, color, fontH,
        0, width, false, false, 1.5f, false, false, true, 0,
        screenWidth * 0.86f, true, true);

    renderedKeys.insert(item.key);

    // ── Suppress native English rendering ──
    // Register this HudElem for x-coordinate offscreen suppression.
    // Type stays 1 so the engine still processes effects (typewriter sounds).
    // ObjSuppressElem applies x=9999 immediately — no deferred step needed.
    // Use item.x (the current native x, patched by scanner from suppress map)
    // instead of reading from memory (which is already 9999 for suppressed elems).
    if (item.rawPtr != 0) {
      uint32_t origXBits = 0;
      memcpy(&origXBits, &item.x, sizeof(uint32_t));
      TextHook_ObjSuppressElem(item.rawPtr, origXBits);
    }

    // Update lane latch AFTER successful render.
    s_keyLaneLatch[item.key] = kv.second.laneSlot;

    // Throttled log.
    static std::unordered_map<std::string, DWORD> s_renderLog;
    const std::string logKey =
        item.key + "|" + std::to_string(kv.second.laneSlot);
    auto itLog = s_renderLog.find(logKey);
    if (itLog == s_renderLog.end() || (now - itLog->second) > 800) {
      s_renderLog[logKey] = now;
      char buf[448];
      sprintf_s(buf,
                "[OBJ-UNIFIED-RENDER] key=%s lane=%d x=%.1f y=%.1f "
                "a=%.2f vc=%d srv=%d fxB=%d fxL=%d scale=%.2f type=%s",
                item.key.c_str(), kv.second.laneSlot, sx, sy,
                alpha, rawVisibleChars, item.serverTime,
                item.fxBirthTime, item.fxLetterTime, scale,
                kv.second.isStatus ? "STATUS" : "OBJ");
      LogToFile(buf);
    }
  }

  // =====================================================================
  // PART 2 REMOVED — Status now renders through unified HudElem path above.
  // =====================================================================
  //
  // The synthetic event-queue status rendering (hardcoded positions,
  // synthetic alpha envelope, 5s timer) has been removed because it caused:
  //   1. Ghost subtitles: events fire on SLC capture even when no HudElem
  //      is visible (total=0 in live scan → nothing on screen)
  //   2. Wrong position: hardcoded y=10.0 instead of live HudElem position
  //   3. Status/objective overlap: both at y=76.5 when objective is at lane 0
  //   4. Timing mismatch: 5s synthetic envelope vs game's actual fade timing
  //
  // Status now uses the live HudElem path (STATUS-BRIDGE in texthook.cpp):
  //   - Position: live HudElem y → matches English exactly (Rule 1)
  //   - Alpha: live HudElem alpha → matches English timing (Rule 6)
  //   - Color: live HudElem color → same as objective (Rule 2)
  //   - No HudElem → no render → no ghosts (Rule 7)
  //   - Lane occupation prevents overlap (Rule 4)

  // NOTE: ReapplyOffscreen removed — x=9999 is now applied immediately
  // in TextHook_ObjSuppressElem, so no end-of-frame reapply is needed.
  // This eliminates the TempRestore→ReapplyOffscreen race window.

  // Publish rendered keys for cross-pipeline dedup (hint pipeline).
  g_objUnifiedRenderedKeys = std::move(renderedKeys);
}

// =============================================================================
// Per-frame live update (called externally if needed — now done inside Render)
// =============================================================================

void ObjUnified_UpdateFromLiveHudElems() {
  // No-op: live scan is done inside ObjUnified_Render() now.
}

// =============================================================================
// RESET / MAP LOAD
// =============================================================================

void ObjUnified_Reset() {
  // Restore all suppressed HudElems before clearing.
  TextHook_ObjSuppressClearAll();
  std::lock_guard<std::mutex> lock(g_objUnifiedMutex);
  g_objUnifiedStatusEvents.clear();
  g_objUnifiedResetTick = GetTickCount();
  s_statusAlphaMap.clear();
  LogToFile("[OBJ-UNIFIED-RESET]");
}

void ObjUnified_OnMapLoad() {
  ObjUnified_Reset();
}

// =============================================================================
// SNAPSHOTS (debug only)
// =============================================================================

std::vector<ObjUnifiedItem> ObjUnified_GetItemsSnapshot() {
  // Items are now sourced from NativeObjectiveItems, not our own map.
  return {};
}

std::vector<ObjUnifiedStatusEvent> ObjUnified_GetStatusEventsSnapshot() {
  std::lock_guard<std::mutex> lock(g_objUnifiedMutex);
  return {g_objUnifiedStatusEvents.begin(), g_objUnifiedStatusEvents.end()};
}

// Returns true if ObjUnified_Render() rendered this key in the current frame.
// Called by the hint pipeline to suppress duplicate rendering.
bool ObjUnified_WasKeyRenderedThisFrame(const std::string &key) {
  return g_objUnifiedRenderedKeys.find(key) != g_objUnifiedRenderedKeys.end();
}

// =============================================================================
// Legacy compatibility: D3D11Hook_SignalMissionRestart
// =============================================================================

void D3D11Hook_SignalMissionRestart() {
  ObjUnified_Reset();
}
