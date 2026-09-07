static bool IsCodeReady(void *addr) {
  if (!addr || !GameBuild::RuntimeReady()) return false;
  // This build's ten-argument DrawText wrapper; the old implementation was
  // split into wrappers and a differently typed command-buffer routine.
  const unsigned char expected[] = {0x48, 0x83, 0xEC, 0x68};
  return memcmp(addr, expected, sizeof(expected)) == 0;
}
static bool IsProbablyExecutableCode(void *addr) {
  if (!addr)
    return false;

  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0)
    return false;
  if (mbi.State != MEM_COMMIT)
    return false;
  if ((mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD))
    return false;

  DWORD p = (mbi.Protect & 0xFF);
  bool executable = (p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
                     p == PAGE_EXECUTE_READWRITE ||
                     p == PAGE_EXECUTE_WRITECOPY);
  if (!executable)
    return false;

  unsigned char *bytes = (unsigned char *)addr;
  bool allFF = true;
  for (int i = 0; i < 8; ++i) {
    if (bytes[i] != 0xFF) {
      allFF = false;
      break;
    }
  }
  if (allFF)
    return false;

  // Accept common function prologues and thunks.
  bool match1 = (bytes[0] == 0x48 && bytes[1] == 0x83 && bytes[2] == 0xEC);
  bool match2 = (bytes[0] == 0x48 && bytes[1] == 0x81 && bytes[2] == 0xEC);
  bool match3 = (bytes[0] == 0x40 && bytes[1] == 0x53);
  bool match4 = (bytes[0] == 0x48 && bytes[1] == 0x89);
  bool match5 = (bytes[0] == 0xE9);
  bool leaf = bytes[0] == 0x48 && (bytes[1] == 0x63 || bytes[1] == 0x8D);
  return match1 || match2 || match3 || match4 || match5 || leaf;
}


// =============================================================================
// CG_DrawHudElem Hook — enforces x=9999 for suppressed objective elements
// BEFORE CG_DrawHudElem reads x into a local variable, eliminating the
// per-frame race where the engine overwrites x between our Present writes.
// The function runs normally (typewriter sounds play), just at x=9999.
// =============================================================================
static void *Original_CG_DrawHudElem = nullptr;
static bool g_CG_DrawHudElem_Hooked = false;

// Scan backward from a known address inside a function to find the function
// start, identified by CC (int3) padding inserted by MSVC between functions.
static uintptr_t ScanBackForFunctionStart(uintptr_t addrInsideFunction,
                                          int maxScanBack = 0x1000) {
  __try {
    const unsigned char *p = (const unsigned char *)addrInsideFunction;
    for (int i = 1; i < maxScanBack; i++) {
      if (p[-i] == 0xCC && p[-i + 1] != 0xCC) {
        return addrInsideFunction - i + 1;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Memory access violation during scan
  }
  return 0;
}

// Identify which of RCX/RDX is the hudelem_s pointer by checking if it falls
// within the known HudElem array region.  Returns 0 if neither matches.
static uintptr_t IdentifyHudElemPtr(uintptr_t rcx, uintptr_t rdx) {
  static const uintptr_t s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  const uintptr_t arrayBase =
      s_moduleBase + IW6Offsets::HudElem_Array_SP;
  const uintptr_t arrayEnd =
      arrayBase +
      (uintptr_t)IW6Offsets::HudElem_Array_Stride_SP * 2048;

  uintptr_t params[2] = {rcx, rdx};
  for (int i = 0; i < 2; i++) {
    uintptr_t p = params[i];
    if (p >= arrayBase && p < arrayEnd &&
        ((p - arrayBase) % IW6Offsets::HudElem_Array_Stride_SP) == 0) {
      return p;
    }
  }
  return 0;
}

// Slot-reuse detection for timescript persist suppress.
// Countdown timers use specific text configstring values at +0xA0.
// If the HudElem's text configstring changes to a value outside this set,
// the slot has been repurposed (e.g., for "File: gloaming.ogg" debug text).
static bool IsKnownTimeScriptTextCfg(uint32_t textCfg) {
  // Values observed at HudElem+0xA0 while the timer element is active.
  // This whitelist prevents the text-based slot-reuse check from falsely
  // deactivating suppress on the timer itself.  Slot reuse by non-timer
  // elements (gauge bars, objectives) is caught by the LABEL check
  // (originalLabel vs curLabel at +0x40), which is the primary guard.
  return textCfg == 0 || textCfg == 0x11 || textCfg == 0x18 ||
         textCfg == 0x1D || textCfg == 0x23 || textCfg == 0x24 ||
         textCfg == 0x3B || textCfg == 0x3C;
}

void __fastcall Detour_CG_DrawHudElem(uintptr_t rcx, uintptr_t rdx,
                                      uintptr_t r8, uintptr_t r9) {

  // --- Identify hudelem_s pointer (always, not just when suppressing) ---
  const uintptr_t elemPtr = IdentifyHudElemPtr(rcx, rdx);

  // Cache native coordinates BEFORE any suppression writes x=9999.
  if (elemPtr != 0) {
    float ex = *(volatile float *)(elemPtr + 0x04);
    float ey = *(volatile float *)(elemPtr + 0x08);
    float fs = *(volatile float *)(elemPtr + 0x14);
    if (std::isfinite(ex) && std::isfinite(ey) && std::isfinite(fs) &&
        fs > 0.01f && fs < 6.0f &&
        ex > -2000.0f && ex < 2000.0f &&
        ey > -2000.0f && ey < 2000.0f) {
      CgDrawHudElemCapture cap{};
      cap.x = ex;
      cap.y = ey;
      cap.fontScale = fs;
      cap.alignOrg = *(volatile uint32_t *)(elemPtr + 0x28);
      cap.alignScreen = *(volatile uint32_t *)(elemPtr + 0x2C);
      cap.colorPacked = *(volatile uint32_t *)(elemPtr + 0x30);
      cap.tick = GetTickCount();
      TextHook_StoreCgDrawElemPtrCapture(elemPtr, cap);
    }
  }

  const bool hasObj =
      g_hasObjSuppressed.load(std::memory_order_relaxed);
  const bool hasTS = g_TimeScriptPersistSuppress.active;
  // Detection alone must not hide the native digits. Wait until the overlay
  // has a fresh, computed countdown (including its number).
  const bool hasTimerReplacement =
      hasTS && TextHook_HasTimeScriptTimerReplacement();

  if (hasObj || hasTS) {
    // --- Objective suppress: x = +9999 (scan rcx/rdx) ---
    if (hasObj) {
      uintptr_t params[2] = {rcx, rdx};
      for (int pi = 0; pi < 2; pi++) {
        uintptr_t ptr = params[pi];
        if (ptr < 0x10000 || ptr >= 0x7FFFFFFFFFFF) continue;
        std::lock_guard<std::mutex> lk(g_objSuppressMutex);
        auto it = g_objSuppressedElems.find(ptr);
        if (it != g_objSuppressedElems.end()) {
          // Check freshness: only suppress if the objective renderer
          // refreshed this entry recently.  Do NOT refresh the tick here
          // — only the renderer should refresh.  This prevents stale
          // entries from being kept alive when the game repurposes the
          // HudElem slot for a different element (e.g., gauge bar).
          if ((GetTickCount() - it->second.lastRefreshTick) > 2000) {
            g_objSuppressedElems.erase(it);
          } else {
            const uint32_t liveXBits = *(volatile uint32_t *)(ptr + 0x04);
            float liveX = 0.0f;
            memcpy(&liveX, &liveXBits, sizeof(float));
            if (std::isfinite(liveX) && liveX > -2000.0f && liveX < 2000.0f) {
              it->second.currentXBits = liveXBits;
            }
            static const float kOff = 9999.0f;
            uint32_t offBits;
            memcpy(&offBits, &kOff, sizeof(uint32_t));
            *(volatile uint32_t *)(ptr + 0x04) = offBits;
          }
          break;
        }
      }
    }

    // --- TimeScript suppress: x = +9999 (using canonical elemPtr) ---
    if (hasTS && elemPtr != 0) {
      static const float kOff = 9999.0f;
      uint32_t offBits;
      memcpy(&offBits, &kOff, sizeof(uint32_t));

      if (elemPtr == g_TimeScriptPersistSuppress.elemPtr) {
        uint32_t curText = *(volatile uint32_t *)(elemPtr + 0xA0);
        uint32_t curLabel = *(volatile uint32_t *)(elemPtr + 0x40);

        // Slot-reuse detection: deactivate suppress when the HudElem
        // slot has been repurposed for a different element.
        //
        // Guard 1: original label changed.
        // Guard 2: when we never recorded a label, fall back to the known
        // +0xA0 timescript whitelist from the SSOT.  0x23/0x24/0x3B/0x3C can
        // legitimately appear on timer HudElems, so we must only deactivate
        // when +0xA0 is non-zero and outside that whitelist.
        const bool labelKnown =
            (g_TimeScriptPersistSuppress.originalLabel != 0);
        const bool labelChanged =
            labelKnown &&
            curLabel != g_TimeScriptPersistSuppress.originalLabel;
        const bool textReused =
            !labelKnown &&
            curText != 0 &&
            !IsKnownTimeScriptTextCfg(curText);

        if (labelChanged || textReused) {
          g_TimeScriptPersistSuppress.active = false;
          g_TimeScriptPersistSuppress.deactivatedElemPtr = elemPtr;
          g_TimeScriptPersistSuppress.deactivatedTick = GetTickCount();
          {
            static DWORD s_lastDeactLog = 0;
            DWORD _dnow = GetTickCount();
            if ((_dnow - s_lastDeactLog) > 500) {
              s_lastDeactLog = _dnow;
              char _dm[256];
              sprintf_s(_dm,
                        "[TIMESCRIPT-SUPPRESS-DEACT] elemPtr=0x%llX "
                        "labelChanged=%d textReused=%d curText=0x%X "
                        "curLabel=0x%X origLabel=0x%X",
                        (unsigned long long)elemPtr,
                        labelChanged ? 1 : 0, textReused ? 1 : 0, curText,
                        curLabel, g_TimeScriptPersistSuppress.originalLabel);
              LogToFile(_dm);
            }
          }
        } else if (hasTimerReplacement) {
          // Suppress regardless of current type.  During SATFARM cutscenes
          // the timer HudElem type switches from TIMER_DOWN(5) to TEXT(1)
          // or TIMER_UP(4) for extended periods while the slot is still
          // our timer.  Checking originalType here caused English to leak
          // through for the entire cutscene duration.
          *(volatile uint32_t *)(elemPtr + 0x04) = offBits;  // x=9999
          g_TimeScriptPersistSuppress.lastSuppressTick = GetTickCount();
        }
      }
      // Companion: timer-value HudElem sharing the same countdown end time.
      else if (hasTimerReplacement &&
               g_TimeScriptPersistSuppress.cachedTimeField > 30000) {
        uint32_t timeField = *(volatile uint32_t *)(elemPtr + 0x78);
        if (timeField == g_TimeScriptPersistSuppress.cachedTimeField) {
          *(volatile uint32_t *)(elemPtr + 0x04) = offBits;  // x=9999
        }
      }
      else {
        // elemPtr doesn't match suppress target and no companion match.
        // Log periodically to diagnose post-transition mismatch.
        static DWORD s_lastMissLog = 0;
        DWORD _tnow = GetTickCount();
        if ((_tnow - s_lastMissLog) > 900) {
          s_lastMissLog = _tnow;
          char _mb[256];
          sprintf_s(_mb, "[TIMESCRIPT-SUPPRESS-MISS] got=0x%llX "
                    "want=0x%llX cachedTF=%u",
                    (unsigned long long)elemPtr,
                    (unsigned long long)g_TimeScriptPersistSuppress.elemPtr,
                    g_TimeScriptPersistSuppress.cachedTimeField);
          LogToFile(_mb);
        }
      }
    }
  }

  // --- Label-based auto-registration for timer (즉시 이어하기 path) ---
  // When IAR-DIRECT never fires (e.g., checkpoint load mid-cutscene),
  // g_TimeScriptPersistSuppress is never activated.  Detect the timer
  // HudElem by its label configstring and self-register so suppression
  // applies even without an IAR-DIRECT scan.
  // Only auto-register when suppression is completely inactive.  If
  // g_TimeScriptPersistSuppress.active is already true (IAR-DIRECT ran and
  // registered the correct elemPtr), never overwrite — another HudElem
  // sharing the same label configstring would corrupt the established
  // registration and break suppression for the whole session.
  if (elemPtr != 0 && !g_TimeScriptPersistSuppress.active) {
    // Cooldown: don't re-register the same elemPtr that was just deactivated
    // due to slot reuse.  The stale timer type/label on the repurposed slot
    // would cause a false re-registration loop.
    const DWORD _cdNow = GetTickCount();
    const bool inCooldown =
        (g_TimeScriptPersistSuppress.deactivatedElemPtr == elemPtr &&
         (_cdNow - g_TimeScriptPersistSuppress.deactivatedTick) < 5000);
    if (inCooldown) {
      static DWORD s_lastCdLog = 0;
      if ((_cdNow - s_lastCdLog) > 2000) {
        s_lastCdLog = _cdNow;
        char _cdm[256];
        sprintf_s(_cdm,
                  "[TIMESCRIPT-COOLDOWN-BLOCK] autoreg blocked "
                  "elemPtr=0x%llX age=%ums",
                  (unsigned long long)elemPtr,
                  _cdNow - g_TimeScriptPersistSuppress.deactivatedTick);
        LogToFile(_cdm);
      }
      goto skip_auto_reg;
    }
    uint32_t label = 0;
    bool isSatfarmLabel = false, isClockworkLabel = false;
    if (TryReadIntroU32(elemPtr, 0x40, label)) {
      isSatfarmLabel = (label == 0x3B || label == 0x3C);
      // 0x23 is a low configstring index — require timer type to avoid
      // false matches in non-CLOCKWORK missions.
      if (label == 0x23) {
        uint32_t eType = 0;
        TryReadIntroU32(elemPtr, 0x00, eType);
        isClockworkLabel = IsHudElemTimerType(eType);
      }
    }
    if (isSatfarmLabel || isClockworkLabel) {
      uint32_t curText = 0;
      TryReadIntroU32(elemPtr, 0xA0, curText);
      if (curText == 0 || IsKnownTimeScriptTextCfg(curText)) {
        g_TimeScriptPersistSuppress.elemPtr = elemPtr;
        g_TimeScriptPersistSuppress.originalLabel = label;
        g_TimeScriptPersistSuppress.originalTextSlc = 0;
        g_TimeScriptPersistSuppress.active = true;
        g_TimeScriptPersistSuppress.lastSuppressTick = GetTickCount();
        // Refresh cachedTimeField from HudElem+0x78
        uint32_t tf = 0;
        if (TryReadIntroU32(elemPtr, 0x78, tf) && tf > 30000) {
          g_TimeScriptPersistSuppress.cachedTimeField = tf;
        }
        // Keep native digits while the newly registered timer is being found.
        static const float kOff = 9999.0f;
        uint32_t offBits;
        memcpy(&offBits, &kOff, sizeof(uint32_t));
        if (TextHook_HasTimeScriptTimerReplacement()) {
          *(volatile uint32_t *)(elemPtr + 0x04) = offBits;
        }

        static DWORD s_lastAutoRegLog = 0;
        DWORD _tnow = GetTickCount();
        if ((_tnow - s_lastAutoRegLog) > 2000) {
          s_lastAutoRegLog = _tnow;
          char _mb[256];
          sprintf_s(_mb,
                    "[TIMESCRIPT-CG-AUTOREG] elemPtr=0x%llX label=0x%X "
                    "tf=%u text=%u",
                    (unsigned long long)elemPtr, label,
                    g_TimeScriptPersistSuppress.cachedTimeField, curText);
          LogToFile(_mb);
        }
      }
    }
  skip_auto_reg:;
  }

  // Cache every HudElem's coordinates by elemPtr so that SEH-based
  // key resolution (which fires later, possibly from a non-CG_DrawHudElem
  // caller) can still look up the real HudElem data.
  if (elemPtr != 0) {
    float ex = *(volatile float *)(elemPtr + 0x04);
    float ey = *(volatile float *)(elemPtr + 0x08);
    float fs = *(volatile float *)(elemPtr + 0x14);
    if (std::isfinite(ex) && std::isfinite(ey) && std::isfinite(fs) &&
        fs > 0.01f && fs < 6.0f &&
        ex > -2000.0f && ex < 2000.0f &&
        ey > -2000.0f && ey < 2000.0f) {
      CgDrawHudElemCapture cap{};
      cap.x = ex;
      cap.y = ey;
      cap.fontScale = fs;
      cap.alignOrg = *(volatile uint32_t *)(elemPtr + 0x28);
      cap.alignScreen = *(volatile uint32_t *)(elemPtr + 0x2C);
      cap.colorPacked = *(volatile uint32_t *)(elemPtr + 0x30);
      cap.tick = GetTickCount();
      TextHook_StoreCgDrawElemPtrCapture(elemPtr, cap);
    }
  }

  // Set context BEFORE calling original — SEH hook reads this inside.
  g_CGDrawHudElem_ElemPtr.store(elemPtr, std::memory_order_release);

  // Call original — runs fully (sounds, animations), just offscreen if suppressed.
  ((void(__fastcall *)(uintptr_t, uintptr_t, uintptr_t,
                       uintptr_t))Original_CG_DrawHudElem)(rcx, rdx, r8, r9);

  // Clear context after return.
  g_CGDrawHudElem_ElemPtr.store(0, std::memory_order_release);
}

static bool ParseEnvFlag(const char *name, bool defaultValue = false) {
  char raw[32] = {0};
  DWORD n = GetEnvironmentVariableA(name, raw, (DWORD)sizeof(raw));
  if (n == 0 || n >= sizeof(raw)) {
    return defaultValue;
  }
  std::string v = ToUpperAscii(TrimSpaces(std::string(raw)));
  if (v == "1" || v == "TRUE" || v == "ON" || v == "YES")
    return true;
  if (v == "0" || v == "FALSE" || v == "OFF" || v == "NO")
    return false;
  return defaultValue;
}

static bool IsTargetBinocularHintKey(const std::string &key) {
  return key == "CORNERED_BINOCULARS_USE_HINT" ||
         key == "CORNERED_BINOCULARS_ZOOM_HINT" ||
         key == "CORNERED_BINOCULARS_SCAN_HINT";
}

static bool IsTargetMovementPromptKey(const std::string &key) {
  if (key.empty()) {
    return false;
  }
  const std::string up = ToUpperAscii(key);
  if (up == "+GOCROUCH" || up == "+TOGGLECROUCH") {
    return true;
  }
  if (up.rfind("DEER_HUNT_", 0) != 0) {
    return false;
  }
  return (up.find("CROUCH") != std::string::npos ||
          up.find("SLIDE") != std::string::npos ||
          up.find("LASER") != std::string::npos ||
          up.find("MATV_HINT") != std::string::npos);
}

static bool IsTargetHudHintWriterFocusKey(const std::string &key) {
  if (IsTargetBinocularHintKey(key) || IsTargetMovementPromptKey(key)) {
    return true;
  }
  const std::string up = ToUpperAscii(key);
  return (up == "CORNERED_READY" || up == "CORNERED_NO" ||
          up == "CORNERED_MATCH" || up == "CORNERED_DATA" ||
          up == "CORNERED_IDENTIFIED" ||
          up.rfind("CORNERED_SCANNING_", 0) == 0);
}

static bool IsCenterHudHintStructKey(const std::string &key) {
  if (IsTargetBinocularHintKey(key)) {
    return true;
  }
  return key == "CORNERED_SCANNING_DOT" ||
         key == "CORNERED_SCANNING_DOT_DOT" ||
         key == "CORNERED_SCANNING_DOT_DOT_DOT" ||
         key == "CORNERED_IDENTIFIED";
}

static bool IsCenterHudHintAuthorityCaller(unsigned int callerOffset) {
  // Runtime chain evidence:
  // writer(+0x5DC950) -> update/draw(+0x1F1762)
  // prompt producer(+0x204816)
  // plus short-lived feeder callers observed in the same lifecycle.
  return (callerOffset == IW6Offsets::Profile::Rva_232562 || callerOffset == IW6Offsets::Profile::Rva_2454FC ||
          callerOffset == IW6Offsets::Profile::Rva_389BB5 ||
          callerOffset == IW6Offsets::Profile::Rva_4156EC || callerOffset == IW6Offsets::Profile::Rva_2B1453);
}

static bool ShouldForceCenterAlignGameplayTranslation(const std::string &key,
                                                       const std::string &resolvedKorText) {
  if (key.empty()) {
    return false;
  }

  const HudKeyType keyType = ClassifyHudKey(key);
  switch (keyType) {
  case HUD_KEY_MENU:
  case HUD_KEY_SUBTITLE:
  case HUD_KEY_INTRO:
  case HUD_KEY_EXCLUDE:
  case HUD_KEY_OBJECTIVE_LIST:
  case HUD_KEY_OBJECTIVE_UPDATE:
  case HUD_KEY_OBJECTIVE_HEADER:
  case HUD_KEY_GAME_SAVED:
  case HUD_KEY_NOW_SAVING:
    return false;
  default:
    break;
  }

  if (IsObjectiveOverlayKeyName(key) || IsObjectiveStatusMessageKeyName(key) ||
      IsDirectAddCmdObjHudHintKey(key) || IsHudRuntimeTailJoinPromptKey(key)) {
    return false;
  }

  if (key.find("_FAIL") != std::string::npos ||
      key.find("FAIL_") != std::string::npos ||
      key.find("_DEATH") != std::string::npos ||
      key.find("_DROWN") != std::string::npos ||
      key.find("_SUICIDE") != std::string::npos ||
      key.find("_QUOTE_") != std::string::npos ||
      key.find("_KILLED") != std::string::npos ||
      key.find("GOT_AWAY") != std::string::npos) {
    return true;
  }

  // Long Korean gameplay text (6+ syllables): also force center
  if (!resolvedKorText.empty()) {
    int count = 0;
    const unsigned char *p = (const unsigned char *)resolvedKorText.c_str();
    while (*p) {
      if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
        unsigned int cp =
            ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        if (cp >= 0xAC00 && cp <= 0xD7AF)
          ++count;
        p += 3;
      } else {
        p += (*p & 0x80) ? ((*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4) : 1;
      }
    }
    if (count >= 6)
      return true;
  }

  return false;
}

void TextHook_ArmHudHintWriterFocus(const std::string &key,
                                    unsigned int callerOffset) {
  (void)key;
  (void)callerOffset;
  // Writer INT3 tracking is disabled in shipping builds.
  // Center HUDHINT now flows through native mission-prompt params + native
  // snapshot, without installing or arming the expensive breakpoint path.
}

static LONG CALLBACK HudHintWriterVehHandler(PEXCEPTION_POINTERS ep) {
  if (!g_HudHintWriterBpInstalled || !ep || !ep->ExceptionRecord ||
      !ep->ContextRecord) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  CONTEXT *ctx = ep->ContextRecord;
  if ((uintptr_t)ctx->Rip != g_HudHintWriterBpAddr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  const uint64_t rdx = (uint64_t)ctx->Rdx;
  const uint64_t rcx = (uint64_t)ctx->Rcx;
  const uint64_t slotAddr = (rdx + rcx) - 4ull;
  const uint64_t effOff = (rdx >= 4ull) ? (rdx - 4ull) : 0ull;
  const uint32_t newValue = (uint32_t)ctx->Rax;
  uint32_t oldValue = 0;
  bool writeOk = false;
  if (IsSafeRead((void *)slotAddr, sizeof(uint32_t)) &&
      IsBadWritePtr((void *)slotAddr, sizeof(uint32_t)) == 0) {
    oldValue = *(volatile uint32_t *)slotAddr;
    *(volatile uint32_t *)slotAddr = newValue;
    writeOk = true;
  }

  const uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  const uintptr_t ripOff =
      (moduleBase != 0 && g_HudHintWriterBpAddr >= moduleBase)
          ? (g_HudHintWriterBpAddr - moduleBase)
          : 0;
  uintptr_t ret0 = 0, ret1 = 0, ret2 = 0;
  if (IsSafeRead((void *)ctx->Rsp, 0x18)) {
    ret0 = *(uintptr_t *)(ctx->Rsp + 0x00);
    ret1 = *(uintptr_t *)(ctx->Rsp + 0x08);
    ret2 = *(uintptr_t *)(ctx->Rsp + 0x10);
  }
  const uintptr_t ret0Off =
      (moduleBase != 0 && ret0 >= moduleBase) ? (ret0 - moduleBase) : 0;
  const uintptr_t ret1Off =
      (moduleBase != 0 && ret1 >= moduleBase) ? (ret1 - moduleBase) : 0;
  const uintptr_t ret2Off =
      (moduleBase != 0 && ret2 >= moduleBase) ? (ret2 - moduleBase) : 0;

  const uint32_t low16 = (uint32_t)(newValue & 0xFFFFu);
  const bool likelyHudHintId = (low16 >= 0x6800u && low16 <= 0x7900u);
  const bool chainCaller =
      (ret0Off == IW6Offsets::Profile::Rva_5DD45F || ret0Off == IW6Offsets::Profile::Rva_5DB732 || ret0Off == IW6Offsets::Profile::Rva_5DBB01 ||
       ret0Off == IW6Offsets::Profile::Rva_5DC13F || ret0Off == IW6Offsets::Profile::Rva_5DC266 || ret0Off == IW6Offsets::Profile::Rva_5DC637 ||
       ret1Off == IW6Offsets::Profile::Rva_5DD45F || ret1Off == IW6Offsets::Profile::Rva_5DB732 || ret1Off == IW6Offsets::Profile::Rva_5DBB01 ||
       ret1Off == IW6Offsets::Profile::Rva_5DC13F || ret1Off == IW6Offsets::Profile::Rva_5DC266 || ret1Off == IW6Offsets::Profile::Rva_5DC637);

  const unsigned long hits = g_HudHintWriterBpHitCount.fetch_add(1) + 1;
  const DWORD now = GetTickCount();
  const bool focusActive =
      (g_HudHintWriterFocusUntil.load() != 0 &&
       now <= g_HudHintWriterFocusUntil.load());
  const bool structRangeLikely = (rdx <= 0xA8ull);
  if (writeOk && structRangeLikely && rcx != 0 &&
      (chainCaller || likelyHudHintId || focusActive)) {
    const unsigned int callerHint =
        focusActive ? g_HudHintWriterFocusCaller.load() : 0;
    TextHook_RecordHudHintRuntimeSlot((uintptr_t)rcx, now, callerHint,
                                      (unsigned int)ret0Off,
                                      (unsigned int)ret1Off,
                                      (unsigned int)effOff, newValue);
  }
  unsigned long remaining = g_HudHintWriterFocusBudget.load();
  bool consumeFocusBudget = false;
  if (focusActive && structRangeLikely && (chainCaller || likelyHudHintId)) {
    while (remaining > 0) {
      if (g_HudHintWriterFocusBudget.compare_exchange_weak(remaining,
                                                           remaining - 1)) {
        consumeFocusBudget = true;
        break;
      }
    }
  }

  const DWORD prev = g_HudHintWriterBpLastLogTick.load();
  if ((consumeFocusBudget || likelyHudHintId || chainCaller || hits <= 24 ||
       (now - prev) > 250) &&
      g_HudHintWriterBpLastLogTick.exchange(now) != now) {
    g_HudHintWriterBpLogCount.fetch_add(1);
    char buf[640];
    if (consumeFocusBudget) {
      std::string focusKey;
      {
        std::lock_guard<std::mutex> lock(g_HudHintWriterFocusMutex);
        focusKey = g_HudHintWriterFocusKey;
      }
      sprintf_s(
          buf,
          "[HUDW-BP-FOCUS] key=%s fcaller=+0x%X rem=%lu hit=%lu rip=+0x%llX "
          "ret0=+0x%llX ret1=+0x%llX ret2=+0x%llX rbx=0x%llX rcx=0x%llX "
          "rdx=0x%llX off=0x%llX slot=0x%llX eax=0x%08X low16=0x%04X "
          "old=0x%08X ok=%d",
          focusKey.c_str(), g_HudHintWriterFocusCaller.load(), remaining,
          (unsigned long)hits, (unsigned long long)ripOff,
          (unsigned long long)ret0Off, (unsigned long long)ret1Off,
          (unsigned long long)ret2Off, (unsigned long long)ctx->Rbx,
          (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx,
          (unsigned long long)effOff,
          (unsigned long long)slotAddr, (unsigned int)newValue,
          (unsigned int)low16, (unsigned int)oldValue, writeOk ? 1 : 0);
    } else {
      sprintf_s(
          buf,
          "[HUDW-BP] hit=%lu rip=+0x%llX ret0=+0x%llX ret1=+0x%llX ret2=+0x%llX "
          "rbx=0x%llX rcx=0x%llX rdx=0x%llX slot=0x%llX eax=0x%08X low16=0x%04X "
          "old=0x%08X ok=%d",
          (unsigned long)hits, (unsigned long long)ripOff,
          (unsigned long long)ret0Off, (unsigned long long)ret1Off,
          (unsigned long long)ret2Off, (unsigned long long)ctx->Rbx,
          (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx,
          (unsigned long long)slotAddr, (unsigned int)newValue,
          (unsigned int)low16, (unsigned int)oldValue, writeOk ? 1 : 0);
    }
    LogToFile(buf);
  }

  // Emulate the overwritten 4-byte instruction and continue.
  ctx->Rip = (DWORD64)(g_HudHintWriterBpAddr + 4);
  return EXCEPTION_CONTINUE_EXECUTION;
}

static bool TryInstallHudHintWriterBreakpoint(uintptr_t moduleBase) {
  if (!g_HudHintWriterBpEnabled || g_HudHintWriterBpInstalled ||
      moduleBase == 0) {
    return g_HudHintWriterBpInstalled;
  }

  g_HudHintWriterBpAddr = moduleBase + IW6Offsets::Profile::Rva_5DC950;
  if (!IsSafeRead((void *)g_HudHintWriterBpAddr, 4)) {
    return false;
  }

  DWORD oldProtect = 0;
  if (!VirtualProtect((void *)g_HudHintWriterBpAddr, 1, PAGE_EXECUTE_READWRITE,
                      &oldProtect)) {
    LogToFile("[HUDW-BP] install failed: VirtualProtect");
    return false;
  }

  g_HudHintWriterBpOrigByte = *(unsigned char *)g_HudHintWriterBpAddr;
  *(unsigned char *)g_HudHintWriterBpAddr = 0xCC;
  DWORD dummy = 0;
  VirtualProtect((void *)g_HudHintWriterBpAddr, 1, oldProtect, &dummy);
  FlushInstructionCache(GetCurrentProcess(), (void *)g_HudHintWriterBpAddr, 1);

  if (!g_HudHintWriterVehHandle) {
    g_HudHintWriterVehHandle = AddVectoredExceptionHandler(
        1, (PVECTORED_EXCEPTION_HANDLER)HudHintWriterVehHandler);
  }
  if (!g_HudHintWriterVehHandle) {
    LogToFile("[HUDW-BP] install failed: AddVectoredExceptionHandler");
    return false;
  }

  g_HudHintWriterBpInstalled = true;
  char ibuf[192];
  sprintf_s(ibuf, "[HUDW-BP] installed at +0x%llX (addr=0x%llX)",
            (unsigned long long)(g_HudHintWriterBpAddr - moduleBase),
            (unsigned long long)g_HudHintWriterBpAddr);
  LogToFile(ibuf);
  return true;
}

static void UninstallHudHintWriterBreakpoint() {
  if (g_HudHintWriterBpInstalled && g_HudHintWriterBpAddr != 0) {
    DWORD oldProtect = 0;
    if (VirtualProtect((void *)g_HudHintWriterBpAddr, 1,
                       PAGE_EXECUTE_READWRITE, &oldProtect)) {
      *(unsigned char *)g_HudHintWriterBpAddr = g_HudHintWriterBpOrigByte;
      DWORD dummy = 0;
      VirtualProtect((void *)g_HudHintWriterBpAddr, 1, oldProtect, &dummy);
      FlushInstructionCache(GetCurrentProcess(), (void *)g_HudHintWriterBpAddr,
                            1);
    }
  }
  if (g_HudHintWriterVehHandle) {
    RemoveVectoredExceptionHandler(g_HudHintWriterVehHandle);
    g_HudHintWriterVehHandle = nullptr;
  }
  if (g_HudHintWriterBpEnabled) {
    char sbuf[192];
    sprintf_s(sbuf, "[HUDW-BP] uninstalled hits=%lu logs=%lu",
              (unsigned long)g_HudHintWriterBpHitCount.load(),
              (unsigned long)g_HudHintWriterBpLogCount.load());
    LogToFile(sbuf);
  }
  g_HudHintWriterBpInstalled = false;
}

bool TextHook_IsHudNativeEnabled() { return g_EnableHudNative; }
void TextHook_SetHudNativeEnabled(bool enabled) {
  g_EnableHudNative = enabled;
}
static bool TryReadHudElem(uintptr_t ptr, unsigned int stringValue,
                           const char *regName, uintptr_t callerOffset,
                           const char *keyName, char *outBuf,
                           size_t outBufSize) {
  __try {
    int fieldLabel = *(volatile int *)(ptr + 0x40);
    int expectedAC = (int)stringValue - 0xAC;
    int expected8C = (int)stringValue - 0x8C;
    bool match = (fieldLabel == expectedAC) || (fieldLabel == expected8C) ||
                 (fieldLabel == (int)stringValue);
    if (!match)
      return false;

    int type = *(volatile int *)(ptr + 0x00);
    float x = *(volatile float *)(ptr + 0x04);
    float y = *(volatile float *)(ptr + 0x08);
    float fontScale = *(volatile float *)(ptr + 0x14);
    int alignOrg = *(volatile int *)(ptr + 0x28);
    int alignScreen = *(volatile int *)(ptr + 0x2C);
    unsigned int color = *(volatile unsigned int *)(ptr + 0x30);
    int fadeStart = *(volatile int *)(ptr + 0x38);
    int fadeTime = *(volatile int *)(ptr + 0x3C);
    int label = fieldLabel;
    int text = *(volatile int *)(ptr + 0x84);
    int fxBirth = *(volatile int *)(ptr + 0x90);
    int fxLetter = *(volatile int *)(ptr + 0x94);
    int fxDecayStart = *(volatile int *)(ptr + 0x98);
    int fxDecayDur = *(volatile int *)(ptr + 0x9C);

    sprintf_s(outBuf, outBufSize,
              "[SLC HUDELEM FOUND] reg=%s ptr=0x%llX caller=0x%llX "
              "key=\"%.40s\" type=%d x=%.1f y=%.1f fontScale=%.2f "
              "alignOrg=%d alignScreen=%d color=0x%08X "
              "fadeStart=%d fadeTime=%d label=0x%X text=0x%X "
              "fxBirth=%d fxLetter=%d fxDecayStart=%d fxDecayDur=%d",
              regName, (unsigned long long)ptr,
              (unsigned long long)callerOffset, keyName, type, x, y,
              fontScale, alignOrg, alignScreen, color, fadeStart, fadeTime,
              label, text, fxBirth, fxLetter, fxDecayStart, fxDecayDur);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Helper: Probe a suspected HudElem array element (SEH-protected).
// Returns true and fills outBuf if valid HudElem found.
static bool TryProbeHudElem(uintptr_t elemAddr, unsigned int stringValue,
                            int elemIdx, uintptr_t callerOffset,
                            const char *keyName, char *outBuf,
                            size_t outBufSize) {
  __try {
    int fieldLabel = *(volatile int *)(elemAddr + 0x40);
    int labelAC = (int)stringValue - 0xAC;
    int label8C = (int)stringValue - 0x8C;
    if (fieldLabel != labelAC && fieldLabel != label8C &&
        fieldLabel != (int)stringValue)
      return false;
    if (fieldLabel == 0)
      return false;

    int type = *(volatile int *)(elemAddr + 0x00);
    if (type < 1 || type > 13)
      return false;

    float x = *(volatile float *)(elemAddr + 0x04);
    float y = *(volatile float *)(elemAddr + 0x08);
    float fontScale = *(volatile float *)(elemAddr + 0x14);
    int alignOrg = *(volatile int *)(elemAddr + 0x28);
    int alignScreen = *(volatile int *)(elemAddr + 0x2C);
    unsigned int color = *(volatile unsigned int *)(elemAddr + 0x30);
    int fadeStart = *(volatile int *)(elemAddr + 0x38);
    int fadeTime = *(volatile int *)(elemAddr + 0x3C);
    int text = *(volatile int *)(elemAddr + 0x84);
    int fxBirth = *(volatile int *)(elemAddr + 0x90);
    int fxLetter = *(volatile int *)(elemAddr + 0x94);
    int fxDecayStart = *(volatile int *)(elemAddr + 0x98);
    int fxDecayDur = *(volatile int *)(elemAddr + 0x9C);

    sprintf_s(outBuf, outBufSize,
              "[SLC HUDPROBE] idx=%d addr=0x%llX "
              "caller=0x%llX key=\"%.40s\" "
              "type=%d x=%.1f y=%.1f fontScale=%.2f "
              "alignOrg=%d alignScreen=%d color=0x%08X "
              "fadeStart=%d fadeTime=%d label=0x%X text=0x%X "
              "fxBirth=%d fxLetter=%d fxDecayStart=%d fxDecayDur=%d",
              elemIdx, (unsigned long long)elemAddr,
              (unsigned long long)callerOffset, keyName, type, x, y, fontScale,
              alignOrg, alignScreen, color, fadeStart, fadeTime, fieldLabel,
              text, fxBirth, fxLetter, fxDecayStart, fxDecayDur);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Helper: Read a single int from a global address (SEH-protected).
static bool TryReadGlobalInt(uintptr_t addr, int *outVal) {
  __try {
    *outVal = *(volatile int *)addr;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// =============================================================================
void __fastcall Detour_CG_GameMessage(int localClientNum, const char *message) {
  (void)localClientNum;

  if (RuntimeFlags_ObjectiveStatusEnableGameMessage() && message && message[0]) {
    std::string raw(message);
    raw = TrimSpaces(StripColorCodes(raw));
    std::string sanitized = ObjRev_SanitizeEnglishForKeyResolve(raw);

    static uintptr_t s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
    unsigned int callerOffset = 0;
    if (s_moduleBase != 0) {
      const uintptr_t ret = (uintptr_t)_ReturnAddress();
      if (ret >= s_moduleBase) {
        callerOffset = (unsigned int)(ret - s_moduleBase);
      }
    }

    std::string key;
    std::string resolveMode = "none";
    std::string mapped;
    if (LooksLikeHudLocalizationKey(raw) &&
        IsObjectiveStatusMessageKeyName(raw)) {
      key = raw;
      resolveMode = "direct_key";
    } else {
      if (!sanitized.empty() &&
          ResolveKeyFromEnglishInternal(sanitized, mapped) &&
          IsObjectiveStatusMessageKeyName(mapped)) {
        key = mapped;
        resolveMode = "resolved_english";
      } else {
        const std::string upperRaw = ToUpper(raw);
        if ((upperRaw.find("OBJECTIVE") != std::string::npos &&
             (upperRaw.find("COMPLETED") != std::string::npos ||
              upperRaw.find("COMPLETE") != std::string::npos ||
              upperRaw.find("FAILED") != std::string::npos))) {
          key = (upperRaw.find("FAILED") != std::string::npos)
                    ? "OBJECTIVE_FAILED"
                    : "GAME_OBJECTIVECOMPLETED";
          resolveMode = "heuristic_objective_terminal";
        } else if (upperRaw.find("OBJECTIVE") != std::string::npos &&
                   upperRaw.find("UPDATED") != std::string::npos) {
          key = "GAME_OBJECTIVESUPDATED";
          resolveMode = "heuristic_objective_updated";
        } else if (upperRaw.find("GAME SAVED") != std::string::npos ||
                   upperRaw.find("GAMESAVED") != std::string::npos) {
          key = "EXE_GAMESAVED";
          resolveMode = "heuristic_game_saved";
        } else if (upperRaw.find("NOW SAVING") != std::string::npos ||
                   upperRaw.find("SAVING") != std::string::npos) {
          key = "CGAME_NOW_SAVING";
          resolveMode = "heuristic_now_saving";
        }
      }
    }

    {
      static std::unordered_map<std::string, DWORD> s_cgMsgCallLogTick;
      const DWORD now = GetTickCount();
      const std::string sig = raw.empty() ? std::string("(empty)") : raw;
      auto it = s_cgMsgCallLogTick.find(sig);
      if (it == s_cgMsgCallLogTick.end() || (now - it->second) > 1200) {
        s_cgMsgCallLogTick[sig] = now;
        if (s_cgMsgCallLogTick.size() > 256) {
          for (auto itr = s_cgMsgCallLogTick.begin();
               itr != s_cgMsgCallLogTick.end();) {
            if ((now - itr->second) > 8000) {
              itr = s_cgMsgCallLogTick.erase(itr);
            } else {
              ++itr;
            }
          }
        }

        char cbuf[560];
        sprintf_s(cbuf,
                  "[CG-GAMEMSG-CALL] caller=+0x%X mode=%s resolved=%s "
                  "raw=\"%.96s\" sanitized=\"%.96s\" mapped=\"%.64s\"",
                  callerOffset, resolveMode.c_str(),
                  key.empty() ? "<none>" : key.c_str(), raw.c_str(),
                  sanitized.c_str(), mapped.c_str());
        LogToFile(cbuf);
      }
    }

    if (!key.empty()) {
      const DWORD nowStatus = GetTickCount();
      static std::unordered_map<std::string, DWORD> s_cgMsgLogTick;
      const DWORD now = GetTickCount();
      auto it = s_cgMsgLogTick.find(key);
      if (it == s_cgMsgLogTick.end() || (now - it->second) > 1200) {
        s_cgMsgLogTick[key] = now;
        char buf[384];
        sprintf_s(buf,
                  "[CG-GAMEMSG-STATUS] key=%s caller=+0x%X raw=\"%.96s\" src=observe_only",
                  key.c_str(), callerOffset, raw.c_str());
        LogToFile(buf);
      }
    }
  }

  if (Original_CG_GameMessage) {
    Original_CG_GameMessage(localClientNum, message);
  }
}
// SL_ConvertToString DETOUR - Reimplemented (no gateway/trampoline)
//
// Original function (24 bytes):
//   test ecx, ecx        ; 85 c9
//   je   +0x12           ; 74 12  (-> xor eax,eax; ret)
//   mov  rax,[rip+disp]  ; 48 8b 05 XX XX XX XX (string table ptr)
//   shl  ecx, 4          ; c1 e1 04
//   add  rax, 4          ; 48 83 c0 04
//   add  rax, rcx        ; 48 03 c1
//   ret                  ; c3
//   xor  eax, eax        ; 33 c0 (return NULL path)
//
// We reimplemented this in C++ so no gateway is needed, avoiding the
// short-jump-in-stolen-bytes problem entirely.
// =============================================================================

unsigned int __fastcall Detour_ConfigString_IndexToSlc(unsigned int queryIdx) {

  if (!Original_ConfigString_IndexToSlc) {
    return 0;
  }

  const unsigned int slcIdx = Original_ConfigString_IndexToSlc(queryIdx);

  // Credits guard + burst throttle: skip the expensive OBJECTIVE_REVERSE
  // processing when credits are active, or when call volume is abnormally
  // high (credits init causes 29K+ calls/frame vs normal ~50-100).
  // The burst throttle catches the initial explosion BEFORE the credits
  // guard activates via "ui_play_credits" SLC detection.
  {
    extern std::atomic<bool> g_creditsGuardActive;
    if (g_creditsGuardActive.load(std::memory_order_relaxed)) {
      return slcIdx;
    }
    // Burst throttle: 200ms window, 3000 call budget.
    // Normal gameplay: ~600 calls/200ms.  Credits: 300K+/200ms.
    static std::atomic<uint32_t> s_burstCount{0};
    static std::atomic<DWORD> s_burstTick{0};
    DWORD now = GetTickCount();
    if ((now - s_burstTick.load(std::memory_order_relaxed)) > 200) {
      s_burstCount.store(0, std::memory_order_relaxed);
      s_burstTick.store(now, std::memory_order_relaxed);
    }
    if (s_burstCount.fetch_add(1, std::memory_order_relaxed) > 3000) {
      return slcIdx;
    }
  }

#if GHOSTSKOR_OBJECTIVE_REVERSE
  if (slcIdx != 0) {
    const uintptr_t retAddr = (uintptr_t)_ReturnAddress();
    const uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
    const uintptr_t callerOffset =
        (retAddr >= moduleBase) ? (retAddr - moduleBase) : 0;
    const bool isBroadCfgProducerCaller =
        ObjRev_IsBulkCfgProducerCaller(callerOffset);
    const DWORD now = GetTickCount();

    // Root-fix path: capture raw query->slc provenance first.
    // We no longer depend on SL_ConvertToString handoff for objective binding.
    // Objective authority is consumed directly from objective cfg callers.
    g_objCfgSlcTlsCtx.queryIdx = queryIdx;
    g_objCfgSlcTlsCtx.slcIdx = slcIdx;
    g_objCfgSlcTlsCtx.callerOffset = callerOffset;
    g_objCfgSlcTlsCtx.tick = now;
    ObjRev_RecordCfgSlcTlsRecent(queryIdx, slcIdx, callerOffset, now);

    {
      std::lock_guard<std::mutex> lk(g_objCfgSlcRawMutex);
      ObjCfgSlcRawObservation &obs = g_objCfgSlcRawBySlc[slcIdx];
      obs.queryIdx = queryIdx;
      obs.callerOffset = callerOffset;
      obs.tick = now;
      if (g_objCfgSlcRawBySlc.size() > 4096) {
        for (auto it = g_objCfgSlcRawBySlc.begin();
             it != g_objCfgSlcRawBySlc.end();) {
          if ((now - it->second.tick) > 12000) {
            it = g_objCfgSlcRawBySlc.erase(it);
          } else {
            ++it;
          }
        }
      }
    }

    // Direct cfg->slc provenance bind:
    // Decode cfg base from caller LEA displacement for any structurally matching
    // cfg producer callsite. Objective callers remain authoritative for objective
    // promotion, but HUD hint callers also need this map for deterministic
    // cfg->slc->key joins (Press/Hold prompts).
    {
      const bool osrevOnly = RuntimeFlags_ObjectiveStatusEnableOsRevOnly();
      const bool isObjectiveCfgCaller =
          (ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset) ||
           ObjRev_IsObjectiveConsumerCandidateCaller(callerOffset));
      const bool isHudHintCfgCaller =
          (callerOffset == IW6Offsets::Profile::Rva_2454FC || callerOffset == IW6Offsets::Profile::Rva_232562 ||
           callerOffset == IW6Offsets::Profile::Rva_389BB5 || callerOffset == IW6Offsets::Profile::Rva_4156EC ||
           callerOffset == IW6Offsets::Profile::Rva_2B1453 || callerOffset == IW6Offsets::Profile::Rva_4DAC50 ||
           callerOffset == IW6Offsets::Profile::Rva_369002);
      const bool shouldAttemptDirectDecode =
          ((!osrevOnly && isObjectiveCfgCaller) || isHudHintCfgCaller) &&
          !isBroadCfgProducerCaller;
      uint32_t cfgIdx = 0;
      uint32_t bias = 0;
      if (shouldAttemptDirectDecode &&
          ObjRev_DecodeCfgBaseFromCaller(queryIdx, retAddr, cfgIdx, bias) &&
          cfgIdx != 0) {
        // Stability guard:
        // broad producer walks can fire at very high volume.
        if (!isBroadCfgProducerCaller) {
          ObjRev_RecordDirectCfgSlc(cfgIdx, bias, slcIdx, callerOffset);
        }
        if (!isBroadCfgProducerCaller &&
            (ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset) ||
             ObjRev_IsObjectiveConsumerCandidateCaller(callerOffset))) {
          ObjRev_RecordObjectiveConsumerResolve(cfgIdx, bias, queryIdx, slcIdx,
                                                callerOffset, "cfg_to_slc");
        }

        if (kVerboseRuntimeLogs) {
          static std::unordered_map<uint64_t, DWORD> s_cfgDirectLogTick;
          static std::mutex s_cfgDirectLogMutex;
          const uint64_t sig = ((uint64_t)callerOffset << 40) ^
                               ((uint64_t)(cfgIdx & 0xFFFFu) << 20) ^
                               (uint64_t)(slcIdx & 0xFFFFFu);
          bool shouldEmitDirectLog = false;
          char dbuf[320] = {};
          {
            std::lock_guard<std::mutex> lk(s_cfgDirectLogMutex);
            auto it = s_cfgDirectLogTick.find(sig);
            if (it == s_cfgDirectLogTick.end() || (now - it->second) > 1500) {
              s_cfgDirectLogTick[sig] = now;
              if (isObjectiveCfgCaller) {
                sprintf_s(
                    dbuf,
                    "[OBJ-CFGSLC-DIRECT] caller=+0x%llX cfg=0x%X bias=0x%X query=0x%X slc=0x%X",
                    (unsigned long long)callerOffset, cfgIdx, bias, queryIdx,
                    slcIdx);
                shouldEmitDirectLog = true;
              } else if (isHudHintCfgCaller) {
                sprintf_s(
                    dbuf,
                    "[HUD-CFGSLC-DIRECT] caller=+0x%llX cfg=0x%X bias=0x%X query=0x%X slc=0x%X",
                    (unsigned long long)callerOffset, cfgIdx, bias, queryIdx,
                    slcIdx);
                shouldEmitDirectLog = true;
              }
            }
          }
          if (shouldEmitDirectLog) {
            LogToFile(dbuf);
          }
        }
      } else if (shouldAttemptDirectDecode) {
        if (kVerboseRuntimeLogs) {
          static std::unordered_map<uint64_t, DWORD> s_cfgDecodeMissLogTick;
          static std::mutex s_cfgDecodeMissLogMutex;
          const uint64_t sig = ((uint64_t)callerOffset << 32) | (uint64_t)queryIdx;
          bool shouldEmitDecodeMiss = false;
          char mbuf[256] = {};
          {
            std::lock_guard<std::mutex> lk(s_cfgDecodeMissLogMutex);
            auto it = s_cfgDecodeMissLogTick.find(sig);
            if (it == s_cfgDecodeMissLogTick.end() || (now - it->second) > 2500) {
              s_cfgDecodeMissLogTick[sig] = now;
              sprintf_s(
                  mbuf,
                  "[OBJ-CFGSLC-DIRECT-DECODE-MISS] caller=+0x%llX query=0x%X slc=0x%X",
                  (unsigned long long)callerOffset, queryIdx, slcIdx);
              shouldEmitDecodeMiss = true;
            }
          }
          if (shouldEmitDecodeMiss) {
            LogToFile(mbuf);
          }
        }
      }
    }

    // Targeted reverse telemetry (diagnostic only)
    if (kVerboseRuntimeLogs &&
        ((slcIdx >= 0x5400u && slcIdx <= 0x57FFu) ||
         (queryIdx >= 0x40u && queryIdx <= 0x80u))) {
      static std::unordered_map<uint64_t, DWORD> s_cfgSlcRawLogTick;
      static std::mutex s_cfgSlcRawLogMutex;
      static DWORD s_cfgSlcRawBudgetTick = 0;
      static int s_cfgSlcRawBudgetCount = 0;
      const uint64_t sig =
          ((uint64_t)(callerOffset & 0xFFFFFFu) << 40) ^
          ((uint64_t)(queryIdx & 0xFFFFu) << 20) ^
          (uint64_t)(slcIdx & 0xFFFFFu);
      bool shouldEmitRawLog = false;
      char rbuf[256] = {};
      {
        std::lock_guard<std::mutex> lk(s_cfgSlcRawLogMutex);
        if (s_cfgSlcRawBudgetTick == 0 || (now - s_cfgSlcRawBudgetTick) > 1000) {
          s_cfgSlcRawBudgetTick = now;
          s_cfgSlcRawBudgetCount = 0;
        }
        const bool objectiveCaller =
            (ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset) ||
             ObjRev_IsObjectiveConsumerCandidateCaller(callerOffset));
        if (objectiveCaller && !isBroadCfgProducerCaller &&
            s_cfgSlcRawBudgetCount < 6) {
          auto it = s_cfgSlcRawLogTick.find(sig);
          if (it == s_cfgSlcRawLogTick.end() || (now - it->second) > 5000) {
            s_cfgSlcRawLogTick[sig] = now;
            ++s_cfgSlcRawBudgetCount;
            sprintf_s(rbuf,
                      "[CFGSLC-RAW] caller=+0x%llX query=0x%X slc=0x%X",
                      (unsigned long long)callerOffset, queryIdx, slcIdx);
            shouldEmitRawLog = true;
          }
        }
      }
      if (shouldEmitRawLog) {
        LogToFile(rbuf);
      }
    }
  } else {
    g_objCfgSlcTlsCtx = {};
  }
#endif

  return slcIdx;
}

unsigned int __fastcall Detour_SL_StringTypeCheck(unsigned int stringValue) {
  if (!Original_SL_StringTypeCheck) {
    return 0;
  }

  const uintptr_t retAddr = (uintptr_t)_ReturnAddress();
  const uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  const uintptr_t callerOffset =
      (retAddr >= moduleBase) ? (retAddr - moduleBase) : 0;
  const SLC_CallerRegs preRegs = g_SL_StringTypeCheck_CallerRegs;
  const uintptr_t preRetOffset =
      (preRegs.ret >= moduleBase) ? (preRegs.ret - moduleBase) : 0;
  const uint32_t curTid = GetCurrentThreadId();
  const bool preRegsMatch =
      (preRetOffset == callerOffset) &&
      ((uint32_t)(preRegs.rcx & 0xFFFFFFFFu) == stringValue) &&
      ((uint32_t)(preRegs.tid & 0xFFFFFFFFu) == curTid);

  const unsigned int typeValue = Original_SL_StringTypeCheck(stringValue);

#if GHOSTSKOR_OBJECTIVE_REVERSE
  // Objective path observed (legacy offsets):
  //   +0x34E24A: call SL_StringTypeCheck
  //   +0x34E24F: cmp eax, 3
  //   +0x34E259: call SL_ConvertToString (ret +0x34E25E)
  // Runtime build variants shift these offsets, so capture by type==3 and
  // let SL_ConvertToString join resolve the exact caller pair.
  if (typeValue == 3u) {
    const DWORD now = GetTickCount();
    const uint32_t ordinal =
        preRegsMatch ? (uint32_t)(preRegs.rdi & 0xFFFFFFFFu) : 0u;
    const uintptr_t r12Value = preRegsMatch ? preRegs.r12 : 0;
    const uintptr_t rsiValue = preRegsMatch ? preRegs.rsi : 0;
    const uintptr_t rbpValue = preRegsMatch ? preRegs.rbp : 0;
    const uintptr_t rspValue = preRegsMatch ? preRegs.rsp : 0;
    g_objTypeChkTlsCtx.queryIdx = stringValue;
    g_objTypeChkTlsCtx.resultType = typeValue;
    g_objTypeChkTlsCtx.ordinal = ordinal;
    g_objTypeChkTlsCtx.callerOffset = callerOffset;
    g_objTypeChkTlsCtx.r12 = r12Value;
    g_objTypeChkTlsCtx.rsi = rsiValue;
    g_objTypeChkTlsCtx.rbp = rbpValue;
    g_objTypeChkTlsCtx.rsp = rspValue;
    g_objTypeChkTlsCtx.tick = now;

    static std::unordered_map<uint32_t, DWORD> s_typeLogTick;
    auto it = s_typeLogTick.find(stringValue);
    if (it == s_typeLogTick.end() || (now - it->second) > 1200) {
      s_typeLogTick[stringValue] = now;
      char tbuf[384];
      sprintf_s(tbuf,
                "[OBJ-TYPECHK] caller=+0x%llX q=0x%X type=%u pre=%u ord=%u "
                "r12=0x%llX rsi=0x%llX rbp=0x%llX rsp=0x%llX",
                (unsigned long long)callerOffset, stringValue, typeValue,
                preRegsMatch ? 1u : 0u, ordinal, (unsigned long long)r12Value,
                (unsigned long long)rsiValue, (unsigned long long)rbpValue,
                (unsigned long long)rspValue);
      LogToFile(tbuf);
    }

    static bool s_ripDumped = false;
    if (!s_ripDumped &&
        retAddr > 160 &&
        IsSafeRead((void *)(retAddr - 128), 224)) {
      s_ripDumped = true;
      const unsigned char *rip = (const unsigned char *)retAddr;
      std::stringstream rs;
      rs << "[OBJ-TYPECHK-RIP] ret=+0x" << std::hex << std::uppercase
         << (unsigned long long)callerOffset << " bytes(-128..+95): ";
      for (int j = -128; j < 96; ++j) {
        rs << std::setw(2) << std::setfill('0') << std::hex
           << (unsigned int)rip[j] << " ";
      }
      LogToFile(rs.str());
    }
  }
#endif

  return typeValue;
}

// =============================================================================
// Cbuf_AddText Detour — restart signal detection + state reset
// Intercepts console commands from LUI menus to detect restarts.
// On restart: clears subtitles/objectives/HUD, sets RuntimeResetTick for
// 2-second HUD hint rendering suppression.
// =============================================================================
void __fastcall Detour_Cbuf_AddText(int localClientNum, const char *text) {
  if (text && text[0] != '\0') {
    const bool isLoadgameContinue =
        (strstr(text, "loadgame_continue") != nullptr);
    const bool isFastRestart = (strstr(text, "fast_restart") != nullptr);
    const bool isMapRestart = (strstr(text, "map_restart") != nullptr);
    const bool isDisconnect = (strstr(text, "disconnect") != nullptr);

    if (isLoadgameContinue || isFastRestart || isMapRestart || isDisconnect) {
      const bool isMissionFailed =
          (strstr(text, "loadgame_continue_missionfailed") != nullptr);

      char buf[512];
      snprintf(buf, sizeof(buf),
               "[CBUF_PROBE] cmd=\"%.200s\" checkpoint=%d missionfailed=%d "
               "fast_restart=%d map_restart=%d disconnect=%d tick=%lu",
               text, (isLoadgameContinue && !isMissionFailed) ? 1 : 0,
               isMissionFailed ? 1 : 0, isFastRestart ? 1 : 0,
               isMapRestart ? 1 : 0, isDisconnect ? 1 : 0,
               (unsigned long)GetTickCount());
      LogToFile(buf);

      // Reset state + set RuntimeResetTick (used by hint rendering suppression)
      if (isDisconnect) {
        ResetSubtitleStateForMapRestart();
        TextHook_ResetOverlayRuntime(OVERLAY_RESET_DISCONNECT);
        // Disconnect means returning to frontend (main menu).
        g_FrontendMenuContext.store(true, std::memory_order_relaxed);
        TextHook_ClearSubtitleQueue();
        TextHook_ArmMenuDiagWindow(2200, "frontend_enter_disconnect");
        LogToFile("[FRONTEND] entered via disconnect");
      } else if (isMissionFailed) {
        ResetSubtitleStateForMapRestart();
        TextHook_ResetOverlayRuntime(OVERLAY_RESET_MISSION_RESTART);
        g_FrontendMenuContext.store(false, std::memory_order_relaxed);
      } else if (isLoadgameContinue) {
        ResetSubtitleStateForMapRestart();
        TextHook_ResetOverlayRuntime(OVERLAY_RESET_CHECKPOINT_RESTART);
        g_FrontendMenuContext.store(false, std::memory_order_relaxed);
      } else if (isFastRestart) {
        ResetSubtitleStateForMapRestart();
        TextHook_ResetOverlayRuntime(OVERLAY_RESET_FAST_RESTART);
        g_FrontendMenuContext.store(false, std::memory_order_relaxed);
      } else if (isMapRestart) {
        ResetSubtitleStateForMapRestart();
        TextHook_ResetOverlayRuntime(OVERLAY_RESET_MAP_RESTART);
        g_FrontendMenuContext.store(false, std::memory_order_relaxed);
      }
    }
  }

  // Always call original
  if (Original_Cbuf_AddText) {
    Original_Cbuf_AddText(localClientNum, text);
  }
}

const char *__fastcall Detour_SL_ConvertToString(unsigned int stringValue) {

  // --- Reimplemented original logic (no gateway call) ---
  if (stringValue == 0)
    return nullptr;

  uintptr_t tableBase = *(uintptr_t *)g_SLC_StringTableGlobal;
  if (!tableBase)
    return nullptr;

  const char *result =
      (const char *)(tableBase + 4 + ((uintptr_t)stringValue << 4));
  void *retAddr = _ReturnAddress();
  static uintptr_t s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  uintptr_t callerOffset = (uintptr_t)retAddr - s_moduleBase;

  if (result && strcmp(result, "map_restart") == 0) {
    static DWORD s_lastSigMonCmdTick = 0;
    DWORD now = GetTickCount();
    if ((now - s_lastSigMonCmdTick) > 700) {
      s_lastSigMonCmdTick = now;
      char sbuf[320];
      sprintf_s(sbuf,
                "[SIGMON][CMD] source=SLC cmd=\"%s\" caller=+0x%llX slc=0x%08X",
                result, (unsigned long long)callerOffset, stringValue);
      LogToFile(sbuf);
    }
  }

  // Runtime reset is restricted to the one restart path actually observed in SP.
  if (result && strcmp(result, "map_restart") == 0) {
    static DWORD s_lastResetTick = 0;
    DWORD now = GetTickCount();
    if ((now - s_lastResetTick) > 1200) {
      s_lastResetTick = now;
      ResetSubtitleStateForMapRestart();
      TextHook_ResetOverlayRuntime(OVERLAY_RESET_MAP_RESTART);
      // Clear frontend flag: when loading a new mission after disconnect,
      // map_restart fires via SLC (not Cbuf). Without this, frontMenu
      // stays true and objectives are hidden for the entire new mission.
      g_FrontendMenuContext.store(false, std::memory_order_relaxed);
      LogToFile(std::string("[TextHook] restart detected: state reset (") +
                result + ")");
    }
  }
  if (result && strcmp(result, "disconnect") == 0) {
    // Root rule:
    // "disconnect" token is noisy and must NOT trigger subtitle/overlay reset.
    // Keep telemetry only; frontend entry is owned by on-screen menu text
    // detection + session edge logic.
    static DWORD s_lastDisconnectIgnoreLogTick = 0;
    DWORD now = GetTickCount();
    if ((now - s_lastDisconnectIgnoreLogTick) > 2000) {
      s_lastDisconnectIgnoreLogTick = now;
      char dbuf[224];
      sprintf_s(
          dbuf,
          "[TextHook] disconnect string observed: reset ignored caller=+0x%llX menu=%d front=%d",
          (unsigned long long)callerOffset, TextHook_IsMenuContext() ? 1 : 0,
          TextHook_IsFrontendMenuLikely() ? 1 : 0);
      LogToFile(dbuf);
    }
  }

  if (result && strcmp(result, "ui_play_credits") == 0) {
    extern std::atomic<bool> g_creditsSoftGuard;
    extern std::atomic<bool> g_creditsGuardActive;
    // Soft guard: disable SLC only.  IntroTextLayout (still active) will
    // determine credits vs gameplay and either upgrade to full guard or cancel.
    if (!g_creditsSoftGuard.load(std::memory_order_relaxed) &&
        !g_creditsGuardActive.load(std::memory_order_relaxed)) {
      g_creditsSoftGuard.store(true, std::memory_order_relaxed);
      TextHook_ClearHudNativeEntries();
      TextHook_SoftSuspendSLC();
      LogToFile("[CREDITS-GUARD] SOFT — ui_play_credits detected");
    }
  }


  // --- Caller capture (lightweight, throttled) ---
  // Only do work if string looks meaningful (>5 chars)
  if (!result || !result[0])
    return result;

  // Quick length check without full strlen - just verify 6 chars exist
  if (result[1] == '\0' || result[2] == '\0' || result[3] == '\0' ||
      result[4] == '\0' || result[5] == '\0')
    return result;

  // Credits guard: skip all expensive SLC processing (objective authority,
  // HUD key detection, NHUD param capture) while credits are active.
  {
    extern std::atomic<bool> g_creditsGuardActive;
    if (g_creditsGuardActive.load(std::memory_order_relaxed)) {
      return result;
    }
  }

#if GHOSTSKOR_OBJECTIVE_REVERSE
  const DWORD now = GetTickCount();
  const bool osrevOnly = RuntimeFlags_ObjectiveStatusEnableOsRevOnly();
  if (osrevOnly) {
    static bool s_osrevSlcDisabledLogged = false;
    if (!s_osrevSlcDisabledLogged) {
      s_osrevSlcDisabledLogged = true;
      LogToFile("[OSREV] SLC objective/status authority disabled");
    }
  }
  const bool objectiveAuthorityJoinCandidate =
      (LooksLikeHudLocalizationKey(result) && result[0] != '\\' &&
       IsObjectiveListBootstrapKeyName(result));
  (void)objectiveAuthorityJoinCandidate;

  if (!osrevOnly && ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset) && stringValue > 0 &&
      LooksLikeHudLocalizationKey(result) && result[0] != '\\') {
    const uintptr_t slcRsp = g_SLC_CallerRegs.rsp;
    const uintptr_t slcRbp = g_SLC_CallerRegs.rbp;
    const uintptr_t slcRdi = g_SLC_CallerRegs.rdi;
    const uintptr_t slcRsi = g_SLC_CallerRegs.rsi;
    const uintptr_t slcR12 = g_SLC_CallerRegs.r12;
    const uintptr_t slcRcx = g_SLC_CallerRegs.rcx;
    const uintptr_t slcRdx = g_SLC_CallerRegs.rdx;
    const uintptr_t slcR8 = g_SLC_CallerRegs.r8;
    const uintptr_t slcR9 = g_SLC_CallerRegs.r9;
    const uintptr_t slcTid = g_SLC_CallerRegs.tid;
    const uintptr_t slcRet = g_SLC_CallerRegs.ret;
    const uint32_t curTid = GetCurrentThreadId();
    uintptr_t slcCallerOffset = 0;
    if (slcRet >= s_moduleBase) {
      slcCallerOffset = slcRet - s_moduleBase;
    }
    bool pendingHandoffMissLog = false;
    uint32_t missMapQuery = 0;
    uintptr_t missMapCaller = 0;
    DWORD missMapAge = 0xFFFFFFFFu;
    DWORD missTlsAge = 0xFFFFFFFFu;
    uint32_t handoffQuery = 0;
    uintptr_t handoffCaller = 0;
    const char *handoffSource = "none";
    bool typeChkJoined = false;
    uint32_t typeChkType = 0;
    uint32_t typeChkOrdinal = 0;
    uintptr_t typeChkR12 = 0;
    uintptr_t typeChkRsi = 0;

    // Hard safety: do not re-call string type-check from inside
    // SL_ConvertToString objective path.
    // Engine already performed this check before entering +0x34E25E.
    // Re-calling here can perturb native flow.

    if (!typeChkJoined &&
        g_objTypeChkTlsCtx.queryIdx == stringValue &&
        ObjRev_IsObjectiveAuthorityTypeCheckCaller(
            g_objTypeChkTlsCtx.callerOffset) &&
        (now - g_objTypeChkTlsCtx.tick) <= 64) {
      typeChkJoined = true;
      typeChkType = g_objTypeChkTlsCtx.resultType;
      typeChkOrdinal = g_objTypeChkTlsCtx.ordinal;
      typeChkR12 = g_objTypeChkTlsCtx.r12;
      typeChkRsi = g_objTypeChkTlsCtx.rsi;
      static std::unordered_map<uint32_t, DWORD> s_typeJoinLogTick;
      auto itJoin = s_typeJoinLogTick.find(stringValue);
      if (itJoin == s_typeJoinLogTick.end() || (now - itJoin->second) > 1200) {
        s_typeJoinLogTick[stringValue] = now;
        char jbuf[384];
        sprintf_s(jbuf,
                  "[OBJ-TYPECHK-JOIN] slc=0x%X type=%u ord=%u r12=0x%llX "
                  "rsi=0x%llX preCaller=+0x%llX",
                  stringValue, typeChkType, typeChkOrdinal,
                  (unsigned long long)typeChkR12,
                  (unsigned long long)typeChkRsi,
                  (unsigned long long)g_objTypeChkTlsCtx.callerOffset);
        LogToFile(jbuf);
      }
    }

    if (g_objCfgSlcTlsCtx.slcIdx == stringValue &&
        g_objCfgSlcTlsCtx.queryIdx != 0 &&
        (now - g_objCfgSlcTlsCtx.tick) <= 64) {
      handoffQuery = g_objCfgSlcTlsCtx.queryIdx;
      handoffCaller = g_objCfgSlcTlsCtx.callerOffset;
      handoffSource = "tls";
    }

    if (handoffQuery == 0) {
      std::lock_guard<std::mutex> lk(g_objCfgSlcRawMutex);
      auto itRaw = g_objCfgSlcRawBySlc.find(stringValue);
      if (itRaw != g_objCfgSlcRawBySlc.end() && itRaw->second.queryIdx != 0 &&
          (now - itRaw->second.tick) <= 500) {
        handoffQuery = itRaw->second.queryIdx;
        handoffCaller = itRaw->second.callerOffset;
        handoffSource = "map";
      }
    }

    // Thread-local ring rescue:
    // A single TLS slot is frequently overwritten before +0x34E25E consumes it.
    // Keep recent cfg->slc calls and recover the best temporal match.
    if (handoffQuery == 0) {
      ObjCfgSlcTlsContext ringHit{};
      if (ObjRev_FindRecentCfgSlcTlsForSlc(stringValue, now, 280, ringHit) &&
          ringHit.queryIdx != 0) {
        handoffQuery = ringHit.queryIdx;
        handoffCaller = ringHit.callerOffset;
        handoffSource = "tls_ring";
      }
    }

    if (handoffQuery != 0) {
      uint32_t cfgIdx = 0;
      uint32_t bias = 0;
      const uintptr_t cfgRetAddr =
          (handoffCaller > 0) ? ((uintptr_t)s_moduleBase + handoffCaller) : 0;
      if (cfgRetAddr > 0 &&
          ObjRev_DecodeCfgBaseFromCaller(handoffQuery, cfgRetAddr, cfgIdx, bias)) {
        {
          std::lock_guard<std::mutex> lkCfg(g_slcToKeyMutex);
          g_slcToKey[stringValue] = result;
          if (g_slcToKey.size() > 16384) {
            g_slcToKey.clear();
          }
        }

        // Objective root-fix: promote only from objective SLC callsite.
        ObjRev_RecordDirectCfgSlc(cfgIdx, bias, stringValue, callerOffset);

        static std::unordered_map<uint64_t, DWORD> s_handoffLogTick;
        const uint64_t sig = ((uint64_t)cfgIdx << 32) | (uint64_t)stringValue;
        auto it = s_handoffLogTick.find(sig);
        if (it == s_handoffLogTick.end() || (now - it->second) > 1500) {
          s_handoffLogTick[sig] = now;
          char hbuf[384];
          sprintf_s(hbuf,
                    "[OBJ-CFGSLC-HANDOFF] src=%s cfg=0x%X bias=0x%X query=0x%X slc=0x%X key=%s cfgCaller=+0x%llX",
                    handoffSource, cfgIdx, bias, handoffQuery, stringValue,
                    result, (unsigned long long)handoffCaller);
          LogToFile(hbuf);
        }
      } else {
        static std::unordered_map<uint64_t, DWORD> s_handoffDecodeMissTick;
        const uint64_t sig =
            ((uint64_t)handoffQuery << 32) | (uint64_t)stringValue;
        auto it = s_handoffDecodeMissTick.find(sig);
        if (it == s_handoffDecodeMissTick.end() || (now - it->second) > 2000) {
          s_handoffDecodeMissTick[sig] = now;
          char mbuf[320];
          sprintf_s(
              mbuf,
              "[OBJ-CFGSLC-HANDOFF-DECODE-MISS] src=%s query=0x%X slc=0x%X key=%s cfgCaller=+0x%llX",
              handoffSource, handoffQuery, stringValue, result,
              (unsigned long long)handoffCaller);
          LogToFile(mbuf);
        }
      }
    } else {
      static std::unordered_map<uint32_t, DWORD> s_handoffMissTick;
      auto it = s_handoffMissTick.find(stringValue);
      if (it == s_handoffMissTick.end() || (now - it->second) > 2000) {
        s_handoffMissTick[stringValue] = now;
        uint32_t mapQuery = 0;
        uintptr_t mapCaller = 0;
        DWORD mapAge = 0xFFFFFFFFu;
        {
          std::lock_guard<std::mutex> lk(g_objCfgSlcRawMutex);
          auto itRaw = g_objCfgSlcRawBySlc.find(stringValue);
          if (itRaw != g_objCfgSlcRawBySlc.end()) {
            mapQuery = itRaw->second.queryIdx;
            mapCaller = itRaw->second.callerOffset;
            mapAge = now - itRaw->second.tick;
          }
        }
        DWORD tlsAge = 0xFFFFFFFFu;
        if (g_objCfgSlcTlsCtx.tick != 0 && now >= g_objCfgSlcTlsCtx.tick) {
          tlsAge = now - g_objCfgSlcTlsCtx.tick;
        }
        pendingHandoffMissLog = true;
        missMapQuery = mapQuery;
        missMapCaller = mapCaller;
        missMapAge = mapAge;
        missTlsAge = tlsAge;

        // Deterministic reverse probe (authoritative event only):
        // On +0x34E25E handoff miss, inspect the immediate producer context
        // memory for cfg indices whose direct-map slc equals this objective slc.
        // This avoids guess pairing and keeps evidence tied to the same event.
        if (ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset)) {
          auto IsReadableLocal = [](uintptr_t p, size_t n) -> bool {
            return (p > 0x10000 && p < 0x7FFFFFFFFFFF &&
                    IsBadReadPtr((void *)p, n) == 0);
          };

          std::unordered_map<uint32_t, CfgDirectMapEntry> slcCfgMatches;
          {
            std::lock_guard<std::mutex> lk(g_cfgDirectMapMutex);
            for (const auto &kv : g_cfgDirectMap) {
              const uint32_t cfg = kv.first;
              const CfgDirectMapEntry &e = kv.second;
              if (cfg == 0 || e.slcIdx == 0) {
                continue;
              }
              if (e.slcIdx != stringValue) {
                continue;
              }
              if ((now - e.tick) > 15000) {
                continue;
              }
              slcCfgMatches[cfg] = e;
            }
          }

          if (!slcCfgMatches.empty()) {
            struct ProbeBase {
              const char *name;
              uintptr_t ptr;
            };
            const ProbeBase bases[] = {
                {"rsi", slcRsi},
                {"rdx", slcRdx},
                {"r8", slcR8},
            };

            int totalHits = 0;
            for (const auto &b : bases) {
              if (b.ptr == 0 || !IsReadableLocal(b.ptr, 0x40)) {
                continue;
              }
              int hits = 0;
              for (int off = -0x120; off <= 0x240 && hits < 6; off += 4) {
                const intptr_t ai = (intptr_t)b.ptr + (intptr_t)off;
                if (ai <= 0) {
                  continue;
                }
                const uintptr_t a = (uintptr_t)ai;
                if (!IsReadableLocal(a, sizeof(uint32_t))) {
                  continue;
                }
                const uint32_t v = *(volatile uint32_t *)a;
                auto itCfg = slcCfgMatches.find(v);
                if (itCfg == slcCfgMatches.end()) {
                  continue;
                }

                const CfgDirectMapEntry &e = itCfg->second;
                char cbuf[384];
                sprintf_s(
                    cbuf,
                    "[OBJ-CFGSLC-CFGMATCH] slc=0x%X key=%s cfg=0x%X src=%s%+d "
                    "base=0x%llX bias=0x%X q=%d hits=%u age=%lu",
                    stringValue, result, v, b.name, off,
                    (unsigned long long)b.ptr, e.bias, e.quality,
                    (unsigned int)e.hits, (unsigned long)(now - e.tick));
                LogToFile(cbuf);
                hits += 1;
                totalHits += 1;
              }
            }

            if (totalHits == 0) {
              char mbuf[224];
              sprintf_s(
                  mbuf,
                  "[OBJ-CFGSLC-CFGMATCH-MISS] slc=0x%X key=%s candCfg=%u rsi=0x%llX "
                  "rdx=0x%llX r8=0x%llX",
                  stringValue, result, (unsigned int)slcCfgMatches.size(),
                  (unsigned long long)slcRsi, (unsigned long long)slcRdx,
                  (unsigned long long)slcR8);
              LogToFile(mbuf);
            }
          }
        }
      }
    }

    // +0x34E25E stack/reg deep probe path is a closed dead-end for objective
    // authority. Keep disabled by default to avoid noisy reverse regressions.
    constexpr bool kEnableObjCfgSlcDeepProbe = false;
    static std::unordered_set<uint32_t> s_objCfgSlcProbeDumped;
    if (kEnableObjCfgSlcDeepProbe &&
        s_objCfgSlcProbeDumped.insert(stringValue).second) {
      char pbuf[512];
      sprintf_s(pbuf,
                "[OBJ-CFGSLC-PROBE] slc=0x%X key=%s rdi=0x%llX rsi=0x%llX "
                "r12=0x%llX rbp=0x%llX rsp=0x%llX rcx=0x%llX rdx=0x%llX "
                "r8=0x%llX r9=0x%llX stubCaller=+0x%llX tid=%llu curTid=%u",
                stringValue, result, (unsigned long long)slcRdi,
                (unsigned long long)slcRsi, (unsigned long long)slcR12,
                (unsigned long long)slcRbp, (unsigned long long)slcRsp,
                (unsigned long long)slcRcx, (unsigned long long)slcRdx,
                (unsigned long long)slcR8, (unsigned long long)slcR9,
                (unsigned long long)slcCallerOffset,
                (unsigned long long)slcTid, curTid);
      LogToFile(pbuf);

      auto IsReadableLocal = [](uintptr_t p, size_t n) -> bool {
        return (p > 0x10000 && p < 0x7FFFFFFFFFFF &&
                IsBadReadPtr((void *)p, n) == 0);
      };

      auto DumpRegRows = [&](const char *tag, uintptr_t basePtr) {
        if (!IsReadableLocal(basePtr, 0x60)) {
          char ubuf[192];
          sprintf_s(ubuf, "[OBJ-CFGSLC-%s-DUMP] unreadable ptr=0x%llX", tag,
                    (unsigned long long)basePtr);
          LogToFile(ubuf);
          return;
        }
        unsigned char snap[0x60] = {0};
        memcpy(snap, (const void *)basePtr, sizeof(snap));
        for (int row = 0; row < 6; ++row) {
          const uintptr_t b = basePtr + (uintptr_t)(row * 0x10);
          const unsigned char *r = snap + row * 0x10;
          uint32_t d0 = *(const uint32_t *)(r + 0x0);
          uint32_t d4 = *(const uint32_t *)(r + 0x4);
          uint32_t d8 = *(const uint32_t *)(r + 0x8);
          uint32_t dc = *(const uint32_t *)(r + 0xC);
          char dbuf[320];
          sprintf_s(dbuf,
                    "[OBJ-CFGSLC-%s-DUMP] row=%d base=0x%llX d0=0x%X d4=0x%X "
                    "d8=0x%X dc=0x%X",
                    tag, row, (unsigned long long)b, d0, d4, d8, dc);
          LogToFile(dbuf);
        }
      };

      DumpRegRows("RSI", slcRsi);
      DumpRegRows("RSP", slcRsp);
      if (slcRbp != slcRsp)
        DumpRegRows("RBP", slcRbp);
      if (slcRdx != slcRsi)
        DumpRegRows("RDX", slcRdx);
      if (slcR8 != slcRsi && slcR8 != slcRdx)
        DumpRegRows("R8", slcR8);
      if (slcR9 != slcRsi && slcR9 != slcRdx && slcR9 != slcR8)
        DumpRegRows("R9", slcR9);

      if (retAddr && IsReadableLocal((uintptr_t)retAddr - 40, 80)) {
        const unsigned char *rip = (const unsigned char *)retAddr;
        std::stringstream rs;
        rs << "[OBJ-CFGSLC-RIP] slc=0x" << std::hex << std::uppercase
           << stringValue << " ret=+0x" << (unsigned long long)callerOffset
           << " bytes(-40..+39): ";
        for (int j = -40; j < 40; ++j) {
          rs << std::setw(2) << std::setfill('0') << std::hex
             << (unsigned int)rip[j] << " ";
        }
        LogToFile(rs.str());
      } else {
        char rb[192];
        sprintf_s(rb, "[OBJ-CFGSLC-RIP] slc=0x%X unreadable ret=+0x%llX",
                  stringValue, (unsigned long long)callerOffset);
        LogToFile(rb);
      }

      // One-shot direct reverse: look for HudElem pointers/cfg-like values
      // around objective producer context pointers.
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
        LogToFile("[OBJ-CFGSLC-HUDPTR] hud_array_unavailable");
      } else {
        const uintptr_t hudEnd = hudBase + (uintptr_t)hudStride * (uintptr_t)hudCount;
        std::vector<uint32_t> activeCfgs;
        {
          std::lock_guard<std::mutex> lock(g_activeObjectiveCfgMutex);
          for (const auto &kv : g_activeObjectiveCfgTick) {
            if ((now - kv.second) <= 20000 && kv.first > 0 && kv.first < 0x10000u) {
              activeCfgs.push_back(kv.first);
            }
          }
        }
        std::sort(activeCfgs.begin(), activeCfgs.end());
        activeCfgs.erase(std::unique(activeCfgs.begin(), activeCfgs.end()),
                         activeCfgs.end());
        if (!activeCfgs.empty()) {
          std::stringstream as;
          as << "[OBJ-CFGSLC-ACTCFG] slc=0x" << std::hex << std::uppercase
             << stringValue << " count=" << std::dec << activeCfgs.size()
             << " vals=";
          for (size_t i = 0; i < activeCfgs.size(); ++i) {
            if (i) as << ",";
            as << "0x" << std::hex << std::uppercase << activeCfgs[i];
          }
          LogToFile(as.str());
        } else {
          char mbuf[160];
          sprintf_s(mbuf, "[OBJ-CFGSLC-ACTCFG] slc=0x%X count=0", stringValue);
          LogToFile(mbuf);
        }

        auto ScanHudPtrs = [&](const char *tag, uintptr_t basePtr) {
          if (basePtr == 0) {
            return;
          }
          int hitCount = 0;
          std::unordered_set<uintptr_t> seenPtrs;
          for (int off = -0x120; off <= 0x240 && hitCount < 12; off += 8) {
            const intptr_t ai = (intptr_t)basePtr + (intptr_t)off;
            if (ai <= 0) {
              continue;
            }
            const uintptr_t a = (uintptr_t)ai;
            if (!IsReadableLocal(a, sizeof(uintptr_t))) {
              continue;
            }
            const uintptr_t q = *(volatile uintptr_t *)a;
            if (q < hudBase || q >= hudEnd) {
              continue;
            }
            const uintptr_t rel = q - hudBase;
            if ((rel % (uintptr_t)hudStride) != 0) {
              continue;
            }
            if (!seenPtrs.insert(q).second) {
              continue;
            }
            const int idx = (int)(rel / (uintptr_t)hudStride);
            if (idx < 0 || idx >= hudCount) {
              continue;
            }
            if (!IsReadableLocal(q + 0x94, 4)) {
              continue;
            }
            const uint32_t cfgLabel = *(volatile uint32_t *)(q + 0x80);
            const uint32_t cfgText = *(volatile uint32_t *)(q + 0x84);
            const int fxBirth = *(volatile int *)(q + 0x90);
            const int fxLetter = *(volatile int *)(q + 0x94);
            float x = 0.0f;
            float y = 0.0f;
            if (IsReadableLocal(q + 0x08, sizeof(float))) {
              x = *(volatile float *)(q + 0x04);
              y = *(volatile float *)(q + 0x08);
            }
            char hbuf[512];
            sprintf_s(hbuf,
                      "[OBJ-CFGSLC-HUDPTR] slc=0x%X key=%s src=%s off=%+d "
                      "ptr=0x%llX idx=%d cfgLabel=0x%X cfgText=0x%X "
                      "fxB=%d fxL=%d x=%.1f y=%.1f",
                      stringValue, result, tag, off, (unsigned long long)q, idx,
                      cfgLabel, cfgText, fxBirth, fxLetter, x, y);
            LogToFile(hbuf);
            ++hitCount;
          }
          if (hitCount == 0) {
            char mbuf[256];
            sprintf_s(mbuf, "[OBJ-CFGSLC-HUDPTR-MISS] slc=0x%X src=%s base=0x%llX",
                      stringValue, tag, (unsigned long long)basePtr);
            LogToFile(mbuf);
          }
        };

        auto ScanCfgReferences = [&](const char *tag, uintptr_t basePtr) {
          if (basePtr == 0) {
            return;
          }
          int hitCount = 0;
          std::unordered_set<uint64_t> dedup;
          for (int off = -0x280; off <= 0x500 && hitCount < 64; off += 4) {
            const intptr_t ai = (intptr_t)basePtr + (intptr_t)off;
            if (ai <= 0) {
              continue;
            }
            const uintptr_t a = (uintptr_t)ai;
            if (!IsReadableLocal(a, 4)) {
              continue;
            }
            const uint32_t v32 = *(volatile uint32_t *)a;
            bool matched = false;
            if (!activeCfgs.empty()) {
              for (uint32_t cfg : activeCfgs) {
                const uint16_t lo16 = (uint16_t)(v32 & 0xFFFFu);
                const uint16_t hi16 = (uint16_t)((v32 >> 16) & 0xFFFFu);
                if (v32 == cfg || lo16 == cfg || hi16 == cfg) {
                  uint32_t mode = (v32 == cfg) ? 0u : ((lo16 == cfg) ? 1u : 2u);
                  const uint64_t sig =
                      ((uint64_t)(uint32_t)off << 32) ^
                      ((uint64_t)cfg << 4) ^ (uint64_t)mode;
                  if (!dedup.insert(sig).second) {
                    continue;
                  }
                  char cbuf[320];
                  sprintf_s(
                      cbuf,
                      "[OBJ-CFGSLC-CFGREF] slc=0x%X key=%s src=%s off=%+d "
                      "w=32 mode=%u v=0x%X cfg=0x%X",
                      stringValue, result, tag, off, mode, v32, cfg);
                  LogToFile(cbuf);
                  ++hitCount;
                  matched = true;
                  if (hitCount >= 64) {
                    break;
                  }
                }
              }
            }
            if (matched) {
              continue;
            }
            if (!IsReadableLocal(a, 2)) {
              continue;
            }
            const uint16_t v16 = *(volatile uint16_t *)a;
            if (!activeCfgs.empty()) {
              for (uint32_t cfg : activeCfgs) {
                if (v16 == (uint16_t)cfg) {
                  const uint64_t sig =
                      ((uint64_t)(uint32_t)(off << 1) << 32) ^ (uint64_t)cfg;
                  if (!dedup.insert(sig).second) {
                    continue;
                  }
                  char cbuf[320];
                  sprintf_s(
                      cbuf,
                      "[OBJ-CFGSLC-CFGREF] slc=0x%X key=%s src=%s off=%+d "
                      "w=16 mode=0 v=0x%X cfg=0x%X",
                      stringValue, result, tag, off, (unsigned int)v16, cfg);
                  LogToFile(cbuf);
                  ++hitCount;
                  if (hitCount >= 64) {
                    break;
                  }
                }
              }
            }
          }
          if (hitCount == 0) {
            char mbuf[256];
            sprintf_s(mbuf,
                      "[OBJ-CFGSLC-CFGREF-MISS] slc=0x%X src=%s base=0x%llX",
                      stringValue, tag, (unsigned long long)basePtr);
            LogToFile(mbuf);
          }
        };

        ScanHudPtrs("RSI", slcRsi);
        ScanCfgReferences("RSI", slcRsi);

        auto ScanIndirectPtrs = [&](const char *tag, uintptr_t basePtr) {
          if (basePtr == 0 || !IsReadableLocal(basePtr, 0x80)) {
            return;
          }
          int ptrHits = 0;
          int cfgHits = 0;
          std::unordered_set<uintptr_t> seenPtrs;
          for (int off = -0x40; off <= 0x140 && ptrHits < 12; off += 8) {
            const intptr_t ai = (intptr_t)basePtr + (intptr_t)off;
            if (ai <= 0) {
              continue;
            }
            const uintptr_t a = (uintptr_t)ai;
            if (!IsReadableLocal(a, sizeof(uintptr_t))) {
              continue;
            }
            const uintptr_t p = *(volatile uintptr_t *)a;
            if (!IsReadableLocal(p, 0x40)) {
              continue;
            }
            if (!seenPtrs.insert(p).second) {
              continue;
            }
            ++ptrHits;

            char pbuf2[256];
            sprintf_s(pbuf2,
                      "[OBJ-CFGSLC-INDPTR] slc=0x%X key=%s src=%s off=%+d ptr=0x%llX",
                      stringValue, result, tag, off, (unsigned long long)p);
            LogToFile(pbuf2);

            // Pointer target scan: cfg-like values + HudElem array pointers.
            for (int poff = 0; poff <= 0x180 && cfgHits < 24; poff += 4) {
              const uintptr_t pa = p + (uintptr_t)poff;
              if (!IsReadableLocal(pa, 4)) {
                continue;
              }
              const uint32_t v = *(volatile uint32_t *)pa;
              if (v >= 0x40u && v <= 0x90u) {
                char cbuf2[320];
                sprintf_s(cbuf2,
                          "[OBJ-CFGSLC-IND-CFG] slc=0x%X key=%s src=%s "
                          "baseOff=%+d ptr=0x%llX off=+0x%X v=0x%X",
                          stringValue, result, tag, off,
                          (unsigned long long)p, poff, v);
                LogToFile(cbuf2);
                ++cfgHits;
              }
              if ((poff % 8) == 0 && IsReadableLocal(pa, sizeof(uintptr_t))) {
                const uintptr_t q = *(volatile uintptr_t *)pa;
                if (q >= hudBase && q < hudEnd) {
                  const uintptr_t rel = q - hudBase;
                  if ((rel % (uintptr_t)hudStride) == 0) {
                    const int idx = (int)(rel / (uintptr_t)hudStride);
                    if (idx >= 0 && idx < hudCount) {
                      char hbuf2[320];
                      sprintf_s(hbuf2,
                                "[OBJ-CFGSLC-IND-HUDPTR] slc=0x%X key=%s src=%s "
                                "baseOff=%+d ptr=0x%llX off=+0x%X q=0x%llX idx=%d",
                                stringValue, result, tag, off,
                                (unsigned long long)p, poff,
                                (unsigned long long)q, idx);
                      LogToFile(hbuf2);
                    }
                  }
                }
              }
            }
          }
          if (ptrHits == 0) {
            char mbuf[256];
            sprintf_s(mbuf,
                      "[OBJ-CFGSLC-INDPTR-MISS] slc=0x%X src=%s base=0x%llX",
                      stringValue, tag, (unsigned long long)basePtr);
            LogToFile(mbuf);
          }
        };

        ScanIndirectPtrs("RSI", slcRsi);
      }
    }
    if (pendingHandoffMissLog) {
      char ring0[96] = "-";
      char ring1[96] = "-";
      char ring2[96] = "-";
      auto ringRecent = ObjRev_GetRecentCfgSlcTlsList(now, 320, 3);
      for (size_t ri = 0; ri < ringRecent.size() && ri < 3; ++ri) {
        const ObjCfgSlcTlsContext &rv = ringRecent[ri];
        const DWORD rAge = (now >= rv.tick) ? (now - rv.tick) : 0;
        char tmp[96];
        sprintf_s(tmp, "q=0x%X/slc=0x%X/a=%u/c=+0x%llX", rv.queryIdx,
                  rv.slcIdx, (unsigned int)rAge,
                  (unsigned long long)rv.callerOffset);
        if (ri == 0) {
          strcpy_s(ring0, tmp);
        } else if (ri == 1) {
          strcpy_s(ring1, tmp);
        } else {
          strcpy_s(ring2, tmp);
        }
      }
      char mbuf[1400];
      sprintf_s(mbuf,
                "[OBJ-CFGSLC-HANDOFF-MISS] slc=0x%X key=%s caller=+0x%llX "
                "stubCaller=+0x%llX tid=%llu curTid=%u "
                "typeChk(join=%u type=%u ord=%u r12=0x%llX rsi=0x%llX) "
                "tls(q=0x%X slc=0x%X age=%u caller=+0x%llX) "
                "map(q=0x%X age=%u caller=+0x%llX) "
                "ring[%s | %s | %s] "
                "regs(rdi=0x%llX rsi=0x%llX r12=0x%llX rbp=0x%llX rsp=0x%llX "
                "rcx=0x%llX rdx=0x%llX r8=0x%llX r9=0x%llX)",
                stringValue, result, (unsigned long long)callerOffset,
                (unsigned long long)slcCallerOffset,
                (unsigned long long)slcTid, curTid,
                typeChkJoined ? 1u : 0u, typeChkType, typeChkOrdinal,
                (unsigned long long)typeChkR12, (unsigned long long)typeChkRsi,
                g_objCfgSlcTlsCtx.queryIdx, g_objCfgSlcTlsCtx.slcIdx, missTlsAge,
                (unsigned long long)g_objCfgSlcTlsCtx.callerOffset,
                missMapQuery, missMapAge, (unsigned long long)missMapCaller,
                ring0, ring1, ring2,
                (unsigned long long)slcRdi, (unsigned long long)slcRsi,
                (unsigned long long)slcR12, (unsigned long long)slcRbp,
                (unsigned long long)slcRsp, (unsigned long long)slcRcx,
                (unsigned long long)slcRdx, (unsigned long long)slcR8,
                (unsigned long long)slcR9);
      LogToFile(mbuf);
    }
  }

  // Minimal reverse probe mode:
  // 1) Only objective caller (+0x34E25E) + objective key.
  // 2) Capture once per key.
  // 3) If the same source signature repeats twice in a row, auto-lock probe.
  constexpr bool kEnableLegacyObjSrcProbe = false; // dead-end path (kept for fallback reference only)
  if (kEnableLegacyObjSrcProbe &&
      ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset) &&
      LooksLikeHudLocalizationKey(result) &&
      IsObjectiveOverlayKeyName(result)) {
    static bool s_objSrcProbeLocked = false;
    static bool s_objRipDumped = false;
    static std::unordered_set<std::string> s_objSrcProbedKeys;
    static std::string s_lastHitSig;
    static int s_hitStreak = 0;

    if (!s_objSrcProbeLocked &&
        s_objSrcProbedKeys.insert(std::string(result)).second) {
      auto IsReadableLocal = [](uintptr_t p, size_t n) -> bool {
        return (p > 0x10000 && p < 0x7FFFFFFFFFFF &&
                IsBadReadPtr((void *)p, n) == 0);
      };

      if (!s_objRipDumped) {
        s_objRipDumped = true;
        unsigned char *rip = (unsigned char *)retAddr;
        std::stringstream ss;
        ss << "[OBJ-SLC-RIP-DUMP] caller=+0x" << std::hex
           << std::uppercase << (unsigned long long)callerOffset << " id=0x"
           << std::hex
           << std::uppercase << stringValue << " bytes(-32..+16): ";
        for (int j = -32; j <= 16; ++j) {
          ss << std::setw(2) << std::setfill('0') << std::hex
             << (unsigned int)rip[j] << " ";
        }
        LogToFile(ss.str());
      }

      struct ProbeBase {
        const char *name;
        uintptr_t ptr;
      };
      const ProbeBase bases[] = {
          {"rsp", g_SLC_CallerRegs.rsp}, {"rbp", g_SLC_CallerRegs.rbp},
          {"rdi", g_SLC_CallerRegs.rdi},
          {"rsi", g_SLC_CallerRegs.rsi}, {"rbx", g_SLC_CallerRegs.rbx},
          {"r12", g_SLC_CallerRegs.r12}, {"r13", g_SLC_CallerRegs.r13},
          {"r14", g_SLC_CallerRegs.r14}, {"r15", g_SLC_CallerRegs.r15},
          {"rcx", g_SLC_CallerRegs.rcx}, {"rdx", g_SLC_CallerRegs.rdx},
          {"r8", g_SLC_CallerRegs.r8},   {"r9", g_SLC_CallerRegs.r9},
          {"r10", g_SLC_CallerRegs.r10}, {"r11", g_SLC_CallerRegs.r11},
      };

      struct Hit32 {
        bool found = false;
        const char *src = "";
        const char *base = "";
        uintptr_t ptr = 0;
        int off = 0;
        uint32_t m4 = 0;
        uint32_t v = 0;
        uint32_t p4 = 0;
      } hit;

      auto TryHit = [&](const char *srcTag, const char *baseName, uintptr_t basePtr,
                        int off) -> bool {
        if (!IsReadableLocal(basePtr + (uintptr_t)off, 4))
          return false;
        uint32_t v = *(volatile uint32_t *)(basePtr + (uintptr_t)off);
        if (v != stringValue)
          return false;
        hit.found = true;
        hit.src = srcTag;
        hit.base = baseName;
        hit.ptr = basePtr;
        hit.off = off;
        hit.v = v;
        if (off >= 4 && IsReadableLocal(basePtr + (uintptr_t)off - 4, 4))
          hit.m4 = *(volatile uint32_t *)(basePtr + (uintptr_t)off - 4);
        if (IsReadableLocal(basePtr + (uintptr_t)off + 4, 4))
          hit.p4 = *(volatile uint32_t *)(basePtr + (uintptr_t)off + 4);
        return true;
      };

      // Deterministic first-hit scan: register contexts first.
      for (const auto &b : bases) {
        if (hit.found)
          break;
        if (!IsReadableLocal(b.ptr, 0x100))
          continue;
        for (int off = 0; off <= 0xC0; off += 4) {
          if (TryHit("reg", b.name, b.ptr, off))
            break;
        }
      }

      // Fallback: stack pointers.
      if (!hit.found) {
        const uintptr_t rspRet = (uintptr_t)_AddressOfReturnAddress();
        if (IsReadableLocal(rspRet, 0x200)) {
          for (int soff = 0; soff <= 0x180 && !hit.found; soff += 8) {
            if (!IsReadableLocal(rspRet + (uintptr_t)soff, 8))
              continue;
            uintptr_t q = *(volatile uintptr_t *)(rspRet + (uintptr_t)soff);
            if (!IsReadableLocal(q, 0x100))
              continue;
            for (int off = 0; off <= 0x80; off += 4) {
              if (TryHit("stack", "stackptr", q, off))
                break;
            }
          }
        }
      }

      // If the value is synthesized in registers (not stored in memory),
      // detect the call-site argument generation pattern directly.
      if (!hit.found) {
        unsigned char *rip = (unsigned char *)retAddr;
        // Common objective path pattern near +0x34E25E:
        //   41 8D 4C 24 xx  => lea ecx, [r12+imm8]
        for (int j = -32; j <= -5; ++j) {
          if (rip[j + 0] == 0x41 && rip[j + 1] == 0x8D &&
              rip[j + 2] == 0x4C && rip[j + 3] == 0x24) {
            const int imm = (int)(int8_t)rip[j + 4];
            const uint32_t r12v = (uint32_t)g_SLC_CallerRegs.r12;
            const uint32_t calc = (uint32_t)(r12v + imm);
            if (calc == stringValue) {
              hit.found = true;
              hit.src = "argpat";
              hit.base = "r12";
              hit.ptr = (uintptr_t)g_SLC_CallerRegs.r12;
              hit.off = imm;
              hit.m4 = r12v;
              hit.v = calc;
              hit.p4 = 0;

              char abuf[320];
              sprintf_s(abuf,
                        "[OBJ-SLC-ARGPAT] id=0x%X key=%s pat=lea_ecx_[r12%+d] "
                        "r12=0x%X calc=0x%X",
                        (unsigned int)stringValue, result, imm,
                        (unsigned int)r12v, (unsigned int)calc);
              LogToFile(abuf);
              break;
            }
          }
        }
      }

      if (hit.found) {
        char sigBuf[128];
        sprintf_s(sigBuf, "%s|%s|0x%X", hit.src, hit.base,
                  (unsigned int)hit.off);
        std::string sig(sigBuf);
        if (sig == s_lastHitSig) {
          s_hitStreak++;
        } else {
          s_lastHitSig = sig;
          s_hitStreak = 1;
        }

        char hbuf[512];
        sprintf_s(
            hbuf,
            "[OBJ-SLC-SRC32] id=0x%X key=%s sig=%s streak=%d ptr=0x%llX "
            "m4=0x%X v=0x%X p4=0x%X",
            (unsigned int)stringValue, result, sig.c_str(), s_hitStreak,
            (unsigned long long)hit.ptr, (unsigned int)hit.m4,
            (unsigned int)hit.v, (unsigned int)hit.p4);
        LogToFile(hbuf);

        if (s_hitStreak >= 2) {
          s_objSrcProbeLocked = true;
          char lbuf[256];
          sprintf_s(lbuf, "[OBJ-SLC-SRC32-LOCK] sig=%s key=%s", sig.c_str(),
                    result);
          LogToFile(lbuf);
        }
      } else {
        // One-shot register delta snapshot for unresolved objective callsites.
        uint32_t rv[15] = {(uint32_t)g_SLC_CallerRegs.rsp,
                           (uint32_t)g_SLC_CallerRegs.rbp,
                           (uint32_t)g_SLC_CallerRegs.rdi,
                           (uint32_t)g_SLC_CallerRegs.rsi,
                           (uint32_t)g_SLC_CallerRegs.rbx,
                           (uint32_t)g_SLC_CallerRegs.r12,
                           (uint32_t)g_SLC_CallerRegs.r13,
                           (uint32_t)g_SLC_CallerRegs.r14,
                           (uint32_t)g_SLC_CallerRegs.r15,
                           (uint32_t)g_SLC_CallerRegs.rcx,
                           (uint32_t)g_SLC_CallerRegs.rdx,
                           (uint32_t)g_SLC_CallerRegs.r8,
                           (uint32_t)g_SLC_CallerRegs.r9,
                           (uint32_t)g_SLC_CallerRegs.r10,
                           (uint32_t)g_SLC_CallerRegs.r11};
        char rbuf[1024];
        sprintf_s(
            rbuf,
            "[OBJ-SLC-SRCREG] id=0x%X key=%s rsp=0x%X(d=%d) rbp=0x%X(d=%d) "
            "rdi=0x%X(d=%d) "
            "rsi=0x%X(d=%d) rbx=0x%X(d=%d) r12=0x%X(d=%d) r13=0x%X(d=%d) "
            "r14=0x%X(d=%d) r15=0x%X(d=%d) rcx=0x%X(d=%d) rdx=0x%X(d=%d) "
            "r8=0x%X(d=%d) r9=0x%X(d=%d) r10=0x%X(d=%d) r11=0x%X(d=%d)",
            (unsigned int)stringValue, result,
            rv[0], (int)((int64_t)stringValue - (int64_t)rv[0]),
            rv[1], (int)((int64_t)stringValue - (int64_t)rv[1]),
            rv[2], (int)((int64_t)stringValue - (int64_t)rv[2]),
            rv[3], (int)((int64_t)stringValue - (int64_t)rv[3]),
            rv[4], (int)((int64_t)stringValue - (int64_t)rv[4]),
            rv[5], (int)((int64_t)stringValue - (int64_t)rv[5]),
            rv[6], (int)((int64_t)stringValue - (int64_t)rv[6]),
            rv[7], (int)((int64_t)stringValue - (int64_t)rv[7]),
            rv[8], (int)((int64_t)stringValue - (int64_t)rv[8]),
            rv[9], (int)((int64_t)stringValue - (int64_t)rv[9]),
            rv[10], (int)((int64_t)stringValue - (int64_t)rv[10]),
            rv[11], (int)((int64_t)stringValue - (int64_t)rv[11]),
            rv[12], (int)((int64_t)stringValue - (int64_t)rv[12]),
            rv[13], (int)((int64_t)stringValue - (int64_t)rv[13]),
            rv[14], (int)((int64_t)stringValue - (int64_t)rv[14]));
        LogToFile(rbuf);

        char mbuf[192];
        sprintf_s(mbuf, "[OBJ-SLC-SRC32-MISS] id=0x%X key=%s",
                  (unsigned int)stringValue, result);
        LogToFile(mbuf);
      }
    }
  }
#endif

  // --- Deduplicated caller logging ---
  // Track unique callers with hit counts. Log first 5 hits per caller.
  {
    static std::unordered_map<uintptr_t, int> s_callerMap;
    static std::mutex s_callerMtx;
    static int s_totalLogs = 0;

    if (s_totalLogs < 200) {
      std::lock_guard<std::mutex> lk(s_callerMtx);
      int &hits = s_callerMap[callerOffset];
      hits++;

      if (hits <= 5 && s_totalLogs < 200) {
        s_totalLogs++;
        char buf[512];
        sprintf_s(buf,
                  "[SLC] caller=0x%llX id=0x%08X text=\"%.80s\" hit#%d",
                  (unsigned long long)callerOffset, stringValue, result, hits);
        LogToFile(buf);
      }
    }
  }

  // --- HUD key match check (cross-reference with FSHook HUD keys) ---
  {
    std::shared_lock<std::shared_mutex> lock(g_OverlayMapMutex);
    std::string upper =
        ToUpperOverlay(StripColorCodesOverlay(std::string(result)));
    auto it = g_EngToKorOverlayMap.find(upper);
    if (it != g_EngToKorOverlayMap.end()) {
      static std::unordered_map<uintptr_t, int> s_hudCallers;
      static std::mutex s_hudMtx;
      static int s_hudLogs = 0;

      std::lock_guard<std::mutex> lk(s_hudMtx);
      int &hits = s_hudCallers[callerOffset];
      hits++;

      if (hits <= 5 && s_hudLogs < 100) {
        s_hudLogs++;
        char buf[512];
        sprintf_s(buf,
                  "[SLC HUD-MATCH] caller=0x%llX id=0x%08X "
                  "eng=\"%.50s\" kor=\"%.30s\" hit#%d",
                  (unsigned long long)callerOffset, stringValue, result,
                  it->second.c_str(), hits);
        LogToFile(buf);
      }
    }
  }

  // --- HUD KEY NAME pattern detection ---
  // Detect localization key names resolved via SLC (e.g., "GAME_GET_TO_COVER")
  // These use HudElem.label (string_t for key name), not the English text.
  {
    bool isHudKey = false;
    bool slcHasRenderParams = false;
    float slcParamX = 0.0f;
    float slcParamY = 0.0f;
    float slcParamSX = 1.0f;
    float slcParamSY = 1.0f;
    float slcParamColor[4] = {1.0f, 1.0f, 1.0f, 0.6f};
    bool slcHasColorParams = false;
    unsigned short slcParamVirtualW = 0;
    unsigned short slcParamVirtualH = 0;
    int slcParamSlotBucket = 0;
    auto IsObjectiveAuthorityCaller = [](uintptr_t off) -> bool {
      // Root-fix strict authority caller (resolved at runtime per build).
      return ObjRev_IsObjectiveAuthoritySlcCaller(off);
    };
    const bool isObjectiveCallerByRip = IsObjectiveAuthorityCaller(callerOffset);
    HudRouteChannel callerSeedChannel = HUD_ROUTE_UNSPECIFIED;
    const bool hasCallerRouteSeed =
        TryGetHudSeedChannelForConsumer(OVERLAY_SOURCE_SLC,
                                        (unsigned int)callerOffset, -1,
                                        callerSeedChannel) &&
        callerSeedChannel != HUD_ROUTE_UNSPECIFIED;
    const bool isIntroKeyName =
        (result && strstr(result, "INTROSCREEN") != nullptr);
    const std::string keyCandidate = result ? std::string(result) : "";
    const bool isCreditsKey = (keyCandidate.rfind("CREDITS_", 0) == 0);
    // Credits detection: latch/tickWindow removed — the game resolves
    // CREDITS_* SLC keys during normal map loading for ALL missions,
    // making SLC-based detection fundamentally unreliable.
    // creditsActive now uses ONLY the credits_active dvar.
    if (strncmp(result, "GAME_", 5) == 0 ||
        strncmp(result, "CGAME_", 6) == 0 ||
        strncmp(result, "EXE_", 4) == 0 ||
        strncmp(result, "SCRIPT_", 7) == 0 ||
        strncmp(result, "CORNERED_", 9) == 0 ||
        strncmp(result, "HINT_", 5) == 0 ||
        strncmp(result, "OBJ_", 4) == 0 ||
        strncmp(result, "INTROSCREEN_", 12) == 0 ||
        strncmp(result, "PLATFORM_", 9) == 0)
      isHudKey = true;

    // Intro keys are handled by the dedicated intro overlay path; don't let
    // them enter runtime hint/objective lifecycles.
    if (isIntroKeyName) {
      isHudKey = false;
    }
    // Runtime-first objective classification:
    // caller +0x34E25E is the objective SLC path, so accept keys even when
    // their names don't match GAME_/OBJ_/OBJECTIVE_* patterns.
    if (!isIntroKeyName && isObjectiveCallerByRip) {
      isHudKey = true;
    }
    // Consumer-authority first: if caller is seeded as HUD/OBJ authority,
    // treat incoming SLC payload as HUD candidate and resolve key from text.
    if (!isIntroKeyName && hasCallerRouteSeed) {
      isHudKey = true;
    }
    if (!isHudKey && !isIntroKeyName) {
      const bool trustedHintCaller =
          (callerOffset == IW6Offsets::Profile::Rva_3720F2 || callerOffset == IW6Offsets::Profile::Rva_2454FC ||
           callerOffset == IW6Offsets::Profile::Rva_369002);
      if (trustedHintCaller) {
        std::string autoReason;
        if (IsGameplayHudKeyName(keyCandidate, &autoReason)) {
          isHudKey = true;
          if (!autoReason.empty()) {
            MaybeLogHudKeyAuto(keyCandidate, callerOffset,
                               std::string("slc_") + autoReason);
          }
        }
      }
    }

#if GHOSTSKOR_OBJECTIVE_REVERSE
    // Objective/HUD reverse path:
    // Keep cfgIndex->key observations from live SLC traffic so HudElem reverse
    // can resolve keys even when direct table reads are transiently unavailable.
#endif

    std::string canonicalKey = keyCandidate;
    if ((canonicalKey.empty() || !LooksLikeHudLocalizationKey(canonicalKey)) &&
        stringValue > 0) {
      std::string mappedFromSlc;
      if (TextHook_GetSlcKey((uint32_t)stringValue, mappedFromSlc) &&
          LooksLikeHudLocalizationKey(mappedFromSlc)) {
        canonicalKey = mappedFromSlc;
        isHudKey = true;
      } else if ((uint32_t)stringValue > 0 && (uint32_t)stringValue <= 0x03FFu) {
        uint32_t directSlc = 0;
        uint32_t directBias = 0;
        if (ObjRev_GetDirectCfgSlc((uint32_t)stringValue, directSlc, directBias,
                                   false)) {
          std::string mappedFromCfg;
          if (TextHook_GetSlcKey(directSlc, mappedFromCfg) &&
              LooksLikeHudLocalizationKey(mappedFromCfg)) {
            canonicalKey = mappedFromCfg;
            isHudKey = true;
          }
        }
      }
    }
    // Credits keys must never enter HUD/objective pipeline — prevents
    // SLC param probe flooding and false HUD rendering during credits.
    if (isCreditsKey) {
      isHudKey = false;
    }
    // Credits guard: suppress ALL HUD/objective intake while credits are
    // active.  Without this, non-CREDITS_ keys (e.g. GAME_GET_TO_COVER)
    // still enter the pipeline via R_AddCmdDrawText during credits,
    // causing false HUDHINT entries and wasted CPU.
    {
      extern std::atomic<bool> g_creditsGuardActive;
      if (g_creditsGuardActive.load(std::memory_order_relaxed)) {
        isHudKey = false;
      }
    }
    if (isHudKey) {
      if (!canonicalKey.empty() && !LooksLikeHudLocalizationKey(canonicalKey)) {
        std::string mappedKey;
        if (ResolveKeyFromEnglishInternal(canonicalKey, mappedKey) &&
            !mappedKey.empty() && LooksLikeHudLocalizationKey(mappedKey)) {
          canonicalKey = mappedKey;
        }
      }
      if (!isIntroKeyName && !canonicalKey.empty() &&
          LooksLikeHudLocalizationKey(canonicalKey) && result &&
          result[0] != '\0') {
        const std::string runtimeSample = TrimSpaces(StripColorCodes(result));
        const bool sampleLooksKey = LooksLikeHudLocalizationKey(runtimeSample);
        const bool samplePromptLike =
            LooksLikePressHoldPrompt(runtimeSample) ||
            HasBindingPlaceholderToken(result) ||
            (runtimeSample.find(' ') != std::string::npos);
        if (!runtimeSample.empty() && !sampleLooksKey && samplePromptLike) {
          TextHook_RegisterRuntimeEnglishKey(canonicalKey.c_str(), result);
        }
      }
#if GHOSTSKOR_OBJECTIVE_REVERSE
      if (!isIntroKeyName && !canonicalKey.empty() &&
          LooksLikeHudLocalizationKey(canonicalKey) && stringValue > 0) {
        const bool objectiveLikeKey =
            IsObjectiveListBootstrapKeyName(canonicalKey) ||
            IsObjectiveStatusMessageKeyName(canonicalKey);
        const bool allowObjectiveMapWrite =
            (!objectiveLikeKey ||
             ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset));
        if (allowObjectiveMapWrite) {
          std::lock_guard<std::mutex> lkCfg(g_slcToKeyMutex);
          g_slcToKey[stringValue] = canonicalKey;
          // Keep map bounded without introducing a second timestamp map.
          if (g_slcToKey.size() > 16384) {
            g_slcToKey.clear();
          }
        }
      }
#endif
      static std::unordered_map<uintptr_t, int> s_keyCallers;
      static std::mutex s_keyMtx;
      static int s_keyLogs = 0;

      std::lock_guard<std::mutex> lk(s_keyMtx);
      int &hits = s_keyCallers[callerOffset];
      hits++;

      if (hits <= 10 && s_keyLogs < 200) {
        s_keyLogs++;
        char buf[512];
        sprintf_s(buf,
                  "[SLC KEYNAME] caller=0x%llX id=0x%08X key=\"%.60s\" hit#%d",
                  (unsigned long long)callerOffset, stringValue,
                  canonicalKey.empty() ? result : canonicalKey.c_str(), hits);
        LogToFile(buf);
      }

      // === SLC render param capture ===
      // Primary: RBP (legacy verified path).
      // Fallback: register-pointer probe for callers where frame layout differs.
      slcHasRenderParams = false;
      slcParamX = 0.0f;
      slcParamY = 0.0f;
      slcParamSX = 1.0f;
      slcParamSY = 1.0f;
      slcParamVirtualW = 0;
      slcParamVirtualH = 0;
      slcParamSlotBucket = 0;
      bool slcBestParamValid = false;
      float slcBestParamScore = -1.0e30f;
      std::string slcBestParamSource;
      float slcBestX = 0.0f;
      float slcBestY = 0.0f;
      float slcBestSX = 1.0f;
      float slcBestSY = 1.0f;
      unsigned short slcBestVW = 0;
      unsigned short slcBestVH = 0;
      auto TryAcceptSlcRenderParams = [&](float px, float py, float sx, float sy,
                                          const char *srcTag) -> bool {
        const bool finite = (std::isfinite(px) && std::isfinite(py) &&
                             std::isfinite(sx) && std::isfinite(sy));
        const bool plausiblePos = (px > -200.0f && px < 5000.0f &&
                                   py > -200.0f && py < 3000.0f);
        const bool scaleAsFactor =
            (sx > 0.01f && sx < 6.0f && sy > 0.01f && sy < 6.0f);
        // Some SLC callers expose glyph height in pixels (not scale factor).
        const bool scaleAsPixels =
            (sx >= 8.0f && sx <= 256.0f && sy >= 8.0f && sy <= 256.0f);
        if (!finite || !plausiblePos || (!scaleAsFactor && !scaleAsPixels)) {
          return false;
        }

        float workX = px;
        float workY = py;
        float outSX = sx;
        float outSY = sy;
        unsigned short outVW = 0;
        unsigned short outVH = 0;
        const bool centerRelativeProbe =
            scaleAsFactor && (px >= -96.0f && px <= 96.0f) &&
            (py >= -220.0f && py <= 140.0f) &&
            (sx >= 0.20f && sx <= 6.0f) && (sy >= 0.20f && sy <= 6.0f);
        if (scaleAsPixels) {
          outSX = sx / 35.0f;
          outSY = sy / 35.0f;
          if (outSX < 0.15f) outSX = 0.15f;
          if (outSX > 4.5f) outSX = 4.5f;
          if (outSY < 0.15f) outSY = 0.15f;
          if (outSY > 4.5f) outSY = 4.5f;
        } else if (centerRelativeProbe) {
          // Some SLC callsites expose center-relative offsets around (0,0).
          // Convert to the 640x480 virtual basis used by native HUD placement.
          workX = px + 320.0f;
          workY = py + 240.0f;
          outVW = 640;
          outVH = 480;
        } else if (px >= -8.0f && px <= 960.0f && py >= -8.0f && py <= 720.0f) {
          outVW = 640;
          outVH = 480;
        }

        float score = 0.0f;
        score += 20.0f;
        if (workX >= 40.0f && workX <= 1880.0f && workY >= 40.0f &&
            workY <= 1060.0f) {
          score += 28.0f;
        }
        if (workY >= 120.0f && workY <= 900.0f) {
          score += 12.0f;
        }
        if (outSX >= 0.2f && outSX <= 3.0f && outSY >= 0.2f && outSY <= 3.0f) {
          score += 10.0f;
        }
        if (scaleAsPixels) {
          score += 10.0f;
        }
        if (centerRelativeProbe) {
          score += 34.0f;
        }
        if (std::fabs(workX) < 12.0f && std::fabs(workY) < 12.0f) {
          score -= 120.0f;
        }
        if (!centerRelativeProbe && workX >= -2.0f && workX <= 20.0f &&
            workY >= -20.0f && workY <= 20.0f) {
          score -= 70.0f;
        }

        const bool improved = (!slcBestParamValid || score > slcBestParamScore);
        if (!improved) {
          return false;
        }
        slcBestParamValid = true;
        slcBestParamScore = score;
        slcBestParamSource = (srcTag ? srcTag : "unknown");
        slcBestX = workX;
        slcBestY = workY;
        slcBestSX = outSX;
        slcBestSY = outSY;
        slcBestVW = outVW;
        slcBestVH = outVH;
        return true;
      };

      // Commit selected best candidate only once after probing.
      auto CommitBestSlcRenderParams = [&]() {
        if (!slcBestParamValid || slcBestParamScore < 48.0f) {
          return;
        }
        slcHasRenderParams = true;
        slcParamX = slcBestX;
        slcParamY = slcBestY;
        slcParamSX = slcBestSX;
        slcParamSY = slcBestSY;
        slcParamVirtualW = slcBestVW;
        slcParamVirtualH = slcBestVH;
        slcParamSlotBucket =
            TextHook_HudSlotFromEvent(slcParamY, slcParamVirtualH, 0);
        if (slcParamSlotBucket < 0 || slcParamSlotBucket > 5) {
          slcParamSlotBucket = 0;
        }
        static std::unordered_map<std::string, DWORD> s_slcParamProbeTick;
        const DWORD nowProbe = GetTickCount();
        const std::string sig =
            canonicalKey + "|" + std::to_string((unsigned int)callerOffset);
        auto it = s_slcParamProbeTick.find(sig);
        if (it == s_slcParamProbeTick.end() || (nowProbe - it->second) > 1200) {
          s_slcParamProbeTick[sig] = nowProbe;
          if (s_slcParamProbeTick.size() > 512) {
            for (auto it2 = s_slcParamProbeTick.begin();
                 it2 != s_slcParamProbeTick.end();) {
              if ((nowProbe - it2->second) > 15000) {
                it2 = s_slcParamProbeTick.erase(it2);
              } else {
                ++it2;
              }
            }
          }
          char pbuf[448];
          sprintf_s(
              pbuf,
              "[HUD-SLC-PARAM-PROBE] key=%s caller=+0x%llX src=%s x=%.2f y=%.2f sx=%.4f sy=%.4f vw=%u vh=%u q=%.1f",
              canonicalKey.c_str(), (unsigned long long)callerOffset,
              slcBestParamSource.c_str(), slcParamX, slcParamY, slcParamSX,
              slcParamSY, (unsigned int)slcParamVirtualW,
              (unsigned int)slcParamVirtualH, slcBestParamScore);
          LogToFile(pbuf);
        }
      };
      {
        static int s_rbpCount = 0;
        uintptr_t rbp = g_SLC_CallerRegs.rbp;
        if (rbp > 0x10000 && rbp < 0x7FFFFFFFFFFF &&
            !IsBadReadPtr((void *)rbp, 0x14)) {
          float px = *(volatile float *)(rbp + 0x00);
          float py = *(volatile float *)(rbp + 0x04);
          float sx = *(volatile float *)(rbp + 0x08);
          float sy = *(volatile float *)(rbp + 0x0C);
          uint8_t a1 = *(volatile uint8_t *)(rbp + 0x10);
          uint8_t a2 = *(volatile uint8_t *)(rbp + 0x11);

          TryAcceptSlcRenderParams(px, py, sx, sy, "rbp");

          if (s_rbpCount < 50) {
            s_rbpCount++;
            char sbuf[512];
            sprintf_s(sbuf,
                      "[SLC RBPPARAMS] #%d caller=0x%llX "
                      "key=\"%.50s\" "
                      "x=%.2f y=%.2f scaleX=%.4f scaleY=%.4f "
                      "a1=%d a2=%d rbp=0x%llX",
                      s_rbpCount, (unsigned long long)callerOffset, result, px,
                      py, sx, sy, (int)a1, (int)a2, (unsigned long long)rbp);
            LogToFile(sbuf);

            // Also dump raw 32 bytes for verification
            volatile unsigned int *dw = (volatile unsigned int *)rbp;
            sprintf_s(sbuf,
                      "[SLC RBPPARAMS] raw: %08X %08X %08X "
                      "%08X %08X %08X %08X %08X",
                      dw[0], dw[1], dw[2], dw[3], dw[4], dw[5], dw[6], dw[7]);
            LogToFile(sbuf);
          }
        }
      }

      // Secondary probe for SLC callers where RBP is not the render-param base.
      if (!slcHasRenderParams &&
          !ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset)) {
        auto TryProbeBase = [&](uintptr_t base, const char *baseName) -> bool {
          if (!(base > 0x10000 && base < 0x7FFFFFFFFFFF)) {
            return false;
          }
          bool anyAccepted = false;
          for (int off = -0x40; off <= 0x80; off += 4) {
            uintptr_t addr = base + (intptr_t)off;
            if (!(addr > 0x10000 && addr < 0x7FFFFFFFFFFF)) {
              continue;
            }
            if (IsBadReadPtr((void *)addr, 0x10)) {
              continue;
            }
            float px = *(volatile float *)(addr + 0x00);
            float py = *(volatile float *)(addr + 0x04);
            float sx = *(volatile float *)(addr + 0x08);
            float sy = *(volatile float *)(addr + 0x0C);
            char srcTag[48];
            sprintf_s(srcTag, "%s%+d", baseName, off);
            anyAccepted = TryAcceptSlcRenderParams(px, py, sx, sy, srcTag) ||
                          anyAccepted;
          }
          return anyAccepted;
        };

        const struct ProbeReg {
          const char *name;
          uintptr_t value;
        } probeRegs[] = {
            {"rsp", g_SLC_CallerRegs.rsp}, {"rbp", g_SLC_CallerRegs.rbp},
            {"rdi", g_SLC_CallerRegs.rdi}, {"rsi", g_SLC_CallerRegs.rsi},
            {"rbx", g_SLC_CallerRegs.rbx}, {"r12", g_SLC_CallerRegs.r12},
            {"r13", g_SLC_CallerRegs.r13}, {"r14", g_SLC_CallerRegs.r14},
            {"r15", g_SLC_CallerRegs.r15}, {"r10", g_SLC_CallerRegs.r10},
            {"r11", g_SLC_CallerRegs.r11},
        };
        for (const auto &pr : probeRegs) {
          if (slcHasRenderParams) {
            break;
          }
          TryProbeBase(pr.value, pr.name);
        }
      }
      CommitBestSlcRenderParams();

      // --- Live HudElem bridge (objective-reverse data) ---
      // Some gameplay hints (e.g., Press/Hold tutorial prompts) do not expose
      // reliable render params through this SLC caller's RBP frame. In those
      // cases, recover native position/scale/alpha from the live HudElem cache
      // by joining:
      //   SLC stringValue -> cfg/slc observations -> LiveHudElem(cfg slot).
      if (stringValue != 0) {
        struct SlcLiveBridgeHit {
          uint32_t cfgIndex = 0;
          LiveHudElem elem{};
          float score = -1.0e30f;
          bool valid = false;
        };

        const DWORD nowLive = GetTickCount();
        std::unordered_set<uint32_t> cfgCandidates;
        // Always keep direct SLC id as a candidate. Some HudElem lanes expose
        // text SLC directly (not cfg index), so cfg->slc reverse map can be empty.
        if (stringValue > 1) {
          cfgCandidates.insert((uint32_t)stringValue);
        }

        // 1) Direct cfg->slc telemetry reverse lookup.
        {
          std::lock_guard<std::mutex> lk(g_cfgDirectMapMutex);
          for (const auto &kv : g_cfgDirectMap) {
            const uint32_t cfgIdx = kv.first;
            const CfgDirectMapEntry &obs = kv.second;
            if (obs.slcIdx != stringValue) {
              continue;
            }
            if ((nowLive - obs.tick) > 7000) {
              continue;
            }
            cfgCandidates.insert(cfgIdx);
          }
        }

        // 2) Key-index cache join: key -> likely cfg ids (small indices).
        if (!canonicalKey.empty() && LooksLikeHudLocalizationKey(canonicalKey)) {
          std::lock_guard<std::mutex> lk(g_slcToKeyMutex);
          for (const auto &kv : g_slcToKey) {
            if (kv.second != canonicalKey) {
              continue;
            }
            if (kv.first <= 0x03FFu) {
              cfgCandidates.insert(kv.first);
            }
          }
          if (cfgCandidates.empty()) {
            for (const auto &kv : g_slcToKey) {
              if (kv.second != canonicalKey) {
                continue;
              }
              if (kv.first <= 0x0FFFu) {
                cfgCandidates.insert(kv.first);
              }
            }
          }
        }

        SlcLiveBridgeHit bestHit{};
        const bool keyLooksObjective =
            (!canonicalKey.empty() && IsObjectiveOverlayKeyName(canonicalKey));
        const std::vector<LiveHudElem> liveElems = TextHook_GetLiveHudElems();
        for (const auto &le : liveElems) {
          if (!le.active) {
            continue;
          }
          // Hard lane split: prevent objective lanes from seeding HUD hint
          // params and vice versa while both systems evolve in parallel.
          if (keyLooksObjective) {
            if (!le.objectiveLane) {
              continue;
            }
          } else {
            // Some center Press/Hold hint lanes are geometrically close to the
            // objective update lane (y~-68, fs~2.0) and can be dual-tagged.
            // Allow non-objective keys to consume only when the lane is
            // explicitly marked hint-bridge capable.
            const bool exactKeyMatch =
                (!canonicalKey.empty() && !le.key.empty() &&
                 le.key == canonicalKey);
            if (le.objectiveLane && !le.hintBridgeLane && !exactKeyMatch) {
              continue;
            }
          }
          if (!std::isfinite(le.x) || !std::isfinite(le.y) ||
              !std::isfinite(le.fontScale) || !std::isfinite(le.alpha)) {
            continue;
          }

          bool candidate = false;
          uint32_t cfgIdx = 0;
          if (!cfgCandidates.empty()) {
            if (cfgCandidates.find(le.slcIndex) != cfgCandidates.end()) {
              candidate = true;
              cfgIdx = le.slcIndex;
            }
          }
          if (!candidate && !canonicalKey.empty() && !le.key.empty() &&
              le.key == canonicalKey) {
            candidate = true;
            cfgIdx = le.slcIndex;
          }

          if (!candidate) {
            continue;
          }

          float score = 0.0f;
          float a = le.alpha;
          if (a < 0.0f) a = 0.0f;
          if (a > 1.0f) a = 1.0f;
          score += a * 100.0f;
          if (le.slcIndex == (uint32_t)stringValue) {
            score += 32.0f;
          }
          if (!canonicalKey.empty() && !le.key.empty() && le.key == canonicalKey) {
            score += 18.0f;
          }
          if (le.flags == 0x7) {
            score += 26.0f;
          } else if (le.flags == 0x5) {
            score += 12.0f;
          }
          if (le.fontScale >= 1.4f && le.fontScale <= 2.6f) {
            score += 8.0f;
          }
          if (le.y >= -180.0f && le.y <= 140.0f) {
            score += 6.0f;
          }

          if (!bestHit.valid || score > bestHit.score) {
            bestHit.valid = true;
            bestHit.score = score;
            bestHit.cfgIndex = cfgIdx;
            bestHit.elem = le;
          }
        }

        if (bestHit.valid) {
          const float screenH = GetScreenHeightApprox();
          const float screenW = GetScreenWidthApprox();
          const float scaleX = (screenW > 1.0f) ? (screenW / 1280.0f) : 1.0f;
          const float scaleY = (screenH > 1.0f) ? (screenH / 720.0f) : 1.0f;
          const float scaleF = (screenH > 1.0f) ? (screenH / 480.0f) : 1.0f;
          EngineSafeArea sa = TextHook_GetEngineSafeArea();
          const float safeX =
              (sa.x > 1.0f && sa.x < screenW * 0.5f) ? sa.x : (24.0f * scaleX);
          const float safeY =
              (sa.y > 1.0f && sa.y < screenH * 0.5f) ? sa.y : (24.0f * scaleY);

          float vx = bestHit.elem.x;
          float vy = bestHit.elem.y;
          const bool centerRelativeLane =
              (bestHit.elem.flags == 0x7 && vx >= -96.0f && vx <= 96.0f &&
               vy >= -200.0f && vy <= 120.0f);
          if (centerRelativeLane) {
            // For center-notification lanes the engine stores offsets around
            // (0,0) with center anchoring; convert to 640x480-like basis first.
            vx += 320.0f;
            vy += 240.0f;
          }

          const float px = safeX + (vx * scaleF);
          const float py = safeY + (vy * scaleF);
          if (std::isfinite(px) && std::isfinite(py)) {
            const bool hadRenderParams = slcHasRenderParams;
            const float prevX = slcParamX;
            const float prevY = slcParamY;
            const float prevSX = slcParamSX;
            const float prevSY = slcParamSY;
            slcHasRenderParams = true;
            slcParamX = px;
            slcParamY = py;
            // LiveHudElem fontScale is not the same unit as AddCmd/HUD560 yScale.
            // Normalize aggressively for hint tracks to avoid oversized text.
            float normScale = bestHit.elem.fontScale;
            if (!std::isfinite(normScale)) {
              normScale = 0.65f;
            } else if (normScale >= 1.30f && normScale <= 3.20f) {
              normScale = 0.65f;
            } else if (normScale < 0.20f || normScale > 1.20f) {
              normScale = 0.65f;
            }
            slcParamSX = normScale;
            slcParamSY = slcParamSX;
            slcParamVirtualW = 0;
            slcParamVirtualH = 0;
            slcParamSlotBucket = TextHook_HudSlotFromEvent(slcParamY, 0, 0);
            if (slcParamSlotBucket < 0 || slcParamSlotBucket > 5) {
              slcParamSlotBucket = 0;
            }

            const uint32_t packed = bestHit.elem.color;
            slcParamColor[0] = (float)((packed >> 0) & 0xFF) / 255.0f;
            slcParamColor[1] = (float)((packed >> 8) & 0xFF) / 255.0f;
            slcParamColor[2] = (float)((packed >> 16) & 0xFF) / 255.0f;
            slcParamColor[3] = bestHit.elem.alpha;
            if (!std::isfinite(slcParamColor[3]) || slcParamColor[3] < 0.0f ||
                slcParamColor[3] > 1.0f) {
              slcParamColor[3] = (float)((packed >> 24) & 0xFF) / 255.0f;
            }
            auto clamp01 = [](float v) -> float {
              if (!std::isfinite(v)) return 0.0f;
              if (v < 0.0f) return 0.0f;
              if (v > 1.0f) return 1.0f;
              return v;
            };
            slcParamColor[0] = clamp01(slcParamColor[0]);
            slcParamColor[1] = clamp01(slcParamColor[1]);
            slcParamColor[2] = clamp01(slcParamColor[2]);
            slcParamColor[3] = clamp01(slcParamColor[3]);
            slcHasColorParams = true;

            // Propagate live pulse alpha for HUDHINT rendering.
            // This bypasses the slot/caller filters in the route-free
            // native intake path that may block this alpha from reaching
            // HintOverlayEntry.color[3].
            if (!canonicalKey.empty() && slcParamColor[3] > 0.0f) {
              TextHook_SetSlcLivePulseAlpha(canonicalKey, slcParamColor[3]);
            }

            static std::unordered_map<std::string, DWORD> s_slcLiveBridgeLogTick;
            std::string bridgeSig =
                canonicalKey + "|" + std::to_string((unsigned int)stringValue) +
                "|" + std::to_string((unsigned int)bestHit.cfgIndex);
            if (!canonicalKey.empty() &&
                (TextHook_ShouldTrackMissionPromptHintKey(canonicalKey) ||
                 IsStrictHintCaptureCandidateKey(canonicalKey))) {
              TextHook_RecordRecentHudHintCfgKey(bestHit.cfgIndex, canonicalKey,
                                                 (unsigned int)callerOffset,
                                                 (uint32_t)stringValue,
                                                 "slc_livebridge");
            }
            auto itB = s_slcLiveBridgeLogTick.find(bridgeSig);
            if (itB == s_slcLiveBridgeLogTick.end() ||
                (nowLive - itB->second) > 1200) {
              s_slcLiveBridgeLogTick[bridgeSig] = nowLive;
              if (s_slcLiveBridgeLogTick.size() > 512) {
                for (auto it = s_slcLiveBridgeLogTick.begin();
                     it != s_slcLiveBridgeLogTick.end();) {
                  if ((nowLive - it->second) > 15000) {
                    it = s_slcLiveBridgeLogTick.erase(it);
                  } else {
                    ++it;
                  }
                }
              }
              char lbuf[448];
              sprintf_s(
                  lbuf,
                  "[HUD-SLC-LIVEBRIDGE] key=%s slc=0x%X cfg=0x%X caller=+0x%llX "
                  "x=%.1f y=%.1f sx=%.2f a=%.2f flags=0x%X",
                  canonicalKey.c_str(), (unsigned int)stringValue,
                  (unsigned int)bestHit.cfgIndex,
                  (unsigned long long)callerOffset, slcParamX, slcParamY,
                  slcParamSX, slcParamColor[3], bestHit.elem.flags);
              LogToFile(lbuf);
            }
            if (hadRenderParams &&
                (std::fabs(prevX - slcParamX) > 12.0f ||
                 std::fabs(prevY - slcParamY) > 12.0f ||
                 std::fabs(prevSX - slcParamSX) > 0.20f ||
                 std::fabs(prevSY - slcParamSY) > 0.20f)) {
              static std::unordered_map<std::string, DWORD> s_slcOverrideLogTick;
              std::string sig = canonicalKey + "|" +
                                std::to_string((unsigned int)callerOffset);
              auto itO = s_slcOverrideLogTick.find(sig);
              if (itO == s_slcOverrideLogTick.end() ||
                  (nowLive - itO->second) > 1200) {
                s_slcOverrideLogTick[sig] = nowLive;
                if (s_slcOverrideLogTick.size() > 512) {
                  for (auto it2 = s_slcOverrideLogTick.begin();
                       it2 != s_slcOverrideLogTick.end();) {
                    if ((nowLive - it2->second) > 15000) {
                      it2 = s_slcOverrideLogTick.erase(it2);
                    } else {
                      ++it2;
                    }
                  }
                }
                char obuf[512];
                sprintf_s(
                    obuf,
                    "[HUD-SLC-LIVEBRIDGE-OVERRIDE] key=%s caller=+0x%llX old=(%.1f,%.1f,%.2f,%.2f) new=(%.1f,%.1f,%.2f,%.2f)",
                    canonicalKey.c_str(), (unsigned long long)callerOffset,
                    prevX, prevY, prevSX, prevSY, slcParamX, slcParamY,
                    slcParamSX, slcParamSY);
                LogToFile(obuf);
              }
            }
          }
        }
      }

      // Code byte dump for first unique HUD key caller only
      static std::unordered_set<uintptr_t> s_dumpedCallers;
      static int s_dumpCount = 0;
      if (s_dumpedCallers.find(callerOffset) == s_dumpedCallers.end() &&
          s_dumpCount < 3) {
        s_dumpedCallers.insert(callerOffset);
        s_dumpCount++;

        unsigned char *codeAddr = (unsigned char *)retAddr;
        std::stringstream ss;
        ss << "[SLC CODEDUMP] caller=0x" << std::hex << callerOffset
           << " BEFORE(-64 to -1): ";
        for (int j = -64; j < 0; j++) {
          ss << std::hex << std::setw(2) << std::setfill('0')
             << (int)codeAddr[j] << " ";
        }
        LogToFile(ss.str());

        std::stringstream ss2;
        ss2 << "[SLC CODEDUMP] caller=0x" << std::hex << callerOffset
            << " AFTER(+0 to +47): ";
        for (int j = 0; j < 48; j++) {
          ss2 << std::hex << std::setw(2) << std::setfill('0')
              << (int)codeAddr[j] << " ";
        }
        LogToFile(ss2.str());
      }
    }

    // === SLC LIFECYCLE TRACKING ===
    // SLC fires per-frame for active HUD elements. Refresh lastSeen on
    // existing overlay entries so they stay alive while the engine keeps
    // rendering them, and fade out when the engine stops.
    // Callers: 0x32EE82=Hints, 0x204816=Prompts, 0x34E25E=Objectives
    if (isHudKey) {
      std::string keyStr = canonicalKey.empty() ? std::string(result) : canonicalKey;

      const bool isPlatformKey = (keyStr.rfind("PLATFORM_", 0) == 0);
      const bool isCorneredKey = (keyStr.rfind("CORNERED_", 0) == 0);
      const bool isCorneredObjectiveKey =
          (keyStr.rfind("CORNERED_OBJ_", 0) == 0 ||
           keyStr.rfind("CORNERED_OBJECTIVE_", 0) == 0);
      const bool isIntroKey =
          (keyStr.find("INTROSCREEN") != std::string::npos);
      const bool isCorneredHintKey =
          isCorneredKey && !isCorneredObjectiveKey && !isIntroKey;
      const bool isHintKey =
          (keyStr.rfind("HINT_", 0) == 0 ||
           keyStr.find("_HINT") != std::string::npos ||
           keyStr.rfind("SCRIPT_HINT_", 0) == 0 ||
           keyStr.rfind("SCRIPT_PLATFORM_HINT_", 0) == 0);

      // Mission prompts that don't use *_HINT naming still need runtime hint
      // lifecycle. Detect these by localized English prompt shape instead of
      // hardcoded mission prefixes.
      const bool scriptPrefix = (keyStr.rfind("SCRIPT_", 0) == 0);
      bool missionPromptLike = false;
      if (!scriptPrefix && !IsObjectiveOverlayKeyName(keyStr) &&
          !IsExcludedHudAutoPrefix(keyStr)) {
        std::string locKor;
        std::string locEng;
        if (TextHook_GetLocalizedKeyText(keyStr, locKor, locEng) &&
            !locEng.empty()) {
          missionPromptLike =
              LooksLikePressHoldPrompt(TrimSpaces(StripColorCodes(locEng))) ||
              HasBindingPlaceholderToken(locEng);
        }
      }
      if (missionPromptLike) {
        MaybeLogHudKeyAuto(keyStr, callerOffset, "slc_mission_prompt");
      }

      const bool missionPromptHintKey =
          TextHook_ShouldTrackMissionPromptHintKey(keyStr);
      if (missionPromptHintKey && stringValue != 0) {
        TextHook_RecordRecentHudHintCfgKeyBySlc((uint32_t)stringValue, keyStr,
                                                (unsigned int)callerOffset,
                                                "slc_prompt");
      }

      // --- CG_DrawHudElem coordinate capture ---
      // When SEH fires INSIDE CG_DrawHudElem (any call site), the Detour
      // has stored the authoritative hudelem_s pointer.  Read real coords
      // + alignment from it.  This catches DRILL etc. where blind register
      // probing fails.
      // Skip objective/status keys — they use a separate rendering pipeline.
      if (missionPromptHintKey && !IsObjectiveOverlayKeyName(keyStr)) {
        const uintptr_t cgElemPtr = TextHook_GetCGDrawHudElemPtr();
        uintptr_t capturePtr = cgElemPtr;

        // Fallback: if SEH fired OUTSIDE CG_DrawHudElem (cgElemPtr==0),
        // scan the HudElem array for an element whose text(+0x84) or
        // label(+0x40) cfg matches any known SLC→key mapping for this key.
        // The Detour cached all elemPtrs' coordinates — look them up.
        if (capturePtr == 0) {
          static const uintptr_t s_modBase =
              (uintptr_t)GetModuleHandleA(NULL);
          const uintptr_t arrayBase =
              s_modBase + IW6Offsets::HudElem_Array_SP;
          constexpr int kStride = IW6Offsets::HudElem_Array_Stride_SP;
          constexpr int kMaxElems = 1024; // scan first 1024

          for (int ei = 0; ei < kMaxElems; ++ei) {
            uintptr_t ep = arrayBase + (uintptr_t)ei * kStride;
            if (IsBadReadPtr((void *)ep, kStride)) break;

            uint32_t eType = *(volatile uint32_t *)(ep + 0x00);
            if (eType < 1 || eType > 13) continue;

            // Check text (+0x84) and label (+0x40) fields.
            uint32_t textCfg = *(volatile uint32_t *)(ep + 0x84);
            uint32_t labelCfg = *(volatile uint32_t *)(ep + 0x40);

            // Try to resolve cfg values to key via g_slcToKey.
            std::string probeKey;
            bool matched = false;
            if (textCfg > 0 && textCfg <= 0xFFFFu &&
                TextHook_GetSlcKey(textCfg, probeKey) &&
                probeKey == keyStr) {
              matched = true;
            }
            if (!matched && labelCfg > 0 && labelCfg <= 0xFFFFu &&
                TextHook_GetSlcKey(labelCfg, probeKey) &&
                probeKey == keyStr) {
              matched = true;
            }

            if (matched) {
              // Found the HudElem — use Detour's cached coordinates.
              CgDrawHudElemCapture epCap{};
              if (TextHook_GetCgDrawElemPtrCapture(ep, epCap, 500)) {
                capturePtr = ep;
              }
              break;
            }
          }
        }

        if (capturePtr != 0) {
          float ex, ey, fs;
          uint32_t ao, as, cp;
          if (capturePtr == cgElemPtr) {
            // Direct read from CG_DrawHudElem context.
            ex = *(volatile float *)(capturePtr + 0x04);
            ey = *(volatile float *)(capturePtr + 0x08);
            fs = *(volatile float *)(capturePtr + 0x14);
            ao = *(volatile uint32_t *)(capturePtr + 0x28);
            as = *(volatile uint32_t *)(capturePtr + 0x2C);
            cp = *(volatile uint32_t *)(capturePtr + 0x30);
          } else {
            // From elemPtr cache (Detour snapshot).
            CgDrawHudElemCapture cached{};
            TextHook_GetCgDrawElemPtrCapture(capturePtr, cached, 500);
            ex = cached.x;
            ey = cached.y;
            fs = cached.fontScale;
            ao = cached.alignOrg;
            as = cached.alignScreen;
            cp = cached.colorPacked;
          }

          if (std::isfinite(ex) && std::isfinite(ey) &&
              std::isfinite(fs) && fs > 0.01f && fs < 6.0f &&
              ex > -2000.0f && ex < 2000.0f &&
              ey > -2000.0f && ey < 2000.0f) {
            CgDrawHudElemCapture cap{};
            cap.x = ex;
            cap.y = ey;
            cap.fontScale = fs;
            cap.alignOrg = ao;
            cap.alignScreen = as;
            cap.colorPacked = cp;
            cap.tick = GetTickCount();
            TextHook_StoreCgDrawCapture(keyStr, cap);

            static std::unordered_map<std::string, DWORD> s_cgCapLog;
            DWORD _now = GetTickCount();
            auto itCL = s_cgCapLog.find(keyStr);
            if (itCL == s_cgCapLog.end() || (_now - itCL->second) > 2000) {
              s_cgCapLog[keyStr] = _now;
              char _clb[384];
              sprintf_s(_clb,
                        "[CG-DRAW-CAPTURE] key=%s caller=+0x%llX "
                        "x=%.2f y=%.2f fs=%.3f ao=%u as=0x%02X "
                        "color=0x%08X elem=0x%llX%s",
                        keyStr.c_str(),
                        (unsigned long long)callerOffset,
                        ex, ey, fs, ao, as, cp,
                        (unsigned long long)capturePtr,
                        (capturePtr != cgElemPtr) ? " [SCAN]" : "");
              LogToFile(_clb);
            }
          }
        }
      }

      const bool missionPromptReverseCaller =
          (callerOffset == IW6Offsets::Profile::Rva_232562 || callerOffset == IW6Offsets::Profile::Rva_3680B2 ||
           callerOffset == IW6Offsets::Profile::Rva_389BB5 || callerOffset == IW6Offsets::Profile::Rva_4156EC ||
           callerOffset == IW6Offsets::Profile::Rva_2B1453);
      if (missionPromptHintKey && !slcHasRenderParams &&
          (missionPromptLike || missionPromptReverseCaller)) {
        static std::mutex s_mpProbeMtx;
        static std::unordered_map<std::string, DWORD> s_mpProbeTick;
        const DWORD nowMpProbe = GetTickCount();
        const std::string probeSig =
            keyStr + "|" + std::to_string((unsigned int)callerOffset);
        bool shouldProbe = false;
        {
          std::lock_guard<std::mutex> lk(s_mpProbeMtx);
          auto itProbe = s_mpProbeTick.find(probeSig);
          if (itProbe == s_mpProbeTick.end() ||
              (nowMpProbe - itProbe->second) > 220) {
            s_mpProbeTick[probeSig] = nowMpProbe;
            shouldProbe = true;
          }
          if (s_mpProbeTick.size() > 512) {
            for (auto it = s_mpProbeTick.begin(); it != s_mpProbeTick.end();) {
              if ((nowMpProbe - it->second) > 15000) {
                it = s_mpProbeTick.erase(it);
              } else {
                ++it;
              }
            }
          }
        }

        if (shouldProbe) {
          bool mpBestValid = false;
          float mpBestScore = -1.0e30f;
          std::string mpBestSource;
          float mpBestX = 0.0f;
          float mpBestY = 0.0f;
          float mpBestSX = 1.0f;
          float mpBestSY = 1.0f;
          unsigned short mpBestVW = 0;
          unsigned short mpBestVH = 0;
          bool mpBestHasColor = false;
          float mpBestColor[4] = {1.0f, 1.0f, 1.0f, 0.0f};
          auto TryAcceptMissionPromptParams = [&](float px, float py, float sx,
                                                  float sy,
                                                  const char *srcTag,
                                                  float scoreBias = 0.0f,
                                                  const float *colorSample = nullptr,
                                                  bool hasColorSample = false) {
            const bool finite = (std::isfinite(px) && std::isfinite(py) &&
                                 std::isfinite(sx) && std::isfinite(sy));
            const bool plausiblePos =
                (px > -300.0f && px < 5000.0f && py > -300.0f && py < 3000.0f);
            const bool scaleAsFactor =
                (sx > 0.01f && sx < 6.0f && sy > 0.01f && sy < 6.0f);
            const bool scaleAsPixels =
                (sx >= 8.0f && sx <= 256.0f && sy >= 8.0f && sy <= 256.0f);
            if (!finite || !plausiblePos || (!scaleAsFactor && !scaleAsPixels)) {
              return;
            }

            float workX = px;
            float workY = py;
            float outSX = sx;
            float outSY = sy;
            unsigned short outVW = 0;
            unsigned short outVH = 0;
            const bool centerRelativeProbe =
                scaleAsFactor && (px >= -160.0f && px <= 160.0f) &&
                (py >= -260.0f && py <= 180.0f) && (sx >= 0.15f && sx <= 6.0f) &&
                (sy >= 0.15f && sy <= 6.0f);
            if (scaleAsPixels) {
              outSX = sx / 35.0f;
              outSY = sy / 35.0f;
              if (outSX < 0.15f)
                outSX = 0.15f;
              if (outSX > 4.5f)
                outSX = 4.5f;
              if (outSY < 0.15f)
                outSY = 0.15f;
              if (outSY > 4.5f)
                outSY = 4.5f;
            } else if (centerRelativeProbe) {
              workX = px + 320.0f;
              workY = py + 240.0f;
              outVW = 640;
              outVH = 480;
            } else if (px >= -8.0f && px <= 960.0f && py >= -8.0f &&
                       py <= 720.0f) {
              outVW = 640;
              outVH = 480;
            }

            float score = 20.0f + scoreBias;
            if (workX >= 40.0f && workX <= 1880.0f && workY >= 40.0f &&
                workY <= 1060.0f) {
              score += 28.0f;
            }
            if (workY >= 120.0f && workY <= 900.0f) {
              score += 12.0f;
            }
            if (outSX >= 0.2f && outSX <= 3.0f && outSY >= 0.2f &&
                outSY <= 3.0f) {
              score += 10.0f;
            }
            if (scaleAsPixels) {
              score += 10.0f;
            }
            if (centerRelativeProbe) {
              score += 34.0f;
            }
            if (workX >= 180.0f && workX <= 460.0f && workY >= 110.0f &&
                workY <= 340.0f) {
              score += 22.0f;
            }
            if (std::fabs(workX) < 12.0f && std::fabs(workY) < 12.0f) {
              score -= 120.0f;
            }

            if (!mpBestValid || score > mpBestScore) {
              mpBestValid = true;
              mpBestScore = score;
              mpBestSource = (srcTag ? srcTag : "unknown");
              mpBestX = workX;
              mpBestY = workY;
              mpBestSX = outSX;
              mpBestSY = outSY;
              mpBestVW = outVW;
              mpBestVH = outVH;
              mpBestHasColor = hasColorSample;
              if (hasColorSample && colorSample) {
                memcpy(mpBestColor, colorSample, sizeof(mpBestColor));
              } else {
                mpBestColor[0] = 1.0f;
                mpBestColor[1] = 1.0f;
                mpBestColor[2] = 1.0f;
                mpBestColor[3] = 0.0f;
              }
            }
          };

          auto TryMissionPromptProbeStruct = [&](uintptr_t addr,
                                                 const char *srcTagBase) {
            if (!IsBadReadPtr((void *)addr, 0xA8)) {
              uint32_t t = 0;
              float px = 0.0f, py = 0.0f, fs = 0.0f;
              uint32_t packed = 0;
              if (SubRev_ReadU32((uintptr_t)(addr + 0x00), &t) &&
                  SubRev_ReadF32((uintptr_t)(addr + 0x04), &px) &&
                  SubRev_ReadF32((uintptr_t)(addr + 0x08), &py) &&
                  SubRev_ReadF32((uintptr_t)(addr + 0x14), &fs) &&
                  SubRev_ReadU32((uintptr_t)(addr + 0x30), &packed)) {
                if (t >= 1u && t <= 13u && std::isfinite(fs) &&
                    fs >= 0.20f && fs <= 3.20f) {
                  // Read alignment flags from HudElem struct to properly
                  // normalize raw 480-space coordinates to 640x480 absolute.
                  // Without this, elements with non-center alignment (e.g.
                  // CLOCKWORK_HINT_DRILL) get wrong screen positions.
                  uint32_t alignOrg = 0, alignScreen = 0;
                  SubRev_ReadU32((uintptr_t)(addr + 0x28), &alignOrg);
                  SubRev_ReadU32((uintptr_t)(addr + 0x2C), &alignScreen);

                  // IW6 alignScreen encoding:
                  //   horiz = (alignScreen >> 4) & 0xF
                  //     0=SUBLEFT,1=LEFT,2=CENTER,3=RIGHT,7=CENTER_SA
                  //   vert  = alignScreen & 0xF
                  //     0=SUBTOP,1=TOP,2=CENTER,3=BOTTOM,7=CENTER_SA
                  const uint32_t horzAlign = (alignScreen >> 4) & 0xF;
                  const uint32_t vertAlign = alignScreen & 0xF;

                  // Convert raw x/y to 640x480 absolute coordinates using
                  // the screen anchor from alignScreen.
                  float normX = px;
                  float normY = py;
                  switch (horzAlign) {
                    case 2: case 7: // CENTER, CENTER_SAFEAREA
                      normX = 320.0f + px;
                      break;
                    case 3: // RIGHT
                      normX = 640.0f + px;
                      break;
                    default: // LEFT, SUBLEFT, etc.
                      break;
                  }
                  switch (vertAlign) {
                    case 2: case 7: // CENTER, CENTER_SAFEAREA
                      normY = 240.0f + py;
                      break;
                    case 3: // BOTTOM
                      normY = 480.0f + py;
                      break;
                    default: // TOP, SUBTOP, etc.
                      break;
                  }

                  float exactColor[4] = {
                      (float)((packed >> 0) & 0xFFu) / 255.0f,
                      (float)((packed >> 8) & 0xFFu) / 255.0f,
                      (float)((packed >> 16) & 0xFFu) / 255.0f,
                      (float)((packed >> 24) & 0xFFu) / 255.0f,
                  };
                  char srcTag[64];
                  sprintf_s(srcTag, "%s:native14a", srcTagBase);
                  // Exact HUDHINT struct evidence with alignment normalization:
                  // +0x04/+0x08 raw coords, +0x28/+0x2C alignment, +0x14 fontScale.
                  // Coordinates are now in 640x480 absolute space.
                  TryAcceptMissionPromptParams(normX, normY, fs, fs, srcTag, 64.0f,
                                               exactColor, true);

                  // Also log alignment for diagnostics.
                  {
                    static std::unordered_map<std::string, DWORD> s_native14aLogTick;
                    const DWORD nowN14 = GetTickCount();
                    auto itN14 = s_native14aLogTick.find(keyStr);
                    if (itN14 == s_native14aLogTick.end() ||
                        (nowN14 - itN14->second) > 1500) {
                      s_native14aLogTick[keyStr] = nowN14;
                      char n14buf[384];
                      sprintf_s(n14buf,
                                "[NATIVE14-ALIGN] key=%s raw=(%.1f,%.1f) "
                                "alignOrg=%u alignScreen=0x%02X "
                                "horzAlign=%u vertAlign=%u "
                                "norm=(%.1f,%.1f) fs=%.3f src=%s",
                                keyStr.c_str(), px, py,
                                alignOrg, alignScreen,
                                horzAlign, vertAlign,
                                normX, normY, fs, srcTagBase);
                      LogToFile(n14buf);
                    }
                  }
                }
              }
            }
            struct ProbeLayout {
              int xOff;
              int yOff;
              int sxOff;
              int syOff;
              const char *tag;
            };
            static const ProbeLayout kLayouts[] = {
                {0x00, 0x04, 0x08, 0x0C, "seq0"},
                {0x04, 0x08, 0x0C, 0x10, "seq4"},
                {0x04, 0x08, 0x10, 0x14, "hudA"},
                {0x04, 0x08, 0x14, 0x18, "hudB"},
                {0x04, 0x08, 0x1C, 0x24, "hudC"},
                {0x04, 0x08, 0x20, 0x24, "hudD"},
                {0x04, 0x08, 0x24, 0x2C, "hudE"},
                {0x04, 0x08, 0x2C, 0x30, "hudF"},
            };
            for (const auto &layout : kLayouts) {
              const int maxOff =
                  max(max(layout.xOff, layout.yOff),
                      max(layout.sxOff, layout.syOff));
              if (IsBadReadPtr((void *)addr, (UINT_PTR)(maxOff + 4))) {
                continue;
              }
              float px = *(volatile float *)(addr + layout.xOff);
              float py = *(volatile float *)(addr + layout.yOff);
              float sx = *(volatile float *)(addr + layout.sxOff);
              float sy = *(volatile float *)(addr + layout.syOff);
              char srcTag[64];
              sprintf_s(srcTag, "%s:%s", srcTagBase, layout.tag);
              TryAcceptMissionPromptParams(px, py, sx, sy, srcTag);
            }
          };

          auto TryMissionPromptProbeBase = [&](uintptr_t base,
                                               const char *baseName) {
            if (!(base > 0x10000 && base < 0x7FFFFFFFFFFF)) {
              return;
            }
            for (int off = -0x140; off <= 0x240; off += 4) {
              uintptr_t addr = base + (intptr_t)off;
              if (!(addr > 0x10000 && addr < 0x7FFFFFFFFFFF)) {
                continue;
              }
              char srcTag[48];
              sprintf_s(srcTag, "%s%+d", baseName, off);
              TryMissionPromptProbeStruct(addr, srcTag);
            }
          };

          const struct ProbeReg {
            const char *name;
            uintptr_t value;
          } probeRegs[] = {
              {"rsp", g_SLC_CallerRegs.rsp}, {"rbp", g_SLC_CallerRegs.rbp},
              {"rdi", g_SLC_CallerRegs.rdi}, {"rsi", g_SLC_CallerRegs.rsi},
              {"rbx", g_SLC_CallerRegs.rbx}, {"r12", g_SLC_CallerRegs.r12},
              {"r13", g_SLC_CallerRegs.r13}, {"r14", g_SLC_CallerRegs.r14},
              {"r15", g_SLC_CallerRegs.r15}, {"rcx", g_SLC_CallerRegs.rcx},
              {"rdx", g_SLC_CallerRegs.rdx}, {"r8", g_SLC_CallerRegs.r8},
              {"r9", g_SLC_CallerRegs.r9},   {"r10", g_SLC_CallerRegs.r10},
              {"r11", g_SLC_CallerRegs.r11},
          };
          for (const auto &pr : probeRegs) {
            TryMissionPromptProbeBase(pr.value, pr.name);
          }

          // Keep the reverse path strict enough to reject stack noise from the
          // mission-prompt caller family (e.g. 43/43, sy=4.5 false positives)
          // while still accepting centered prompt/scanner layouts.
          if (mpBestValid && mpBestScore >= 60.0f) {
            slcHasRenderParams = true;
            slcParamX = mpBestX;
            slcParamY = mpBestY;
            slcParamSX = mpBestSX;
            slcParamSY = mpBestSY;
            slcParamVirtualW = mpBestVW;
            slcParamVirtualH = mpBestVH;
            slcHasColorParams = mpBestHasColor;
            if (mpBestHasColor) {
              memcpy(slcParamColor, mpBestColor, sizeof(slcParamColor));
            }
            slcParamSlotBucket =
                TextHook_HudSlotFromEvent(slcParamY, slcParamVirtualH, 0);
            if (slcParamSlotBucket < 0 || slcParamSlotBucket > 5) {
              slcParamSlotBucket = 0;
            }
            const bool trackMissionPromptNative =
                TextHook_ShouldTrackMissionPromptHintKey(keyStr) &&
                !ShouldForceCenterAlignGameplayTranslation(keyStr);
            if (trackMissionPromptNative &&
                IsTargetHudHintWriterFocusKey(keyStr)) {
              TextHook_ArmHudHintWriterFocus(keyStr,
                                             (unsigned int)callerOffset);
            }
            if (trackMissionPromptNative) {
              TextHook_RecordHudHintRuntimeParams(
                  keyStr, (unsigned int)callerOffset, slcParamX, slcParamY,
                  slcParamSX, slcParamSY, slcParamVirtualW, slcParamVirtualH,
                  slcParamSlotBucket, slcHasColorParams ? slcParamColor : nullptr,
                  slcHasColorParams);
            }

            // Route-free native HUDHINT intake:
            // When reverse probe has already recovered authoritative render
            // params for the confirmed +0x5DC950 caller family, promote that
            // sample directly into the native HUD map here. Do not wait for a
            // later raw SLC event branch; many crouch/slide prompts only ever
            // become param-bearing at this probe stage.
            if (trackMissionPromptNative &&
                IsCenterHudHintAuthorityCaller((unsigned int)callerOffset) &&
                slcParamSlotBucket > 0 && slcParamSlotBucket < 4) {
              float nativeFontScale = 1.0f;
              if (std::isfinite(slcParamSX) && slcParamSX >= 0.20f &&
                  slcParamSX <= 3.20f) {
                nativeFontScale = slcParamSX;
              } else if (std::isfinite(slcParamSY) && slcParamSY >= 0.20f &&
                         slcParamSY <= 3.20f) {
                nativeFontScale = slcParamSY;
              }
              HudNativeEntry mpNative{};
              mpNative.key = keyStr;
              mpNative.x = slcParamX;
              mpNative.y = slcParamY;
              mpNative.xScale = nativeFontScale;
              mpNative.yScale = nativeFontScale;
              mpNative.fontHeight = 35.0f;
              mpNative.color[0] = slcHasColorParams ? slcParamColor[0] : 1.0f;
              mpNative.color[1] = slcHasColorParams ? slcParamColor[1] : 1.0f;
              mpNative.color[2] = slcHasColorParams ? slcParamColor[2] : 1.0f;
              mpNative.color[3] = slcHasColorParams ? slcParamColor[3] : 0.0f;
              mpNative.style = 0;
              mpNative.lastUpdate = nowMpProbe;
              mpNative.source = HUD_NATIVE_RENDER;
              mpNative.sourceToken = NATIVE_HUD_SOURCE_BRIDGE;
              mpNative.virtualW = slcParamVirtualW;
              mpNative.virtualH = slcParamVirtualH;
              mpNative.callerOffset = (unsigned int)callerOffset;
              mpNative.objectiveChannel = (unsigned char)OBJ_CHANNEL_NONE;
              mpNative.consumerId = TextHook_MakeHudConsumerId(
                  OVERLAY_SOURCE_SLC, (unsigned int)callerOffset,
                  slcParamSlotBucket, slcParamVirtualW, slcParamVirtualH);
              mpNative.slotBucket = slcParamSlotBucket;
              mpNative.objectiveLike = false;
              mpNative.valid = true;
              TextHook_UpdateHudNativeEntry(mpNative);
              if (!ApplyNativeHudEntryDirect(keyStr, mpNative, nullptr)) {
                RefreshHintEntryNative(keyStr, mpNative, nullptr);
              }

              static std::unordered_map<std::string, DWORD> s_mpNativeProbeLog;
              auto itProbeLog = s_mpNativeProbeLog.find(keyStr);
              if (itProbeLog == s_mpNativeProbeLog.end() ||
                  (nowMpProbe - itProbeLog->second) > 1200) {
                s_mpNativeProbeLog[keyStr] = nowMpProbe;
                char pbuf[448];
                sprintf_s(
                    pbuf,
                    "[HUD-MP-NATIVE-PROBE] key=%s caller=+0x%llX slot=%d x=%.2f y=%.2f sx=%.3f sy=%.3f a=%.2f",
                    keyStr.c_str(), (unsigned long long)callerOffset,
                    slcParamSlotBucket, slcParamX, slcParamY, slcParamSX,
                    slcParamSY, mpNative.color[3]);
                LogToFile(pbuf);
              }
            }
          }

          static std::mutex s_mpProbeLogMtx;
          static std::unordered_map<std::string, DWORD> s_mpProbeLogTick;
          bool shouldLog = false;
          {
            std::lock_guard<std::mutex> lk(s_mpProbeLogMtx);
            auto itLog = s_mpProbeLogTick.find(probeSig);
            if (itLog == s_mpProbeLogTick.end() ||
                (nowMpProbe - itLog->second) > 3000) {
              s_mpProbeLogTick[probeSig] = nowMpProbe;
              shouldLog = true;
            }
            if (s_mpProbeLogTick.size() > 512) {
              for (auto it = s_mpProbeLogTick.begin();
                   it != s_mpProbeLogTick.end();) {
                if ((nowMpProbe - it->second) > 30000) {
                  it = s_mpProbeLogTick.erase(it);
                } else {
                  ++it;
                }
              }
            }
          }


          if (shouldLog) {
            char mpBuf[512];
            if (slcHasRenderParams) {
              sprintf_s(
                  mpBuf,
                  "[HUD-MP-PARAM] key=%s caller=+0x%llX src=%s x=%.2f y=%.2f sx=%.4f sy=%.4f slot=%d",
                  keyStr.c_str(), (unsigned long long)callerOffset,
                  mpBestSource.c_str(), slcParamX, slcParamY, slcParamSX,
                  slcParamSY, slcParamSlotBucket);
            } else if (mpBestValid) {
              sprintf_s(
                  mpBuf,
                  "[HUD-MP-REV-CAND] key=%s caller=+0x%llX src=%s x=%.2f y=%.2f sx=%.4f sy=%.4f q=%.1f",
                  keyStr.c_str(), (unsigned long long)callerOffset,
                  mpBestSource.c_str(), mpBestX, mpBestY, mpBestSX, mpBestSY,
                  mpBestScore);
            } else {
              sprintf_s(mpBuf,
                        "[HUD-MP-REV-NOPARAM] key=%s caller=+0x%llX slc=0x%X",
                        keyStr.c_str(), (unsigned long long)callerOffset,
                        (unsigned int)stringValue);
            }
            LogToFile(mpBuf);
          }
        }
      }

      // Runtime hint lifecycle is driven by SP gameplay HUD callsites.
      // Seed with known offsets, and learn additional callers only when the key
      // is CORNERED_* (gameplay-only, not used in menus).
      static std::mutex s_slcHintCallerMtx;
      static std::unordered_set<uintptr_t> s_slcHintCallers = {
          IW6Offsets::Profile::Rva_3720F2, // Hints
          IW6Offsets::Profile::Rva_2454FC, // Prompts
          IW6Offsets::Profile::Rva_369002, // Platform prompts
          IW6Offsets::Profile::Rva_389BB5, // Mission prompt path variant
          IW6Offsets::Profile::Rva_3680B2  // Mission prompt path variant
      };
      bool isHintCaller = false;
      const bool seededHintCaller =
          hasCallerRouteSeed && (callerSeedChannel == HUD_ROUTE_HINT);
      {
        std::lock_guard<std::mutex> lk(s_slcHintCallerMtx);
        if (isCorneredHintKey) {
          s_slcHintCallers.insert(callerOffset);
        }
        if (seededHintCaller) {
          s_slcHintCallers.insert(callerOffset);
        }
        isHintCaller =
            (s_slcHintCallers.find(callerOffset) != s_slcHintCallers.end());
      }
      // SLC key loads can happen ahead of on-screen rendering for some script
      // locals. Keep runtime hint lifecycle strict here to avoid early/ghost
      // fallback overlays from non-hint script strings.
      const bool isRuntimeHintKey =
          (isPlatformKey || isCorneredHintKey || isHintKey ||
           missionPromptLike);
      const bool blockByPause = TextHook_IsPauseMenuLikely();
      const bool isObjectiveRuntimeKey = IsObjectiveOverlayKeyName(keyStr);
      const bool isStructuredObjectivePayload =
          (!keyStr.empty() && keyStr[0] == '\\');
      const bool isObjectiveStatusRuntimeKey =
          IsObjectiveStatusMessageKeyName(keyStr);
      const bool isObjectiveRuntimeKeyStrict =
          isObjectiveRuntimeKey && LooksLikeHudLocalizationKey(keyStr) &&
          !isStructuredObjectivePayload;

      // NEW: Unified objective/status capture from SLC hook.
      // RDI points to hudelem_s for objective callers — capture everything
      // in one call, no multi-path, no OSAUTH.
      if ((isObjectiveRuntimeKeyStrict || isObjectiveStatusRuntimeKey) &&
          !blockByPause && !isStructuredObjectivePayload) {
        ObjUnified_CaptureFromSLC(
            g_SLC_CallerRegs.rdi,
            keyStr,
            (uint32_t)stringValue,  // cfgIndex (SLC value)
            (uint32_t)stringValue,  // slcIndex
            (unsigned int)callerOffset);
      }

      // Status messages are rendered from objective lanes, but the lane key can
      // lag behind real status traffic. Capture fresh status probes from this
      // SLC callsite and let TextHook_ReadLiveHudElems() select the lane key
      // with objective-lane edge semantics.
      if (isObjectiveStatusRuntimeKey && !blockByPause) {
        TextHook_RecordObjectiveStatusProbe(
            keyStr, (unsigned int)callerOffset, (uint32_t)stringValue,
            slcHasRenderParams);
        const bool osauthOnly = RuntimeFlags_ObjectiveStatusEnableOsRevOnly();
        const bool enableSlcStatusProducer =
            RuntimeFlags_ObjectiveStatusEnableSlcStatusProducer();
        const bool consumeCapStatusSource =
            RuntimeFlags_ObjectiveStatusEnableConsumeCap();
        const bool isSaveStatusRuntimeKey =
            (keyStr == "EXE_GAMESAVED" || keyStr == "CGAME_NOW_SAVING");
        const bool allowSlcProducerPush =
            !osauthOnly &&
            (enableSlcStatusProducer ||
             (consumeCapStatusSource && isSaveStatusRuntimeKey));
        const OverlaySessionState sessionStateNow = g_OverlaySessionState;
        const bool gameplaySession =
            (sessionStateNow == OVERLAY_SESSION_GAMEPLAY ||
             sessionStateNow == OVERLAY_SESSION_PAUSE ||
             sessionStateNow == OVERLAY_SESSION_WARMUP);
        const unsigned int callerOffset32 = (unsigned int)callerOffset;
        const bool confirmedStatusConsumerCaller =
            TextHook_IsConfirmedStatusConsumerCaller(callerOffset32);
        const bool primaryStatusProducerCaller =
            TextHook_IsPrimaryStatusProducerCaller(callerOffset32);
        if (gameplaySession && slcHasRenderParams &&
            primaryStatusProducerCaller && allowSlcProducerPush) {
          const bool requiresTrustedCfg =
              (keyStr == "GAME_OBJECTIVESUPDATED" ||
               keyStr == "GAME_OBJECTIVECOMPLETED");
          const uint32_t trustedCfg =
              TextHook_GetTrustedObjectiveStatusCfgForKey(keyStr, 180000u);
          if (requiresTrustedCfg && trustedCfg == 0) {
            static std::unordered_map<std::string, DWORD>
                s_statusProducerCfgSkipLogTick;
            const DWORD nowStatus = GetTickCount();
            const std::string logSig =
                keyStr + "|" + std::to_string(callerOffset32);
            auto itLog = s_statusProducerCfgSkipLogTick.find(logSig);
            if (itLog == s_statusProducerCfgSkipLogTick.end() ||
                (nowStatus - itLog->second) > 1000) {
              s_statusProducerCfgSkipLogTick[logSig] = nowStatus;
              char sbuf[320];
              sprintf_s(
                  sbuf,
                  "[STATUS-SLC-PRODUCER-SKIP-CFG] key=%s caller=+0x%llX slc=0x%X lane=%d",
                  keyStr.c_str(), (unsigned long long)callerOffset,
                  (unsigned int)stringValue, slcParamSlotBucket);
              LogToFile(sbuf);
            }
          } else {
            const DWORD nowStatus = GetTickCount();
            if (trustedCfg != 0) {
              TextHook_RecordObjectiveStatusCfgTrust(
                  keyStr, trustedCfg, nowStatus, "slc_status_producer_cfg");
            }
            uint32_t laneSig = 0;
            if (slcParamSlotBucket >= -128 && slcParamSlotBucket <= 255) {
              laneSig = (uint32_t)((slcParamSlotBucket + 128) & 0xFF);
            }
            uint64_t instanceSig =
                ((uint64_t)(laneSig & 0xFFu) << 24) ^
                (uint64_t)((uint32_t)stringValue & 0x00FFFFFFu);
            if (instanceSig == 0) {
              instanceSig = ((uint64_t)(callerOffset32 & 0xFFFFu) << 16) ^
                            (uint64_t)((uint32_t)stringValue & 0xFFFFu);
            }
            if (osauthOnly) {
              TextHook_OSAuth_RecordStatusProducerCapture(
                  keyStr, trustedCfg, (uint32_t)stringValue, slcParamSlotBucket,
                  callerOffset32, instanceSig, 0, 0, 0);
            } else {
              TextHook_RecordObjectiveStatusEvent(keyStr, callerOffset32,
                                                  "slc_status_producer",
                                                  instanceSig);
            }
            static std::unordered_map<std::string, DWORD>
                s_statusSlcProducerLogTick;
            const std::string logSig =
                keyStr + "|" + std::to_string(callerOffset32) + "|" +
                std::to_string((unsigned int)(instanceSig & 0xFFFFFFFFu));
            auto itLog = s_statusSlcProducerLogTick.find(logSig);
            if (itLog == s_statusSlcProducerLogTick.end() ||
                (nowStatus - itLog->second) > 1000) {
              s_statusSlcProducerLogTick[logSig] = nowStatus;
              char sbuf[384];
              sprintf_s(
                  sbuf,
                  "[STATUS-SLC-PRODUCER] key=%s caller=+0x%llX cfg=0x%X slc=0x%X isig=0x%llX lane=%d mode=%s",
                  keyStr.c_str(), (unsigned long long)callerOffset, trustedCfg,
                  (unsigned int)stringValue, (unsigned long long)instanceSig,
                  slcParamSlotBucket, osauthOnly ? "osauth" : "event");
              LogToFile(sbuf);
            }
          }
        } else if (gameplaySession && slcHasRenderParams &&
                   !primaryStatusProducerCaller &&
                   allowSlcProducerPush) {
          const bool requiresTrustedCfg =
              (keyStr == "GAME_OBJECTIVESUPDATED" ||
               keyStr == "GAME_OBJECTIVECOMPLETED" ||
               keyStr == "GAME_OBJECTIVEFAILED");
          const uint32_t trustedCfg =
              TextHook_GetTrustedObjectiveStatusCfgForKey(keyStr, 180000u);
          if (requiresTrustedCfg && trustedCfg != 0) {
            const DWORD nowStatus = GetTickCount();
            TextHook_RecordObjectiveStatusCfgTrust(
                keyStr, trustedCfg, nowStatus, "slc_status_producer_trusted_rp");
            uint32_t laneSig = 0;
            if (slcParamSlotBucket >= -128 && slcParamSlotBucket <= 255) {
              laneSig = (uint32_t)((slcParamSlotBucket + 128) & 0xFF);
            }
            uint64_t instanceSig =
                ((uint64_t)(laneSig & 0xFFu) << 24) ^
                (uint64_t)((uint32_t)stringValue & 0x00FFFFFFu);
            if (instanceSig == 0) {
              instanceSig = ((uint64_t)(callerOffset32 & 0xFFFFu) << 16) ^
                            (uint64_t)((uint32_t)stringValue & 0xFFFFu);
            }
            if (osauthOnly) {
              TextHook_OSAuth_RecordStatusProducerCapture(
                  keyStr, trustedCfg, (uint32_t)stringValue, slcParamSlotBucket,
                  callerOffset32, instanceSig, 0, 0, 0);
            } else {
              TextHook_RecordObjectiveStatusEvent(
                  keyStr, trustedCfg, "slc_status_producer_trusted_rp",
                  instanceSig);
            }
            static std::unordered_map<std::string, DWORD>
                s_statusTrustedRpProducerLogTick;
            const std::string logSig =
                keyStr + "|" + std::to_string(callerOffset32) + "|" +
                std::to_string((unsigned int)(instanceSig & 0xFFFFFFFFu));
            auto itLog = s_statusTrustedRpProducerLogTick.find(logSig);
            if (itLog == s_statusTrustedRpProducerLogTick.end() ||
                (nowStatus - itLog->second) > 1000) {
              s_statusTrustedRpProducerLogTick[logSig] = nowStatus;
              char sbuf[384];
              sprintf_s(
                  sbuf,
                  "[STATUS-SLC-PRODUCER] key=%s caller=+0x%llX cfg=0x%X slc=0x%X isig=0x%llX lane=%d mode=%s",
                  keyStr.c_str(), (unsigned long long)callerOffset, trustedCfg,
                  (unsigned int)stringValue, (unsigned long long)instanceSig,
                  slcParamSlotBucket,
                  osauthOnly ? "osauth_trusted_rp" : "event_trusted_rp");
              LogToFile(sbuf);
            }
          } else {
          static std::unordered_map<std::string, DWORD>
              s_statusProducerCallerSkipLogTick;
          const DWORD nowStatus = GetTickCount();
          const std::string logSig = keyStr + "|" +
                                     std::to_string(callerOffset32) + "|" +
                                     (confirmedStatusConsumerCaller
                                          ? "non_primary"
                                          : "unconfirmed");
          auto itLog = s_statusProducerCallerSkipLogTick.find(logSig);
          if (itLog == s_statusProducerCallerSkipLogTick.end() ||
              (nowStatus - itLog->second) > 1000) {
            s_statusProducerCallerSkipLogTick[logSig] = nowStatus;
            char sbuf[320];
            sprintf_s(
                sbuf,
                "[STATUS-SLC-PRODUCER-SKIP-CALLER] key=%s caller=+0x%llX slc=0x%X lane=%d reason=%s",
                keyStr.c_str(), (unsigned long long)callerOffset,
                (unsigned int)stringValue, slcParamSlotBucket,
                confirmedStatusConsumerCaller ? "non_primary"
                                              : "unconfirmed");
            LogToFile(sbuf);
          }
          }
        } else if (!gameplaySession) {
          static std::unordered_map<std::string, DWORD> s_statusSessionSkipLogTick;
          const DWORD nowStatus = GetTickCount();
          const std::string logSig =
              keyStr + "|" + std::to_string((unsigned int)callerOffset);
          auto itLog = s_statusSessionSkipLogTick.find(logSig);
          if (itLog == s_statusSessionSkipLogTick.end() ||
              (nowStatus - itLog->second) > 1000) {
            s_statusSessionSkipLogTick[logSig] = nowStatus;
            char sbuf[256];
            sprintf_s(
                sbuf,
                "[STATUS-SLC-SKIP-SESSION] key=%s caller=+0x%llX state=%d",
                keyStr.c_str(), (unsigned long long)callerOffset,
                (int)sessionStateNow);
            LogToFile(sbuf);
          }
        }
      }

      // Native-only HUD hint policy:
      // SLC must not drive hint/object-hint lifecycle or routing.
      static bool s_loggedSlcHudHintDisabled = false;
      if (!s_loggedSlcHudHintDisabled) {
        s_loggedSlcHudHintDisabled = true;
        LogToFile("[HUD-SLC-DISABLED] hint/object-hint SLC path disabled");
      }
      const bool slcHudHintPathEnabled = false;

      // Route-free native HUDHINT refresh:
      // Keep generic SLC hint lifecycle disabled, but still allow confirmed
      // HUDHINT callsites to refresh native entries from recovered render
      // params. This is the path that keeps prompts like "Press C to crouch"
      // alive without reviving the old SLC overlay routing system.
      const bool directMissionPromptKey =
          TextHook_ShouldTrackMissionPromptHintKey(keyStr) &&
          !ShouldForceCenterAlignGameplayTranslation(keyStr);
      if (directMissionPromptKey && !blockByPause) {
        const DWORD nowHintNative = GetTickCount();
        if (!slcHasRenderParams) {
          HudHintRuntimeParamSnapshot recentParam{};
          if (TextHook_GetRecentHudHintRuntimeParams(keyStr, 0, 1800,
                                                     recentParam)) {
            slcHasRenderParams = true;
            slcParamX = recentParam.x;
            slcParamY = recentParam.y;
            slcParamSX = recentParam.sx;
            slcParamSY = recentParam.sy;
            slcParamVirtualW = recentParam.virtualW;
            slcParamVirtualH = recentParam.virtualH;
            slcParamSlotBucket = recentParam.slotBucket;
            slcHasColorParams = recentParam.hasColor;
            if (recentParam.hasColor) {
              memcpy(slcParamColor, recentParam.color, sizeof(slcParamColor));
            }

            static std::unordered_map<std::string, DWORD>
                s_directMpRecentLogTick;
            auto itRecent = s_directMpRecentLogTick.find(keyStr);
            if (itRecent == s_directMpRecentLogTick.end() ||
                (nowHintNative - itRecent->second) > 1200) {
              s_directMpRecentLogTick[keyStr] = nowHintNative;
              char rbuf[384];
              sprintf_s(
                  rbuf,
                  "[HUD-MP-RECENT] key=%s caller=+0x%llX from=+0x%X slot=%d x=%.2f y=%.2f sx=%.3f sy=%.3f a=%.2f",
                  keyStr.c_str(), (unsigned long long)callerOffset,
                  recentParam.callerHint, recentParam.slotBucket,
                  recentParam.x, recentParam.y, recentParam.sx,
                  recentParam.sy,
                  recentParam.hasColor ? recentParam.color[3] : 0.0f);
              LogToFile(rbuf);
            }
          }
        }

        const bool authoritativeMissionPrompt =
            directMissionPromptKey && slcHasRenderParams &&
            IsCenterHudHintAuthorityCaller((unsigned int)callerOffset) &&
            slcParamSlotBucket > 0 && slcParamSlotBucket < 4;
        if (authoritativeMissionPrompt) {
          float nativeFontScale = 1.0f;
          if (std::isfinite(slcParamSX) && slcParamSX >= 0.20f &&
              slcParamSX <= 3.20f) {
            nativeFontScale = slcParamSX;
          } else if (std::isfinite(slcParamSY) && slcParamSY >= 0.20f &&
                     slcParamSY <= 3.20f) {
            nativeFontScale = slcParamSY;
          }

          std::string eventEnglish;
          {
            std::string runtimeEnglish;
            if (TextHook_GetRuntimeEnglishForKey(keyStr, runtimeEnglish)) {
              runtimeEnglish = TrimSpaces(StripColorCodes(runtimeEnglish));
              if (!runtimeEnglish.empty() &&
                  runtimeEnglish != keyStr &&
                  !LooksLikeHudLocalizationKey(runtimeEnglish)) {
                eventEnglish = runtimeEnglish;
              }
            }
          }
          if (eventEnglish.empty()) {
            auto itEng = g_KeyToEnglish.find(keyStr);
            if (itEng != g_KeyToEnglish.end()) {
              eventEnglish = TrimSpaces(StripColorCodes(itEng->second));
            }
          }

          HudNativeEntry directNative{};
          directNative.key = keyStr;
          directNative.x = slcParamX;
          directNative.y = slcParamY;
          directNative.xScale = nativeFontScale;
          directNative.yScale = nativeFontScale;
          directNative.fontHeight = 35.0f;
          directNative.color[0] = slcHasColorParams ? slcParamColor[0] : 1.0f;
          directNative.color[1] = slcHasColorParams ? slcParamColor[1] : 1.0f;
          directNative.color[2] = slcHasColorParams ? slcParamColor[2] : 1.0f;
          directNative.color[3] = slcHasColorParams ? slcParamColor[3] : 0.0f;
          directNative.style = 0;
          directNative.lastUpdate = nowHintNative;
          directNative.source = HUD_NATIVE_RENDER;
          directNative.sourceToken = NATIVE_HUD_SOURCE_BRIDGE;
          directNative.virtualW = slcParamVirtualW;
          directNative.virtualH = slcParamVirtualH;
          directNative.callerOffset = (unsigned int)callerOffset;
          directNative.objectiveChannel = (unsigned char)OBJ_CHANNEL_NONE;
          directNative.consumerId = TextHook_MakeHudConsumerId(
              OVERLAY_SOURCE_SLC, (unsigned int)callerOffset,
              slcParamSlotBucket, slcParamVirtualW, slcParamVirtualH);
          directNative.slotBucket = slcParamSlotBucket;
          directNative.objectiveLike = false;
          directNative.valid = true;

          const std::string *eventEnglishPtr =
              eventEnglish.empty() ? nullptr : &eventEnglish;
          TextHook_UpdateHudNativeEntry(directNative);
          if (!ApplyNativeHudEntryDirect(keyStr, directNative,
                                         eventEnglishPtr)) {
            RefreshHintEntryNative(keyStr, directNative, eventEnglishPtr);
          }

          static std::unordered_map<std::string, DWORD> s_directMpNativeLogTick;
          auto itNative = s_directMpNativeLogTick.find(keyStr);
          if (itNative == s_directMpNativeLogTick.end() ||
              (nowHintNative - itNative->second) > 1200) {
            s_directMpNativeLogTick[keyStr] = nowHintNative;
            char nbuf[448];
            sprintf_s(
                nbuf,
                "[HUD-MP-NATIVE] key=%s caller=+0x%llX slot=%d x=%.2f y=%.2f sx=%.3f sy=%.3f a=%.2f",
                keyStr.c_str(), (unsigned long long)callerOffset,
                slcParamSlotBucket, slcParamX, slcParamY, slcParamSX,
                slcParamSY,
                slcHasColorParams ? slcParamColor[3] : 0.0f);
            LogToFile(nbuf);
          }
        }
      }

      // Queue runtime-managed hints only from trusted HUD callsites and
      // non-pause contexts.
      const bool shouldQueueHintBySlc =
          slcHudHintPathEnabled && isHintCaller && isRuntimeHintKey &&
          !blockByPause;
      bool queuedHintBySlc = false;
      if (shouldQueueHintBySlc) {
        queuedHintBySlc = TextHook_OnHudKey(keyStr.c_str());
        if (!queuedHintBySlc) {
          // Log missing HUD keys once to help translators.
          static std::mutex s_missMtx;
          static std::unordered_set<std::string> s_loggedHudMiss;
          bool shouldLog = false;
          {
            std::lock_guard<std::mutex> lk(s_missMtx);
            if (s_loggedHudMiss.size() < 600 &&
                s_loggedHudMiss.insert(keyStr).second) {
              shouldLog = true;
            }
          }

          if (shouldLog) {
            std::string eng;
            auto it = g_KeyToEnglish.find(keyStr);
            if (it != g_KeyToEnglish.end()) {
              eng = it->second;
            }
            if (eng.size() > 120)
              eng.resize(120);
            char buf[512];
            sprintf_s(buf, "[HUD-MISS] key=%s caller=+0x%llX eng=\"%s\"",
                      keyStr.c_str(), (unsigned long long)callerOffset,
                      eng.c_str());
            LogToFile(buf);
          }
        }
      }

      // Refresh lifecycle for HUD keys so they stay visible while the engine
      // continues rendering them.
      const bool shouldRefreshHintBySlc =
          slcHudHintPathEnabled && isHintCaller && isRuntimeHintKey &&
          !blockByPause;
      if (shouldRefreshHintBySlc) {
        bool hasLocalizedHintKey = false;
        {
          auto itKor = g_KeyToKorean.find(keyStr);
          hasLocalizedHintKey =
              (itKor != g_KeyToKorean.end() && !itKor->second.empty());
        }
        if (queuedHintBySlc || hasLocalizedHintKey) {
          const DWORD nowHint = GetTickCount();
          std::string eventEnglish;
          {
            std::string runtimeEnglish;
            if (TextHook_GetRuntimeEnglishForKey(keyStr, runtimeEnglish)) {
              runtimeEnglish = TrimSpaces(StripColorCodes(runtimeEnglish));
              if (!runtimeEnglish.empty() &&
                  runtimeEnglish != keyStr &&
                  !LooksLikeHudLocalizationKey(runtimeEnglish)) {
                eventEnglish = runtimeEnglish;
              }
            }
          }
          if (eventEnglish.empty()) {
            auto itEng = g_KeyToEnglish.find(keyStr);
            if (itEng != g_KeyToEnglish.end()) {
              eventEnglish = TrimSpaces(StripColorCodes(itEng->second));
            }
          }

          const int slcSlotBucket =
              (slcHasRenderParams && slcParamSlotBucket != 0)
                  ? slcParamSlotBucket
                  : 0;
          {
            // Stabilize SLC hint slot authority per key+caller:
            // If no-param traffic consistently says this prompt is in one slot
            // (typically cursor/bottom), reject transient param-probe samples
            // from a different slot (common false-positive: center slot 3).
            static std::mutex s_slcHintSlotMtx;
            static std::unordered_map<std::string, std::pair<int, DWORD>>
                s_slcNoParamSlotByKeyCaller;
            const std::string slotSig =
                keyStr + "|" + std::to_string((unsigned int)callerOffset);
            std::lock_guard<std::mutex> lkSlot(s_slcHintSlotMtx);
            auto itSlot = s_slcNoParamSlotByKeyCaller.find(slotSig);
            if (!slcHasRenderParams) {
              s_slcNoParamSlotByKeyCaller[slotSig] = {slcSlotBucket, nowHint};
            } else if (itSlot != s_slcNoParamSlotByKeyCaller.end()) {
              const int lockedSlot = itSlot->second.first;
              const DWORD lockedAge = nowHint - itSlot->second.second;
              if (lockedSlot > 0 && lockedAge <= 3000 &&
                  lockedSlot != slcSlotBucket) {
                char rbuf[384];
                sprintf_s(
                    rbuf,
                    "[HUD-SLC-PARAM-REJECT] key=%s caller=+0x%llX lockedSlot=%d probeSlot=%d x=%.2f y=%.2f sx=%.4f sy=%.4f",
                    keyStr.c_str(), (unsigned long long)callerOffset,
                    lockedSlot, slcSlotBucket, slcParamX, slcParamY,
                    slcParamSX, slcParamSY);
                LogToFile(rbuf);
                slcHasRenderParams = false;
              }
            }
            if (s_slcNoParamSlotByKeyCaller.size() > 512) {
              for (auto it = s_slcNoParamSlotByKeyCaller.begin();
                   it != s_slcNoParamSlotByKeyCaller.end();) {
                if ((nowHint - it->second.second) > 15000) {
                  it = s_slcNoParamSlotByKeyCaller.erase(it);
                } else {
                  ++it;
                }
              }
            }
          }
          const float slcEventX =
              slcHasRenderParams ? slcParamX
                                 : std::numeric_limits<float>::quiet_NaN();
          const float slcEventY =
              slcHasRenderParams ? slcParamY
                                 : std::numeric_limits<float>::quiet_NaN();
          const float slcEventSX =
              (slcHasRenderParams && slcParamSX > 0.01f && slcParamSX < 6.0f)
                  ? slcParamSX
                  : 1.0f;
          const float slcEventSY =
              (slcHasRenderParams && slcParamSY > 0.01f && slcParamSY < 6.0f)
                  ? slcParamSY
                  : 1.0f;
          const unsigned short slcEventVW =
              slcHasRenderParams ? slcParamVirtualW : 0;
          const unsigned short slcEventVH =
              slcHasRenderParams ? slcParamVirtualH : 0;
          const float slcEventColorR =
              slcHasColorParams ? slcParamColor[0] : 1.0f;
          const float slcEventColorG =
              slcHasColorParams ? slcParamColor[1] : 1.0f;
          const float slcEventColorB =
              slcHasColorParams ? slcParamColor[2] : 1.0f;
          const float slcEventAlpha =
              slcHasColorParams ? slcParamColor[3] : 0.0f;
          const bool centerMissionPromptKey =
              TextHook_ShouldTrackMissionPromptHintKey(keyStr) &&
              !ShouldForceCenterAlignGameplayTranslation(keyStr);
          if (centerMissionPromptKey && !slcHasRenderParams) {
            HudHintRuntimeParamSnapshot recentParam{};
            if (TextHook_GetRecentHudHintRuntimeParams(keyStr, 0, 1400,
                                                       recentParam)) {
              slcHasRenderParams = true;
              slcParamX = recentParam.x;
              slcParamY = recentParam.y;
              slcParamSX = recentParam.sx;
              slcParamSY = recentParam.sy;
              slcParamVirtualW = recentParam.virtualW;
              slcParamVirtualH = recentParam.virtualH;
              slcParamSlotBucket = recentParam.slotBucket;
              slcHasColorParams = recentParam.hasColor;
              if (recentParam.hasColor) {
                memcpy(slcParamColor, recentParam.color, sizeof(slcParamColor));
              }

              static std::unordered_map<std::string, DWORD> s_mpRecentLogTick;
              auto itRecent = s_mpRecentLogTick.find(keyStr);
              if (itRecent == s_mpRecentLogTick.end() ||
                  (nowHint - itRecent->second) > 1200) {
                s_mpRecentLogTick[keyStr] = nowHint;
                char rbuf[384];
                sprintf_s(
                    rbuf,
                    "[HUD-MP-RECENT] key=%s caller=+0x%llX from=+0x%X slot=%d x=%.2f y=%.2f sx=%.3f sy=%.3f a=%.2f",
                    keyStr.c_str(), (unsigned long long)callerOffset,
                    recentParam.callerHint, recentParam.slotBucket,
                    recentParam.x, recentParam.y, recentParam.sx,
                    recentParam.sy,
                    recentParam.hasColor ? recentParam.color[3] : 0.0f);
                LogToFile(rbuf);
              }
            }
          }
          const bool authoritativeCenterMissionPrompt =
              centerMissionPromptKey && slcHasRenderParams &&
              IsCenterHudHintAuthorityCaller((unsigned int)callerOffset) &&
              slcSlotBucket > 0 && slcSlotBucket < 4;
          float nativeFontScale = 1.0f;
          if (std::isfinite(slcEventSX) && slcEventSX >= 0.20f &&
              slcEventSX <= 3.20f) {
            nativeFontScale = slcEventSX;
          } else if (std::isfinite(slcEventSY) && slcEventSY >= 0.20f &&
                     slcEventSY <= 3.20f) {
            nativeFontScale = slcEventSY;
          }

          HudNativeEntry slcNative{};
          slcNative.key = keyStr;
          slcNative.x = slcEventX;
          slcNative.y = slcEventY;
          slcNative.xScale = nativeFontScale;
          slcNative.yScale = nativeFontScale;
          slcNative.fontHeight = 35.0f;
          slcNative.color[0] = slcEventColorR;
          slcNative.color[1] = slcEventColorG;
          slcNative.color[2] = slcEventColorB;
          slcNative.color[3] = slcEventAlpha;
          slcNative.style = 0;
          slcNative.lastUpdate = nowHint;
          slcNative.source =
              authoritativeCenterMissionPrompt ? HUD_NATIVE_RENDER
                                               : HUD_NATIVE_SCANNER;
          slcNative.sourceToken =
              authoritativeCenterMissionPrompt ? NATIVE_HUD_SOURCE_BRIDGE : 0;
          slcNative.virtualW = slcEventVW;
          slcNative.virtualH = slcEventVH;
          slcNative.callerOffset = (unsigned int)callerOffset;
          slcNative.objectiveChannel = (unsigned char)OBJ_CHANNEL_NONE;
          uint64_t slcConsumerId = TextHook_MakeHudConsumerId(
              OVERLAY_SOURCE_SLC, (unsigned int)callerOffset, slcSlotBucket,
              slcEventVW, slcEventVH);
          if (!slcHasRenderParams) {
            // No-param SLC traffic can mix multiple prompt families on the same
            // (caller,slot). Split consumer identity by key to prevent
            // cross-key conflict quarantine from poisoning hint routing.
            uint64_t keyHash = 1469598103934665603ull;
            for (unsigned char uc : keyStr) {
              unsigned char up = uc;
              if (up >= 'a' && up <= 'z') {
                up = (unsigned char)(up - ('a' - 'A'));
              }
              keyHash ^= (uint64_t)up;
              keyHash *= 1099511628211ull;
            }
            slcConsumerId ^= keyHash;
          }
          slcNative.consumerId = slcConsumerId;
          slcNative.slotBucket = slcSlotBucket;
          slcNative.objectiveLike = false;
          slcNative.valid = true;

          const std::string nativeEnglish =
              eventEnglish.empty() ? keyStr : eventEnglish;
          if (authoritativeCenterMissionPrompt) {
            TextHook_UpdateHudNativeEntry(slcNative);
            if (!ApplyNativeHudEntryDirect(keyStr, slcNative,
                                           &nativeEnglish)) {
              RefreshHintEntryNative(keyStr, slcNative, &nativeEnglish);
            }
            static std::unordered_map<std::string, DWORD> s_centerPromptLogTick;
            auto itLog = s_centerPromptLogTick.find(keyStr);
            if (itLog == s_centerPromptLogTick.end() ||
                (nowHint - itLog->second) > 1200) {
              s_centerPromptLogTick[keyStr] = nowHint;
              char nbuf[448];
              sprintf_s(
                  nbuf,
                  "[HUD-MP-NATIVE] key=%s caller=+0x%llX slot=%d x=%.2f y=%.2f sx=%.3f sy=%.3f a=%.2f",
                  keyStr.c_str(), (unsigned long long)callerOffset,
                  slcSlotBucket, slcEventX, slcEventY, slcEventSX, slcEventSY,
                  slcEventAlpha);
              LogToFile(nbuf);
            }
          }
        }
      }

      // Objective producer authority is fixed to the dedicated caller.
      // Do not learn/promote alternate callers from runtime probes.
      const bool hasObjectiveListRenderParams =
          (slcHasRenderParams && std::isfinite(slcParamX) &&
           std::isfinite(slcParamY) && std::isfinite(slcParamSX) &&
           std::isfinite(slcParamSY) && slcParamX >= 35.0f &&
           slcParamX <= 170.0f && slcParamY >= 40.0f &&
           slcParamY <= 175.0f && slcParamSX >= 0.30f &&
           slcParamSX <= 1.20f && slcParamSY >= 0.30f &&
           slcParamSY <= 1.20f &&
           (fabsf(slcParamY - 58.5f) <= 20.0f ||
            fabsf(slcParamY - 103.5f) <= 20.0f ||
            fabsf(slcParamY - 148.5f) <= 20.0f));
      const bool isObjectiveCaller =
          ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset);
      const bool nativeLearnCapture = false;
      std::string structuredObjectiveKey;
      const bool hasStructuredObjectiveKey =
          isStructuredObjectivePayload &&
          ObjRev_TryExtractStructuredObjectiveKey(keyStr, structuredObjectiveKey);
      const std::string &objectiveQueueKey =
          hasStructuredObjectiveKey ? structuredObjectiveKey : keyStr;
      const bool isObjectiveStatusKey =
          IsObjectiveStatusMessageKeyName(objectiveQueueKey);
      const bool isObjectiveBootstrapKey =
          IsObjectiveListBootstrapKeyName(objectiveQueueKey);
      const bool isObjectiveLaneAuthorityCandidate =
          LooksLikeHudLocalizationKey(objectiveQueueKey) &&
          hasObjectiveListRenderParams &&
          TextHook_OSAuth_IsObjectiveCaptureCandidateKey(objectiveQueueKey);
      const bool objectiveNativeEligibleKey =
          ObjRev_LooksLikeLocalizationKey(objectiveQueueKey.c_str()) &&
          (isObjectiveBootstrapKey || isObjectiveLaneAuthorityCandidate);
      const bool isObjectiveBootstrapObservationCaller =
          (ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset) ||
           ObjRev_IsObjectiveConsumerCandidateCaller(callerOffset));
      const bool shouldRecordBootstrapObjectiveObservation =
          !isIntroKeyName && !isObjectiveStatusKey &&
          TextHook_OSAuth_IsObjectiveCaptureCandidateKey(objectiveQueueKey) &&
          isObjectiveBootstrapObservationCaller &&
          (hasStructuredObjectiveKey || isObjectiveBootstrapKey ||
           hasObjectiveListRenderParams);

      if (shouldRecordBootstrapObjectiveObservation) {
        const DWORD nowObjObs = GetTickCount();
        uint32_t obsOrdinal = 0;
        if (hasObjectiveListRenderParams && std::isfinite(slcParamY)) {
          const int lane =
              (int)lroundf((slcParamY - 58.5f) / 45.0f);
          if (lane >= 0) {
            obsOrdinal = (uint32_t)(lane + 1);
          }
        }
        ObjectiveRuntime::WithOverlayState(
            [&](ObjectiveRuntime::OverlayState &overlayState) {
              auto &g_ObjectiveKeyObservations =
                  overlayState.keyObservations;
              ObjectiveKeyObservation &obs =
                  g_ObjectiveKeyObservations[objectiveQueueKey];
              obs.key = objectiveQueueKey;
              obs.channel = OBJ_CHANNEL_GAMEPLAY_LIST;
              obs.lastSeen = nowObjObs;
              if (obsOrdinal > 0) {
                obs.ordinal = obsOrdinal;
              }
              obs.callerOffset = (unsigned int)callerOffset;
            });
        static std::unordered_map<std::string, DWORD>
            s_bootstrapObjectiveObsLogTick;
        const std::string logSig =
            objectiveQueueKey + "|" + std::to_string(obsOrdinal) + "|" +
            std::to_string((unsigned int)callerOffset);
        auto itLog = s_bootstrapObjectiveObsLogTick.find(logSig);
        if (itLog == s_bootstrapObjectiveObsLogTick.end() ||
            (nowObjObs - itLog->second) > 1200) {
          s_bootstrapObjectiveObsLogTick[logSig] = nowObjObs;
          char obsBuf[320];
          sprintf_s(
              obsBuf,
              "[OBJ-BOOT-OBS] key=%s ord=%u caller=+0x%llX rp=%u",
              objectiveQueueKey.c_str(), obsOrdinal,
              (unsigned long long)callerOffset,
              hasObjectiveListRenderParams ? 1u : 0u);
          LogToFile(obsBuf);
        }
      }

      const bool shouldQueueObjectiveBySlc =
          !isIntroKeyName && isObjectiveCaller && !isObjectiveStatusKey &&
          objectiveNativeEligibleKey;
      if (shouldQueueObjectiveBySlc) {
#if GHOSTSKOR_OBJECTIVE_REVERSE
        // Record SLC observation for targeted bias discovery.
        // ONLY from the dedicated objective caller +0x34E25E.
        // Structured payloads like "\state\1\str\KEY" are unwrapped to their
        // inner key first, but other callers still never contribute because
        // their stringValue space is noisy and caused status/objective drift.
        if (ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset) &&
            stringValue > 0 &&
            isObjectiveLaneAuthorityCandidate) {
          std::lock_guard<std::mutex> lk(g_objSlcObsMutex);
          uint32_t obsOrdinal = 0;
          if (std::isfinite(slcParamY)) {
            const int lane =
                (int)lroundf((slcParamY - 58.5f) / 45.0f);
            if (lane >= 0) {
              obsOrdinal = (uint32_t)(lane + 1);
            }
          }
          const DWORD nowObs = GetTickCount();
          ObjSlcObservation *matched = nullptr;
          for (auto &obs : g_objSlcObservations) {
            if (obs.slcIdx == (uint32_t)stringValue) {
              matched = &obs;
              break;
            }
          }
          bool shouldLogObs = false;
          if (matched) {
            const bool changed =
                (matched->key != objectiveQueueKey ||
                 matched->ordinal != obsOrdinal);
            matched->key = objectiveQueueKey;
            matched->ordinal = obsOrdinal;
            matched->tick = nowObs;
            shouldLogObs = changed;
          } else {
            g_objSlcObservations.push_back(
                {(uint32_t)stringValue, objectiveQueueKey, obsOrdinal, nowObs});
            if (g_objSlcObservations.size() > 64)
              g_objSlcObservations.erase(g_objSlcObservations.begin());
            shouldLogObs = true;
          }
          if (shouldLogObs) {
            char obsBuf[256];
            sprintf_s(obsBuf,
                      "[OBJ-SLC-OBS] slcIdx=0x%X key=%s ord=%u caller=+0x%llX",
                      (unsigned int)stringValue, objectiveQueueKey.c_str(),
                      obsOrdinal,
                      (unsigned long long)callerOffset);
            LogToFile(obsBuf);
          }
        }

        // Legacy stack probe (slc==textField) is intentionally disabled.
        // Objective text fields carry cfg index; authoritative cfg capture now
        // lives in TryCaptureObjectiveNativeFromSlc() via dedicated caller
        // stack/elem bridge.
#endif // GHOSTSKOR_OBJECTIVE_REVERSE
        ObjectiveChannel objChannel =
            DetermineObjectiveChannelForKey(objectiveQueueKey, blockByPause);
        if (objChannel == OBJ_CHANNEL_NONE) {
          // Wrapped objective payloads are emitted in the real list lanes even
          // before the key becomes "strict". Only the dedicated objective
          // caller is allowed to seed gameplay-list evidence.
          if (isObjectiveCaller) {
            objChannel = OBJ_CHANNEL_GAMEPLAY_LIST;
          }
        }
        if (!RuntimeFlags_ObjectiveStatusEnableOsRevOnly() &&
            objChannel != OBJ_CHANNEL_NONE) {
          const bool shouldCaptureObjectiveNative =
              isObjectiveCaller &&
              (isObjectiveBootstrapKey || isObjectiveLaneAuthorityCandidate);
          const bool nativeCaptured =
              nativeLearnCapture ||
              (shouldCaptureObjectiveNative &&
               TryCaptureObjectiveNativeFromSlc(objectiveQueueKey, callerOffset,
                                                stringValue));
          bool queued = TextHook_OnHudKey(objectiveQueueKey.c_str());
          if (!queued) {
            // Non-pattern objective key: bypass key-pattern gate and register
            // objective lifecycle directly.
            RefreshObjectiveOverlayEntry(objectiveQueueKey, objChannel);
            EnsureTranslationsLoaded();
            auto itKor = g_KeyToKorean.find(objectiveQueueKey);
            queued = (itKor != g_KeyToKorean.end() && !itKor->second.empty());
          }
          {
            const DWORD nowObj = GetTickCount();
            uint32_t obsOrdinal = 0;
            if (std::isfinite(slcParamY)) {
              const int lane =
                  (int)lroundf((slcParamY - 58.5f) / 45.0f);
              obsOrdinal = (uint32_t)((std::max)(0, (std::min)(2, lane)) + 1);
            }
            ObjectiveRuntime::WithOverlayState(
                [&](ObjectiveRuntime::OverlayState &overlayState) {
                  auto &g_ObjectiveOverlay = overlayState.entries;
                  auto &g_ObjectiveKeyObservations =
                      overlayState.keyObservations;
                  ObjectiveKeyObservation &obs =
                      g_ObjectiveKeyObservations[objectiveQueueKey];
                  obs.key = objectiveQueueKey;
                  obs.channel = objChannel;
                  obs.lastSeen = nowObj;
                  if (obsOrdinal > 0) {
                    obs.ordinal = obsOrdinal;
                  }
                  obs.callerOffset = (unsigned int)callerOffset;
                  for (auto &e : g_ObjectiveOverlay) {
                    if (e.key != objectiveQueueKey) {
                      continue;
                    }
                    if (objChannel != OBJ_CHANNEL_NONE &&
                        e.channel != objChannel) {
                      continue;
                    }
                    e.lastFSHookSeen = nowObj;
                  }
                });
          }
          if (!queued && objChannel != OBJ_CHANNEL_NONE) {
            static std::mutex s_objMissMtx;
            static std::unordered_set<std::string> s_loggedObjMiss;
            bool shouldLog = false;
            {
              std::lock_guard<std::mutex> lk(s_objMissMtx);
              if (s_loggedObjMiss.size() < 400 &&
                  s_loggedObjMiss.insert(objectiveQueueKey).second) {
                shouldLog = true;
              }
            }
  
          if (shouldLog) {
              std::string eng;
              auto it = g_KeyToEnglish.find(objectiveQueueKey);
              if (it != g_KeyToEnglish.end()) {
                eng = it->second;
              }
              if (eng.size() > 120)
                eng.resize(120);
              char buf[512];
              sprintf_s(buf, "[OBJ-MISS] key=%s caller=+0x%llX eng=\"%s\"",
                        objectiveQueueKey.c_str(), (unsigned long long)callerOffset,
                        eng.c_str());
              LogToFile(buf);
            }
          }

          if (nativeCaptured) {
            static std::unordered_map<std::string, DWORD> s_objNativeLog;
            DWORD nowCap = GetTickCount();
            auto itCap = s_objNativeLog.find(objectiveQueueKey);
            if (itCap == s_objNativeLog.end() || (nowCap - itCap->second) > 2500) {
              s_objNativeLog[objectiveQueueKey] = nowCap;
              if (s_objNativeLog.size() > 96) {
                for (auto it2 = s_objNativeLog.begin(); it2 != s_objNativeLog.end();) {
                  if ((nowCap - it2->second) > 60000) it2 = s_objNativeLog.erase(it2);
                  else ++it2;
                }
              }
              char nbuf[256];
              sprintf_s(nbuf, "[OBJ-NATIVE-SLC] key=%s caller=+0x%llX",
                        objectiveQueueKey.c_str(),
                        (unsigned long long)callerOffset);
              LogToFile(nbuf);
            }
          }
        }
      }
    }
  }

  return result;
}

// =============================================================================
// R_AddCmdDrawTextWithCursor Detour - Text input fields, possibly subtitles
// =============================================================================
void __fastcall
Detour_R_AddCmdDrawTextWithCursor(const char *text, int maxChars, Font_s *font,
                                  float x, float y, float xScale, float yScale,
                                  float rotation, const float *color, int style,
                                  int cursorPos, char cursorChar) {

  // Log to see if subtitles come through here
  static int cursorLogCount = 0;
  if (cursorLogCount < 100 && text && strlen(text) > 3) {
    cursorLogCount++;
    char logBuf[300];
    sprintf_s(logBuf, "[CURSOR TEXT] text=\"%.60s\" x=%.1f y=%.1f cursor=%d",
              text, x, y, cursorPos);
    LogToFile(logBuf);

    // Check for subtitle-like text (color codes)
    if (strstr(text, "^2") || strstr(text, "^7") || strstr(text, "^1")) {
      LogToFile(
          "[CURSOR SUBTITLE!] Found subtitle-like text in Cursor function!");
    }
  }

  // Call original
  if (Original_R_AddCmdDrawTextWithCursor) {
    Original_R_AddCmdDrawTextWithCursor(text, maxChars, font, x, y, xScale,
                                        yScale, rotation, color, style,
                                        cursorPos, cursorChar);
  }
}

// =============================================================================
// R_AddCmdDrawText Detour - Main text rendering hook
// =============================================================================
void __fastcall Detour_R_AddCmdDrawText(const char *text, int maxChars,

                                        Font_s *font, float x, float y,

                                        float xScale, float yScale,

                                        float rotation, float *color,

                                        int style) {

  if (!text)
    return;

  // ── Gamepad glyph atlas capture (runs once, no-op afterwards) ────────────
  // When the game renders text containing control chars \x01–\x17 (button
  // glyphs), capture the font material SRV + glyph UVs for inline rendering.
  if (font && !GamepadGlyphAtlas::IsReady()) {
    for (const char *p = text; *p; ++p) {
      unsigned char c = (unsigned char)*p;
      if (c >= 1 && c <= 23) {
        GamepadGlyphAtlas::TryCapture(font);
        break;
      }
    }
  }

  // --- &&1 variable capture from engine-rendered text ---
  // --- Multi-line body text English suppression zone ---
  // When we translate a multi-line body text (e.g. Rorke Files), the engine
  // calls R_AddCmdDrawText per visual line. We match the first line and queue
  // the full Korean overlay; subsequent English lines must be suppressed.
  static float s_BodySuppX = -9999.0f;
  static float s_BodySuppBaseY = -9999.0f;
  static float s_BodySuppMaxY = 99999.0f;
  static float s_BodySuppMaxDx = 50.0f;
  static DWORD s_BodySuppTime = 0;
  static DWORD s_BodySuppDurationMs = 50;
  bool routedGameplayHudPrompt = false;
  bool routedTailJoinKey = false;

  if (s_BodySuppTime != 0) {
    DWORD nowBS = GetTickCount();
    float dx = x - s_BodySuppX;
    if (dx < 0) dx = -dx;
    if ((nowBS - s_BodySuppTime) < s_BodySuppDurationMs && text[0] != '\0' &&
        dx < s_BodySuppMaxDx && y > s_BodySuppBaseY && y < s_BodySuppMaxY) {
      // Don't suppress text that has its own direct translation
      // (e.g. difficulty label "Veteran" below a split description).
      EnsureTranslationsLoaded();
      std::string suppUpper = ToUpper(TrimSpaces(StripColorCodes(std::string(text))));
      bool hasOwnTranslation =
          (g_EnglishToKey.find(suppUpper) != g_EnglishToKey.end());
      if (!hasOwnTranslation) {
        if (Original_R_AddCmdDrawText) {
          Original_R_AddCmdDrawText(" ", maxChars, font, x, y, xScale, yScale,
                                    rotation, color, style);
        }
        return;
      }
      // Text has its own translation — let it through
      s_BodySuppTime = 0;
    }
    if ((nowBS - s_BodySuppTime) >= s_BodySuppDurationMs) {
      s_BodySuppTime = 0;
    }
  }

  // Shared state
  static DWORD s_rkBodySuppTick_shared = 0;
  // Animated header cache (set at QueueText, used by body piggyback)
  static bool s_rkHdrCacheValid = false;
  static std::string s_rkHdrCachedText;
  static float s_rkHdrX = 0, s_rkHdrY = 0, s_rkHdrScaleAdj = 0;
  static float s_rkHdrFontH = 0, s_rkHdrEngW = 0, s_rkHdrOrigScale = 0;
  static float s_rkHdrColor[4] = {1,1,1,1};
  static int s_rkHdrStyle = 0;
  static bool s_rkHdrSmall = false, s_rkHdrShadow = false, s_rkHdrSkipClip = false;
  static int s_rkHdrAlign = 0;
  static float s_rkHdrMaxW = 0;
  static uint8_t s_rkHdrAtlas = 0;
  static DWORD s_rkHdrCacheTick = 0;
  static float s_rkHdrFixedX = 0;

  // ==========================================================================
  // Rorke header drift fix: freeze x for animated headers (pages 1,6,19,20,21)
  // ==========================================================================
  // When on a Rorke detail page (detected by recent body prefix match),
  // header text (y < 16%) gets its x latched on first appearance.
  // This prevents the left-drift animation that overshoots the yellow marker.
  // Rorke header: no hook-level suppression. All text goes through normal path.
  // Render() handles animated header removal by text content matching.
  {
  }

  // ==========================================================================
  // Rorke body text: O(1) prefix map — QueueText + suppress (21 entries)
  // ==========================================================================
  {
    static DWORD s_rkBodySuppTick = 0;
    static float s_rkBodySuppX = 0.0f;
    static float s_rkBodySuppBaseY = 0.0f;

    // Subsequent-line suppression (armed by first-line match below)
    if (s_rkBodySuppTick != 0) {
      const DWORD elapsed = GetTickCount() - s_rkBodySuppTick;
      if (elapsed < 500 && x >= (s_rkBodySuppX - 5.0f) &&
          x <= (s_rkBodySuppX + 5.0f) && y > s_rkBodySuppBaseY) {
        if (Original_R_AddCmdDrawText)
          Original_R_AddCmdDrawText(" ", 1, font, x, y, xScale, yScale,
                                    rotation, color, style);
        return;
      }
      if (elapsed >= 500) {
        s_rkBodySuppTick = 0;
        s_rkHdrCacheValid = false; // clear header cache when leaving page
      }
    }

    // First-line: O(1) prefix map lookup
    if (text && strlen(text) >= 5) {
      const auto &prefixMap = TranslationStore::RorkeBodyPrefix20();
      if (!prefixMap.empty()) {
        // Quick probe WITHOUT string allocation: use raw text, strip ^X only if present
        const char *probeBase = text;
        std::string cleanBuf; // only allocated if color codes found
        if (strchr(text, '^')) {
          cleanBuf = StripColorCodes(std::string(text));
          probeBase = cleanBuf.c_str();
        }
        size_t probeBaseLen = strlen(probeBase);
        if (probeBaseLen >= 5) {
        // Try prefix lengths: 20, 15, 10, 7, 5 (longest match first)
        char probeBuf[21]; // max 20 chars + null
        auto rkIt = prefixMap.end();
        for (size_t plen : {(size_t)20, (size_t)15, (size_t)10, (size_t)7, (size_t)5}) {
          if (probeBaseLen >= plen) {
            memcpy(probeBuf, probeBase, plen);
            probeBuf[plen] = '\0';
            for (size_t i = 0; i < plen; i++)
              probeBuf[i] = (char)toupper((unsigned char)probeBuf[i]);
            rkIt = prefixMap.find(std::string(probeBuf, plen));
            if (rkIt != prefixMap.end()) break;
          }
        }
        if (rkIt != prefixMap.end()) {
          const auto &it = rkIt;
          // Arm suppression for subsequent lines
          s_rkBodySuppTick = GetTickCount();
          s_rkBodySuppTick_shared = s_rkBodySuppTick;
          s_rkBodySuppX = x;
          s_rkBodySuppBaseY = y;

          // QueueText Korean body with ADDCMD native values
          float fontH = 24.0f;
          if (font) {
            float rawH = (float)font->fontHeight;
            if (rawH > 1.0f) fontH = rawH;
          }
          float maxW = GetScreenWidthApprox() * 0.45f;
          KoreanRenderer::QueueText(
              it->second.korean,   // Korean body text
              x, y,                // ADDCMD position
              xScale,              // ADDCMD scale
              color,               // ADDCMD color
              fontH,               // ADDCMD font height
              style,               // ADDCMD style
              0.0f,                // finalWidthEng (unused)
              false,               // disableShadow
              false,               // isSmallMenu
              xScale,              // originalXScale
              false,               // allowZeroAlpha
              false,               // code7ResetsToBase
              false,               // disableAutoWrap
              -1,                  // forceAlign
              maxW);               // maxWidthPx for word wrap

          // Piggyback: re-QueueText cached header (prevents blink during hidden phase)
          if (s_rkHdrCacheValid && !s_rkHdrCachedText.empty()) {
            KoreanRenderer::QueueText(
                s_rkHdrCachedText, s_rkHdrX, s_rkHdrY,
                s_rkHdrScaleAdj, s_rkHdrColor, s_rkHdrFontH,
                s_rkHdrStyle, s_rkHdrEngW, s_rkHdrShadow,
                s_rkHdrSmall, s_rkHdrOrigScale,
                false, false, false, s_rkHdrAlign,
                s_rkHdrMaxW, s_rkHdrSkipClip, false, s_rkHdrAtlas);
          }

          // Suppress English line
          if (Original_R_AddCmdDrawText)
            Original_R_AddCmdDrawText(" ", 1, font, x, y, xScale, yScale,
                                      rotation, color, style);
          return;
        }
        } // cleanText.length() >= 5
      }
    }
  }
  // ==========================================================================
  // Two-part countdown timer capture (label + tail drawn separately by engine)
  // Only active when a TimeScript countdown is being suppressed — avoids
  // expensive string ops (TrimSpaces, StripColorCodes, ToUpperAscii) on every
  // R_AddCmdDrawText call during menus/normal gameplay.
  // ==========================================================================
  if (g_TimeScriptPersistSuppress.active) {
    static DWORD s_SplitCdLabelTick = 0;
    static float s_SplitCdLabelX = 0.0f;
    static float s_SplitCdLabelY = 0.0f;
    static float s_SplitCdXScale = 1.0f;
    static float s_SplitCdYScale = 1.0f;
    static float s_SplitCdFontHeight = 35.0f;
    static float s_SplitCdColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static std::string s_SplitCdKey;
    static std::string s_SplitCdEngPrefix;
    static std::string s_SplitCdKorPrefix;

    // --- Part 2: If tail capture is armed, check if this text is the timer value ---
    if (s_SplitCdLabelTick != 0) {
      const DWORD nowCd = GetTickCount();
      if ((nowCd - s_SplitCdLabelTick) <= 2) {
        const std::string tailCandidate = TrimSpaces(std::string(text));
        if (IsLikelyHudScriptCountdownTail(tailCandidate)) {
          float dy = y - s_SplitCdLabelY;
          if (dy < 0) dy = -dy;
          if (dy < 5.0f) {
            s_SplitCdLabelTick = 0; // disarm

            // Build combined Korean text: "전력 차단까지 1:02.6"
            std::string korText = s_SplitCdKorPrefix;
            if (!korText.empty() &&
                !std::isspace((unsigned char)korText.back())) {
              korText.push_back(' ');
            }
            korText += tailCandidate;

            HudScriptCountdownMatch match{};
            match.key = s_SplitCdKey;
            match.englishPrefix = s_SplitCdEngPrefix;
            match.koreanPrefix = s_SplitCdKorPrefix;
            match.tail = tailCandidate;
            match.renderText = korText;

            const float cdScale =
                (std::isfinite(s_SplitCdYScale) && s_SplitCdYScale > 0.01f &&
                 s_SplitCdYScale < 6.0f)
                    ? s_SplitCdYScale
                    : ((std::isfinite(s_SplitCdXScale) &&
                        s_SplitCdXScale > 0.01f && s_SplitCdXScale < 6.0f)
                           ? s_SplitCdXScale
                           : 1.0f);

            // A label-only snapshot must receive the native number before
            // this draw is suppressed. Keep updating on subsequent draws.
            TextHook_TimeScriptCaptureRenderedTimer(tailCandidate, s_SplitCdKey);

            // Only queue Korean overlay if the IAR-direct path isn't
            // already rendering for this key (prevents duplicate text).
            if (!TextHook_HasTimeScriptTimerReplacement() ||
                g_TimeScriptRuntimeState.renderSnapshot.key != s_SplitCdKey) {
              QueueHudScriptCountdownOverlay(
                  match, s_SplitCdLabelX, s_SplitCdLabelY, cdScale,
                  s_SplitCdFontHeight, 0, s_SplitCdColor, false, 0x00u,
                  "addcmd_split");
            }

            // Suppress English timer number
            if (Original_R_AddCmdDrawText) {
              Original_R_AddCmdDrawText(" ", maxChars, font, x, y, xScale,
                                        yScale, rotation, color, style);
            }
            return;
          }
        }
      }
      // Expired or non-matching tail — disarm
      if ((GetTickCount() - s_SplitCdLabelTick) > 2) {
        s_SplitCdLabelTick = 0;
      }
    }

    // --- Part 1: Check if this text is a countdown label prefix (no timer tail) ---
    if (text[0] != '\0') {
      const size_t textLen = strlen(text);
      // Quick first-char gate to avoid expensive work on every draw call
      if (textLen >= 4 && textLen <= 32 &&
          (text[0] == 'P' || text[0] == 'p' || text[0] == 'T' ||
           text[0] == 't' || text[0] == 'D' || text[0] == 'd')) {
        const std::string cleaned =
            TrimSpaces(StripColorCodes(std::string(text)));
        if (!cleaned.empty()) {
          EnsureTranslationsLoaded();
          static const char *kSplitCdKeys[] = {
              "CLOCKWORK_POWERDOWN", "CLOCKWORK_EXFIL",
              "SATFARM_TIME_IMACT"};
          for (const char *keyName : kSplitCdKeys) {
            auto itEng = g_KeyToEnglish.find(keyName);
            auto itKor = g_KeyToKorean.find(keyName);
            if (itEng == g_KeyToEnglish.end() ||
                itKor == g_KeyToKorean.end() || itEng->second.empty() ||
                itKor->second.empty()) {
              continue;
            }
            const std::string engTrimmed =
                TrimSpaces(StripColorCodes(itEng->second));
            if (ToUpperAscii(cleaned) == ToUpperAscii(engTrimmed)) {
              // Matched countdown LABEL without timer tail.
              // Suppress English label and arm timer-tail capture.
              s_SplitCdLabelTick = GetTickCount();
              s_SplitCdLabelX = x;
              s_SplitCdLabelY = y;
              s_SplitCdXScale = xScale;
              s_SplitCdYScale = yScale;
              s_SplitCdFontHeight = 35.0f;
              if (font) {
                float fh =
                    (float)((IW6Font::Font_s *)font)->fontHeight;
                if (fh > 5.0f && fh < 200.0f) {
                  s_SplitCdFontHeight = fh;
                }
              }
              if (color) {
                s_SplitCdColor[0] = color[0];
                s_SplitCdColor[1] = color[1];
                s_SplitCdColor[2] = color[2];
                s_SplitCdColor[3] = color[3];
              } else {
                s_SplitCdColor[0] = 1.0f;
                s_SplitCdColor[1] = 1.0f;
                s_SplitCdColor[2] = 1.0f;
                s_SplitCdColor[3] = 1.0f;
              }
              s_SplitCdKey = keyName;
              s_SplitCdEngPrefix = itEng->second;
              s_SplitCdKorPrefix = itKor->second;

              static DWORD s_lastSplitLabelLog = 0;
              const DWORD nowLog = GetTickCount();
              if ((nowLog - s_lastSplitLabelLog) > 900) {
                s_lastSplitLabelLog = nowLog;
                char lbuf[384];
                sprintf_s(
                    lbuf,
                    "[SPLIT-CD-LABEL] key=%s x=%.1f y=%.1f sx=%.3f sy=%.3f "
                    "fontH=%.1f a=%.2f text=\"%.48s\"",
                    keyName, x, y, xScale, yScale, s_SplitCdFontHeight,
                    s_SplitCdColor[3], text);
                LogToFile(lbuf);
              }

              // Suppress English label
              if (Original_R_AddCmdDrawText) {
                Original_R_AddCmdDrawText(" ", maxChars, font, x, y, xScale,
                                          yScale, rotation, color, style);
              }
              return;
            }
          }
        }
      }
    }
  }
  // === End two-part countdown timer capture ===

#ifdef DEBUG_TEXT_LOGGING
  // === TEST: Log ALL text to confirm R_AddCmdDrawText is being called ===
  static int allTextLogCount = 0;
  if (allTextLogCount < 100 && text && strlen(text) > 3) {
    allTextLogCount++;
    char logBuf[300];
    sprintf_s(logBuf, "[ADDCMD ALL] text=\"%.60s\" x=%.1f y=%.1f", text, x, y);
    LogToFile(logBuf);
  }

  // === DIAGNOSTIC: Detect subtitle text (with color codes like ^2, ^7) ===
  static int colorCodeLogCount = 0;
  if (colorCodeLogCount < 50 && text) {
    // Color codes like ^2, ^7 are typical in subtitles
    if (strstr(text, "^2") || strstr(text, "^7") || strstr(text, "^1")) {
      colorCodeLogCount++;
      char logBuf[256];
      sprintf_s(logBuf, "[ADDCMD SUBTITLE!] text=\"%.80s\" x=%.1f y=%.1f", text,
                x, y);
      LogToFile(logBuf);
    }
  }
#endif

  // --- [1] Ghost Text Suppression (Houston subtitle in menu) ---
  // Raw legacy flags were unverified. Menu keywords below establish state.
  auto IsMenuActive = []() -> bool { return false; };

  // Track menu state globally. IMPORTANT: raw menu flags can be noisy (stuck ON),
  // so only allow them to *extend* menu state if we recently saw strong menu text.
  static DWORD s_GlobalMenuTime = 0;
  static DWORD s_LastMenuKeywordTime = 0;
  static DWORD s_LastPopupHeaderTime = 0;
  static bool s_InMenu = false;


  const DWORD nowMenu = GetTickCount();
  const bool rawMenuFlags = IsMenuActive();

  // Strong menu-text detection (normalized; avoids variant markers and color codes).
  bool keywordMenu = false;
  std::string cleanedMenu;
  std::string cleanedUpper;
  if (text && text[0]) {
    // NOTE: "MISSION" alone is intentionally NOT used (false positives with
    // "Mission Objectives Updated" during gameplay).
    cleanedMenu = NormalizeMenuText(std::string(text));
    cleanedUpper = ToUpperAscii(cleanedMenu);

    if (cleanedUpper == "CAMPAIGN" || cleanedUpper == "MULTIPLAYER" ||
        cleanedUpper == "SQUADS" || cleanedUpper == "EXTINCTION" ||
        cleanedUpper == "PAUSE MENU" || cleanedUpper == "RESUME GAME" ||
        cleanedUpper == "SETTINGS" || cleanedUpper == "VIDEO" ||
        cleanedUpper == "AUDIO" || cleanedUpper == "CONTROLS" ||
        cleanedUpper == "BRIGHTNESS" || cleanedUpper == "FRIENDS" ||
        cleanedUpper == "MATCH SUMMARY" || cleanedUpper == "ACCEPT" ||
        cleanedUpper == "DECLINE" || cleanedUpper == "BACK" ||
        cleanedUpper == "CONTINUE" || cleanedUpper == "NEW GAME" ||
        cleanedUpper == "LOAD GAME" || cleanedUpper == "SAVE AND QUIT" ||
        cleanedUpper == "RESTART MISSION" || cleanedUpper == "MISSION RESTART" ||
        cleanedUpper == "MISSION SELECT" || cleanedUpper == "SELECT MISSION" ||
        cleanedUpper == "SELECT DIFFICULTY" || cleanedUpper == "APPLY SETTINGS" ||
        cleanedUpper == "NOTICE" || cleanedUpper == "OK" ||
        cleanedUpper == "CANCEL") {
      keywordMenu = true;
    }
    bool isPauseMenuText = false;
    bool isCenterPopupHeader = false;
    bool isMenuHeaderLarge = false;
    bool isYesNo = false;
    if (!keywordMenu) {
      // Korean-aware checks for common popup/menu headers.
      isPauseMenuText = IsPauseMenuText(cleanedMenu);
      isCenterPopupHeader = IsCenterPopupHeaderOnly(cleanedMenu);
      isMenuHeaderLarge = IsMenuHeaderForceLarge(cleanedMenu);
      if (isPauseMenuText || isCenterPopupHeader || isMenuHeaderLarge) {
        keywordMenu = true;
      }
      if (isPauseMenuText || isCenterPopupHeader || isMenuHeaderLarge) {
        s_LastPopupHeaderTime = nowMenu;
      }
      isYesNo = IsYesNoText(cleanedMenu);
      if (!keywordMenu && isYesNo) {
        // YES/NO alone should not flip gameplay into menu context.
        // Only treat it as menu when it follows a popup/header shortly.
        constexpr DWORD kYesNoAfterHeaderMs = 2500;
        if (s_LastPopupHeaderTime != 0 &&
            (nowMenu - s_LastPopupHeaderTime) <= kYesNoAfterHeaderMs) {
          keywordMenu = true;
        }
      }
    }
    if (!keywordMenu) {
      // Keep this conservative: broad "PAUSE/OPTIONS" substring probes caused
      // gameplay false positives and objective gate flicker.
      if (cleanedUpper.find("SETTINGS") != std::string::npos) {
        keywordMenu = true;
      }
    }
  }

  if (keywordMenu) {
    s_LastMenuKeywordTime = nowMenu;
  }

  // Decay/hold windows. Keep these short to avoid pinning menu state when raw flags
  // are noisy in gameplay.
  constexpr DWORD kMenuDecayMs = 2000;
  constexpr DWORD kRawExtendMs = 2500;

  const bool extendByRaw =
      rawMenuFlags && (s_LastMenuKeywordTime != 0) &&
      ((nowMenu - s_LastMenuKeywordTime) <= kRawExtendMs);

  if (keywordMenu || extendByRaw) {
    s_GlobalMenuTime = nowMenu;
    s_InMenu = true;
  }

  if ((nowMenu - s_GlobalMenuTime) > kMenuDecayMs) {
    s_InMenu = false;
  }

  // Frontend menu context is now driven by Cbuf_AddText disconnect/loadgame
  // detection — no per-call keyword scanning needed here.

  g_MenuContext.store(s_InMenu);
  {
    static bool s_LastLoggedMenuState = false;
    if (s_InMenu != s_LastLoggedMenuState) {
      const std::string preview =
          cleanedMenu.empty() ? std::string("(empty)")
                              : Utf8Preview(cleanedMenu, 64);
      char mbuf[384];
      sprintf_s(mbuf,
                "[MENU-CTX] menu=%d raw=%d kw=%d text=\"%s\" x=%.1f y=%.1f",
                s_InMenu ? 1 : 0,
                rawMenuFlags ? 1 : 0, keywordMenu ? 1 : 0,
                preview.c_str(), x, y);
      LogToFile(mbuf);
    }
    s_LastLoggedMenuState = s_InMenu;
  }

  // Pause/ESC menu detection for intro/subtitle timing control.
  // Keep pause clock alive while ESC menu remains active, including options
  // submenus where "PAUSED" text may not be rendered every frame.
  if (s_InMenu) {
    const bool hasMenuText = !cleanedMenu.empty();
    const bool pauseSignal =
        hasMenuText &&
        (IsPauseMenuText(cleanedMenu) || IsPauseRootHeaderText(cleanedMenu) ||
         IsOptionsHeaderText(cleanedMenu) ||
         IsAdvancedVideoHeaderText(cleanedMenu));
    const DWORD lastPauseTick = g_LastPauseMenuTime.load();
    const bool pauseStickyMenu =
        (!pauseSignal && lastPauseTick != 0 && (nowMenu - lastPauseTick) <= 450);
    if (pauseSignal || pauseStickyMenu) {
      g_LastPauseMenuTime.store(nowMenu);
    }
  }

  // === SUBTITLE NATIVE CAPTURE - DISABLED ===
  // In this build, in-game/video subtitles do not go through R_AddCmdDrawText,
  // so capturing subtitle tint/style here does not work and should not affect
  // menu rendering. Keep the old reference code under #if 0.
#if 0
  if (text && strlen(text) > 5) {
    std::string textStr(text);
    std::string textNorm = NormalizeEnglishKey(textStr);

    static int debugMapCount = 0;
    if (debugMapCount < 20) {
      std::shared_lock<std::shared_mutex> dbgLock(g_SubtitleMapMutex);
      size_t mapSize = g_SubtitleOnlyMap.size();
      dbgLock.unlock();

      if (mapSize > 0) {
        debugMapCount++;
        char dbgBuf[300];
        sprintf_s(dbgBuf,
                  "[DEBUG] SubtitleMap size=%zu, looking for: '%.50s...'",
                  mapSize, textNorm.c_str());
        LogToFile(dbgBuf);
      }
    }

    // Thread-safe lookup in SUBTITLE-ONLY map (NOT g_EngToKorOverlayMap which
    // has menu texts)
    std::shared_lock<std::shared_mutex> lock(g_SubtitleMapMutex);
    auto it = g_SubtitleOnlyMap.find(textNorm);

    if (it != g_SubtitleOnlyMap.end()) {
      SubtitleOnlyInfo subInfo = it->second;
      // This is an English SUBTITLE we translated!
      lock.unlock(); // Release lock before further processing

      // Check menu state - don't capture state in menus
      bool inMenuContext = IsMenuActive();

      if (!inMenuContext && g_EnableSubtitleNative) {
        // CAPTURE NATIVE STATE from this English subtitle render call
        float fHeight = 0.0f;
        if (font) {
          fHeight = (float)((IW6Font::Font_s *)font)->fontHeight;
        }

        // Update global state for D3D11Hook to use
        TextHook_UpdateNativeSubtitleState(x, y, color, fHeight, style, xScale,
                                           yScale);

        // Update per-line native state for typewriter + per-line sync
        SubtitleNativeEntry entry{};
        entry.x = x;
        entry.y = y;
        entry.xScale = xScale;
        entry.yScale = yScale;
        entry.fontHeight = fHeight;
        entry.style = style;
        entry.lastUpdate = GetTickCount();
        entry.valid = true;

        if (color) {
          entry.color[0] = color[0];
          entry.color[1] = color[1];
          entry.color[2] = color[2];
          entry.color[3] = color[3];
        } else {
          entry.color[0] = 1.0f;
          entry.color[1] = 1.0f;
          entry.color[2] = 1.0f;
          entry.color[3] = 1.0f;
        }

        std::string cleanNow = StripColorCodes(textStr);
        entry.visibleChars = (int)cleanNow.size();
        entry.totalChars =
            (subInfo.totalChars > 0) ? subInfo.totalChars : entry.visibleChars;

        {
          std::lock_guard<std::mutex> lock2(g_SubtitleNativeMutex);
          if (!textNorm.empty()) {
            g_SubtitleNativeByNorm[textNorm] = entry;
          }
          if (!subInfo.key.empty()) {
            g_SubtitleNativeByKey[subInfo.key] = entry;
          }
        }

        // Throttled logging
        static int captureLogCount = 0;
        if (captureLogCount < 30) {
          captureLogCount++;
          char logBuf[256];
          sprintf_s(logBuf,
                    "[Capture] SUBTITLE English='%.30s...' x=%.1f y=%.1f "
                    "A=%.2f h=%.1f "
                    "style=%d vis=%d/%d",
                    text, x, y, (color ? color[3] : 1.0f), fHeight, style,
                    entry.visibleChars, entry.totalChars);
          LogToFile(logBuf);
        }
      }

      // SUPPRESS ENGLISH SUBTITLE - Don't render it (Korean is queued for
      // D3D11Hook)
      return;
    }
  }
#endif

  // === Native HUD capture from R_AddCmdDrawText (partial path) ===
  // Some center gameplay hints (notably CORNERED_BINOCULARS_* Press/Hold lines)
  // are drawn through AddCmd instead of HUD_DrawText560.
  // We capture params here for alignment only; this does NOT alter menu draw flow.
  if (TextHook_IsHudNativeEnabled()) {
    // Credits guard: credits text must not enter HUD tail-capture state —
    // prevents false-positive objective/hint overlays and avoids per-call
    // processing overhead for the credit-roll draw flood.
    do {
    { extern std::atomic<bool> g_creditsGuardActive;
      extern std::atomic<bool> g_creditsSoftGuard;
      if (g_creditsGuardActive.load(std::memory_order_relaxed) ||
          g_creditsSoftGuard.load(std::memory_order_relaxed)) break; }
    static uintptr_t s_modBase = (uintptr_t)GetModuleHandleA(NULL);
    static std::mutex s_addcmdCallerMtx;
    static std::unordered_set<uintptr_t> s_trustedHudCallers = {IW6Offsets::Profile::Rva_27DF17};

    std::string rawText(text);
    uintptr_t addcmdCallerOffset = (uintptr_t)_ReturnAddress() - s_modBase;
    if (!s_InMenu && !TextHook_IsPauseMenuLikely()) {
      HudScriptCountdownMatch countdownMatch{};
      if (TryResolveHudScriptCountdownText(rawText, countdownMatch)) {
        float countdownFontHeight = 35.0f;
        if (font) {
          float fh = (float)((IW6Font::Font_s *)font)->fontHeight;
          if (fh > 5.0f && fh < 200.0f) {
            countdownFontHeight = fh;
          }
        }

        float countdownColor[4] = {0.8f, 1.0f, 0.8f, 1.0f};
        if (color) {
          countdownColor[0] = color[0];
          countdownColor[1] = color[1];
          countdownColor[2] = color[2];
          countdownColor[3] = color[3];
        }

        const float countdownScale =
            (std::isfinite(yScale) && yScale > 0.01f && yScale < 6.0f)
                ? yScale
                : ((std::isfinite(xScale) && xScale > 0.01f && xScale < 6.0f)
                       ? xScale
                       : 1.0f);
        QueueHudScriptCountdownOverlay(
            countdownMatch, x, y, countdownScale, countdownFontHeight, style,
            countdownColor, false, (unsigned int)addcmdCallerOffset, "addcmd");
        if (Original_R_AddCmdDrawText) {
          Original_R_AddCmdDrawText(" ", maxChars, font, x, y, xScale, yScale,
                                    rotation, color, style);
        }
        return;
      }

    }

    bool trustedHudCaller = false;
    {
      std::lock_guard<std::mutex> lk(s_addcmdCallerMtx);
      trustedHudCaller =
          (s_trustedHudCallers.find(addcmdCallerOffset) !=
           s_trustedHudCallers.end());
    }
    // Compute tail-join prompt activity BEFORE anonymous capture so weapon-name
    // texts from untrusted callers can still be captured and suppressed.
    const DWORD lastPromptForSuppress =
        g_lastTailJoinPromptTick.load(std::memory_order_relaxed);
    const DWORD nowForSuppress = GetTickCount();
    const bool tailJoinPromptRecentlyActive =
        (lastPromptForSuppress != 0 &&
         (nowForSuppress - lastPromptForSuppress) <= 350);
    // When a tail-join prompt is active, trust anonymous weapon-name captures
    // even from callers not yet in the trusted set.  The internal guards in
    // CaptureHudRuntimeTailFragment (prompt-tick gate, IsLikelyHudRuntimeTailText)
    // prevent false positives.
    const bool tailCaptureTrusted =
        trustedHudCaller || tailJoinPromptRecentlyActive;
    CaptureHudRuntimeTailFragment("", rawText, x, y,
                                  (unsigned int)addcmdCallerOffset,
                                  tailCaptureTrusted, !s_InMenu);

    const float screenHeightApprox = GetScreenHeightApprox();
    // Suppress weapon-name tail text in AddCmd when a tail-join prompt is
    // active.  trustedHudCaller is NOT required here — the prompt-active gate
    // + IsLikelyHudRuntimeTailText + position check are sufficient evidence.
    const bool suppressTailOnlyAddCmd =
        tailJoinPromptRecentlyActive &&
        !s_InMenu &&
        IsLikelyHudRuntimeTailText(rawText) &&
        std::isfinite(y) && std::isfinite(screenHeightApprox) &&
        screenHeightApprox > 0.0f &&
        y >= (screenHeightApprox * 0.60f);
    if (suppressTailOnlyAddCmd) {
      if (kVerboseRuntimeLogs) {
        static std::unordered_map<unsigned int, DWORD> s_tailSuppressLogTick;
        const DWORD nowTailSuppress = GetTickCount();
        const unsigned int suppressKey = (unsigned int)addcmdCallerOffset;
        auto itSuppress = s_tailSuppressLogTick.find(suppressKey);
        if (itSuppress == s_tailSuppressLogTick.end() ||
            (nowTailSuppress - itSuppress->second) > 1500) {
          s_tailSuppressLogTick[suppressKey] = nowTailSuppress;
          char sbuf[384];
          sprintf_s(sbuf,
                    "[HUD-TAIL-SUPPRESS] src=addcmd caller=+0x%llX x=%.1f y=%.1f text=\"%.96s\"",
                    (unsigned long long)addcmdCallerOffset, x, y, rawText.c_str());
          LogToFile(sbuf);
        }
      }
      if (Original_R_AddCmdDrawText) {
        Original_R_AddCmdDrawText(" ", maxChars, font, x, y, xScale, yScale,
                                  rotation, color, style);
      }
      return;
    }

    const bool looksPromptLike = LooksLikePressHoldPrompt(rawText);
    const bool looksHudKeyLike = LooksLikeHudLocalizationKey(rawText);
    // Credits guard: skip AddCmd HUD key processing during credits.
    bool addcmdCreditsGuarded = false;
    {
      extern std::atomic<bool> g_creditsGuardActive;
      addcmdCreditsGuarded =
          g_creditsGuardActive.load(std::memory_order_relaxed);
    }
    if (!addcmdCreditsGuarded && (looksPromptLike || looksHudKeyLike)) {
      EnsureTranslationsLoaded();
      std::string hudKey;
      bool resolved = ResolveKeyFromEnglishInternal(rawText, hudKey);
      std::string hudAutoReason;
      const bool isGameplayHudKey =
          resolved && IsGameplayHudKeyName(hudKey, &hudAutoReason) &&
          (hudKey.find("SUBTITLE_") != 0);
      const bool isObjectiveLikeKey =
          resolved &&
          (hudKey.find("_OBJ_") != std::string::npos ||
           hudKey.find("_OBJECTIVE_") != std::string::npos ||
           HasObjectiveSuffixOnlyKey(hudKey) ||
           hudKey.rfind("GAME_OBJECTIVE", 0) == 0 ||
           hudKey.rfind("CGAME_OBJECTIVE", 0) == 0 ||
           hudKey.rfind("CORNERED_OBJ_", 0) == 0 ||
           hudKey.rfind("CORNERED_OBJECTIVE_", 0) == 0);

      // AddCmd center prompts can come from multiple callsites. Prefer key-based
      // gameplay filtering over caller whitelists so we don't miss valid hints.
      {
        std::lock_guard<std::mutex> lk(s_addcmdCallerMtx);
        if (isGameplayHudKey) {
          s_trustedHudCallers.insert(addcmdCallerOffset);
        }
        trustedHudCaller =
            (s_trustedHudCallers.find(addcmdCallerOffset) !=
             s_trustedHudCallers.end());
      }

      if (isGameplayHudKey && !hudAutoReason.empty()) {
        MaybeLogHudKeyAuto(hudKey, addcmdCallerOffset,
                           std::string("addcmd_") + hudAutoReason);
      }

      const bool shouldCapture = isGameplayHudKey;

      if (shouldCapture) {
        // Keep runtime english?봩ey map fresh for draw-time variants.
        TextHook_RegisterRuntimeEnglishKey(hudKey.c_str(), rawText.c_str());
        HudNativeEntry entry{};
        entry.key = hudKey;
        entry.x = x;
        entry.y = y;
        entry.xScale = (xScale > 0.01f) ? xScale : 0.65f;
        entry.yScale = (yScale > 0.01f) ? yScale : 0.65f;
        entry.fontHeight = 35.0f;
        if (font) {
          float fh = (float)((IW6Font::Font_s *)font)->fontHeight;
          if (fh > 5.0f && fh < 200.0f) {
            entry.fontHeight = fh;
          }
        }
        entry.style = style;
        entry.lastUpdate = GetTickCount();
        // AddCmd uses pixel-space params and often flattens alpha.
        // Keep source distinct so HUD560/scanner virtual captures can win.
        entry.source = HUD_NATIVE_ADDCMD;
        entry.sourceToken = NATIVE_HUD_SOURCE_ADDCMD;
        entry.callerOffset = (unsigned int)addcmdCallerOffset;
        entry.objectiveLike = isObjectiveLikeKey;
        entry.objectiveChannel = (unsigned char)DetermineObjectiveChannelForKey(
            hudKey, TextHook_IsPauseMenuLikely());
        // R_AddCmdDrawText passes screen-space coordinates on this path.
        // Mark as pixel space so renderer does not re-scale X/Y.
        entry.virtualW = 0;
        entry.virtualH = 0;
        entry.valid = true;

        if (color) {
          entry.color[0] = color[0];
          entry.color[1] = color[1];
          entry.color[2] = color[2];
          entry.color[3] = color[3];
        } else {
          entry.color[0] = 1.0f;
          entry.color[1] = 1.0f;
          entry.color[2] = 1.0f;
          entry.color[3] = 0.6f;
        }

        if (IsDirectAddCmdObjHudHintKey(hudKey)) {
          // Direct AddCmd prompt keys are now observation-only on this path.
          // Final rendering belongs to the SSOT native family pipeline
          // (HUD560/ADDCMD -> OBJHUDHINT), which already owns tail-join.
          // Keeping the old immediate QueueText path here caused per-frame
          // state deletion/recreation churn and visible stutter on swap/pickup
          // prompts.
          NoteRecentHudRuntimeTailJoinPrompt(hudKey, entry.x, entry.y,
                                             entry.callerOffset);
        }
        TextHook_UpdateHudNativeEntry(entry);
        TextHook_OnHudKey(hudKey.c_str());
        CaptureHudRuntimeTailFragment(hudKey, rawText, entry.x, entry.y,
                                      entry.callerOffset, trustedHudCaller,
                                      !s_InMenu);
        HudRouteChannel directChannel = HUD_ROUTE_UNRESOLVED;
        if (ClassifyNativeHudDisplayChannelDirect(entry, directChannel)) {
          ApplyNativeHudEntryDirect(hudKey, entry, &rawText);
          if (!entry.objectiveLike &&
              (directChannel == HUD_ROUTE_HINT ||
               directChannel == HUD_ROUTE_OBJHUDHINT)) {
            // Bypass legacy AddCmd translation queue to avoid duplicate
            // Korean overlays for gameplay HUD prompts.
            routedGameplayHudPrompt = true;
            if (IsHudRuntimeTailJoinPromptKey(hudKey)) {
              routedTailJoinKey = true;
            }
          }
        }

        if (kVerboseRuntimeLogs) {
          static std::unordered_map<std::string, DWORD> s_lastAddcmdHudLog;
          const DWORD nowCap = entry.lastUpdate;
          auto itCap = s_lastAddcmdHudLog.find(hudKey);
          if (itCap == s_lastAddcmdHudLog.end() ||
              (nowCap - itCap->second) > 2500) {
            s_lastAddcmdHudLog[hudKey] = nowCap;
            if (s_lastAddcmdHudLog.size() > 96) {
              for (auto it = s_lastAddcmdHudLog.begin();
                   it != s_lastAddcmdHudLog.end();) {
                if ((nowCap - it->second) > 60000) {
                  it = s_lastAddcmdHudLog.erase(it);
                } else {
                  ++it;
                }
              }
            }
            char capBuf[512];
            sprintf_s(capBuf,
                      "[HUD-ADDCMD] caller=+0x%llX key='%s' x=%.1f y=%.1f sx=%.3f sy=%.3f a=%.2f h=%.1f text=\"%.80s\"",
                      (unsigned long long)addcmdCallerOffset, hudKey.c_str(),
                      entry.x, entry.y, entry.xScale, entry.yScale,
                      entry.color[3], entry.fontHeight, text);
            LogToFile(capBuf);
          }
        }
      } else if (!resolved && kVerboseRuntimeLogs) {
        static std::mutex s_addcmdFailMtx;
        static std::unordered_set<std::string> s_addcmdFailLogged;
        std::string norm = ToUpper(NormalizeForMatching(rawText));
        bool shouldLog = false;
        {
          std::lock_guard<std::mutex> lk(s_addcmdFailMtx);
          if (s_addcmdFailLogged.size() < 200 &&
              s_addcmdFailLogged.insert(norm).second) {
            shouldLog = true;
          }
        }
        if (shouldLog) {
          std::string preview = StripColorCodes(rawText);
          preview = TrimSpaces(preview);
          if (preview.size() > 120)
            preview.resize(120);
          char failBuf[512];
          sprintf_s(failBuf,
                    "[HUD-ADDCMD-RESOLVE-FAIL] caller=+0x%llX text=\"%s\" menuCtx=%d",
                    (unsigned long long)addcmdCallerOffset, preview.c_str(),
                    s_InMenu ? 1 : 0);
          LogToFile(failBuf);
        }
      } else if (kVerboseRuntimeLogs) {
        static std::mutex s_addcmdSkipMtx;
        static std::unordered_set<std::string> s_addcmdSkipLogged;
        std::string sig = hudKey + "|" + ToUpper(NormalizeForMatching(rawText));
        bool shouldLog = false;
        {
          std::lock_guard<std::mutex> lk(s_addcmdSkipMtx);
          if (s_addcmdSkipLogged.size() < 200 &&
              s_addcmdSkipLogged.insert(sig).second) {
            shouldLog = true;
          }
        }
        if (shouldLog) {
          std::string preview = StripColorCodes(rawText);
          preview = TrimSpaces(preview);
          if (preview.size() > 120)
            preview.resize(120);
          char skipBuf[512];
          sprintf_s(skipBuf,
                    "[HUD-ADDCMD-SKIP] caller=+0x%llX key=%s text=\"%s\" menuCtx=%d",
                    (unsigned long long)addcmdCallerOffset, hudKey.c_str(),
                    preview.c_str(), s_InMenu ? 1 : 0);
          LogToFile(skipBuf);
        }
      }
    }
    } while (0); // credits-break end
  }

  // Gameplay HUD hints/objhudhints are rendered by dedicated overlay pipelines.
  // Do not run legacy AddCmd translation queue for the same draw call.
  if (routedGameplayHudPrompt) {
    if (Original_R_AddCmdDrawText) {
      // Tail-join keys (PLATFORM_SWAPWEAPONS etc.) must ALWAYS suppress
      // the English original so the Korean overlay with joined weapon-name
      // tail is the only visible text.  Other gameplay keys honour the
      // user's g_KeepHudEnglishReference preference.
      const bool keepHudEnglish =
          g_KeepHudEnglishReference && !routedTailJoinKey &&
          !(IsMenuActive() || s_InMenu);
      if (!keepHudEnglish) {
        // Throttled log: one per key per 2s
        {
          static std::unordered_map<std::string, DWORD> s_suppressLog;
          const DWORD nowSup = GetTickCount();
          std::string supKey(text ? text : "");
          if (supKey.size() > 60) supKey.resize(60);
          auto itSup = s_suppressLog.find(supKey);
          if (itSup == s_suppressLog.end() || (nowSup - itSup->second) > 2000) {
            s_suppressLog[supKey] = nowSup;
            char sbuf[256];
            sprintf_s(sbuf,
                "[HUD-ENG-SUPPRESS] text=\"%.80s\" tail=%d menu=%d x=%.0f y=%.0f",
                text ? text : "", routedTailJoinKey ? 1 : 0,
                (IsMenuActive() || s_InMenu) ? 1 : 0, x, y);
            LogToFile(sbuf);
          }
        }
        Original_R_AddCmdDrawText(" ", maxChars, font, x, y, xScale, yScale,
                                  rotation, color, style);
      } else {
        Original_R_AddCmdDrawText(text, maxChars, font, x, y, xScale, yScale,
                                  rotation, color, style);
      }
    }
    return;
  }

  // Suppress Houston/Picking subtitles in menu context
  if (text[0] == 'P' || text[0] == 'H') {
    if (strstr(text, "Houston") || strstr(text, "Picking it up")) {
      // Suppress if memory flags OR in menu context
      if (IsMenuActive() || s_InMenu)
        return;
    }
  }

  // Suppress Clockwork Kick subtitle in menu context (Korean/English text)
  if (IsMenuActive() || s_InMenu) {
    std::string cleanMenuText = StripColorCodes(std::string(text));
    if (cleanMenuText.find("Kick:") != std::string::npos &&
        cleanMenuText.find("Blackbird") != std::string::npos &&
        (cleanMenuText.find("ten minutes") != std::string::npos ||
         cleanMenuText.find("ten minute") != std::string::npos)) {
      return;
    }
  }

  // Suppress specific subtitle key in menu context
  if (IsMenuActive() || s_InMenu) {
    std::string cleanMenuText = StripColorCodes(std::string(text));
    std::string upperMenuText = ToUpper(cleanMenuText);
    auto itKey = g_EnglishToKey.find(upperMenuText);
    if (itKey != g_EnglishToKey.end() &&
        itKey->second == "SUBTITLE_CLOCKWORK_DIZ_NORTHERNRIDGE32") {
      return;
    }
    // Fallback: suppress by matching the English subtitle sentence
    if (cleanMenuText.find("Kick:") != std::string::npos &&
        (cleanMenuText.find("ten minutes") != std::string::npos ||
         cleanMenuText.find("ten minute") != std::string::npos) &&
        cleanMenuText.find("Blackbird") != std::string::npos) {
      return;
    }
  }

  // === DEBUG: Log ORIGINAL English text style BEFORE translation ===
  static int origStyleLogCount = 0;
  if (origStyleLogCount < 100) {
    // Log main menu items
    if (strstr(text, "CAMPAIGN") || strstr(text, "MULTIPLAYER") ||
        strstr(text, "SQUADS") || strstr(text, "EXTINCTION") ||
        strstr(text, "RESUME") || strstr(text, "NEW GAME") ||
        strstr(text, "VIDEO") || strstr(text, "Aspect")) {
      origStyleLogCount++;
      char logBuf[512];
      sprintf(logBuf, "[ORIG STYLE] text=\"%.40s\" style=%d x=%.1f y=%.1f",
              text, style, x, y);
      LogToFile(logBuf);
    }
  }

  // === TRANSLATION LOGIC (ALWAYS RUNS) ===
  // Credits guard for translation block: credits strings are never in
  // localize.json, so skip expensive O(N) fallback loops during credits.
  const bool translationCreditsGuarded = []() -> bool {
    extern std::atomic<bool> g_creditsGuardActive;
    return g_creditsGuardActive.load(std::memory_order_relaxed);
  }();
  const char *translated = text;
  const char *korean = nullptr;
  std::string translatedKey;
  bool isSuppressedSuffix = false;
  bool textAlreadyKorean = ContainsKorean(text);

  // CASE 1: Text already contains Korean (from SEH hook returning Korean
  // directly) In this case, we need to render it ourselves since engine fonts
  // may not have Korean glyphs
  if (textAlreadyKorean) {
    korean = text; // The text itself is already Korean
    translated = text;

    static int koreanDetectedCount = 0;
    if (koreanDetectedCount < 10) {
      koreanDetectedCount++;
      LogToFile("[TextHook] KOREAN TEXT: \"" +
                Utf8Preview(std::string(text), 60) + "\"");
    }
  }
  // CASE 2: Text is English - try to find translation
  else {
    // Phase 6: Primary path ??g_EnglishToKey ??g_KeyToKorean chain
    {
      std::string cleanText = StripColorCodes(std::string(text));
      std::string upperText = ToUpper(cleanText);

      auto itKey = g_EnglishToKey.find(upperText);
      if (itKey != g_EnglishToKey.end()) {
        translatedKey = itKey->second;
        auto itKor = g_KeyToKorean.find(itKey->second);
        if (itKor != g_KeyToKorean.end() && !itKor->second.empty()) {
          korean = itKor->second.c_str();
        }
      }
    }

    // Prefix probe for split long menu texts.  The engine word-wraps
    // descriptions and passes each visual line as a separate call.
    // g_PrefixToKey stores word-boundary prefixes at lengths 30..60
    // built from localize.json — O(14) hash lookups, no linear scan.
    if (!korean) {
      std::string pfxClean = StripColorCodes(std::string(text));
      std::string pfxUpper = ToUpper(pfxClean);
      if (pfxUpper.size() >= 30) {
        for (size_t targetLen : {(size_t)60, (size_t)55, (size_t)50,
                                 (size_t)45, (size_t)40, (size_t)35,
                                 (size_t)30}) {
          if (pfxUpper.size() <= targetLen) continue;
          bool found = false;
          // Exact cut
          {
            auto it = g_PrefixToKey.find(pfxUpper.substr(0, targetLen));
            if (it != g_PrefixToKey.end()) {
              auto itKor = g_KeyToKorean.find(it->second);
              if (itKor != g_KeyToKorean.end() && !itKor->second.empty()) {
                korean = itKor->second.c_str();
                translatedKey = it->second;
                found = true;
              }
            }
          }
          // Word-boundary cut (mirrors LoadTranslations logic)
          if (!found) {
            size_t mx = (std::min)(targetLen + 3, pfxUpper.size());
            size_t sp = pfxUpper.rfind(' ', mx);
            if (sp != std::string::npos && sp > targetLen - 10 &&
                sp != targetLen) {
              auto it = g_PrefixToKey.find(pfxUpper.substr(0, sp + 1));
              if (it != g_PrefixToKey.end()) {
                auto itKor = g_KeyToKorean.find(it->second);
                if (itKor != g_KeyToKorean.end() &&
                    !itKor->second.empty()) {
                  korean = itKor->second.c_str();
                  translatedKey = it->second;
                  found = true;
                }
              }
            }
          }
          if (found) {
            // Arm body suppression for subsequent split fragments
            s_BodySuppTime = GetTickCount();
            s_BodySuppX = x;
            s_BodySuppBaseY = y;
            s_BodySuppDurationMs = 50;
            break;
          }
        }
      }
    }

    // Fallback: SEH overlay map (runtime-discovered variants, append-only)
    // Credits guard: credits strings are never in the overlay map, so skip
    // the entire section (lock + O(N) prefix scan) when credits are active.
    if (!korean && !translationCreditsGuarded) {
      std::shared_lock<std::shared_mutex> lock(g_OverlayMapMutex);
      std::string cleanText = StripColorCodes(std::string(text));
      std::string upperText = ToUpper(cleanText);

      // DEBUG: Log overlay map size once
      static bool s_MapSizeLogged = false;
      if (!s_MapSizeLogged) {
        s_MapSizeLogged = true;
        LogToFile("[OVERLAY] Map size: " + std::to_string(g_EngToKorOverlayMap.size()) + " entries");
      }

      static int overlayLookupLog = 0;
      bool isKeyword = (cleanText.find("Federation") != std::string::npos ||
                        cleanText.find("Objective") != std::string::npos ||
                        cleanText.find("Press") != std::string::npos ||
                        cleanText.find("Logan") != std::string::npos ||
                        cleanText.find("Ghost") != std::string::npos ||
                        cleanText.find("Day") != std::string::npos);
      // Suppress logging on Rorke detail pages: untranslated text like
      // "101% COMPLETE" causes hundreds of LogToFile disk I/O calls per
      // second via the isKeyword bypass, degrading FPS.
      const bool rorkeLogSuppress = KoreanRenderer_IsRecentRorkeDetailActive(2000);
      bool shouldLog = !rorkeLogSuppress &&
                       ((overlayLookupLog < 200 && cleanText.length() > 5) || isKeyword);

      auto it = g_EngToKorOverlayMap.find(upperText);
      if (it != g_EngToKorOverlayMap.end()) {
        const std::string &overlayVal = it->second;
        std::string overlayNorm = NormalizeMenuText(overlayVal);
        std::string cleanNorm = NormalizeMenuText(cleanText);
        const bool overlayLooksEnglishEcho =
            (!ContainsKorean(overlayVal.c_str()) &&
             ToUpperAscii(overlayNorm) == ToUpperAscii(cleanNorm));
        if (!overlayLooksEnglishEcho) {
          korean = overlayVal.c_str();
        }

        // Log successful match
        if (shouldLog) {
          overlayLookupLog++;
          std::string korPreview = Utf8Preview(it->second, 30);
          LogToFile("[OVERLAY MATCH] \"" + Utf8Preview(cleanText, 40) +
                    "\" -> \"" + korPreview + "\"");
        }
      } else {
        // Log failed lookup for debugging
        if (shouldLog) {
          overlayLookupLog++;
          LogToFile("[OVERLAY MISS] \"" + Utf8Preview(cleanText, 40) +
                    "\" upper=\"" + Utf8Preview(upperText, 40) + "\" NOT in map");
        }
        // [FIX] Try Longest Prefix Match
        // The engine word-wraps long menu descriptions and passes each
        // visual line as a separate R_AddCmdDrawText call.  The first
        // fragment won't match the full text exactly, but it IS a prefix
        // of the full overlay-map entry.
        // Rorke file body text (the previous reason for the 50-char
        // limit) is now intercepted by the dedicated O(1) Rorke body
        // handler above and never reaches this point.
        //
        // Miss cache: avoid O(N) scan for strings already known to have no
        // prefix match.  R_AddCmdDrawText runs on the render thread so a
        // plain static is safe.  Cache expires after 10 s or 2048 entries.
        {
          static std::unordered_set<std::string> s_overlayPrefixMissCache;
          static DWORD s_overlayPrefixMissCacheTick = 0;
          const DWORD nowPfx = GetTickCount();
          if ((nowPfx - s_overlayPrefixMissCacheTick) > 10000 ||
              s_overlayPrefixMissCache.size() > 2048) {
            s_overlayPrefixMissCache.clear();
            s_overlayPrefixMissCacheTick = nowPfx;
          }
          if (s_overlayPrefixMissCache.count(upperText) == 0) {
            size_t bestMatchLen = 0;
            for (const auto &entry : g_EngToKorOverlayMap) {
              const std::string &key = entry.first;
              // Check if upperText starts with key
              if (key.length() >= 20 &&
                  upperText.compare(0, key.length(), key) == 0) {
                if (key.length() > bestMatchLen) {
                  bestMatchLen = key.length();
                  const std::string &overlayVal = entry.second;
                  std::string overlayNorm = NormalizeMenuText(overlayVal);
                  std::string cleanNorm = NormalizeMenuText(cleanText);
                  const bool overlayLooksEnglishEcho =
                      (!ContainsKorean(overlayVal.c_str()) &&
                       ToUpperAscii(overlayNorm) == ToUpperAscii(cleanNorm));
                  if (!overlayLooksEnglishEcho) {
                    korean = overlayVal.c_str();
                  } else {
                    korean = nullptr;
                  }
                }
              }
            }
            if (!korean) {
              s_overlayPrefixMissCache.insert(upperText);
            } else {
              // Arm body suppression: subsequent split lines at same x,
              // below this y, within 50ms are English continuations.
              if (bestMatchLen < upperText.length()) {
                s_BodySuppTime = GetTickCount();
                s_BodySuppX = x;
                s_BodySuppBaseY = y;
                s_BodySuppDurationMs = 50;
              }
              static int prefixLogCount = 0;
              if (prefixLogCount < 10) {
                prefixLogCount++;
                LogToFile("[PREFIX] \"" + Utf8Preview(upperText, 40) +
                          "...\" matched (len=" + std::to_string(bestMatchLen) +
                          ")");
              }
            }
          }
        }
      }
    } // end if (!korean) fallback to overlay map

    // If not found in overlay map, try the static translation lookup.
    // Credits guard: FindTranslation has O(N) fallback loops (g_BindingTemplates,
    // g_PrefixToKey, g_RuntimeBindingTemplateToKey).  Credits strings never
    // resolve there, so skip during credits to avoid pure overhead.
    if (!korean && !translationCreditsGuarded) {
      korean = FindTranslation(text);
    }
    if (!korean && text) {
      std::string quitNorm =
          ToUpper(NormalizeForMatching(StripColorCodes(std::string(text))));
      const bool quitBodyLead =
          (quitNorm.rfind("IF YOU QUIT NOW", 0) == 0) ||
          (quitNorm.rfind("YOU WILL LOSE ANY PROGRESS", 0) == 0);
      if (quitBodyLead) {
        auto itQuitBody = g_KeyToKorean.find(
            "If you quit now, you will lose any progress since your last checkpoint.");
        if (itQuitBody != g_KeyToKorean.end() && !itQuitBody->second.empty()) {
          korean = itQuitBody->second.c_str();
        }
      }
    }
    // Check if this is a suffix fragment that should be suppressed
    // (second part of a split long sentence, already translated via prefix)
    if (!korean && text) {
      std::string cleanText = StripColorCodes(std::string(text));
      std::string upperText = ToUpper(cleanText);

      // Suffix Suppression Logic
      if (g_SuffixToSuppress.find(upperText) != g_SuffixToSuppress.end()) {
        isSuppressedSuffix = true;
      }

      // [FIX] Suffix Suppression REMOVED per user request
      // It was causing valid text to vanish.
      // We will rely on Longest Prefix Match to handle the main sentence
      // correcty. If small fragments ("Almagro.") remain, it's better than
      // losing valid text.

      /* REMOVED: Time/Location based suffix suppression
      if (!isSuppressedSuffix && text && strlen(text) < 40) {
        DWORD now = GetTickCount();
        if ((now - s_LastTranslatedTime) < 200 &&
            fabs(y - s_LastTranslatedY) < 5.0f) {
          isSuppressedSuffix = true;
          // ...
        }
      }
      */
    }
  }

  if (korean) {
    // Handle \n in Korean translation.
    // For multi-line body text in menus (e.g. Rorke Files), PRESERVE \n
    // so KoreanRenderer can render wrapped multi-line text.
    // For short texts, strip \n as before (single-line overlay).
    std::string korStr(korean);
    bool hasNewline = (korStr.find('\n') != std::string::npos ||
                       korStr.find('\r') != std::string::npos);
    if (hasNewline) {
      int nlCount = 0;
      for (char c : korStr) { if (c == '\n') nlCount++; }

      static std::string s_BodyTextBuffer;
      const bool preserveMultilineMenuBody =
          (nlCount >= 3) &&
          (s_InMenu || IsMenuActive() || IsKoreanMenuText(text));
      if (preserveMultilineMenuBody) {
        // Multi-line body text in menu (e.g. classified file body).
        // Preserve \n, only strip \r. Use dynamic buffer for long text.
        korStr.erase(std::remove(korStr.begin(), korStr.end(), '\r'),
                     korStr.end());
        s_BodyTextBuffer = korStr;
        translated = s_BodyTextBuffer.c_str();
      } else {
        // Short text or non-menu ??strip newlines as before.
        korStr = StripNewlines(korStr);
        int idx = g_StrippedIndex.fetch_add(1) % MAX_TRANSLATED_STRINGS;
        strncpy(g_StrippedStrings[idx], korStr.c_str(), MAX_TRANSLATED_LEN - 1);
        g_StrippedStrings[idx][MAX_TRANSLATED_LEN - 1] = '\0';
        translated = g_StrippedStrings[idx];
      }
    } else {
      translated = korean;
    }
  } else if (isSuppressedSuffix) {
    // This is a suffix fragment - suppress it by rendering space
    translated = " ";
  }

  // Button-layout right legend force-translation.
  // IMPORTANT: This runs BEFORE translationChanged gating, because some legend
  // fragments never hit dictionary/overlay and would otherwise stay English.
  // Scope is strict: "버튼 배치" header recently seen + right legend coordinate band.
  static DWORD s_ButtonLayoutHeaderTick = 0;
  if (text) {
    std::string headerProbe = NormalizeMenuText(std::string(text));
    std::string headerProbeUpper = ToUpperAscii(headerProbe);
    float headerScreenH = GetScreenHeightApprox();
    if (headerScreenH > 0.0f &&
        y <= headerScreenH * 0.28f &&
        (headerProbe == "버튼 배치" || headerProbeUpper == "BUTTON LAYOUT")) {
      s_ButtonLayoutHeaderTick = GetTickCount();
    }
  }
  if (!textAlreadyKorean && text) {
    const DWORD nowButtonLegend = GetTickCount();
    const bool inButtonLayoutContext =
        (s_ButtonLayoutHeaderTick != 0) &&
        ((nowButtonLegend - s_ButtonLayoutHeaderTick) < 1200);
    if (inButtonLayoutContext) {
      float legendW = GetScreenWidthApprox();
      float legendH = GetScreenHeightApprox();
      const bool inRightLegendBand =
          (legendW > 0.0f && legendH > 0.0f &&
           x >= legendW * 0.60f && x <= legendW * 0.98f &&
           y >= legendH * 0.16f && y <= legendH * 0.90f);
      const bool inLeftLegendBand =
          (legendW > 0.0f && legendH > 0.0f &&
           x >= legendW * 0.26f && x <= legendW * 0.66f &&
           y >= legendH * 0.16f && y <= legendH * 0.90f);
      if (inRightLegendBand || inLeftLegendBand) {
        std::string legendRaw = NormalizeMenuText(std::string(text));
        std::string legendUpper = ToUpperAscii(legendRaw);
        const char *forcedKor = nullptr;
        if (legendUpper.find("THROW") != std::string::npos &&
            legendUpper.find("FRAG") != std::string::npos) {
          forcedKor = "파편 수류탄";
        } else if (legendUpper.find("GRENADE") != std::string::npos) {
          forcedKor = "투척";
        } else if (legendUpper.find("TOGGLE") != std::string::npos &&
                   legendUpper.find("AIM") != std::string::npos) {
          forcedKor = "정조준";
        } else if (legendUpper.find("DOWN SIGHT") != std::string::npos) {
          forcedKor = "토글";
        } else if (legendUpper.find("MELEE") != std::string::npos &&
                   legendUpper.find("CHANGE") != std::string::npos) {
          forcedKor = "근접 공격/교체";
        } else if (legendUpper.find("MELEE") != std::string::npos &&
                   legendUpper.find("HOLD") != std::string::npos &&
                   legendUpper.find("BREATH") != std::string::npos) {
          forcedKor = "근접 공격/숨 참기";
        } else if (legendUpper.find("MELEE") != std::string::npos &&
                   legendUpper.find("HOLD") != std::string::npos) {
          forcedKor = "근접 공격";
        } else if (legendUpper.find("HOLD BREATH") != std::string::npos) {
          forcedKor = "숨 참기";
        }

        if (forcedKor) {
          static std::string s_ButtonLegendForcedText;
          s_ButtonLegendForcedText = forcedKor;
          translated = s_ButtonLegendForcedText.c_str();
          korean = translated;
        }
      }
    }
  }

  // Controls keybind value strings are runtime text and may bypass dictionary
  // mapping entirely. Promote them before translationChanged gating, but only
  // inside controls keybind pages to avoid false positives in mission menus.
  static DWORD s_ControlBindMoveHeaderTick = 0;
  static DWORD s_ControlBindActionHeaderTick = 0;
  static DWORD s_ControlBindLookHeaderTick = 0;
  static DWORD s_ControlBindChatHeaderTick = 0;
  constexpr DWORD kControlBindContextWindowMs = 1200;
  if (!textAlreadyKorean && text) {
    const std::string bindProbe = NormalizeMenuText(std::string(text));
    const std::string bindProbeUpper = ToUpperAscii(bindProbe);
    const float bindScreenW = GetScreenWidthApprox();
    const float bindScreenH = GetScreenHeightApprox();
    const bool inControlHeaderBand =
        (bindScreenW > 0.0f && bindScreenH > 0.0f &&
         x <= bindScreenW * 0.30f && y <= bindScreenH * 0.16f);
    if (inControlHeaderBand) {
      if (bindProbe == "이동" || bindProbeUpper == "MOVE" ||
          bindProbeUpper == "MOVEMENT") {
        s_ControlBindMoveHeaderTick = nowMenu;
      } else if (bindProbe == "동작" || bindProbeUpper == "ACTION" ||
                 bindProbeUpper == "ACTIONS") {
        s_ControlBindActionHeaderTick = nowMenu;
      } else if (bindProbe == "시선" || bindProbeUpper == "LOOK" ||
                 bindProbeUpper == "VIEW") {
        s_ControlBindLookHeaderTick = nowMenu;
      } else if (bindProbe == "채팅" || bindProbeUpper == "CHAT") {
        s_ControlBindChatHeaderTick = nowMenu;
      }
    }

    const bool inControlBindContext =
        (s_ControlBindMoveHeaderTick != 0 &&
         (nowMenu - s_ControlBindMoveHeaderTick) < kControlBindContextWindowMs) ||
        (s_ControlBindActionHeaderTick != 0 &&
         (nowMenu - s_ControlBindActionHeaderTick) < kControlBindContextWindowMs) ||
        (s_ControlBindLookHeaderTick != 0 &&
         (nowMenu - s_ControlBindLookHeaderTick) < kControlBindContextWindowMs) ||
        (s_ControlBindChatHeaderTick != 0 &&
         (nowMenu - s_ControlBindChatHeaderTick) < kControlBindContextWindowMs);
    const bool inBindValueBand =
        (bindScreenW > 0.0f && bindScreenH > 0.0f &&
         x >= bindScreenW * 0.24f && x <= bindScreenW * 0.72f &&
         y >= bindScreenH * 0.16f && y <= bindScreenH * 0.92f);

    auto IsSimpleBindTokenUpper = [](const std::string &u) -> bool {
      if (u.empty()) {
        return false;
      }
      if (u == "SPACE" || u == "CTRL" || u == "SHIFT" || u == "ALT" ||
          u == "TAB" || u == "ENTER" || u == "ESC" || u == "INS" ||
          u == "DEL" || u == "PGUP" || u == "PGDN" || u == "HOME" ||
          u == "END" || u == "MOUSE1" || u == "MOUSE2" || u == "MOUSE3" ||
          u == "MWHEELUP" || u == "MWHEELDOWN") {
        return true;
      }
      if (u.size() == 1 && std::isalnum((unsigned char)u[0])) {
        return true;
      }
      if (u.size() >= 2 && u.size() <= 4 && u[0] == 'F') {
        bool allDigits = true;
        for (size_t i = 1; i < u.size(); ++i) {
          if (!std::isdigit((unsigned char)u[i])) {
            allDigits = false;
            break;
          }
        }
        if (allDigits) {
          return true;
        }
      }
      return false;
    };

    auto IsCompositeBindValueUpper = [&](const std::string &u) -> bool {
      if (u.find(" OR ") == std::string::npos &&
          u.find(" KEY_OR ") == std::string::npos &&
          u.find(" 또는 ") == std::string::npos) {
        return false;
      }
      auto TrimAsciiToken = [](const std::string &s) -> std::string {
        size_t b = 0;
        while (b < s.size() && std::isspace((unsigned char)s[b])) {
          ++b;
        }
        size_t e = s.size();
        while (e > b && std::isspace((unsigned char)s[e - 1])) {
          --e;
        }
        return s.substr(b, e - b);
      };
      auto FindDelimiter = [&](size_t from, size_t &delimLen) -> size_t {
        delimLen = 0;
        size_t dOr = u.find(" OR ", from);
        size_t dKeyOr = u.find(" KEY_OR ", from);
        size_t dKor = u.find(" 또는 ", from);
        size_t d = std::string::npos;
        if (dOr != std::string::npos) {
          d = dOr;
        }
        if (dKeyOr != std::string::npos &&
            (d == std::string::npos || dKeyOr < d)) {
          d = dKeyOr;
        }
        if (dKor != std::string::npos &&
            (d == std::string::npos || dKor < d)) {
          d = dKor;
        }
        if (d == dOr) {
          delimLen = 4;
        } else if (d == dKeyOr || d == dKor) {
          delimLen = 8;
        }
        return d;
      };

      size_t delimLenProbe = 0;
      if (FindDelimiter(0, delimLenProbe) == std::string::npos ||
          delimLenProbe == 0) {
        return false;
      }

      size_t pos = 0;
      size_t parts = 0;
      while (pos <= u.size()) {
        size_t delimLen = 0;
        size_t d = FindDelimiter(pos, delimLen);
        std::string part =
            (d == std::string::npos) ? u.substr(pos) : u.substr(pos, d - pos);
        part = TrimAsciiToken(part);
        if (part.empty()) {
          return false;
        }
        const bool partIsBind =
            IsSimpleBindTokenUpper(part) || part.rfind("KEY_", 0) == 0 ||
            part.find("MOUSE") != std::string::npos ||
            part.find("WHEEL") != std::string::npos;
        if (!partIsBind) {
          return false;
        }
        ++parts;
        if (d == std::string::npos) {
          break;
        }
        pos = d + delimLen;
      }
      return parts >= 2;
    };

    const bool looksLikeBindValue =
        (bindProbe.find("할당되지 않음") != std::string::npos ||
         bindProbe.find("마우스") != std::string::npos ||
         IsCompositeBindValueUpper(bindProbeUpper) ||
         bindProbeUpper.find("MOUSE") != std::string::npos ||
         bindProbeUpper.find("WHEEL") != std::string::npos ||
         bindProbeUpper.find("NOT BOUND") != std::string::npos ||
         bindProbeUpper.find("UNBOUND") != std::string::npos ||
         bindProbeUpper.rfind("KEY_", 0) == 0 ||
         IsSimpleBindTokenUpper(bindProbeUpper));

    if (inControlBindContext && inBindValueBand && looksLikeBindValue) {
      std::string localizedBind =
          BindingResolver::LocalizeBindingDisplayText(bindProbe, true);
      if (!localizedBind.empty() && localizedBind != bindProbe) {
        static std::string s_ControlBindForcedText;
        s_ControlBindForcedText = localizedBind;
        translated = s_ControlBindForcedText.c_str();
        korean = translated;
      }
    }
  }
  if (text) {
    auto ResolveRorkeDetailPageIdByKey = [&](const std::string &key) -> int {
      // MENU_SP_CLASSIFIED_XX_CAPS  (prefix=19, suffix=5)
      if (key.size() > 24 &&
          key.compare(0, 19, "MENU_SP_CLASSIFIED_") == 0 &&
          key.compare(key.size() - 5, 5, "_CAPS") == 0) {
        int p = std::atoi(key.c_str() + 19);
        if (p >= 1 && p <= 21) return p;
      }
      // MENU_SP_VF_AUDIO_CAP_XX  (prefix=21)
      if (key.size() > 21 &&
          key.compare(0, 21, "MENU_SP_VF_AUDIO_CAP_") == 0) {
        int p = std::atoi(key.c_str() + 21);
        if (p >= 1 && p <= 21) return p;
      }
      // MENU_SP_VF_FILE_CAPTION_XX_NN  (prefix=24, page digits at 24-25)
      if (key.size() > 27 &&
          key.compare(0, 24, "MENU_SP_VF_FILE_CAPTION_") == 0) {
        int p = std::atoi(key.c_str() + 24);
        if (p >= 1 && p <= 21) return p;
      }
      return 0;
    };
    auto ResolveStaticRorkeHeaderId = [&](const std::string &key,
                                          const char *probeText) -> int {
      if (key.size() > 24 &&
          key.compare(0, 19, "MENU_SP_CLASSIFIED_") == 0 &&
          key.compare(key.size() - 5, 5, "_CAPS") == 0) {
        int p = std::atoi(key.c_str() + 19);
        if (p >= 1 && p <= 21) return p;
      }
      // Fast gate: only probe text if it could be a header (short text in
      // top area).  Body text, captions, navigation are skipped entirely.
      if (!probeText || !probeText[0]) return 0;
      size_t plen = strlen(probeText);
      if (plen < 8 || plen > 80) return 0;
      // Quick substring check before expensive NormalizeMenuText
      if (!strstr(probeText, "PROFILE") && !strstr(probeText, "OPERATION") &&
          !strstr(probeText, "PSYCH") &&
          !strstr(probeText, "\xED\x94\x84\xEB\xA1\x9C\xED\x95\x84") && // 프로필
          !strstr(probeText, "\xEC\x9E\x91\xEC\xA0\x84") &&              // 작전
          !strstr(probeText, "\xEC\x8B\xAC\xEB\xA6\xAC"))                // 심리
        return 0;
      std::string probe = NormalizeMenuText(std::string(probeText));
      std::string upper = ToUpperAscii(probe);
      if (probe == "심리 보고서 - 가브리엘 로크" ||
           probe == "심리 보고서-가브리엘 로크" ||
           upper == "PSYCH PROFILE - GABRIEL RORKE")
        return 1;
      if (probe == "작전: 반송" || probe == "작전:반송" ||
          upper == "OPERATION: RETURN TO SENDER")
        return 6;
      if (probe == "프로필: 일라이어스 T. 워커, 예비역 대위" ||
           (probe.find("프로필:") != std::string::npos &&
            probe.find("일라이어스") != std::string::npos) ||
           upper == "PROFILE: ELIAS T. WALKER, CPT. RET." ||
           upper == "PROFILE: ELIAS T. WALKER, CDR. USN RET.")
        return 19;
      if (probe == "프로필: 데이비드 \"헤쉬\" 워커, 중위" ||
           (probe.find("프로필:") != std::string::npos &&
            probe.find("헤쉬") != std::string::npos) ||
           upper == "PROFILE: DAVID \"HESH\" WALKER, LT.")
        return 20;
      if (probe == "프로필: 토마스 A. 메릭, 대위" ||
           (probe.find("프로필:") != std::string::npos &&
            probe.find("메릭") != std::string::npos) ||
           upper == "PROFILE: THOMAS A. MERRICK, CPT.")
        return 21;
      return 0;
    };

    std::string rorkeDetailKey = translatedKey;
    if (rorkeDetailKey.empty()) {
      ResolveKeyFromEnglishInternal(StripColorCodes(std::string(text)),
                                    rorkeDetailKey);
    }
    const int rorkeDetailPageId = ResolveRorkeDetailPageIdByKey(rorkeDetailKey);
    if (rorkeDetailPageId != 0) {
      KoreanRenderer_LatchRorkeDetailPage(rorkeDetailPageId);
      // --- Rorke detail page diagnostic (throttled 1/sec) ---
      {
        static std::atomic<int> s_rorkeAddCmdCount{0};
        static std::atomic<DWORD> s_rorkeLastLogTick{0};
        s_rorkeAddCmdCount.fetch_add(1, std::memory_order_relaxed);
        const DWORD rkNow = GetTickCount();
        const DWORD rkLast = s_rorkeLastLogTick.load(std::memory_order_relaxed);
        if (rkLast == 0 || (rkNow - rkLast) > 1000) {
          s_rorkeLastLogTick.store(rkNow, std::memory_order_relaxed);
          const int cnt = s_rorkeAddCmdCount.exchange(0, std::memory_order_relaxed);
          char rkBuf[256];
          sprintf_s(rkBuf,
                    "[RORKE-DETAIL-DIAG] pageId=%d addCmdCalls/sec=%d key=%s text=\"%.60s\"",
                    rorkeDetailPageId, cnt, rorkeDetailKey.c_str(),
                    text ? text : "(null)");
          LogToFile(rkBuf);
        }
      }
    }
    const int staticRorkeHeaderId =
        ResolveStaticRorkeHeaderId(rorkeDetailKey, text);
    if (rorkeDetailPageId == 0 && staticRorkeHeaderId != 0) {
      KoreanRenderer_LatchRorkeDetailPage(staticRorkeHeaderId);
    }
  }
  // Treat pointer-different but text-equivalent translations as unchanged.
  // This keeps native glyph/font path for footer key tokens like ESC/F1.
  bool translationChanged = false;
  if (translated && text && translated != text) {
    auto normalizeForCompare = [](const char *s) -> std::string {
      // NormalizeMenuText already handles StripVariantMarkers,
      // StripTrailingMenuIndex, and TrimSpaces — no need to repeat.
      return ToUpperAscii(NormalizeMenuText(std::string(s ? s : "")));
    };
    translationChanged = (normalizeForCompare(translated) !=
                          normalizeForCompare(text));
    if (!translationChanged) {
      translated = text;
    }
  }
  const bool needsKoreanOverlay = textAlreadyKorean;

  bool skipObjectiveStatusLegacyOverlay = false;
  if (!textAlreadyKorean && text && translationChanged) {
    std::string statusKey = translatedKey;
    if (statusKey.empty()) {
      std::string keyProbe = TrimSpaces(StripColorCodes(std::string(text)));
      if (!keyProbe.empty()) {
        ResolveKeyFromEnglishInternal(keyProbe, statusKey);
      }
    }
    // During any menu (pause, options, popups), the D3D11 overlay is disabled
    // (ObjUnified_Render returns early when hudPauseMenu==true), so objective/
    // status keys MUST be translated here in R_AddCmdDrawText.
    // Use s_InMenu (keyword-based with 2s decay) instead of
    // TextHook_IsPauseMenuLikely() which oscillates due to cl_paused jitter.
    if (!statusKey.empty() && !s_InMenu &&
        (IsObjectiveStatusMessageKeyName(statusKey) ||
         (RuntimeFlags_ObjectiveStatusEnableOsRevOnly() &&
          IsObjectiveOverlayKeyName(statusKey)))) {
      bool allowObserveOnlyBypass = true;
      if (statusKey == "CGAME_NOW_SAVING") {
        std::string recentProbeKey;
        const bool hasRecentProbe = TextHook_GetRecentObjectiveStatusProbeByKey(
            1200u, statusKey.c_str(), 0, recentProbeKey);
        const uint32_t trustedCfg =
            TextHook_GetTrustedObjectiveStatusCfgForKey(statusKey, 1200u);
        allowObserveOnlyBypass = (trustedCfg != 0) || hasRecentProbe;
      }
      if (allowObserveOnlyBypass) {
        skipObjectiveStatusLegacyOverlay = true;
        translated = text;
        const DWORD nowBypass = GetTickCount();
        if (kVerboseRuntimeLogs) {
          static std::unordered_map<std::string, DWORD> s_statusAddCmdBypassLogTick;
          auto itBypass = s_statusAddCmdBypassLogTick.find(statusKey);
          if (itBypass == s_statusAddCmdBypassLogTick.end() ||
              (nowBypass - itBypass->second) > 1500) {
            s_statusAddCmdBypassLogTick[statusKey] = nowBypass;
            uintptr_t modBaseNow = (uintptr_t)GetModuleHandleA(NULL);
            unsigned int callerOffsetNow = 0;
            if (modBaseNow) {
              callerOffsetNow = (unsigned int)((uintptr_t)_ReturnAddress() - modBaseNow);
            }
            char bbuf[320];
            sprintf_s(
                bbuf,
                "[STATUS-ADDCMD-BYPASS] key=%s caller=+0x%llX x=%.1f y=%.1f sx=%.3f src=observe_only",
                statusKey.c_str(), (unsigned long long)callerOffsetNow, x, y,
                xScale);
            LogToFile(bbuf);
          }
        }
      } else if (kVerboseRuntimeLogs) {
        static std::unordered_map<std::string, DWORD> s_statusAddCmdFallbackLogTick;
        const DWORD nowFallback = GetTickCount();
        auto itFallback = s_statusAddCmdFallbackLogTick.find(statusKey);
        if (itFallback == s_statusAddCmdFallbackLogTick.end() ||
            (nowFallback - itFallback->second) > 1500) {
          s_statusAddCmdFallbackLogTick[statusKey] = nowFallback;
          uintptr_t modBaseNow = (uintptr_t)GetModuleHandleA(NULL);
          unsigned int callerOffsetNow = 0;
          if (modBaseNow) {
            callerOffsetNow = (unsigned int)((uintptr_t)_ReturnAddress() - modBaseNow);
          }
          char fbuf[352];
          sprintf_s(
              fbuf,
              "[STATUS-ADDCMD-FALLBACK] key=%s caller=+0x%llX x=%.1f y=%.1f sx=%.3f src=legacy_overlay",
              statusKey.c_str(), (unsigned long long)callerOffsetNow, x, y,
              xScale);
          LogToFile(fbuf);
        }
      }
    }
  }

  // State update removed as Suffix Suppression is disabled
  /*
  if (korean) {
    s_LastTranslatedTime = GetTickCount();
    s_LastTranslatedY = y;
  }
  */

  // Runtime discovery logger is expensive in hot paths.
  // Keep it opt-in for debugging only.
  if (kEnableRuntimeTextCapture && text && text[0]) {
    uint32_t hash = HashString(text); // Hash ORIGINAL text
    if (!IsRecentlySeen(hash)) {
      MarkAsSeen(hash);
      int head = g_QueueHead.fetch_add(1) % MAX_QUEUE_SIZE;
      if (!g_Queue[head].ready.load()) {
        int i = 0;
        while (text[i] && i < MAX_STRING_LEN - 1) {
          g_Queue[head].text[i] = text[i];
          i++;
        }
        g_Queue[head].text[i] = '\0';
        g_Queue[head].ready.store(true);
      }
    }
  }

  // Friends menu: if detected, render English only to avoid clipping.
  bool friendsMenuActive = false;
  if (IsMenuActive() || s_InMenu || IsKoreanMenuText(text)) {
    static std::atomic<DWORD> s_LastFriendsMenuTime{0};
    DWORD nowFriends = GetTickCount();

    DWORD lastFriends = s_LastFriendsMenuTime.load();
    if (lastFriends != 0 && (nowFriends - lastFriends) <= 180) {
      friendsMenuActive = true;
    }

    float screenHeight = GetScreenHeightApprox();
    float screenWidth = GetScreenWidthApprox();
    float centerX = screenWidth * 0.5f;
    float textWidthApprox = MeasureEnglishWidthForLayout(
        text, font, font ? (float)font->fontHeight : 0.0f, xScale,
        "friends_probe");
    float textCenterX = (textWidthApprox > 0.0f) ? (x + textWidthApprox * 0.5f) : x;
    float dxCenter = textCenterX - centerX;
    if (dxCenter < 0.0f) dxCenter = -dxCenter;
    bool nearTopCenter = (dxCenter <= screenWidth * 0.35f);

    std::string textNorm =
        NormalizeMenuText(text ? std::string(text) : std::string());
    std::string textUpper = ToUpperAscii(textNorm);
    std::string translatedNorm =
        NormalizeMenuText(translated ? std::string(translated) : std::string());
    std::string translatedUpper = ToUpperAscii(translatedNorm);

    const bool hasFriendsToken =
        textUpper.find("FRIENDS") != std::string::npos ||
        textNorm.find("친구") != std::string::npos ||
        translatedUpper.find("FRIENDS") != std::string::npos ||
        translatedNorm.find("친구") != std::string::npos;
    const bool hasPlayingToken =
        textUpper.find("PLAYING") != std::string::npos ||
        textNorm.find("플레이 중") != std::string::npos ||
        translatedUpper.find("PLAYING") != std::string::npos ||
        translatedNorm.find("플레이 중") != std::string::npos;
    const bool hasGhostsTitleToken =
        textUpper.find("CALL OF DUTY: GHOSTS") != std::string::npos ||
        translatedUpper.find("CALL OF DUTY: GHOSTS") != std::string::npos;

    if ((hasFriendsToken || hasPlayingToken || hasGhostsTitleToken) &&
        nearTopCenter && y <= screenHeight * 0.45f) {
      s_LastFriendsMenuTime.store(nowFriends);
      friendsMenuActive = true;
    }
  }
  if (friendsMenuActive) {
    translated = text;
    goto render_original;
  }

  // Popup tail fragments can arrive untranslated ("checkpoint.", etc.).
  // Suppress them by popup position/token regardless of translation hit.
  {
    bool menuContextNow = IsMenuActive() || s_InMenu || IsKoreanMenuText(text);
    if (menuContextNow && text) {
      float screenHeight = GetScreenHeightApprox();
      float screenWidth = GetScreenWidthApprox();
      float centerY = screenHeight * 0.5f;
      float centerX = screenWidth * 0.5f;
      float textWidthPopup = MeasureEnglishWidthForLayout(
          text, font, font ? (float)font->fontHeight : 0.0f, xScale,
          "popup_tail");
      float textCenterXPopup =
          (textWidthPopup > 0.0f) ? (x + textWidthPopup * 0.5f) : x;
      float dxPopup = textCenterXPopup - centerX;
      if (dxPopup < 0.0f)
        dxPopup = -dxPopup;
      float dxPopupAnchor = x - centerX;
      if (dxPopupAnchor < 0.0f)
        dxPopupAnchor = -dxPopupAnchor;

      bool nearPopupY =
          (y >= (centerY - screenHeight * 0.35f) &&
           y <= (centerY + screenHeight * 0.35f));
      bool nearPopupCenter =
          ((dxPopup <= screenWidth * 0.22f) ||
           (dxPopupAnchor <= screenWidth * 0.22f));

      if (nearPopupCenter && nearPopupY) {
        std::string cleanPopup = NormalizeMenuText(std::string(text));
        std::string upperPopup = ToUpperAscii(cleanPopup);
        const bool isLastCheckpointHeader =
            (upperPopup == "LAST CHECKPOINT");
        bool quitCheckpointTail =
            (upperPopup == "CHECKPOINT." || upperPopup == "CHECKPOINT" ||
             upperPopup == "SINCE YOUR LAST CHECKPOINT." ||
             upperPopup == "SINCE YOUR LAST CHECKPOINT" ||
             upperPopup == "SINCE YOUR LAST" ||
             (upperPopup.find("CHECKPOINT") != std::string::npos &&
              upperPopup.find("LAST CHECKPOINT") == std::string::npos) ||
             upperPopup.find("SINCE YOUR LAST CHECKPOINT") != std::string::npos);
        if (quitCheckpointTail && !isLastCheckpointHeader) {
          if (Original_R_AddCmdDrawText) {
            Original_R_AddCmdDrawText(" ", maxChars, font, x, y, xScale, yScale,
                                      rotation, color, style);
          }
          return;
        }
        bool controllerPopupTail =
            (upperPopup.find("LIKE TO SWITCH") != std::string::npos &&
             (upperPopup.find("KEYBOARD") != std::string::npos ||
              upperPopup.find("GAMEPAD") != std::string::npos)) ||
            (upperPopup.find("SWITCH TO THE") != std::string::npos &&
             (upperPopup.find("KEYBOARD") != std::string::npos ||
              upperPopup.find("GAMEPAD") != std::string::npos)) ||
            (upperPopup == "SCHEME?" || upperPopup == "CONTROL SCHEME?" ||
             upperPopup == "SCHEME." || upperPopup == "CONTROL SCHEME.");
        if (controllerPopupTail) {
          if (Original_R_AddCmdDrawText) {
            Original_R_AddCmdDrawText(" ", maxChars, font, x, y, xScale, yScale,
                                      rotation, color, style);
          }
          return;
        }
      }
    }
  }

  bool likelyMenuContextForRorke =
      IsMenuActive() || s_InMenu || IsKoreanMenuText(text) ||
      (text && (strstr(text, "MENU") || strstr(text, "PAUSE") ||
                strstr(text, "options")));
  const float rorkeScreenWidth = GetScreenWidthApprox();
  const float rorkeScreenHeight = GetScreenHeightApprox();
  // Fast "RORKE FILES" header detection — avoid NormalizeMenuText + ToUpperAscii
  // by checking raw text with strstr/strcmp (covers English + Korean).
  if (text && rorkeScreenHeight > 0.0f && y <= rorkeScreenHeight * 0.16f) {
    bool isRorkeFilesHeaderNow = false;
    if (strstr(text, "RORKE FILES") || strstr(text, "Rorke Files"))
      isRorkeFilesHeaderNow = true;
    else if (strstr(text, "\xEB\xA1\x9C\xED\x81\xAC")) // "로크" UTF-8
      isRorkeFilesHeaderNow = true;
    if (isRorkeFilesHeaderNow) {
      KoreanRenderer_ClearRorkeDetailState();
    }
  }
  // === RENDER QUEUE (Green Box / Overlay) ===
  if (skipObjectiveStatusLegacyOverlay) {
    goto render_original;
  }

  // If we found a translation, suppress original and queue overlay
  if (translationChanged || needsKoreanOverlay) {
    // === CONTEXT DETECTION (for Houston suppression) ===
    static DWORD s_LastMenuTime = 0;
    bool isMenuElement = false;

    // Broad Key Check for menu context
    if (text) {
      if (strstr(text, "MENU") || strstr(text, "PAUSE") ||
          strstr(text, "options"))
        isMenuElement = true;
    }

    if (isMenuElement) {
      s_LastMenuTime = GetTickCount();
    }

    bool menuContext =
        s_InMenu || isMenuElement || IsKoreanMenuText(text);

    // Popup-only translation: when a center popup is active, keep outside text in English.
    if (menuContext) {
      static std::atomic<DWORD> s_LastPopupStrongHeaderTime{0};
      static std::atomic<DWORD> s_LastPopupYesNoTime{0};
      static std::atomic<DWORD> s_LastPopupQuestionTime{0};
      static bool s_OptimalVideoPopupLatch = false;
      float screenHeight = GetScreenHeightApprox();
      float screenWidth = GetScreenWidthApprox();
      float centerY = screenHeight * 0.5f;
      float centerX = screenWidth * 0.5f;
      float popupBandX = screenWidth * 0.17f;
      float popupBandY = screenHeight * 0.35f;

      float textWidthApproxPopup = MeasureEnglishWidthForLayout(
          text, font, font ? (float)font->fontHeight : 0.0f, xScale,
          "popup_probe");
      float textCenterXPopup =
          (textWidthApproxPopup > 0.0f) ? (x + textWidthApproxPopup * 0.5f) : x;
      float dxPopup = textCenterXPopup - centerX;
      if (dxPopup < 0.0f)
        dxPopup = -dxPopup;
      float dxPopupAnchor = x - centerX;
      if (dxPopupAnchor < 0.0f)
        dxPopupAnchor = -dxPopupAnchor;
      std::string cleanedPopup = NormalizeMenuText(std::string(text ? text : ""));
      std::string cleanedPopupUpper = ToUpperAscii(cleanedPopup);
      const bool popupDescText = IsPopupDescText(cleanedPopup);
      const bool popupQuestionText = IsPopupQuestionHeader(cleanedPopup);
      const bool popupYesNoText = IsYesNoText(cleanedPopup);
      bool nearPopupY =
          (y >= (centerY - popupBandY) && y <= (centerY + popupBandY));
      bool isLeftMenuColumn = (textCenterXPopup <= screenWidth * 0.46f);
      bool nearPopupCenter =
          !isLeftMenuColumn &&
          ((dxPopup <= popupBandX) || (dxPopupAnchor <= popupBandX)) &&
          nearPopupY;
      bool nearPopupCenterLoose =
          !isLeftMenuColumn &&
          ((dxPopup <= popupBandX * 1.25f) ||
           (dxPopupAnchor <= popupBandX * 1.25f)) &&
          nearPopupY;
      if ((popupDescText || popupQuestionText || popupYesNoText) && nearPopupY) {
        if (dxPopup <= popupBandX * 1.05f ||
            dxPopupAnchor <= popupBandX * 1.05f)
          nearPopupCenter = true;
        if (dxPopup <= popupBandX * 1.25f ||
            dxPopupAnchor <= popupBandX * 1.25f)
          nearPopupCenterLoose = true;
      }
      DWORD nowPopup = GetTickCount();
      bool isQuitPopupHeader =
          cleanedPopupUpper == "ARE YOU SURE YOU WANT TO QUIT?" ||
          cleanedPopupUpper == "ARE YOU SURE YOU WANT TO EXIT?" ||
          cleanedPopupUpper == "SAVE AND QUIT" ||
          (cleanedPopupUpper.find("ARE YOU SURE") != std::string::npos &&
           (cleanedPopupUpper.find("QUIT") != std::string::npos ||
            cleanedPopupUpper.find("SAVE") != std::string::npos ||
            cleanedPopupUpper.find("EXIT") != std::string::npos));
      bool isStrongHeader =
          IsCenterPopupHeaderOnly(cleanedPopup) ||
          isQuitPopupHeader ||
          cleanedPopupUpper == "MISSION SELECT" ||
          cleanedPopupUpper == "SELECT DIFFICULTY" ||
          cleanedPopupUpper == "LOWER DIFFICULTY" ||
          cleanedPopupUpper == "DECREASE DIFFICULTY";
      bool isOptimalVideoPopupHeader =
          (cleanedPopupUpper == "OPTIMAL VIDEO");
      if (isStrongHeader && nearPopupCenterLoose) {
        s_LastPopupStrongHeaderTime.store(nowPopup);
      }
      if (isOptimalVideoPopupHeader && nearPopupCenterLoose)
        s_OptimalVideoPopupLatch = true;
      if (isStrongHeader && nearPopupCenterLoose && !isOptimalVideoPopupHeader)
        s_OptimalVideoPopupLatch = false;
      if (nearPopupCenter) {
        if (IsYesNoText(cleanedPopup))
          s_LastPopupYesNoTime.store(nowPopup);
        if (IsPopupQuestionHeader(cleanedPopup))
          s_LastPopupQuestionTime.store(nowPopup);
      }
      DWORD lastStrong = s_LastPopupStrongHeaderTime.load();
      DWORD lastYesNo = s_LastPopupYesNoTime.load();
      DWORD lastQuestion = s_LastPopupQuestionTime.load();
      // Use 500ms window (was 300ms) to prevent brief false-negatives
      // when engine skips drawing popup header text on some frames.
      // Without this, popupActive flickers off → "PAUSED" enters Korean
      // overlay path → English suppressed with " " → cache miss → blank.
      bool popupActive =
          (lastStrong != 0 && (nowPopup - lastStrong) <= 500) ||
          ((lastQuestion != 0 && (nowPopup - lastQuestion) <= 500) &&
           (lastYesNo != 0 && (nowPopup - lastYesNo) <= 500));
      if (!popupActive)
        s_OptimalVideoPopupLatch = false;

      bool hasPrecisePopup = false;
      bool inPrecisePopup = false;
      if (popupActive) {
        extern bool KoreanRenderer_GetPopupBounds(float &, float &, float &,
                                                  float &);
        float precTopY, precBottomY, precCenterX, precBandX;
        if (KoreanRenderer_GetPopupBounds(precTopY, precBottomY, precCenterX,
                                          precBandX)) {
          hasPrecisePopup = true;
          float dxPrec = textCenterXPopup - precCenterX;
          if (dxPrec < 0.0f)
            dxPrec = -dxPrec;
          float dxPrecAnchor = x - precCenterX;
          if (dxPrecAnchor < 0.0f)
            dxPrecAnchor = -dxPrecAnchor;
          inPrecisePopup =
              (y >= precTopY && y <= precBottomY) &&
              ((dxPrec <= precBandX) || (dxPrecAnchor <= precBandX * 1.05f));
        }
      }

      // Immediate popup-fragment handling (no time gating):
      // - Quit warning body
      // - Restart warning body
      // - trailing "checkpoint" fragment suppression
      {
        static bool s_QuitPopupBodySeen = false;
        const bool quitBodyLead =
            (cleanedPopupUpper.find("IF YOU QUIT NOW") != std::string::npos);
        const bool quitBodyMiddle =
            (cleanedPopupUpper.find("YOU WILL LOSE ANY PROGRESS") !=
             std::string::npos);
        const bool checkpointTailFragment =
            (cleanedPopupUpper == "CHECKPOINT." ||
             cleanedPopupUpper == "CHECKPOINT" ||
             cleanedPopupUpper == "SINCE YOUR LAST CHECKPOINT." ||
             cleanedPopupUpper == "SINCE YOUR LAST CHECKPOINT" ||
             cleanedPopupUpper == "SINCE YOUR LAST" ||
             cleanedPopupUpper.find("SINCE YOUR LAST CHECKPOINT") !=
                 std::string::npos);
        const bool missionSelectOverwriteBody =
            (cleanedPopupUpper.find("OVERWRITE") != std::string::npos &&
             cleanedPopupUpper.find("CURRENT MISSION") != std::string::npos);
        const bool restartBodyLead =
            (cleanedPopupUpper.find("IF YOU RESTART NOW") != std::string::npos);
        const bool restartBodyTail =
            (cleanedPopupUpper.find("ANY PROGRESS THAT YOU HAVE MADE") !=
                 std::string::npos ||
             cleanedPopupUpper == "IN THIS MISSION." ||
             cleanedPopupUpper == "IN THIS MISSION" ||
             cleanedPopupUpper.find("YOU HAVE MADE IN THIS MISSION") !=
                 std::string::npos);

        if (nearPopupY && quitBodyLead) {
          static const std::string s_QuitPopupBodyWrappedFixed =
              "지금 종료하면 마지막 체크포인트 이후의\n진행 상황을 잃게 됩니다.";
          translated = s_QuitPopupBodyWrappedFixed.c_str();
          s_QuitPopupBodySeen = true;
        } else if (nearPopupY && restartBodyLead) {
          static const std::string s_RestartPopupBodyWrappedFixed =
              "지금 다시 시작하면 잃게 되는 것:\n이 임무에서 이루어 놓은 진행 상황.";
          translated = s_RestartPopupBodyWrappedFixed.c_str();
          s_QuitPopupBodySeen = false;
        } else if (nearPopupY && missionSelectOverwriteBody) {
          static const std::string s_MissionSelectOverwriteWrapped =
              "현재 임무의 진행 상황을 덮어씁니다.\n계속하시겠습니까?";
          translated = s_MissionSelectOverwriteWrapped.c_str();
          s_QuitPopupBodySeen = false;
        } else if (nearPopupY &&
                   (quitBodyMiddle || restartBodyTail ||
                    (checkpointTailFragment && s_QuitPopupBodySeen))) {
          translated = " ";
          goto render_original;
        }
        if (IsCenterPopupHeaderOnly(cleanedPopup) &&
            cleanedPopupUpper != "ARE YOU SURE YOU WANT TO QUIT?" &&
            cleanedPopupUpper != "ARE YOU SURE YOU WANT TO EXIT?") {
          s_QuitPopupBodySeen = false;
        }
      }

      // Controller popup: force line break + suppress English tail fragment.
      if (nearPopupY) {
        if (cleanedPopupUpper.find("GAMEPAD ATTACHED") != std::string::npos) {
          if (cleanedPopupUpper.find("DO NOT") != std::string::npos) {
            // MENU_NO_CONTROLLER_INITIAL
            static const std::string s_NoControllerInitialWrapped =
                "게임패드가 연결되어 있지 않습니다.\n마우스 및 키보드 제어 방식으로 전환하시겠습니까?";
            translated = s_NoControllerInitialWrapped.c_str();
          } else {
            // MENU_CONTROLLER_INITIAL
            static const std::string s_ControllerInitialWrapped =
                "게임패드가 연결되어 있습니다.\n게임패드 제어 방식으로 전환하시겠습니까?";
            translated = s_ControllerInitialWrapped.c_str();
          }
        } else if (cleanedPopupUpper.find("RECONNECT YOUR CONTROLLER") != std::string::npos) {
          // PLATFORM_CONTROLLER_DISCONNECTED
          static const std::string s_ControllerDisconnectedWrapped =
              "컨트롤러를 다시 연결하거나\n마우스 및 키보드 조작 방식으로 전환하십시오.";
          translated = s_ControllerDisconnectedWrapped.c_str();
        } else if ((cleanedPopupUpper.find("LIKE TO SWITCH") != std::string::npos ||
                    cleanedPopupUpper.find("SWITCH TO THE") != std::string::npos) &&
                   (cleanedPopupUpper.find("KEYBOARD") != std::string::npos ||
                    cleanedPopupUpper.find("GAMEPAD CONTROL") != std::string::npos)) {
          // Tail fragment suppression
          translated = " ";
          goto render_original;
        }
      }

      if (popupActive && s_OptimalVideoPopupLatch) {
        const bool isControlNoiseBehindPopup =
            (cleanedPopupUpper == "GAMEPAD" ||
             cleanedPopupUpper == "GAME PAD" ||
             cleanedPopupUpper == "RESET CONTROLS");
        if (isControlNoiseBehindPopup) {
          translated = " ";
          goto render_original;
        }
      }

      // Lower-difficulty popup body: wrap known variants to keep vertical balance.
      if (nearPopupY &&
          cleanedPopupUpper.find("LOWER THE DIFFICULTY") != std::string::npos &&
          cleanedPopupUpper.find("ALL MISSIONS") != std::string::npos) {
        if (cleanedPopupUpper.find("FROM REGULAR TO RECRUIT") !=
                std::string::npos ||
            cleanedPopupUpper.find("FROM NORMAL TO EASY") !=
                std::string::npos) {
          static const std::string s_LowerDiffRegToEasy =
              "모든 임무의 난이도를 보통에서 쉬움으로\n낮추시겠습니까?";
          translated = s_LowerDiffRegToEasy.c_str();
        } else if (cleanedPopupUpper.find("FROM VETERAN TO HARDENED") !=
                       std::string::npos ||
                   cleanedPopupUpper.find("FROM VETERAN TO HARD") !=
                       std::string::npos) {
          static const std::string s_LowerDiffVetToHard =
              "모든 임무의 난이도를 베테랑에서 어려움으로\n낮추시겠습니까?";
          translated = s_LowerDiffVetToHard.c_str();
        } else if (cleanedPopupUpper.find("FROM HARDENED TO REGULAR") !=
                       std::string::npos ||
                   cleanedPopupUpper.find("FROM HARD TO NORMAL") !=
                       std::string::npos) {
          static const std::string s_LowerDiffHardToReg =
              "모든 임무의 난이도를 어려움에서 보통으로\n낮추시겠습니까?";
          translated = s_LowerDiffHardToReg.c_str();
        }
      }

      // Fallback wrap for lower-difficulty popup body:
      // some builds feed a different English template, so the branch above
      // may not trigger even though Korean translation is present.
      if (popupActive && nearPopupY && translated && translationChanged) {
        std::string tr = std::string(translated);
        std::string trClean = NormalizeMenuText(tr);
        if (trClean.find("모든 임무의 난이도를") != std::string::npos &&
            trClean.find("낮추시겠습니까") != std::string::npos &&
            tr.find('\n') == std::string::npos) {
          static std::string s_LowerDifficultyWrapped;
          s_LowerDifficultyWrapped = tr;
          size_t splitPos =
              s_LowerDifficultyWrapped.find(" 낮추시겠습니까");
          if (splitPos != std::string::npos) {
            s_LowerDifficultyWrapped.replace(
                splitPos, strlen(" 낮추시겠습니까"), "\n낮추시겠습니까");
          } else {
            splitPos = s_LowerDifficultyWrapped.find("낮추시겠습니까");
            if (splitPos != std::string::npos && splitPos > 0) {
              s_LowerDifficultyWrapped.insert(splitPos, "\n");
            }
          }
          translated = s_LowerDifficultyWrapped.c_str();
        }
      }

      if (popupActive) {
        std::string popupKey;
        bool objectiveLikeText = false;
        if (!cleanedPopup.empty() &&
            ResolveKeyFromEnglishInternal(cleanedPopup, popupKey)) {
          objectiveLikeText = IsObjectiveLikeMenuKeyName(popupKey);
        }
        if (objectiveLikeText) {
          translated = text;
          goto render_original;
        }

        // When a center popup is active, do not translate background options
        // headers rendered behind the popup frame.
        const bool isUnderlyingOptionsHeader =
            IsAdvancedVideoHeaderText(cleanedPopup) ||
            cleanedPopupUpper == "OPTIONS" ||
            cleanedPopupUpper == "VIDEO OPTIONS" ||
            cleanedPopupUpper == "AUDIO OPTIONS" ||
            cleanedPopupUpper == "CONTROL OPTIONS";
        if (isUnderlyingOptionsHeader && !isStrongHeader) {
          translated = text;
          goto render_original;
        }

        bool insidePopupRegion = hasPrecisePopup ? inPrecisePopup : nearPopupCenter;
        if (cleanedPopupUpper.find("IF YOU QUIT NOW") != std::string::npos ||
            cleanedPopupUpper.find("ANY PROGRESS") != std::string::npos) {
          insidePopupRegion = true;
        }
        if (!hasPrecisePopup && screenWidth > 0.0f &&
            x > screenWidth * 0.56f) {
          // Coarse popup center checks can accidentally include the right
          // objective panel. Keep that panel in English while popup is active.
          insidePopupRegion = false;
        }
        if (!insidePopupRegion) {
          bool allowPopupDesc = false;
          if (!hasPrecisePopup) {
            const bool likelyRightPanel =
                (screenWidth > 0.0f && x > screenWidth * 0.56f);
            allowPopupDesc =
                (!likelyRightPanel && !isLeftMenuColumn && nearPopupY &&
                 popupDescText);
          }
          if (!allowPopupDesc) {
            translated = text;
            goto render_original;
          }
        }
      }
    }

    // === TARGETED SUPPRESSION (Houston subtitle in menu) ===
    if (translated) {
      if (strstr(translated, "Houston") ||
          strstr(translated, "\xED\x9C\xB4\xEC\x8A\xA4\xED\x84\xB4") ||
          strstr(translated, "\xED\x8F\xAC\xEC\xB0\xA9")) { // ??揶?        // Use memory flags OR global menu flag
        if (IsMenuActive() || s_InMenu)
          return;
      }
      if (IsMenuActive() || s_InMenu) {
        auto itKor =
            g_KeyToKorean.find("SUBTITLE_CLOCKWORK_DIZ_NORTHERNRIDGE32");
        if (itKor != g_KeyToKorean.end()) {
          const std::string &targetKor = itKor->second;
          if (!targetKor.empty() && strstr(translated, targetKor.c_str()))
            return;
        }
      }
    }

    float fontHeight = 0.0f;
    if (font) {
      fontHeight = (float)font->fontHeight;
    }

    // === MENU FONT SIZE BUCKETING (3 tiers) ===
    float screenHeight = GetScreenHeightApprox();
    float screenWidth = GetScreenWidthApprox();
    float textWidthApprox = MeasureEnglishWidthForLayout(
        text, font, fontHeight, xScale, "menu_scale");
    float scaleMul =
        GetMenuFontScaleMul(fontHeight, menuContext, translated, x, y,
                            screenWidth, screenHeight, textWidthApprox);
    if (menuContext && text) {
      static DWORD s_RorkeFilesHeaderSeenTick = 0;
      std::string scaleProbe = NormalizeMenuText(std::string(text));
      std::string scaleProbeUpper = ToUpperAscii(scaleProbe);
      const bool rorkeHeaderSignal =
          (screenHeight > 0.0f && y <= screenHeight * 0.28f &&
           (scaleProbe.find("로크 파일") != std::string::npos ||
            scaleProbeUpper.find("RORKE FILES") != std::string::npos));
      if (rorkeHeaderSignal) {
        s_RorkeFilesHeaderSeenTick = nowMenu;
      }
      const bool inRorkeFilesHeaderContext =
          (s_RorkeFilesHeaderSeenTick != 0 &&
           (nowMenu - s_RorkeFilesHeaderSeenTick) <= 1500);
      const bool isRorkeProfileHeader =
          (scaleProbe.find("프로필:") != std::string::npos ||
           scaleProbeUpper.find("PROFILE:") != std::string::npos);
      const bool fixRorkeMetaSize =
          inRorkeFilesHeaderContext && !isRorkeProfileHeader &&
          (scaleProbe.find("U.S.S. 리버레이터, 태평양") != std::string::npos ||
           scaleProbe.find("일라이어스 T. 워커") != std::string::npos ||
           scaleProbeUpper.find("U.S.S. LIBERATOR, PACIFIC OCEAN") !=
               std::string::npos ||
           scaleProbeUpper.find("ELIAS T. WALKER") != std::string::npos);
      if (fixRorkeMetaSize) {
        scaleMul = kMenuScaleNormal;
      }
    }
    float xScaleAdjusted = xScale * scaleMul;
    bool isSmallMenu = menuContext && (scaleMul <= kMenuScaleSmall + 0.001f);

    // DEBUG: log menu font bucketing (throttled)
    static int menuScaleLogCount = 0;
    if (menuScaleLogCount < 40 && menuContext) {
      menuScaleLogCount++;
      char logBuf[256];
      sprintf_s(logBuf,
                "[MENU SCALE] text=\"%.30s\" fontH=%.1f xScale=%.2f mul=%.2f",
                text ? text : "(null)", fontHeight, xScale, scaleMul);
      LogToFile(logBuf);
    }

    // === MEASURE ENGLISH WIDTH ACCURATELY ===
    // Use game's R_TextWidth if available for pixel-perfect alignment.
    // CRITICAL: Use original xScale (NOT xScaleAdjusted) because the engine
    // positions text based on original scale. If we used xScaleAdjusted
    // (which includes our scaleMul), the computed right edge would overshoot
    // the engine's actual position — causing right-aligned items (headers etc.)
    // to fly off-screen when scaleMul > 1.0.
    float finalWidthEng = MeasureEnglishWidthForLayout(
        text, font, fontHeight, xScale, "menu_queue");

    // === USE GAME'S ORIGINAL STYLE - NO FORCED CHANGES ===
    // The game's style determines alignment. Trust it completely.
    int effectiveStyle = style;

    // DEBUG: Log style values for menu items
    static int styleLogCount = 0;
    if (styleLogCount < 50 && fontHeight >= 20.0f) {
      styleLogCount++;
      char logBuf[512];
      sprintf(logBuf,
              "[STYLE DEBUG] text=\"%.30s\" style=%d x=%.1f y=%.1f fontH=%.1f "
              "engW=%.1f",
              translated, style, x, y, fontHeight, finalWidthEng);
      LogToFile(logBuf);
    }

    // === HOVER COLOR TRACKING ===
    // Track color changes for hover effects
    float finalColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (color) {
      finalColor[0] = color[0];
      finalColor[1] = color[1];
      finalColor[2] = color[2];
      finalColor[3] = color[3];

      // Check if color changed (hover detection)
      uint32_t colorHash = HashTextPosition(text, x, y);
      bool colorChanged =
          UpdateColorCache(colorHash, color[0], color[1], color[2], color[3]);

      // Log hover color changes for debugging (first few only)
      static int hoverLogCount = 0;
      if (colorChanged && hoverLogCount < 20) {
        hoverLogCount++;
        char logBuf[256];
        sprintf(logBuf,
                "[HOVER] Color changed: text=\"%.20s\" "
                "color=(%.2f,%.2f,%.2f,%.2f)",
                text, color[0], color[1], color[2], color[3]);
        LogToFile(logBuf);
      }
    }

    // === QUEUE FOR KOREAN OVERLAY ===
    bool disableShadow = IsMenuActive() || s_InMenu;

    // Resolve key bindings in translated text (e.g., [{+changezoom}])
    std::string translatedStr = translated ? std::string(translated) : "";
    if (menuContext) {
      translatedStr = StripVariantMarkers(translatedStr);
      translatedStr = StripTrailingMenuIndex(translatedStr);
      translatedStr = TrimSpaces(translatedStr);
    }
    std::string resolvedText =
        BindingResolver::ResolveBindingsInText(translatedStr,
                                               text ? std::string(text) : "");

    // Controls/compass key hint oddity: keep pure key for patterns like
    // "N Book", "S South", or standalone compass words.
    // Direction hints must stay as N/S/W/E, not localized cardinal words.
    if (menuContext && text) {
      std::string englishMenuNorm = NormalizeMenuText(std::string(text));
      std::string englishMenuUpper = ToUpperAscii(englishMenuNorm);
      std::string englishMenuComp;
      englishMenuComp.reserve(englishMenuUpper.size());
      bool prevSpace = false;
      for (char c : englishMenuUpper) {
        bool isWs = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (isWs) {
          if (!prevSpace) {
            englishMenuComp.push_back(' ');
            prevSpace = true;
          }
        } else {
          englishMenuComp.push_back(c);
          prevSpace = false;
        }
      }
      while (!englishMenuComp.empty() && englishMenuComp.front() == ' ')
        englishMenuComp.erase(englishMenuComp.begin());
      while (!englishMenuComp.empty() && englishMenuComp.back() == ' ')
        englishMenuComp.pop_back();

      std::string resolvedMenuNorm =
          NormalizeMenuText(!resolvedText.empty() ? resolvedText : std::string(text));
      std::string resolvedMenuUpper = ToUpperAscii(resolvedMenuNorm);
      std::string resolvedMenuComp;
      resolvedMenuComp.reserve(resolvedMenuUpper.size());
      prevSpace = false;
      for (char c : resolvedMenuUpper) {
        bool isWs = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (isWs) {
          if (!prevSpace) {
            resolvedMenuComp.push_back(' ');
            prevSpace = true;
          }
        } else {
          resolvedMenuComp.push_back(c);
          prevSpace = false;
        }
      }
      while (!resolvedMenuComp.empty() && resolvedMenuComp.front() == ' ')
        resolvedMenuComp.erase(resolvedMenuComp.begin());
      while (!resolvedMenuComp.empty() && resolvedMenuComp.back() == ' ')
        resolvedMenuComp.pop_back();

      auto HasComp = [](const std::string &hay, const std::string &needle) -> bool {
        return !needle.empty() && hay.find(needle) != std::string::npos;
      };
      const bool inLayoutRightLegendBand =
          (screenWidth > 0.0f && screenHeight > 0.0f &&
           x >= screenWidth * 0.62f && x <= screenWidth * 0.96f &&
           y >= screenHeight * 0.20f && y <= screenHeight * 0.88f);
      const bool inLayoutCenterLabelBand =
          (screenWidth > 0.0f && screenHeight > 0.0f &&
           x >= screenWidth * 0.22f && x <= screenWidth * 0.72f &&
           y >= screenHeight * 0.14f && y <= screenHeight * 0.60f);

      // Hard overrides for controller/layout screens with unstable key joins.
      if (englishMenuComp == "STICK LAYOUT" ||
          englishMenuComp == "THUMBSTICK LAYOUT" ||
          resolvedMenuComp == "STICK LAYOUT" ||
          resolvedMenuComp == "THUMBSTICK LAYOUT" ||
          HasComp(englishMenuComp, "STICK LAYOUT") ||
          HasComp(resolvedMenuComp, "STICK LAYOUT")) {
        resolvedText = "스틱 배치";
      } else if (englishMenuComp == "BUTTON LAYOUT" ||
                 resolvedMenuComp == "BUTTON LAYOUT" ||
                 HasComp(englishMenuComp, "BUTTON LAYOUT") ||
                 HasComp(resolvedMenuComp, "BUTTON LAYOUT")) {
        resolvedText = "버튼 배치";
      } else if (englishMenuComp == "NOM4D" || resolvedMenuComp == "NOM4D" ||
                 HasComp(englishMenuComp, "NOM4D") ||
                 HasComp(resolvedMenuComp, "NOM4D")) {
        resolvedText = "노매드 전술";
      } else if (englishMenuComp == "NOMAD TACTICAL" ||
                 resolvedMenuComp == "NOMAD TACTICAL" ||
                 HasComp(englishMenuComp, "NOMAD TACTICAL") ||
                 HasComp(resolvedMenuComp, "NOMAD TACTICAL")) {
        resolvedText = "노매드 전술";
      } else if (englishMenuComp == "LEG. SP NO CLICK SWAP" ||
                 resolvedMenuComp == "LEG. SP NO CLICK SWAP" ||
                 HasComp(englishMenuComp, "LEG. SP NO CLICK SWAP") ||
                 HasComp(resolvedMenuComp, "LEG. SP NO CLICK SWAP")) {
        resolvedText = "레거시 SP 클릭 교체 없음";
      } else if (englishMenuComp == "SP NO CLICK SWAP" ||
                 resolvedMenuComp == "SP NO CLICK SWAP" ||
                 HasComp(englishMenuComp, "SP NO CLICK SWAP") ||
                 HasComp(resolvedMenuComp, "SP NO CLICK SWAP")) {
        resolvedText = "SP 클릭 교체 없음";
      } else if (englishMenuComp == "TOGGLE AIM DOWN SIGHT" ||
                 resolvedMenuComp == "TOGGLE AIM DOWN SIGHT" ||
                 HasComp(englishMenuComp, "TOGGLE AIM DOWN SIGHT") ||
                 HasComp(resolvedMenuComp, "TOGGLE AIM DOWN SIGHT")) {
        resolvedText = "정조준 토글";
      } else if ((englishMenuComp == "TOGGLE AIM" ||
                  resolvedMenuComp == "TOGGLE AIM" ||
                  HasComp(englishMenuComp, "TOGGLE AIM") ||
                  HasComp(resolvedMenuComp, "TOGGLE AIM")) &&
                 inLayoutRightLegendBand) {
        resolvedText = "정조준";
      } else if ((englishMenuComp == "DOWN SIGHT" ||
                  resolvedMenuComp == "DOWN SIGHT" ||
                  HasComp(englishMenuComp, "DOWN SIGHT") ||
                  HasComp(resolvedMenuComp, "DOWN SIGHT")) &&
                 inLayoutRightLegendBand) {
        resolvedText = "토글";
      } else if (englishMenuComp == "MELEE/HOLD BREATH" ||
                 englishMenuComp == "MELEE / HOLD BREATH" ||
                 resolvedMenuComp == "MELEE/HOLD BREATH" ||
                 resolvedMenuComp == "MELEE / HOLD BREATH" ||
                 HasComp(englishMenuComp, "MELEE/HOLD BREATH") ||
                 HasComp(englishMenuComp, "MELEE / HOLD BREATH") ||
                 HasComp(resolvedMenuComp, "MELEE/HOLD BREATH") ||
                 HasComp(resolvedMenuComp, "MELEE / HOLD BREATH")) {
        resolvedText = "근접 공격/숨 참기";
      } else if ((englishMenuComp == "MELEE/HOLD" ||
                  englishMenuComp == "MELEE / HOLD" ||
                  resolvedMenuComp == "MELEE/HOLD" ||
                  resolvedMenuComp == "MELEE / HOLD" ||
                  HasComp(englishMenuComp, "MELEE/HOLD") ||
                  HasComp(englishMenuComp, "MELEE / HOLD") ||
                  HasComp(resolvedMenuComp, "MELEE/HOLD") ||
                  HasComp(resolvedMenuComp, "MELEE / HOLD")) &&
                 inLayoutRightLegendBand) {
        resolvedText = "근접 공격";
      } else if ((englishMenuComp == "HOLD BREATH" ||
                  englishMenuComp == "BREATH" ||
                  resolvedMenuComp == "HOLD BREATH" ||
                  resolvedMenuComp == "BREATH" ||
                  HasComp(englishMenuComp, "HOLD BREATH") ||
                  HasComp(resolvedMenuComp, "HOLD BREATH")) &&
                 inLayoutRightLegendBand) {
        resolvedText = "숨 참기";
      } else if (englishMenuComp == "MELEE/CHANGE" ||
                 englishMenuComp == "MELEE / CHANGE" ||
                 resolvedMenuComp == "MELEE/CHANGE" ||
                 resolvedMenuComp == "MELEE / CHANGE" ||
                 HasComp(englishMenuComp, "MELEE/CHANGE") ||
                 HasComp(englishMenuComp, "MELEE / CHANGE") ||
                 HasComp(resolvedMenuComp, "MELEE/CHANGE") ||
                 HasComp(resolvedMenuComp, "MELEE / CHANGE")) {
        resolvedText = "근접 공격/교체";
      } else if (englishMenuComp == "THROW FRAG GRENADE" ||
                 resolvedMenuComp == "THROW FRAG GRENADE" ||
                 HasComp(englishMenuComp, "THROW FRAG GRENADE") ||
                 HasComp(resolvedMenuComp, "THROW FRAG GRENADE")) {
        resolvedText = "파편 수류탄 투척";
      } else if (englishMenuComp == "THROW FRAG" ||
                 resolvedMenuComp == "THROW FRAG" ||
                 HasComp(englishMenuComp, "THROW FRAG") ||
                 HasComp(resolvedMenuComp, "THROW FRAG")) {
        resolvedText = "파편 수류탄";
      } else if ((englishMenuComp == "GRENADE" || resolvedMenuComp == "GRENADE") &&
                 inLayoutRightLegendBand) {
        // Button-layout right legend second-line token.
        resolvedText = "투척";
      } else if ((englishMenuComp == "TACTICAL EQUIPMENT" ||
                  resolvedMenuComp == "TACTICAL EQUIPMENT" ||
                  HasComp(englishMenuComp, "TACTICAL EQUIPMENT") ||
                  HasComp(resolvedMenuComp, "TACTICAL EQUIPMENT") ||
                  resolvedMenuNorm == "전술 장비") &&
                 inLayoutCenterLabelBand) {
        // Layout diagram label: avoid awkward wrapped split ("전술" + "장비").
        resolvedText = "전술장비";
      } else if ((englishMenuComp == "TACTICAL" || resolvedMenuComp == "TACTICAL" ||
                  HasComp(englishMenuComp, "TACTICAL") ||
                  HasComp(resolvedMenuComp, "TACTICAL")) &&
                 inLayoutCenterLabelBand) {
        resolvedText = "전술장비";
      } else if ((englishMenuComp == "EQUIPMENT" ||
                  resolvedMenuComp == "EQUIPMENT") &&
                 inLayoutCenterLabelBand) {
        resolvedText.clear();
      } else if (englishMenuComp == "ROTATE LEFT" ||
                 resolvedMenuComp == "ROTATE LEFT" ||
                 HasComp(englishMenuComp, "ROTATE LEFT") ||
                 HasComp(resolvedMenuComp, "ROTATE LEFT")) {
        resolvedText = "왼쪽 회전";
      } else if (englishMenuComp == "ROTATE RIGHT" ||
                 resolvedMenuComp == "ROTATE RIGHT" ||
                 HasComp(englishMenuComp, "ROTATE RIGHT") ||
                 HasComp(resolvedMenuComp, "ROTATE RIGHT")) {
        resolvedText = "오른쪽 회전";
      } else if (englishMenuComp == "ROTATE LEFT/RIGHT" ||
                 resolvedMenuComp == "ROTATE LEFT/RIGHT" ||
                 HasComp(englishMenuComp, "ROTATE LEFT/RIGHT") ||
                 HasComp(resolvedMenuComp, "ROTATE LEFT/RIGHT")) {
        resolvedText = "왼쪽/오른쪽 회전";
      }

      if (englishMenuUpper == "N" || englishMenuUpper == "S" ||
          englishMenuUpper == "W" || englishMenuUpper == "E") {
        resolvedText = englishMenuUpper;
      } else if (englishMenuUpper == "NORTH" || englishMenuUpper == "SOUTH" ||
                 englishMenuUpper == "WEST" || englishMenuUpper == "EAST") {
        resolvedText = englishMenuUpper.substr(0, 1);
      }

      size_t sp = englishMenuUpper.find(' ');
      if (sp != std::string::npos) {
        std::string keyTok = englishMenuNorm.substr(0, sp);
        std::string keyTokUpper = englishMenuUpper.substr(0, sp);
        std::string tailUpper = englishMenuUpper.substr(sp + 1);
        if (keyTokUpper.size() == 1 &&
            ((keyTokUpper[0] >= 'A' && keyTokUpper[0] <= 'Z') ||
             (keyTokUpper[0] >= '0' && keyTokUpper[0] <= '9')) &&
            (tailUpper == "BOOK" || tailUpper == "NORTH" ||
             tailUpper == "SOUTH" || tailUpper == "WEST" ||
             tailUpper == "EAST")) {
          resolvedText = keyTok;
        }
      }

      // Final sanitize: strip unsafe controls, but keep gamepad-icon control
      // markers (0x01~0x18) used by UI strings.
      if (!resolvedText.empty()) {
        std::string cleanResolved;
        cleanResolved.reserve(resolvedText.size());
        for (unsigned char uc : resolvedText) {
          const bool isPadIconToken = (uc >= 0x01 && uc <= 0x18);
          if ((uc >= 0x20 && uc != 0x7F) || uc >= 0x80 || uc == '\n' ||
              isPadIconToken)
            cleanResolved.push_back((char)uc);
        }
        resolvedText = TrimSpaces(cleanResolved);
      }
    }

    // Footer shortcut labels can arrive as combined runtime strings
    // (e.g. "F1 Friends ... 1"), while dictionary entries may only map the
    // label word ("Friends" -> "친구"). Preserve leading/trailing key tokens.
    if (menuContext && text && screenHeight > 0.0f && y >= screenHeight * 0.66f &&
        !resolvedText.empty()) {
      auto IsAsciiAlnum = [](char c) -> bool {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
               (c >= 'a' && c <= 'z');
      };
      auto IsLikelyShortcutToken = [&](const std::string &token) -> bool {
        if (token.empty())
          return false;
        std::string up = ToUpperAscii(token);
        // Mission titles like "End Of The Line" can appear in lower area.
        // Only treat END as a key token when the source token is all-caps.
        if (up == "END" && token != up) {
          return false;
        }
        if (up == "ESC" || up == "TAB" || up == "ENTER" || up == "SPACE" ||
            up == "PGUP" || up == "PGDN" || up == "HOME" || up == "END" ||
            up == "INS" || up == "DEL" || up == "LMB" || up == "RMB" ||
            up == "MMB" || up == "MOUSE1" || up == "MOUSE2" ||
            up == "MOUSE3") {
          return true;
        }
        if (up.size() == 1 && IsAsciiAlnum(up[0]))
          return true;
        if (up.size() >= 2 && up.size() <= 3 && up[0] == 'F') {
          bool allDigits = true;
          for (size_t i = 1; i < up.size(); ++i) {
            if (!(up[i] >= '0' && up[i] <= '9')) {
              allDigits = false;
              break;
            }
          }
          if (allDigits)
            return true;
        }
        return false;
      };

      std::string englishMenu = NormalizeMenuText(std::string(text));
      std::string resolvedMenu = NormalizeMenuText(resolvedText);
      std::string resolvedUpper = ToUpperAscii(resolvedMenu);

      if (englishMenu.size() <= 64) {
        size_t firstSep = englishMenu.find(' ');
        std::string firstTok =
            (firstSep == std::string::npos) ? englishMenu
                                            : englishMenu.substr(0, firstSep);
        std::string firstTokUp = ToUpperAscii(firstTok);
        if (IsLikelyShortcutToken(firstTok) &&
            resolvedUpper.find(firstTokUp) == std::string::npos) {
          resolvedText = firstTok + " " + resolvedText;
          resolvedUpper = ToUpperAscii(NormalizeMenuText(resolvedText));
        }

        size_t endPos = englishMenu.size();
        while (endPos > 0 &&
               std::isspace(static_cast<unsigned char>(englishMenu[endPos - 1]))) {
          endPos--;
        }
        size_t lastStart = endPos;
        while (lastStart > 0 &&
               !std::isspace(static_cast<unsigned char>(englishMenu[lastStart - 1]))) {
          lastStart--;
        }
        if (lastStart < endPos) {
          std::string lastTok = englishMenu.substr(lastStart, endPos - lastStart);
          std::string lastTokUp = ToUpperAscii(lastTok);
          if (IsLikelyShortcutToken(lastTok) &&
              resolvedUpper.find(lastTokUp) == std::string::npos) {
            resolvedText += " " + lastTok;
            resolvedUpper = ToUpperAscii(NormalizeMenuText(resolvedText));
          }
        }

        size_t digitEnd = endPos;
        size_t digitStart = digitEnd;
        while (digitStart > 0 &&
               std::isdigit(static_cast<unsigned char>(englishMenu[digitStart - 1]))) {
          digitStart--;
        }
        if (digitStart < digitEnd &&
            (digitStart == 0 || std::isspace(static_cast<unsigned char>(englishMenu[digitStart - 1])))) {
          std::string tailDigits =
              englishMenu.substr(digitStart, digitEnd - digitStart);
          if (resolvedUpper.find(tailDigits) == std::string::npos) {
            resolvedText += " " + tailDigits;
          }
        }
      }
    }

    // DEBUG: Log QueueText calls (first 100 only)
    static int queueLogCount = 0;
    if (queueLogCount < 100) {
      queueLogCount++;
      std::string textPreview =
          text ? Utf8Preview(std::string(text), 30) : "(null)";
      std::string transPreview = Utf8Preview(resolvedText, 30);
      LogToFile("[QUEUE] eng=\"" + textPreview + "\" kor=\"" + transPreview +
                "\" x=" + std::to_string((int)x) + " y=" + std::to_string((int)y));
    }

    // Pass original xScale (before GetMenuFontScaleMul adjustment) for clipping
    // logic This allows distinguishing real headers (large original scale) from
    // scroll content
    // Tag as menu source so KoreanRenderer caches these items across frames
    // where R_AddCmdDrawText doesn't fire (in-game pause UI update frequency).
    extern std::atomic<bool> g_bQueueFromMenu;
    g_bQueueFromMenu.store(true);

    std::string gameplayAlignKey = translatedKey;
    if (gameplayAlignKey.empty() && text && text[0]) {
      ResolveKeyFromEnglishInternal(StripColorCodes(std::string(text)),
                                    gameplayAlignKey);
    }
    if (!gameplayAlignKey.empty() && text && text[0]) {
      TextHook_ResolveLocalizedTemplateArgs(gameplayAlignKey,
                                            std::string(text), resolvedText);
    }

    // Right-side pause menu objectives: force right-alignment so Korean
    // text right-edge matches the engine's layout.  ONLY active when the
    // pause menu is on-screen (detected by "MISSION OBJECTIVES" header).
    // Other right-side screens (Rorke file detail, etc.) are NOT affected.
    int menuForceAlign = -1; // auto (default)
    if (!menuContext &&
        ShouldForceCenterAlignGameplayTranslation(gameplayAlignKey, resolvedText)) {
      menuForceAlign = 2;
      if (screenWidth > 0.0f) {
        x = screenWidth * 0.5f;
        finalWidthEng = 0.0f;
      }

      static std::unordered_map<std::string, DWORD> s_forceCenterHudLogTick;
      const DWORD nowCenter = GetTickCount();
      auto itCenter = s_forceCenterHudLogTick.find(gameplayAlignKey);
      if (itCenter == s_forceCenterHudLogTick.end() ||
          (nowCenter - itCenter->second) > 1200) {
        s_forceCenterHudLogTick[gameplayAlignKey] = nowCenter;
        char cbuf[384];
        sprintf_s(cbuf,
                  "[HUD-CENTER-FORCE] key=%s x=%.2f y=%.2f engW=%.2f style=%d",
                  gameplayAlignKey.c_str(), x, y, finalWidthEng, style);
        LogToFile(cbuf);
      }
    }
    float menuMaxWidthPx = 0.0f; // 0 = no wrapping override

    std::string alignProbeText =
        NormalizeMenuText(std::string(translated ? translated : ""));
    std::string alignProbeTextRaw =
        NormalizeMenuText(text ? std::string(text) : std::string());
    if (alignProbeText.empty() && !alignProbeTextRaw.empty()) {
      alignProbeText = alignProbeTextRaw;
    }

    const DWORD nowMenuAlignTick = GetTickCount();
    static bool g_pauseMenuContext = false;
    static DWORD g_optionsHeaderSeenTick = 0;
    static DWORD g_advancedVideoSeenTick = 0;
    static DWORD g_gamepadHeaderSeenTick = 0;
    static DWORD g_stickLayoutSeenTick = 0;
    static DWORD g_buttonLayoutSeenTick = 0;
    static DWORD g_movementHeaderSeenTick = 0;
    static DWORD g_controlsBindHeaderSeenTick = 0;

    if (menuContext) {
      std::string alignProbeUpper = ToUpperAscii(alignProbeText);
      std::string alignProbeRawUpper = ToUpperAscii(alignProbeTextRaw);
      const bool pauseHeaderSignal =
          (!alignProbeText.empty() && IsPauseRootHeaderText(alignProbeText)) ||
          (!alignProbeTextRaw.empty() && IsPauseRootHeaderText(alignProbeTextRaw));
      const bool pauseMenuSignal =
          pauseHeaderSignal ||
          (!alignProbeText.empty() && IsPauseMenuText(alignProbeText)) ||
          (!alignProbeTextRaw.empty() && IsPauseMenuText(alignProbeTextRaw));
      const bool optionsHeaderSignal =
          (!alignProbeText.empty() && IsOptionsHeaderText(alignProbeText)) ||
          (!alignProbeTextRaw.empty() && IsOptionsHeaderText(alignProbeTextRaw));
      const bool gamepadHeaderSignal =
          ((!alignProbeText.empty() &&
            (alignProbeText == "게임패드" || alignProbeUpper == "GAMEPAD")) ||
           (!alignProbeTextRaw.empty() &&
            (alignProbeTextRaw == "게임패드" || alignProbeRawUpper == "GAMEPAD")));
      const bool buttonLayoutHeaderSignal =
          ((!alignProbeText.empty() &&
            (alignProbeText == "버튼 배치" ||
             alignProbeUpper == "BUTTON LAYOUT")) ||
           (!alignProbeTextRaw.empty() &&
            (alignProbeTextRaw == "버튼 배치" ||
             alignProbeRawUpper == "BUTTON LAYOUT")));
      const bool stickLayoutHeaderSignal =
          ((!alignProbeText.empty() &&
            (alignProbeText == "스틱 배치" ||
             alignProbeUpper == "STICK LAYOUT" ||
             alignProbeUpper == "THUMBSTICK LAYOUT")) ||
           (!alignProbeTextRaw.empty() &&
            (alignProbeTextRaw == "스틱 배치" ||
             alignProbeRawUpper == "STICK LAYOUT" ||
             alignProbeRawUpper == "THUMBSTICK LAYOUT")));
      const bool movementHeaderSignal =
          ((!alignProbeText.empty() &&
            (alignProbeText == "이동" || alignProbeUpper == "MOVE" ||
             alignProbeUpper == "MOVEMENT")) ||
           (!alignProbeTextRaw.empty() &&
            (alignProbeTextRaw == "이동" || alignProbeRawUpper == "MOVE" ||
             alignProbeRawUpper == "MOVEMENT")));
      const bool actionHeaderSignal =
          ((!alignProbeText.empty() &&
            (alignProbeText == "동작" || alignProbeUpper == "ACTION" ||
             alignProbeUpper == "ACTIONS")) ||
           (!alignProbeTextRaw.empty() &&
            (alignProbeTextRaw == "동작" || alignProbeRawUpper == "ACTION" ||
             alignProbeRawUpper == "ACTIONS")));
      const bool lookHeaderSignal =
          ((!alignProbeText.empty() &&
            (alignProbeText == "시선" || alignProbeUpper == "LOOK" ||
             alignProbeUpper == "VIEW")) ||
           (!alignProbeTextRaw.empty() &&
            (alignProbeTextRaw == "시선" || alignProbeRawUpper == "LOOK" ||
             alignProbeRawUpper == "VIEW")));
      const bool controlsBindHeaderSignal =
          (movementHeaderSignal || actionHeaderSignal || lookHeaderSignal);
      std::string sceneProbe =
          !alignProbeTextRaw.empty() ? alignProbeTextRaw : alignProbeText;
      std::string sceneProbeUpper = ToUpperAscii(sceneProbe);
      const bool missionSelectLikeSignal =
          (sceneProbe == "임무 선택" || sceneProbe == "캠페인" ||
           sceneProbe == "멀티플레이어" || sceneProbe == "스쿼드" ||
           sceneProbe == "익스팅션" ||
           sceneProbeUpper == "MISSION SELECT" ||
           sceneProbeUpper == "CAMPAIGN" ||
           sceneProbeUpper == "MULTIPLAYER" || sceneProbeUpper == "SQUADS" ||
           sceneProbeUpper == "EXTINCTION");
      if (optionsHeaderSignal && y <= screenHeight * 0.28f) {
        g_optionsHeaderSeenTick = nowMenuAlignTick;
        g_pauseMenuContext = false;
        // Invalidate cached bindings so changes made in options are picked
        // up when gameplay resumes.
        BindingResolver::InvalidateBindings();
      }
      if (pauseMenuSignal) {
        g_pauseMenuContext = true;
      }
      if (missionSelectLikeSignal) {
        g_pauseMenuContext = false;
      }
      if (gamepadHeaderSignal && y <= screenHeight * 0.28f) {
        g_gamepadHeaderSeenTick = nowMenuAlignTick;
      }
      if (stickLayoutHeaderSignal && y <= screenHeight * 0.28f) {
        g_stickLayoutSeenTick = nowMenuAlignTick;
      }
      if (buttonLayoutHeaderSignal && y <= screenHeight * 0.28f) {
        g_buttonLayoutSeenTick = nowMenuAlignTick;
      }
      if (movementHeaderSignal && x <= screenWidth * 0.30f &&
          y <= screenHeight * 0.16f) {
        g_movementHeaderSeenTick = nowMenuAlignTick;
      }
      if (controlsBindHeaderSignal && x <= screenWidth * 0.30f &&
          y <= screenHeight * 0.16f) {
        g_controlsBindHeaderSeenTick = nowMenuAlignTick;
      }
    }

    const bool optionsHeaderRecent =
        (g_optionsHeaderSeenTick != 0) &&
        ((nowMenuAlignTick - g_optionsHeaderSeenTick) < 650);
    bool inPauseMenu = g_pauseMenuContext;

    if (menuContext && IsAdvancedVideoHeaderText(alignProbeText) &&
        y <= screenHeight * 0.22f) {
      g_advancedVideoSeenTick = nowMenuAlignTick;
    }
    bool inAdvancedVideoMenu =
        (g_advancedVideoSeenTick != 0 &&
         (nowMenuAlignTick - g_advancedVideoSeenTick) < 650);
    bool inGamepadMenu =
        (g_gamepadHeaderSeenTick != 0 &&
         (nowMenuAlignTick - g_gamepadHeaderSeenTick) < 650);
    bool inStickLayoutMenu =
        (g_stickLayoutSeenTick != 0 &&
         (nowMenuAlignTick - g_stickLayoutSeenTick) < 650);
    bool inButtonLayoutMenu =
        (g_buttonLayoutSeenTick != 0 &&
         (nowMenuAlignTick - g_buttonLayoutSeenTick) < 650);
    bool inLayoutDiagramMenu = (inButtonLayoutMenu || inStickLayoutMenu);
    bool inMovementHeaderMenu =
        (g_movementHeaderSeenTick != 0 &&
         (nowMenuAlignTick - g_movementHeaderSeenTick) < 650);
    bool inControlsBindHeaderMenu =
        (g_controlsBindHeaderSeenTick != 0 &&
         (nowMenuAlignTick - g_controlsBindHeaderSeenTick) < 650);
    const bool inControlsBindMenuEffective = inControlsBindHeaderMenu;

    // Controls keybind pages ("이동/동작/시선"): localize value-column bind
    // strings, including composite forms like "Left Mouse or Right Mouse".
    // preserveSingleAsciiAlpha=true keeps single-letter binds (e.g. H) as-is.
    if (menuContext && screenWidth > 0.0f && screenHeight > 0.0f &&
        !resolvedText.empty()) {
      const bool inBindValueBand =
          (x >= screenWidth * 0.26f && x <= screenWidth * 0.70f &&
           y >= screenHeight * 0.16f && y <= screenHeight * 0.92f);
      if (inBindValueBand) {
        auto IsSimpleBindTokenUpper = [](const std::string &u) -> bool {
          if (u.empty()) {
            return false;
          }
          if (u == "SPACE" || u == "CTRL" || u == "SHIFT" || u == "ALT" ||
              u == "TAB" || u == "ENTER" || u == "ESC" || u == "INS" ||
              u == "DEL" || u == "PGUP" || u == "PGDN" || u == "HOME" ||
              u == "END" || u == "MOUSE1" || u == "MOUSE2" ||
              u == "MOUSE3" || u == "MWHEELUP" || u == "MWHEELDOWN") {
            return true;
          }
          if (u.size() == 1 &&
              std::isalnum((unsigned char)u[0])) {
            return true;
          }
          if (u.size() >= 2 && u.size() <= 4 && u[0] == 'F') {
            bool allDigits = true;
            for (size_t i = 1; i < u.size(); ++i) {
              if (!std::isdigit((unsigned char)u[i])) {
                allDigits = false;
                break;
              }
            }
            if (allDigits) {
              return true;
            }
          }
          return false;
        };

        auto IsMouseWheelValueTokenUpper = [](const std::string &u) -> bool {
          return (u == "LEFT MOUSE" || u == "RIGHT MOUSE" ||
                  u == "MIDDLE MOUSE" || u == "MOUSE LEFT" ||
                  u == "MOUSE RIGHT" || u == "MOUSE MIDDLE" ||
                  u == "MOUSE1" || u == "MOUSE2" || u == "MOUSE3" ||
                  u == "MWHEELUP" || u == "MWHEELDOWN" || u == "WHEEL UP" ||
                  u == "WHEEL DOWN");
        };

        auto IsCompositeBindValueUpper = [&](const std::string &u) -> bool {
          auto TrimAsciiToken = [](const std::string &s) -> std::string {
            size_t b = 0;
            while (b < s.size() && std::isspace((unsigned char)s[b])) {
              ++b;
            }
            size_t e = s.size();
            while (e > b && std::isspace((unsigned char)s[e - 1])) {
              --e;
            }
            return s.substr(b, e - b);
          };
          auto FindDelimiter = [&](size_t from, size_t &delimLen) -> size_t {
            delimLen = 0;
            size_t dOr = u.find(" OR ", from);
            size_t dKeyOr = u.find(" KEY_OR ", from);
            size_t dKor = u.find(" 또는 ", from);
            size_t d = std::string::npos;
            if (dOr != std::string::npos) {
              d = dOr;
            }
            if (dKeyOr != std::string::npos &&
                (d == std::string::npos || dKeyOr < d)) {
              d = dKeyOr;
            }
            if (dKor != std::string::npos &&
                (d == std::string::npos || dKor < d)) {
              d = dKor;
            }
            if (d == dOr) {
              delimLen = 4;
            } else if (d == dKeyOr) {
              delimLen = 8;
            } else if (d == dKor) {
              delimLen = 8;
            }
            return d;
          };

          size_t delimLenProbe = 0;
          if (FindDelimiter(0, delimLenProbe) == std::string::npos ||
              delimLenProbe == 0) {
            return false;
          }

          size_t pos = 0;
          size_t parts = 0;
          while (pos <= u.size()) {
            size_t delimLen = 0;
            size_t d = FindDelimiter(pos, delimLen);
            std::string part = (d == std::string::npos)
                                   ? u.substr(pos)
                                   : u.substr(pos, d - pos);
            part = TrimAsciiToken(part);
            if (part.empty()) {
              return false;
            }
            const bool partIsBind =
                IsSimpleBindTokenUpper(part) || part.rfind("KEY_", 0) == 0 ||
                IsMouseWheelValueTokenUpper(part);
            if (!partIsBind) {
              return false;
            }
            ++parts;
            if (d == std::string::npos) {
              break;
            }
            pos = d + delimLen;
          }
          return parts >= 2;
        };

        auto IsExplicitMouseWheelValueText =
            [&](const std::string &probe, const std::string &probeUpper)
            -> bool {
          if (probe == "마우스 왼쪽" || probe == "마우스 오른쪽" ||
              probe == "마우스 가운데" || probe == "휠 위로" ||
              probe == "휠 아래로") {
            return true;
          }
          return IsMouseWheelValueTokenUpper(probeUpper);
        };

        auto LooksLikeBindValueText = [&](const std::string &probe,
                                          const std::string &probeUpper) -> bool {
          if (probe.empty()) {
            return false;
          }
          return (probe.find("할당되지 않음") != std::string::npos ||
                  IsCompositeBindValueUpper(probeUpper) ||
                  IsExplicitMouseWheelValueText(probe, probeUpper) ||
                  probe.find("또는") != std::string::npos ||
                  probeUpper.find("NOT BOUND") != std::string::npos ||
                  probeUpper.find("UNBOUND") != std::string::npos ||
                  probeUpper.rfind("KEY_", 0) == 0 ||
                  IsSimpleBindTokenUpper(probeUpper));
        };

        std::string bindSource = resolvedText;
        std::string resolvedNorm = NormalizeMenuText(resolvedText);
        std::string resolvedUpper = ToUpperAscii(resolvedNorm);
        const bool resolvedLooksLikeBind =
            LooksLikeBindValueText(resolvedNorm, resolvedUpper);
        bool rawLooksLikeBind = false;
        if (text && text[0]) {
          std::string rawBind = NormalizeMenuText(std::string(text));
          std::string rawBindUpper = ToUpperAscii(rawBind);
          rawLooksLikeBind = LooksLikeBindValueText(rawBind, rawBindUpper);
          if (rawLooksLikeBind) {
            bindSource = rawBind;
          }
        }

        const bool shouldLocalizeBindValue =
            (inControlsBindHeaderMenu || rawLooksLikeBind ||
             resolvedLooksLikeBind);
        if (shouldLocalizeBindValue) {
          std::string localizedBindValue =
              BindingResolver::LocalizeBindingDisplayText(bindSource, true);
          if (!localizedBindValue.empty()) {
            resolvedText = localizedBindValue;
          }
        }
      }
    }

    // Stick/Button-layout legend: hard fallback translation for split/variant
    // English draws that bypass normal map matching.
    if (menuContext && inLayoutDiagramMenu && screenWidth > 0.0f &&
        screenHeight > 0.0f && text) {
      std::string rescueRaw = NormalizeMenuText(std::string(text));
      std::string rescueUpper = ToUpperAscii(rescueRaw);
      const bool inRightLegendBand =
          (x >= screenWidth * 0.60f && x <= screenWidth * 0.98f &&
           y >= screenHeight * 0.16f && y <= screenHeight * 0.90f);
      const bool inLeftLegendBand =
          (x >= screenWidth * 0.26f && x <= screenWidth * 0.66f &&
           y >= screenHeight * 0.16f && y <= screenHeight * 0.90f);
      if (inRightLegendBand || inLeftLegendBand) {
        if (rescueUpper.find("THROW") != std::string::npos &&
            rescueUpper.find("FRAG") != std::string::npos) {
          resolvedText = "파편 수류탄";
        } else if (rescueUpper == "GRENADE" ||
                   rescueUpper.find("GRENADE") != std::string::npos) {
          resolvedText = "투척";
        } else if (rescueUpper.find("TOGGLE") != std::string::npos &&
                   rescueUpper.find("AIM") != std::string::npos) {
          resolvedText = "정조준";
        } else if (rescueUpper.find("DOWN SIGHT") != std::string::npos) {
          resolvedText = "토글";
        } else if (rescueUpper.find("MELEE/HOLD") != std::string::npos ||
                   rescueUpper.find("MELEE / HOLD") != std::string::npos) {
          resolvedText = "근접 공격";
        } else if (rescueUpper.find("HOLD BREATH") != std::string::npos) {
          resolvedText = "숨 참기";
        } else if (rescueUpper.find("MELEE/CHANGE") != std::string::npos ||
                   rescueUpper.find("MELEE / CHANGE") != std::string::npos) {
          resolvedText = "근접 공격/교체";
        }
      }
    }

    // Credits pause menu left-column actions ("Resume Credits", "Quit")
    // are right-aligned in the original layout.
    if (menuContext && screenWidth > 0.0f && screenHeight > 0.0f &&
        x < screenWidth * 0.42f && y < screenHeight * 0.30f &&
        IsCreditsPauseMenuItemText(alignProbeText)) {
      menuForceAlign = 1;
    }

    // Rorke Files top-left labels should stay left-anchored.
    if (menuContext && screenWidth > 0.0f && screenHeight > 0.0f &&
        x <= screenWidth * 0.45f && y <= screenHeight * 0.42f) {
      std::string leftProbe =
          !alignProbeTextRaw.empty() ? alignProbeTextRaw : alignProbeText;
      std::string leftProbeUpper = ToUpperAscii(leftProbe);
      const bool isClassifiedLabel =
          (leftProbe == "기밀" || leftProbeUpper == "CLASSIFIED");
      const bool isNoDupLabel =
          (leftProbe.find("무단 복제 금지") != std::string::npos ||
           leftProbeUpper.find("NO UNAUTHORIZED DUPLICATION") !=
               std::string::npos);
      if (isClassifiedLabel || isNoDupLabel) {
        menuForceAlign = 0;
      }
    }
    if (menuContext && screenWidth > 0.0f && screenHeight > 0.0f &&
        x >= screenWidth * 0.55f && y <= screenHeight * 0.42f) {
      std::string rightProbe =
          !alignProbeTextRaw.empty() ? alignProbeTextRaw : alignProbeText;
      std::string rightProbeUpper = ToUpperAscii(rightProbe);
      const bool isDoiLabel =
          (rightProbe == "정보부" ||
           rightProbeUpper.find("DEPARTMENT OF INTELLIGENCE") !=
               std::string::npos);
      if (isDoiLabel) {
        // DOI right edge should match objective panel right anchor (not panel edge).
        menuForceAlign = 1;
        float panelRight = screenWidth - 70.0f * (screenWidth / 1280.0f);
        float objectiveRight = panelRight - 33.0f * (screenWidth / 1280.0f);
        float targetWidth = objectiveRight - x;
        if (targetWidth > 0.0f)
          finalWidthEng = targetWidth;
      }
    }

    if (menuContext && inPauseMenu && screenWidth > 0.0f &&
        screenHeight > 0.0f && x > screenWidth * 0.55f &&
        y < screenHeight * 0.66f) {
      menuForceAlign = 1; // force right-align
      float panelRight = screenWidth - 70.0f * (screenWidth / 1280.0f);
      float objectiveRight = panelRight - 33.0f * (screenWidth / 1280.0f);
      float itemRight = x + finalWidthEng;
      std::string rightProbe =
          !alignProbeTextRaw.empty() ? alignProbeTextRaw : alignProbeText;
      std::string rightProbeUpper = ToUpperAscii(rightProbe);
      bool isObjectiveHeader =
          (rightProbe == "임무 목표" || rightProbeUpper == "MISSION OBJECTIVES");
      bool isRorkeStatus = IsRorkeFileStatusText(rightProbe);
      float targetRight = (isObjectiveHeader || isRorkeStatus)
                              ? panelRight
                              : objectiveRight;

      if (itemRight > targetRight + 5.0f || itemRight < targetRight - 5.0f) {
        finalWidthEng = targetRight - x;
        if (finalWidthEng < 0.0f)
          finalWidthEng = 0.0f;
      }
    }

    // Advanced Video right-value column should stay center-aligned to
    // the English slot center; clustering can drift a few pixels on short values.
    if (menuContext && inAdvancedVideoMenu && screenWidth > 0.0f &&
        IsAdvancedVideoValueText(alignProbeText)) {
      bool inValueColumn =
          (x >= screenWidth * 0.28f) && (x <= screenWidth * 0.62f);
      if (inValueColumn) {
        menuForceAlign = 2; // center
      }
    }

    // Entering options from pause can briefly inherit pause-layout clusters.
    // Pin value-column center alignment immediately while options headers are active.
    if (menuContext && optionsHeaderRecent && screenWidth > 0.0f &&
        IsAdvancedVideoValueText(alignProbeText)) {
      bool inValueColumn =
          (x >= screenWidth * 0.24f) && (x <= screenWidth * 0.64f);
      if (inValueColumn) {
        menuForceAlign = 2; // center
      }
    }

    // Stick/Button layout left list: keep Nomad Tactical row right-aligned
    // with peer entries (기본/전술/왼손잡이).
    // Gamepad main menu value column should stay centered.
    if (menuContext && screenWidth > 0.0f && screenHeight > 0.0f) {
      std::string layoutProbe =
          !alignProbeText.empty() ? alignProbeText : alignProbeTextRaw;
      std::string layoutProbeUpper = ToUpperAscii(layoutProbe);
      const bool isNomadLayoutItem =
          (layoutProbe == "노매드 전술" || layoutProbe == "노매드전술" ||
           layoutProbe == "노매드" ||
           layoutProbeUpper == "NOMAD TACTICAL" || layoutProbeUpper == "NOM4D");
      const bool inLayoutListBand =
          (x >= screenWidth * 0.08f && x <= screenWidth * 0.27f &&
           y >= screenHeight * 0.16f && y <= screenHeight * 0.80f);
      if (isNomadLayoutItem && inLayoutListBand) {
        menuForceAlign = 1; // right
      }
      const bool inGamepadValueBand =
          (x >= screenWidth * 0.24f && x <= screenWidth * 0.64f &&
           y >= screenHeight * 0.16f && y <= screenHeight * 0.86f);
      if (isNomadLayoutItem && inGamepadMenu && inGamepadValueBand) {
        menuForceAlign = 2; // center
      }
    }

    // Gamepad options value column: keep "비활성화됨/Disabled" perfectly centered.
    if (menuContext && inGamepadMenu && screenWidth > 0.0f && screenHeight > 0.0f) {
      std::string gpProbe =
          !alignProbeText.empty() ? alignProbeText : alignProbeTextRaw;
      std::string gpProbeUpper = ToUpperAscii(gpProbe);
      const bool isDisabledValue =
          (gpProbe == "비활성화됨" || gpProbeUpper == "DISABLED" ||
           gpProbeUpper == "DISABLED (DEFAULT)");
      const bool inGamepadValueBand =
          (x >= screenWidth * 0.24f && x <= screenWidth * 0.64f &&
           y >= screenHeight * 0.16f && y <= screenHeight * 0.86f);
      if (isDisabledValue && inGamepadValueBand) {
        menuForceAlign = 2; // center
      }
    }

    // Options/Gamepad/Audio value column: keep toggle-state values
    // ("활성화됨"/"비활성화됨", Enabled/Disabled) center-locked.
    // Keep this purely text+band based (no header-timer dependency) to avoid
    // frame-to-frame center jitter on quick value toggles.
    if (menuContext && screenWidth > 0.0f && screenHeight > 0.0f) {
      std::string optProbe =
          !alignProbeText.empty() ? alignProbeText : alignProbeTextRaw;
      std::string optProbeUpper = ToUpperAscii(optProbe);
      const bool isToggleStateValue =
          (optProbe.find("활성화됨") != std::string::npos ||
           optProbeUpper.find("ENABLED") != std::string::npos ||
           optProbeUpper.find("DISABLED") != std::string::npos ||
           optProbeUpper == "DISABLED (DEFAULT)");
      const bool inOptionsValueBand =
          (x >= screenWidth * 0.24f && x <= screenWidth * 0.66f &&
           y >= screenHeight * 0.14f && y <= screenHeight * 0.88f);
      if (isToggleStateValue && inOptionsValueBand) {
        menuForceAlign = 2; // center
      }
    }

    // Controls binding detail pages: right-side bound value tokens should stay
    // center-anchored even when key text changes at runtime.
    if (menuContext && screenWidth > 0.0f && screenHeight > 0.0f) {
      std::string bindProbe =
          !alignProbeText.empty() ? alignProbeText : alignProbeTextRaw;
      std::string bindUpper = ToUpperAscii(bindProbe);
      auto IsSimpleKeyToken = [](const std::string &u) -> bool {
        if (u.empty())
          return false;
        if (u == "SPACE" || u == "CTRL" || u == "SHIFT" || u == "ALT" ||
            u == "TAB" || u == "ENTER" || u == "ESC" ||
            u == "MOUSE1" || u == "MOUSE2" || u == "MOUSE3" ||
            u == "MWHEELUP" || u == "MWHEELDOWN")
          return true;
        if (u.size() == 1) {
          unsigned char c = (unsigned char)u[0];
          if (std::isalnum(c))
            return true;
        }
        if (u.size() >= 2 && u.size() <= 4 && u[0] == 'F') {
          bool allDigit = true;
          for (size_t i = 1; i < u.size(); ++i) {
            if (!std::isdigit((unsigned char)u[i])) {
              allDigit = false;
              break;
            }
          }
          if (allDigit)
            return true;
        }
        return false;
      };
      auto IsMouseWheelValueTokenUpper = [](const std::string &u) -> bool {
        return (u == "LEFT MOUSE" || u == "RIGHT MOUSE" ||
                u == "MIDDLE MOUSE" || u == "MOUSE LEFT" ||
                u == "MOUSE RIGHT" || u == "MOUSE MIDDLE" ||
                u == "MOUSE1" || u == "MOUSE2" || u == "MOUSE3" ||
                u == "MWHEELUP" || u == "MWHEELDOWN" || u == "WHEEL UP" ||
                u == "WHEEL DOWN");
      };
      auto IsCompositeBindToken = [&](const std::string &u, const std::string &raw) {
        auto TrimAsciiToken = [](const std::string &s) -> std::string {
          size_t b = 0;
          while (b < s.size() && std::isspace((unsigned char)s[b])) {
            ++b;
          }
          size_t e = s.size();
          while (e > b && std::isspace((unsigned char)s[e - 1])) {
            --e;
          }
          return s.substr(b, e - b);
        };
        auto FindDelimiter = [&](size_t from, size_t &delimLen) -> size_t {
          delimLen = 0;
          size_t dOr = u.find(" OR ", from);
          size_t dKeyOr = u.find(" KEY_OR ", from);
          size_t dKor = raw.find(" 또는 ", from);
          size_t d = std::string::npos;
          if (dOr != std::string::npos) {
            d = dOr;
          }
          if (dKeyOr != std::string::npos &&
              (d == std::string::npos || dKeyOr < d)) {
            d = dKeyOr;
          }
          if (dKor != std::string::npos &&
              (d == std::string::npos || dKor < d)) {
            d = dKor;
          }
          if (d == dOr) {
            delimLen = 4;
          } else if (d == dKeyOr) {
            delimLen = 8;
          } else if (d == dKor) {
            delimLen = 8;
          }
          return d;
        };

        size_t delimLenProbe = 0;
        if (FindDelimiter(0, delimLenProbe) == std::string::npos ||
            delimLenProbe == 0) {
          return false;
        }

        size_t pos = 0;
        int partCount = 0;
        while (pos <= u.size()) {
          size_t delimLen = 0;
          size_t d = FindDelimiter(pos, delimLen);
          std::string part =
              (d == std::string::npos) ? u.substr(pos) : u.substr(pos, d - pos);
          part = TrimAsciiToken(part);
          if (part.empty()) {
            return false;
          }
          const bool partIsBind =
              IsSimpleKeyToken(part) || part.rfind("KEY_", 0) == 0 ||
              IsMouseWheelValueTokenUpper(part);
          if (!partIsBind) {
            return false;
          }
          ++partCount;
          if (d == std::string::npos) {
            break;
          }
          pos = d + delimLen;
        }
        return partCount >= 2;
      };
      auto IsExplicitMouseWheelValueText =
          [&](const std::string &probe, const std::string &probeUpper) -> bool {
        if (probe == "마우스 왼쪽" || probe == "마우스 오른쪽" ||
            probe == "마우스 가운데" || probe == "휠 위로" ||
            probe == "휠 아래로") {
          return true;
        }
        return IsMouseWheelValueTokenUpper(probeUpper);
      };
      const bool isBindValueText =
          (bindProbe.find("할당되지 않음") != std::string::npos ||
           bindProbe.find("또는") != std::string::npos ||
           bindUpper.find("NOT BOUND") != std::string::npos ||
           bindUpper.find("UNBOUND") != std::string::npos ||
           IsExplicitMouseWheelValueText(bindProbe, bindUpper) ||
           IsCompositeBindToken(bindUpper, bindProbe) ||
           IsSimpleKeyToken(bindUpper));
      const bool inBindValueBand =
          (x >= screenWidth * 0.26f && x <= screenWidth * 0.70f &&
           y >= screenHeight * 0.16f && y <= screenHeight * 0.92f);
      if (isBindValueText && inBindValueBand) {
        menuForceAlign = 2;
      }
    }

    // Control Options category list (right panel): keep left alignment.
    // Prevent global "이동" token rules from shifting these category labels.
    if (menuContext && screenWidth > 0.0f && screenHeight > 0.0f) {
      std::string catProbe =
          !alignProbeText.empty() ? alignProbeText : alignProbeTextRaw;
      const bool isControlCategoryItem =
          (catProbe == "이동" || catProbe == "동작" || catProbe == "시선" ||
           catProbe == "채팅" || catProbe == "게임패드" ||
           catProbe == "조작 초기화");
      const bool inControlCategoryBand =
          (x >= screenWidth * 0.34f && x <= screenWidth * 0.76f &&
           y >= screenHeight * 0.16f && y <= screenHeight * 0.68f);
      if (isControlCategoryItem && inControlCategoryBand) {
        menuForceAlign = 0; // left
      }
    }

    // Quit-confirm header can occasionally miss popup-center clustering due to
    // width/anchor jitter; force center only for this exact popup header band.
    if (menuContext && screenWidth > 0.0f && screenHeight > 0.0f) {
      std::string quitHeaderProbe =
          !alignProbeText.empty() ? alignProbeText : alignProbeTextRaw;
      std::string quitHeaderUpper = ToUpperAscii(quitHeaderProbe);
      const bool isQuitConfirmHeader =
          (quitHeaderProbe == "정말 종료하시겠습니까?" ||
           quitHeaderUpper == "ARE YOU SURE YOU WANT TO QUIT?" ||
           quitHeaderUpper == "ARE YOU SURE YOU WANT TO EXIT?");
      const bool inCenterPopupHeaderBand =
          (x >= screenWidth * 0.22f && x <= screenWidth * 0.78f &&
           y >= screenHeight * 0.28f && y <= screenHeight * 0.60f);
      if (isQuitConfirmHeader && inCenterPopupHeaderBand) {
        menuForceAlign = 2;
      }
    }

    // Lower-difficulty popup body should always be center-aligned.
    if (menuContext && screenWidth > 0.0f && screenHeight > 0.0f) {
      std::string lowerDiffProbe =
          !alignProbeText.empty() ? alignProbeText : alignProbeTextRaw;
      const bool isLowerDiffBody =
          (lowerDiffProbe.find("모든 임무의 난이도를") != std::string::npos &&
           lowerDiffProbe.find("낮추시겠습니까") != std::string::npos);
      const bool inCenterPopupBand =
          (x >= screenWidth * 0.22f && x <= screenWidth * 0.78f &&
           y >= screenHeight * 0.32f && y <= screenHeight * 0.64f);
      if (isLowerDiffBody && inCenterPopupBand) {
        menuForceAlign = 2;
      }
    }

    // Quit/restart popup body should always be center-aligned.
    if (menuContext && screenWidth > 0.0f && screenHeight > 0.0f) {
      std::string quitBodyProbe =
          !alignProbeText.empty() ? alignProbeText : alignProbeTextRaw;
      const bool isQuitRestartBody =
          (quitBodyProbe.find("종료하면") != std::string::npos &&
           quitBodyProbe.find("잃게") != std::string::npos) ||
          (quitBodyProbe.find("다시 시작하면") != std::string::npos &&
           quitBodyProbe.find("잃게") != std::string::npos) ||
          (quitBodyProbe.find("덮어씁니다") != std::string::npos &&
           quitBodyProbe.find("계속") != std::string::npos);
      const bool inQuitPopupBand =
          (x >= screenWidth * 0.22f && x <= screenWidth * 0.78f &&
           y >= screenHeight * 0.28f && y <= screenHeight * 0.64f);
      if (isQuitRestartBody && inQuitPopupBand) {
        menuForceAlign = 2;
      }
    }

    // Detect multi-line body text (e.g. Rorke Files classified body).
    // The Korean translation has \n preserved; count newlines to determine
    // if this is body text that needs wrapping and English suppression.
    bool isMultiLineBody = false;
    float bodyMaxWidthPx = 0.0f;
    if (menuContext && resolvedText.find('\n') != std::string::npos) {
      int bodyNlCount = 0;
      for (char c : resolvedText) { if (c == '\n') bodyNlCount++; }
      if (bodyNlCount >= 3) {
        isMultiLineBody = true;
        // LUI body text element width is 648 in 1280-base.
        // Scale to actual screen pixels.
        bodyMaxWidthPx = 648.0f * (screenWidth / 1280.0f);
        menuForceAlign = 0; // left-align for body text
      }
    }

    // Key-based skipMenuClipping for specific footer items (e.g. LUA_MENU_SELECT)
    // that would otherwise be clipped but whose Korean text ("선택") is too generic
    // for the substring-based footer whitelist.
    bool skipClipForKey = false;
    bool forceHeaderAtlasForKey = false;
    if (menuContext && text) {
      std::string cleanE = StripColorCodes(std::string(text));
      cleanE = TrimSpaces(cleanE);
      std::string clipProbe = NormalizeMenuText(!resolvedText.empty() ? resolvedText : cleanE);
      std::string clipProbeUpper = ToUpperAscii(clipProbe);
      const bool inLayoutLegendContext = inLayoutDiagramMenu;
      const bool isLayoutLegendText =
          (clipProbe.find("왼쪽 회전") != std::string::npos ||
           clipProbe.find("오른쪽 회전") != std::string::npos ||
           clipProbe.find("왼쪽/오른쪽 회전") != std::string::npos ||
           clipProbe.find("앞으로 이동") != std::string::npos ||
           clipProbe.find("뒤로 이동") != std::string::npos ||
           clipProbe.find("오른쪽 이동") != std::string::npos ||
           clipProbe.find("왼쪽 이동") != std::string::npos ||
           clipProbe.find("위로 보기") != std::string::npos ||
           clipProbe.find("아래 보기") != std::string::npos ||
           clipProbeUpper.find("ROTATE LEFT") != std::string::npos ||
           clipProbeUpper.find("ROTATE RIGHT") != std::string::npos ||
           clipProbeUpper.find("MOVE FORWARD") != std::string::npos ||
           clipProbeUpper.find("MOVE BACKWARD") != std::string::npos ||
           clipProbeUpper.find("MOVE RIGHT") != std::string::npos ||
           clipProbeUpper.find("MOVE LEFT") != std::string::npos ||
           clipProbeUpper.find("LOOK UP") != std::string::npos ||
           clipProbeUpper.find("LOOK DOWN") != std::string::npos);
      const bool inLayoutLegendBand =
          (screenWidth > 0.0f && screenHeight > 0.0f &&
            y >= screenHeight * 0.54f &&
           ((x >= screenWidth * 0.10f && x <= screenWidth * 0.48f) ||
             (x >= screenWidth * 0.68f && x <= screenWidth * 0.96f)));
      if (inLayoutLegendContext && isLayoutLegendText && inLayoutLegendBand) {
        skipClipForKey = true;
      }

      // Movement bind page: suppress stray duplicated labels outside the
      // protected center panel (header/footer leak artifacts).
      if (inMovementHeaderMenu && screenHeight > 0.0f) {
        // Hardcoded guard for movement-page top clipping leaks.
        const bool isMoveTopLeakLabel =
            (clipProbe.find("위로 이동") != std::string::npos ||
             clipProbe.find("왼쪽으로 이동") != std::string::npos ||
             clipProbe.find("오른쪽으로 이동") != std::string::npos ||
             clipProbe.find("뒤로 이동") != std::string::npos ||
             clipProbe.find("왼쪽 이동") != std::string::npos ||
             clipProbe.find("오른쪽 이동") != std::string::npos ||
             clipProbeUpper.find("MOVE UP") != std::string::npos ||
             clipProbeUpper.find("MOVE LEFT") != std::string::npos ||
             clipProbeUpper.find("MOVE RIGHT") != std::string::npos ||
             clipProbeUpper.find("MOVE BACKWARD") != std::string::npos);
        const bool isMoveLabelTopLeak =
            isMoveTopLeakLabel && (y <= screenHeight * 0.20f);
        const bool isRotateFooterLeak =
            (clipProbe == "오른쪽 회전") && (y >= screenHeight * 0.90f);
        if (isMoveLabelTopLeak || isRotateFooterLeak) {
          resolvedText = " ";
        }
      }
      // Controls bind pages: suppress orphan wrapped tail tokens emitted as
      // separate draw-calls (e.g. trailing "Mouse" line after composite value).
      if (inControlsBindMenuEffective && screenWidth > 0.0f &&
          screenHeight > 0.0f) {
        const bool inControlBindValueBand =
            (x >= screenWidth * 0.26f && x <= screenWidth * 0.70f &&
             y >= screenHeight * 0.16f && y <= screenHeight * 0.92f);
        if (inControlBindValueBand) {
          std::string orphanProbe =
              NormalizeMenuText(!resolvedText.empty() ? resolvedText : cleanE);
          std::string orphanUpper = ToUpperAscii(orphanProbe);
          const bool isOrphanBindTail =
              (orphanUpper == "MOUSE" || orphanUpper == "OR" ||
               orphanProbe == "또는");
          if (isOrphanBindTail) {
            resolvedText = " ";
          }
        }
      }
      std::string upperE = ToUpper(cleanE);
      auto itK = g_EnglishToKey.find(upperE);
      if (itK != g_EnglishToKey.end()) {
        const std::string &resolvedKey = itK->second;
        std::string resolvedKeyUpper = ToUpperAscii(resolvedKey);

        // Key-name fallback for unstable localized text packets.
        if (resolvedKeyUpper.find("STICK_LAYOUT") != std::string::npos) {
          resolvedText = "스틱 배치";
        } else if (resolvedKeyUpper.find("BUTTON_LAYOUT") != std::string::npos) {
          resolvedText = "버튼 배치";
        } else if (resolvedKeyUpper.find("NOM4D") != std::string::npos ||
                   resolvedKeyUpper.find("NOMAD_TACTICAL") != std::string::npos) {
          resolvedText = "노매드 전술";
        } else if (resolvedKeyUpper.find("SP_NO_CLICK_SWAP") != std::string::npos) {
          if (resolvedKeyUpper.find("LEG") != std::string::npos)
            resolvedText = "레거시 SP 클릭 교체 없음";
          else
            resolvedText = "SP 클릭 교체 없음";
        }

        if (resolvedKey == "LUA_MENU_SELECT") {
          skipClipForKey = true;
        }
        if (resolvedKey == "LUA_MENU_ROTATE_LEFT" ||
            resolvedKey == "LUA_MENU_ROTATE_RIGHT" ||
            resolvedKey == "MENU_ROTATE_LEFT_RIGHT") {
          if (inLayoutLegendContext && inLayoutLegendBand) {
            skipClipForKey = true;
          }
        }
        if (resolvedKey == "LUA_MENU_INTEL_NOT_FOUND" ||
            resolvedKey == "LUA_MENU_INTEL_FOUND") {
          skipClipForKey = true;
          menuForceAlign = 1; // right-align (right edge flush)
          if (inPauseMenu) {
            // Pause-menu right-bottom Rorke status must keep header atlas path.
            forceHeaderAtlasForKey = true;
          }
        }
        if (resolvedKey == "MENU_SP_VF_MENU_SELECT_DOI") {
          // Key-based strict guard: DOI uses objective panel right anchor.
          menuForceAlign = 1;
          if (screenWidth > 0.0f) {
            float panelRight = screenWidth - 70.0f * (screenWidth / 1280.0f);
            float objectiveRight = panelRight - 33.0f * (screenWidth / 1280.0f);
            float targetWidth = objectiveRight - x;
            if (targetWidth > 0.0f)
              finalWidthEng = targetWidth;
          }
        }
        if (resolvedKey == "MENU_SP_VF_MENU_SELECT_CLASSIFIED" &&
            screenWidth > 0.0f && screenHeight > 0.0f &&
            x <= screenWidth * 0.45f && y <= screenHeight * 0.42f) {
          menuForceAlign = 0;
        }

        // Footer key labels (F1/F/ESC/PGUP...) often render as separate tiny
        // draw calls and can be clipped out while adjacent text survives.
        const bool nearBottomFooterBand =
            (screenHeight > 0.0f && y >= screenHeight * 0.68f);
        const bool isPlatformKeyLabel =
            (resolvedKey.rfind("PLATFORM_", 0) == 0) &&
            (resolvedKey.find("_BUTTON") != std::string::npos ||
             resolvedKey.find("_SHORTCUT") != std::string::npos);
        const bool isImageNavLabel =
            (resolvedKey == "MENU_PREV_IMG" || resolvedKey == "MENU_NEXT_IMG");
        if (nearBottomFooterBand && (isPlatformKeyLabel || isImageNavLabel)) {
          skipClipForKey = true;
        }
      }
    }

    // Use whichever maxWidth is active: right-panel or body text
    float queueMaxWidth = (bodyMaxWidthPx > 1.0f) ? bodyMaxWidthPx : menuMaxWidthPx;
    uint8_t atlasSlot = 0;
    if (menuContext) {
      const bool isLargeBucket =
          (scaleMul >= (kMenuScaleLarge - 0.001f) &&
           scaleMul <= (kMenuScaleLarge + 0.001f));
      bool isHeaderKeyword = IsMenuHeaderForceLarge(alignProbeText);
      if (!isHeaderKeyword && !alignProbeTextRaw.empty()) {
        isHeaderKeyword = IsMenuHeaderForceLarge(alignProbeTextRaw);
      }
      bool isPauseRorkeStatus = false;
      if (!isPauseRorkeStatus) {
        std::string statusProbe =
            !alignProbeTextRaw.empty() ? alignProbeTextRaw : alignProbeText;
        isPauseRorkeStatus = inPauseMenu && IsRorkeFileStatusText(statusProbe);
      }
      if (isLargeBucket || isHeaderKeyword || isPauseRorkeStatus ||
          forceHeaderAtlasForKey) {
        atlasSlot = 1;
      }
    }
    float queueY = y;
    if (menuContext && screenWidth > 0.0f && screenHeight > 0.0f &&
        scaleMul <= (kMenuScaleSmall + 0.001f)) {
      std::string popupPosProbe =
          !alignProbeTextRaw.empty() ? alignProbeTextRaw : alignProbeText;
      const bool inCenterPopupBand =
          (x >= screenWidth * 0.22f && x <= screenWidth * 0.78f &&
           y >= screenHeight * 0.32f && y <= screenHeight * 0.64f);
      const bool isPopupSentence =
          (!IsYesNoText(popupPosProbe) && IsPopupDescText(popupPosProbe));
      if (inCenterPopupBand && isPopupSentence) {
        // Resolution-scaled nudge: popup small-body Korean text sits slightly high.
        queueY += screenHeight * (6.0f / 1440.0f);
        // Ensure popup body text wraps at 65% of screen width even when
        // renderer-side popup detection fails intermittently.
        if (queueMaxWidth < 1.0f) {
          queueMaxWidth = screenWidth * 0.65f;
        }
      }
    }
    if (menuContext && TextHook_IsMenuDiagActive()) {
      static unsigned long long s_LastMenuDiagQueueSession = 0;
      static int s_MenuDiagQueueCount = 0;
      const unsigned long long diagSession = TextHook_GetMenuDiagSessionId();
      if (diagSession != s_LastMenuDiagQueueSession) {
        s_LastMenuDiagQueueSession = diagSession;
        s_MenuDiagQueueCount = 0;
      }
      if (s_MenuDiagQueueCount < 96) {
        const std::string rawPreview =
            text ? Utf8Preview(std::string(text), 64) : std::string("(null)");
        const std::string resolvedPreview = Utf8Preview(resolvedText, 64);
        char qbuf[768];
        sprintf_s(
            qbuf,
            "[MENU-DIAG-QUEUE] sess=%llu idx=%d front=%d x=%.1f y=%.1f qy=%.1f font=%.1f scale=%.3f mul=%.3f engW=%.1f force=%d small=%d clip=%d atlas=%u style=%d raw=\"%s\" kor=\"%s\"",
            diagSession, s_MenuDiagQueueCount, TextHook_IsFrontendMenuLikely() ? 1 : 0,
            x, y, queueY, fontHeight, xScale, scaleMul, finalWidthEng,
            menuForceAlign, isSmallMenu ? 1 : 0, skipClipForKey ? 1 : 0,
            (unsigned int)atlasSlot, effectiveStyle, rawPreview.c_str(),
            resolvedPreview.c_str());
        LogToFile(qbuf);
        ++s_MenuDiagQueueCount;
      }
    }
    // Rorke animated header: capture params + fix x to prevent drift
    {
      const bool isAnimatedHdrByKey =
          !translatedKey.empty() &&
          (translatedKey == "MENU_SP_CLASSIFIED_01_CAPS" ||
           translatedKey == "MENU_SP_CLASSIFIED_06_CAPS" ||
           translatedKey == "MENU_SP_CLASSIFIED_19_CAPS" ||
           translatedKey == "MENU_SP_CLASSIFIED_20_CAPS" ||
           translatedKey == "MENU_SP_CLASSIFIED_21_CAPS");
      // Also detect by English text (for pages 19,20,21 where translatedKey is empty)
      // Guard: only on Rorke pages (body supp active) AND header area (y < 16%)
      const bool isAnimatedHdrByText =
          s_rkBodySuppTick_shared != 0 &&
          (GetTickCount() - s_rkBodySuppTick_shared) < 2000 &&
          GetScreenHeightApprox() > 0.0f &&
          y < GetScreenHeightApprox() * 0.16f &&
          text && strstr(text, "PROFILE:") != nullptr;
      const bool isAnimatedHdr = isAnimatedHdrByKey || isAnimatedHdrByText;
      if (isAnimatedHdr) {
        // Latch x once per page (reset when header text changes)
        static std::string s_rkHdrLastText;
        std::string hdrId = translatedKey.empty() ? std::string(text ? text : "") : translatedKey;
        if (hdrId != s_rkHdrLastText) {
          s_rkHdrLastText = hdrId;
          s_rkHdrFixedX = 0;
          s_rkHdrCacheTick = 0;
        }
        if (s_rkHdrCacheTick == 0 && finalColor[3] > 0.5f) {
          float sw = GetScreenWidthApprox();
          float hdrNudge = sw * 0.004f; // pages 1, 6 (~8px at 1920)
          if (isAnimatedHdrByText) {
            hdrNudge = sw * 0.017f; // pages 19, 21 (~32px at 1920)
            if (text && strstr(text, "DAVID")) hdrNudge = sw * 0.0125f; // page 20 (~24px at 1920)
          }
          s_rkHdrFixedX = x + hdrNudge;
          s_rkHdrCacheTick = GetTickCount();
        }
        s_rkHdrCachedText = resolvedText;
        s_rkHdrX = s_rkHdrFixedX;
        s_rkHdrY = queueY;
        s_rkHdrScaleAdj = xScaleAdjusted;
        s_rkHdrFontH = fontHeight;
        s_rkHdrEngW = finalWidthEng;
        s_rkHdrOrigScale = xScale;
        memcpy(s_rkHdrColor, finalColor, sizeof(float)*4);
        s_rkHdrColor[3] = 1.0f;
        s_rkHdrStyle = effectiveStyle;
        s_rkHdrSmall = isSmallMenu;
        s_rkHdrShadow = disableShadow;
        s_rkHdrAlign = menuForceAlign;
        s_rkHdrMaxW = queueMaxWidth;
        s_rkHdrSkipClip = skipClipForKey;
        s_rkHdrAtlas = atlasSlot;
        s_rkHdrCacheValid = true;
        // Skip normal QueueText — header is ONLY rendered via body piggyback.
        // This prevents duplicate entries with animated vs fixed x.
        if (Original_R_AddCmdDrawText)
          Original_R_AddCmdDrawText(" ", 1, font, x, y, xScale, yScale,
                                    rotation, color, style);
        goto render_done_skip_queue;
      }
    }
    KoreanRenderer::QueueText(resolvedText, x, queueY, xScaleAdjusted, finalColor,
                              fontHeight, effectiveStyle, finalWidthEng,
                              disableShadow, isSmallMenu, xScale,
                              false, false, false, menuForceAlign,
                              queueMaxWidth, skipClipForKey, false, atlasSlot);
    g_bQueueFromMenu.store(false);

    // If multi-line body text, activate English suppression zone so
    // subsequent per-line draw calls from the engine are blanked.
    if (isMultiLineBody) {
      s_BodySuppX = x;
      s_BodySuppBaseY = queueY;
      s_BodySuppMaxY = queueY + screenHeight * 0.6f;
      s_BodySuppMaxDx = 50.0f;
      s_BodySuppDurationMs = 50;
      s_BodySuppTime = GetTickCount();
    }

    if (Original_R_AddCmdDrawText) {
      if (menuContext || IsMenuActive() || s_InMenu) {
        Original_R_AddCmdDrawText(" ", maxChars, font, x, y, xScale, yScale,
                                  rotation, color, style);
      } else {
        // Korean overlay was queued above — render English with alpha=0
        // so it's invisible but all capture/queuing code ran normally.
        float zeroA[4] = {color ? color[0] : 1.f, color ? color[1] : 1.f,
                          color ? color[2] : 1.f, 0.0f};
        Original_R_AddCmdDrawText(text, maxChars, font, x, y, xScale, yScale,
                                  rotation, zeroA, style);
      }
    }
    return;
  }

  render_original:
  // === CALL ORIGINAL ===

  // Suppress English tail fragment from autosave notice popup.
  if (translated && !ContainsKorean(translated)) {
    std::string tailUpper = ToUpperAscii(std::string(translated));
    if ((tailUpper.find("SAVING") != std::string::npos &&
         tailUpper.find("MESSAGE APPEARS") != std::string::npos) ||
        (tailUpper.find("DO NOT TURN OFF") != std::string::npos) ||
        (tailUpper.find("DO NOT SWITCH OFF") != std::string::npos)) {
      translated = " ";
    }
  }

  if (Original_R_AddCmdDrawText) {

    static int logRawCounter = 0;

    if (logRawCounter < 20) {

      logRawCounter++;

      std::string msg = "[TextHook] Raw AddCmd: \"" +

                        std::string(text ? text : "null") +

                        "\" x=" + std::to_string(x) + " y=" + std::to_string(y);

      LogToFile(msg);
    }

    Original_R_AddCmdDrawText(translated, -1, font, x, y, xScale, yScale,

                              rotation, color, style);
  }
  render_done_skip_queue: ;
}

// Helper: Check if SEH code is ready
bool IsSEHCodeReady(void *addr) {
  if (!addr)
    return false;
  unsigned char *bytes = (unsigned char *)addr;

  // Log actual bytes for debugging (first time only)
  static bool logged = false;
  if (!logged) {
    logged = true;
    std::stringstream ss;
    ss << "[TextHook] SEH actual bytes at target: ";
    for (int i = 0; i < 16; i++) {
      ss << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[i]
         << " ";
    }
    LogToFile(ss.str());
  }

  // SEH_StringEd_GetString expected prologue: 48 83 EC 28 (sub rsp, 28h)
  // But accept any valid function start
  bool match1 = (bytes[0] == 0x48 && bytes[1] == 0x83 && bytes[2] == 0xEC &&
                 bytes[3] == 0x28);
  bool match2 = (bytes[0] == 0x48 && bytes[1] == 0x89); // mov [rsp+...], ...
  bool match3 = (bytes[0] == 0x40 && bytes[1] == 0x53); // push rbx
  bool match4 = (bytes[0] == 0x48 && bytes[1] == 0x83 &&
                 bytes[2] == 0xEC); // sub rsp, imm8

  return match1 || match2 || match3 || match4;
}

// =============================================================================
// HudTextResolve Detour (0x1FC8B0) - Captures text resolution during rendering
// =============================================================================
const char *Detour_HudTextResolve(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4) {
  const char *result =
      Original_HudTextResolve ? Original_HudTextResolve(a1, a2, a3, a4) : nullptr;

  static uintptr_t s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  uintptr_t retAddr = (uintptr_t)_ReturnAddress();
  uintptr_t callerOffset = retAddr - s_moduleBase;
  if (result && result[0]) {
    // OSREV resolve draw sample removed — OSAUTH is the sole authority.
    (void)g_HTR_CallerRegs.rbp;
  }

  // Deduplicate: only log when text or caller changes
  static char s_lastText[128] = {};
  static uintptr_t s_lastCaller = 0;
  static int s_uniqueCount = 0;
  static int s_repeatCount = 0;
  static int s_totalCount = 0;

  s_totalCount++;

  bool isNewEntry = false;
  const char *txt = result ? result : "(null)";
  if (callerOffset != s_lastCaller || strncmp(txt, s_lastText, 127) != 0) {
    isNewEntry = true;
    s_lastCaller = callerOffset;
    strncpy_s(s_lastText, txt, 127);
    if (s_repeatCount > 1) {
      char rbuf[256];
      sprintf_s(rbuf, "[HUDRESOLVE] (repeated %d times)", s_repeatCount);
      LogToFile(rbuf);
    }
    s_repeatCount = 0;
  }
  s_repeatCount++;

  if (isNewEntry && s_uniqueCount < 500) {
    s_uniqueCount++;

    uintptr_t rbp = g_HTR_CallerRegs.rbp;
    uintptr_t rbx = g_HTR_CallerRegs.rbx;
    uintptr_t rdi = g_HTR_CallerRegs.rdi;
    uintptr_t rsi = g_HTR_CallerRegs.rsi;
    uintptr_t r12 = g_HTR_CallerRegs.r12;
    uintptr_t r13 = g_HTR_CallerRegs.r13;
    uintptr_t r14 = g_HTR_CallerRegs.r14;
    uintptr_t r15 = g_HTR_CallerRegs.r15;

    char buf[768];

    // === Entry: caller, args, result, all registers ===
    sprintf_s(buf,
              "[HUDRESOLVE] #%d total=%d caller=0x%llX "
              "a1=0x%llX a2=0x%llX a3=0x%llX a4=0x%llX "
              "result=\"%.80s\"",
              s_uniqueCount, s_totalCount,
              (unsigned long long)callerOffset,
              (unsigned long long)a1, (unsigned long long)a2,
              (unsigned long long)a3, (unsigned long long)a4, txt);
    LogToFile(buf);

    sprintf_s(buf,
              "[HUDRESOLVE] regs: rdi=0x%llX rsi=0x%llX rbx=0x%llX "
              "rbp=0x%llX r12=0x%llX r13=0x%llX r14=0x%llX r15=0x%llX",
              (unsigned long long)rdi, (unsigned long long)rsi,
              (unsigned long long)rbx, (unsigned long long)rbp,
              (unsigned long long)r12, (unsigned long long)r13,
              (unsigned long long)r14, (unsigned long long)r15);
    LogToFile(buf);

    // === RBP struct dump (ScreenPlacement/RenderParams) ===
    if (rbp > 0x10000 && rbp < 0x7FFFFFFFFFFF &&
        !IsBadReadPtr((void *)rbp, 0x40)) {
      float px = *(volatile float *)(rbp + 0x00);
      float py = *(volatile float *)(rbp + 0x04);
      float sx = *(volatile float *)(rbp + 0x08);
      float sy = *(volatile float *)(rbp + 0x0C);
      uint8_t al1 = *(volatile uint8_t *)(rbp + 0x10);
      uint8_t al2 = *(volatile uint8_t *)(rbp + 0x11);
      sprintf_s(buf,
                "[HUDRESOLVE] RBP x=%.2f y=%.2f sx=%.4f sy=%.4f "
                "align=%d,%d",
                px, py, sx, sy, (int)al1, (int)al2);
      LogToFile(buf);
    }

    // NOTE: DataStruct(0x5ADF930) and FontStruct(R14) dumps removed
    // - DataStruct is a pointer table, NOT render params (confirmed 2026-02-09)
    // - FontStruct constant at R14=0x142FDAF98, height=35
  }

  return result;
}

// OSREV chain hook detours removed — OSAUTH is the sole authority.

// Delayed hook thread
DWORD WINAPI DelayedHookThread(LPVOID lpParam) {

  LogToFile("[TextHook] Delayed hook thread started. Waiting for code...");
  // OSREV chain hooks removed — OSAUTH is the sole authority.

  bool r_addcmd_hooked = g_HookApplied;
  bool seh_hooked = g_SEH_HookApplied;

  // Native HUD capture (HUD_DrawText hook) may only become executable much later
  // (post-decryption / after loading). Keep this thread alive long enough to
  // observe that transition; otherwise HUD native capture silently never enables.
  const bool needHudDrawHook = TextHook_IsHudNativeEnabled();
  const bool needTimeScriptHook = false; // Legacy target was mid-function, not a callable ABI.
  const int kMaxIters = needHudDrawHook ? 600 : 120; // 5min vs 1min (500ms sleep)

  for (int i = 0; i < kMaxIters; i++) {

    Sleep(500);

    const bool cgMsgHookNeeded =
        RuntimeFlags_ObjectiveStatusEnableGameMessage() &&
        g_CG_GameMessage_TargetAddr != nullptr;

    // Late-bind R_TextWidth when IW6 code becomes executable (decrypted).
    if (!g_R_TextWidth.load(std::memory_order_acquire)) {
      if (IsProbablyExecutableCode(g_R_TextWidthAddrPrimary)) {
        g_R_TextWidth.store((R_TextWidth_t)g_R_TextWidthAddrPrimary,
                            std::memory_order_release);
        LogToFile("[TextHook] R_TextWidth became ready (primary).");
      } else if (IsProbablyExecutableCode(g_R_TextWidthAddrFallback)) {
        g_R_TextWidth.store((R_TextWidth_t)g_R_TextWidthAddrFallback,
                            std::memory_order_release);
        LogToFile("[TextHook] R_TextWidth became ready (fallback).");
      }
    }

    // === R_AddCmdDrawText Hook ===
    if (!r_addcmd_hooked) {
      void *candidate = nullptr;
      if (g_TargetAddrPrimary && IsCodeReady(g_TargetAddrPrimary)) {
        candidate = g_TargetAddrPrimary;
      } else if (g_TargetAddrFallback && IsCodeReady(g_TargetAddrFallback)) {
        candidate = g_TargetAddrFallback;
      }
      if (candidate) {
        g_TargetAddr = candidate;

        LogToFile("[TextHook] R_AddCmdDrawText code is ready! Applying hook...");

        std::stringstream ss;
        ss << "[TextHook] R_AddCmdDrawText bytes: ";
        unsigned char *bytes = (unsigned char *)g_TargetAddr;
        for (int j = 0; j < 16; j++) {
          ss << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[j]
             << " ";
        }
        LogToFile(ss.str());

        CreateHook(g_TargetAddr, (void *)&Detour_R_AddCmdDrawText,
                   (void **)&Original_R_AddCmdDrawText);

        if (Original_R_AddCmdDrawText) {
          g_HookApplied = true;
          r_addcmd_hooked = true;
          LogToFile("[TextHook] SUCCESS: R_AddCmdDrawText hook applied!");
          if (kEnableRuntimeTextCapture) {
            CreateThread(NULL, 0, LoggerThread, NULL, 0, NULL);
          }
        }
      }
    }

    // === SEH_StringEd_GetString Hook ===
    if (!seh_hooked && g_SEH_TargetAddr && IsSEHCodeReady(g_SEH_TargetAddr)) {

      LogToFile("[TextHook] SEH_StringEd_GetString code is ready! Applying "
                "hook...");

      std::stringstream ss;
      ss << "[TextHook] SEH bytes: ";
      unsigned char *bytes = (unsigned char *)g_SEH_TargetAddr;
      for (int j = 0; j < 14; j++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[j]
           << " ";
      }
      if (cgMsgHookNeeded && !g_CG_GameMessage_HookApplied) {
        ss << "CGMsg: pending ";
      }
      LogToFile(ss.str());

      // CRITICAL: Use 14 bytes for SEH_StringEd_GetString!
      // SEH prologue: sub rsp,28h(4) + mov rax,[rip+...](7) + test rax,rax(3)
      // = 14 bytes
      CreateHook(g_SEH_TargetAddr, (void *)&Detour_SEH_StringEd_GetString,
                 (void **)&Original_SEH_StringEd_GetString, 14);

      if (Original_SEH_StringEd_GetString) {
        g_SEH_HookApplied = true;
        seh_hooked = true;
        LogToFile("[TextHook] SUCCESS: SEH Hook applied!");
        LogToFile("[TextHook] This enables: Loading subtitles, Mission "
                  "briefings, Dynamic text");
      }
    }

    // === Cbuf_AddText Hook — restart signal detection ===
    if (!g_Cbuf_AddText_HookApplied && g_Cbuf_AddText_TargetAddr &&
        IsProbablyExecutableCode(g_Cbuf_AddText_TargetAddr)) {

      LogToFile("[TextHook] Cbuf_AddText code is ready! Applying hook...");

      {
        std::stringstream ss;
        ss << "[TextHook] Cbuf_AddText bytes: ";
        unsigned char *bytes = (unsigned char *)g_Cbuf_AddText_TargetAddr;
        for (int j = 0; j < 16; j++) {
          ss << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[j]
             << " ";
        }
        LogToFile(ss.str());
      }

      CreateHook(g_Cbuf_AddText_TargetAddr, (void *)&Detour_Cbuf_AddText,
                 (void **)&Original_Cbuf_AddText);

      if (Original_Cbuf_AddText) {
        g_Cbuf_AddText_HookApplied = true;
        LogToFile("[TextHook] SUCCESS: Cbuf_AddText hook applied! Restart "
                  "signal detection active.");
      } else {
        LogToFile("[TextHook] FAILED: Cbuf_AddText hook could not be applied.");
      }
    }

    // === CG_DrawHudElem Hook — runtime function discovery ===
    // We know SEH_StringEd_GetString is called from inside CG_DrawHudElem
    // at caller offset 0x1F087D.  Scan backward from that address to find
    // the function prologue (CC padding), then hook it.
    if (!g_CG_DrawHudElem_Hooked && seh_hooked) {
      static uintptr_t s_cgDrawHudElemAddr = 0;
      if (s_cgDrawHudElemAddr == 0 && GameBuild::RuntimeReady()) {
        // Exact function boundary verified from both stock runtime captures.
        s_cgDrawHudElemAddr = (uintptr_t)GetModuleHandleW(nullptr) + IW6Offsets::CG_DrawHudElem_SP;
      }
      if (s_cgDrawHudElemAddr != 0 && !g_CG_DrawHudElem_Hooked) {
        CreateHook((void *)s_cgDrawHudElemAddr,
                   (void *)&Detour_CG_DrawHudElem,
                   (void **)&Original_CG_DrawHudElem);
        if (Original_CG_DrawHudElem) {
          g_CG_DrawHudElem_Hooked = true;
          LogToFile("[TextHook] SUCCESS: CG_DrawHudElem hook applied! "
                    "English flicker suppression active.");
        } else {
          LogToFile("[TextHook] FAILED: CG_DrawHudElem hook.");
        }
      }
    }

    // === SL_ConvertToString - CUSTOM 14-byte JMP hook (no gateway) ===
    // The function is only 24 bytes with a short jump at offset 2,
    // so Detour::Create would produce a broken gateway. Instead we:
    // 1. Calculate string table global address from the RIP-relative MOV
    // 2. Write a raw 14-byte absolute JMP to our reimplemented detour
    if (!g_SLC_HookApplied && g_SLC_TargetAddr) {
      unsigned char *bytes = (unsigned char *)g_SLC_TargetAddr;

      // Log prologue for verification
      static bool s_slcBytesLogged = false;
      if (!s_slcBytesLogged) {
        s_slcBytesLogged = true;
        std::stringstream ss;
        ss << "[TextHook] SLC prologue: ";
        for (int j = 0; j < 24; j++) {
          ss << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[j]
             << " ";
        }
        LogToFile(ss.str());
      }

      // Verify expected prologue: 85 c9 74 12 48 8b 05 ...
      if (bytes[0] == 0x85 && bytes[1] == 0xc9 && bytes[2] == 0x74 &&
          bytes[4] == 0x48 && bytes[5] == 0x8b && bytes[6] == 0x05) {

        // Extract RIP-relative displacement from MOV RAX,[RIP+disp32]
        // Instruction at offset 4, length 7, so RIP = target + 4 + 7 = target + 11
        int32_t disp = *(int32_t *)(bytes + 7);
        uintptr_t instrEnd = (uintptr_t)g_SLC_TargetAddr + 11;
        g_SLC_StringTableGlobal = instrEnd + disp;

        LogToFile("[TextHook] SLC string table global at: 0x" +
                  ([](uintptr_t v) {
                    std::stringstream s;
                    s << std::hex << v;
                    return s.str();
                  })(g_SLC_StringTableGlobal));

        // Verify the global is readable
        uintptr_t testVal = 0;
        if (IsBadReadPtr((void *)g_SLC_StringTableGlobal, 8) == 0) {
          testVal = *(uintptr_t *)g_SLC_StringTableGlobal;
          LogToFile("[TextHook] SLC string table base ptr: 0x" +
                    ([](uintptr_t v) {
                      std::stringstream s;
                      s << std::hex << v;
                      return s.str();
                    })(testVal));
        }

        if (testVal != 0) {
          // Save original bytes for potential unhook
          memcpy(g_SLC_OriginalBytes, bytes, 14);

          // === Build assembly pre-stub ===
          // Saves caller register state to g_SLC_CallerRegs
          // BEFORE the C++ compiler prologue overwrites them, then JMPs
          // to the C++ detour.
          //
          // Layout:
          //   MOV RAX, &g_SLC_CallerRegs  ; 10 bytes (48 B8 imm64)
          //   MOV [RAX+0],  RSP           ; 3  bytes (48 89 20)
          //   MOV [RAX+8],  RDI           ; 4  bytes (48 89 78 08)
          //   MOV [RAX+16], RSI           ; 4  bytes (48 89 70 10)
          //   MOV [RAX+24], RBX           ; 4  bytes (48 89 58 18)
          //   MOV [RAX+32], RBP           ; 4  bytes (48 89 68 20)
          //   MOV [RAX+40], R12           ; 4  bytes (4C 89 60 28)
          //   MOV [RAX+48], R13           ; 4  bytes (4C 89 68 30)
          //   MOV [RAX+56], R14           ; 4  bytes (4C 89 70 38)
          //   MOV [RAX+64], R15           ; 4  bytes (4C 89 78 40)
          //   MOV [RAX+72], RCX           ; 4  bytes (48 89 48 48)
          //   MOV [RAX+80], RDX           ; 4  bytes (48 89 50 50)
          //   MOV [RAX+88], R8            ; 4  bytes (4C 89 40 58)
          //   MOV [RAX+96], R9            ; 4  bytes (4C 89 48 60)
          //   MOV [RAX+104], R10          ; 4  bytes (4C 89 50 68)
          //   MOV [RAX+112], R11          ; 4  bytes (4C 89 58 70)
          //   MOV R10, [RSP]              ; 4  bytes (4C 8B 14 24)
          //   MOV [RAX+120], R10          ; 4  bytes (4C 89 50 78)
          //   MOV R10, GS:[0x48]          ; 9  bytes (65 4C 8B 14 25 48 00 00 00)
          //   MOV [RAX+128], R10          ; 7  bytes (4C 89 90 80 00 00 00)
          //   MOV RAX, &Detour            ; 10 bytes (48 B8 imm64)
          //   JMP RAX                     ; 2  bytes (FF E0)
          g_SLC_PreStub =
              VirtualAlloc(NULL, 160, MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
          if (!g_SLC_PreStub) {
            LogToFile("[TextHook] FAILED: VirtualAlloc for SLC pre-stub");
          } else {
            unsigned char *s = (unsigned char *)g_SLC_PreStub;
            int off = 0;

            // MOV RAX, imm64 (&g_SLC_CallerRegs)
            s[off++] = 0x48;
            s[off++] = 0xB8;
            *(uint64_t *)(s + off) = (uint64_t)&g_SLC_CallerRegs;
            off += 8;

            // MOV [RAX+0], RSP  (48 89 20)
            s[off++] = 0x48;
            s[off++] = 0x89;
            s[off++] = 0x20;

            // MOV [RAX+8], RDI  (48 89 78 08)
            s[off++] = 0x48;
            s[off++] = 0x89;
            s[off++] = 0x78;
            s[off++] = 0x08;

            // MOV [RAX+16], RSI (48 89 70 10)
            s[off++] = 0x48;
            s[off++] = 0x89;
            s[off++] = 0x70;
            s[off++] = 0x10;

            // MOV [RAX+24], RBX (48 89 58 18)
            s[off++] = 0x48;
            s[off++] = 0x89;
            s[off++] = 0x58;
            s[off++] = 0x18;

            // MOV [RAX+32], RBP (48 89 68 20)
            s[off++] = 0x48;
            s[off++] = 0x89;
            s[off++] = 0x68;
            s[off++] = 0x20;

            // MOV [RAX+40], R12 (4C 89 60 28)
            s[off++] = 0x4C;
            s[off++] = 0x89;
            s[off++] = 0x60;
            s[off++] = 0x28;

            // MOV [RAX+48], R13 (4C 89 68 30)
            s[off++] = 0x4C;
            s[off++] = 0x89;
            s[off++] = 0x68;
            s[off++] = 0x30;

            // MOV [RAX+56], R14 (4C 89 70 38)
            s[off++] = 0x4C;
            s[off++] = 0x89;
            s[off++] = 0x70;
            s[off++] = 0x38;

            // MOV [RAX+64], R15 (4C 89 78 40)
            s[off++] = 0x4C;
            s[off++] = 0x89;
            s[off++] = 0x78;
            s[off++] = 0x40;

            // MOV [RAX+72], RCX (48 89 48 48)
            s[off++] = 0x48;
            s[off++] = 0x89;
            s[off++] = 0x48;
            s[off++] = 0x48;

            // MOV [RAX+80], RDX (48 89 50 50)
            s[off++] = 0x48;
            s[off++] = 0x89;
            s[off++] = 0x50;
            s[off++] = 0x50;

            // MOV [RAX+88], R8  (4C 89 40 58)
            s[off++] = 0x4C;
            s[off++] = 0x89;
            s[off++] = 0x40;
            s[off++] = 0x58;

            // MOV [RAX+96], R9  (4C 89 48 60)
            s[off++] = 0x4C;
            s[off++] = 0x89;
            s[off++] = 0x48;
            s[off++] = 0x60;

            // MOV [RAX+104], R10 (4C 89 50 68)
            s[off++] = 0x4C;
            s[off++] = 0x89;
            s[off++] = 0x50;
            s[off++] = 0x68;

            // MOV [RAX+112], R11 (4C 89 58 70)
            s[off++] = 0x4C;
            s[off++] = 0x89;
            s[off++] = 0x58;
            s[off++] = 0x70;

            // MOV R10, [RSP] (4C 8B 14 24)
            s[off++] = 0x4C;
            s[off++] = 0x8B;
            s[off++] = 0x14;
            s[off++] = 0x24;

            // MOV [RAX+120], R10 (4C 89 50 78)
            s[off++] = 0x4C;
            s[off++] = 0x89;
            s[off++] = 0x50;
            s[off++] = 0x78;

            // MOV R10, GS:[0x48] (65 4C 8B 14 25 48 00 00 00)
            s[off++] = 0x65;
            s[off++] = 0x4C;
            s[off++] = 0x8B;
            s[off++] = 0x14;
            s[off++] = 0x25;
            s[off++] = 0x48;
            s[off++] = 0x00;
            s[off++] = 0x00;
            s[off++] = 0x00;

            // MOV [RAX+128], R10 (4C 89 90 80 00 00 00)
            s[off++] = 0x4C;
            s[off++] = 0x89;
            s[off++] = 0x90;
            *(uint32_t *)(s + off) = 0x80;
            off += 4;

            // MOV RAX, imm64 (&Detour_SL_ConvertToString)
            s[off++] = 0x48;
            s[off++] = 0xB8;
            *(uint64_t *)(s + off) =
                (uint64_t)&Detour_SL_ConvertToString;
            off += 8;

            // JMP RAX (FF E0)
            s[off++] = 0xFF;
            s[off++] = 0xE0;

            FlushInstructionCache(GetCurrentProcess(), g_SLC_PreStub, off);

            char stubLog[256];
            sprintf_s(stubLog,
                      "[TextHook] SLC pre-stub built at %p (%d bytes), "
                      "regs struct at %p",
                      g_SLC_PreStub, off, &g_SLC_CallerRegs);
            LogToFile(stubLog);
          }

          // Write 14-byte absolute JMP at original function ??pre-stub
          // (or directly to detour if pre-stub allocation failed)
          void *jmpTarget =
              g_SLC_PreStub ? g_SLC_PreStub
                            : (void *)&Detour_SL_ConvertToString;

          DWORD oldProtect;
          if (VirtualProtect(g_SLC_TargetAddr, 14, PAGE_EXECUTE_READWRITE,
                             &oldProtect)) {
            bytes[0] = 0xFF;
            bytes[1] = 0x25;
            *(uint32_t *)(bytes + 2) = 0; // RIP-relative 0 = next 8 bytes
            *(uint64_t *)(bytes + 6) = (uint64_t)jmpTarget;

            DWORD dummy;
            VirtualProtect(g_SLC_TargetAddr, 14, oldProtect, &dummy);
            FlushInstructionCache(GetCurrentProcess(), g_SLC_TargetAddr, 14);

            g_SLC_HookApplied = true;
            char hookLog[256];
            sprintf_s(hookLog,
                      "[TextHook] SUCCESS: SLC hook applied! JMP ??%p "
                      "(pre-stub=%s)",
                      jmpTarget,
                      g_SLC_PreStub ? "YES" : "NO (fallback)");
            LogToFile(hookLog);
          } else {
            LogToFile("[TextHook] FAILED: VirtualProtect for SLC hook");
          }
        } else {
          LogToFile("[TextHook] SLC hook deferred: string table not loaded yet");
        }
      } else {
        LogToFile("[TextHook] SLC prologue mismatch - hook NOT applied");
      }
    }

    // === ConfigString idx->SLC hook (fundamental cfg mapping capture) ===
    if (!g_CfgToSlc_HookApplied) {
      if (!g_CfgToSlc_TargetAddr ||
          !IsProbablyExecutableCode(g_CfgToSlc_TargetAddr)) {
        ObjRev_ConfigIndexToSlcFn discovered = ObjRev_GetConfigIndexToSlcFn();
        if (discovered != nullptr) {
          void *discoveredAddr = (void *)discovered;
          if (g_CfgToSlc_TargetAddr != discoveredAddr) {
            g_CfgToSlc_TargetAddr = discoveredAddr;
            uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
            uintptr_t off = (moduleBase != 0)
                                ? ((uintptr_t)discoveredAddr - moduleBase)
                                : 0;
            char fbuf[192];
            sprintf_s(fbuf,
                      "[TextHook] cfg_to_slc target fallback resolved: +0x%llX",
                      (unsigned long long)off);
            LogToFile(fbuf);
          }
        }
      }
    }
    if (!g_CfgToSlc_HookApplied && g_CfgToSlc_TargetAddr) {
      static bool s_cfgToSlcBytesLogged = false;
      static DWORD s_cfgToSlcLastRetryLog = 0;
      if (!IsSafeRead(g_CfgToSlc_TargetAddr, 16)) {
        DWORD now = GetTickCount();
        if ((now - s_cfgToSlcLastRetryLog) > 2000) {
          s_cfgToSlcLastRetryLog = now;
          LogToFile("[TextHook] cfg_to_slc target unreadable; retry");
        }
      } else {
        if (!s_cfgToSlcBytesLogged) {
          s_cfgToSlcBytesLogged = true;
          unsigned char *b = (unsigned char *)g_CfgToSlc_TargetAddr;
          std::stringstream ss;
          ss << "[TextHook] cfg_to_slc prologue: ";
          for (int j = 0; j < 16; ++j) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)b[j]
               << " ";
          }
          LogToFile(ss.str());
        }

        CreateHook(g_CfgToSlc_TargetAddr, (void *)&Detour_ConfigString_IndexToSlc,
                   (void **)&Original_ConfigString_IndexToSlc, 14);
        if (Original_ConfigString_IndexToSlc) {
          g_CfgToSlc_HookApplied = true;
          LogToFile("[TextHook] SUCCESS: cfg_to_slc hook applied");
        } else {
          DWORD now = GetTickCount();
          if ((now - s_cfgToSlcLastRetryLog) > 1500) {
            s_cfgToSlcLastRetryLog = now;
            LogToFile(
                "[TextHook] FAILED: cfg_to_slc hook apply returned null original");
          }
        }
      }
    }

    if (!g_SL_StringTypeCheck_HookApplied && g_SL_StringTypeCheck_TargetAddr) {
      static bool s_typechkBytesLogged = false;
      if (!IsSafeRead(g_SL_StringTypeCheck_TargetAddr, 16) ||
          !IsProbablyExecutableCode(g_SL_StringTypeCheck_TargetAddr)) {
        static DWORD s_lastTypechkRetryLog = 0;
        DWORD now = GetTickCount();
        if ((now - s_lastTypechkRetryLog) > 2000) {
          s_lastTypechkRetryLog = now;
          LogToFile("[TextHook] sl_string_typecheck target unreadable; retry");
        }
      } else {
        if (!s_typechkBytesLogged) {
          s_typechkBytesLogged = true;
          unsigned char *b = (unsigned char *)g_SL_StringTypeCheck_TargetAddr;
          std::stringstream ss;
          ss << "[TextHook] sl_string_typecheck prologue: ";
          for (int j = 0; j < 16; ++j) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)b[j]
               << " ";
          }
          LogToFile(ss.str());
        }

        // Crash fix:
        // Do NOT detour 0x3DE320 with 14-byte stolen gateway.
        // Keep raw function pointer and use direct-call mode only.
        Original_SL_StringTypeCheck =
            (SL_StringTypeCheck_t)g_SL_StringTypeCheck_TargetAddr;
        g_SL_StringTypeCheck_HookApplied = (Original_SL_StringTypeCheck != nullptr);
        if (g_SL_StringTypeCheck_HookApplied) {
          LogToFile(
              "[TextHook] SUCCESS: sl_string_typecheck direct-call mode enabled (no detour)");
        }
      }
    }

    if (!g_CG_GameMessage_HookApplied && g_CG_GameMessage_TargetAddr) {
      static bool s_cgMsgBytesLogged = false;
      if (!IsSafeRead(g_CG_GameMessage_TargetAddr, 16) ||
          !IsProbablyExecutableCode(g_CG_GameMessage_TargetAddr)) {
        static DWORD s_lastCgMsgRetryLog = 0;
        DWORD now = GetTickCount();
        if ((now - s_lastCgMsgRetryLog) > 2000) {
          s_lastCgMsgRetryLog = now;
          LogToFile("[TextHook] CG_GameMessage target unreadable; retry");
        }
      } else {
        if (!s_cgMsgBytesLogged) {
          s_cgMsgBytesLogged = true;
          unsigned char *b = (unsigned char *)g_CG_GameMessage_TargetAddr;
          std::stringstream ss;
          ss << "[TextHook] CG_GameMessage prologue: ";
          for (int j = 0; j < 16; ++j) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)b[j]
               << " ";
          }
          LogToFile(ss.str());
        }

        CreateHook(g_CG_GameMessage_TargetAddr, (void *)&Detour_CG_GameMessage,
                   (void **)&Original_CG_GameMessage, 15);
        if (Original_CG_GameMessage) {
          g_CG_GameMessage_HookApplied = true;
          LogToFile("[TextHook] SUCCESS: CG_GameMessage hook applied");
        }
      }
    }

    if (g_SLC_HookApplied && g_CfgToSlc_HookApplied &&
        g_SL_StringTypeCheck_HookApplied) {
      ObjRev_ScanIndexLookupTablesOnce();
    }

    // === RENDERFN: Dump prologues + Hook HudTextResolve (0x1FC8B0) ===
    if (!g_HTR_Hooked) {
      uintptr_t mb = (uintptr_t)GetModuleHandleA(NULL);

      // === PHASE 1: Dump prologues of key functions (64 bytes each) ===
      {
        uintptr_t fnOffsets[] = {IW6Offsets::Profile::Rva_1F9400, IW6Offsets::HUD_DrawText_SP, IW6Offsets::TextFxRender_SP, IW6Offsets::Profile::Rva_1FC990,
                                 IW6Offsets::IntroTextLayout_SP, IW6Offsets::ConfigStringWrapper_SP, IW6Offsets::IntroRender_SP, IW6Offsets::IntroActualRender_SP};
        const char *fnNames[] = {
            "ParentHudRender(0x1F9400)",
            "HUD_DrawText(0x3FE560)",
            "RenderFn2(0x3F4A70)",
            "TextProc(0x1FC990)",
            "IntroTextLayout(0x3F3DE0)",
            "SLC_Wrapper(0x2349D0)",
            "IntroRenderFn(0x1F1740)",
            "IntroActualRender(0x416AE0)"};
        for (int fi = 0; fi < 8; fi++) {
          void *addr = (void *)(mb + fnOffsets[fi]);
          if (!IsBadReadPtr(addr, 64)) {
            unsigned char *b = (unsigned char *)addr;
            // Dump in two rows of 32 bytes
            for (int row = 0; row < 2; row++) {
              std::stringstream ss;
              ss << "[PROLOGUE] " << fnNames[fi];
              ss << (row == 0 ? " +00: " : " +20: ");
              for (int j = 0; j < 32; j++) {
                ss << std::hex << std::setw(2) << std::setfill('0')
                   << (int)b[row * 32 + j] << " ";
              }
              LogToFile(ss.str());
            }
          }
        }

        // Subtitle reverse candidates from runtime SEH-SUB stack traces.
        // Goal: identify the subtitle queue/remove lifecycle without blind hooks.
        uintptr_t subRevOffsets[] = {
            IW6Offsets::SubtitleWrapper_SP, // callsite container for SEH subtitle lookups (call=+0x235210)
            IW6Offsets::SubtitleEnqueue_SP, // called by 0x2351E0 with localized text pointer
            IW6Offsets::SubtitlePrecheck_SP, // pre-check call inside 0x2351E0
            IW6Offsets::Profile::Rva_1F622C, // upper caller from stack traces
            IW6Offsets::Profile::Rva_580D53, // script/flow side frame (changes by context)
            IW6Offsets::Profile::Rva_3E85AB, // script/flow side frame (changes by context)
            IW6Offsets::Profile::Rva_589C31, // branch frame near subtitle events
            IW6Offsets::SubtitleBranch_SP, // branch frame near subtitle events
            IW6Offsets::Profile::Rva_204C33  // large frame containing +0x204EE0 path
        };
        const char *subRevNames[] = {
            "SubRev_CallsiteFn(0x2351E0)",
            "SubRev_EnqueueFn(0x235620)",
            "SubRev_PreCheckFn(0x2417E0)",
            "SubRev_StackFn1(0x1F622C)",
            "SubRev_StackFn2(0x580D53)",
            "SubRev_StackFn3(0x3E85AB)",
            "SubRev_StackFn4(0x589C31)",
            "SubRev_StackFn5(0x241830)",
            "SubRev_StackFnBig(0x204C33)"};

        for (int i = 0; i < 9; ++i) {
          uintptr_t off = subRevOffsets[i];
          uintptr_t addr = mb + off;
          LogCodeBytes(subRevNames[i], addr, 96);
          LogRelativeCalls(subRevNames[i], addr, 96);
        }

        // Dump once during early init (best-effort). A second "late" dump runs
        // on first enqueue to catch dvars registered after the delayed thread.
        DumpSubtitleDvarCandidatesEarlyOnce();

        // Hook subtitle wrapper (+0x2351E0) to capture enqueue-time parameters.
        if (!g_SubtitleWrap2351E0_Hooked) {
          uintptr_t subWrapAddr = reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(mb, IW6Offsets::SubtitleWrapper_SP));
          unsigned char *subWrapBytes = (unsigned char *)subWrapAddr;
          if (!IsBadReadPtr(subWrapBytes, 16)) {
            // Expected prologue:
            // 48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18
            bool ok = (subWrapBytes[0] == 0x48 && subWrapBytes[1] == 0x89 &&
                       subWrapBytes[2] == 0x5C && subWrapBytes[5] == 0x48 &&
                       subWrapBytes[6] == 0x89 && subWrapBytes[7] == 0x6C);
            if (ok) {
              LogToFile(
                  "[TextHook] SubWrap(0x2351E0) prologue verified, hooking...");
              CreateHook((void *)subWrapAddr, (void *)&Detour_SubtitleWrap2351E0,
                         (void **)&Original_SubtitleWrap2351E0, 15);
              if (Original_SubtitleWrap2351E0) {
                g_SubtitleWrap2351E0_Hooked = true;
                LogToFile("[TextHook] SUCCESS: SubWrap (0x2351E0) hooked!");
              } else {
                LogToFile("[TextHook] FAILED: SubWrap (0x2351E0) hook");
              }
            } else {
              char b[160];
              sprintf_s(b,
                        "[TextHook] SubWrap(0x2351E0) prologue mismatch: %02X %02X %02X %02X %02X %02X %02X %02X",
                        subWrapBytes[0], subWrapBytes[1], subWrapBytes[2],
                        subWrapBytes[3], subWrapBytes[4], subWrapBytes[5],
                        subWrapBytes[6], subWrapBytes[7]);
              LogToFile(b);
            }
          }
        }

        // Hook enqueue candidate (+0x235620) to capture lifecycle args.
        if (!g_SubtitleEnqueue235620_Hooked) {
          uintptr_t subEnqAddr = reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(mb, IW6Offsets::SubtitleEnqueue_SP));
          unsigned char *subEnqBytes = (unsigned char *)subEnqAddr;
          if (!IsBadReadPtr(subEnqBytes, 16)) {
            bool ok = (subEnqBytes[0] == 0x40 || subEnqBytes[0] == 0x48 ||
                       subEnqBytes[0] == 0x53 || subEnqBytes[0] == 0x55);
            if (ok) {
              char b[160];
              sprintf_s(
                  b,
                  "[TextHook] SubEnq(0x235620) prologue lead: %02X %02X %02X %02X %02X %02X %02X %02X",
                  subEnqBytes[0], subEnqBytes[1], subEnqBytes[2],
                  subEnqBytes[3], subEnqBytes[4], subEnqBytes[5],
                  subEnqBytes[6], subEnqBytes[7]);
              LogToFile(b);

              CreateHook((void *)subEnqAddr,
                         (void *)&Detour_SubtitleEnqueue235620,
                         (void **)&Original_SubtitleEnqueue235620, 15);
              if (Original_SubtitleEnqueue235620) {
                g_SubtitleEnqueue235620_Hooked = true;
                LogToFile("[TextHook] SUCCESS: SubEnq (0x235620) hooked!");
              } else {
                LogToFile("[TextHook] FAILED: SubEnq (0x235620) hook");
              }
            } else {
              char b[160];
              sprintf_s(
                  b,
                  "[TextHook] SubEnq(0x235620) prologue mismatch: %02X %02X %02X %02X %02X %02X %02X %02X",
                  subEnqBytes[0], subEnqBytes[1], subEnqBytes[2],
                  subEnqBytes[3], subEnqBytes[4], subEnqBytes[5],
                  subEnqBytes[6], subEnqBytes[7]);
              LogToFile(b);
            }
          } else {
            LogToFile("[TextHook] SubEnq(0x235620) unreadable");
          }
        }
      }

      // NOTE: PHASE 2 (ARGSETUP), PHASE 3 (SLCCONTEXT), PHASE 4 (PARENTDUMP)
      // removed - all fully analyzed as of 2026-02-09

      // Hook 0x1FC8B0 with pre-stub for register capture
      g_HTR_Addr = (void *)(reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(mb, IW6Offsets::HudTextRender_SP)));
      g_HTR_PreStub = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
      if (g_HTR_PreStub) {
        unsigned char *s = (unsigned char *)g_HTR_PreStub;
        int off = 0;

        // === 1. Save RSP to g_HTR_CallerRSP (before any modification) ===
        // MOV RAX, imm64 (&g_HTR_CallerRSP)
        s[off++] = 0x48; s[off++] = 0xB8;
        *(uint64_t *)(s + off) = (uint64_t)&g_HTR_CallerRSP;
        off += 8;
        // MOV [RAX], RSP  (48 89 20)
        s[off++] = 0x48; s[off++] = 0x89; s[off++] = 0x20;

        // === 2. Save non-volatile registers to g_HTR_CallerRegs ===
        // MOV RAX, imm64 (&g_HTR_CallerRegs)
        s[off++] = 0x48; s[off++] = 0xB8;
        *(uint64_t *)(s + off) = (uint64_t)&g_HTR_CallerRegs;
        off += 8;

        // Save non-volatile registers
        s[off++] = 0x48; s[off++] = 0x89; s[off++] = 0x38;             // [RAX+0] = RDI
        s[off++] = 0x48; s[off++] = 0x89; s[off++] = 0x70; s[off++] = 0x08; // [RAX+8] = RSI
        s[off++] = 0x48; s[off++] = 0x89; s[off++] = 0x58; s[off++] = 0x10; // [RAX+16]= RBX
        s[off++] = 0x48; s[off++] = 0x89; s[off++] = 0x68; s[off++] = 0x18; // [RAX+24]= RBP
        s[off++] = 0x4C; s[off++] = 0x89; s[off++] = 0x60; s[off++] = 0x20; // [RAX+32]= R12
        s[off++] = 0x4C; s[off++] = 0x89; s[off++] = 0x68; s[off++] = 0x28; // [RAX+40]= R13
        s[off++] = 0x4C; s[off++] = 0x89; s[off++] = 0x70; s[off++] = 0x30; // [RAX+48]= R14
        s[off++] = 0x4C; s[off++] = 0x89; s[off++] = 0x78; s[off++] = 0x38; // [RAX+56]= R15

        // === 3. JMP to C++ detour function ===
        // MOV RAX, imm64 (&Detour_HudTextResolve)
        s[off++] = 0x48; s[off++] = 0xB8;
        *(uint64_t *)(s + off) = (uint64_t)&Detour_HudTextResolve;
        off += 8;

        // JMP RAX
        s[off++] = 0xFF; s[off++] = 0xE0;

        FlushInstructionCache(GetCurrentProcess(), g_HTR_PreStub, off);

        char log[256];
        sprintf_s(log, "[TextHook] HTR pre-stub at %p (%d bytes)", g_HTR_PreStub, off);
        LogToFile(log);

        // Use Detour::Create via CreateHook: target??멸콎eStub, gateway??맞riginal
        // Prologue analysis: instruction boundary at offset 15
        //   0-4: MOV [RSP+8],RBX (5) | 5: PUSH RDI (1) | 6-12: SUB RSP,0x520 (7)
        //   13-14: MOV EDI,ECX (2) | 15+: MOV ECX,[RIP+...] (6, RIP-relative)
        // stolenBytes=15 avoids cutting the RIP-relative instruction
        CreateHook(g_HTR_Addr, g_HTR_PreStub,
                   (void **)&Original_HudTextResolve, 15);

        if (Original_HudTextResolve) {
          g_HTR_Hooked = true;
          LogToFile("[TextHook] SUCCESS: HudTextResolve (0x1FC8B0) hooked!");
        } else {
          LogToFile("[TextHook] FAILED: HudTextResolve (0x1FC8B0) hook");
        }
      } else {
        LogToFile("[TextHook] FAILED: VirtualAlloc for HTR pre-stub");
      }
    }

    // OSREV chain hooks removed — OSAUTH is the sole authority.

    // === IntroTextLayout Hook (0x3F3DE0) - Intro/chyron text rendering ===
    if (!g_ITL_Hooked && g_SLC_HookApplied) {
      uintptr_t itlBase = (uintptr_t)GetModuleHandleA(NULL);
      uintptr_t itlAddr = reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(itlBase, IW6Offsets::IntroTextLayout_SP));
      unsigned char *itlBytes = (unsigned char *)itlAddr;

      // Verify prologue matches expected bytes
      // 48 89 5c 24 10 = MOV [RSP+10h], RBX
      // 44 89 44 24 18 = MOV [RSP+18h], R8D
      if (itlBytes[0] == 0x48 && itlBytes[1] == 0x89 && itlBytes[2] == 0x5C &&
          itlBytes[5] == 0x44 && itlBytes[6] == 0x89) {
        LogToFile("[TextHook] IntroTextLayout prologue verified, hooking...");

        // Prologue: MOV [RSP+10],RBX(5) + MOV [RSP+18],R8D(5) +
        //   PUSH RBP(1) + PUSH RSI(1) + PUSH RDI(1) + PUSH R12(2) = 15
        // No RIP-relative instructions in stolen range
        CreateHook((void *)itlAddr, (void *)&Detour_IntroTextLayout,
                   (void **)&Original_IntroTextLayout, 15);

        if (Original_IntroTextLayout) {
          g_ITL_Hooked = true;
          LogToFile("[TextHook] SUCCESS: IntroTextLayout (0x3F3DE0) hooked!");
        } else {
          LogToFile("[TextHook] FAILED: IntroTextLayout (0x3F3DE0) hook");
        }
      } else {
        char byteBuf[128];
        sprintf_s(byteBuf,
                  "[TextHook] IntroTextLayout prologue mismatch: %02X %02X %02X %02X "
                  "%02X %02X %02X",
                  itlBytes[0], itlBytes[1], itlBytes[2], itlBytes[3],
                  itlBytes[4], itlBytes[5], itlBytes[6]);
        LogToFile(byteBuf);
      }
    }

    // === IntroRenderFn Hook (0x1F1740) - Captures render context arg3 ===
    if (!g_IRF_Hooked && g_ITL_Hooked) {
      uintptr_t irfBase = (uintptr_t)GetModuleHandleA(NULL);
      uintptr_t irfAddr = reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(irfBase, IW6Offsets::IntroRender_SP));
      unsigned char *irfBytes = (unsigned char *)irfAddr;

      // Verify prologue: 48 89 5c 24 08 = MOV [RSP+8], RBX
      //                  48 89 74 24 10 = MOV [RSP+10h], RSI
      if (irfBytes[0] == 0x48 && irfBytes[1] == 0x89 && irfBytes[2] == 0x5C &&
          irfBytes[5] == 0x48 && irfBytes[6] == 0x89 && irfBytes[7] == 0x74) {
        LogToFile("[TextHook] IntroRenderFn prologue verified, hooking...");

        // Prologue: MOV [RSP+8],RBX(5) + MOV [RSP+10],RSI(5) +
        //   PUSH RDI(1) + SUB RSP,0x20(4) = 15 bytes
        // No RIP-relative instructions in stolen range
        CreateHook((void *)irfAddr, (void *)&Detour_IntroRenderFn,
                   (void **)&Original_IntroRenderFn, 15);

        if (Original_IntroRenderFn) {
          g_IRF_Hooked = true;
          LogToFile("[TextHook] SUCCESS: IntroRenderFn (0x1F1740) hooked!");

          // === CALLSCAN+HEXDUMP: 0x416AE0 (IntroActualRender) ===
          // IntroRenderFn is only 79 bytes and TAIL-JMPs to 0x416AE0.
          // 0x416AE0 receives (type, layoutResult, renderCtx, count)
          // and is the REAL rendering function. Scan it for E8/E9/FF15.
          {
            unsigned char *fnStart = (unsigned char *)(reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(irfBase, IW6Offsets::IntroActualRender_SP)));
            const int scanLen = 4096;
            int callCount = 0;
            LogToFile("[SCAN416] Scanning IntroActualRender (0x416AE0)...");

            // Hex dump first 512 bytes for disassembly
            for (int row = 0; row < 512; row += 32) {
              std::stringstream ss;
              ss << "[HEX416] +" << std::hex << std::setw(4)
                 << std::setfill('0') << row << ": ";
              for (int bi = row; bi < row + 32 && bi < 512; bi++) {
                ss << std::hex << std::setw(2) << std::setfill('0')
                   << (int)fnStart[bi] << " ";
              }
              LogToFile(ss.str());
            }

            // Scan for E8 CALL, E9 JMP, FF15 indirect CALL
            for (int ci = 0; ci < scanLen - 5; ci++) {
              unsigned char *p = fnStart + ci;

              if (p[0] == 0xE8) {
                int32_t rel = *(int32_t *)(p + 1);
                uintptr_t target = (uintptr_t)(p + 5) + rel;
                uintptr_t offset = target - irfBase;
                // Filter out bogus targets (must be in module range)
                if (offset < 0x8000000) {
                  char buf[256];
                  sprintf_s(buf, "[SCAN416] +%04X: E8 CALL -> base+0x%llX",
                            ci, (unsigned long long)offset);
                  LogToFile(buf);
                  callCount++;
                }
              }
              else if (p[0] == 0xE9) {
                int32_t rel = *(int32_t *)(p + 1);
                uintptr_t target = (uintptr_t)(p + 5) + rel;
                uintptr_t offset = target - irfBase;
                if (offset < 0x8000000 && ci > 8) {
                  char buf[256];
                  sprintf_s(buf, "[SCAN416] +%04X: E9 JMP -> base+0x%llX",
                            ci, (unsigned long long)offset);
                  LogToFile(buf);
                  callCount++;
                }
              }
              else if (p[0] == 0xFF && p[1] == 0x15) {
                int32_t rel = *(int32_t *)(p + 2);
                uintptr_t ptrAddr = (uintptr_t)(p + 6) + rel;
                char buf[256];
                sprintf_s(buf, "[SCAN416] +%04X: FF15 CALL [RIP] -> ptr at base+0x%llX",
                          ci, (unsigned long long)(ptrAddr - irfBase));
                LogToFile(buf);
                callCount++;
              }
              // Function end: RET + padding or next function prologue
              if (p[0] == 0xC3 && ci > 32) {
                if (ci + 1 < scanLen && (p[1] == 0xCC || p[1] == 0x90 ||
                    (p[1] == 0x48 && p[2] == 0x89))) {
                  char buf[128];
                  sprintf_s(buf, "[SCAN416] Function end at +%04X (RET)", ci);
                  LogToFile(buf);
                  break;
                }
              }
            }
            char buf[128];
            sprintf_s(buf, "[SCAN416] Scan complete. Found %d CALL/JMP instructions.", callCount);
            LogToFile(buf);
          }

          if (!g_IAR_Hooked) {
            uintptr_t iarAddr = reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(irfBase, IW6Offsets::IntroActualRender_SP));
            unsigned char *iarBytes = (unsigned char *)iarAddr;
            const bool introActualRenderPrologueOk =
                iarBytes[0] == 0x48 && iarBytes[1] == 0x89 &&
                iarBytes[2] == 0x5C && iarBytes[3] == 0x24 &&
                iarBytes[4] == 0x18 && iarBytes[5] == 0x89 &&
                iarBytes[6] == 0x4C && iarBytes[7] == 0x24 &&
                iarBytes[8] == 0x08 && iarBytes[9] == 0x55 &&
                iarBytes[10] == 0x56 && iarBytes[11] == 0x57 &&
                iarBytes[12] == 0x41 && iarBytes[13] == 0x54 &&
                iarBytes[14] == 0x41 && iarBytes[15] == 0x55 &&
                iarBytes[16] == 0x41 && iarBytes[17] == 0x56 &&
                iarBytes[18] == 0x41 && iarBytes[19] == 0x57;
            if (introActualRenderPrologueOk) {
              LogToFile(
                  "[TextHook] IntroActualRender(0x416AE0) prologue verified, hooking...");
              CreateHook((void *)iarAddr, (void *)&Detour_IntroActualRender,
                         (void **)&Original_IntroActualRender, 20);
              if (Original_IntroActualRender) {
                g_IAR_Hooked = true;
                LogToFile(
                    "[TextHook] SUCCESS: IntroActualRender (0x416AE0) hooked!");
              } else {
                LogToFile(
                    "[TextHook] FAILED: IntroActualRender (0x416AE0) hook");
              }
            } else {
              char iarBuf[192];
              sprintf_s(
                  iarBuf,
                  "[TextHook] IntroActualRender prologue mismatch: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                  iarBytes[0], iarBytes[1], iarBytes[2], iarBytes[3],
                  iarBytes[4], iarBytes[5], iarBytes[6], iarBytes[7],
                  iarBytes[8], iarBytes[9]);
              LogToFile(iarBuf);
            }
          }

          // === HEX DUMP + CALLSCAN: 0x1F0CE0 (ViewportSetup) ===
          // 0x1F17A0 calls this function 2x for lerp.
          // Safe area calculation is INSIDE this function.
          // Dump 1024 bytes + scan for MULSS/ADDSS/CALL patterns.
          {
            unsigned char *vpStart = (unsigned char *)(reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(irfBase, IW6Offsets::IntroSubRender_SP)));
            LogToFile("[HEXVP] Dumping 0x1F0CE0 (ViewportSetup)...");
            int vpLen = 1024;
            for (int row = 0; row < vpLen; row += 32) {
              std::stringstream ss;
              ss << "[HEXVP] +" << std::hex << std::setw(4)
                 << std::setfill('0') << row << ": ";
              int end = row + 32;
              if (end > vpLen) end = vpLen;
              for (int bi = row; bi < end; bi++) {
                ss << std::hex << std::setw(2) << std::setfill('0')
                   << (int)vpStart[bi] << " ";
              }
              LogToFile(ss.str());
            }

            // Scan all F3 0F xx instructions (MOVSS/MULSS/ADDSS/SUBSS/DIVSS)
            // to find RIP-relative memory references (float constants + globals)
            int foundCount = 0;
            for (int ci = 0; ci < vpLen - 8; ci++) {
              unsigned char *p = vpStart + ci;
              if (p[0] != 0xF3 || p[1] != 0x0F) continue;
              // Check for [RIP+disp32] addressing: ModR/M byte & 0xC7 == 0x05
              // Opcodes: 10=MOVSS, 11=MOVSS store, 58=ADDSS, 59=MULSS, 5C=SUBSS, 5E=DIVSS
              unsigned char op = p[2];
              if (op != 0x10 && op != 0x58 && op != 0x59 &&
                  op != 0x5C && op != 0x5E) continue;
              unsigned char modrm = p[3];
              if ((modrm & 0xC7) != 0x05) continue; // [RIP+disp32]
              int reg = (modrm >> 3) & 7;
              int32_t disp = *(int32_t *)(p + 4);
              uintptr_t target = (uintptr_t)(p + 8) + disp;
              uintptr_t offset = target - irfBase;
              float val = 0.0f;
              if (offset < 0x8000000 && !IsBadReadPtr((void *)target, 4)) {
                val = *(volatile float *)target;
              }
              const char *opName = "???";
              if (op == 0x10) opName = "MOVSS";
              else if (op == 0x58) opName = "ADDSS";
              else if (op == 0x59) opName = "MULSS";
              else if (op == 0x5C) opName = "SUBSS";
              else if (op == 0x5E) opName = "DIVSS";
              char buf2[256];
              sprintf_s(buf2,
                "[VPSCAN] +%04X: %s XMM%d,[RIP+0x%X] -> base+0x%llX = %.6f",
                ci, opName, reg, (unsigned)disp,
                (unsigned long long)offset, val);
              LogToFile(buf2);
              foundCount++;
            }

            // Also scan E8 CALLs
            for (int ci = 0; ci < vpLen - 5; ci++) {
              if (vpStart[ci] == 0xE8) {
                int32_t rel = *(int32_t *)(vpStart + ci + 1);
                uintptr_t target = (uintptr_t)(vpStart + ci + 5) + rel;
                uintptr_t offset = target - irfBase;
                if (offset < 0x8000000) {
                  char buf2[256];
                  sprintf_s(buf2, "[VPSCAN] +%04X: E8 CALL -> base+0x%llX",
                    ci, (unsigned long long)offset);
                  LogToFile(buf2);
                  foundCount++;
                }
              }
              // RET detection
              if (vpStart[ci] == 0xC3 && ci > 32) {
                if (ci + 1 < vpLen && (vpStart[ci+1] == 0xCC ||
                    vpStart[ci+1] == 0x90 ||
                    (vpStart[ci+1] == 0x48 && vpStart[ci+2] == 0x89))) {
                  char buf2[128];
                  sprintf_s(buf2, "[VPSCAN] Function end at +%04X (RET)", ci);
                  LogToFile(buf2);
                  break;
                }
              }
            }
            char buf2[128];
            sprintf_s(buf2, "[VPSCAN] Scan complete. %d entries.", foundCount);
            LogToFile(buf2);
          }
        } else {
          LogToFile("[TextHook] FAILED: IntroRenderFn (0x1F1740) hook");
        }
      } else {
        char byteBuf[128];
        sprintf_s(byteBuf,
                  "[TextHook] IntroRenderFn prologue mismatch: %02X %02X %02X "
                  "%02X %02X %02X %02X %02X",
                  irfBytes[0], irfBytes[1], irfBytes[2], irfBytes[3],
                  irfBytes[4], irfBytes[5], irfBytes[6], irfBytes[7]);
        LogToFile(byteBuf);
      }
    }

    // === IRFSub1 Hook (0x1F0CE0) - Called 3x from IntroRenderFn ===
    // This function is the most likely candidate for the actual text
    // rendering / coordinate-transform call within IntroRenderFn.
    // Disabled in production: this analysis hook is extremely hot and causes
    // heavy log/CPU overhead.
    if (false && !g_IRFSub1_Hooked && g_IRF_Hooked) {
      uintptr_t sub1Base = (uintptr_t)GetModuleHandleA(NULL);
      uintptr_t sub1Addr = reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(sub1Base, IW6Offsets::IntroSubRender_SP));
      unsigned char *sub1Bytes = (unsigned char *)sub1Addr;

      // Log prologue for analysis
      {
        std::stringstream ss;
        ss << "[TextHook] IRFSub1(0x1F0CE0) prologue: ";
        for (int bi = 0; bi < 20; bi++) {
          ss << std::hex << std::setw(2) << std::setfill('0')
             << (int)sub1Bytes[bi] << " ";
        }
        LogToFile(ss.str());
      }

      // Try to determine stolenBytes from prologue pattern
      int sub1Stolen = 0;

      // Pattern 1: MOV [RSP+8],RBX(5) + MOV [RSP+10],RSI(5) + PUSH(1) + SUB RSP(4) = 15
      if (sub1Bytes[0] == 0x48 && sub1Bytes[1] == 0x89 &&
          sub1Bytes[5] == 0x48 && sub1Bytes[6] == 0x89) {
        sub1Stolen = 15;
        LogToFile("[TextHook] IRFSub1 prologue: MOV+MOV pattern, stolenBytes=15");
      }
      // Pattern 2: SUB RSP,xx (48 83 EC xx = 4 bytes) ??too short, need more
      // Pattern 3: MOV [RSP+8],RCX(5) + SUB RSP,xx(4) = look at next bytes
      else if (sub1Bytes[0] == 0x48 && sub1Bytes[1] == 0x83 && sub1Bytes[2] == 0xEC) {
        // SUB RSP, imm8 = 4 bytes. Need more context.
        // Check if followed by more instructions that total >= 14
        sub1Stolen = 14; // minimum, may crash
        LogToFile("[TextHook] IRFSub1 prologue: SUB RSP pattern, stolenBytes=14 (risky)");
      }
      // Pattern 4: PUSH RBX (40 53) or similar
      else if (sub1Bytes[0] == 0x40 || sub1Bytes[0] == 0x41 ||
               sub1Bytes[0] == 0x48 || sub1Bytes[0] == 0x4C) {
        // REX prefix family - assume standard prologue
        sub1Stolen = 15;
        LogToFile("[TextHook] IRFSub1 prologue: REX prefix, trying stolenBytes=15");
      }

      if (sub1Stolen >= 14) {
        CreateHook((void *)sub1Addr, (void *)&Detour_IRFSub1,
                   (void **)&Original_IRFSub1, sub1Stolen);
        if (Original_IRFSub1) {
          g_IRFSub1_Hooked = true;
          LogToFile("[TextHook] SUCCESS: IRFSub1 (0x1F0CE0) hooked!");
        } else {
          LogToFile("[TextHook] FAILED: IRFSub1 (0x1F0CE0) hook");
        }
      } else {
        LogToFile("[TextHook] IRFSub1: Cannot determine stolenBytes, skipping hook");
      }
    }

    // === IRFSub2 Hook (0x6275A0) - Called 2x from IntroRenderFn ===
    // Prologue: 48 83 EC 58 = SUB RSP, 0x58 (4 bytes, too short for 14-byte JMP)
    // Need to check total instruction boundary. SUB RSP,0x58 (4) + next instructions...
    // Actually 48 83 EC 58 = SUB RSP, imm8 is 4 bytes with REX prefix.
    // Followed by: 45 33 C0(3) = XOR R8D, R8D  [total 7]
    //              0F 28 C8(3) = MOVAPS XMM1, XMM0  [total 10]
    //              F3 0F 11 4C 24 60(6) = MOVSS [RSP+60h], XMM1  [total 16 >= 14!]
    // === IRFSub2 Hook (0x6275A0) - DISABLED: caused black screen crash ===
    // stolenBytes=16 with MOVSS [RSP+60h] in stolen range may corrupt
    // trampoline stack. Function is called too frequently from many callers.
    // Investigate via CALLSCAN_ITL instead.
    if (false && !g_IRFSub2_Hooked && g_IRF_Hooked) {
      // disabled
    }

    // === R_AddCmdDrawTextWithCursor Hook - Text input, possibly subtitles ===
    if (!g_CursorTextHookApplied && g_CursorTextTargetAddr) {
      unsigned char *bytes = (unsigned char *)g_CursorTextTargetAddr;

      // Log bytes for debugging (always log once)
      static bool cursorBytesLogged = false;
      if (!cursorBytesLogged) {
        std::stringstream ss;
        ss << "[TextHook] R_AddCmdDrawTextWithCursor bytes at 0x" << std::hex
           << g_CursorTextTargetAddr << ": ";
        for (int j = 0; j < 16; j++) {
          ss << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[j]
             << " ";
        }
        LogToFile(ss.str());
        cursorBytesLogged = true;
      }

      // Only skip if code is clearly not ready (int3 breakpoints)
      bool codeNotReady = (bytes[0] == 0xCC && bytes[1] == 0xCC);
      if (!codeNotReady) {
        LogToFile("[TextHook] R_AddCmdDrawTextWithCursor applying hook...");

        // Same signature as R_AddCmdDrawText - sub rsp (4), mov rax (7) = 11+
        // bytes
        CreateHook(g_CursorTextTargetAddr,
                   (void *)&Detour_R_AddCmdDrawTextWithCursor,
                   (void **)&Original_R_AddCmdDrawTextWithCursor, 14);

        if (Original_R_AddCmdDrawTextWithCursor) {
          g_CursorTextHookApplied = true;
          LogToFile(
              "[TextHook] SUCCESS: R_AddCmdDrawTextWithCursor hook applied!");
        } else {
          LogToFile(
              "[TextHook] WARNING: R_AddCmdDrawTextWithCursor hook failed.");
        }
      }
    }

    // === TimeScript HUD draw Hook (0x555EA0) - dedicated countdown HUD ===
    if (needTimeScriptHook && !g_TimeScriptHooked) {
      const uintptr_t tsBase = (uintptr_t)GetModuleHandleA(NULL);
      const uintptr_t tsAddr = tsBase + IW6Offsets::Profile::Rva_555EA0;
      if (!IsProbablyExecutableCode((void *)tsAddr)) {
        static DWORD s_lastTimeScriptDefer = 0;
        const DWORD nowDefer = GetTickCount();
        if ((nowDefer - s_lastTimeScriptDefer) > 5000) {
          s_lastTimeScriptDefer = nowDefer;
          LogToFile(
              "[TextHook] TimeScript HUD hook deferred (0x555EA0 not executable yet)");
        }
      } else {
        if (!g_TimeScriptPreStub) {
          g_TimeScriptPreStub =
              VirtualAlloc(NULL, 512, MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
        }

        if (g_TimeScriptPreStub) {
          unsigned char *s = (unsigned char *)g_TimeScriptPreStub;
          int off = 0;

          auto emit8 = [&](unsigned char v) { s[off++] = v; };
          auto emit32 = [&](uint32_t v) {
            *(uint32_t *)(s + off) = v;
            off += 4;
          };
          auto emit64 = [&](uint64_t v) {
            *(uint64_t *)(s + off) = v;
            off += 8;
          };

          // sub rsp, 0xB8
          emit8(0x48); emit8(0x81); emit8(0xEC); emit32(0x000000B8);

          // Save caller-saved GPRs.
          emit8(0x48); emit8(0x89); emit8(0x4C); emit8(0x24); emit8(0x20); // rcx
          emit8(0x48); emit8(0x89); emit8(0x54); emit8(0x24); emit8(0x28); // rdx
          emit8(0x4C); emit8(0x89); emit8(0x44); emit8(0x24); emit8(0x30); // r8
          emit8(0x4C); emit8(0x89); emit8(0x4C); emit8(0x24); emit8(0x38); // r9
          emit8(0x4C); emit8(0x89); emit8(0x54); emit8(0x24); emit8(0x40); // r10
          emit8(0x4C); emit8(0x89); emit8(0x5C); emit8(0x24); emit8(0x48); // r11
          emit8(0x48); emit8(0x89); emit8(0x44); emit8(0x24); emit8(0x50); // rax

          // Save caller-saved XMMs (float args may live here).
          emit8(0xF3); emit8(0x0F); emit8(0x7F); emit8(0x44); emit8(0x24); emit8(0x60); // xmm0
          emit8(0xF3); emit8(0x0F); emit8(0x7F); emit8(0x4C); emit8(0x24); emit8(0x70); // xmm1
          emit8(0xF3); emit8(0x0F); emit8(0x7F); emit8(0x54); emit8(0x24); emit8(0x80); // xmm2
          emit8(0xF3); emit8(0x0F); emit8(0x7F); emit8(0x5C); emit8(0x24); emit8(0x90); // xmm3
          emit8(0xF3); emit8(0x0F); emit8(0x7F); emit8(0x64); emit8(0x24); emit8(0xA0); // xmm4
          emit8(0xF3); emit8(0x0F); emit8(0x7F); emit8(0x6C); emit8(0x24); emit8(0xB0); // xmm5

          // Save entry RSP/RBX... to g_TimeScriptCallerRegs.
          emit8(0x48); emit8(0xB8); emit64((uint64_t)&g_TimeScriptCallerRegs); // mov rax, &regs
          emit8(0x48); emit8(0x8D); emit8(0x8C); emit8(0x24); emit32(0x000000B8); // lea rcx, [rsp+0xB8]
          emit8(0x48); emit8(0x89); emit8(0x08); // [rax+0] = rcx(entry rsp)
          emit8(0x48); emit8(0x89); emit8(0x58); emit8(0x08); // [rax+8] = rbx
          emit8(0x48); emit8(0x89); emit8(0x68); emit8(0x10); // [rax+16] = rbp
          emit8(0x48); emit8(0x89); emit8(0x78); emit8(0x18); // [rax+24] = rdi
          emit8(0x48); emit8(0x89); emit8(0x70); emit8(0x20); // [rax+32] = rsi
          emit8(0x4C); emit8(0x89); emit8(0x60); emit8(0x28); // [rax+40] = r12
          emit8(0x4C); emit8(0x89); emit8(0x68); emit8(0x30); // [rax+48] = r13
          emit8(0x4C); emit8(0x89); emit8(0x70); emit8(0x38); // [rax+56] = r14
          emit8(0x4C); emit8(0x89); emit8(0x78); emit8(0x40); // [rax+64] = r15
          emit8(0x48); emit8(0x8B); emit8(0x09); // mov rcx, [rcx]
          emit8(0x48); emit8(0x89); emit8(0x48); emit8(0x48); // [rax+72] = ret

          // Call pre-hook helper.
          emit8(0x48); emit8(0xB8); emit64((uint64_t)&TimeScript_PreDrawHook);
          emit8(0xFF); emit8(0xD0);

          // Restore caller-saved XMMs.
          emit8(0xF3); emit8(0x0F); emit8(0x6F); emit8(0x44); emit8(0x24); emit8(0x60); // xmm0
          emit8(0xF3); emit8(0x0F); emit8(0x6F); emit8(0x4C); emit8(0x24); emit8(0x70); // xmm1
          emit8(0xF3); emit8(0x0F); emit8(0x6F); emit8(0x54); emit8(0x24); emit8(0x80); // xmm2
          emit8(0xF3); emit8(0x0F); emit8(0x6F); emit8(0x5C); emit8(0x24); emit8(0x90); // xmm3
          emit8(0xF3); emit8(0x0F); emit8(0x6F); emit8(0x64); emit8(0x24); emit8(0xA0); // xmm4
          emit8(0xF3); emit8(0x0F); emit8(0x6F); emit8(0x6C); emit8(0x24); emit8(0xB0); // xmm5

          // Restore caller-saved GPRs.
          emit8(0x48); emit8(0x8B); emit8(0x4C); emit8(0x24); emit8(0x20); // rcx
          emit8(0x48); emit8(0x8B); emit8(0x54); emit8(0x24); emit8(0x28); // rdx
          emit8(0x4C); emit8(0x8B); emit8(0x44); emit8(0x24); emit8(0x30); // r8
          emit8(0x4C); emit8(0x8B); emit8(0x4C); emit8(0x24); emit8(0x38); // r9
          emit8(0x4C); emit8(0x8B); emit8(0x54); emit8(0x24); emit8(0x40); // r10
          emit8(0x4C); emit8(0x8B); emit8(0x5C); emit8(0x24); emit8(0x48); // r11
          emit8(0x48); emit8(0x8B); emit8(0x44); emit8(0x24); emit8(0x50); // rax

          // Call original trampoline (pointer loaded after CreateHook).
          emit8(0x48); emit8(0xB8); emit64((uint64_t)&Original_TimeScriptDraw);
          emit8(0x48); emit8(0x8B); emit8(0x00);
          emit8(0xFF); emit8(0xD0);

          // Save return regs around post-helper.
          emit8(0x48); emit8(0x89); emit8(0x44); emit8(0x24); emit8(0x20); // rax
          emit8(0x48); emit8(0x89); emit8(0x54); emit8(0x24); emit8(0x28); // rdx
          emit8(0xF3); emit8(0x0F); emit8(0x7F); emit8(0x44); emit8(0x24); emit8(0x60); // xmm0
          emit8(0xF3); emit8(0x0F); emit8(0x7F); emit8(0x4C); emit8(0x24); emit8(0x70); // xmm1

          emit8(0x48); emit8(0xB8); emit64((uint64_t)&TimeScript_PostDrawHook);
          emit8(0xFF); emit8(0xD0);

          // Restore return regs and return to caller.
          emit8(0xF3); emit8(0x0F); emit8(0x6F); emit8(0x44); emit8(0x24); emit8(0x60); // xmm0
          emit8(0xF3); emit8(0x0F); emit8(0x6F); emit8(0x4C); emit8(0x24); emit8(0x70); // xmm1
          emit8(0x48); emit8(0x8B); emit8(0x44); emit8(0x24); emit8(0x20); // rax
          emit8(0x48); emit8(0x8B); emit8(0x54); emit8(0x24); emit8(0x28); // rdx
          emit8(0x48); emit8(0x81); emit8(0xC4); emit32(0x000000B8); // add rsp, 0xB8
          emit8(0xC3); // ret

          FlushInstructionCache(GetCurrentProcess(), g_TimeScriptPreStub, off);
        }

        if (g_TimeScriptPreStub && !g_TimeScriptHooked) {
          char pbuf[192];
          sprintf_s(pbuf,
                    "[TextHook] TimeScript pre-stub ready at %p target=0x%llX",
                    g_TimeScriptPreStub, (unsigned long long)tsAddr);
          LogToFile(pbuf);
          CreateHook((void *)tsAddr, g_TimeScriptPreStub,
                     (void **)&Original_TimeScriptDraw, 16);
          if (Original_TimeScriptDraw) {
            g_TimeScriptHooked = true;
            LogToFile(
                "[TextHook] SUCCESS: TimeScript HUD draw (0x555EA0) hooked!");
          } else {
            LogToFile(
                "[TextHook] FAILED: TimeScript HUD draw (0x555EA0) hook");
          }
        }
      }
    }

    // === HUD_DrawText Hook (0x3FE560) - Captures final screen coords ===
    // Do not gate this on SLC; native capture is independent and should be ready
    // whenever the function body becomes executable.
    if (!g_HDT_Hooked) {
      uintptr_t hdtBase = (uintptr_t)GetModuleHandleA(NULL);
      uintptr_t hdtAddr = reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(hdtBase, IW6Offsets::HUD_DrawText_SP));
      unsigned char *hdtBytes = (unsigned char *)hdtAddr;

      if (!IsProbablyExecutableCode((void *)hdtAddr)) {
        // Defer until the code section is decrypted/executable.
        static DWORD s_lastHdtDefer = 0;
        DWORD nowDefer = GetTickCount();
        if (nowDefer - s_lastHdtDefer > 5000) {
          s_lastHdtDefer = nowDefer;
          LogToFile("[TextHook] HUD_DrawText hook deferred (code not executable yet)");
        }
      }
      else
      // Verify prologue: 48 89 5C 24 08 = MOV [RSP+8], RBX
      //                  48 89 6C 24 10 = MOV [RSP+10h], RBP
      if (hdtBytes[0] == 0x48 && hdtBytes[1] == 0x89 && hdtBytes[2] == 0x5C &&
          hdtBytes[5] == 0x48 && hdtBytes[6] == 0x89 && hdtBytes[7] == 0x6C) {
        LogToFile("[TextHook] HUD_DrawText(0x3FE560) prologue verified, "
                  "hooking...");
        CreateHook((void *)hdtAddr, (void *)&Detour_HUD_DrawText560,
                   (void **)&Original_HUD_DrawText560, 15);
        if (Original_HUD_DrawText560) {
          g_HDT_Hooked = true;
          LogToFile("[TextHook] SUCCESS: HUD_DrawText(0x3FE560) hooked!");
        } else {
          LogToFile("[TextHook] FAILED: HUD_DrawText(0x3FE560) hook");
        }
      } else {
        char byteBuf[128];
        sprintf_s(byteBuf,
                  "[TextHook] HUD_DrawText prologue mismatch: %02X %02X %02X "
                  "%02X %02X %02X %02X %02X",
                  hdtBytes[0], hdtBytes[1], hdtBytes[2], hdtBytes[3],
                  hdtBytes[4], hdtBytes[5], hdtBytes[6], hdtBytes[7]);
        LogToFile(byteBuf);
      }
    }

    // All core hooks applied
    if (r_addcmd_hooked && seh_hooked && g_SLC_HookApplied &&
        g_R_TextWidth.load(std::memory_order_acquire) &&
        g_CfgToSlc_HookApplied && g_SL_StringTypeCheck_HookApplied &&
        (!cgMsgHookNeeded || g_CG_GameMessage_HookApplied) &&
        (!needHudDrawHook || g_HDT_Hooked) &&
        (!needTimeScriptHook || g_TimeScriptHooked)) {
      LogToFile("[TextHook] All hooks applied successfully!");
      return 0;
    }

    if (i % 10 == 0) {
      std::stringstream ss;
      ss << "[TextHook] Waiting (" << (i / 2) << "s)... ";

      if (!r_addcmd_hooked) {
        unsigned char *bytes = (unsigned char *)g_TargetAddr;
        ss << "R_AddCmd: " << std::hex << std::setw(2) << std::setfill('0')
           << (int)bytes[0] << " ";
      }
      if (!seh_hooked && g_SEH_TargetAddr) {
        unsigned char *bytes = (unsigned char *)g_SEH_TargetAddr;
        ss << "SEH: " << std::hex << std::setw(2) << std::setfill('0')
           << (int)bytes[0] << " ";
      }
      if (cgMsgHookNeeded && !g_CG_GameMessage_HookApplied) {
        ss << "CGMsg: pending ";
      }
      LogToFile(ss.str());
    }
  }

  if (!r_addcmd_hooked) {
    LogToFile("[TextHook] Timeout! R_AddCmdDrawText hook not applied.");
  }
  if (!seh_hooked) {
    LogToFile("[TextHook] Timeout! SEH hook not applied.");
  }
  if (!g_R_TextWidth.load(std::memory_order_acquire)) {
    LogToFile("[TextHook] Timeout! Native text width function not ready.");
  }
  if (!g_SLC_HookApplied) {
    LogToFile("[TextHook] Timeout! SLC hook not applied.");
  }
  if (!g_CfgToSlc_HookApplied) {
    LogToFile("[TextHook] Timeout! cfg_to_slc hook not applied.");
  }
  if (!g_SL_StringTypeCheck_HookApplied) {
    LogToFile("[TextHook] Timeout! sl_string_typecheck hook not applied.");
  }
  if (RuntimeFlags_ObjectiveStatusEnableGameMessage() &&
      g_CG_GameMessage_TargetAddr && !g_CG_GameMessage_HookApplied) {
    LogToFile("[TextHook] Timeout! CG_GameMessage hook not applied.");
  }
  if (TextHook_IsHudNativeEnabled() && !g_HDT_Hooked) {
    LogToFile("[TextHook] Timeout! HUD_DrawText hook not applied.");
  }
  if (false && TextHook_IsHudNativeEnabled() && !g_TimeScriptHooked) {
    LogToFile("[TextHook] Timeout! TimeScript HUD draw hook not applied.");
  }
  // OSREV chain hook timeout checks removed — OSAUTH is the sole authority.

  return 1;
}
void TextHook::Init() {
  if (!GameBuild::RuntimeReady()) return;
  static std::atomic<int> s_InitState{
      0}; // 0=not started, 1=initializing, 2=initialized

  int expected = 0;
  if (!s_InitState.compare_exchange_strong(expected, 1,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
    return;
  }

  bool initSucceeded = false;
  struct InitStateGuard {
    std::atomic<int> &state;
    bool &success;

    ~InitStateGuard() {
      state.store(success ? 2 : 0, std::memory_order_release);
    }
  } initStateGuard{s_InitState, initSucceeded};

  LogToFile("[TextHook] Initializing (DELAYED + XMM-SAFE logging)...");
  {
    char buildStamp[128];
    sprintf_s(buildStamp, "[TextHook] Build stamp: %s %s", __DATE__, __TIME__);
    LogToFile(buildStamp);
  }
  RuntimeFlags_EnsureLoaded((HMODULE)GetModuleHandleA(NULL));
  {
    char keepHudEnglishRaw[16] = {0};
    DWORD nKeep = GetEnvironmentVariableA(
        "GHOSTSKOR_KEEP_HUD_ENGLISH", keepHudEnglishRaw,
        (DWORD)sizeof(keepHudEnglishRaw));
    if (nKeep > 0 && nKeep < sizeof(keepHudEnglishRaw)) {
      std::string v = ToUpperAscii(TrimSpaces(std::string(keepHudEnglishRaw)));
      if (v == "1" || v == "TRUE" || v == "ON" || v == "YES") {
        g_KeepHudEnglishReference = true;
      }
    }
    LogToFile(g_KeepHudEnglishReference
        ? "[TextHook] HUD English reference mode: KEEP (English visible)"
        : "[TextHook] HUD English reference mode: SUPPRESS (English hidden)");
  }
  {
    char strictHudAuthRaw[16] = {0};
    DWORD nStrict = GetEnvironmentVariableA(
        "GHOSTSKOR_HUD_STRICT_AUTHORITY", strictHudAuthRaw,
        (DWORD)sizeof(strictHudAuthRaw));
    if (nStrict > 0 && nStrict < sizeof(strictHudAuthRaw)) {
      std::string v = ToUpperAscii(TrimSpaces(std::string(strictHudAuthRaw)));
      if (v == "1" || v == "TRUE" || v == "ON" || v == "YES") {
        g_HudStrictConsumerAuthorityOverride = 1;
      } else if (v == "0" || v == "FALSE" || v == "OFF" || v == "NO") {
        g_HudStrictConsumerAuthorityOverride = 0;
      }
    }
    if (g_HudStrictConsumerAuthorityOverride >= 0) {
      LogToFile(std::string("[TextHook] HUD strict authority override=") +
                (g_HudStrictConsumerAuthorityOverride ? "1" : "0") +
                " (GHOSTSKOR_HUD_STRICT_AUTHORITY)");
    }
  }
  {
    // Default OFF: INT3 writer breakpoint is very expensive in normal gameplay
    // and should only be enabled for explicit reverse sessions.
    g_HudHintWriterBpEnabled =
        false; // Requires a separately validated instruction/context for this build.
    if (g_HudHintWriterBpEnabled) {
      LogToFile("[HUDW-BP] enabled (INT3@+0x5DC950)");
    } else {
      LogToFile("[HUDW-BP] disabled");
    }
  }

  SubRev_EnsurePageSize();
  {
    g_SubtitleReverseScanCursor = (uintptr_t)0x10000;
  }
  {
    char subRevInit[192];
    sprintf_s(subRevInit,
              "[SUBREV_SUMMARY] init enabled=%d veh=%d autoFailOpen=%d page=%lu",
              g_SubtitleReverseEnabled.load() ? 1 : 0,
              g_SubtitleReverseVehEnabled.load() ? 1 : 0,
              kEnableSubtitleReverseAutoFailOpen ? 1 : 0,
              (unsigned long)g_SubtitleReversePageSize);
    LogToFile(subRevInit);
  }

  // Runtime toggle for reversing sessions (no rebuild required):
  // If `enable_subrev.txt` exists in the game directory, enable the expensive
  // subtitle reverse path (PAGE_GUARD/VEH) to collect access RIPs and removal.
  {
    std::string gameDir = GetModuleDirPath(nullptr);
    if (!gameDir.empty()) {
      const std::string togglePath = gameDir + "\\enable_subrev.txt";
      if (FileExistsSimple(togglePath)) {
        g_SubtitleReverseEnabled.store(true);
        g_SubtitleReverseVehEnabled.store(true);
        LogToFile("[SUBREV_SUMMARY] runtime enabled via enable_subrev.txt");
      }
    }
  }

  // Initialize queue

  for (int i = 0; i < MAX_QUEUE_SIZE; i++) {

    g_Queue[i].ready.store(false);
  }

  HMODULE hModule = GetModuleHandle(NULL);

  if (!hModule) {

    LogToFile("[TextHook] ERROR: GetModuleHandle failed!");

    return;
  }

  uintptr_t moduleBase = reinterpret_cast<uintptr_t>(hModule);

  LogToFile("[TextHook] Module base: " + std::to_string(moduleBase));

  // Initialize R_TextWidth function pointer for accurate text measurement.
  // The code can be decrypted/ready later, so keep primary+fallback addresses.
  g_R_TextWidthAddrPrimary =
      IW6Offsets::GetAddress(moduleBase, IW6Offsets::R_TextWidth_SP);
  g_R_TextWidthAddrFallback =
      IW6Offsets::GetAddress(moduleBase, IW6Offsets::R_TextWidth_SP_FALLBACK);

  g_R_TextWidth.store(nullptr, std::memory_order_release);
  if (IsProbablyExecutableCode(g_R_TextWidthAddrPrimary)) {
    g_R_TextWidth.store((R_TextWidth_t)g_R_TextWidthAddrPrimary,
                        std::memory_order_release);
  } else if (IsProbablyExecutableCode(g_R_TextWidthAddrFallback)) {
    g_R_TextWidth.store((R_TextWidth_t)g_R_TextWidthAddrFallback,
                        std::memory_order_release);
  }
  const auto nativeTextWidth = g_R_TextWidth.load(std::memory_order_acquire);
  if (nativeTextWidth) {
    LogToFile("[TextHook] R_TextWidth initialized at: " +
              std::to_string(reinterpret_cast<uintptr_t>(nativeTextWidth)));
  } else {
    LogToFile("[TextHook] WARNING: R_TextWidth not ready (will retry).");
  }

  // Resolve primary+fallback addresses for R_AddCmdDrawText, then validate prologue.
  g_TargetAddrPrimary =
      IW6Offsets::GetAddress(moduleBase, IW6Offsets::R_AddCmdDrawText_SP);
  g_TargetAddrFallback = IW6Offsets::GetAddress(
      moduleBase, IW6Offsets::R_AddCmdDrawText_SP_FALLBACK);

  if (g_TargetAddrPrimary) {
    LogToFile("[TextHook] R_AddCmdDrawText primary: " +
              std::to_string(reinterpret_cast<uintptr_t>(g_TargetAddrPrimary)));
  }
  if (g_TargetAddrFallback) {
    LogToFile("[TextHook] R_AddCmdDrawText fallback: " +
              std::to_string(reinterpret_cast<uintptr_t>(g_TargetAddrFallback)));
  }

  g_TargetAddr = g_TargetAddrPrimary ? g_TargetAddrPrimary : g_TargetAddrFallback;
  if (!g_TargetAddr) {
    LogToFile("[TextHook] ERROR: R_AddCmdDrawText offset is 0!");
    return;
  }

  void *candidate = nullptr;
  if (g_TargetAddrPrimary && IsCodeReady(g_TargetAddrPrimary)) {
    candidate = g_TargetAddrPrimary;
  } else if (g_TargetAddrFallback && IsCodeReady(g_TargetAddrFallback)) {
    candidate = g_TargetAddrFallback;
  }

  if (candidate) {
    g_TargetAddr = candidate;
    LogToFile("[TextHook] R_AddCmdDrawText code ready! Applying hook...");
    CreateHook(g_TargetAddr, (void *)&Detour_R_AddCmdDrawText,
               (void **)&Original_R_AddCmdDrawText);
    if (Original_R_AddCmdDrawText) {
      g_HookApplied = true;
      LogToFile("[TextHook] SUCCESS: Hook applied!");
      if (kEnableRuntimeTextCapture) {
        CreateThread(NULL, 0, LoggerThread, NULL, 0, NULL);
      }
    }
  } else {
    LogToFile("[TextHook] R_AddCmdDrawText code not ready. Starting delayed hook...");
    if (g_TargetAddrPrimary) {
      std::stringstream ss;
      ss << "[TextHook] R_AddCmdDrawText primary bytes: ";
      unsigned char *bytes = (unsigned char *)g_TargetAddrPrimary;
      for (int i = 0; i < 16; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[i]
           << " ";
      }
      LogToFile(ss.str());
    }
    if (g_TargetAddrFallback) {
      std::stringstream ss;
      ss << "[TextHook] R_AddCmdDrawText fallback bytes: ";
      unsigned char *bytes = (unsigned char *)g_TargetAddrFallback;
      for (int i = 0; i < 16; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[i]
           << " ";
      }
      LogToFile(ss.str());
    }
  }

  // ALWAYS start DelayedHookThread for SEH hook (even if R_AddCmdDrawText was
  // hooked immediately)
  CreateThread(NULL, 0, DelayedHookThread, NULL, 0, NULL);

  // =============================================================================
  // SEH_StringEd_GetString Hook - Set up address (hook applied in
  // DelayedHookThread)
  // =============================================================================
  // This hook captures localization key lookups and builds English->Korean
  // map Critical for: Loading subtitles, Mission briefings, Dynamic text

  g_SEH_TargetAddr =
      IW6Offsets::GetAddress(moduleBase, IW6Offsets::SEH_StringEd_GetString_SP);

  if (g_SEH_TargetAddr) {
    LogToFile("[TextHook] SEH Target Address set: " +
              std::to_string(reinterpret_cast<uintptr_t>(g_SEH_TargetAddr)));
    LogToFile("[TextHook] SEH Hook will be applied when code is ready (in "
              "DelayedHookThread)");
  } else {
    LogToFile("[TextHook] ERROR: SEH Target Address is null!");
  }

  // =============================================================================
  // SL_ConvertToString Hook - Custom detour (reimplemented, no gateway)
  // Hook applied in DelayedHookThread after string table is loaded
  // =============================================================================
  g_SLC_TargetAddr =
      IW6Offsets::GetAddress(moduleBase, IW6Offsets::SL_ConvertToString_SP);

  if (g_SLC_TargetAddr) {
    LogToFile("[TextHook] SL_ConvertToString Target Address set: " +
              std::to_string(reinterpret_cast<uintptr_t>(g_SLC_TargetAddr)));
  } else {
    LogToFile("[TextHook] WARNING: SL_ConvertToString offset not available.");
  }

  g_CfgToSlc_TargetAddr = IW6Offsets::GetAddress(
      moduleBase, IW6Offsets::ConfigString_IndexToSlc_SP);
  if (g_CfgToSlc_TargetAddr) {
    LogToFile("[TextHook] cfg_to_slc Target Address set: " +
              std::to_string(reinterpret_cast<uintptr_t>(g_CfgToSlc_TargetAddr)));
  } else {
    LogToFile("[TextHook] WARNING: cfg_to_slc offset not available.");
  }

  g_SL_StringTypeCheck_TargetAddr =
      IW6Offsets::GetAddress(moduleBase, IW6Offsets::SL_StringTypeCheck_SP);
  if (g_SL_StringTypeCheck_TargetAddr) {
    LogToFile("[TextHook] sl_string_typecheck Target Address set: " +
              std::to_string(reinterpret_cast<uintptr_t>(
                  g_SL_StringTypeCheck_TargetAddr)));
  } else {
    LogToFile("[TextHook] WARNING: sl_string_typecheck offset not available.");
  }

  // =============================================================================
  g_CG_GameMessage_TargetAddr =
      IW6Offsets::GetAddress(moduleBase, IW6Offsets::CG_GameMessage_SP);
  if (g_CG_GameMessage_TargetAddr) {
    LogToFile("[TextHook] CG_GameMessage Target Address set: " +
              std::to_string(reinterpret_cast<uintptr_t>(
                  g_CG_GameMessage_TargetAddr)));
  } else {
    LogToFile("[TextHook] WARNING: CG_GameMessage offset not available.");
  }

  // =============================================================================
  // Cbuf_AddText Hook — restart signal detection probe
  // =============================================================================
  g_Cbuf_AddText_TargetAddr =
      IW6Offsets::GetAddress(moduleBase, IW6Offsets::Cbuf_AddText_SP);

  if (g_Cbuf_AddText_TargetAddr) {
    LogToFile("[TextHook] Cbuf_AddText Target Address set: " +
              std::to_string(reinterpret_cast<uintptr_t>(g_Cbuf_AddText_TargetAddr)));
  } else {
    LogToFile("[TextHook] WARNING: Cbuf_AddText offset not available.");
  }

  // R_AddCmdDrawTextWithCursor Hook - Text input fields, possibly subtitles
  // =============================================================================
  g_CursorTextTargetAddr = IW6Offsets::GetAddress(
      moduleBase, IW6Offsets::R_AddCmdDrawTextWithCursor_SP);

  if (g_CursorTextTargetAddr) {
    LogToFile(
        "[TextHook] R_AddCmdDrawTextWithCursor Target Address set: " +
        std::to_string(reinterpret_cast<uintptr_t>(g_CursorTextTargetAddr)));
    LogToFile("[TextHook] R_AddCmdDrawTextWithCursor Hook will be applied when "
              "code is ready.");
  } else {
    LogToFile("[TextHook] WARNING: R_AddCmdDrawTextWithCursor offset not set.");
  }

  initSucceeded = true;
}

// =============================================================================
// TextHook::Shutdown - Save missing translations before exit
// =============================================================================
void TextHook::Shutdown() {
  LogToFile("[TextHook] Shutdown - saving missing subtitle keys...");
  UninstallHudHintWriterBreakpoint();
  SubtitleReverse_Shutdown();
  SaveMissingSubtitles();
  LogToFile("[TextHook] Shutdown complete.");
}
