uintptr_t __fastcall Detour_SubtitleWrap2351E0(int a1, const char *key, int a3,
                                               int a4) {
  // Provide per-thread context for downstream +0x235620 hook.
  SubtitleWrapCallCtx prevCtx = g_SubWrapCallCtx;
  g_SubWrapCallCtx.active = (key != nullptr);
  g_SubWrapCallCtx.key = key ? key : "";
  g_SubWrapCallCtx.a1 = a1;
  g_SubWrapCallCtx.a3 = a3;
  g_SubWrapCallCtx.a4 = a4;
  g_SubWrapCallCtx.tick = GetTickCount();

  uintptr_t ret = 0;
  if (Original_SubtitleWrap2351E0) {
    ret = Original_SubtitleWrap2351E0(a1, key, a3, a4);
  }

  if (!key) {
    g_SubWrapCallCtx = prevCtx;
    return ret;
  }
  bool isSubtitleKey = (strncmp(key, "SUBTITLE_", 9) == 0) ||
                       (strncmp(key, "VIDSUBTITLES_", 13) == 0);
  if (!isSubtitleKey) {
    g_SubWrapCallCtx = prevCtx;
    return ret;
  }

  static std::atomic<int> s_logCount{0};
  int c = s_logCount.fetch_add(1);
  if (c < 300) {
    char buf[512];
    sprintf_s(buf,
              "[SUBWRAP] key=%s a1=%d a3=%d a4=%d ret=0x%llX",
              key, a1, a3, a4, (unsigned long long)ret);
    LogToFile(buf);
  }

  // If return value looks pointer-like, dump first bytes/words to detect
  // native message fields (expiry/fade timestamps etc).
  if (ret > 0x10000 && IsSafeRead((void *)ret, 0x80) && c < 220) {
    const unsigned int *dw = (const unsigned int *)ret;
    char row0[512], row1[512];
    sprintf_s(row0,
              "[SUBWRAP-RET] ptr=0x%llX d0=%08X d1=%08X d2=%08X d3=%08X d4=%08X d5=%08X d6=%08X d7=%08X",
              (unsigned long long)ret, dw[0], dw[1], dw[2], dw[3], dw[4],
              dw[5], dw[6], dw[7]);
    sprintf_s(row1,
              "[SUBWRAP-RET] ptr=0x%llX d8=%08X d9=%08X d10=%08X d11=%08X d12=%08X d13=%08X d14=%08X d15=%08X",
              (unsigned long long)ret, dw[8], dw[9], dw[10], dw[11], dw[12],
              dw[13], dw[14], dw[15]);
    LogToFile(row0);
    LogToFile(row1);
  }

  g_SubWrapCallCtx = prevCtx;
  return ret;
}
uintptr_t __fastcall Detour_SubtitleEnqueue235620(int a1, int windowId,
                                                  const char *localizedText,
                                                  int a3, int a4, int a6) {
  // Pass blank text to engine so English subtitle is hidden.
  // Our Korean overlay uses localizedText (unchanged) for translation.
  static const char kBlankSubtitle[] = " ";
  uintptr_t ret = 0;
  if (Original_SubtitleEnqueue235620) {
    ret = Original_SubtitleEnqueue235620(a1, windowId, kBlankSubtitle, a3, a4,
                                         a6);
  }

  uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
  uintptr_t caller = (uintptr_t)_ReturnAddress();
  uintptr_t callerOff =
      (base && caller >= base) ? (caller - base) : (uintptr_t)0;
  bool fromSubWrap = (callerOff >= 0x235220 && callerOff <= 0x235280);

  // === PROBE Phase 9: Dereference slot pointers to find subtitle text ===
  // Slots at base+0x1640738 contain 3 pointers each (ptr1, ptr2, ptr3)
  // Dereference ptr1 of each slot to read actual text buffer content
  {
    static int s_phase9Count = 0;
    if (s_phase9Count < 15 && base && windowId == 4 && localizedText &&
        localizedText[0]) {
      s_phase9Count++;
      for (int line = 0; line < 3; line++) {
        uintptr_t slotAddr = base + 0x1640738 + (uintptr_t)(line * 64);
        if (IsSafeRead((void *)slotAddr, 24)) {
          // Read the 3 pointers from the slot
          uintptr_t ptr1 = *(uintptr_t *)(slotAddr);
          uintptr_t ptr2 = *(uintptr_t *)(slotAddr + 8);
          uintptr_t ptr3 = *(uintptr_t *)(slotAddr + 16);
          // Dereference ptr1: read first 128 bytes as text
          if (ptr1 && IsSafeRead((void *)ptr1, 128)) {
            const char *txt = (const char *)ptr1;
            char buf[256];
            sprintf_s(buf,
                      "[SUBTXT] #%d L%d ptr1(+0x%llX) => \"%.*s\"",
                      s_phase9Count, line,
                      (unsigned long long)(ptr1 - base), 120, txt);
            LogToFile(buf);
          }
          // Dereference ptr2: read first 128 bytes
          if (ptr2 && IsSafeRead((void *)ptr2, 128)) {
            const char *txt = (const char *)ptr2;
            char buf[256];
            sprintf_s(buf,
                      "[SUBTXT] #%d L%d ptr2(+0x%llX) => \"%.*s\"",
                      s_phase9Count, line,
                      (unsigned long long)(ptr2 - base), 120, txt);
            LogToFile(buf);
          }
          // Dereference ptr3: read first 128 bytes
          if (ptr3 && IsSafeRead((void *)ptr3, 128)) {
            const char *txt = (const char *)ptr3;
            char buf[256];
            sprintf_s(buf,
                      "[SUBTXT] #%d L%d ptr3(+0x%llX) => \"%.*s\"",
                      s_phase9Count, line,
                      (unsigned long long)(ptr3 - base), 120, txt);
            LogToFile(buf);
          }
        }
      }
    }
  }

  const SubtitleWrapCallCtx &ctx = g_SubWrapCallCtx;
  bool ctxSubtitle =
      ctx.active &&
      (!ctx.key.empty() &&
       (ctx.key.rfind("SUBTITLE_", 0) == 0 ||
        ctx.key.rfind("VIDSUBTITLES_", 0) == 0));

  bool textLooksSubtitle = false;
  if (localizedText && localizedText[0]) {
    textLooksSubtitle = (strstr(localizedText, "^2") != nullptr) ||
                        (strstr(localizedText, "^7") != nullptr) ||
                        (strstr(localizedText, ": ") != nullptr);
  }

  std::string resolvedKey;
  if (ctxSubtitle && !ctx.key.empty()) {
    resolvedKey = ctx.key;
  } else if (localizedText && localizedText[0]) {
    ResolveSubtitleKeyFromLocalizedText(localizedText, resolvedKey);
  }

  bool likelySubtitle =
      fromSubWrap || ctxSubtitle || textLooksSubtitle ||
      !resolvedKey.empty() || (windowId == 4 && localizedText && localizedText[0]);
  if (!likelySubtitle)
    return ret;

  // Late dvar dump: run on first real subtitle enqueue (windowId=4) to catch
  // dvars registered after early init.
  if (windowId == 4 && localizedText && localizedText[0]) {
    DumpSubtitleDvarCandidatesLateOnce();
  }

  // Capture runtime timing for all subtitle-like events even when key context
  // is missing, so SEH queue can still attach timing via normalized text.
  UpdateSubtitleRuntimeTimingByText(localizedText, windowId, a3, a4, a6);

  if (!resolvedKey.empty()) {
    UpdateSubtitleRuntimeTiming(resolvedKey, windowId, a3, a4, a6);
    ApplyRuntimeTimingToSubtitleQueue(resolvedKey, a3, a4, a6,
                                      TextHook_GetSubtitleClockNow());

    if (!ctxSubtitle) {
      static std::atomic<int> s_matchLogCount{0};
      int m = s_matchLogCount.fetch_add(1);
      if (m < 120) {
        char kbuf[512];
        sprintf_s(kbuf,
                  "[SUBENQ-MATCH] key=%s via=text win=%d a3=%d a4=%d a6=%d",
                  resolvedKey.c_str(), windowId, a3, a4, a6);
        LogToFile(kbuf);
      }
    }
  }

  std::string enqueueKey = resolvedKey;
  if (enqueueKey.empty() && ctxSubtitle && !ctx.key.empty()) {
    enqueueKey = ctx.key;
  }
  if (!enqueueKey.empty()) {
    SubtitleReverse_OnEnqueueInstance(enqueueKey, localizedText, a3, a4,
                                      TextHook_GetSubtitleClockNow());
  }

  // Credits guard: activate when Hesh's "I'm proud of you" line fires,
  // which precedes the credits sequence.  This suppresses all HUD hint /
  // objective detection pipelines during credits (not just rendering).
  //
  // False-positive guard (Method 1+2): During map init the engine prefetches
  // ALL subtitle keys for the level via SEH_StringEd_GetString before gameplay
  // begins. Block the guard unless at least 30 seconds OR 1800 frames have
  // elapsed since the last Bink video closed (the map load video).
  // If no video has ever closed in this session, allow unconditionally.
  if (!enqueueKey.empty() &&
      enqueueKey == "SUBTITLE_SKYWAY_HSH_IMPROUDOFYOU1111") {
    extern std::atomic<bool>     g_creditsGuardActive;
    extern std::atomic<DWORD>    g_lastBinkVideoCloseTick;
    extern std::atomic<uint64_t> g_presentFrameCount;
    extern std::atomic<uint64_t> g_presentFrameAtLastVideoClose;
    if (!g_creditsGuardActive.load(std::memory_order_relaxed)) {
      const DWORD lastClose = g_lastBinkVideoCloseTick.load(std::memory_order_relaxed);
      const bool  noVideoYet = (lastClose == 0);
      const DWORD elapsedMs  = noVideoYet ? 0xFFFFFFFFu : (GetTickCount() - lastClose);
      const uint64_t framesSinceClose =
          g_presentFrameCount.load(std::memory_order_relaxed) -
          g_presentFrameAtLastVideoClose.load(std::memory_order_relaxed);
      // Allow if no video yet, OR 30s elapsed, OR 1800 frames elapsed.
      const bool pastLoading = noVideoYet || (elapsedMs >= 30000u) || (framesSinceClose >= 1800u);
      if (pastLoading) {
        g_creditsGuardActive.store(true, std::memory_order_relaxed);
        LogToFile("[CREDITS-GUARD] ON — pre-credits subtitle detected "
                  "(IMPROUDOFYOU1111)");
        // NOTE: intentionally NOT calling TextHook_SuspendHooksForCredits() here.
        // Suspending the SEH hook would block dialogue subtitles that follow
        // this line in the same scene.  The guard flag alone is enough:
        // - SLC hook checks guard → immediate return (no CPU overhead)
        // - HudElem scan checks guard → immediate return
        // Only the menu-credits path (ui_play_credits SLC) needs full suspension.
      } else {
        char skipbuf[128];
        sprintf_s(skipbuf,
                  "[CREDITS-GUARD] SKIP — prefetch burst (elapsed=%ums frames=%llu)",
                  elapsedMs, (unsigned long long)framesSinceClose);
        LogToFile(skipbuf);
      }
    }
  }

  // === SUBTITLE RE-INJECTION (Engine-pipeline backup capture) ===
  // When the engine enqueues a subtitle (e.g., after checkpoint restore or
  // death/respawn), check if our Korean overlay queue already has it.
  // If not, re-inject from the translation store.
  if (windowId == 4 && !enqueueKey.empty() &&
      (enqueueKey.rfind("SUBTITLE_", 0) == 0 ||
       enqueueKey.rfind("VIDSUBTITLES_", 0) == 0)) {
    std::string keyUpper = ToUpper(enqueueKey);
    auto itKor = g_KeyToKorean.find(keyUpper);
    if (itKor != g_KeyToKorean.end() && !itKor->second.empty()) {
      DWORD reinjectClockNow = TextHook_GetSubtitleClockNow();
      std::lock_guard<std::mutex> queueLock(g_InGameSubtitleMutex);
      auto qIt = std::find_if(
          g_InGameSubtitleQueue.begin(), g_InGameSubtitleQueue.end(),
          [&](const InGameSubtitleEntry &e) { return e.key == enqueueKey; });
      if (qIt == g_InGameSubtitleQueue.end()) {
        // Subtitle not in our queue — re-inject from Korean store.
        for (auto &ex : g_InGameSubtitleQueue) {
          int slot = ex.renderSlot;
          if (slot < 0 || slot >= (int)MAX_SUBTITLE_QUEUE) {
            slot = ex.stackIndex;
            if (slot < 0) slot = 0;
            if (slot >= (int)MAX_SUBTITLE_QUEUE)
              slot = (int)MAX_SUBTITLE_QUEUE - 1;
          }
          if (slot < (int)MAX_SUBTITLE_QUEUE - 1) slot++;
          ex.renderSlot = slot;
        }
        for (auto &existing : g_InGameSubtitleQueue) {
          if (existing.pushAnimStart == 0)
            existing.pushAnimStart = reinjectClockNow;
          existing.stackIndex = existing.renderSlot;
        }
        InGameSubtitleEntry newEntry;
        newEntry.text = itKor->second;
        newEntry.timestamp = reinjectClockNow;
        newEntry.firstSeen = reinjectClockNow;
        newEntry.key = enqueueKey;
        newEntry.english = localizedText ? std::string(localizedText) : "";
        newEntry.stackIndex = 0;
        newEntry.pushAnimStart = 0;
        newEntry.renderSlot = 0;
        newEntry.msgTimeMs = a3;
        newEntry.fadeOutMs = a4;
        newEntry.runtimeArg6 = a6;
        newEntry.runtimeTick = reinjectClockNow;
        newEntry.nativeSlotPtr = 0;
        newEntry.nativeSlotValid = false;
        newEntry.nativeRemoveTick = 0;
        newEntry.nativeRemoveValid = false;
        newEntry.nativeRemoveReason = SUBREV_REMOVE_NONE;
        g_InGameSubtitleQueue.insert(g_InGameSubtitleQueue.begin(), newEntry);
        if (g_InGameSubtitleQueue.size() > MAX_SUBTITLE_QUEUE)
          g_InGameSubtitleQueue.resize(MAX_SUBTITLE_QUEUE);
        static int reinjectLogCount = 0;
        if (reinjectLogCount < 100) {
          reinjectLogCount++;
          char rbuf[256];
          sprintf_s(rbuf,
                    "[SUBENQ-REINJECT] key=%s msg=%d fade=%d queueSize=%zu",
                    enqueueKey.c_str(), a3, a4, g_InGameSubtitleQueue.size());
          LogToFile(rbuf);
        }
      }
    }
  }

  static std::atomic<int> s_logCount{0};
  int c = s_logCount.fetch_add(1);
  if (c < 480) {
    std::string textPreview = localizedText ? localizedText : "";
    if (textPreview.size() > 96)
      textPreview = Utf8Preview(textPreview, 96);

    char buf[1024];
    sprintf_s(
        buf,
        "[SUBENQ] caller=+0x%llX a1=%d win=%d a3=%d a4=%d a6=%d ret=0x%llX key=%s text=%s",
        (unsigned long long)callerOff, a1, windowId, a3, a4, a6,
        (unsigned long long)ret,
        !resolvedKey.empty() ? resolvedKey.c_str()
                             : (ctx.active ? ctx.key.c_str() : "(none)"),
        textPreview.c_str());
    LogToFile(buf);
  }

  // Probe returned object for potential timing/duration fields.
  if (ret > 0x10000 && IsSafeRead((void *)ret, 0x60) && c < 260) {
    const unsigned int *dw = (const unsigned int *)ret;
    char row0[512], row1[512];
    sprintf_s(
        row0,
        "[SUBENQ-RET] ptr=0x%llX d0=%08X d1=%08X d2=%08X d3=%08X d4=%08X d5=%08X d6=%08X d7=%08X",
        (unsigned long long)ret, dw[0], dw[1], dw[2], dw[3], dw[4], dw[5],
        dw[6], dw[7]);
    sprintf_s(
        row1,
        "[SUBENQ-RET] ptr=0x%llX d8=%08X d9=%08X d10=%08X d11=%08X d12=%08X d13=%08X d14=%08X d15=%08X",
        (unsigned long long)ret, dw[8], dw[9], dw[10], dw[11], dw[12], dw[13],
        dw[14], dw[15]);
    LogToFile(row0);
    LogToFile(row1);
  }

  return ret;
}
unsigned long TextHook_GetSubtitleClockNow() {
  // Subtitle lifetime must follow the engine's timescale (slow-motion segments).
  // We don't have a reliable SP serverTime/global clock offset yet, so we
  // integrate GetTickCount() using the runtime timescale dvar.
  DWORD tickNow = GetTickCount();
  bool paused = TextHook_IsPauseMenuLikely();

  std::lock_guard<std::mutex> lock(g_SubtitleClockMutex);

  typedef void *(*Dvar_FindVar_t)(const char *name);
  static uintptr_t s_moduleBase = 0;
  static Dvar_FindVar_t s_findVar = nullptr;
  static void *s_timescaleDvar = nullptr;
  static void *s_comTimescaleDvar = nullptr;
  static DWORD s_lastFindTick = 0;
  static bool s_clockInitLogged = false;

  if (!s_moduleBase) {
    s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  }
  if (s_moduleBase && !s_findVar && IW6Offsets::Dvar_FindVar_SP != 0) {
    s_findVar = (Dvar_FindVar_t)(s_moduleBase + IW6Offsets::Dvar_FindVar_SP);
  }

  // Resolve dvar pointers lazily (dvar system may not be ready at early init).
  if (s_findVar && (tickNow - s_lastFindTick) > 2000) {
    s_lastFindTick = tickNow;
    if (!s_timescaleDvar) {
      s_timescaleDvar = s_findVar("timescale");
      if (s_timescaleDvar) {
        char buf[192];
        sprintf_s(buf, "[SUBCLOCK] dvar found: timescale ptr=%p",
                  s_timescaleDvar);
        LogToFile(buf);
      }
    }
    if (!s_comTimescaleDvar) {
      s_comTimescaleDvar = s_findVar("com_timescale");
      if (s_comTimescaleDvar) {
        char buf[192];
        sprintf_s(buf, "[SUBCLOCK] dvar found: com_timescale ptr=%p",
                  s_comTimescaleDvar);
        LogToFile(buf);
      }
    }
  }

  auto readDvarFloat = [](void *dv, float *outVal) -> bool {
    if (!dv || !outVal)
      return false;
    float v = 1.0f;
    __try {
      // Observed float dvars store the current value at +0x10.
      v = *(volatile float *)((unsigned char *)dv + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
    }
    // NaN check and sanity clamp.
    if (!(v == v))
      return false;
    if (v < 0.0f || v > 4.0f)
      return false;
    *outVal = v;
    return true;
  };

  float ts = 1.0f;
  float tsA = 1.0f, tsB = 1.0f;
  bool hasA = readDvarFloat(s_timescaleDvar, &tsA);
  bool hasB = readDvarFloat(s_comTimescaleDvar, &tsB);
  const char *tsName = "default";
  if (hasA && !hasB) {
    ts = tsA;
    tsName = "timescale";
  } else if (!hasA && hasB) {
    ts = tsB;
    tsName = "com_timescale";
  } else if (hasA && hasB) {
    float dA = tsA - 1.0f;
    if (dA < 0.0f)
      dA = -dA;
    float dB = tsB - 1.0f;
    if (dB < 0.0f)
      dB = -dB;
    if (dA >= dB) {
      ts = tsA;
      tsName = "timescale";
    } else {
      ts = tsB;
      tsName = "com_timescale";
    }
  }

  // Integrated clock in "game milliseconds" (approx).
  static bool s_scaledInit = false;
  static DWORD s_lastTickNow = 0;
  static double s_scaledAcc = 0.0;
  if (!s_scaledInit) {
    s_scaledInit = true;
    s_lastTickNow = tickNow;
    s_scaledAcc = (double)tickNow;
  }

  DWORD dtTick = tickNow - s_lastTickNow;
  s_lastTickNow = tickNow;
  // Clamp extreme hitches so subtitle age doesn't jump wildly on stalls.
  if (dtTick > 5000)
    dtTick = 5000;
  s_scaledAcc += (double)dtTick * (double)ts;
  DWORD rawNow = (DWORD)(s_scaledAcc + 0.5);

  if (!s_clockInitLogged) {
    s_clockInitLogged = true;
    char buf[256];
    sprintf_s(buf,
              "[SUBCLOCK] init mode=timescale base=0x%llX off=0x10",
              (unsigned long long)s_moduleBase);
    LogToFile(buf);
  }

  // Log timescale changes (throttled).
#if (GHOSTSKOR_SUBCLOCK_LOG_TS != 0)
  {
    static float s_lastTsLogged = 1.0f;
    static DWORD s_lastTsLogTick = 0;
    static int s_tsLogCount = 0;
    float d = ts - s_lastTsLogged;
    if (d < 0.0f)
      d = -d;
    bool wantsLog = false;
    if (d > 0.01f && (tickNow - s_lastTsLogTick) > 250) {
      wantsLog = true;
    } else if (ts != 1.0f && (tickNow - s_lastTsLogTick) > 6000) {
      wantsLog = true;
    }
    if (wantsLog && s_tsLogCount < 120) {
      s_tsLogCount++;
      s_lastTsLogTick = tickNow;
      s_lastTsLogged = ts;
      char buf[256];
      sprintf_s(buf,
                "[SUBCLOCK] ts name=%s val=%.3f hasA=%d hasB=%d",
                tsName, ts, hasA ? 1 : 0, hasB ? 1 : 0);
      LogToFile(buf);
    }
  }
#endif

  if (paused) {
    if (!g_SubtitleClockWasPaused) {
      g_SubtitleClockWasPaused = true;
      g_SubtitleClockPauseStartMs = rawNow;
    }
  } else if (g_SubtitleClockWasPaused) {
    g_SubtitleClockPausedAccumMs += (rawNow - g_SubtitleClockPauseStartMs);
    g_SubtitleClockWasPaused = false;
    g_SubtitleClockPauseStartMs = 0;
  }

  DWORD pausedAccum = g_SubtitleClockPausedAccumMs;
  if (g_SubtitleClockWasPaused && g_SubtitleClockPauseStartMs > 0) {
    pausedAccum += (rawNow - g_SubtitleClockPauseStartMs);
  }
  return rawNow - pausedAccum;
}
bool TextHook_IsNativeSubtitleEnabled() { return g_EnableSubtitleNative; }
void TextHook_SetNativeSubtitleEnabled(bool enabled) {
  g_EnableSubtitleNative = enabled;
}
static void SubRev_EnsurePageSize() {
  if (g_SubtitleReversePageSize != 0)
    return;
  SYSTEM_INFO si = {};
  GetSystemInfo(&si);
  g_SubtitleReversePageSize =
      (si.dwPageSize > 0) ? si.dwPageSize : (DWORD)4096;
}
static bool SubRev_IsWritableProtect(DWORD protect) {
  DWORD p = (protect & 0xFF);
  return p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
         p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}
static bool SubRev_IsCandidateRegion(const MEMORY_BASIC_INFORMATION &mbi) {
  if (mbi.State != MEM_COMMIT)
    return false;
  if (mbi.Protect & PAGE_NOACCESS)
    return false;
  if (mbi.Protect & PAGE_GUARD)
    return false;
  if (!SubRev_IsWritableProtect(mbi.Protect))
    return false;
  return true;
}
template <typename T> static bool SafeReadVolatile(uintptr_t addr, T *outVal) {
  if (!outVal)
    return false;
  __try {
    *outVal = *(volatile T *)addr;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
static bool SubRev_ReadU32(uintptr_t addr, uint32_t *outVal) {
  return SafeReadVolatile(addr, outVal);
}
static bool SubRev_ReadPtr(uintptr_t addr, uintptr_t *outVal) {
  return SafeReadVolatile(addr, outVal);
}
static bool SubRev_ReadF32(uintptr_t addr, float *outVal) {
  return SafeReadVolatile(addr, outVal);
}
static bool SubRev_ReadU8(uintptr_t addr, uint8_t *outVal) {
  return SafeReadVolatile(addr, outVal);
}
static bool SubRev_ReadTextPtrForBinding(const SubtitleReverseBinding &b,
                                        uintptr_t *outVal) {
  if (!outVal || b.slotPtr == 0)
    return false;

  // If we already learned the storage width during binding, respect it.
  if (b.textPtrKind == 1) {
    uint32_t v = 0;
    if (!SubRev_ReadU32(b.slotPtr + 0x00, &v))
      return false;
    *outVal = (uintptr_t)v;
    return true;
  }
  if (b.textPtrKind == 2) {
    return SubRev_ReadPtr(b.slotPtr + 0x00, outVal);
  }

  // Unknown: prefer u32 for low-address subtitle pools, but keep a fallback.
  uint32_t v = 0;
  if (SubRev_ReadU32(b.slotPtr + 0x00, &v)) {
    *outVal = (uintptr_t)v;
    return true;
  }
  return SubRev_ReadPtr(b.slotPtr + 0x00, outVal);
}
static bool SubRev_TryReadAsciiPreview(uintptr_t addr, size_t maxLen,
                                       std::string &out) {
  out.clear();
  if (addr < 0x10000 || addr > 0x7FFFFFFFFFFF || maxLen < 4)
    return false;

  if (!IsSafeRead((void *)addr, 1))
    return false;

  for (size_t i = 0; i < maxLen; ++i) {
    uint8_t c = 0;
    if (!SubRev_ReadU8(addr + i, &c))
      return false;
    if (c == 0)
      break;

    bool ok = (c == '^' || c == '[' || c == ']' || c == '{' || c == '}' ||
               c == '+' || c == '-' || c == '_' || c == ':' || c == '.' ||
               c == ',' || c == '\'' || c == '\"' || c == '/' || c == '\\' ||
               c == '(' || c == ')' || c == '!' || c == '?' || c == '%' ||
               c == '#');
    if (!ok) {
      ok = (c >= 32 && c <= 126);
    }
    if (!ok)
      return false;

    out.push_back((char)c);
  }

  return out.size() >= 4;
}
static bool SubRev_TryReadLiveTextForBinding(const SubtitleReverseBinding &b,
                                             uintptr_t curTextPtr,
                                             std::string &outText,
                                             bool *outFromBuf = nullptr) {
  outText.clear();
  if (outFromBuf)
    *outFromBuf = false;

  if (b.textBufValid && b.textBufPtr != 0 &&
      SubRev_TryReadAsciiPreview(b.textBufPtr, 192, outText)) {
    if (outFromBuf)
      *outFromBuf = true;
    return true;
  }

  uintptr_t liveTextPtr = curTextPtr;
  if (liveTextPtr == 0 && !SubRev_ReadTextPtrForBinding(b, &liveTextPtr))
    liveTextPtr = 0;
  if (liveTextPtr == 0)
    return false;

  return SubRev_TryReadAsciiPreview(liveTextPtr, 192, outText);
}
static uint32_t SubRev_SimpleSig32(const char *s, size_t n) {
  // FNV-1a 32-bit, cheap for short prefixes.
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}
static bool SubRev_TryFindTextCopyNearSlot(uintptr_t slotPtr,
                                           const char *localizedText,
                                           uintptr_t *outBufPtr,
                                           uint32_t *outSig) {
  if (!outBufPtr || !outSig)
    return false;
  *outBufPtr = 0;
  *outSig = 0;
  if (slotPtr < 0x10000 || !localizedText || !localizedText[0])
    return false;

  // Probe up to a small prefix; enough to uniquely identify the string within
  // the subtitle slot struct without huge scans.
  size_t maxProbe = 48;
  char rawBuf[49] = {};
  size_t rawLen = 0;
  for (size_t i = 0; i < maxProbe; ++i) {
    uint8_t c = 0;
    if (!SubRev_ReadU8((uintptr_t)localizedText + i, &c))
      break;
    if (c == 0)
      break;
    rawBuf[rawLen++] = (char)c;
  }
  if (rawLen < 8)
    return false;
  rawBuf[rawLen] = '\0';
  const std::string raw(rawBuf, rawLen);

  uintptr_t start = (slotPtr > 0x800) ? (slotPtr - 0x800) : (uintptr_t)0x10000;
  uintptr_t stop = slotPtr + 0x4000;

  auto scanFor = [&](const std::string &patStr) -> bool {
    if (patStr.size() < 8)
      return false;
    const size_t patLen = (patStr.size() > 32) ? 32 : patStr.size();
    const char *pat = patStr.c_str();
    const uint32_t patSig = SubRev_SimpleSig32(pat, patLen);

    uintptr_t cursor = start;
    while (cursor < stop) {
      MEMORY_BASIC_INFORMATION mbi = {};
      if (VirtualQuery((LPCVOID)cursor, &mbi, sizeof(mbi)) == 0)
        break;
      uintptr_t regionBase = (uintptr_t)mbi.BaseAddress;
      SIZE_T regionSizeRaw = mbi.RegionSize;
      uintptr_t regionEnd = regionBase + regionSizeRaw;
      if (regionEnd <= regionBase)
        break;

      uintptr_t scanBase = cursor;
      if (scanBase < regionBase)
        scanBase = regionBase;
      uintptr_t scanEnd = regionEnd;
      if (scanEnd > stop)
        scanEnd = stop;
      if (scanEnd <= scanBase) {
        cursor = regionEnd;
        continue;
      }

      bool okRegion = (mbi.State == MEM_COMMIT) && !(mbi.Protect & PAGE_NOACCESS) &&
                      !(mbi.Protect & PAGE_GUARD);
      if (okRegion) {
        for (uintptr_t p = scanBase; p + patLen <= scanEnd; ++p) {
          if (*(volatile char *)p != pat[0])
            continue;
          if (memcmp((const void *)p, (const void *)pat, patLen) != 0)
            continue;
          *outBufPtr = p;
          *outSig = patSig;
          return true;
        }
      }

      cursor = regionEnd;
    }
    return false;
  };

  if (scanFor(raw)) {
    return true;
  }

  // Some paths copy without IW color codes; try stripped text as a fallback.
  std::string stripped = StripColorCodes(raw);
  if (!stripped.empty() && stripped != raw) {
    if (scanFor(stripped)) {
      return true;
    }
  }

  return false;
}
static bool SubRev_IsImageTextOrRdata(uintptr_t addr) {
  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0)
    return true;
  if (mbi.Type != MEM_IMAGE)
    return false;

  DWORD p = (mbi.Protect & 0xFF);
  bool executable = (p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
                     p == PAGE_EXECUTE_READWRITE ||
                     p == PAGE_EXECUTE_WRITECOPY);
  bool readonly = (p == PAGE_READONLY || p == PAGE_EXECUTE_READ);
  return executable || readonly;
}
static void SubRev_QueueSummaryLog(DWORD now) {
  DWORD prev = g_SubtitleReverseLastSummaryTick.load();
  if ((now - prev) < SUBREV_SUMMARY_INTERVAL_MS)
    return;
  if (!g_SubtitleReverseLastSummaryTick.compare_exchange_strong(prev, now))
    return;

  char buf[512];
  sprintf_s(
      buf,
      "[SUBREV_SUMMARY] enabled=%d veh=%d events=%lu bind=%lu ok=%lu conflict=%lu guardInst=%lu guardHits=%lu vehSeen=%lu ssSeen=%lu rearm=%lu remove=%lu(text=%lu flag=%lu alpha=%lu) failOpen=%lu",
      g_SubtitleReverseEnabled.load() ? 1 : 0,
      g_SubtitleReverseVehEnabled.load() ? 1 : 0,
      g_SubtitleReverseEventsSeen.load(), g_SubtitleReverseBindAttempts.load(),
      g_SubtitleReverseBindSuccess.load(),
      g_SubtitleReverseBindConflicts.load(),
      g_SubtitleReverseGuardInstall.load(), g_SubtitleReverseGuardHits.load(),
      g_SubtitleReverseVehGuardSeen.load(),
      g_SubtitleReverseVehSingleStepSeen.load(),
      g_SubtitleReverseGuardRearms.load(),
      g_SubtitleReverseRemoveDetected.load(),
      g_SubtitleReverseRemoveByText.load(),
      g_SubtitleReverseRemoveByFlag.load(),
      g_SubtitleReverseRemoveByAlpha.load(),
      g_SubtitleReverseFailOpenCount.load());
  LogToFile(buf);
}
static void SubRev_ClearWatchPagesLocked() {
  SubRev_EnsurePageSize();
  for (auto &w : g_SubtitleReverseWatchPages) {
    if (w.active.load() == 0)
      continue;
    uintptr_t page = w.pageBase.load();
    DWORD baseProtect = w.baseProtect.load();
    if (page != 0 && baseProtect != 0) {
      DWORD oldProtect = 0;
      VirtualProtect((LPVOID)page, g_SubtitleReversePageSize, baseProtect,
                     &oldProtect);
    }
    w.active.store(0);
    w.pageBase.store(0);
    w.slotPtr.store(0);
    w.baseProtect.store(0);
    w.lastHitTick.store(0);
    w.hits.store(0);
  }
}
static void SubRev_RearmWatchPagesFromVeh() {
  if (!g_SubtitleReverseVehEnabled.load() || !g_SubtitleReverseEnabled.load())
    return;

  SubRev_EnsurePageSize();
  for (auto &w : g_SubtitleReverseWatchPages) {
    if (w.active.load() == 0)
      continue;
    uintptr_t page = w.pageBase.load();
    DWORD baseProtect = w.baseProtect.load();
    if (page == 0 || baseProtect == 0)
      continue;

    DWORD oldProtect = 0;
    DWORD newProtect = (baseProtect & ~PAGE_GUARD) | PAGE_GUARD;
    if (VirtualProtect((LPVOID)page, g_SubtitleReversePageSize, newProtect,
                       &oldProtect)) {
      g_SubtitleReverseGuardRearms.fetch_add(1);
    }
  }
}
static void SubRev_DisableFailOpen(const char *reason) {
  if (!kEnableSubtitleReverseAutoFailOpen)
    return;

  bool expected = true;
  if (!g_SubtitleReverseEnabled.compare_exchange_strong(expected, false))
    return;

  g_SubtitleReverseFailOpenCount.fetch_add(1);
  g_SubtitleReverseVehEnabled.store(false);

  {
    std::lock_guard<std::mutex> lock(g_SubtitleReverseMutex);
    SubRev_ClearWatchPagesLocked();
    g_SubtitleReverseBindingsByKey.clear();
    g_SubtitleReverseKeyBySlot.clear();
    g_SubtitleReverseEvents.clear();
  }

  if (g_SubtitleReverseVehHandle) {
    RemoveVectoredExceptionHandler(g_SubtitleReverseVehHandle);
    g_SubtitleReverseVehHandle = nullptr;
  }

  std::string msg =
      "[SUBREV_SUMMARY] fail-open: reverse path disabled reason=" +
      std::string(reason ? reason : "unknown");
  LogToFile(msg);
}
static LONG CALLBACK SubRev_VectoredHandler(PEXCEPTION_POINTERS info) {
  if (!info || !info->ExceptionRecord || !info->ContextRecord) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (!g_SubtitleReverseEnabled.load() || !g_SubtitleReverseVehEnabled.load()) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code == STATUS_GUARD_PAGE_VIOLATION) {
    g_SubtitleReverseVehGuardSeen.fetch_add(1);
    uintptr_t faultAddr = 0;
    if (info->ExceptionRecord->NumberParameters >= 2) {
      faultAddr = (uintptr_t)info->ExceptionRecord->ExceptionInformation[1];
    }
    SubRev_EnsurePageSize();
    uintptr_t pageMask = (uintptr_t)g_SubtitleReversePageSize - 1;
    uintptr_t faultPage = faultAddr & ~pageMask;

    bool watched = false;
    for (auto &w : g_SubtitleReverseWatchPages) {
      if (w.active.load() == 0)
        continue;
      if (w.pageBase.load() == faultPage) {
        watched = true;
        w.lastHitTick.store(GetTickCount());
        w.hits.fetch_add(1);
        break;
      }
    }

    if (!watched)
      return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t op = 0;
    if (info->ExceptionRecord->NumberParameters >= 1) {
      op = (uintptr_t)info->ExceptionRecord->ExceptionInformation[0];
    }
    g_SubtitleReverseLastGuardOp.store(op);
#ifdef _M_X64
    g_SubtitleReverseLastGuardRcx.store((uintptr_t)info->ContextRecord->Rcx);
    g_SubtitleReverseLastGuardRdx.store((uintptr_t)info->ContextRecord->Rdx);
    g_SubtitleReverseLastGuardR8.store((uintptr_t)info->ContextRecord->R8);
    g_SubtitleReverseLastGuardR9.store((uintptr_t)info->ContextRecord->R9);
    g_SubtitleReverseLastGuardRax.store((uintptr_t)info->ContextRecord->Rax);
#endif

#ifdef _M_X64
    uintptr_t rip = (uintptr_t)info->ContextRecord->Rip;
#else
    uintptr_t rip = (uintptr_t)info->ContextRecord->Eip;
#endif
    g_SubtitleReverseLastGuardRip.store(rip);
    g_SubtitleReverseLastGuardAddr.store(faultAddr);
    g_SubtitleReverseLastGuardTick.store(GetTickCount());
    g_SubtitleReverseGuardHits.fetch_add(1);

    DWORD now = GetTickCount();
    DWORD winStart = g_SubtitleReverseGuardWindowTick.load();
    if (winStart == 0 || (now - winStart) > 1000) {
      g_SubtitleReverseGuardWindowTick.store(now);
      g_SubtitleReverseGuardWindowHits.store(1);
    } else {
      unsigned long hitInWin = g_SubtitleReverseGuardWindowHits.fetch_add(1) + 1;
      if (hitInWin > 700) {
        SubRev_DisableFailOpen("veh_guard_flood");
      }
    }

    g_SubtitleReverseNeedRearm.store(true);
    info->ContextRecord->EFlags |= 0x100; // Trap flag (single-step)
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  if (code == EXCEPTION_SINGLE_STEP) {
    g_SubtitleReverseVehSingleStepSeen.fetch_add(1);
    if (g_SubtitleReverseNeedRearm.exchange(false)) {
      SubRev_RearmWatchPagesFromVeh();
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }

  return EXCEPTION_CONTINUE_SEARCH;
}
static void SubRev_EnsureVehInstalledLocked() {
  if (!g_SubtitleReverseEnabled.load() || !g_SubtitleReverseVehEnabled.load())
    return;
  if (g_SubtitleReverseVehHandle)
    return;
  g_SubtitleReverseVehHandle = AddVectoredExceptionHandler(1, SubRev_VectoredHandler);
  if (g_SubtitleReverseVehHandle) {
    LogToFile("[SUBREV_GUARD] VEH installed");
  } else {
    LogToFile("[SUBREV_GUARD] VEH install failed");
    SubRev_DisableFailOpen("veh_install_failed");
  }
}
static bool SubRev_InstallGuardForSlotLocked(uintptr_t slotPtr,
                                             const std::string &key) {
  if (!g_SubtitleReverseEnabled.load() || !g_SubtitleReverseVehEnabled.load() ||
      slotPtr == 0) {
    return false;
  }

  SubRev_EnsureVehInstalledLocked();
  if (!g_SubtitleReverseVehHandle)
    return false;

  SubRev_EnsurePageSize();
  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery((LPCVOID)slotPtr, &mbi, sizeof(mbi)) == 0)
    return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS))
    return false;

  DWORD baseProtect = (mbi.Protect & ~PAGE_GUARD);
  if (!SubRev_IsWritableProtect(baseProtect))
    return false;

  uintptr_t page = ((uintptr_t)mbi.BaseAddress);

  SubtitleGuardWatchPage *targetWatch = nullptr;
  for (auto &w : g_SubtitleReverseWatchPages) {
    if (w.active.load() != 0 && w.pageBase.load() == page) {
      targetWatch = &w;
      break;
    }
  }
  if (!targetWatch) {
    for (auto &w : g_SubtitleReverseWatchPages) {
      if (w.active.load() == 0) {
        targetWatch = &w;
        break;
      }
    }
  }
  if (!targetWatch)
    return false;

  DWORD oldProtect = 0;
  DWORD newProtect = (baseProtect & ~PAGE_GUARD) | PAGE_GUARD;
  if (!VirtualProtect((LPVOID)page, g_SubtitleReversePageSize, newProtect,
                      &oldProtect)) {
    return false;
  }

  targetWatch->active.store(1);
  targetWatch->pageBase.store(page);
  targetWatch->baseProtect.store(baseProtect);
  targetWatch->slotPtr.store(slotPtr);
  targetWatch->lastHitTick.store(GetTickCount());
  targetWatch->hits.store(0);

  g_SubtitleReverseGuardInstall.fetch_add(1);

  // Self-test (limited): touch the guarded page once so we can confirm VEH is
  // actually receiving STATUS_GUARD_PAGE_VIOLATION in this environment.
  {
    static std::atomic<int> s_guardSelfTestCount{0};
    int n = s_guardSelfTestCount.fetch_add(1);
    if (n < 3) {
      MEMORY_BASIC_INFORMATION mbiBefore = {};
      DWORD protBefore = 0;
      if (VirtualQuery((LPCVOID)page, &mbiBefore, sizeof(mbiBefore)) != 0) {
        protBefore = mbiBefore.Protect;
      }

      unsigned long vehBefore = g_SubtitleReverseVehGuardSeen.load();
      unsigned long ssBefore = g_SubtitleReverseVehSingleStepSeen.load();
      unsigned long hitsBefore = g_SubtitleReverseGuardHits.load();
      volatile uint8_t tmp = *(volatile uint8_t *)slotPtr;
      (void)tmp;
      unsigned long hitsAfter = g_SubtitleReverseGuardHits.load();
      unsigned long vehAfter = g_SubtitleReverseVehGuardSeen.load();
      unsigned long ssAfter = g_SubtitleReverseVehSingleStepSeen.load();

      MEMORY_BASIC_INFORMATION mbiAfter = {};
      DWORD protAfter = 0;
      if (VirtualQuery((LPCVOID)page, &mbiAfter, sizeof(mbiAfter)) != 0) {
        protAfter = mbiAfter.Protect;
      }

      char sbuf[256];
      sprintf_s(
          sbuf,
          "[SUBREV_GUARD] selfTest slot=0x%llX page=0x%llX old=0x%X new=0x%X prot=0x%X->0x%X veh=%lu->%lu ss=%lu->%lu hits=%lu->%lu",
          (unsigned long long)slotPtr, (unsigned long long)page,
          (unsigned int)oldProtect, (unsigned int)newProtect,
          (unsigned int)protBefore, (unsigned int)protAfter, vehBefore, vehAfter,
          ssBefore, ssAfter, hitsBefore, hitsAfter);
      LogToFile(sbuf);
    }
  }

  char buf[256];
  sprintf_s(buf, "[SUBREV_GUARD] key=%s slot=0x%llX page=0x%llX",
            key.c_str(), (unsigned long long)slotPtr,
            (unsigned long long)page);
  LogToFile(buf);
  return true;
}
static int SubRev_ScoreCandidate(uintptr_t base, uintptr_t textPtr, int msgTimeMs,
                                 int fadeOutMs, DWORD enqueueTick,
                                 SubRevCandidateScoreDetail *outDetail) {
  if (base < 0x10000)
    return (std::numeric_limits<int>::min)();
  if (SubRev_IsImageTextOrRdata(base))
    return (std::numeric_limits<int>::min)();

  uintptr_t ptrAtBase = 0;
  bool ptrMatch = false;
  int ptrMatchKind = 0;
  if (SubRev_ReadPtr(base + 0x00, &ptrAtBase) && ptrAtBase == textPtr) {
    ptrMatch = true;
    ptrMatchKind = 2;
  } else {
    // Some IW engines keep runtime pools below 4GB and store pointers as u32.
    uint32_t ptr32 = 0;
    if (SubRev_ReadU32(base + 0x00, &ptr32) && (uintptr_t)ptr32 == textPtr) {
      ptrMatch = true;
      ptrMatchKind = 1;
    }
  }
  if (!ptrMatch)
    return (std::numeric_limits<int>::min)();

  uint32_t startTick = 0;
  uint32_t msgCandidate = 0;
  uint32_t fadeCandidate = 0;
  bool hasStart = SubRev_ReadU32(base - 0x14, &startTick);
  bool hasMsgFixed = SubRev_ReadU32(base - 0x0C, &msgCandidate);
  bool hasFadeFixed = SubRev_ReadU32(base - 0x08, &fadeCandidate);

  int score = 4;
  if ((base & 0x7) == 0) {
    score += 1;
  }
  if (ptrMatchKind == 1) {
    // If stored as u32, the next u32 is often a small field (duration/flags).
    uint32_t hi = 0;
    if (SubRev_ReadU32(base + 0x04, &hi)) {
      if (hi <= 60000) {
        score += 1;
      }
    }
  }

  int msgBestOff = (int)0x7FFFFFFF;
  int fadeBestOff = (int)0x7FFFFFFF;
  bool hasMsg = false;
  bool hasFade = false;
  uint32_t msgBestVal = 0;
  uint32_t fadeBestVal = 0;

  if (msgTimeMs > 0) {
    // Fixed layout candidate first.
    if (hasMsgFixed) {
      hasMsg = true;
      msgBestOff = -0x0C;
      msgBestVal = msgCandidate;
    }
    // Near scan to tolerate layout drift.
    for (int off = -0x40; off <= 0x40; off += 4) {
      uint32_t v = 0;
      if (!SubRev_ReadU32(base + (uintptr_t)off, &v))
        continue;
      int d = (int)v - msgTimeMs;
      if (d < 0)
        d = -d;
      if (d <= 64) {
        if (!hasMsg || d < (int)(msgBestVal > (uint32_t)msgTimeMs
                                    ? (msgBestVal - (uint32_t)msgTimeMs)
                                    : ((uint32_t)msgTimeMs - msgBestVal))) {
          hasMsg = true;
          msgBestOff = off;
          msgBestVal = v;
        }
      }
    }
  }
  if (fadeOutMs > 0) {
    if (hasFadeFixed) {
      hasFade = true;
      fadeBestOff = -0x08;
      fadeBestVal = fadeCandidate;
    }
    for (int off = -0x40; off <= 0x40; off += 4) {
      uint32_t v = 0;
      if (!SubRev_ReadU32(base + (uintptr_t)off, &v))
        continue;
      int d = (int)v - fadeOutMs;
      if (d < 0)
        d = -d;
      if (d <= 64) {
        if (!hasFade || d < (int)(fadeBestVal > (uint32_t)fadeOutMs
                                     ? (fadeBestVal - (uint32_t)fadeOutMs)
                                     : ((uint32_t)fadeOutMs - fadeBestVal))) {
          hasFade = true;
          fadeBestOff = off;
          fadeBestVal = v;
        }
      }
    }
  }

  if (hasMsg && msgTimeMs > 0) {
    int d = (int)msgBestVal - msgTimeMs;
    if (d < 0)
      d = -d;
    if (d <= 64)
      score += 2;
    else if (d <= 220)
      score += 1;
    if (msgBestOff == -0x0C)
      score += 2;
    else if (msgBestOff != (int)0x7FFFFFFF)
      score += 1;
  }
  if (hasFade && fadeOutMs > 0) {
    int d = (int)fadeBestVal - fadeOutMs;
    if (d < 0)
      d = -d;
    if (d <= 64)
      score += 2;
    else if (d <= 220)
      score += 1;
    if (fadeBestOff == -0x08)
      score += 2;
    else if (fadeBestOff != (int)0x7FFFFFFF)
      score += 1;
  }

  if (hasMsg && msgTimeMs > 0) {
    // (scored above)
  }
  if (hasFade && fadeOutMs > 0) {
    // (scored above)
  }
  if (hasStart) {
    DWORD d = (startTick > enqueueTick) ? (startTick - enqueueTick)
                                        : (enqueueTick - startTick);
    if (d <= 1600)
      score += 2;
    else if (d <= 5000)
      score += 1;
  }

  uint32_t flagVal = 0;
  bool hasFlag = SubRev_ReadU32(base - 0x18, &flagVal);
  if (hasFlag && flagVal <= 3) {
    score += 1;
  }

  if (outDetail) {
    outDetail->startTick = startTick;
    outDetail->hasStart = hasStart;
    outDetail->msgVal = msgBestVal;
    outDetail->hasMsg = hasMsg;
    outDetail->msgOff = (msgBestOff == (int)0x7FFFFFFF) ? 0 : msgBestOff;
    outDetail->fadeVal = fadeBestVal;
    outDetail->hasFade = hasFade;
    outDetail->fadeOff = (fadeBestOff == (int)0x7FFFFFFF) ? 0 : fadeBestOff;
    outDetail->flagVal = flagVal;
    outDetail->hasFlag = hasFlag;
    outDetail->ptrMatchKind = ptrMatchKind;
  }
  return score;
}
static bool SubRev_FindBestSlotCandidate(uintptr_t textPtr, int msgTimeMs,
                                         int fadeOutMs, DWORD enqueueTick,
                                         uintptr_t *outSlotPtr, int *outScore,
                                         SubRevCandidateScoreDetail *outDetail) {
  if (!outSlotPtr || !outScore || textPtr == 0)
    return false;
  *outSlotPtr = 0;
  *outScore = (std::numeric_limits<int>::min)();
  if (outDetail) {
    *outDetail = SubRevCandidateScoreDetail{};
  }

  int bestScore = (std::numeric_limits<int>::min)();
  uintptr_t bestBase = 0;
  SubRevCandidateScoreDetail bestDetail{};
  size_t totalPagesScanned = 0;
  size_t totalBytesScanned = 0;
  constexpr SIZE_T kScanChunkBytes = 2 * 1024 * 1024;

  auto boundedAdd = [](uintptr_t base, uintptr_t add) -> uintptr_t {
    const uintptr_t maxV = (std::numeric_limits<uintptr_t>::max)();
    if (base > (maxV - add)) {
      return maxV;
    }
    return base + add;
  };

  auto boundedSub = [](uintptr_t base, uintptr_t sub) -> uintptr_t {
    if (base < sub) {
      return 0;
    }
    return base - sub;
  };

  auto scanPass = [&](uintptr_t startCursor, uintptr_t stopCursor,
                      size_t maxPages, size_t maxBytes,
                      uintptr_t *outCursor) {
    uintptr_t cursor = (startCursor < 0x10000) ? (uintptr_t)0x10000 : startCursor;
    size_t pagesScanned = 0;
    size_t bytesScanned = 0;

    for (;;) {
      if (pagesScanned >= maxPages || bytesScanned >= maxBytes)
        break;
      if (cursor < 0x10000 || cursor >= stopCursor)
        break;

      MEMORY_BASIC_INFORMATION mbi = {};
      if (VirtualQuery((LPCVOID)cursor, &mbi, sizeof(mbi)) == 0) {
        break;
      }

      uintptr_t regionBase = (uintptr_t)mbi.BaseAddress;
      SIZE_T regionSizeRaw = mbi.RegionSize;
      uintptr_t regionEnd = regionBase + regionSizeRaw;
      if (regionEnd <= regionBase) {
        break;
      }
      if (!SubRev_IsCandidateRegion(mbi))
      {
        cursor = regionEnd;
        continue;
      }

      uintptr_t scanBase = cursor;
      if (scanBase < startCursor)
        scanBase = startCursor;
      if (scanBase < regionBase)
        scanBase = regionBase;
      if (scanBase >= stopCursor || scanBase >= regionEnd) {
        cursor = regionEnd;
        continue;
      }
      scanBase &= ~((uintptr_t)0x3ull); // align down to 4 bytes
      if (scanBase < regionBase)
        scanBase = regionBase;
      if (scanBase < startCursor)
        scanBase = startCursor;

      uintptr_t scanEnd = scanBase + (uintptr_t)kScanChunkBytes;
      if (scanEnd > regionEnd)
        scanEnd = regionEnd;
      if (scanEnd > stopCursor)
        scanEnd = stopCursor;
      if (scanEnd <= scanBase) {
        cursor = regionEnd;
        continue;
      }

      SIZE_T bytesToScan = (SIZE_T)(scanEnd - scanBase);
        if (textPtr <= 0xFFFFFFFFull) {
          const uint32_t needle = (uint32_t)textPtr;
          const uint32_t *dwData = (const uint32_t *)scanBase;
          size_t count = (size_t)(bytesToScan / sizeof(uint32_t));
          for (size_t i = 0; i < count; ++i) {
            if (dwData[i] != needle)
              continue;
            uintptr_t candidateBase = scanBase + i * sizeof(uint32_t);
            SubRevCandidateScoreDetail detail{};
            int score =
                SubRev_ScoreCandidate(candidateBase, textPtr, msgTimeMs,
                                      fadeOutMs, enqueueTick, &detail);
            if (score > bestScore) {
              bestScore = score;
              bestBase = candidateBase;
              bestDetail = detail;
            }
          }
        } else {
          const uintptr_t *ptrData = (const uintptr_t *)scanBase;
          size_t count = (size_t)(bytesToScan / sizeof(uintptr_t));
          for (size_t i = 0; i < count; ++i) {
            if (ptrData[i] != textPtr)
              continue;
            uintptr_t candidateBase = scanBase + i * sizeof(uintptr_t);
            SubRevCandidateScoreDetail detail{};
            int score =
                SubRev_ScoreCandidate(candidateBase, textPtr, msgTimeMs,
                                      fadeOutMs, enqueueTick, &detail);
            if (score > bestScore) {
              bestScore = score;
              bestBase = candidateBase;
              bestDetail = detail;
            }
          }
        }

      pagesScanned +=
          (size_t)((bytesToScan + g_SubtitleReversePageSize - 1) /
                   (SIZE_T)g_SubtitleReversePageSize);
      bytesScanned += (size_t)bytesToScan;
      cursor = scanEnd;
    }

    totalPagesScanned += pagesScanned;
    totalBytesScanned += bytesScanned;
    if (outCursor) {
      *outCursor = cursor;
    }
  };

  // Most observed subtitle localizedText pointers are < 4GB.
  // Prioritize scanning the low address range and around the pointer value.
  if (textPtr < SUBREV_LOW_ADDR_STOP) {
    uintptr_t center = textPtr & ~((uintptr_t)0xFFFFF); // 1MB aligned
    uintptr_t winStart = boundedSub(center, SUBREV_SCAN_WINDOW_BYTES / 2);
    if (winStart < 0x10000) {
      winStart = 0x10000;
    }
    uintptr_t winStop = boundedAdd(winStart, SUBREV_SCAN_WINDOW_BYTES);
    if (winStop > SUBREV_LOW_ADDR_STOP) {
      winStop = SUBREV_LOW_ADDR_STOP;
    }
    scanPass(winStart, winStop, SUBREV_SCAN_PRIMARY_MAX_PAGES,
             SUBREV_SCAN_PRIMARY_MAX_BYTES, nullptr);

    scanPass((uintptr_t)0x10000, (uintptr_t)SUBREV_LOW_ADDR_STOP,
             SUBREV_SCAN_LOW_MAX_PAGES, SUBREV_SCAN_LOW_MAX_BYTES, nullptr);
  } else {
    uintptr_t moduleBase = (uintptr_t)GetModuleHandle(NULL);
    if (moduleBase >= 0x10000) {
      uintptr_t primaryStop = boundedAdd(moduleBase, SUBREV_SCAN_WINDOW_BYTES);
      scanPass(moduleBase, primaryStop, SUBREV_SCAN_PRIMARY_MAX_PAGES,
               SUBREV_SCAN_PRIMARY_MAX_BYTES, nullptr);
    }
  }

  // Rolling scan cursor across low space to amortize work between events.
  {
    uintptr_t rollingStart = g_SubtitleReverseScanCursor;
    if (rollingStart < 0x10000 || rollingStart >= SUBREV_LOW_ADDR_STOP) {
      rollingStart = 0x10000;
    }
    uintptr_t rollingStop = boundedAdd(rollingStart, SUBREV_SCAN_WINDOW_BYTES);
    if (rollingStop > SUBREV_LOW_ADDR_STOP) {
      rollingStop = SUBREV_LOW_ADDR_STOP;
    }
    uintptr_t rollingCursor = rollingStart;
    scanPass(rollingStart, rollingStop, SUBREV_SCAN_ROLLING_MAX_PAGES,
             SUBREV_SCAN_ROLLING_MAX_BYTES, &rollingCursor);
    g_SubtitleReverseScanCursor = rollingCursor;
  }

  if (bestBase == 0 || bestScore < SUBREV_BIND_SCORE_THRESHOLD)
  {
    static DWORD s_lastMissLog = 0;
    DWORD now = GetTickCount();
    if ((now - s_lastMissLog) > 1500) {
      s_lastMissLog = now;
      char missBuf[256];
      sprintf_s(
          missBuf,
          "[SUBREV_BIND] miss text=0x%llX score=%d slot=0x%llX ptrKind=%d msg@%d=%u fade@%d=%u flag=%u start=%u scan=%lluKB/%llu pages",
          (unsigned long long)textPtr, bestScore,
          (unsigned long long)bestBase, bestDetail.ptrMatchKind,
          bestDetail.msgOff, (unsigned int)bestDetail.msgVal,
          bestDetail.fadeOff, (unsigned int)bestDetail.fadeVal,
          (unsigned int)bestDetail.flagVal, (unsigned int)bestDetail.startTick,
          (unsigned long long)(totalBytesScanned / 1024),
          (unsigned long long)totalPagesScanned);
      LogToFile(missBuf);
    }
    return false;
  }
  *outSlotPtr = bestBase;
  *outScore = bestScore;
  if (outDetail) {
    *outDetail = bestDetail;
  }
  return true;
}
static void SubRev_ApplyBindingToQueue(const std::string &key, uintptr_t slotPtr) {
  if (key.empty() || slotPtr == 0)
    return;
  std::lock_guard<std::mutex> qLock(g_InGameSubtitleMutex);
  for (auto &e : g_InGameSubtitleQueue) {
    if (!e.key.empty() && e.key == key) {
      e.nativeSlotPtr = slotPtr;
      e.nativeSlotValid = true;
    }
  }
}
static void SubRev_TryBindForEvent(const SubtitleEnqueueInstanceEvent &ev) {
  if (!g_SubtitleReverseEnabled.load())
    return;
  if (ev.key.empty() || ev.textPtr == 0)
    return;

  g_SubtitleReverseBindAttempts.fetch_add(1);
  uintptr_t slotPtr = 0;
  int score = (std::numeric_limits<int>::min)();
  SubRevCandidateScoreDetail detail{};
  if (!SubRev_FindBestSlotCandidate(ev.textPtr, ev.msgTimeMs, ev.fadeOutMs,
                                    ev.enqueueTick, &slotPtr, &score, &detail)) {
    return;
  }

  bool conflict = false;
  uintptr_t boundBufPtr = 0;
  {
    std::lock_guard<std::mutex> lock(g_SubtitleReverseMutex);
    auto itKey = g_SubtitleReverseKeyBySlot.find(slotPtr);
    if (itKey != g_SubtitleReverseKeyBySlot.end() && itKey->second != ev.key) {
      auto itExisting = g_SubtitleReverseBindingsByKey.find(itKey->second);
      if (itExisting != g_SubtitleReverseBindingsByKey.end()) {
        DWORD age = GetTickCount() - itExisting->second.boundTick;
        if (!itExisting->second.removeLatched && age < 8000) {
          conflict = true;
        }
      }
    }
    if (conflict) {
      g_SubtitleReverseBindConflicts.fetch_add(1);
    } else {
      SubtitleReverseBinding &b = g_SubtitleReverseBindingsByKey[ev.key];
      b.key = ev.key;
      b.slotPtr = slotPtr;
      b.textPtr = ev.textPtr;
      b.textPtrKind = detail.ptrMatchKind;
      b.msgOff = detail.msgOff;
      b.fadeOff = detail.fadeOff;
      b.msgTimeMs = ev.msgTimeMs;
      b.fadeOutMs = ev.fadeOutMs;
      b.enqueueTick = ev.enqueueTick;
      b.boundTick = GetTickCount();
      b.bindScore = score;
      b.lastGuardTick = 0;
      b.lastGuardRip = 0;
      b.removeTick = 0;
      b.removeReason = SUBREV_REMOVE_NONE;
      b.removeLatched = false;
      b.alphaZeroStreak = 0;
      b.flagEverNonZero = (detail.hasFlag && detail.flagVal != 0);
      b.alphaPrimed = false;
      b.textBufPtr = 0;
      b.textBufSig = 0;
      b.textBufValid = false;
      b.activeSinceTick = 0;
      b.lastNonZeroTick = 0;
      b.zeroStreak = 0;
      b.earlyZeroCount = 0;
      b.removeArmed = false;
      b.strEverNonZero = false;
      b.strZeroStreak = 0;
      b.lastRehydrateCandLogTick = 0;
      b.lastRehydrateMissLogTick = 0;

      // Try to locate a copied subtitle string buffer inside/near the slot.
      // This is often more reliable than tracking the transient input pointer.
      uintptr_t bufPtr = 0;
      uint32_t bufSig = 0;
      if (SubRev_TryFindTextCopyNearSlot(slotPtr, (const char *)ev.textPtr,
                                         &bufPtr, &bufSig)) {
        b.textBufPtr = bufPtr;
        b.textBufSig = bufSig;
        b.textBufValid = true;
        boundBufPtr = bufPtr;
      }

      g_SubtitleReverseKeyBySlot[slotPtr] = ev.key;
      SubRev_InstallGuardForSlotLocked(slotPtr, ev.key);
    }
  }

  if (conflict) {
    char buf[256];
    sprintf_s(buf,
              "[SUBREV_BIND] key=%s seq=%u conflict slot=0x%llX score=%d",
              ev.key.c_str(), ev.seq, (unsigned long long)slotPtr, score);
    LogToFile(buf);
    return;
  }

  g_SubtitleReverseBindSuccess.fetch_add(1);
  char buf[320];
  sprintf_s(buf,
            "[SUBREV_BIND] key=%s seq=%u slot=0x%llX score=%d ptrKind=%d msgOff=%d fadeOff=%d buf=0x%llX",
            ev.key.c_str(), ev.seq, (unsigned long long)slotPtr, score,
            detail.ptrMatchKind, detail.msgOff, detail.fadeOff,
            (unsigned long long)boundBufPtr);
  LogToFile(buf);
  SubRev_ApplyBindingToQueue(ev.key, slotPtr);
}
static bool ResolveSubtitleKeyFromLocalizedTextDetailed(
    const char *localizedText, std::string &outKey, bool *outExactMatch,
    std::string *outNormText) {
  outKey.clear();
  if (outExactMatch)
    *outExactMatch = false;
  if (outNormText)
    outNormText->clear();
  if (!localizedText || !localizedText[0])
    return false;

  std::string norm = NormalizeEnglishKey(localizedText);
  if (outNormText)
    *outNormText = norm;
  if (norm.empty())
    return false;

  std::shared_lock<std::shared_mutex> lock(g_SubtitleMapMutex);
  auto itExact = g_SubtitleOnlyMap.find(norm);
  if (itExact != g_SubtitleOnlyMap.end() && !itExact->second.key.empty()) {
    outKey = itExact->second.key;
    if (outExactMatch)
      *outExactMatch = true;
    return true;
  }

  const size_t prefixLens[] = {96, 90, 84, 80, 72, 64, 56, 48,
                               40, 32, 28, 24, 20, 16, 12};
  for (size_t len : prefixLens) {
    if (len >= norm.size())
      continue;
    auto itPrefix = g_SubtitleOnlyMap.find(norm.substr(0, len));
    if (itPrefix != g_SubtitleOnlyMap.end() && !itPrefix->second.key.empty()) {
      outKey = itPrefix->second.key;
      return true;
    }
  }

  return false;
}
static bool ResolveSubtitleKeyFromLocalizedText(const char *localizedText,
                                                std::string &outKey) {
  return ResolveSubtitleKeyFromLocalizedTextDetailed(localizedText, outKey,
                                                     nullptr, nullptr);
}
static void UpdateSubtitleRuntimeTimingByText(const char *localizedText,
                                              int windowId, int msgTimeMs,
                                              int fadeOutMs, int arg6) {
  if (!localizedText || !localizedText[0])
    return;
  std::string norm = NormalizeEnglishKey(localizedText);
  if (norm.empty())
    return;

  SubtitleRuntimeTiming t;
  t.msgTimeMs = msgTimeMs;
  t.fadeOutMs = fadeOutMs;
  t.arg6 = arg6;
  t.windowId = windowId;
  t.tick = GetTickCount();

  std::lock_guard<std::mutex> lock(g_SubtitleRuntimeTimingMutex);
  g_SubtitleRuntimeTimingByNormText[norm] = t;
  if (g_SubtitleRuntimeTimingByNormText.size() >
      SUBTITLE_RUNTIME_TIMING_NORM_MAX) {
    DWORD now = t.tick;
    for (auto it = g_SubtitleRuntimeTimingByNormText.begin();
         it != g_SubtitleRuntimeTimingByNormText.end();) {
      if ((now - it->second.tick) > SUBTITLE_RUNTIME_TIMING_STALE_MS) {
        it = g_SubtitleRuntimeTimingByNormText.erase(it);
      } else {
        ++it;
      }
    }
    while (g_SubtitleRuntimeTimingByNormText.size() >
           SUBTITLE_RUNTIME_TIMING_NORM_MAX) {
      g_SubtitleRuntimeTimingByNormText.erase(
          g_SubtitleRuntimeTimingByNormText.begin());
    }
  }
}
static void UpdateSubtitleRuntimeTiming(const std::string &key, int windowId,
                                        int msgTimeMs, int fadeOutMs,
                                        int arg6) {
  if (key.empty())
    return;
  SubtitleRuntimeTiming t;
  t.msgTimeMs = msgTimeMs;
  t.fadeOutMs = fadeOutMs;
  t.arg6 = arg6;
  t.windowId = windowId;
  t.tick = GetTickCount();

  std::lock_guard<std::mutex> lock(g_SubtitleRuntimeTimingMutex);
  g_SubtitleRuntimeTimingByKey[key] = t;
  if (g_SubtitleRuntimeTimingByKey.size() > SUBTITLE_RUNTIME_TIMING_KEY_MAX) {
    DWORD now = t.tick;
    for (auto it = g_SubtitleRuntimeTimingByKey.begin();
         it != g_SubtitleRuntimeTimingByKey.end();) {
      if ((now - it->second.tick) > SUBTITLE_RUNTIME_TIMING_STALE_MS) {
        it = g_SubtitleRuntimeTimingByKey.erase(it);
      } else {
        ++it;
      }
    }
    while (g_SubtitleRuntimeTimingByKey.size() >
           SUBTITLE_RUNTIME_TIMING_KEY_MAX) {
      g_SubtitleRuntimeTimingByKey.erase(g_SubtitleRuntimeTimingByKey.begin());
    }
  }
}
static bool TryGetSubtitleRuntimeTimingByText(const std::string &normText,
                                              SubtitleRuntimeTiming &out,
                                              DWORD maxAgeMs) {
  if (normText.empty())
    return false;
  std::lock_guard<std::mutex> lock(g_SubtitleRuntimeTimingMutex);
  auto it = g_SubtitleRuntimeTimingByNormText.find(normText);
  if (it == g_SubtitleRuntimeTimingByNormText.end())
    return false;
  DWORD now = GetTickCount();
  if ((now - it->second.tick) > maxAgeMs)
    return false;
  out = it->second;
  return true;
}
static bool TryGetSubtitleRuntimeTiming(const std::string &key,
                                        SubtitleRuntimeTiming &out,
                                        DWORD maxAgeMs) {
  if (key.empty())
    return false;
  std::lock_guard<std::mutex> lock(g_SubtitleRuntimeTimingMutex);
  auto it = g_SubtitleRuntimeTimingByKey.find(key);
  if (it == g_SubtitleRuntimeTimingByKey.end())
    return false;
  DWORD now = GetTickCount();
  if ((now - it->second.tick) > maxAgeMs)
    return false;
  out = it->second;
  return true;
}
static void ApplyRuntimeTimingToSubtitleQueue(const std::string &key,
                                              int msgTimeMs, int fadeOutMs,
                                              int arg6, DWORD tick) {
  if (key.empty())
    return;

  bool updated = false;
  {
    std::lock_guard<std::mutex> lock(g_InGameSubtitleMutex);
    for (auto &e : g_InGameSubtitleQueue) {
      if (!e.key.empty() && e.key == key) {
        e.msgTimeMs = msgTimeMs;
        e.fadeOutMs = fadeOutMs;
        e.runtimeArg6 = arg6;
        e.runtimeTick = tick;
        updated = true;
      }
    }
  }

  if (updated) {
    static int s_logCount = 0;
    if (s_logCount < 120) {
      s_logCount++;
      char buf[256];
      sprintf_s(buf,
                "[SEH-TIMING-LATE] key=%s msg=%d fade=%d arg6=%d",
                key.c_str(), msgTimeMs, fadeOutMs, arg6);
      LogToFile(buf);
    }
  }
}
static void SubtitleReverse_OnEnqueueInstance(const std::string &key,
                                              const char *localizedText,
                                              int msgTimeMs, int fadeOutMs,
                                              DWORD enqueueTick) {
  if (!g_SubtitleReverseEnabled.load())
    return;
  if (key.empty() || !localizedText || !localizedText[0])
    return;

  SubtitleEnqueueInstanceEvent ev;
  ev.key = key;
  ev.textPtr = (uintptr_t)localizedText;
  ev.msgTimeMs = msgTimeMs;
  ev.fadeOutMs = fadeOutMs;
  ev.enqueueTick = enqueueTick;
  ev.seq = g_SubtitleReverseNextSeq.fetch_add(1);

  g_SubtitleReverseEventsSeen.fetch_add(1);
  {
    std::lock_guard<std::mutex> lock(g_SubtitleReverseMutex);
    g_SubtitleReverseEvents.push_back(ev);

    while (g_SubtitleReverseEvents.size() > SUBREV_MAX_EVENTS) {
      g_SubtitleReverseEvents.pop_front();
    }
    while (!g_SubtitleReverseEvents.empty()) {
      DWORD age = enqueueTick - g_SubtitleReverseEvents.front().enqueueTick;
      if (age <= SUBREV_EVENT_STALE_MS)
        break;
      g_SubtitleReverseEvents.pop_front();
    }
  }

  SubRev_TryBindForEvent(ev);
  SubRev_QueueSummaryLog(GetTickCount());
}
static void SubtitleReverse_PollLocked(DWORD now) {
  if (!g_SubtitleReverseEnabled.load())
    return;

  SubRev_EnsurePageSize();
  uintptr_t guardRip = g_SubtitleReverseLastGuardRip.exchange(0);
  uintptr_t guardAddr = g_SubtitleReverseLastGuardAddr.exchange(0);
  DWORD guardTick = g_SubtitleReverseLastGuardTick.exchange(0);
  uintptr_t guardOp = 0;
  uintptr_t guardRcx = 0;
  uintptr_t guardRdx = 0;
  uintptr_t guardR8 = 0;
  uintptr_t guardR9 = 0;
  uintptr_t guardRax = 0;
  if (guardTick != 0) {
    guardOp = g_SubtitleReverseLastGuardOp.exchange(0);
    guardRcx = g_SubtitleReverseLastGuardRcx.exchange(0);
    guardRdx = g_SubtitleReverseLastGuardRdx.exchange(0);
    guardR8 = g_SubtitleReverseLastGuardR8.exchange(0);
    guardR9 = g_SubtitleReverseLastGuardR9.exchange(0);
    guardRax = g_SubtitleReverseLastGuardRax.exchange(0);
  }

  std::unordered_set<std::string> activeKeys;
  activeKeys.reserve(g_InGameSubtitleQueue.size());
  for (const auto &e : g_InGameSubtitleQueue) {
    if (!e.key.empty())
      activeKeys.insert(e.key);
  }
  const bool queueEmpty = g_InGameSubtitleQueue.empty();

  {
    std::lock_guard<std::mutex> lock(g_SubtitleReverseMutex);
    std::vector<std::string> dropKeys;

    if (guardTick != 0 && guardAddr != 0) {
      uintptr_t pageMask = (uintptr_t)g_SubtitleReversePageSize - 1;
      uintptr_t faultPage = guardAddr & ~pageMask;
      for (auto &kv : g_SubtitleReverseBindingsByKey) {
        SubtitleReverseBinding &b = kv.second;
        if (b.slotPtr == 0)
          continue;
        uintptr_t slotPage = b.slotPtr & ~pageMask;
        if (slotPage == faultPage) {
           b.lastGuardTick = guardTick;
           b.lastGuardRip = guardRip;
           static DWORD s_lastRipLog = 0;
           if ((now - s_lastRipLog) > 200) {
             s_lastRipLog = now;
            char rbuf[640];
            uintptr_t modBase = (uintptr_t)GetModuleHandleA(NULL);
            unsigned long long ripOff =
                (modBase && guardRip >= modBase)
                    ? (unsigned long long)(guardRip - modBase)
                    : 0ull;
            char opCh = '?';
            if (guardOp == 0) {
              opCh = 'R';
            } else if (guardOp == 1) {
              opCh = 'W';
            } else if (guardOp == 8) {
              opCh = 'X';
            }

            unsigned char bytes[16] = {};
            bool hasBytes = false;
            if (guardRip > 0x10000 && IsSafeRead((void *)guardRip, 16)) {
              memcpy(bytes, (const void *)guardRip, sizeof(bytes));
              hasBytes = true;
            }

            if (hasBytes) {
              sprintf_s(
                  rbuf,
                  "[SUBREV_RIP] key=%s rip=0x%llX(+0x%llX) op=%c addr=0x%llX slot=0x%llX "
                  "rcx=0x%llX rdx=0x%llX r8=0x%llX r9=0x%llX rax=0x%llX "
                  "b=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                  b.key.c_str(), (unsigned long long)guardRip, ripOff, opCh,
                  (unsigned long long)guardAddr,
                  (unsigned long long)b.slotPtr,
                  (unsigned long long)guardRcx, (unsigned long long)guardRdx,
                  (unsigned long long)guardR8, (unsigned long long)guardR9,
                  (unsigned long long)guardRax,
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                  bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                  bytes[12], bytes[13], bytes[14], bytes[15]);
            } else {
              sprintf_s(
                  rbuf,
                  "[SUBREV_RIP] key=%s rip=0x%llX(+0x%llX) op=%c addr=0x%llX slot=0x%llX "
                  "rcx=0x%llX rdx=0x%llX r8=0x%llX r9=0x%llX rax=0x%llX",
                  b.key.c_str(), (unsigned long long)guardRip, ripOff, opCh,
                  (unsigned long long)guardAddr,
                  (unsigned long long)b.slotPtr,
                  (unsigned long long)guardRcx, (unsigned long long)guardRdx,
                  (unsigned long long)guardR8, (unsigned long long)guardR9,
                  (unsigned long long)guardRax);
            }
            LogToFile(rbuf);

            SubRev_LogRipDetailsOnce(guardRip, (uintptr_t)ripOff);
            if (!b.slotDumped) {
              b.slotDumped = true;
              char sbuf[320] = {};
              sprintf_s(
                  sbuf,
                  "[SUBREV_SLOT] key=%s slot=0x%llX ptrKind=%d msgOff=%d fadeOff=%d text=0x%llX buf=0x%llX score=%d",
                  b.key.c_str(), (unsigned long long)b.slotPtr, b.textPtrKind,
                  b.msgOff, b.fadeOff, (unsigned long long)b.textPtr,
                  (unsigned long long)(b.textBufValid ? b.textBufPtr : 0),
                  b.bindScore);
              LogToFile(sbuf);
              LogCodeBytes("SLOT", b.slotPtr, 256);
              SubRev_LogFloat4Candidates("SLOT", b.slotPtr, 0x200, 24);
              if (b.textBufValid && b.textBufPtr != 0) {
                LogCodeBytes("SLOT_BUF", b.textBufPtr, 128);
              }
            }
           }
           break;
         }
       }
    }

    for (auto &kv : g_SubtitleReverseBindingsByKey) {
      SubtitleReverseBinding &b = kv.second;
      if (b.slotPtr == 0)
        continue;

      if (!b.removeLatched) {
        int reason = SUBREV_REMOVE_NONE;
        uintptr_t curTextPtr = 0;
        bool activeKnown = false;
        bool active = false;
        bool activeByBuf = false;
        bool hasStrByte = false;
        uint8_t strByte0 = 0;

        // Prefer a copied slot buffer if we found it.
        if (b.textBufValid && b.textBufPtr != 0) {
          uint8_t c0 = 0;
          if (SubRev_ReadU8(b.textBufPtr, &c0)) {
            activeKnown = true;
            activeByBuf = true;
            active = (c0 != 0);
          } else {
            // Buffer pointer became invalid; fall back to pointer tracking.
            b.textBufValid = false;
            b.textBufPtr = 0;
            b.textBufSig = 0;
          }
        }

        if (!activeKnown) {
          bool readText = SubRev_ReadTextPtrForBinding(b, &curTextPtr);
          if (readText) {
            activeKnown = true;
            active = (curTextPtr != 0);
          }
        }

        constexpr DWORD kArmMs = 250;
        constexpr DWORD kBindInactiveTimeoutMs = 800;
        constexpr int kZeroStreakToRemove = 2;
        constexpr int kStrZeroStreakToRemove = 3;

        if (curTextPtr != 0) {
          uint8_t c0 = 0;
          if (SubRev_ReadU8(curTextPtr, &c0)) {
            hasStrByte = true;
            strByte0 = c0;
            if (c0 != 0) {
              b.strEverNonZero = true;
              b.strZeroStreak = 0;
            } else {
              if (b.strEverNonZero) {
                b.strZeroStreak++;
              } else {
                b.strZeroStreak = 0;
              }
            }
          } else {
            b.strZeroStreak = 0;
          }
        } else {
          b.strZeroStreak = 0;
        }

        // If we can read the pointed string, treat empty-string as inactive even
        // when the pointer is non-null (engine often clears first byte).
        if (activeKnown && curTextPtr != 0 && hasStrByte) {
          active = (strByte0 != 0);
        }

        if (activeKnown) {
          if (active) {
            if (b.activeSinceTick == 0) {
              b.activeSinceTick = now;
              b.removeArmed = false;
            }
            b.lastNonZeroTick = now;
            b.zeroStreak = 0;
            if (!b.removeArmed && b.activeSinceTick > 0 &&
                (now - b.activeSinceTick) >= kArmMs) {
              b.removeArmed = true;
            }
          } else {
            // If we never observed it active, drop the binding after a short timeout.
            if (b.activeSinceTick == 0) {
              DWORD bindAge = now - b.boundTick;
              if (bindAge >= kBindInactiveTimeoutMs) {
                dropKeys.push_back(b.key);
              }
            } else {
              b.zeroStreak++;
              // If it went inactive before being "armed", treat as a false bind and fail-open.
              if (!b.removeArmed) {
                DWORD activeAge = now - b.activeSinceTick;
                if (activeAge < kArmMs) {
                  dropKeys.push_back(b.key);
                }
              } else if (b.zeroStreak >= kZeroStreakToRemove) {
                reason = SUBREV_REMOVE_TEXTPTR;
              }
            }
          }
        }

        if (reason == SUBREV_REMOVE_NONE && b.removeArmed && b.strEverNonZero &&
            b.strZeroStreak >= kStrZeroStreakToRemove) {
          reason = SUBREV_REMOVE_TEXTPTR;
        }

        if (reason != SUBREV_REMOVE_NONE) {
          b.removeLatched = true;
          b.removeReason = reason;
          b.removeTick = (b.lastGuardTick > 0 &&
                          (now - b.lastGuardTick) <= SUBREV_REMOVE_SETTLE_MS)
                             ? b.lastGuardTick
                             : now;
          g_SubtitleReverseRemoveDetected.fetch_add(1);
          if (reason == SUBREV_REMOVE_TEXTPTR) {
            g_SubtitleReverseRemoveByText.fetch_add(1);
          } else if (reason == SUBREV_REMOVE_FLAG) {
            g_SubtitleReverseRemoveByFlag.fetch_add(1);
          } else if (reason == SUBREV_REMOVE_ALPHA) {
            g_SubtitleReverseRemoveByAlpha.fetch_add(1);
          }

          char buf[320];
          sprintf_s(
              buf,
              "[SUBREV_REMOVE] key=%s slot=0x%llX reason=%d tick=%lu rip=0x%llX cur=0x%llX byBuf=%d c0=%u",
              b.key.c_str(), (unsigned long long)b.slotPtr, reason,
              (unsigned long)b.removeTick, (unsigned long long)b.lastGuardRip,
              (unsigned long long)curTextPtr, activeByBuf ? 1 : 0,
              (unsigned int)strByte0);
          LogToFile(buf);
        }

        bool keyActive = activeKeys.find(b.key) != activeKeys.end();
        if (!keyActive && activeKnown && active && !b.removeLatched) {
          constexpr DWORD kRehydrateLogIntervalMs = 1200;
          std::string liveText;
          bool fromBuf = false;
          bool readLive =
              SubRev_TryReadLiveTextForBinding(b, curTextPtr, liveText, &fromBuf);
          if (readLive) {
            std::string resolvedKey;
            std::string normText;
            bool exactMatch = false;
            bool resolved = ResolveSubtitleKeyFromLocalizedTextDetailed(
                liveText.c_str(), resolvedKey, &exactMatch, &normText);
            if (resolved) {
              if ((now - b.lastRehydrateCandLogTick) >=
                  kRehydrateLogIntervalMs) {
                b.lastRehydrateCandLogTick = now;
                std::string preview = liveText;
                for (char &ch : preview) {
                  if (ch == '\r' || ch == '\n' || ch == '\t')
                    ch = ' ';
                  else if (ch == '"')
                    ch = '\'';
                }
                if (preview.size() > 120)
                  preview.resize(120);

                char candBuf[768];
                sprintf_s(
                    candBuf,
                    "[SUBREV-REHYDRATE-CAND] bind=%s resolved=%s same=%d exact=%d src=%s queueEmpty=%d slot=0x%llX cur=0x%llX len=%zu normLen=%zu text=\"%s\"",
                    b.key.c_str(), resolvedKey.c_str(),
                    (resolvedKey == b.key) ? 1 : 0, exactMatch ? 1 : 0,
                    fromBuf ? "buf" : "ptr", queueEmpty ? 1 : 0,
                    (unsigned long long)b.slotPtr,
                    (unsigned long long)curTextPtr, liveText.size(),
                    normText.size(), preview.c_str());
                LogToFile(candBuf);
              }
            } else if ((now - b.lastRehydrateMissLogTick) >=
                       kRehydrateLogIntervalMs) {
              b.lastRehydrateMissLogTick = now;
              std::string preview = liveText;
              for (char &ch : preview) {
                if (ch == '\r' || ch == '\n' || ch == '\t')
                  ch = ' ';
                else if (ch == '"')
                  ch = '\'';
              }
              if (preview.size() > 120)
                preview.resize(120);

              char missBuf[768];
              sprintf_s(
                  missBuf,
                  "[SUBREV-REHYDRATE-MISS] bind=%s src=%s queueEmpty=%d slot=0x%llX cur=0x%llX len=%zu text=\"%s\"",
                  b.key.c_str(), fromBuf ? "buf" : "ptr", queueEmpty ? 1 : 0,
                  (unsigned long long)b.slotPtr,
                  (unsigned long long)curTextPtr, liveText.size(),
                  preview.c_str());
              LogToFile(missBuf);
            }
          } else if ((now - b.lastRehydrateMissLogTick) >=
                     kRehydrateLogIntervalMs) {
            b.lastRehydrateMissLogTick = now;
            char missBuf[512];
            sprintf_s(
                missBuf,
                "[SUBREV-REHYDRATE-MISS] bind=%s src=none queueEmpty=%d slot=0x%llX cur=0x%llX",
                b.key.c_str(), queueEmpty ? 1 : 0,
                (unsigned long long)b.slotPtr,
                (unsigned long long)curTextPtr);
            LogToFile(missBuf);
          }
        }
      }
    }

    for (auto &e : g_InGameSubtitleQueue) {
      if (e.key.empty())
        continue;
      auto it = g_SubtitleReverseBindingsByKey.find(e.key);
      if (it == g_SubtitleReverseBindingsByKey.end())
        continue;

      SubtitleReverseBinding &b = it->second;
      if (b.slotPtr == 0)
        continue;

      e.nativeSlotPtr = b.slotPtr;
      e.nativeSlotValid = true;
      if (b.removeLatched) {
        e.nativeRemoveTick = b.removeTick;
        e.nativeRemoveValid = true;
        e.nativeRemoveReason = b.removeReason;
      }
    }

    if (!dropKeys.empty()) {
      for (const std::string &k : dropKeys) {
        auto itDrop = g_SubtitleReverseBindingsByKey.find(k);
        if (itDrop == g_SubtitleReverseBindingsByKey.end())
          continue;
        const SubtitleReverseBinding &b = itDrop->second;
        if (b.slotPtr != 0) {
          g_SubtitleReverseKeyBySlot.erase(b.slotPtr);
        }
        g_SubtitleReverseBindingsByKey.erase(itDrop);
      }
    }

    for (auto it = g_SubtitleReverseBindingsByKey.begin();
         it != g_SubtitleReverseBindingsByKey.end();) {
      const SubtitleReverseBinding &b = it->second;
      bool keyActive = activeKeys.find(it->first) != activeKeys.end();
      bool bindingLive =
          !b.removeLatched && b.lastNonZeroTick != 0 &&
          (now - b.lastNonZeroTick) <= SUBREV_EVENT_STALE_MS;
      DWORD age = now - b.boundTick;
      bool stale = (!keyActive && !bindingLive && age > SUBREV_EVENT_STALE_MS) ||
                   (b.removeLatched && (now - b.removeTick) > SUBREV_EVENT_STALE_MS);
      if (stale) {
        g_SubtitleReverseKeyBySlot.erase(b.slotPtr);
        it = g_SubtitleReverseBindingsByKey.erase(it);
      } else {
        ++it;
      }
    }

    if (g_SubtitleReverseBindingsByKey.size() > SUBREV_MAX_BINDINGS) {
      size_t over = g_SubtitleReverseBindingsByKey.size() - SUBREV_MAX_BINDINGS;
      for (auto it = g_SubtitleReverseBindingsByKey.begin();
           it != g_SubtitleReverseBindingsByKey.end() && over > 0;) {
        g_SubtitleReverseKeyBySlot.erase(it->second.slotPtr);
        it = g_SubtitleReverseBindingsByKey.erase(it);
        over--;
      }
    }
  }

  SubRev_QueueSummaryLog(GetTickCount());
}
static void SubtitleReverse_Reset() {
  std::lock_guard<std::mutex> lock(g_SubtitleReverseMutex);
  SubRev_ClearWatchPagesLocked();
  g_SubtitleReverseEvents.clear();
  g_SubtitleReverseBindingsByKey.clear();
  g_SubtitleReverseKeyBySlot.clear();
  g_SubtitleReverseScanCursor = (uintptr_t)0x10000;
  g_SubtitleReverseNeedRearm.store(false);
  g_SubtitleReverseLastGuardRip.store(0);
  g_SubtitleReverseLastGuardAddr.store(0);
  g_SubtitleReverseLastGuardTick.store(0);
  g_SubtitleReverseGuardWindowTick.store(0);
  g_SubtitleReverseGuardWindowHits.store(0);
}
static void SubtitleReverse_Shutdown() {
  {
    std::lock_guard<std::mutex> lock(g_SubtitleReverseMutex);
    SubRev_ClearWatchPagesLocked();
    g_SubtitleReverseEvents.clear();
    g_SubtitleReverseBindingsByKey.clear();
    g_SubtitleReverseKeyBySlot.clear();
  }

  if (g_SubtitleReverseVehHandle) {
    RemoveVectoredExceptionHandler(g_SubtitleReverseVehHandle);
    g_SubtitleReverseVehHandle = nullptr;
  }
}
static void ResetSubtitleStateForMapRestart() {
  TextHook_ClearSubtitleQueue();
  TextHook_ResetObjectiveRuntimeForMapRestart();
  TextHook_ClearHudNativeEntries();

  {
    std::lock_guard<std::mutex> lock(g_SubtitleNativeMutex);
    g_SubtitleNativeByNorm.clear();
    g_SubtitleNativeByKey.clear();
  }

  SubtitleReverse_Reset();
}
NativeSubtitleState TextHook_GetNativeSubtitleState() {
  std::lock_guard<std::mutex> lock(g_NativeStateMutex);
  return g_NativeSubtitleState;
}
void TextHook_UpdateNativeSubtitleState(float x, float y, const float *color,
                                        float fontHeight, int style,
                                        float xScale, float yScale) {
  std::lock_guard<std::mutex> lock(g_NativeStateMutex);
  g_NativeSubtitleState.x = x;
  g_NativeSubtitleState.y = y;
  g_NativeSubtitleState.xScale = xScale;
  g_NativeSubtitleState.yScale = yScale;
  if (color) {
    g_NativeSubtitleState.color[0] = color[0];
    g_NativeSubtitleState.color[1] = color[1];
    g_NativeSubtitleState.color[2] = color[2];
    g_NativeSubtitleState.color[3] = color[3];
  } else {
    // Default white
    g_NativeSubtitleState.color[0] = 1.0f;
    g_NativeSubtitleState.color[1] = 1.0f;
    g_NativeSubtitleState.color[2] = 1.0f;
    g_NativeSubtitleState.color[3] = 1.0f;
  }
  g_NativeSubtitleState.fontHeight = fontHeight;
  g_NativeSubtitleState.style = style; // Store style
  g_NativeSubtitleState.lastUpdate = GetTickCount();
  g_NativeSubtitleState.valid = true;

  // Also update NativeSubtitleTracker for fade detection
  NativeSubtitleTracker::SubtitleStateTracker::Instance().UpdateFromRenderCall(
      x, y, xScale, yScale, color, fontHeight, style);
}
bool TextHook_GetSubtitleNativeState(const std::string &key,
                                     const std::string &english,
                                     SubtitleNativeEntry &out) {
  std::lock_guard<std::mutex> lock(g_SubtitleNativeMutex);
  DWORD now = GetTickCount();

  // Prune stale entries
  if (g_SubtitleNativeByNorm.size() > 200) {
    for (auto it = g_SubtitleNativeByNorm.begin();
         it != g_SubtitleNativeByNorm.end();) {
      if ((now - it->second.lastUpdate) > SUBTITLE_NATIVE_STALE)
        it = g_SubtitleNativeByNorm.erase(it);
      else
        ++it;
    }
  }
  if (g_SubtitleNativeByKey.size() > 200) {
    for (auto it = g_SubtitleNativeByKey.begin();
         it != g_SubtitleNativeByKey.end();) {
      if ((now - it->second.lastUpdate) > SUBTITLE_NATIVE_STALE)
        it = g_SubtitleNativeByKey.erase(it);
      else
        ++it;
    }
  }

  if (!key.empty()) {
    auto it = g_SubtitleNativeByKey.find(key);
    if (it != g_SubtitleNativeByKey.end() &&
        (now - it->second.lastUpdate) < SUBTITLE_NATIVE_TIMEOUT) {
      out = it->second;
      return true;
    }
  }

  if (!english.empty()) {
    std::string norm = NormalizeEnglishKey(english);
    auto it = g_SubtitleNativeByNorm.find(norm);
    if (it != g_SubtitleNativeByNorm.end() &&
        (now - it->second.lastUpdate) < SUBTITLE_NATIVE_TIMEOUT) {
      out = it->second;
      return true;
    }
  }

  out.valid = false;
  return false;
}
void TextHook_UpdateSubtitleNativeFromKey(const std::string &key, float x,
                                          float y, float xScale, float yScale,
                                          const float *color, float fontHeight,
                                          int style) {
  if (key.empty())
    return;

  SubtitleNativeEntry entry{};
  entry.x = x;
  entry.y = y;
  entry.xScale = xScale;
  entry.yScale = yScale;
  entry.fontHeight = fontHeight;
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

  int totalChars = 0;
  {
    std::shared_lock<std::shared_mutex> lock(g_SubtitleMapMutex);
    auto it = g_SubtitleByKey.find(key);
    if (it != g_SubtitleByKey.end()) {
      totalChars = it->second.totalChars;
    }
  }
  entry.totalChars = (totalChars > 0) ? totalChars : 0;
  entry.visibleChars = entry.totalChars; // no native typewriter info here

  {
    std::lock_guard<std::mutex> lock(g_SubtitleNativeMutex);
    g_SubtitleNativeByKey[key] = entry;
  }
}
static void ShiftRenderSlotsForNewEntryLocked() {
  for (auto &existing : g_InGameSubtitleQueue) {
    int slot = existing.renderSlot;
    if (slot < 0 || slot >= (int)MAX_SUBTITLE_QUEUE) {
      slot = existing.stackIndex;
      if (slot < 0)
        slot = 0;
      if (slot >= (int)MAX_SUBTITLE_QUEUE)
        slot = (int)MAX_SUBTITLE_QUEUE - 1;
    }
    if (slot < (int)MAX_SUBTITLE_QUEUE - 1) {
      slot++;
    }
    existing.renderSlot = slot;
  }
}
static void PruneInGameSubtitleQueueLocked(DWORD now) {
  SubtitleReverse_PollLocked(now);
  if (g_InGameSubtitleQueue.empty())
    return;

  g_InGameSubtitleQueue.erase(
      std::remove_if(g_InGameSubtitleQueue.begin(), g_InGameSubtitleQueue.end(),
                     [now](const InGameSubtitleEntry &e) {
                       if (e.nativeRemoveValid) {
                         DWORD baseTick =
                             (e.firstSeen > 0) ? e.firstSeen : e.timestamp;
                         constexpr DWORD kMinNativeRemoveHoldMs = 220;
                         DWORD removeTick = e.nativeRemoveTick;
                         // Fail-open: ignore suspiciously-early removal.
                         if (removeTick > (baseTick + kMinNativeRemoveHoldMs)) {
                           DWORD keepTail =
                               (removeTick > 0)
                                   ? (removeTick + SUBREV_REMOVE_SETTLE_MS)
                                   : now;
                           return now > keepTail;
                         }
                       }
                       DWORD age = (e.firstSeen > 0) ? (now - e.firstSeen)
                                                     : (now - e.timestamp);
                       DWORD maxAgeMs = SUBTITLE_NEWEST_PRUNE_MS;
                       if (e.msgTimeMs > 0 && e.msgTimeMs < 30000) {
                         int fadeMs = (e.fadeOutMs > 0 && e.fadeOutMs < 10000)
                                          ? e.fadeOutMs
                                          : 500;
                         // msgTime is treated as total lifetime (includes tail fade).
                         // Keep a small margin to avoid clipping the final frame.
                         DWORD runtimeAge = (DWORD)(e.msgTimeMs + 350);
                         // For broken values where msgTime is too short, fall back to
                         // a safer lower-bound using fade time.
                         DWORD minSafeAge = (DWORD)(fadeMs + 350);
                         if (runtimeAge < minSafeAge)
                           runtimeAge = minSafeAge;
                         maxAgeMs = runtimeAge;
                       }
                       return age > maxAgeMs;
                     }),
      g_InGameSubtitleQueue.end());
}

