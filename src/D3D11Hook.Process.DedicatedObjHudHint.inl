    // Suppress objhudhint rendering for 2 seconds after restart (same as hint).
    const unsigned long objhhResetTick = TextHook_GetObjectiveRuntimeResetTick();
    const bool objhhRestartSuppressed =
        (objhhResetTick != 0 && now >= objhhResetTick &&
         (now - objhhResetTick) < 2000);
    const bool objhhPostVideoSuppressed =
        (g_PostVideoHintSuppressUntil != 0 && now < g_PostVideoHintSuppressUntil);

    const bool objHudHintOverlayVisible =
        (!objHudHintEntries.empty() && !suppressHudOverlay && !gate.pauseMenu &&
         !gate.warmupActive && !gate.creditsActive &&
         overlaySessionState != OVERLAY_SESSION_WARMUP &&
         !objhhRestartSuppressed && !objhhPostVideoSuppressed);
    if (objHudHintOverlayVisible) {
      constexpr float kHintHeight720 = 64.0f;
      constexpr float kBaselineFromTop720 = 43.0f;
      if (g_FrameSwapChainDescValid) {
        const DXGI_SWAP_CHAIN_DESC &ohhScDesc = g_FrameSwapChainDesc;
        float screenWidth = (float)ohhScDesc.BufferDesc.Width;
        float screenHeight = (float)ohhScDesc.BufferDesc.Height;
        float scaleY = g_ActiveArea.height / 720.0f;
        const DWORD kObjHudHintFreshWindowMs = 120;
        const DWORD kObjHudHintNativeBackedWindowMs = 120;
        const DWORD kObjHudHintEvidenceMinMs = 80;
        const DWORD kObjHudHintRenderGraceMs = 0;
        const DWORD kObjHudHintGateRetainMs = 1200;
        struct ObjHudHintGateState {
          uint64_t lastSeenFrame = 0;
          DWORD lastSeenTick = 0;
          DWORD firstSeenTick = 0;
          DWORD lastRenderTick = 0;
          unsigned int consecutiveFrames = 0;
          unsigned int evidenceFrames = 0;
          bool confirmed = false;
        };
        static std::unordered_map<std::string, ObjHudHintGateState>
            s_objHudHintGateStates;
        static DWORD s_objHudHintGatePruneTick = 0;
        struct ObjCallerSwitchGate {
          DWORD lastTick = 0;
          std::string semantic;
        };
        static std::unordered_map<unsigned int, ObjCallerSwitchGate>
            s_objCallerSwitchGate;
        static DWORD s_objCallerSwitchPruneTick = 0;
        auto objHintSourceToken = [](const ObjHudHintEntry &oe) -> uint8_t {
          if (oe.nativeSourceToken != 0) {
            return oe.nativeSourceToken;
          }
          if (oe.nativeSource == (unsigned char)HUD_NATIVE_ADDCMD) {
            return NATIVE_HUD_SOURCE_ADDCMD;
          }
          if (oe.nativeSource == (unsigned char)HUD_NATIVE_RENDER) {
            return NATIVE_HUD_SOURCE_HUD560;
          }
          return NATIVE_HUD_SOURCE_BRIDGE;
        };
        auto isObjHudHintFamilyToken = [](uint8_t token) -> bool {
          return token == NATIVE_HUD_SOURCE_HUD560 ||
                 token == NATIVE_HUD_SOURCE_ADDCMD;
        };
        auto overlaySourceFromNative = [](unsigned char nativeSource) -> OverlaySource {
          if (nativeSource == (unsigned char)HUD_NATIVE_RENDER) {
            return OVERLAY_SOURCE_HUD560;
          }
          if (nativeSource == (unsigned char)HUD_NATIVE_ADDCMD) {
            return OVERLAY_SOURCE_ADDCMD;
          }
          return OVERLAY_SOURCE_SLC;
        };
        auto overlaySourceFromToken = [&](uint8_t sourceToken,
                                          unsigned char nativeSource) -> OverlaySource {
          if (sourceToken == NATIVE_HUD_SOURCE_HUD560) {
            return OVERLAY_SOURCE_HUD560;
          }
          if (sourceToken == NATIVE_HUD_SOURCE_ADDCMD) {
            return OVERLAY_SOURCE_ADDCMD;
          }
          if (sourceToken == NATIVE_HUD_SOURCE_HUDELEM ||
              sourceToken == NATIVE_HUD_SOURCE_BRIDGE) {
            return OVERLAY_SOURCE_SLC;
          }
          return overlaySourceFromNative(nativeSource);
        };
        auto slotFromNative = [](float y, unsigned short virtualH) -> int {
          if (!std::isfinite(y)) {
            return 0;
          }
          if (virtualH == 0 && std::fabs(y) <= 0.5f) {
            return 0;
          }
          if (virtualH > 0) {
            const float ny = y / (float)virtualH;
            if (ny < 0.18f)
              return 1;
            if (ny < 0.42f)
              return 2;
            if (ny < 0.62f)
              return 3;
            if (ny < 0.78f)
              return 4;
            return 5;
          }
          if (y < 160.0f)
            return 1;
          if (y < 260.0f)
            return 2;
          if (y < 360.0f)
            return 3;
          if (y < 520.0f)
            return 4;
          return 5;
        };
        auto consumerFromNative = [&](unsigned char nativeSource,
                                      uint8_t sourceToken,
                                      unsigned int callerOffset, int slot,
                                      unsigned short virtualW,
                                      unsigned short virtualH) -> uint64_t {
          return TextHook_MakeHudConsumerId(
              overlaySourceFromToken(sourceToken, nativeSource), callerOffset,
              slot, virtualW, virtualH);
        };
        auto buildInstanceSig = [](const std::string &key, uint64_t consumer,
                                   int slot) -> std::string {
          char sigBuf[256];
          snprintf(sigBuf, sizeof(sigBuf), "%s|%llu|%d",
                   key.c_str(), (unsigned long long)consumer, slot);
          return std::string(sigBuf);
        };
        static std::unordered_map<std::string, DWORD> s_objFamilySkipLogTick;
        static std::unordered_map<std::string, DWORD> s_objTokenBackfillLogTick;
        auto isForcedCenterStatusKey = [](const std::string &key) -> bool {
          return key.find("_FAIL") != std::string::npos ||
                 key.find("FAIL_") != std::string::npos ||
                 key.find("_DEATH") != std::string::npos ||
                 key.find("_DROWN") != std::string::npos ||
                 key.find("_SUICIDE") != std::string::npos ||
                 key.find("_QUOTE_") != std::string::npos ||
                 key.find("_KILLED") != std::string::npos ||
                 key.find("GOT_AWAY") != std::string::npos;
        };
        auto logObjFamilySkip = [&](const ObjHudHintEntry &oe, uint8_t token,
                                    int slot) {
          char sigBuf[256];
          snprintf(sigBuf, sizeof(sigBuf), "%s|%u|%u|%d",
                   oe.key.c_str(), (unsigned int)oe.callerOffset,
                   (unsigned int)token, slot);
          const std::string sig(sigBuf);
          auto it = s_objFamilySkipLogTick.find(sig);
          if (it != s_objFamilySkipLogTick.end() &&
              (now - it->second) <= 1200) {
            return;
          }
          s_objFamilySkipLogTick[sig] = now;
          char sbuf[320];
          sprintf_s(
              sbuf,
              "[OBJHUDHINT-FAMILY-SKIP] key=%s caller=+0x%X src=%u tok=%u slot=%d",
              oe.key.c_str(), oe.callerOffset, (unsigned int)oe.nativeSource,
              (unsigned int)token, slot);
          LogToFile(sbuf);
          if (s_objFamilySkipLogTick.size() > 384) {
            for (auto it2 = s_objFamilySkipLogTick.begin();
                 it2 != s_objFamilySkipLogTick.end();) {
              if ((now - it2->second) > 30000) {
                it2 = s_objFamilySkipLogTick.erase(it2);
              } else {
                ++it2;
              }
            }
          }
        };
        auto logObjTokenBackfill = [&](const ObjHudHintEntry &oe,
                                       uint8_t token) {
          char sigBuf2[256];
          snprintf(sigBuf2, sizeof(sigBuf2), "%s|%u|%u",
                   oe.key.c_str(), (unsigned int)oe.callerOffset,
                   (unsigned int)oe.nativeSource);
          const std::string sig(sigBuf2);
          auto it = s_objTokenBackfillLogTick.find(sig);
          if (it != s_objTokenBackfillLogTick.end() &&
              (now - it->second) <= 2500) {
            return;
          }
          s_objTokenBackfillLogTick[sig] = now;
          char tbuf[320];
          sprintf_s(tbuf,
                    "[OBJHUDHINT-TOKEN-BACKFILL] key=%s caller=+0x%X src=%u tok=%u",
                    oe.key.c_str(), oe.callerOffset,
                    (unsigned int)oe.nativeSource, (unsigned int)token);
          LogToFile(tbuf);
          if (s_objTokenBackfillLogTick.size() > 384) {
            for (auto it2 = s_objTokenBackfillLogTick.begin();
                 it2 != s_objTokenBackfillLogTick.end();) {
              if ((now - it2->second) > 30000) {
                it2 = s_objTokenBackfillLogTick.erase(it2);
              } else {
                ++it2;
              }
            }
          }
        };
        // Weapon-swap/pickup prompts need a much tighter lifecycle window
        // so the Korean overlay tracks the native prompt exactly.
        const DWORD kObjHudHintTailJoinFreshMs = 50;
        auto isObjRenderable = [&](const ObjHudHintEntry &oe,
                                   const ObjHudHintGateState &gateState,
                                   DWORD freshMs,
                                   DWORD nativeMs) -> bool {
          if (gate.warmupActive || overlaySessionState == OVERLAY_SESSION_WARMUP) {
            return false;
          }
          const DWORD ageSinceSeen = now - oe.lastSeen;
          if (ageSinceSeen <= freshMs) {
            return true;
          }
          if (oe.lastNativeSeenTick == 0 || now < oe.lastNativeSeenTick) {
            return false;
          }
          const DWORD ageSinceNative = now - oe.lastNativeSeenTick;
          if (ageSinceNative <= nativeMs) {
            return true;
          }
          if (gateState.lastRenderTick != 0 &&
              (now - gateState.lastRenderTick) <= kObjHudHintRenderGraceMs &&
              ageSinceNative <= nativeMs) {
            return true;
          }
          return false;
        };
        std::unordered_set<std::string> renderedObjHudHintSignatures;
        renderedObjHudHintSignatures.reserve(objHudHintEntries.size() * 2 + 8);
        static std::unordered_map<std::string, DWORD> s_objHintRenderLogTick;
        static std::unordered_map<std::string, DWORD> s_objHintDiagRenderLogTick;

        // Key-level dedup: when the same key has multiple OBJHUDHINT
        // entries (e.g. weapon prompts with both HUD560 bookkeeping and
        // ADDCMD visible entries), render only the highest-priority one.
        std::unordered_map<std::string, size_t> objBestIndexByKey;
        objBestIndexByKey.reserve(objHudHintEntries.size());
        for (size_t di = 0; di < objHudHintEntries.size(); ++di) {
          const auto &de = objHudHintEntries[di];
          if (de.key.empty() || !de.hasNativeParams) continue;
          auto dit = objBestIndexByKey.find(de.key);
          if (dit == objBestIndexByKey.end()) {
            objBestIndexByKey[de.key] = di;
          } else {
            const auto &prev = objHudHintEntries[dit->second];
            // Prefer higher sourceToken (ADDCMD=3 > HUD560=1)
            if (de.nativeSourceToken > prev.nativeSourceToken) {
              dit->second = di;
            }
          }
        }

        for (const auto &oe : objHudHintEntries) {
          const std::string &key = oe.key;
          if (key.empty()) continue;
          if (!oe.hasNativeParams) continue;
          // Skip if this entry lost the key-level dedup
          {
            const size_t oeIdx = (size_t)(&oe - objHudHintEntries.data());
            auto bestIt = objBestIndexByKey.find(key);
            if (bestIt != objBestIndexByKey.end() &&
                bestIt->second != oeIdx) {
              continue;
            }
          }
          const uint8_t sourceToken = objHintSourceToken(oe);
          const int entrySlot = slotFromNative(oe.nativeY, oe.nativeVirtualH);
          if (oe.nativeSourceToken == 0) {
            logObjTokenBackfill(oe, sourceToken);
          }
          if (!isObjHudHintFamilyToken(sourceToken)) {
            logObjFamilySkip(oe, sourceToken, entrySlot);
            continue;
          }
          const bool fastTailJoinKey = IsHudRuntimeTailJoinKey(key);
          const bool isDirectPromptKey =
              fastTailJoinKey ||
              key == "SCRIPT_MANTLE" ||
              key == "PLATFORM_MANTLE" ||
              key == "PLATFORM_MANTLEGAMEPAD" ||
              key == "DEER_HUNT_MATV_HINT" ||
              key == "DEER_HUNT_MATV_HINT_RELOAD";
          // Require stable re-observation per instance before rendering:
          // key + consumer + slot must survive 3 consecutive frames,
          // or persist for at least 80ms with repeated evidence.
          const int instanceSlot = entrySlot;
          const uint64_t instanceConsumer = consumerFromNative(
              oe.nativeSource, sourceToken, oe.callerOffset, instanceSlot,
              oe.nativeVirtualW, oe.nativeVirtualH);
          const std::string instanceSig =
              buildInstanceSig(key, instanceConsumer, instanceSlot);
          ObjHudHintGateState &gateState = s_objHudHintGateStates[instanceSig];
          if (gateState.lastSeenFrame != 0 &&
              (gateState.lastSeenFrame + 1) < overlayFrameId) {
            gateState.confirmed = false;
            gateState.consecutiveFrames = 0;
            gateState.evidenceFrames = 0;
            gateState.firstSeenTick = 0;
          }
          const bool seenInPrevFrame =
              (gateState.lastSeenFrame != 0 &&
               (gateState.lastSeenFrame + 1) == overlayFrameId);
          if (seenInPrevFrame) {
            gateState.consecutiveFrames += 1;
            gateState.evidenceFrames += 1;
          } else {
            gateState.consecutiveFrames = 1;
            gateState.evidenceFrames = 1;
            gateState.firstSeenTick = now;
          }
          if (gateState.firstSeenTick == 0) {
            gateState.firstSeenTick = now;
          }
          gateState.lastSeenFrame = overlayFrameId;
          gateState.lastSeenTick = now;
          const bool enoughFrameCount = (gateState.consecutiveFrames >= 3);
          const bool enoughEvidenceMs =
              (gateState.firstSeenTick != 0 &&
               (now - gateState.firstSeenTick) >= kObjHudHintEvidenceMinMs &&
               gateState.evidenceFrames >= 2);
          if (fastTailJoinKey || enoughFrameCount || enoughEvidenceMs) {
            gateState.confirmed = true;
          }
          if (!gateState.confirmed) {
            continue;
          }
          const bool objRenderableResult = isObjRenderable(oe, gateState,
                  fastTailJoinKey ? kObjHudHintTailJoinFreshMs
                                  : kObjHudHintFreshWindowMs,
                  fastTailJoinKey ? kObjHudHintTailJoinFreshMs
                                  : kObjHudHintNativeBackedWindowMs);
          if (!objRenderableResult) {
            continue;
          }

          // Minimum age filter: prevent transient load-time flash artifacts
          if (!fastTailJoinKey &&
              oe.firstSeen != 0 && (now - oe.firstSeen) < 20) continue;

          // One-shot flash filter
          if (!fastTailJoinKey && oe.firstSeen == oe.lastSeen) {
            if ((now - oe.firstSeen) < 8) continue;
          }

          // === Position ======================================================
          float x = 0.0f, y = 0.0f;
          float fontHeight = 88.0f;
          float scale = 1.0f;
          bool customPosIsCenterAnchor = false;
          const bool forcedCenterStatus = isForcedCenterStatusKey(key);

          float baseColor[4] = {1.0f, 1.0f, 1.0f, 0.6f};

          auto nativeCoordToScreen = [](float value, unsigned short virtualSpan,
                                        float screenSpan,
                                        float fallback) -> float {
            if (!std::isfinite(value)) {
              return fallback;
            }
            if (virtualSpan > 0) {
              return (value / (float)virtualSpan) * screenSpan;
            }
            return value;
          };
          const bool hasUsableNativeY =
              std::isfinite(oe.nativeY) &&
              ((oe.nativeVirtualH > 0 &&
                oe.nativeY >= 0.0f &&
                oe.nativeY <= ((float)oe.nativeVirtualH + 64.0f)) ||
               (oe.nativeVirtualH == 0 && oe.nativeY > 0.5f));
          const float nativeScreenY =
              g_ActiveArea.offsetY +
              nativeCoordToScreen(oe.nativeY, oe.nativeVirtualH,
                                  g_ActiveArea.height,
                                  g_ActiveArea.height * 0.5f);

          // OBJHUDHINT keeps center-anchor X, but Y must preserve the native/LUI
          // semantic position of the prompt/status message.
          float luiX = 0.0f, luiY = 0.0f;
          int luiSlot = LUI_HINT_SLOT_NONE;
          bool hasLuiPos =
              GetLuiHintPosition(key, screenWidth, screenHeight, luiX, luiY,
                                 &luiSlot);

          x = screenWidth * 0.5f;
          customPosIsCenterAnchor = true;
          if (hasUsableNativeY) {
            y = nativeScreenY;
          } else if (hasLuiPos) {
            y = luiY;
          } else if (forcedCenterStatus) {
            y = g_ActiveArea.offsetY + g_ActiveArea.height * 0.5f;
          } else {
            // Default: cursorHint baseline position (bottom=-160, center)
            float safeBot = g_ActiveArea.offsetY + g_ActiveArea.height;
            y = safeBot - (160.0f + (kHintHeight720 - kBaselineFromTop720)) * scaleY;
          }

          // === Scale (fixed, matching LUI hint size) ==========================
          float baseFontH = 60.0f * scaleY;
          fontHeight = baseFontH;
          scale = 1.0f;

          // === Alpha (maximum brightness, instant) ============================
          baseColor[3] = 1.0f;

          float finalAlpha = Clamp01(baseColor[3]);
          if (finalAlpha <= 0.01f) continue;

          float color[4] = {baseColor[0], baseColor[1], baseColor[2], finalAlpha};

          // === Text preparation ===============================================
          const bool isTailJoinKey = IsHudRuntimeTailJoinKey(key);
          std::string displayText;
          bool compactTailJoin = false;
          size_t compactTailSplit = std::string::npos;
          float compactTailGap = 0.0f;
          std::string displaySig;
          std::string displaySemanticSig;
          std::string englishSemanticSig;

          if (isTailJoinKey) {
            // ── Cached fast path for weapon swap/pickup prompts ──────────
            // Avoids per-frame ResolveBindingsInText (dominant CPU cost).
            // Tier 1 (base): re-resolved only when oe.korean/oe.english
            //   change or every 2 s as safety net for runtime-English drift.
            // Tier 2 (display+tail): recomputed only when tail text changes.
            struct OhhSwapCached {
              std::string inKorean;
              std::string inEnglish;
              std::string baseResolved;
              std::string engFallback;
              std::string engSemanticSig;
              DWORD baseTick = 0;
              std::string inTail;
              std::string finalDisplay;
              bool finalCompactJoin = false;
              size_t finalCompactSplit = std::string::npos;
              std::string finalDisplaySig;
              std::string finalSemanticSig;
              DWORD tick = 0;
            };
            static std::unordered_map<std::string, OhhSwapCached>
                s_ohhSwapCache;
            static DWORD s_ohhSwapCachePruneTick = 0;

            char ckBuf[256];
            snprintf(ckBuf, sizeof(ckBuf), "%s|%u",
                     key.c_str(), (unsigned int)oe.callerOffset);
            const std::string cacheKey(ckBuf);
            OhhSwapCached &sc = s_ohhSwapCache[cacheKey];

            // ── Tier 1: base resolution (expensive, rarely needed) ───────
            if (sc.baseTick == 0 ||
                sc.inKorean != oe.korean || sc.inEnglish != oe.english ||
                (now - sc.baseTick) > 2000) {
              std::string cleanText = StripColorCodesSimple(oe.korean);
              sc.engFallback = oe.english;
              if (!sc.engFallback.empty())
                sc.engFallback = StripColorCodesSimple(sc.engFallback);
              const bool needsRtEng =
                  sc.engFallback.empty() || sc.engFallback == key ||
                  sc.engFallback.find("&&") != std::string::npos ||
                  sc.engFallback.find("$$") != std::string::npos ||
                  sc.engFallback.find('{') != std::string::npos ||
                  sc.engFallback.find("[{+") != std::string::npos;
              if (needsRtEng) {
                std::string rtEng;
                if (TextHook_GetRuntimeEnglishForKeyRecent(
                        key, rtEng, 1800)) {
                  rtEng = StripColorCodesSimple(rtEng);
                  rtEng = TrimAscii(rtEng);
                  if (!rtEng.empty()) sc.engFallback = rtEng;
                }
              }
              sc.baseResolved =
                  BindingResolver::ResolveBindingsInText(
                      oe.korean, sc.engFallback);
              TextHook_ResolveLocalizedTemplateArgs(
                  key, sc.engFallback, sc.baseResolved);
              if (!sc.engFallback.empty()) {
                std::string engDisp =
                    BindingResolver::ResolveBindingsInText(
                        sc.engFallback, sc.engFallback);
                sc.engSemanticSig = BuildPromptSemanticSig(engDisp);
              } else {
                sc.engSemanticSig.clear();
              }
              sc.inKorean = oe.korean;
              sc.inEnglish = oe.english;
              sc.baseTick = now;
              sc.inTail = "\x01"; // sentinel: guarantees sc.inTail != tailClean
                                  // so Tier 2 always runs after a base refresh
            }

            // ── Tail fetch (cheap — 4 ms cache in GetHudRuntimeTail) ────
            struct TailStickyEntry {
              std::string text;
              DWORD tick = 0;
            };
            static std::unordered_map<std::string, TailStickyEntry>
                s_ohhTailSticky;
            static DWORD s_ohhTailPruneTick = 0;
            const DWORD kTailStickyMs = 500;

            std::string tailClean;
            {
              const float tailQueryX = customPosIsCenterAnchor
                  ? (x - hudText.MeasureTextWidthEx(
                             sc.baseResolved, fontHeight, scale) *
                         0.5f)
                  : x;
              std::string runtimeTail;
              bool haveTail = TextHook_GetHudRuntimeTail(
                  key, tailQueryX, y, oe.callerOffset, runtimeTail);
              if (!haveTail) {
                auto itSticky = s_ohhTailSticky.find(cacheKey);
                if (itSticky != s_ohhTailSticky.end() &&
                    (now - itSticky->second.tick) <= kTailStickyMs) {
                  runtimeTail = itSticky->second.text;
                  haveTail = !runtimeTail.empty();
                }
              }
              if (haveTail) {
                tailClean =
                    TrimAscii(StripColorCodesSimple(runtimeTail));
                if (!tailClean.empty())
                  s_ohhTailSticky[cacheKey] = {tailClean, now};
              }
              if ((now - s_ohhTailPruneTick) > 2000) {
                s_ohhTailPruneTick = now;
                for (auto it = s_ohhTailSticky.begin();
                     it != s_ohhTailSticky.end();) {
                  if ((now - it->second.tick) > 30000)
                    it = s_ohhTailSticky.erase(it);
                  else ++it;
                }
              }
            }

            // ── Tier 2: final display with tail (cheap on tail change) ──
            if (sc.inTail != tailClean) {
              sc.finalDisplay = sc.baseResolved;
              sc.finalCompactJoin = false;
              sc.finalCompactSplit = std::string::npos;
              if (!tailClean.empty()) {
                std::string baseClean = TrimAscii(sc.baseResolved);
                if (!baseClean.empty() &&
                    baseClean.find(tailClean) == std::string::npos) {
                  const bool cj =
                      !baseClean.empty() && baseClean.back() == ':';
                  sc.finalDisplay =
                      baseClean + (cj ? "" : " ") + tailClean;
                  if (cj) {
                    sc.finalCompactJoin = true;
                    sc.finalCompactSplit = baseClean.size();
                  }
                }
              }
              sc.finalDisplaySig = TrimAscii(sc.finalDisplay);
              sc.finalSemanticSig =
                  BuildPromptSemanticSig(sc.finalDisplay);
              sc.inTail = tailClean;
            }
            sc.tick = now;

            // Output from cache
            displayText = sc.finalDisplay;
            compactTailJoin = sc.finalCompactJoin;
            compactTailSplit = sc.finalCompactSplit;
            displaySig = sc.finalDisplaySig;
            displaySemanticSig = sc.finalSemanticSig;
            englishSemanticSig = sc.engSemanticSig;

            // Periodic prune of stale cache entries
            if ((now - s_ohhSwapCachePruneTick) > 5000) {
              s_ohhSwapCachePruneTick = now;
              for (auto it = s_ohhSwapCache.begin();
                   it != s_ohhSwapCache.end();) {
                if ((now - it->second.tick) > 10000)
                  it = s_ohhSwapCache.erase(it);
                else ++it;
              }
            }
          } else {
            // ── Original path for non-tail-join hints ────────────────────
            std::string cleanText = StripColorCodesSimple(oe.korean);
            std::string englishFallback = oe.english;
            if (!englishFallback.empty())
              englishFallback = StripColorCodesSimple(englishFallback);
            const bool needsRuntimeEnglish =
                englishFallback.empty() || englishFallback == key ||
                englishFallback.find("&&") != std::string::npos ||
                englishFallback.find("$$") != std::string::npos ||
                englishFallback.find('{') != std::string::npos ||
                englishFallback.find("[{+") != std::string::npos;
            if (needsRuntimeEnglish) {
              std::string runtimeEnglish;
              if (TextHook_GetRuntimeEnglishForKeyRecent(
                      key, runtimeEnglish, 1800)) {
                runtimeEnglish = StripColorCodesSimple(runtimeEnglish);
                runtimeEnglish = TrimAscii(runtimeEnglish);
                if (!runtimeEnglish.empty()) {
                  englishFallback = runtimeEnglish;
                }
              }
            }
            displayText =
                BindingResolver::ResolveBindingsInText(
                    oe.korean, englishFallback);
            TextHook_ResolveLocalizedTemplateArgs(
                key, englishFallback, displayText);
            displaySig = TrimAscii(displayText);
            displaySemanticSig = BuildPromptSemanticSig(displayText);
            if (!englishFallback.empty()) {
              std::string engDisplay =
                  BindingResolver::ResolveBindingsInText(
                      englishFallback, englishFallback);
              englishSemanticSig = BuildPromptSemanticSig(engDisplay);
            }
          }

          if (displayText.empty()) {
            continue;
          }
          // Dedup against hint pipeline and within ObjHudHint
          if ((!displaySig.empty() &&
               renderedHintSignatures.find(std::string("T:") + displaySig) !=
                   renderedHintSignatures.end()) ||
              (!displaySemanticSig.empty() &&
               renderedHintSignatures.find(std::string("S:") + displaySemanticSig) !=
                   renderedHintSignatures.end()) ||
              (!englishSemanticSig.empty() &&
               (renderedHintSignatures.find(std::string("E:") + englishSemanticSig) !=
                    renderedHintSignatures.end() ||
                renderedHintSignatures.find(std::string("S:") + englishSemanticSig) !=
                    renderedHintSignatures.end()))) {
            continue;
          }
          if (renderedObjHudHintSignatures.find(std::string("I:") + instanceSig) !=
              renderedObjHudHintSignatures.end()) {
            continue;
          }
          if (!displaySig.empty() &&
              renderedObjHudHintSignatures.find(std::string("T:") + displaySig) !=
                  renderedObjHudHintSignatures.end()) {
            continue;
          }
          if (!displaySemanticSig.empty() &&
              renderedObjHudHintSignatures.find(std::string("S:") + displaySemanticSig) !=
                  renderedObjHudHintSignatures.end()) {
            continue;
          }
          if (!englishSemanticSig.empty() &&
              (renderedObjHudHintSignatures.find(std::string("E:") + englishSemanticSig) !=
                   renderedObjHudHintSignatures.end() ||
               renderedObjHudHintSignatures.find(std::string("S:") + englishSemanticSig) !=
                   renderedObjHudHintSignatures.end())) {
            continue;
          }

          // Caller-level rapid-switch guard:
          // mixed prompt stacks can emit 2~3 different keys across a few frames.
          // Keep the first semantic briefly to prevent flash-chain artifacts.
          const std::string callerSemantic =
              !displaySemanticSig.empty()
                  ? displaySemanticSig
                  : (!englishSemanticSig.empty() ? englishSemanticSig
                                                 : displaySig);
          // Direct AddCmd prompt keys (MANTLE, weapon swap, pickup) can
          // legitimately coexist at the same callerOffset — skip the
          // rapid-switch guard so one does not suppress the other.
          if (oe.callerOffset != 0 && !callerSemantic.empty() &&
              !isDirectPromptKey) {
            ObjCallerSwitchGate &cg = s_objCallerSwitchGate[oe.callerOffset];
            if (cg.lastTick != 0 && now >= cg.lastTick &&
                (now - cg.lastTick) <= 130 &&
                !cg.semantic.empty() &&
                cg.semantic != callerSemantic) {
              continue;
            }
          }

          // === Width measurement + center alignment ===========================
          // Pre-compute compact tail split strings and widths once to avoid
          // redundant MeasureTextWidthEx calls and substr operations.
          std::string ctPre, ctTail;
          float ctPreW = 0.0f, ctTailW = 0.0f;
          const bool hasCompactTail = compactTailJoin &&
              compactTailSplit != std::string::npos &&
              compactTailSplit < displayText.size();
          float fullWidth = 0.0f;
          if (hasCompactTail) {
            ctPre = displayText.substr(0, compactTailSplit);
            ctTail = displayText.substr(compactTailSplit);
            ctPreW = hudText.MeasureTextWidthEx(ctPre, fontHeight, scale);
            ctTailW = hudText.MeasureTextWidthEx(ctTail, fontHeight, scale);
            compactTailGap =
                (std::min)(fontHeight * 0.16f,
                           (std::max)(1.5f * scaleY, fontHeight * 0.035f));
            fullWidth = ctPreW + compactTailGap + ctTailW;
          } else {
            fullWidth =
                hudText.MeasureTextWidthEx(displayText, fontHeight, scale);
          }

          if (customPosIsCenterAnchor)
            x = x - fullWidth * 0.5f;

          // === Render with key binding coloring ===============================
          if (hasCompactTail) {
            if (!ctPre.empty())
              hudText.QueueText(ctPre, x, y, scale, color,
                                        fontHeight, 0, ctPreW, false, false, 1.5f,
                                        false, false, false, 0);
            if (!ctTail.empty())
              hudText.QueueText(ctTail, x + ctPreW + compactTailGap, y,
                                        scale, color, fontHeight, 0, ctTailW,
                                        false, false, 1.5f,
                                        false, false, false, 0);
          } else {
            size_t lb = displayText.find('[');
            size_t rb = (lb != std::string::npos)
                            ? displayText.find(']', lb + 1) : std::string::npos;
            if (lb != std::string::npos && rb != std::string::npos &&
                       rb > lb + 1) {
              std::string pre = displayText.substr(0, lb);
              std::string keySeg = displayText.substr(lb, rb - lb + 1);
              std::string post = displayText.substr(rb + 1);
              float preW = hudText.MeasureTextWidthEx(pre, fontHeight, scale);
              float keyW = hudText.MeasureTextWidthEx(keySeg, fontHeight, scale);
              float keyColor[4] = {1.0f, 0.85f, 0.2f, color[3]};
              if (!pre.empty())
                hudText.QueueText(pre, x, y, scale, color,
                                          fontHeight, 0, preW, false, false, 1.5f,
                                          false, false, false, 0);
              hudText.QueueText(keySeg, x + preW, y, scale,
                                        keyColor, fontHeight, 0, keyW, false, false, 1.5f,
                                        false, false, false, 0);
              if (!post.empty()) {
                float postW = hudText.MeasureTextWidthEx(post, fontHeight, scale);
                hudText.QueueText(post, x + preW + keyW, y, scale,
                                          color, fontHeight, 0, postW, false, false, 1.5f,
                                          false, false, false, 0);
              }
            } else {
              hudText.QueueText(displayText, x, y, scale, color,
                                        fontHeight, 0, fullWidth, false, false, 1.5f,
                                        false, false, false, 0);
            }
          }
          {
            auto itRenderLog = s_objHintRenderLogTick.find(instanceSig);
            if (itRenderLog == s_objHintRenderLogTick.end() ||
                (now - itRenderLog->second) > 900) {
              s_objHintRenderLogTick[instanceSig] = now;
              const DWORD seenAge =
                  (oe.lastSeen != 0 && now >= oe.lastSeen) ? (now - oe.lastSeen) : 0;
              const DWORD nativeAge =
                  (oe.lastNativeSeenTick != 0 && now >= oe.lastNativeSeenTick)
                      ? (now - oe.lastNativeSeenTick)
                      : 0;
              const DWORD firstAge =
                  (oe.firstSeen != 0 && now >= oe.firstSeen) ? (now - oe.firstSeen) : 0;
              const DWORD gateAge =
                  (gateState.firstSeenTick != 0 && now >= gateState.firstSeenTick)
                      ? (now - gateState.firstSeenTick)
                      : 0;
              char rbuf[640];
              sprintf_s(
                  rbuf,
                  "[NOHH-RENDER] key=%s caller=+0x%X slot=%d x=%.1f y=%.1f a=%.2f seen=%lums native=%lums first=%lums gate=%lums conf=%d ev=%u cf=%u",
                  key.c_str(), oe.callerOffset, instanceSlot, x, y, color[3],
                  (unsigned long)seenAge, (unsigned long)nativeAge,
                  (unsigned long)firstAge, (unsigned long)gateAge,
                  gateState.confirmed ? 1 : 0, gateState.evidenceFrames,
                  gateState.consecutiveFrames);
              LogToFile(rbuf);
            }
          }
          {
            auto itDiag = s_objHintDiagRenderLogTick.find(instanceSig);
            if (itDiag == s_objHintDiagRenderLogTick.end() ||
                (now - itDiag->second) > 1200) {
              s_objHintDiagRenderLogTick[instanceSig] = now;
              const DWORD seenAge =
                  (oe.lastSeen != 0 && now >= oe.lastSeen) ? (now - oe.lastSeen) : 0;
              const DWORD nativeAge =
                  (oe.lastNativeSeenTick != 0 && now >= oe.lastNativeSeenTick)
                      ? (now - oe.lastNativeSeenTick)
                      : 0;
              char dbuf[768];
              sprintf_s(
                  dbuf,
                  "[OBJHUDHINT-RENDER] key=%s caller=+0x%X src=%u tok=%u slot=%d atlas=%u native=(%.1f,%.1f) pos=(%.1f,%.1f) scale=(%.3f,%.3f) h=%.1f a=%.2f seen=%lums nativeAge=%lums ev=%u cf=%u",
                  key.c_str(), oe.callerOffset, (unsigned int)oe.nativeSource,
                  (unsigned int)sourceToken, instanceSlot,
                  (unsigned int)KoreanAtlas::kAtlasSlotBase, oe.nativeX, oe.nativeY,
                  x, y, oe.nativeXScale, oe.nativeYScale, fontHeight, color[3],
                  (unsigned long)seenAge, (unsigned long)nativeAge,
                  gateState.evidenceFrames, gateState.consecutiveFrames);
              LogToFile(dbuf);
            }
            if (s_objHintDiagRenderLogTick.size() > 384) {
              for (auto it = s_objHintDiagRenderLogTick.begin();
                   it != s_objHintDiagRenderLogTick.end();) {
                if ((now - it->second) > 30000) {
                  it = s_objHintDiagRenderLogTick.erase(it);
                } else {
                  ++it;
                }
              }
            }
          }
          gateState.lastRenderTick = now;
          if (oe.callerOffset != 0) {
            ObjCallerSwitchGate &cg = s_objCallerSwitchGate[oe.callerOffset];
            cg.lastTick = now;
            const std::string callerSemantic =
                !displaySemanticSig.empty()
                    ? displaySemanticSig
                    : (!englishSemanticSig.empty() ? englishSemanticSig
                                                   : displaySig);
            cg.semantic = callerSemantic;
          }
          renderedObjHudHintSignatures.insert(std::string("I:") + instanceSig);
          if (!displaySig.empty()) {
            renderedObjHudHintSignatures.insert(std::string("T:") + displaySig);
            renderedHintSignatures.insert(std::string("T:") + displaySig);
          }
          if (!displaySemanticSig.empty()) {
            renderedObjHudHintSignatures.insert(std::string("S:") + displaySemanticSig);
            renderedHintSignatures.insert(std::string("S:") + displaySemanticSig);
          }
          if (!englishSemanticSig.empty()) {
            renderedObjHudHintSignatures.insert(std::string("E:") + englishSemanticSig);
            renderedObjHudHintSignatures.insert(std::string("S:") + englishSemanticSig);
            renderedHintSignatures.insert(std::string("E:") + englishSemanticSig);
            renderedHintSignatures.insert(std::string("S:") + englishSemanticSig);
          }
        }
        if ((now - s_objHudHintGatePruneTick) > 2000) {
          s_objHudHintGatePruneTick = now;
          for (auto it = s_objHudHintGateStates.begin();
               it != s_objHudHintGateStates.end();) {
            if ((now - it->second.lastSeenTick) > kObjHudHintGateRetainMs) {
              it = s_objHudHintGateStates.erase(it);
            } else {
              ++it;
            }
          }
        }
        if ((now - s_objCallerSwitchPruneTick) > 2000) {
          s_objCallerSwitchPruneTick = now;
          for (auto it = s_objCallerSwitchGate.begin();
               it != s_objCallerSwitchGate.end();) {
            if ((now - it->second.lastTick) > 2000) {
              it = s_objCallerSwitchGate.erase(it);
            } else {
              ++it;
            }
          }
        }
      }
    } else if (!objHudHintEntries.empty()) {
      static DWORD s_lastObjHintRenderGateLog = 0;
      if ((now - s_lastObjHintRenderGateLog) > 900) {
        s_lastObjHintRenderGateLog = now;
        char gbuf[256];
        sprintf_s(
            gbuf,
            "[NOHH-RENDER-GATE] count=%d suppress=%d pause=%d warm=%d credits=%d state=%d",
            (int)objHudHintEntries.size(), suppressHudOverlay ? 1 : 0,
            gate.pauseMenu ? 1 : 0, gate.warmupActive ? 1 : 0,
            gate.creditsActive ? 1 : 0, (int)overlaySessionState);
        LogToFile(gbuf);
      }
    }

    TextHook_EndOverlayFrame(overlayFrameId);
