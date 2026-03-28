
// ─── Phase 4: Refresh ObjHudHint entry with native params from HUD560 ────────
static void RefreshObjHudHintEntryNative(const std::string &key,
                                         const HudNativeEntry &native,
                                         const std::string *englishRuntime) {
  DWORD now = GetTickCount();
  const bool preferAddCmdLayout = IsHudRuntimeTailJoinPromptKey(key);
  const uint8_t nativeSourceToken = HintSourceTokenFromHudNativeEntry(native);
  auto SlotFromNative = [](float y, unsigned short virtualH) -> int {
    if (!std::isfinite(y)) {
      return 0;
    }
    if (virtualH > 0) {
      float ny = y / (float)virtualH;
      if (ny < 0.18f) return 1;
      if (ny < 0.42f) return 2;
      if (ny < 0.62f) return 3;
      if (ny < 0.78f) return 4;
      return 5;
    }
    if (y < 160.0f) return 1;
    if (y < 260.0f) return 2;
    if (y < 360.0f) return 3;
    if (y < 520.0f) return 4;
    return 5;
  };
  const int nativeSlot = SlotFromNative(native.y, native.virtualH);
  auto LogObjHintNativeDecision = [&](const char *action,
                                      const ObjHudHintEntry *existing,
                                      const char *reason, int entrySlot,
                                      DWORD deltaMs) {
    const DWORD incomingAge =
        (native.lastUpdate != 0 && now >= native.lastUpdate)
            ? (now - native.lastUpdate)
            : 0;
    const DWORD existingAge =
        (existing && existing->lastNativeSeenTick != 0 &&
         now >= existing->lastNativeSeenTick)
            ? (now - existing->lastNativeSeenTick)
            : 0;
    const int existingSlot =
        existing ? SlotFromNative(existing->nativeY, existing->nativeVirtualH) : 0;
    const unsigned int existingCaller = existing ? existing->callerOffset : 0u;
    const unsigned int existingSource = existing ? existing->nativeSource : 0u;
    const float existingX = existing ? existing->nativeX : 0.0f;
    const float existingY = existing ? existing->nativeY : 0.0f;
    const float incomingAlpha =
        (std::isfinite(native.color[3])) ? native.color[3] : 0.0f;
    const float existingAlpha =
        (existing && std::isfinite(existing->color[3])) ? existing->color[3] : 0.0f;
    const float existingScaleX = existing ? existing->nativeXScale : 0.0f;
    const float existingScaleY = existing ? existing->nativeYScale : 0.0f;
    const float existingFontH = existing ? existing->fontHeight : 0.0f;

    // Unified hash-based log throttle — single mutex, no string allocation.
    {
      static std::mutex s_objHintLogMtx;
      static std::unordered_map<uint64_t, DWORD> s_objHintLogTick;
      uint64_t h = 1469598103934665603ull;
      auto mix = [&](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
      };
      for (unsigned char c : key) {
        mix((uint64_t)c);
      }
      if (action) {
        for (const char *p = action; *p; ++p) {
          mix((uint64_t)(unsigned char)(*p));
        }
      }
      mix((uint64_t)(unsigned int)native.callerOffset);
      mix((uint64_t)(unsigned int)(nativeSlot & 0xFF));
      mix((uint64_t)(unsigned int)(entrySlot & 0xFF));
      const bool isDrop = (action && strcmp(action, "DROP") == 0);
      const DWORD minIntervalMs = isDrop ? 2000 : 900;
      // Separate hash lanes for verbose vs diag to keep both log prefixes.
      const uint64_t hVerbose = h ^ 0xA5A5A5A5A5A5A5A5ull;
      bool shouldLogVerbose = false;
      bool shouldLogDiag = false;
      {
        std::lock_guard<std::mutex> lk(s_objHintLogMtx);
        if (kVerboseRuntimeLogs) {
          auto it = s_objHintLogTick.find(hVerbose);
          if (it == s_objHintLogTick.end() ||
              (now - it->second) > minIntervalMs) {
            s_objHintLogTick[hVerbose] = now;
            shouldLogVerbose = true;
          }
        }
        {
          auto it = s_objHintLogTick.find(h);
          if (it == s_objHintLogTick.end() ||
              (now - it->second) > minIntervalMs) {
            s_objHintLogTick[h] = now;
            shouldLogDiag = true;
          }
        }
        if (s_objHintLogTick.size() > 640) {
          for (auto jt = s_objHintLogTick.begin();
               jt != s_objHintLogTick.end();) {
            if ((now - jt->second) > 30000) {
              jt = s_objHintLogTick.erase(jt);
            } else {
              ++jt;
            }
          }
        }
      }
      if (shouldLogVerbose) {
        char dbuf[1024];
        sprintf_s(
            dbuf,
            "[NOHH-NATIVE-%s] key=%s reason=%s inCaller=+0x%X inSlot=%d inSrc=%d inXY=(%.1f,%.1f) inAge=%lums curCaller=+0x%X curSlot=%d curSrc=%u curXY=(%.1f,%.1f) curAge=%lums dt=%lums",
            action ? action : "?", key.c_str(), reason ? reason : "?",
            (unsigned int)native.callerOffset, nativeSlot, (int)native.source, native.x,
            native.y, (unsigned long)incomingAge, existingCaller, existingSlot,
            existingSource, existingX, existingY, (unsigned long)existingAge,
            (unsigned long)deltaMs);
        LogToFile(dbuf);
      }
      if (shouldLogDiag) {
        char dbuf[1200];
        sprintf_s(
            dbuf,
            "[OBJHUDHINT-NATIVE-%s] key=%s reason=%s inCaller=+0x%X inSrc=%d inSlot=%d inXY=(%.1f,%.1f) inScale=(%.3f,%.3f) inH=%.1f inA=%.2f inAge=%lums curCaller=+0x%X curSrc=%u curSlot=%d curXY=(%.1f,%.1f) curScale=(%.3f,%.3f) curH=%.1f curA=%.2f curAge=%lums dt=%lums",
            action ? action : "?", key.c_str(), reason ? reason : "?",
            (unsigned int)native.callerOffset, (int)native.source, nativeSlot,
            native.x, native.y, native.xScale, native.yScale, native.fontHeight,
            incomingAlpha, (unsigned long)incomingAge, existingCaller,
            existingSource, existingSlot, existingX, existingY, existingScaleX,
            existingScaleY, existingFontH, existingAlpha,
            (unsigned long)existingAge, (unsigned long)deltaMs);
        LogToFile(dbuf);
      }
    }
  };
  bool refreshedExisting = false;
  {
    std::lock_guard<std::mutex> lock(g_ObjHudHintMutex);
    for (auto &e : g_ObjHudHintEntries) {
      if (e.key == key) {
        const int entrySlot = SlotFromNative(e.nativeY, e.nativeVirtualH);
        const DWORD prevSeen = e.lastSeen;
        const bool existingHasVirtual =
            (e.nativeVirtualW != 0 || e.nativeVirtualH != 0);
        const bool incomingHasVirtual =
            (native.virtualW != 0 || native.virtualH != 0);
        const bool incomingFromAddCmd =
            (native.source == HUD_NATIVE_ADDCMD);
        const bool existingFromAddCmd =
            (e.nativeSource == (unsigned char)HUD_NATIVE_ADDCMD);
        const bool incomingFromHud560 =
            (native.source == HUD_NATIVE_RENDER);
        const bool keepExistingNative =
            preferAddCmdLayout
                ? (existingFromAddCmd && incomingHasVirtual &&
                   incomingFromHud560 && (now - prevSeen) <= 450)
                : (existingHasVirtual && !incomingHasVirtual &&
                   incomingFromAddCmd && (now - prevSeen) <= 120);
        if (keepExistingNative) {
          e.lastSeen = now;
          e.lastNativeSeenTick = now;
          const char *keepReason =
              preferAddCmdLayout ? "prefer_addcmd_layout"
                                 : "prefer_existing_virtual";
          const DWORD keepDelta = (now >= prevSeen) ? (now - prevSeen) : 0;
          LogObjHintNativeDecision("KEEP", &e, keepReason, entrySlot,
                                   keepDelta);
          if (englishRuntime && !englishRuntime->empty()) {
            std::string trimmed = TrimSpaces(*englishRuntime);
            if (!trimmed.empty())
              e.english = trimmed;
            e.lastEnglishSeenTick = now;
          }
          refreshedExisting = true;
          break;
        }
        if (e.hasNativeParams && nativeSlot != 0 && entrySlot != 0 &&
            entrySlot != nativeSlot) {
          LogObjHintNativeDecision("DROP", &e, "slot_mismatch", entrySlot, 0);
          continue;
        }
        if (e.callerOffset != 0 && native.callerOffset != 0 &&
            e.callerOffset != native.callerOffset) {
          LogObjHintNativeDecision("DROP", &e, "caller_mismatch", entrySlot, 0);
          continue;
        }
        e.lastSeen = now;
        e.lastNativeSeenTick = now;
        e.hasNativeParams = true;
        e.nativeX = native.x;
        e.nativeY = native.y;
        e.nativeXScale = native.xScale;
        e.nativeYScale = native.yScale;
        e.fontHeight = native.fontHeight;
        e.nativeSource = (unsigned char)native.source;
        e.nativeSourceToken = nativeSourceToken;
        e.nativeVirtualW = native.virtualW;
        e.nativeVirtualH = native.virtualH;
        e.callerOffset = native.callerOffset;
        auto clamp01 = [](float v, float fallback) -> float {
          if (!std::isfinite(v)) {
            return fallback;
          }
          if (v < 0.0f)
            return 0.0f;
          if (v > 1.0f)
            return 1.0f;
          return v;
        };
        e.color[0] = clamp01(native.color[0], e.color[0]);
        e.color[1] = clamp01(native.color[1], e.color[1]);
        e.color[2] = clamp01(native.color[2], e.color[2]);
        e.color[3] = clamp01(native.color[3], e.color[3]);
        if (englishRuntime && !englishRuntime->empty()) {
          std::string trimmed = TrimSpaces(*englishRuntime);
          if (!trimmed.empty())
            e.english = trimmed;
          e.lastEnglishSeenTick = now;
        }
        {
          const DWORD applyDelta = (now >= prevSeen) ? (now - prevSeen) : 0;
          LogObjHintNativeDecision("APPLY", &e, "native_refresh", entrySlot,
                                   applyDelta);
        }
        refreshedExisting = true;
        break;
      }
    }
    if (!refreshedExisting) {
      // Entry not yet created -- create from HUD560 data
      auto itKor = g_KeyToKorean.find(key);
      if (itKor == g_KeyToKorean.end() || itKor->second.empty())
        return;

      if (g_ObjHudHintEntries.size() >= OBJHUDHINT_ENTRY_MAX) {
        auto oldest = std::min_element(
            g_ObjHudHintEntries.begin(), g_ObjHudHintEntries.end(),
            [](const ObjHudHintEntry &a, const ObjHudHintEntry &b) {
              return a.lastSeen < b.lastSeen;
            });
        if (oldest != g_ObjHudHintEntries.end())
          g_ObjHudHintEntries.erase(oldest);
      }

      ObjHudHintEntry oEntry{};
      oEntry.key = key;
      oEntry.korean = itKor->second;
      oEntry.english = (englishRuntime && !englishRuntime->empty())
                           ? TrimSpaces(*englishRuntime)
                           : key;
      oEntry.nativeX = native.x;
      oEntry.nativeY = native.y;
      oEntry.nativeXScale = native.xScale;
      oEntry.nativeYScale = native.yScale;
      oEntry.fontHeight = native.fontHeight;
      oEntry.color[0] = native.color[0];
      oEntry.color[1] = native.color[1];
      oEntry.color[2] = native.color[2];
      oEntry.color[3] = native.color[3];
      oEntry.firstSeen = now;
      oEntry.lastSeen = now;
      oEntry.hasNativeParams = true;
      oEntry.nativeSource = (unsigned char)native.source;
      oEntry.nativeSourceToken = nativeSourceToken;
      oEntry.nativeVirtualW = native.virtualW;
      oEntry.nativeVirtualH = native.virtualH;
      oEntry.callerOffset = native.callerOffset;
      oEntry.lastNativeSeenTick = now;
      oEntry.lastEnglishSeenTick = oEntry.english.empty() ? 0 : now;
      oEntry.lastRenderedTick = 0;
      g_ObjHudHintEntries.push_back(oEntry);
      LogObjHintNativeDecision("CREATE", nullptr, "new_entry", nativeSlot, 0);
    }
  }
}