// Getter for D3D11Hook to access current in-game subtitle
std::string TextHook_GetInGameSubtitle() {
  std::lock_guard<std::mutex> lock(g_InGameSubtitleMutex);

  static DWORD s_startTime = GetTickCount();
  DWORD now = GetTickCount();

  // FIX: Detect "Brief Black Screen" subtitle bug
  // Suppress subtitles for the first 15 seconds of process lifetime
  if (now - s_startTime < 15000) {
    return "";
  }

  // Clear subtitle after 6 seconds of in-game subtitle clock inactivity
  DWORD subtitleNow = TextHook_GetSubtitleClockNow();
  if (g_InGameSubtitle.timestamp > 0 &&
      (subtitleNow - g_InGameSubtitle.timestamp) > 6000) {
    g_InGameSubtitle.korean = "";
    g_InGameSubtitle.timestamp = 0;
  }

  // Keep queue compact even if legacy getter is used.
  PruneInGameSubtitleQueueLocked(subtitleNow);

  // Debug logging disabled (too verbose)
  // Uncomment for debugging subtitle timing issues:
  // static DWORD lastDebugLog = 0;
  // if (!g_InGameSubtitle.korean.empty() && (now - lastDebugLog) > 2000) {
  //   lastDebugLog = now;
  //   LogToFile("[TextHook] Subtitle age=" + std::to_string(now -
  //   g_InGameSubtitle.timestamp) + "ms");
  // }

  return g_InGameSubtitle.korean;
}
InGameSubtitleSnapshot TextHook_GetInGameSubtitleSnapshot() {
  std::lock_guard<std::mutex> lock(g_InGameSubtitleMutex);
  InGameSubtitleSnapshot snap;
  snap.text = g_InGameSubtitle.korean;
  snap.timestamp = g_InGameSubtitle.timestamp;
  return snap;
}
std::vector<InGameSubtitleEntry> TextHook_GetInGameSubtitleList() {
  std::lock_guard<std::mutex> lock(g_InGameSubtitleMutex);
  PruneInGameSubtitleQueueLocked(TextHook_GetSubtitleClockNow());
  return g_InGameSubtitleQueue;
}

bool TextHook_PollInjectSubtitleByEnglish(const std::string &rawEnglishText) {
  if (rawEnglishText.empty())
    return false;

  // Uppercase for lookup in g_EnglishToKey
  std::string upper = ToUpperAscii(rawEnglishText);

  // Try direct match first
  std::string foundKey;
  {
    auto it = g_EnglishToKey.find(upper);
    if (it != g_EnglishToKey.end()) {
      foundKey = it->second;
    }
  }

  // If no direct match, try runtime map
  if (foundKey.empty()) {
    auto it = g_RuntimeEnglishToKey.find(upper);
    if (it != g_RuntimeEnglishToKey.end() && !it->second.empty()) {
      foundKey = it->second;
    }
  }

  // If still no match, try stripping color codes then matching
  if (foundKey.empty()) {
    std::string stripped = StripColorCodes(upper);
    auto it = g_EnglishToKey.find(stripped);
    if (it != g_EnglishToKey.end()) {
      foundKey = it->second;
    }
  }

  if (foundKey.empty())
    return false;

  // Look up Korean translation
  auto itKor = g_KeyToKorean.find(foundKey);
  if (itKor == g_KeyToKorean.end() || itKor->second.empty()) {
    // Try uppercase key
    std::string keyUp = ToUpperAscii(foundKey);
    itKor = g_KeyToKorean.find(keyUp);
    if (itKor == g_KeyToKorean.end() || itKor->second.empty())
      return false;
  }

  DWORD clockNow = TextHook_GetSubtitleClockNow();
  std::lock_guard<std::mutex> lock(g_InGameSubtitleMutex);

  // Check if already in queue
  for (const auto &e : g_InGameSubtitleQueue) {
    if (e.key == foundKey)
      return false; // Already showing
  }

  // Inject new entry
  InGameSubtitleEntry newEntry;
  newEntry.text = itKor->second;
  newEntry.timestamp = clockNow;
  newEntry.firstSeen = clockNow;
  newEntry.key = foundKey;
  newEntry.english = rawEnglishText;
  newEntry.stackIndex = 0;
  newEntry.pushAnimStart = 0;
  newEntry.renderSlot = 0;
  newEntry.msgTimeMs = 5000; // Default subtitle duration
  newEntry.fadeOutMs = 600;
  newEntry.runtimeArg6 = 32;
  newEntry.runtimeTick = clockNow;
  newEntry.nativeSlotPtr = 0;
  newEntry.nativeSlotValid = false;
  newEntry.nativeRemoveTick = 0;
  newEntry.nativeRemoveValid = false;
  newEntry.nativeRemoveReason = 0;

  // Push existing entries up
  for (auto &ex : g_InGameSubtitleQueue) {
    int slot = ex.renderSlot;
    if (slot < 0) slot = 0;
    if (slot < (int)MAX_SUBTITLE_QUEUE - 1) slot++;
    ex.renderSlot = slot;
    ex.stackIndex = slot;
    if (ex.pushAnimStart == 0)
      ex.pushAnimStart = clockNow;
  }

  g_InGameSubtitleQueue.insert(g_InGameSubtitleQueue.begin(), newEntry);
  if (g_InGameSubtitleQueue.size() > MAX_SUBTITLE_QUEUE)
    g_InGameSubtitleQueue.resize(MAX_SUBTITLE_QUEUE);

  LogToFile("[POLL-INJECT] key=" + foundKey + " korean=" +
            Utf8Preview(itKor->second, 40));
  return true;
}
SubtitleReverseStats TextHook_GetSubtitleReverseStats() {
  SubtitleReverseStats stats{};
  stats.eventsSeen = g_SubtitleReverseEventsSeen.load();
  stats.bindAttempts = g_SubtitleReverseBindAttempts.load();
  stats.bindSuccess = g_SubtitleReverseBindSuccess.load();
  stats.bindConflicts = g_SubtitleReverseBindConflicts.load();
  stats.guardInstall = g_SubtitleReverseGuardInstall.load();
  stats.guardHits = g_SubtitleReverseGuardHits.load();
  stats.guardRearms = g_SubtitleReverseGuardRearms.load();
  stats.removeDetected = g_SubtitleReverseRemoveDetected.load();
  stats.removeByTextPtr = g_SubtitleReverseRemoveByText.load();
  stats.removeByFlag = g_SubtitleReverseRemoveByFlag.load();
  stats.removeByAlpha = g_SubtitleReverseRemoveByAlpha.load();
  stats.failOpenCount = g_SubtitleReverseFailOpenCount.load();
  stats.reverseEnabled = g_SubtitleReverseEnabled.load();
  stats.vehEnabled = g_SubtitleReverseVehEnabled.load();
  return stats;
}
bool TextHook_IsSubtitleKeyActive(const std::string &key,
                                  unsigned long maxAgeMs) {
  if (key.empty())
    return false;
  std::lock_guard<std::mutex> lock(g_InGameSubtitleMutex);
  DWORD now = TextHook_GetSubtitleClockNow();
  for (const auto &e : g_InGameSubtitleQueue) {
    if (!e.key.empty() && e.key == key) {
      return (now - e.timestamp) <= maxAgeMs;
    }
  }
  return false;
}
LastSubtitleDiag TextHook_GetLastSubtitleDiag() {
  std::lock_guard<std::mutex> lock(g_LastSubtitleDiagMutex);
  return g_LastSubtitleDiag;
}
static void DumpSubtitleDvarCandidates(const char *phaseTag) {
  typedef void *(*Dvar_FindVar_t)(const char *name);
  uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
  if (!base)
    return;

  Dvar_FindVar_t findVar =
      (Dvar_FindVar_t)(base + IW6Offsets::Dvar_FindVar_SP);
  if (!findVar) {
    LogToFile("[SUBDVAR] Dvar_FindVar unresolved");
    return;
  }

  char begin[128];
  sprintf_s(begin, "[SUBDVAR] BEGIN phase=%s",
            (phaseTag && phaseTag[0]) ? phaseTag : "?");
  LogToFile(begin);

  const char *names[] = {
      // Subtitle / message window dvars
      "cg_subtitleMinTime",
      "cg_subtitleWidthStandard",
      "cg_subtitleWidthWidescreen",
      "cg_gameMessageWidth",
      "cg_gameBoldMessageWidth",
      "con_gameMsgWindow1MsgTime",
      "con_gameMsgWindow1FadeInTime",
      "con_gameMsgWindow1FadeOutTime",
      "con_gameMsgWindow1ScrollTime",
      "con_gameMsgWindow1LineCount",
      "con_gameMsgWindow1Filter",
      "con_gameMsgWindow1SplitscreenScale",
      "con_gameMsgWindow2MsgTime",
      "con_gameMsgWindow2FadeInTime",
      "con_gameMsgWindow2FadeOutTime",
      "con_gameMsgWindow2ScrollTime",
      "con_gameMsgWindow2LineCount",
      "con_gameMsgWindow2Filter",
      "con_gameMsgWindow2SplitscreenScale",
      // Subtitles appear to enqueue into windowId=4 in this build (see [SUBENQ] win=4)
      "con_gameMsgWindow4MsgTime",
      "con_gameMsgWindow4FadeInTime",
      "con_gameMsgWindow4FadeOutTime",
      "con_gameMsgWindow4ScrollTime",
      "con_gameMsgWindow4LineCount",
      "con_gameMsgWindow4Filter",
      "con_gameMsgWindow4SplitscreenScale",

      // Candidate color/style dvars that may drive subtitle-like text tint/glow.
      "con_typewriterColorBase",
      "con_typewriterColorGlowUpdated",
      "con_typewriterColorGlowCompleted",
      "con_typewriterColorGlowFailed",
      "con_typewriterColorGlowCheckpoint",
      "friendlyNameFontColor",
      "friendlyNameFontGlowColor",
      "hostileNameFontColor",
      "hostileNameFontGlowColor",
  };

  for (const char *name : names) {
    void *dv = findVar(name);
    if (!dv) {
      LogToFile(std::string("[SUBDVAR][") +
                ((phaseTag && phaseTag[0]) ? phaseTag : "?") +
                "] not found: " + name);
      continue;
    }
    char head[256];
    sprintf_s(head, "[SUBDVAR][%s] %s ptr=%p",
              (phaseTag && phaseTag[0]) ? phaseTag : "?", name, dv);
    LogToFile(head);

    if (!IsSafeRead(dv, 0x80)) {
      LogToFile(std::string("[SUBDVAR][") +
                ((phaseTag && phaseTag[0]) ? phaseTag : "?") +
                "] unreadable: " + name);
      continue;
    }

    const unsigned int *dw = (const unsigned int *)dv;
    const float *fw = (const float *)dv;
    char row0[512], row1[512];
    sprintf_s(
        row0,
        "[SUBDVAR][%s] %s d0=%08X d1=%08X d2=%08X d3=%08X d4=%08X d5=%08X d6=%08X d7=%08X",
        (phaseTag && phaseTag[0]) ? phaseTag : "?", name, dw[0], dw[1], dw[2],
        dw[3], dw[4], dw[5], dw[6], dw[7]);
    sprintf_s(
        row1,
        "[SUBDVAR][%s] %s f0=%.3f f1=%.3f f2=%.3f f3=%.3f f4=%.3f f5=%.3f f6=%.3f f7=%.3f",
        (phaseTag && phaseTag[0]) ? phaseTag : "?", name, fw[0], fw[1], fw[2],
        fw[3], fw[4], fw[5], fw[6], fw[7]);
    LogToFile(row0);
    LogToFile(row1);

    // Heuristic: if this looks like a color dvar, scan for plausible float4 vecs
    // inside the dvar struct and print candidates (helps find the actual value
    // offset without full dvar struct reversing).
    if (strstr(name, "Color") || strstr(name, "color")) {
      int found = 0;
      const int scanBytes = 0x200;
      if (IsSafeRead(dv, scanBytes)) {
        const unsigned char *p = (const unsigned char *)dv;
        for (int off = 0; off <= (scanBytes - 16); off += 4) {
          float r = *(const float *)(p + off + 0);
          float g = *(const float *)(p + off + 4);
          float b = *(const float *)(p + off + 8);
          float a = *(const float *)(p + off + 12);
          auto finite01 = [](float v) -> bool {
            return std::isfinite(v) && (v >= 0.0f) && (v <= 1.0f);
          };
          if (!finite01(r) || !finite01(g) || !finite01(b) || !finite01(a))
            continue;
          if ((r + g + b) < 0.01f)
            continue;

          char v[320];
          sprintf_s(v,
                    "[SUBDVAR][%s] %s vec4@+0x%X = (%.3f, %.3f, %.3f, %.3f)",
                    (phaseTag && phaseTag[0]) ? phaseTag : "?", name, off, r,
                    g, b, a);
          LogToFile(v);
          if (++found >= 12)
            break;
        }
        if (found == 0) {
          LogToFile(std::string("[SUBDVAR][") +
                    ((phaseTag && phaseTag[0]) ? phaseTag : "?") + "] " + name +
                    " vec4 candidates: none in first 0x200 bytes");
        }
      }
    }
  }

  char end[128];
  sprintf_s(end, "[SUBDVAR] END phase=%s",
            (phaseTag && phaseTag[0]) ? phaseTag : "?");
  LogToFile(end);
}
static void DumpSubtitleDvarCandidatesEarlyOnce() {
  static bool s_done = false;
  if (s_done)
    return;
  s_done = true;
  DumpSubtitleDvarCandidates("EARLY");
}
static void DumpSubtitleDvarCandidatesLateOnce() {
  static bool s_done = false;
  if (s_done)
    return;
  s_done = true;
  DumpSubtitleDvarCandidates("LATE");
}
bool TextHook_TryGetDvarVec4(const char *name, float out[4]) {
  if (!name || !name[0] || !out)
    return false;

  typedef void *(*Dvar_FindVar_t)(const char *name);
  uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
  if (!base)
    return false;

  Dvar_FindVar_t findVar =
      (Dvar_FindVar_t)(base + IW6Offsets::Dvar_FindVar_SP);
  if (!findVar)
    return false;

  void *dv = findVar(name);
  if (!dv)
    return false;

  // IW6 dvar_s begins with pointers, then type field at +0x0C in this build
  // (see [SUBDVAR] dumps). For vec4, the current value appears at +0x10.
  if (!IsSafeRead(dv, 0x30))
    return false;

  uint32_t type = *(const uint32_t *)((const unsigned char *)dv + 0x0C);
  if (type != 4) // DVAR_TYPE_FLOAT4 in IW6
    return false;

  const float *v = (const float *)((const unsigned char *)dv + 0x10);
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(v[i]))
      return false;
  }
  out[0] = v[0];
  out[1] = v[1];
  out[2] = v[2];
  out[3] = v[3];
  return true;
}
bool TextHook_TryGetDvarInt(const char *name, int &outValue) {
  if (!name || !name[0])
    return false;

  typedef int (*Dvar_GetInt_t)(const char *name);
  typedef const char *(*Dvar_GetVariantStringWithDefault_t)(const char *name,
                                                            const char *fallback);
  typedef void *(*Dvar_FindVar_t)(const char *name);
  uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
  if (!base)
    return false;

  Dvar_GetInt_t getInt = nullptr;
  if (IW6Offsets::Dvar_GetInt_SP != 0) {
    getInt = (Dvar_GetInt_t)(base + IW6Offsets::Dvar_GetInt_SP);
  }
  Dvar_GetVariantStringWithDefault_t getVariantString = nullptr;
  if (IW6Offsets::Dvar_GetVariantStringWithDefault_SP != 0) {
    getVariantString = (Dvar_GetVariantStringWithDefault_t)(
        base + IW6Offsets::Dvar_GetVariantStringWithDefault_SP);
  }
  Dvar_FindVar_t findVar =
      (Dvar_FindVar_t)(base + IW6Offsets::Dvar_FindVar_SP);
  if (!findVar)
    return false;

  void *dv = findVar(name);
  if (!dv || !IsSafeRead(dv, 0x20))
    return false;

  if (getInt) {
    __try {
      outValue = getInt(name);
      return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }

  if (getVariantString) {
    __try {
      const char *raw = getVariantString(name, "");
      if (raw && raw[0]) {
        char *end = nullptr;
        const long parsed = strtol(raw, &end, 10);
        if (end != raw) {
          while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
            ++end;
          }
          if (*end == '\0') {
            outValue = (int)parsed;
            return true;
          }
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }

  const unsigned char *p = (const unsigned char *)dv;
  const int type = *(const int8_t *)(p + 0x0C);
  if (type == 0) { // bool
    outValue = (*(const uint8_t *)(p + 0x10) != 0) ? 1 : 0;
    return true;
  }
  if (type == 1) { // float
    float v = *(const float *)(p + 0x10);
    if (!std::isfinite(v))
      return false;
    outValue = (int)lroundf(v);
    return true;
  }
  if (type == 4) { // vec4: use x channel as best-effort scalar
    float v = *(const float *)(p + 0x10);
    if (!std::isfinite(v))
      return false;
    outValue = (int)lroundf(v);
    return true;
  }
  if (type != 5 && type != 6)
    return false;

  outValue = *(const int *)(p + 0x10);
  return true;
}
static void TrackSubtitleSehCaller(const char *key) {
  static uintptr_t s_gameBase = (uintptr_t)GetModuleHandleA(NULL);
  static uintptr_t s_gameSize = []() -> uintptr_t {
    MODULEINFO mi = {};
    HMODULE h = GetModuleHandleA(NULL);
    if (h &&
        GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi)) &&
        mi.SizeOfImage > 0) {
      return (uintptr_t)mi.SizeOfImage;
    }
    return (uintptr_t)0;
  }();
  static uintptr_t s_gameEnd =
      (s_gameBase && s_gameSize) ? (s_gameBase + s_gameSize) : 0;

  if (!s_gameBase)
    return;

  DWORD now = GetTickCount();
  void *frames[12] = {};
  USHORT captured = RtlCaptureStackBackTrace(0, 12, frames, nullptr);

  // Use first in-module stack frame as callsite (gateway return addresses are
  // often outside the game module and not useful for reverse targeting).
  uintptr_t callsiteOff = 0;
  for (USHORT i = 0; i < captured; ++i) {
    uintptr_t a = (uintptr_t)frames[i];
    if (s_gameEnd != 0 && a >= s_gameBase && a < s_gameEnd) {
      callsiteOff = a - s_gameBase;
      break;
    }
  }

  if (callsiteOff == 0) {
    uintptr_t retAddr = (uintptr_t)_ReturnAddress();
    if (retAddr >= s_gameBase && (s_gameEnd == 0 || retAddr < s_gameEnd)) {
      callsiteOff = retAddr - s_gameBase;
    } else {
      return;
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_SubtitleSehCallsiteMutex);
    auto &st = g_SubtitleSehCallsiteStats[callsiteOff];
    if (st.hits == 0) {
      st.firstTick = now;
      if (key)
        st.sampleKey = key;
    }
    st.hits++;
    st.lastTick = now;
    if (st.sampleKey.empty() && key)
      st.sampleKey = key;
  }

  // Log stack for first N subtitle events to discover stable parent frames.
  int stackCount = g_SubtitleSehStackLogCount.load();
  if (stackCount < 120) {
    void *framesLog[10] = {};
    USHORT capturedLog = RtlCaptureStackBackTrace(0, 10, framesLog, nullptr);
    std::stringstream ss;
    ss << "[SEH-SUB-CALL] key=" << (key ? key : "")
       << " call=+0x" << std::hex << callsiteOff << " stack=";
    int printed = 0;
    for (USHORT i = 0; i < capturedLog; ++i) {
      uintptr_t a = (uintptr_t)framesLog[i];
      if (s_gameEnd != 0 && (a < s_gameBase || a >= s_gameEnd))
        continue;
      if (printed > 0)
        ss << ",";
      ss << "+0x" << std::hex << (a - s_gameBase);
      printed++;
      if (printed >= 6)
        break;
    }
    if (printed > 0) {
      LogToFile(ss.str());
      g_SubtitleSehStackLogCount.fetch_add(1);
    }
  }

  // Periodic top callsite summary for quick reverse targeting.
  DWORD prevSummary = g_SubtitleSehLastSummaryTick.load();
  if ((now - prevSummary) > 5000 &&
      g_SubtitleSehLastSummaryTick.compare_exchange_strong(prevSummary, now)) {
    std::vector<std::pair<uintptr_t, SubtitleSehCallsiteStat>> stats;
    {
      std::lock_guard<std::mutex> lock(g_SubtitleSehCallsiteMutex);
      stats.reserve(g_SubtitleSehCallsiteStats.size());
      for (const auto &kv : g_SubtitleSehCallsiteStats) {
        stats.push_back(kv);
      }
    }
    std::sort(stats.begin(), stats.end(),
              [](const auto &a, const auto &b) {
                if (a.second.hits != b.second.hits)
                  return a.second.hits > b.second.hits;
                return a.first < b.first;
              });

    size_t topN = (stats.size() < 6) ? stats.size() : 6;
    for (size_t i = 0; i < topN; ++i) {
      const auto &it = stats[i];
      char buf[512];
      sprintf_s(buf,
                "[SEH-SUB-TOP] rank=%zu caller=+0x%llX hits=%u age=%lums key=%s",
                i + 1, (unsigned long long)it.first, (unsigned)it.second.hits,
                (unsigned long)(now - it.second.firstTick),
                it.second.sampleKey.c_str());
      LogToFile(buf);
    }
  }
}

// =============================================================================
// SEH_StringEd_GetString DETOUR - Intercepts localization key lookups
// =============================================================================
// g_EngToKorOverlayMap and g_EngToKorMutex are now declared earlier in the file
// near other global translation maps (around line 353)

const char *__fastcall Detour_SEH_StringEd_GetString(const char *reference) {

  // Always call original first to get the English text
  const char *original = nullptr;
  if (Original_SEH_StringEd_GetString) {
    original = Original_SEH_StringEd_GetString(reference);
  }

  if (!reference)
    return original;

  // CREDITS_ key fast path: no Korean translation exists for credits text and
  // any CREDITS_ key is a reliable signal that the credits screen is active.
  // Activate the guard immediately and return — bypasses all logging and
  // expensive lookups (fixing both the HUD false-positive and the per-frame
  // processing flood during credits).
  if (reference[0] == 'C' && reference[1] == 'R' &&
      strncmp(reference, "CREDITS_", 8) == 0) {
    extern std::atomic<bool> g_creditsGuardActive;
    if (!g_creditsGuardActive.load(std::memory_order_relaxed)) {
      g_creditsGuardActive.store(true, std::memory_order_relaxed);
      LogToFile("[CREDITS-GUARD] ON — CREDITS_ key detected in SEH hook");
    }
    return original;
  }

  // Credits guard: allow SUBTITLE_* keys through for Korean dialogue display.
  // All other keys (CREDITS_*, HUD keys, etc.) get an early return to avoid
  // the processing overhead of the 300K+/frame credits string flood.
  // We do NOT call TextHook_SuspendHooksForCredits() from the subtitle-triggered
  // guard path so that subsequent dialogue subtitles still reach this hook.
  {
    extern std::atomic<bool> g_creditsGuardActive;
    if (g_creditsGuardActive.load(std::memory_order_relaxed)) {
      if (strncmp(reference, "SUBTITLE_", 9) != 0) {
        return original;
      }
    }
  }

  // =========================================================================
  // Countdown timer label suppression (2026-03-14 refactor):
  // Return a single space for CLOCKWORK_POWERDOWN label only.
  // This hides the English label text while the engine still renders the
  // timer number (1:02.6) normally — giving an exact, flicker-free timer.
  // The D3D11 overlay renders the Korean label ("전력 차단까지 ") via the
  // TimeScript render snapshot system.
  //
  // CLOCKWORK_EXFIL is NOT suppressed here — its cfg 0x24 doesn't resolve
  // in the config cache (slot reused by audio filenames), so the only way
  // IAR can detect "Exfil in " text is if SEH returns the real string.
  // CG_DrawHudElem alpha=0 handles English visual suppression instead.
  // =========================================================================
  if (reference[0] == 'C' && reference[1] == 'L') {
    if (strcmp(reference, "CLOCKWORK_POWERDOWN") == 0) {
      static const char kSpace[] = " ";
      return kSpace;
    }
  }

  // Ensure translations are loaded
  EnsureTranslationsLoaded();

  // Log localization key lookups for debugging (with categories)
  static int sehLogCount = 0;
  static std::set<std::string> loggedKeys; // Deduplicate logs

  if (loggedKeys.find(reference) == loggedKeys.end() && sehLogCount < 500) {
    loggedKeys.insert(reference);
    sehLogCount++;

    // Categorize the key for better debugging
    std::string category = "[OTHER]";
    std::string refStr(reference);
    if (refStr.find("SUBTITLE") != std::string::npos ||
        refStr.find("subtitle") != std::string::npos)
      category = "[SUBTITLE]";
    else if (refStr.find("MISSION") != std::string::npos ||
             refStr.find("mission") != std::string::npos)
      category = "[MISSION]";
    else if (refStr.find("LOADING") != std::string::npos ||
             refStr.find("loading") != std::string::npos)
      category = "[LOADING]";
    else if (refStr.find("MENU") != std::string::npos ||
             refStr.find("menu") != std::string::npos)
      category = "[MENU]";
    else if (refStr.find("BRIEFING") != std::string::npos ||
             refStr.find("briefing") != std::string::npos)
      category = "[BRIEFING]";
    else if (refStr.find("HUD") != std::string::npos)
      category = "[HUD]";
    else if (refStr.find("OBJECTIVE") != std::string::npos)
      category = "[OBJECTIVE]";

    // Reduced logging - only first 10 per category
    static int catLogCount = 0;
    if (catLogCount < 10) {
      catLogCount++;
      std::string logMsg = "[SEH] " + category + " Key=\"";
      logMsg += reference;
      logMsg += "\" Eng=\"";
      logMsg += (original ? Utf8Preview(std::string(original), 60) : "null");
      logMsg += "\"";
      LogToFile(logMsg);
    }
  }

  // Check if this is a SUBTITLE key (needs special handling - D3D11 direct
  // rendering) Also include VIDSUBTITLES_ for video subtitles!
  bool isSubtitleKey = (strncmp(reference, "SUBTITLE_", 9) == 0) ||
                       (strncmp(reference, "VIDSUBTITLES_", 13) == 0) ||
                       (strncmp(reference, "INTROSCREEN", 11) == 0);
  if (isSubtitleKey && strncmp(reference, "INTROSCREEN", 11) != 0) {
    TrackSubtitleSehCaller(reference);
  }

  // Check if we have a Korean translation for this KEY
  // Normalize to uppercase — game SLC may return keys with lowercase suffixes
  // (e.g. "...1021b" vs JSON's "...1021B")
  std::string refUpper = ToUpper(std::string(reference));
  auto itKor = g_KeyToKorean.find(refUpper);
  if (itKor != g_KeyToKorean.end()) {
    const std::string &koreanText = itKor->second;

    // Register English -> Korean mapping for R_AddCmdDrawText overlay
    if (original && strlen(original) > 0) {
      std::string engStr(original);
      std::string engUpper = ToUpper(StripColorCodes(engStr));

      // Thread-safe write to the overlay map
      {
        std::unique_lock<std::shared_mutex> lock(g_OverlayMapMutex);

        if (g_EngToKorOverlayMap.find(engUpper) == g_EngToKorOverlayMap.end()) {
          g_EngToKorOverlayMap[engUpper] = koreanText;

          // Also map without color codes
          std::string engClean = StripColorCodes(engStr);
          if (engClean != engStr) {
            g_EngToKorOverlayMap[ToUpper(engClean)] = koreanText;
          }

          // Store in EnglishToKey for FindTranslation compatibility
          g_EnglishToKey[engUpper] = refUpper;

          // Logging reduced - only log first 20 unique mappings
          static int mapLogCount = 0;
          if (mapLogCount < 20) {
            mapLogCount++;
            std::string msg = "[SEH] MAPPED: \"";
            msg += Utf8Preview(engStr, 60);
            msg += "\" -> \"";
            msg += Utf8Preview(koreanText, 50);
            msg += "\"";
            LogToFile(msg);
          }
        }
      }

      // Also handle long strings (prefixes for fragment matching)
      if (engStr.size() >= 30) {
        std::unique_lock<std::shared_mutex> lock(g_OverlayMapMutex);

        // Store multiple prefix lengths
        // Store multiple prefix lengths (Expanded for better matching)
        std::vector<size_t> prefixLengths = {20, 25, 30, 35, 40, 45, 50, 55,
                                             60, 65, 70, 75, 80, 85, 90, 95};
        for (size_t targetLen : prefixLengths) {
          if (targetLen >= engStr.size())
            continue;

          std::string prefix = engStr.substr(0, targetLen);
          std::string prefixUpper = ToUpper(prefix);

          if (g_EngToKorOverlayMap.find(prefixUpper) ==
              g_EngToKorOverlayMap.end()) {
            g_EngToKorOverlayMap[prefixUpper] = koreanText;
            g_PrefixToKey[prefixUpper] = std::string(reference);
          }

          // Also store suffix for suppression
          std::string suffix = engStr.substr(targetLen);
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

    // =========================================================================
    // SPLIT STRATEGY:
    // 1. SUBTITLE_* keys: Queue Korean for D3D11 direct rendering
    // 2. CGAME_*, PLATFORM_* (HUD keys): Also queue for overlay rendering
    // 3. Other keys (menus): Use R_AddCmdDrawText overlay (keep English,
    // overlay Korean)
    // =========================================================================

    // HUD keys that should also be rendered as subtitles/overlay
    // These appear during gameplay and need direct rendering
    bool isHudKey = (strncmp(reference, "CGAME_", 6) == 0) ||
                    (strncmp(reference, "PLATFORM_", 9) == 0) ||
                    (strncmp(reference, "OBJECTIVE_", 10) == 0) ||
                    (strncmp(reference, "SP_", 3) == 0);

    // SUBTITLE processing - queue Korean for D3D11 direct rendering
    if (isSubtitleKey) {
      g_LastSubtitleActivityTime.store(GetTickCount());
      std::string keyStr = reference ? std::string(reference) : "";
      std::string englishFull = original ? std::string(original) : "";
      std::string englishNorm = NormalizeEnglishKey(englishFull);
      std::string englishClean = StripColorCodes(englishFull);
      int totalChars = (int)englishClean.size();

      // Queue Korean translation for direct rendering
      {
        std::lock_guard<std::mutex> lock(g_InGameSubtitleMutex);
        g_InGameSubtitle.korean = koreanText;
        g_InGameSubtitle.timestamp = TextHook_GetSubtitleClockNow();

        if (!koreanText.empty()) {
          DWORD insertTime = g_InGameSubtitle.timestamp;
          PruneInGameSubtitleQueueLocked(insertTime);
          SubtitleRuntimeTiming runtimeTiming{};
          bool hasRuntimeTiming =
              TryGetSubtitleRuntimeTiming(keyStr, runtimeTiming);
          if (!hasRuntimeTiming && !englishNorm.empty()) {
            hasRuntimeTiming = TryGetSubtitleRuntimeTimingByText(
                englishNorm, runtimeTiming);
            if (hasRuntimeTiming) {
              static int textTimingLogCount = 0;
              if (textTimingLogCount < 120) {
                textTimingLogCount++;
                char tbuf[256];
                sprintf_s(tbuf,
                          "[SEH-TIMING-TEXT] key=%s msg=%d fade=%d arg6=%d",
                          keyStr.c_str(), runtimeTiming.msgTimeMs,
                          runtimeTiming.fadeOutMs, runtimeTiming.arg6);
                LogToFile(tbuf);
              }
            }
          }
          // Update or insert into subtitle queue
          auto it = std::find_if(g_InGameSubtitleQueue.begin(),
                                 g_InGameSubtitleQueue.end(),
                                 [&](const InGameSubtitleEntry &e) {
                                   if (!e.key.empty() && !keyStr.empty())
                                     return e.key == keyStr;
                                   return e.text == koreanText;
                                 });

          bool treatAsRefresh = false;
          DWORD gap = 0;
          if (it != g_InGameSubtitleQueue.end()) {
            gap = insertTime - it->timestamp;
            // Same key can be emitted as a new subtitle event.
            // Keep refresh semantics only for very short re-entries.
            treatAsRefresh = (gap <= 250);
          }

          if (it != g_InGameSubtitleQueue.end() && treatAsRefresh) {
            // Existing entry: refresh lastSeen time, keep firstSeen
            // DIAGNOSTIC: log SEH refresh pattern
            {
              DWORD totalAge = insertTime - it->firstSeen;
              static int refreshLogCount = 0;
              if (refreshLogCount < 100) {
                refreshLogCount++;
                char diagBuf[256];
                sprintf_s(diagBuf,
                          "[SEH-DIAG] REFRESH key=%s gap=%lums age=%lums",
                          keyStr.c_str(), (unsigned long)gap,
                          (unsigned long)totalAge);
                LogToFile(diagBuf);
              }
            }
            it->timestamp = insertTime; // acts as "lastRefresh"
            it->key = keyStr;
            it->english = englishFull;
            if (hasRuntimeTiming) {
              it->msgTimeMs = runtimeTiming.msgTimeMs;
              it->fadeOutMs = runtimeTiming.fadeOutMs;
              it->runtimeArg6 = runtimeTiming.arg6;
              it->runtimeTick = runtimeTiming.tick;
            }
          } else {
            if (it != g_InGameSubtitleQueue.end()) {
              static int retriggerLogCount = 0;
              if (retriggerLogCount < 100) {
                retriggerLogCount++;
                char rb[256];
                sprintf_s(
                    rb, "[SEH-DIAG] RETRIGGER key=%s gap=%lums -> new event",
                    keyStr.c_str(), (unsigned long)gap);
                LogToFile(rb);
              }
              g_InGameSubtitleQueue.erase(it);
            }

            // Slot-hold policy: keep old lines in their slot and only push up
            // by one slot on each new subtitle event.
            ShiftRenderSlotsForNewEntryLocked();

            // New entry: mark all existing entries as "pushed" for animation
            for (auto &existing : g_InGameSubtitleQueue) {
              // IMPORTANT: do not reset pushAnimStart for already-pushed lines.
              // Resetting this replays fade-out and makes old subtitles reappear.
              if (existing.pushAnimStart == 0) {
                existing.pushAnimStart = insertTime;
              } else {
                static int keepPushLogCount = 0;
                if (keepPushLogCount < 40) {
                  keepPushLogCount++;
                  char keepBuf[256];
                  sprintf_s(keepBuf,
                            "[SEH-DIAG] KEEP_PUSH key=%s prevPushAge=%lums",
                            existing.key.c_str(),
                            (unsigned long)(insertTime - existing.pushAnimStart));
                  LogToFile(keepBuf);
                }
              }
              existing.stackIndex = existing.renderSlot;
            }
            // DIAGNOSTIC: log new subtitle
            {
              static int newLogCount = 0;
              if (newLogCount < 100) {
                newLogCount++;
                char diagBuf[256];
                sprintf_s(diagBuf,
                    "[SEH-DIAG] NEW key=%s time=%lu queueSize=%zu",
                    keyStr.c_str(), (unsigned long)insertTime,
                    g_InGameSubtitleQueue.size());
                LogToFile(diagBuf);
              }
            }
            InGameSubtitleEntry newEntry;
            newEntry.text = koreanText;
            newEntry.timestamp = insertTime;
            DWORD firstSeenTick = insertTime;
            if (hasRuntimeTiming && runtimeTiming.tick > 0) {
              DWORD timingAge = insertTime - runtimeTiming.tick;
              if (timingAge <= 1000) {
                firstSeenTick = runtimeTiming.tick;
              }
            }
            newEntry.firstSeen = firstSeenTick; // first appearance time
            newEntry.key = keyStr;
            newEntry.english = englishFull;
            newEntry.stackIndex = 0;
            newEntry.pushAnimStart = 0;
            newEntry.renderSlot = 0;
            newEntry.msgTimeMs = hasRuntimeTiming ? runtimeTiming.msgTimeMs : 0;
            newEntry.fadeOutMs = hasRuntimeTiming ? runtimeTiming.fadeOutMs : 0;
            newEntry.runtimeArg6 = hasRuntimeTiming ? runtimeTiming.arg6 : 0;
            newEntry.runtimeTick = hasRuntimeTiming ? runtimeTiming.tick : 0;
            newEntry.nativeSlotPtr = 0;
            newEntry.nativeSlotValid = false;
            newEntry.nativeRemoveTick = 0;
            newEntry.nativeRemoveValid = false;
            newEntry.nativeRemoveReason = SUBREV_REMOVE_NONE;
            g_InGameSubtitleQueue.insert(
                g_InGameSubtitleQueue.begin(), newEntry);

            if (hasRuntimeTiming) {
              static int timingLogCount = 0;
              if (timingLogCount < 80) {
                timingLogCount++;
                char tbuf[256];
                sprintf_s(tbuf,
                          "[SEH-TIMING] key=%s msg=%d fade=%d arg6=%d age=%lums",
                          keyStr.c_str(), runtimeTiming.msgTimeMs,
                          runtimeTiming.fadeOutMs, runtimeTiming.arg6,
                          (unsigned long)(insertTime - runtimeTiming.tick));
                LogToFile(tbuf);
              }
            }
          }

          if (g_InGameSubtitleQueue.size() > MAX_SUBTITLE_QUEUE) {
            g_InGameSubtitleQueue.resize(MAX_SUBTITLE_QUEUE);
          }
        }
      }

      // Store last English subtitle for diagnostics
      if (original && strlen(original) > 0) {
        std::lock_guard<std::mutex> diagLock(g_LastSubtitleDiagMutex);
        g_LastSubtitleDiag.key = reference ? reference : "";
        g_LastSubtitleDiag.english = original;
        g_LastSubtitleDiag.timestamp = GetTickCount();
      }

      // Also store English->Korean mapping in SUBTITLE-ONLY map for CMD detour
      if (original && strlen(original) > 0 && !englishNorm.empty()) {
        SubtitleOnlyInfo info;
        info.key = keyStr;
        info.englishFull = englishClean;
        info.totalChars = (totalChars > 0) ? totalChars : (int)englishFull.size();

        std::unique_lock<std::shared_mutex> lock(g_SubtitleMapMutex);
        g_SubtitleOnlyMap[englishNorm] = info;
        if (!keyStr.empty()) {
          g_SubtitleByKey[keyStr] = info;
        }

        // Store a few prefixes for typewriter detection (avoid too-short noise)
        if (englishNorm.size() >= 20) {
          const size_t prefixLens[] = {12, 16, 20, 24, 28, 32};
          for (size_t len : prefixLens) {
            if (len >= englishNorm.size())
              continue;
            std::string prefix = englishNorm.substr(0, len);
            if (g_SubtitleOnlyMap.find(prefix) == g_SubtitleOnlyMap.end()) {
              g_SubtitleOnlyMap[prefix] = info;
            }
          }
        }
      }

      // Log subtitle queueing (reduced to avoid duplicate with MAPPED log)
      static int subLogCount = 0;
      if (subLogCount < 30) {
        subLogCount++;
        LogToFile("[SEH] SUBTITLE: key=\"" + std::string(reference) +
                  "\" korean=\"" + Utf8Preview(koreanText, 60) + "\"");
      }

      // Return original English subtitle so native engine renders it.
      // This allows visual comparison of timing/position with Korean overlay.
      // To suppress English, change this to: return "";
      return original;
    }

    // For menus/UI: Keep original English, overlay will handle Korean
    // (Logging disabled to reduce noise - menus work via overlay)
  } else {
    // =========================================================================
    // MISSING TRANSLATION COLLECTOR
    // Collect untranslated SUBTITLE keys for future translation work
    // =========================================================================
    if (isSubtitleKey && original && strlen(original) > 0) {
      std::string keyStr(reference);
      std::string engStr(original);

      // Add to missing keys set (thread-safe)
      {
        std::lock_guard<std::mutex> lock(g_MissingKeysMutex);
        if (g_MissingSubtitleKeys.find(keyStr) == g_MissingSubtitleKeys.end()) {
          g_MissingSubtitleKeys.insert(keyStr);

          static int missingLogCount = 0;
          if (missingLogCount < 50) {
            missingLogCount++;
            LogToFile("[MISSING] key=\"" + keyStr + "\" english=\"" +
                      Utf8Preview(engStr, 80) + "\"");
          }
        }
      }

      // Periodically save to file
      DWORD now = GetTickCount();
      if (now - g_LastMissingSaveTime > MISSING_SAVE_INTERVAL) {
        g_LastMissingSaveTime = now;
        SaveMissingSubtitles();
      }
    }
  }

  // SEH approach removed — engine caches SEH results at load time and does
  // not call SEH again per-frame from CG_DrawHudElem.  HUDHINT suppression
  // is handled by the SLC hook (x=9999) in TextHook.HookInstallAndDetours.inl.

  // Return original English for menus (R_AddCmdDrawText will overlay Korean)
  return original;
}
