    struct PromptAuthorityLockState {
      unsigned int callerOffset = 0;
      DWORD tick = 0;
    };
    struct PromptFadeHoldState {
      std::string displayText;
      float x = 0.0f;
      float y = 0.0f;
      float scale = 1.0f;
      float fontHeight = 0.0f;
      float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      bool disableShadow = false;
      uint8_t atlasSlot = KoreanAtlas::kAtlasSlotHeader;
      bool compactTailJoin = false;
      size_t compactTailSplit = std::string::npos;
      float compactTailGap = 0.0f;
      float fadeAlpha = 1.0f;
      DWORD lastLiveTick = 0;
      DWORD fadeStartTick = 0;
    };
    static std::unordered_map<std::string, PromptAuthorityLockState>
        s_promptAuthorityLockByKey;
    static std::unordered_map<std::string, PromptFadeHoldState>
        s_promptFadeHoldByKey;
    constexpr DWORD kPromptAuthorityLockHoldMs = 2000;
    constexpr DWORD kPromptFadeOutHoldMs = 300;
    auto isPromptAuthorityLockLive =
        [&](const PromptAuthorityLockState &lock) -> bool {
      return lock.callerOffset != 0 && lock.tick != 0 && now >= lock.tick &&
             (now - lock.tick) <= kPromptAuthorityLockHoldMs;
    };
    auto hasActivePromptFadeHold = [&]() -> bool {
      for (auto it = s_promptFadeHoldByKey.begin();
           it != s_promptFadeHoldByKey.end();) {
        DWORD baseTick =
            (it->second.fadeStartTick != 0) ? it->second.fadeStartTick
                                            : it->second.lastLiveTick;
        if (baseTick == 0 || now < baseTick ||
            (now - baseTick) > (kPromptFadeOutHoldMs + 200)) {
          it = s_promptFadeHoldByKey.erase(it);
        } else {
          ++it;
        }
      }
      return !s_promptFadeHoldByKey.empty();
    };
    // Suppress hint rendering for 2 seconds after restart to prevent
    // stacked HUD elements from flashing on the black loading screen.
    const unsigned long hintResetTick = TextHook_GetObjectiveRuntimeResetTick();
    const bool hintRestartSuppressed =
        (hintResetTick != 0 && now >= hintResetTick &&
         (now - hintResetTick) < 2000);
    // Suppress hints for 2 seconds after loading video closes so stale
    // HudElem entries captured during the video don't flash briefly.
    const bool hintPostVideoSuppressed =
        (g_PostVideoHintSuppressUntil != 0 && now < g_PostVideoHintSuppressUntil);
    // On restart/video suppress: clear stale fade/authority state so
    // nothing flashes when rendering resumes.
    if (hintRestartSuppressed || hintPostVideoSuppressed) {
      s_promptFadeHoldByKey.clear();
      s_promptAuthorityLockByKey.clear();
    }
    const bool hintOverlayVisible =
        ((!hintEntries.empty() || hasActivePromptFadeHold()) &&
         !suppressHudOverlay && !gate.creditsActive &&
         !hintRestartSuppressed && !hintPostVideoSuppressed);
    if (hintOverlayVisible) {
      constexpr float kHintHeight720 = 64.0f;
      constexpr float kBaselineFromTop720 = 43.0f;
      if (g_FrameSwapChainDescValid) {
        const DXGI_SWAP_CHAIN_DESC &hintScDesc = g_FrameSwapChainDesc;
        float screenWidth = (float)hintScDesc.BufferDesc.Width;
        float screenHeight = (float)hintScDesc.BufferDesc.Height;
        float scaleY = g_ActiveArea.height / 720.0f;

        // Native-only strict mode:
        // - require actual native evidence
        // - allow a short grace to hide frame-skips without leaving ghosts alive
        const DWORD kHintFreshWindowMs = 180;
        const DWORD kHintNativeBackedWindowMs = 260;
        auto isHintRenderable = [&](const HintOverlayEntry &he) -> bool {
          if (!he.hasNativeParams || he.lastNativeSeenTick == 0) {
            return false;
          }
          const DWORD ageSinceSeen = now - he.lastSeen;
          if (ageSinceSeen <= kHintFreshWindowMs) {
            return true;
          }
          if (he.lastNativeSeenTick != 0 &&
              (now - he.lastNativeSeenTick) <= kHintNativeBackedWindowMs) {
            return true;
          }
          return false;
        };
        auto hintSourceToken = [](const HintOverlayEntry &he) -> uint8_t {
          if (he.nativeSourceToken != 0) {
            return he.nativeSourceToken;
          }
          if (he.nativeSource == (unsigned char)HUD_NATIVE_ADDCMD) {
            return NATIVE_HUD_SOURCE_ADDCMD;
          }
          if (he.nativeSource == (unsigned char)HUD_NATIVE_RENDER) {
            return NATIVE_HUD_SOURCE_HUD560;
          }
          return NATIVE_HUD_SOURCE_BRIDGE;
        };
        auto isObjHudHintFamilyToken = [](uint8_t token) -> bool {
          return token == NATIVE_HUD_SOURCE_HUD560 ||
                 token == NATIVE_HUD_SOURCE_ADDCMD;
        };
        static std::unordered_map<std::string, DWORD> s_hintFamilySkipLogTick;
        static std::unordered_map<std::string, DWORD> s_hintFamilyColorFixLogTick;
        auto logHintFamilySkip = [&](const HintOverlayEntry &he, int nativeSlot,
                                     uint8_t token) {
          if (he.key.empty()) {
            return;
          }
          char sigBuf[256];
          snprintf(sigBuf, sizeof(sigBuf), "%s|%u|%u|%d",
                   he.key.c_str(), (unsigned int)he.callerOffset,
                   (unsigned int)token, nativeSlot);
          const std::string sig(sigBuf);
          auto it = s_hintFamilySkipLogTick.find(sig);
          if (it != s_hintFamilySkipLogTick.end() &&
              (now - it->second) <= 1200) {
            return;
          }
          s_hintFamilySkipLogTick[sig] = now;
          char sbuf[320];
          sprintf_s(
              sbuf,
              "[HUDHINT-FAMILY-SKIP] key=%s caller=+0x%X src=%u tok=%u slot=%d",
              he.key.c_str(), he.callerOffset, (unsigned int)he.nativeSource,
              (unsigned int)token, nativeSlot);
          LogToFile(sbuf);
          if (s_hintFamilySkipLogTick.size() > 384) {
            for (auto it2 = s_hintFamilySkipLogTick.begin();
                 it2 != s_hintFamilySkipLogTick.end();) {
              if ((now - it2->second) > 30000) {
                it2 = s_hintFamilySkipLogTick.erase(it2);
              } else {
                ++it2;
              }
            }
          }
        };
        auto logHintFamilyColorFix = [&](const HintOverlayEntry &he, int nativeSlot,
                                         uint8_t token, float r, float g, float b) {
          if (he.key.empty()) {
            return;
          }
          char sigBuf2[256];
          snprintf(sigBuf2, sizeof(sigBuf2), "%s|%u|%d",
                   he.key.c_str(), (unsigned int)he.callerOffset, nativeSlot);
          const std::string sig(sigBuf2);
          auto it = s_hintFamilyColorFixLogTick.find(sig);
          if (it != s_hintFamilyColorFixLogTick.end() &&
              (now - it->second) <= 1500) {
            return;
          }
          s_hintFamilyColorFixLogTick[sig] = now;
          char sbuf[384];
          sprintf_s(
              sbuf,
              "[HUDHINT-FAMILY-COLOR-FIX] key=%s caller=+0x%X src=%u tok=%u slot=%d rgb=(%.2f,%.2f,%.2f)",
              he.key.c_str(), he.callerOffset, (unsigned int)he.nativeSource,
              (unsigned int)token, nativeSlot, r, g, b);
          LogToFile(sbuf);
          if (s_hintFamilyColorFixLogTick.size() > 384) {
            for (auto it2 = s_hintFamilyColorFixLogTick.begin();
                 it2 != s_hintFamilyColorFixLogTick.end();) {
              if ((now - it2->second) > 30000) {
                it2 = s_hintFamilyColorFixLogTick.erase(it2);
              } else {
                ++it2;
              }
            }
          }
        };

        // Mantle hint subtitle fade (LUI: animateToState(hidden/default, 500ms))
        bool subtitlesShowing = (now - g_SubtitleActiveTime) < 150;
        static float s_HintMantleVis = 1.0f;
        static DWORD s_HintMantleVisTick = 0;
        DWORD mantleDt =
            (s_HintMantleVisTick == 0) ? 0 : (now - s_HintMantleVisTick);
        s_HintMantleVisTick = now;
        float mantleStep =
            (mantleDt > 1000) ? 1.0f : ((float)mantleDt / 500.0f);
        mantleStep = Clamp01(mantleStep);
        if (subtitlesShowing) {
          s_HintMantleVis = (std::max)(0.0f, s_HintMantleVis - mantleStep);
        } else {
          s_HintMantleVis = (std::min)(1.0f, s_HintMantleVis + mantleStep);
        }

        auto hintAuthorityRank = [&](const HintOverlayEntry &he) -> int {
          const uint8_t token = hintSourceToken(he);
          int rank = 0;
          if (he.hasNativeParams) {
            rank += 4;
          }
          if (token == NATIVE_HUD_SOURCE_ADDCMD) {
            rank += 12;
          } else if (token == NATIVE_HUD_SOURCE_HUD560) {
            rank += 8;
          } else if (he.nativeSource == (unsigned char)HUD_NATIVE_RENDER) {
            rank += 2;
          }
          if (he.callerOffset == IW6Offsets::Profile::Rva_2316DB) {
            rank += 16;
          }
          if (std::isfinite(he.color[3]) && he.color[3] > 0.01f) {
            rank += 8;
          }
          // Penalize wildly off-screen entries (virtual 480-space)
          // so on-screen entries win family selection.
          if (he.nativeY < -50.0f || he.nativeY > 530.0f) {
            rank -= 32;
          }
          return rank;
        };
        auto hintInstanceSig = [&](const HintOverlayEntry &he,
                                   int slot) -> std::string {
          const int slotBucket = (he.slotBucket > 0) ? he.slotBucket : slot;
          const uint8_t sourceToken = hintSourceToken(he);
          const OverlaySource overlaySource =
              (sourceToken == NATIVE_HUD_SOURCE_HUD560)
                  ? OVERLAY_SOURCE_HUD560
                  : ((sourceToken == NATIVE_HUD_SOURCE_ADDCMD)
                         ? OVERLAY_SOURCE_ADDCMD
                         : OVERLAY_SOURCE_SLC);
          const uint64_t consumer =
              (he.consumerId != 0)
                  ? he.consumerId
                  : TextHook_MakeHudConsumerId(overlaySource, he.callerOffset,
                                               slotBucket, he.nativeVirtualW,
                                               he.nativeVirtualH);
          char sigBuf[256];
          snprintf(sigBuf, sizeof(sigBuf), "%s|%llu|%d",
                   he.key.c_str(), (unsigned long long)consumer, slotBucket);
          return std::string(sigBuf);
        };
        auto hasExplicitLayoutScale = [](const HintOverlayEntry &he) -> bool {
          return (std::isfinite(he.nativeXScale) && he.nativeXScale > 0.01f &&
                  he.nativeXScale < 8.0f) ||
                 (std::isfinite(he.nativeYScale) && he.nativeYScale > 0.01f &&
                  he.nativeYScale < 8.0f);
        };
        auto interactionLayoutRank = [&](const HintOverlayEntry &he) -> int {
          const uint8_t token = hintSourceToken(he);
          const int slot =
              TextHook_HudSlotFromEvent(he.nativeY, he.nativeVirtualH, 0);
          int rank = 0;
          if (slot >= LUI_HINT_SLOT_ZOOM) {
            rank += 12;
          }
          if (token == NATIVE_HUD_SOURCE_ADDCMD) {
            rank += 16;
          } else if (token == NATIVE_HUD_SOURCE_HUD560) {
            rank += 8;
          } else if (he.nativeSource == (unsigned char)HUD_NATIVE_RENDER) {
            rank += 2;
          }
          if (hasExplicitLayoutScale(he)) {
            rank += 8;
          }
          if (he.nativeVirtualW == 0 || he.nativeVirtualH == 0) {
            rank += 4;
          }
          return rank;
        };
        auto looksLikeCenterPromptHint = [&](const HintOverlayEntry &he) -> bool {
          if (he.key.empty()) {
            return false;
          }
          std::string englishUpper =
              TrimAscii(StripColorCodesSimple(he.english));
          for (char &c : englishUpper) {
            c = (char)std::toupper((unsigned char)c);
          }
          if (englishUpper.rfind("PRESS ", 0) == 0 ||
              englishUpper.rfind("HOLD ", 0) == 0 ||
              englishUpper.rfind("TAP ", 0) == 0 ||
              englishUpper.rfind("USE ", 0) == 0 ||
              englishUpper.find(" PRESS ") != std::string::npos ||
              englishUpper.find(" HOLD ") != std::string::npos ||
              englishUpper.find("[{+") != std::string::npos ||
              englishUpper.find("&&") != std::string::npos ||
              englishUpper.find("$$") != std::string::npos) {
            return true;
          }
          return he.key.find("_PROMPT") != std::string::npos ||
                 he.key.find("_HINT") != std::string::npos ||
                 he.key.find("NIGHTVISION") != std::string::npos;
        };
        // Pre-compute looksLikeCenterPromptHint per entry to avoid
        // redundant StripColorCodesSimple + ToUpper across 6+ calls/entry.
        static std::vector<int8_t> hintCenterPromptCache;
        hintCenterPromptCache.assign(hintEntries.size(), -1);
        auto looksLikeCenterPromptHintCached =
            [&](const HintOverlayEntry &he) -> bool {
          const size_t idx =
              static_cast<size_t>(&he - hintEntries.data());
          if (idx < hintCenterPromptCache.size()) {
            if (hintCenterPromptCache[idx] >= 0) {
              return hintCenterPromptCache[idx] != 0;
            }
            bool result = looksLikeCenterPromptHint(he);
            hintCenterPromptCache[idx] = result ? 1 : 0;
            return result;
          }
          return looksLikeCenterPromptHint(he);
        };
        auto centerPromptAuthorityRank = [&](const HintOverlayEntry &he) -> int {
          int rank = hintAuthorityRank(he);
          const int slot =
              TextHook_HudSlotFromEvent(he.nativeY, he.nativeVirtualH, 0);
          if (slot > 0 && slot < LUI_HINT_SLOT_ZOOM) {
            rank += 24;
          }
          if (he.callerOffset == IW6Offsets::Profile::Rva_2316DB) {
            rank += 20;
          } else if (he.callerOffset == IW6Offsets::Profile::Rva_2454FC) {
            rank += 14;
          } else if (he.callerOffset == IW6Offsets::Profile::Rva_232562) {
            rank += 4;
          }
          if ((std::isfinite(he.nativeXScale) && he.nativeXScale >= 1.40f) ||
              (std::isfinite(he.nativeYScale) && he.nativeYScale >= 1.40f)) {
            rank += 6;
          }
          return rank;
        };
        auto familyWinnerAuthorityRank = [&](const HintOverlayEntry &he) -> int {
          int rank = hintAuthorityRank(he);
          const int slot =
              TextHook_HudSlotFromEvent(he.nativeY, he.nativeVirtualH, 0);
          if (slot >= LUI_HINT_SLOT_ZOOM) {
            rank += 32;
          } else if (slot > 0) {
            rank += 6;
          }
          if (looksLikeCenterPromptHintCached(he)) {
            rank += 12;
          }
          if (he.callerOffset == IW6Offsets::Profile::Rva_2316DB) {
            rank += 10;
          }
          const float scaleMax =
              (std::max)((std::isfinite(he.nativeXScale) && he.nativeXScale > 0.01f)
                             ? he.nativeXScale
                             : 1.0f,
                         (std::isfinite(he.nativeYScale) && he.nativeYScale > 0.01f)
                             ? he.nativeYScale
                             : 1.0f);
          if (std::isfinite(scaleMax) && scaleMax > 0.01f && scaleMax < 8.0f) {
            rank += (int)std::lround(scaleMax * 10.0f);
          }
          return rank;
        };
        auto isPromptFamilyEntry = [&](const HintOverlayEntry &he,
                                       int nativeSlot) -> bool {
          return !he.key.empty() &&
                 (looksLikeCenterPromptHintCached(he) ||
                  nativeSlot >= LUI_HINT_SLOT_ZOOM);
        };
        auto isPromptAuthorityLockedOut = [&](const HintOverlayEntry &he,
                                              int nativeSlot) -> bool {
          if (!isPromptFamilyEntry(he, nativeSlot)) {
            return false;
          }
          auto itLock = s_promptAuthorityLockByKey.find(he.key);
          if (itLock == s_promptAuthorityLockByKey.end()) {
            return false;
          }
          if (!isPromptAuthorityLockLive(itLock->second) ||
              he.callerOffset == 0) {
            return false;
          }
          return he.callerOffset != itLock->second.callerOffset;
        };
        auto queueHintText = [&](const std::string &displayText, float drawX,
                                 float drawY, float drawScale,
                                 const float drawColor[4], float drawFontHeight,
                                 bool drawDisableShadow, uint8_t drawAtlasSlot,
                                 bool drawCompactTailJoin,
                                 size_t drawCompactTailSplit,
                                 float drawCompactTailGap,
                                 float cachedFullWidth = -1.0f) {
          if (drawCompactTailJoin && drawCompactTailSplit != std::string::npos &&
              drawCompactTailSplit < displayText.size()) {
            const std::string pre = displayText.substr(0, drawCompactTailSplit);
            const std::string tail = displayText.substr(drawCompactTailSplit);
            const float preW = hudText.MeasureTextWidthEx(
                pre, drawFontHeight, drawScale, drawAtlasSlot);
            const float tailW = hudText.MeasureTextWidthEx(
                tail, drawFontHeight, drawScale, drawAtlasSlot);
            if (!pre.empty()) {
              hudText.QueueText(
                  pre, drawX, drawY, drawScale, drawColor, drawFontHeight, 0,
                  preW, drawDisableShadow, false, 1.5f, false, false, false,
                  -1, 0.0f, false, true, drawAtlasSlot);
            }
            if (!tail.empty()) {
              hudText.QueueText(
                  tail, drawX + preW + drawCompactTailGap, drawY, drawScale,
                  drawColor, drawFontHeight, 0, tailW, drawDisableShadow, false,
                  1.5f, false, false, false, -1, 0.0f, false, true,
                  drawAtlasSlot);
            }
          } else {
            const float renderWidth = (cachedFullWidth > 0.0f) ? cachedFullWidth :
                hudText.MeasureTextWidthEx(displayText, drawFontHeight,
                                                   drawScale, drawAtlasSlot);
            size_t lb = displayText.find('[');
            size_t rb = (lb != std::string::npos)
                            ? displayText.find(']', lb + 1)
                            : std::string::npos;
            if (lb != std::string::npos && rb != std::string::npos &&
                       rb > lb + 1) {
              std::string pre = displayText.substr(0, lb);
              std::string keySeg = displayText.substr(lb, rb - lb + 1);
              std::string post = displayText.substr(rb + 1);

              float preW = hudText.MeasureTextWidthEx(
                  pre, drawFontHeight, drawScale, drawAtlasSlot);
              float keyW = hudText.MeasureTextWidthEx(
                  keySeg, drawFontHeight, drawScale, drawAtlasSlot);
              float keyColor[4] = {1.0f, 0.85f, 0.2f, drawColor[3]};

              if (!pre.empty()) {
                hudText.QueueText(
                    pre, drawX, drawY, drawScale, drawColor, drawFontHeight, 0,
                    preW, drawDisableShadow, false, 1.5f, false, false, false,
                    -1, 0.0f, false, true, drawAtlasSlot);
              }
              hudText.QueueText(
                  keySeg, drawX + preW, drawY, drawScale, keyColor,
                  drawFontHeight, 0, keyW, drawDisableShadow, false, 1.5f,
                  false, false, false, -1, 0.0f, false, true, drawAtlasSlot);
              if (!post.empty()) {
                float postW = hudText.MeasureTextWidthEx(
                    post, drawFontHeight, drawScale, drawAtlasSlot);
                hudText.QueueText(
                    post, drawX + preW + keyW, drawY, drawScale, drawColor,
                    drawFontHeight, 0, postW, drawDisableShadow, false, 1.5f,
                    false, false, false, -1, 0.0f, false, true, drawAtlasSlot);
              }
            } else {
              hudText.QueueText(
                  displayText, drawX, drawY, drawScale, drawColor, drawFontHeight,
                  0, renderWidth, drawDisableShadow, false, 1.5f, false, false,
                  false, -1, 0.0f, false, true, drawAtlasSlot);
            }
          }
        };

        static std::unordered_map<std::string, int> bestByInstance;
        bestByInstance.clear();
        for (int i = 0; i < (int)hintEntries.size(); ++i) {
          const auto &he = hintEntries[i];
          if (!isHintRenderable(he) || he.key.empty())
            continue;
          if (isObjHudHintFamilyToken(hintSourceToken(he)) &&
              he.key != "GAME_GET_TO_COVER") {
            continue;
          }
          // Skip keys already rendered by the unified objective subsystem
          // to prevent duplicate rendering at wrong position/scale.
          if (ObjUnified_WasKeyRenderedThisFrame(he.key)) {
            continue;
          }
          const int slot =
              TextHook_HudSlotFromEvent(he.nativeY, he.nativeVirtualH, 0);
          if (slot <= 0 || slot >= LUI_HINT_SLOT_COUNT) continue;
          const std::string sig = hintInstanceSig(he, slot);
          auto it = bestByInstance.find(sig);
          if (it == bestByInstance.end()) {
            bestByInstance.emplace(sig, i);
          } else {
            const auto &prev = hintEntries[it->second];
            const int candRank = hintAuthorityRank(he);
            const int prevRank = hintAuthorityRank(prev);
            if (candRank > prevRank ||
                (candRank == prevRank && he.lastSeen >= prev.lastSeen)) {
              it->second = i;
            }
          }
        }

        static std::vector<int> renderOrder;
        renderOrder.clear();
        for (const auto &kv : bestByInstance) {
          renderOrder.push_back(kv.second);
        }
        static std::unordered_map<std::string, int> bestPromptAuthorityCandidateByKey;
        bestPromptAuthorityCandidateByKey.clear();
        static std::unordered_map<std::string, int> bestPromptLockedCallerByKey;
        bestPromptLockedCallerByKey.clear();
        // Merged pass: build both locked-caller and authority-candidate maps
        // in a single loop over renderOrder (was 2 separate passes).
        for (int idx : renderOrder) {
          const auto &he = hintEntries[idx];
          const int slot =
              TextHook_HudSlotFromEvent(he.nativeY, he.nativeVirtualH, 0);
          if (!isPromptFamilyEntry(he, slot)) {
            continue;
          }
          const float alpha =
              (std::isfinite(he.color[3]) && he.color[3] > 0.0f)
                  ? Clamp01(he.color[3])
                  : 0.0f;
          const float maxRgb =
              (std::max)(he.color[0], (std::max)(he.color[1], he.color[2]));
          // Locked-caller selection (runs regardless of visibility)
          auto itLock = s_promptAuthorityLockByKey.find(he.key);
          if (itLock != s_promptAuthorityLockByKey.end() &&
              isPromptAuthorityLockLive(itLock->second) &&
              he.callerOffset == itLock->second.callerOffset) {
            int candRank = centerPromptAuthorityRank(he);
            candRank += (alpha > 0.01f) ? 40 : 0;
            candRank += (maxRgb > 0.02f) ? 18 : 0;
            candRank += hasExplicitLayoutScale(he) ? 6 : 0;
            auto it = bestPromptLockedCallerByKey.find(he.key);
            if (it == bestPromptLockedCallerByKey.end()) {
              bestPromptLockedCallerByKey.emplace(he.key, idx);
            } else {
              const auto &prev = hintEntries[it->second];
              int prevRank = centerPromptAuthorityRank(prev);
              const float prevAlpha =
                  (std::isfinite(prev.color[3]) && prev.color[3] > 0.0f)
                      ? Clamp01(prev.color[3])
                      : 0.0f;
              const float prevMaxRgb =
                  (std::max)(prev.color[0], (std::max)(prev.color[1], prev.color[2]));
              prevRank += (prevAlpha > 0.01f) ? 40 : 0;
              prevRank += (prevMaxRgb > 0.02f) ? 18 : 0;
              prevRank += hasExplicitLayoutScale(prev) ? 6 : 0;
              if (candRank > prevRank ||
                  (candRank == prevRank && he.lastSeen >= prev.lastSeen)) {
                it->second = idx;
              }
            }
          }
          // Authority-candidate selection (skip invisible entries)
          if (alpha <= 0.01f && maxRgb <= 0.02f) {
            // --- GAME_GET_TO_COVER diagnostic logging ---
            if (he.key == "GAME_GET_TO_COVER") {
              static DWORD s_lastCoverLog = 0;
              if ((now - s_lastCoverLog) > 500) {
                s_lastCoverLog = now;
                int pdr = -1;
                TextHook_TryGetDvarInt("player_died_recently", pdr);
                char dbg[200];
                sprintf_s(dbg,
                    "[COVER-DIAG] GAME_GET_TO_COVER present but a=%.3f rgb=%.3f "
                    "player_died_recently=%d caller=+0x%X",
                    alpha, maxRgb, pdr, (unsigned int)he.callerOffset);
                LogToFile(dbg);
              }
            }
            continue;
          }
          int candRank = centerPromptAuthorityRank(he);
          candRank += (alpha > 0.01f) ? 40 : 0;
          candRank += (maxRgb > 0.02f) ? 18 : 0;
          candRank += hasExplicitLayoutScale(he) ? 6 : 0;
          auto it = bestPromptAuthorityCandidateByKey.find(he.key);
          if (it == bestPromptAuthorityCandidateByKey.end()) {
            bestPromptAuthorityCandidateByKey.emplace(he.key, idx);
          } else {
            const auto &prev = hintEntries[it->second];
            int prevRank = centerPromptAuthorityRank(prev);
            const float prevAlpha =
                (std::isfinite(prev.color[3]) && prev.color[3] > 0.0f)
                    ? Clamp01(prev.color[3])
                    : 0.0f;
            const float prevMaxRgb =
                (std::max)(prev.color[0], (std::max)(prev.color[1], prev.color[2]));
            prevRank += (prevAlpha > 0.01f) ? 40 : 0;
            prevRank += (prevMaxRgb > 0.02f) ? 18 : 0;
            prevRank += hasExplicitLayoutScale(prev) ? 6 : 0;
            if (candRank > prevRank ||
                (candRank == prevRank && he.lastSeen >= prev.lastSeen)) {
              it->second = idx;
            }
          }
        }
        for (const auto &kv : bestPromptLockedCallerByKey) {
          PromptAuthorityLockState &lock = s_promptAuthorityLockByKey[kv.first];
          if (lock.callerOffset != 0) {
            lock.tick = now;
          }
        }
        for (const auto &kv : bestPromptAuthorityCandidateByKey) {
          if (bestPromptLockedCallerByKey.find(kv.first) !=
              bestPromptLockedCallerByKey.end()) {
            continue;
          }
          const auto &he = hintEntries[kv.second];
          if (he.callerOffset == 0) {
            continue;
          }
          PromptAuthorityLockState &lock = s_promptAuthorityLockByKey[kv.first];
          lock.callerOffset = he.callerOffset;
          lock.tick = now;
        }
        if (s_promptAuthorityLockByKey.size() > 128) {
          for (auto it = s_promptAuthorityLockByKey.begin();
               it != s_promptAuthorityLockByKey.end();) {
            const DWORD baseTick = it->second.tick;
            if (baseTick == 0 || now < baseTick ||
                (now - baseTick) > 120000) {
              it = s_promptAuthorityLockByKey.erase(it);
            } else {
              ++it;
            }
          }
        }
        static std::unordered_map<std::string, int> bestCenterByKey;
        bestCenterByKey.clear();
        static std::unordered_map<std::string, int> bestInteractionByKey;
        bestInteractionByKey.clear();
        static std::unordered_map<std::string, int> bestVisibleSupportByKey;
        bestVisibleSupportByKey.clear();
        static std::unordered_map<std::string, int> bestFamilyByKey;
        bestFamilyByKey.clear();
        struct PromptPulseProbe {
          bool sawZeroAlpha = false;
          float minScale = 9999.0f;
          float maxScale = 0.0f;
          float minY = 9999.0f;
          float maxY = -9999.0f;
          unsigned int firstCaller = 0;
          bool mixedCaller = false;
        };
        static std::unordered_map<std::string, PromptPulseProbe> promptPulseProbeByKey;
        promptPulseProbeByKey.clear();
        static std::unordered_map<std::string, int> promptInstanceCountByKey;
        promptInstanceCountByKey.clear();
        for (int idx : renderOrder) {
          const auto &he = hintEntries[idx];
          const int slot =
              TextHook_HudSlotFromEvent(he.nativeY, he.nativeVirtualH, 0);
          if (!he.key.empty() &&
              (looksLikeCenterPromptHintCached(he) || slot >= LUI_HINT_SLOT_ZOOM)) {
            promptInstanceCountByKey[he.key] += 1;
            PromptPulseProbe &probe = promptPulseProbeByKey[he.key];
            const float rawAlpha =
                (std::isfinite(he.color[3]) && he.color[3] > 0.0f)
                    ? Clamp01(he.color[3])
                    : 0.0f;
            if (rawAlpha <= 0.01f) {
              probe.sawZeroAlpha = true;
            }
            const float scaleMax =
                (std::max)((std::isfinite(he.nativeXScale) && he.nativeXScale > 0.01f)
                               ? he.nativeXScale
                               : 1.0f,
                           (std::isfinite(he.nativeYScale) && he.nativeYScale > 0.01f)
                               ? he.nativeYScale
                               : 1.0f);
            probe.minScale = (std::min)(probe.minScale, scaleMax);
            probe.maxScale = (std::max)(probe.maxScale, scaleMax);
            probe.minY = (std::min)(probe.minY, he.nativeY);
            probe.maxY = (std::max)(probe.maxY, he.nativeY);
            if (probe.firstCaller == 0) {
              probe.firstCaller = he.callerOffset;
            } else if (he.callerOffset != 0 && he.callerOffset != probe.firstCaller) {
              probe.mixedCaller = true;
            }
          }
          if (isPromptAuthorityLockedOut(he, slot)) {
            continue;
          }
          if (slot > 0 && slot < LUI_HINT_SLOT_ZOOM &&
              looksLikeCenterPromptHintCached(he) && !he.key.empty()) {
            auto it = bestCenterByKey.find(he.key);
            if (it == bestCenterByKey.end()) {
              bestCenterByKey.emplace(he.key, idx);
            } else {
              const auto &prev = hintEntries[it->second];
              const int candRank = centerPromptAuthorityRank(he);
              const int prevRank = centerPromptAuthorityRank(prev);
              if (candRank > prevRank ||
                  (candRank == prevRank && he.lastSeen >= prev.lastSeen)) {
                it->second = idx;
              }
            }
          }
          const float alpha =
              (std::isfinite(he.color[3]) && he.color[3] > 0.0f) ? he.color[3] : 0.0f;
          const float maxRgb =
              (std::max)(he.color[0], (std::max)(he.color[1], he.color[2]));
          if (alpha > 0.01f || maxRgb > 0.02f) {
            auto it = bestVisibleSupportByKey.find(he.key);
            const int candRank =
                hintAuthorityRank(he) + ((alpha > 0.01f) ? 8 : 0) +
                ((maxRgb > 0.02f) ? 4 : 0);
            if (it == bestVisibleSupportByKey.end()) {
              bestVisibleSupportByKey.emplace(he.key, idx);
            } else {
              const auto &prev = hintEntries[it->second];
              const float prevAlpha =
                  (std::isfinite(prev.color[3]) && prev.color[3] > 0.0f)
                      ? prev.color[3]
                      : 0.0f;
              const float prevMaxRgb =
                  (std::max)(prev.color[0],
                             (std::max)(prev.color[1], prev.color[2]));
              const int prevRank =
                  hintAuthorityRank(prev) + ((prevAlpha > 0.01f) ? 8 : 0) +
                  ((prevMaxRgb > 0.02f) ? 4 : 0);
              if (candRank > prevRank ||
                  (candRank == prevRank && he.lastSeen >= prev.lastSeen)) {
                it->second = idx;
              }
            }
          }
          if (slot < LUI_HINT_SLOT_ZOOM || he.key.empty()) {
            auto itFamily = bestFamilyByKey.find(he.key);
            const int candRank = familyWinnerAuthorityRank(he);
            if (itFamily == bestFamilyByKey.end()) {
              bestFamilyByKey.emplace(he.key, idx);
            } else {
              const auto &prev = hintEntries[itFamily->second];
              const int prevRank = familyWinnerAuthorityRank(prev);
              if (candRank > prevRank ||
                  (candRank == prevRank && he.lastSeen >= prev.lastSeen)) {
                itFamily->second = idx;
              }
            }
            continue;
          }
          auto it = bestInteractionByKey.find(he.key);
          if (it == bestInteractionByKey.end()) {
            bestInteractionByKey.emplace(he.key, idx);
            it = bestInteractionByKey.find(he.key);
          } else {
            const auto &prev = hintEntries[it->second];
            const int candRank = interactionLayoutRank(he);
            const int prevRank = interactionLayoutRank(prev);
            if (candRank > prevRank ||
                (candRank == prevRank && he.lastSeen >= prev.lastSeen)) {
              it->second = idx;
            }
          }
          auto itFamily = bestFamilyByKey.find(he.key);
          const bool isInteractionWinner =
              (it != bestInteractionByKey.end() && it->second == idx);
          int candFamilyRank =
              familyWinnerAuthorityRank(he) + (isInteractionWinner ? 64 : 0);
          if (itFamily == bestFamilyByKey.end()) {
            bestFamilyByKey.emplace(he.key, idx);
          } else {
            const auto &prev = hintEntries[itFamily->second];
            int prevFamilyRank = familyWinnerAuthorityRank(prev);
            auto itPrevInteraction = bestInteractionByKey.find(prev.key);
            if (itPrevInteraction != bestInteractionByKey.end() &&
                itPrevInteraction->second == itFamily->second) {
              prevFamilyRank += 64;
            }
            if (candFamilyRank > prevFamilyRank ||
                (candFamilyRank == prevFamilyRank &&
                 he.lastSeen >= prev.lastSeen)) {
              itFamily->second = idx;
            }
          }
        }
        for (const auto &kv : promptPulseProbeByKey) {
          bool unstable = false;
          const PromptPulseProbe &probe = kv.second;
          if (probe.sawZeroAlpha || ((probe.maxScale - probe.minScale) > 0.20f) ||
              ((probe.maxY - probe.minY) > 8.0f) || probe.mixedCaller) {
            unstable = true;
          }
          auto itCount = promptInstanceCountByKey.find(kv.first);
          if (itCount != promptInstanceCountByKey.end() && itCount->second > 1) {
            unstable = true;
          }
          auto itLock = s_promptAuthorityLockByKey.find(kv.first);
          if (itLock != s_promptAuthorityLockByKey.end()) {
          }
        }
        for (const auto &kv : bestCenterByKey) {
          const auto &he = hintEntries[kv.second];
          auto itFamily = bestFamilyByKey.find(kv.first);
          if (itFamily == bestFamilyByKey.end()) {
            bestFamilyByKey.emplace(kv.first, kv.second);
            continue;
          }
          const auto &prev = hintEntries[itFamily->second];
          const int candRank = familyWinnerAuthorityRank(he) + 48;
          int prevRank = familyWinnerAuthorityRank(prev);
          auto itPrevCenter = bestCenterByKey.find(kv.first);
          if (itPrevCenter != bestCenterByKey.end() &&
              itPrevCenter->second == itFamily->second) {
            prevRank += 48;
          }
          auto itPrevInteraction = bestInteractionByKey.find(kv.first);
          if (itPrevInteraction != bestInteractionByKey.end() &&
              itPrevInteraction->second == itFamily->second) {
            prevRank += 64;
          }
          if (candRank > prevRank ||
              (candRank == prevRank && he.lastSeen >= prev.lastSeen)) {
            itFamily->second = kv.second;
          }
        }
        const bool rawBinkPromptBootstrapActive =
            BinkHook::IsVideoPlaying() && !BinkHook::IsCutsceneVideoPlaying();
        if (rawBinkPromptBootstrapActive) {
          static std::unordered_map<std::string, DWORD>
              s_hintAuthBootstrapLogTick;
          const std::string activeBinkVideo = BinkHook::GetCurrentVideoName();
          for (const auto &kv : bestFamilyByKey) {
            const std::string &bootstrapKey = kv.first;
            if (bootstrapKey.empty()) {
              continue;
            }
            auto itVisibleSupport = bestVisibleSupportByKey.find(bootstrapKey);
            if (itVisibleSupport != bestVisibleSupportByKey.end()) {
              continue;
            }
            auto itPromptCount = promptInstanceCountByKey.find(bootstrapKey);
            if (itPromptCount == promptInstanceCountByKey.end() ||
                itPromptCount->second < 2) {
              continue;
            }
            auto itProbe = promptPulseProbeByKey.find(bootstrapKey);
            if (itProbe == promptPulseProbeByKey.end() ||
                !itProbe->second.sawZeroAlpha) {
              continue;
            }
            const auto &bootstrapHe = hintEntries[kv.second];
            const int bootstrapSlot = TextHook_HudSlotFromEvent(
                bootstrapHe.nativeY, bootstrapHe.nativeVirtualH, 0);
            if (!isPromptFamilyEntry(bootstrapHe, bootstrapSlot) ||
                bootstrapHe.callerOffset == 0) {
              continue;
            }
            auto itLock = s_promptAuthorityLockByKey.find(bootstrapKey);
            if (itLock != s_promptAuthorityLockByKey.end() &&
                isPromptAuthorityLockLive(itLock->second)) {
              continue;
            }
            PromptAuthorityLockState &lock =
                s_promptAuthorityLockByKey[bootstrapKey];
            lock.callerOffset = bootstrapHe.callerOffset;
            lock.tick = now;

            char lsBuf[256];
            snprintf(lsBuf, sizeof(lsBuf), "%s|%u",
                     bootstrapKey.c_str(), (unsigned int)bootstrapHe.callerOffset);
            const std::string logSig(lsBuf);
            auto itLog = s_hintAuthBootstrapLogTick.find(logSig);
            if (itLog == s_hintAuthBootstrapLogTick.end() ||
                (now - itLog->second) > 1200) {
              s_hintAuthBootstrapLogTick[logSig] = now;
              char lbuf[384];
              sprintf_s(
                  lbuf,
                  "[HUDHINT-AUTH-BOOTSTRAP] key=%s caller=+0x%X slot=%d video=%s",
                  bootstrapKey.c_str(), bootstrapHe.callerOffset,
                  bootstrapSlot, activeBinkVideo.c_str());
              LogToFile(lbuf);
            }
          }
        }
        // SSOT 6F: CLOCKWORK_HINT_DRILL authority bootstrap.
        // The vault drill scene emits this key from a single source with
        // zero-alpha; there is no active Bink session, so the rawBink
        // bootstrap above won't fire.  Create an authority lock for drill
        // keys when: (a) sawZeroAlpha evidence exists, (b) no visible
        // support, (c) confirmed prompt family entry with valid caller.
        for (const auto &kv : bestFamilyByKey) {
          const std::string &bk = kv.first;
          if (bk != "CLOCKWORK_HINT_DRILL") {
            continue;
          }
          auto itVis = bestVisibleSupportByKey.find(bk);
          if (itVis != bestVisibleSupportByKey.end()) {
            continue;
          }
          auto itProbe = promptPulseProbeByKey.find(bk);
          if (itProbe == promptPulseProbeByKey.end() ||
              !itProbe->second.sawZeroAlpha) {
            continue;
          }
          // Prefer a HUD560 caller (+0x1F9BA3 or similar) which has the
          // actual on-screen position/scale, over the ObjRev bridge
          // caller +0x204816 which carries ghost init data (scale=2.504,
          // native=(320,240)) unrelated to the visible prompt.
          int bestDrillIdx = kv.second;
          for (int di = 0; di < (int)hintEntries.size(); ++di) {
            if (hintEntries[di].key == bk &&
                hintEntries[di].callerOffset != IW6Offsets::Profile::Rva_2454FC &&
                hintEntries[di].callerOffset != 0) {
              bestDrillIdx = di;
              break;
            }
          }
          const auto &bhe = hintEntries[bestDrillIdx];
          const int bSlot = TextHook_HudSlotFromEvent(
              bhe.nativeY, bhe.nativeVirtualH, 0);
          if (!isPromptFamilyEntry(bhe, bSlot) || bhe.callerOffset == 0) {
            continue;
          }
          auto itLock = s_promptAuthorityLockByKey.find(bk);
          if (itLock != s_promptAuthorityLockByKey.end() &&
              isPromptAuthorityLockLive(itLock->second)) {
            continue;
          }
          PromptAuthorityLockState &lock = s_promptAuthorityLockByKey[bk];
          lock.callerOffset = bhe.callerOffset;
          lock.tick = now;

          char lbuf[384];
          sprintf_s(lbuf,
                    "[HUDHINT-AUTH-BOOTSTRAP-DRILL] key=%s caller=+0x%X "
                    "slot=%d",
                    bk.c_str(), bhe.callerOffset, bSlot);
          LogToFile(lbuf);
        }
        static std::unordered_map<std::string, int> bestPromptAuthorityRenderByKey;
        bestPromptAuthorityRenderByKey.clear();
        for (int idx : renderOrder) {
          const auto &he = hintEntries[idx];
          const int slot =
              TextHook_HudSlotFromEvent(he.nativeY, he.nativeVirtualH, 0);
          if (!isPromptFamilyEntry(he, slot)) {
            continue;
          }
          auto itLock = s_promptAuthorityLockByKey.find(he.key);
          if (itLock == s_promptAuthorityLockByKey.end() ||
              !isPromptAuthorityLockLive(itLock->second) ||
              he.callerOffset != itLock->second.callerOffset) {
            continue;
          }
          int candRank = familyWinnerAuthorityRank(he);
          const float alpha =
              (std::isfinite(he.color[3]) && he.color[3] > 0.0f)
                  ? Clamp01(he.color[3])
                  : 0.0f;
          const float maxRgb =
              (std::max)(he.color[0], (std::max)(he.color[1], he.color[2]));
          candRank += (alpha > 0.01f) ? 40 : 0;
          candRank += (maxRgb > 0.02f) ? 18 : 0;
          candRank += hasExplicitLayoutScale(he) ? 6 : 0;
          auto it = bestPromptAuthorityRenderByKey.find(he.key);
          if (it == bestPromptAuthorityRenderByKey.end()) {
            bestPromptAuthorityRenderByKey.emplace(he.key, idx);
          } else {
            const auto &prev = hintEntries[it->second];
            int prevRank = familyWinnerAuthorityRank(prev);
            const float prevAlpha =
                (std::isfinite(prev.color[3]) && prev.color[3] > 0.0f)
                    ? Clamp01(prev.color[3])
                    : 0.0f;
            const float prevMaxRgb =
                (std::max)(prev.color[0], (std::max)(prev.color[1], prev.color[2]));
            prevRank += (prevAlpha > 0.01f) ? 40 : 0;
            prevRank += (prevMaxRgb > 0.02f) ? 18 : 0;
            prevRank += hasExplicitLayoutScale(prev) ? 6 : 0;
            if (candRank > prevRank ||
                (candRank == prevRank && he.lastSeen >= prev.lastSeen)) {
              it->second = idx;
            }
          }
        }
        for (const auto &kv : bestPromptAuthorityRenderByKey) {
          bestFamilyByKey[kv.first] = kv.second;
          const auto &he = hintEntries[kv.second];
          const int slot =
              TextHook_HudSlotFromEvent(he.nativeY, he.nativeVirtualH, 0);
          if (slot >= LUI_HINT_SLOT_ZOOM) {
            bestInteractionByKey[kv.first] = kv.second;
          } else if (slot > 0 && looksLikeCenterPromptHintCached(he)) {
            bestCenterByKey[kv.first] = kv.second;
          }
        }
        std::sort(renderOrder.begin(), renderOrder.end(),
                  [&](int a, int b) {
                    const int rankA = interactionLayoutRank(hintEntries[a]);
                    const int rankB = interactionLayoutRank(hintEntries[b]);
                    if (rankA != rankB) {
                      return rankA > rankB;
                    }
                    const int slotA = TextHook_HudSlotFromEvent(
                        hintEntries[a].nativeY, hintEntries[a].nativeVirtualH, 0);
                    const int slotB = TextHook_HudSlotFromEvent(
                        hintEntries[b].nativeY, hintEntries[b].nativeVirtualH, 0);
                    if (slotA != slotB) {
                      return slotA < slotB;
                    }
                    const float yA = hintEntries[a].nativeY;
                    const float yB = hintEntries[b].nativeY;
                    if (std::isfinite(yA) && std::isfinite(yB) &&
                        std::fabs(yA - yB) > 0.25f) {
                      return yA < yB;
                    }
                    if (hintEntries[a].lastSeen != hintEntries[b].lastSeen) {
                      return hintEntries[a].lastSeen > hintEntries[b].lastSeen;
                    }
                    return hintEntries[a].key < hintEntries[b].key;
                  });

        static std::unordered_map<std::string, DWORD> s_hintRenderLogTick;
        static std::unordered_map<std::string, DWORD> s_hintDiagRenderLogTick;
        static std::unordered_map<std::string, DWORD> s_hintFadeRenderLogTick;
        struct HintAlphaSmoothState {
          float alpha = 0.0f;
          DWORD tick = 0;
        };
        static std::unordered_map<std::string, HintAlphaSmoothState>
            s_hintAlphaSmooth;
        struct PromptStableLayoutState {
          float x = 0.0f;
          float y = 0.0f;
          float scale = 1.0f;
          float fontHeight = 0.0f;
          float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
          DWORD tick = 0;
        };
        static std::unordered_map<std::string, PromptStableLayoutState>
            s_promptStableLayout;
        struct PromptAuthorityLayoutState {
          unsigned int callerOffset = 0;
          float x = 0.0f;
          float y = 0.0f;
          float scale = 1.0f;
          float fontHeight = 0.0f;
          float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
          DWORD tick = 0;
        };
        static std::unordered_map<std::string, PromptAuthorityLayoutState>
            s_promptAuthorityLayout;
        auto shouldUseHudHintFamilyShadow = [](const HintOverlayEntry &he,
                                               int nativeSlot) -> bool {
          (void)he;
          return nativeSlot > 0;
        };

        constexpr float kHintVerticalNudge720 = -14.0f;
        static std::unordered_set<std::string> renderedPromptKeysThisFrame;
        renderedPromptKeysThisFrame.clear();

        // Keys present in OBJHUDHINT that require forced centering.
        // HUDHINT must yield these to OBJHUDHINT which renders at
        // screenWidth/2 with proper center anchoring.
        static std::unordered_set<std::string> objHudHintCenterKeys;
        objHudHintCenterKeys.clear();
        for (const auto &oe : objHudHintEntries) {
          if (!oe.key.empty() && (now - oe.lastSeen) <= 520) {
            const auto &k = oe.key;
            if (k.find("_FAIL") != std::string::npos ||
                k.find("FAIL_") != std::string::npos ||
                k.find("_DEATH") != std::string::npos ||
                k.find("_SUICIDE") != std::string::npos ||
                k.find("_QUOTE_") != std::string::npos ||
                k.find("_KILLED") != std::string::npos ||
                k.find("GOT_AWAY") != std::string::npos) {
              objHudHintCenterKeys.insert(k);
            }
          }
        }

        // QTE standalone overlay state — persists across frames regardless
        // of hint entry lifecycle, allowing smooth animation.
        struct QteOverlayState {
          float initialScale = 0.0f;
          float targetScale = 0.0f;
          DWORD startTick = 0;
          DWORD completionTick = 0; // when animation finished (cooldown)
          bool active = false;
          // Captured render data:
          std::string displayText;
          float centerX = 0.0f;
          float centerY = 0.0f;
          float baseFontHeight = 0.0f;
          float baseScale = 1.0f;
          uint8_t atlasSlot = 1;
          bool disableShadow = true;
          // Companion text (e.g. PROMPT_TEXT captured alongside PROMPT)
          std::string companionText;
          // Last frame the hint entry fed data into this overlay.
          DWORD lastSeenTick = 0;
        };
        static std::unordered_map<std::string, QteOverlayState> s_qteOverlay;

        // On restart: clear QTE overlay state so stale QTE animations
        // don't flash when hint rendering resumes.
        {
          const unsigned long qteResetTick =
              TextHook_GetObjectiveRuntimeResetTick();
          if (qteResetTick != 0 && now >= qteResetTick &&
              (now - qteResetTick) < 2500) {
            s_qteOverlay.clear();
          }
        }

        // QTE key sets — shared by hint loop and render loop.
        static const std::unordered_set<std::string> s_qteShrinkWhitelistShared = {
            "SKYWAY_HINT_RELOAD",
            "FLOOD_ENDING_QTE_2_PROMPT",
        };
        static const std::unordered_set<std::string> s_qtePulseOnlyShared = {
            "NML_HINT_X_KB",
            "NML_HINT_X",
            "SHIP_GRAVEYARD_HINT_RT",
        };

        // Clear per-frame companion text so stale data doesn't persist.
        for (auto &kv : s_qteOverlay) {
          kv.second.companionText.clear();
        }

        // QTE companion map: PROMPT_TEXT key → owning PROMPT key.
        static const std::unordered_map<std::string, std::string>
            s_qteCompanionMap = {
                {"FLOOD_ENDING_QTE_2_PROMPT_TEXT",
                 "FLOOD_ENDING_QTE_2_PROMPT"},
            };

        // --- Slot stability pre-scan ---
        // Update slot stability map from ALL hintEntries (including
        // non-renderable ones with alpha=0) so that the lastTick stays
        // alive as long as the key exists in the snapshot.  Without
        // this, keys that go alpha=0 stop reaching the gate → lastTick
        // stales → prune removes entry → slot jump goes undetected.
        struct SlotStability { int slot; DWORD lastTick; DWORD changedTick; };
        static std::unordered_map<std::string, SlotStability> s_slotStab;
        static DWORD s_slotStabPruneTick = 0;
        if ((now - s_slotStabPruneTick) > 5000) {
          s_slotStabPruneTick = now;
          for (auto it = s_slotStab.begin(); it != s_slotStab.end();) {
            if ((now - it->second.lastTick) > 10000)
              it = s_slotStab.erase(it);
            else
              ++it;
          }
        }
        for (int pi = 0; pi < (int)hintEntries.size(); ++pi) {
          const auto &pe = hintEntries[pi];
          if (pe.key.empty()) continue;
          const int ps = TextHook_HudSlotFromEvent(pe.nativeY, pe.nativeVirtualH, 0);
          if (ps <= 0 || ps >= LUI_HINT_SLOT_COUNT) continue;
          auto itPS = s_slotStab.find(pe.key);
          if (itPS != s_slotStab.end() && itPS->second.slot == ps) {
            itPS->second.lastTick = now; // keep alive even if not rendered
          }
        }

        // Render each hint entry
        for (int orderIndex = 0; orderIndex < (int)renderOrder.size(); ++orderIndex) {
          const int i = renderOrder[orderIndex];
          const auto &he = hintEntries[i];
          const std::string &key = he.key;

          if (!isHintRenderable(he)) continue;
          const int nativeSlot =
              TextHook_HudSlotFromEvent(he.nativeY, he.nativeVirtualH, 0);
          if (nativeSlot <= 0 || nativeSlot >= LUI_HINT_SLOT_COUNT) {
            continue;
          }
          const uint8_t sourceToken = hintSourceToken(he);
          if (isObjHudHintFamilyToken(sourceToken) &&
              key != "GAME_GET_TO_COVER") {
            logHintFamilySkip(he, nativeSlot, sourceToken);
            continue;
          }
          // Yield forced-center-status keys to OBJHUDHINT renderer which
          // uses screenWidth/2 center anchoring instead of native coords.
          if (!objHudHintCenterKeys.empty() &&
              objHudHintCenterKeys.find(key) != objHudHintCenterKeys.end()) {
            continue;
          }
          if (isPromptAuthorityLockedOut(he, nativeSlot)) {
            continue;
          }
          const std::string instanceSig = hintInstanceSig(he, nativeSlot);
          auto itBestInstance = bestByInstance.find(instanceSig);
          if (itBestInstance != bestByInstance.end() && itBestInstance->second != i)
            continue;
          auto itBestFamily = bestFamilyByKey.find(key);
          if (itBestFamily != bestFamilyByKey.end() &&
              itBestFamily->second != i) {
            continue;
          }
          auto itBestInteraction = bestInteractionByKey.find(key);
          if (itBestInteraction != bestInteractionByKey.end()) {
            // If a key has an interaction-slot winner, suppress all other same-key
            // center duplicates. Mixed native samples like FLOOD_LAUNCHER_MELEE
            // emit both a hidden center probe and a visible lower-slot prompt.
            if (itBestInteraction->second != i) {
              continue;
            }
          } else if (nativeSlot >= LUI_HINT_SLOT_ZOOM) {
            auto itBestCenter = bestCenterByKey.find(key);
            if (itBestCenter != bestCenterByKey.end() &&
                itBestCenter->second != i) {
              continue;
            }
          }
          if ((key == "PLATFORM_HOLD_TO_USE" ||
               key == "PLATFORM_HOLD_TO_DROP") &&
              hintSourceToken(he) != NATIVE_HUD_SOURCE_HUD560) {
            continue;
          }
          // Non-CG_DrawHudElem callers at default center position gate.
          // SEH callers (+0x34A835, +0x204816, +0x3D4B04, +0x328E42 etc.)
          // capture keys at the default center (nativeY≈240) without
          // knowing the real HudElem position.  These ghost entries can
          // have real alpha (≠0) and pass synth gates, causing 1-frame
          // center flashes before the correct source (CG_DrawHudElem or
          // OBJHUDHINT) provides proper coordinates.
          // Block unconditionally — the real render will arrive via
          // CG_Draw caller or OBJHUDHINT.
          // CLOCKWORK_HINT_DRILL exempt (no CG_Draw rendering).
          {
            const bool isCgDrawRange =
                (he.callerOffset >= IW6Offsets::CG_DrawHudElem_SP &&
                 he.callerOffset <= IW6Offsets::Profile::Rva_1F1B00);
            const bool isDrillKey =
                (key.rfind("CLOCKWORK_HINT_DRILL", 0) == 0);
            if (!isCgDrawRange && !isDrillKey &&
                std::fabs(he.nativeY - 240.0f) < 12.0f) {
              continue;
            }
          }
          // One-shot flash filter: require at least one refresh
          if (he.firstSeen == he.lastSeen) {
            if ((now - he.firstSeen) < 8) continue;
          }
          // No-native route-only entries are susceptible to load-time key storms.
          // Require a short stabilization interval before first render.
          if (!he.hasNativeParams && he.firstSeen != 0 &&
              (now - he.firstSeen) < 90) {
            continue;
          }

          // --- Transient HudElem gate (targeted) ---
          // Some keys (NIGHTVISION etc.) flash for 1-3 frames during engine
          // state transitions.  Apply a sustained-presence gate ONLY to keys
          // known to exhibit this transient pattern — not to all HUDHINT.
          {
            const bool isTransientProneKey =
                (key.find("NIGHTVISION") != std::string::npos);
            if (isTransientProneKey) {
              const DWORD entryAge =
                  (now >= he.firstSeen) ? (now - he.firstSeen) : 0;
              if (entryAge < 150) {
                continue;
              }
            }
          }

          // --- Slot transition stabilization gate ---
          // After a slot change, suppress rendering for kSlotStableMs to
          // prevent end-of-life HudElem position flashes (e.g. slot 4→3).
          {
            constexpr DWORD kSlotStableMs = 500;
            auto itSS = s_slotStab.find(key);
            if (itSS == s_slotStab.end()) {
              s_slotStab[key] = {nativeSlot, now, 0};
            } else if (itSS->second.slot != nativeSlot) {
              itSS->second.slot = nativeSlot;
              itSS->second.lastTick = now;
              itSS->second.changedTick = now;
              continue;
            } else {
              itSS->second.lastTick = now;
              if (itSS->second.changedTick != 0 &&
                  (now - itSS->second.changedTick) < kSlotStableMs) {
                continue; // still stabilizing after slot change
              }
            }
          }

          // === Position =======================================================
          float x = 0.0f, y = 0.0f;
          float fontHeight = 88.0f;
          float scale = 1.0f;
          bool customPosIsCenterAnchor = false;
          bool customPosIsRightAnchor = false;
          const uint8_t atlasSlot = KoreanAtlas::kAtlasSlotHeader;

          float baseColor[4] = {
              Clamp01(he.color[0]),
              Clamp01(he.color[1]),
              Clamp01(he.color[2]),
              Clamp01(he.color[3]),
          };

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

          x = g_ActiveArea.offsetX +
              nativeCoordToScreen(he.nativeX, he.nativeVirtualW,
                                  g_ActiveArea.width, g_ActiveArea.width * 0.5f);
          y = g_ActiveArea.offsetY +
              nativeCoordToScreen(he.nativeY, he.nativeVirtualH,
                                  g_ActiveArea.height,
                                  g_ActiveArea.height * 0.375f);

          // --- CG_DrawHudElem authoritative coordinate override ---
          // For keys where blind register probing fails to find the real
          // HudElem struct (e.g. CLOCKWORK_HINT_DRILL), use coordinates
          // captured directly from CG_DrawHudElem's hudelem_s pointer.
          // This provides raw x/y + alignOrg/alignScreen for proper
          // alignment normalization.
          bool cgDrawOverride = false;
          uint32_t cgHorzAlign = 0, cgVertAlign = 0;
          CgDrawHudElemCapture cgCap{};
          {
            if (TextHook_GetCgDrawCapture(key, cgCap, 2000)) {
              const uint32_t horzAlign = (cgCap.alignScreen >> 4) & 0xF;
              const uint32_t vertAlign = cgCap.alignScreen & 0xF;
              cgHorzAlign = horzAlign;
              cgVertAlign = vertAlign;
              const float scaleF = g_ActiveArea.height / 480.0f;

              // Convert raw 480-space coords to screen coords using
              // alignment anchors, matching IW6 ScrPlace logic.
              float anchorX = g_ActiveArea.offsetX;  // LEFT default
              switch (horzAlign) {
                case 2: case 7: // CENTER, CENTER_SAFEAREA
                  anchorX = g_ActiveArea.offsetX +
                            g_ActiveArea.width * 0.5f;
                  break;
                case 3: // RIGHT
                  anchorX = g_ActiveArea.offsetX + g_ActiveArea.width;
                  break;
              }
              float anchorY = g_ActiveArea.offsetY;  // TOP default
              switch (vertAlign) {
                case 2: case 7: // CENTER, CENTER_SAFEAREA
                  anchorY = g_ActiveArea.offsetY +
                            g_ActiveArea.height * 0.5f;
                  break;
                case 3: // BOTTOM
                  anchorY = g_ActiveArea.offsetY + g_ActiveArea.height;
                  break;
              }

              x = anchorX + cgCap.x * scaleF;
              y = anchorY + cgCap.y * scaleF;

              // Use CG_DrawHudElem fontScale as authoritative scale.
              scale = cgCap.fontScale;

              // Override RGB from CG_DrawHudElem (position/scale authority).
              // Alpha is NOT overridden — he.color[3] from R_AddCmdDrawText
              // may carry native pulse oscillation that CgDrawCapture doesn't.
              const float cgA =
                  (float)((cgCap.colorPacked >> 24) & 0xFFu) / 255.0f;
              if (cgA > 0.01f) {
                baseColor[0] =
                    (float)((cgCap.colorPacked >> 0) & 0xFFu) / 255.0f;
                baseColor[1] =
                    (float)((cgCap.colorPacked >> 8) & 0xFFu) / 255.0f;
                baseColor[2] =
                    (float)((cgCap.colorPacked >> 16) & 0xFFu) / 255.0f;
              }

              cgDrawOverride = true;

              static std::unordered_map<std::string, DWORD> s_cgOverLog;
              auto itOL = s_cgOverLog.find(key);
              if (itOL == s_cgOverLog.end() ||
                  (now - itOL->second) > 2000) {
                s_cgOverLog[key] = now;
                char olb[384];
                sprintf_s(olb,
                          "[CG-DRAW-OVERRIDE] key=%s raw=(%.1f,%.1f) "
                          "as=0x%02X hA=%u vA=%u screen=(%.1f,%.1f) "
                          "fs=%.3f cgA=%.2f",
                          key.c_str(), cgCap.x, cgCap.y,
                          cgCap.alignScreen, horzAlign, vertAlign,
                          x, y, cgCap.fontScale, cgA);
                LogToFile(olb);
              }
            }
          }

          // When CG-DRAW-OVERRIDE provided an authoritative anchor,
          // the screen position already accounts for alignment.  Skip
          // the generic center-snap / vertical nudge / center-anchor
          // that are designed for the default CENTER-aligned path.
          if (cgDrawOverride) {
            // Determine anchor mode from horizontal alignment.
            customPosIsCenterAnchor =
                (cgHorzAlign == 2 || cgHorzAlign == 7);
            customPosIsRightAnchor = (cgHorzAlign == 3);
            // No vertical nudge — CG-DRAW-OVERRIDE already positioned.
          } else {
            // Snap to exact center if native X is already near center.
            // This avoids per-key whitelisting while preserving intentionally
            // off-center hints (e.g. left/right split prompts).
            if (std::fabs(x - screenWidth * 0.5f) <
                g_ActiveArea.width * 0.05f) {
              x = screenWidth * 0.5f;
            }
            y += kHintVerticalNudge720 * scaleY;
            customPosIsCenterAnchor = true;
          }

          if (he.fontHeight > 0.01f) {
            if (he.nativeVirtualH > 0) {
              fontHeight = (he.fontHeight / (float)he.nativeVirtualH) * g_ActiveArea.height;
            } else {
              fontHeight = he.fontHeight;
            }
          } else {
            fontHeight = (44.0f / 480.0f) * g_ActiveArea.height;
          }
          if (!std::isfinite(fontHeight) || fontHeight < (28.0f * scaleY) ||
              fontHeight > (120.0f * scaleY)) {
            fontHeight = 60.0f * scaleY;
          }

          if (!cgDrawOverride) {
            scale = (std::isfinite(he.nativeYScale) && he.nativeYScale > 0.01f &&
                     he.nativeYScale < 6.0f)
                        ? he.nativeYScale
                        : 1.0f;
          }

          // SATFARM_INITIALIZING: center-aligned HudElem at virtual (320, 140)
          // in 640×480 space, fontScale=2.00.  HUDHINT captures the center base
          // (320, 240) without the -100 y-offset, so override to correct pos.
          if (key == "SATFARM_INITIALIZING") {
            y = g_ActiveArea.offsetY + g_ActiveArea.height * (140.0f / 480.0f);
            scale = 2.0f;
          }

          // GAME_GET_TO_COVER: "You are Hurt. Get to Cover!" center pulsing text.
          // Native alpha is always 0 (pulsing is shader-driven in the engine).
          // ObjRev captures with virtualH=0; HudElem y is the anchor point of
          // the element which does NOT correspond to where the native engine
          // renders the text (due to element alignment and parent anchoring).
          // Coordinate-based mapping is unreliable — hardcode to match the
          // visual position of the native English text (~27% from top, center).
          if (key == "GAME_GET_TO_COVER") {
            // Dormancy: nativeYScale≈2.0 distinguishes hidden/dormant state.
            // Also check nativeY≈240 (anchor at center in 640×480 space).
            const bool isDormant =
                (std::isfinite(he.nativeYScale) &&
                 std::fabs(he.nativeYScale - 2.0f) < 0.25f &&
                 std::fabs(he.nativeY - 240.0f) < 20.0f);
            if (isDormant) continue;
            // Hardcode position to match native English text visual position.
            x = screenWidth * 0.5f;
            y = g_ActiveArea.offsetY + g_ActiveArea.height * 0.27f;
            fontHeight = 44.0f * scaleY;
            baseColor[0] = 1.0f;
            baseColor[1] = 1.0f;
            baseColor[2] = 1.0f;
            float pulseT = (float)(now % 600) / 600.0f * 6.2831853f;
            baseColor[3] = 0.65f + 0.35f * sinf(pulseT);
            scale = 1.0f;
          }

          // Save raw native scale before any overrides — used for QTE
          // animation detection even when baseline overrides (e.g. DRILL) apply.
          const float rawNativeScale = scale;

          // (DRILL baseline scale override removed — synth-mode scale
          // normalization below handles zero-alpha probe scale instability.)

          constexpr DWORD kPromptStableLayoutHoldMs = 1200;
          constexpr DWORD kPromptAuthorityLayoutHoldMs = 60000;
          char plBuf[256];
          snprintf(plBuf, sizeof(plBuf), "%s|%u|%d",
                   key.c_str(), (unsigned int)he.callerOffset, nativeSlot);
          const std::string promptLayoutSig(plBuf);
          float finalAlpha = Clamp01(baseColor[3]);
          const float rawNativeAlpha = finalAlpha;
          bool synthAlpha = false;
          bool usedAuthorityLayout = false;
          const bool isPromptFamilyKey =
              looksLikeCenterPromptHintCached(he) || nativeSlot >= LUI_HINT_SLOT_ZOOM;

          // QTE shrink animation — capture render data for the standalone
          // overlay that runs AFTER the entry loop (entry lifecycle is too
          // short for smooth animation).
          bool qteScaleActive = false;
          bool qteAnimActive = false;
          float qteAnimProgress = 0.0f;
          float qteAnimScale = scale;
          float qtePulseMul = 1.0f;
          float qteInitScale = 0.0f;
          float qteTargetScale = 0.0f;
          // QTE overlay detection moved after displayText resolution — see below.
          bool suppressSynthRender = false;

          if (finalAlpha <= 0.01f) {
            const float maxRgb =
                (std::max)(baseColor[0], (std::max)(baseColor[1], baseColor[2]));
            if (looksLikeCenterPromptHintCached(he)) {
              auto itVisibleSupport = bestVisibleSupportByKey.find(key);
              if (itVisibleSupport != bestVisibleSupportByKey.end() &&
                  itVisibleSupport->second != i) {
                const auto &support = hintEntries[itVisibleSupport->second];
                const float supportAlpha =
                    (std::isfinite(support.color[3]) && support.color[3] > 0.0f)
                        ? Clamp01(support.color[3])
                        : 0.0f;
                const float supportMaxRgb =
                    (std::max)(support.color[0],
                               (std::max)(support.color[1], support.color[2]));
                if (supportAlpha > 0.01f || supportMaxRgb > 0.02f) {
                  baseColor[0] =
                      Clamp01(std::isfinite(support.color[0]) ? support.color[0]
                                                              : baseColor[0]);
                  baseColor[1] =
                      Clamp01(std::isfinite(support.color[1]) ? support.color[1]
                                                              : baseColor[1]);
                  baseColor[2] =
                      Clamp01(std::isfinite(support.color[2]) ? support.color[2]
                                                              : baseColor[2]);
                  if (supportAlpha > 0.01f) {
                    finalAlpha = supportAlpha;
                  }
                }
              }
            }
            if (finalAlpha <= 0.01f && looksLikeCenterPromptHintCached(he)) {
              auto itStable = s_promptStableLayout.find(promptLayoutSig);
              if (itStable != s_promptStableLayout.end() &&
                  now >= itStable->second.tick &&
                  (now - itStable->second.tick) <= kPromptStableLayoutHoldMs) {
                baseColor[0] = Clamp01(itStable->second.color[0]);
                baseColor[1] = Clamp01(itStable->second.color[1]);
                baseColor[2] = Clamp01(itStable->second.color[2]);
                finalAlpha = Clamp01(itStable->second.color[3]);
              }
            }
            if (finalAlpha <= 0.01f && looksLikeCenterPromptHintCached(he)) {
              auto itAuthority = s_promptAuthorityLayout.find(promptLayoutSig);
              if (itAuthority != s_promptAuthorityLayout.end() &&
                  now >= itAuthority->second.tick &&
                  (now - itAuthority->second.tick) <=
                      kPromptAuthorityLayoutHoldMs) {
                x = itAuthority->second.x;
                y = itAuthority->second.y;
                scale = itAuthority->second.scale;
                fontHeight = itAuthority->second.fontHeight;
                baseColor[0] = Clamp01(itAuthority->second.color[0]);
                baseColor[1] = Clamp01(itAuthority->second.color[1]);
                baseColor[2] = Clamp01(itAuthority->second.color[2]);
                finalAlpha = Clamp01(itAuthority->second.color[3]);
                usedAuthorityLayout = true;
              }
            }
            const float borrowedMaxRgb =
                (std::max)(baseColor[0], (std::max)(baseColor[1], baseColor[2]));
            bool suppressSynthRender = false;
            if (finalAlpha <= 0.01f && borrowedMaxRgb <= 0.02f) {
              bool allowAuthorityOnlyPromptSynthesis = false;
              // Hoist ObjRev alpha lookup — used by both Path A and B.
              const float cfgAlpha = TextHook_GetNativeCfgAlpha(key);
              if (isPromptFamilyKey) {
                auto itLock = s_promptAuthorityLockByKey.find(key);
                auto itPromptCount = promptInstanceCountByKey.find(key);
                const bool mixedPrompt =
                    (itPromptCount != promptInstanceCountByKey.end() &&
                     itPromptCount->second > 1);
                const bool lockMatchesCurrent =
                    (itLock != s_promptAuthorityLockByKey.end() &&
                     isPromptAuthorityLockLive(itLock->second) &&
                     he.callerOffset != 0 &&
                     he.callerOffset == itLock->second.callerOffset);
                const bool noVisibleSupport =
                    (bestVisibleSupportByKey.find(key) ==
                     bestVisibleSupportByKey.end());
                // SSOT 6F: drill keys may have only 1 instance (no
                // mixedPrompt), but the bootstrap created a valid lock.
                const bool isDrillBootstrapKey =
                    (key.rfind("CLOCKWORK_HINT_DRILL", 0) == 0);
                // For drill bootstrap keys, any live lock is sufficient —
                // all probes are zero-alpha with equivalent intent.
                // bestFamilyByKey winner changes between callers each frame,
                // so requiring exact callerOffset match causes intermittent
                // render gaps and lock expiry → position jumps.
                const bool lockLiveForDrill =
                    isDrillBootstrapKey &&
                    (itLock != s_promptAuthorityLockByKey.end() &&
                     isPromptAuthorityLockLive(itLock->second));
                // Entries with default native scale (1.0) haven't received
                // real HUD geometry — these are SLC ghost entries that
                // should not be authority-synthesized.  Drill bootstrap
                // keys are exempt: their probes always have zero-alpha
                // with unstable scale (0.4–2.5), so the gate would
                // block them from ever rendering.
                const bool hasRealNativeScale =
                    (std::isfinite(he.nativeYScale) &&
                     he.nativeYScale > 1.01f && he.nativeYScale < 6.0f);
                // Require ObjRev evidence that the HudElem has been
                // rendered with alpha>0 at least once.  Prevents false-
                // positive synth for always-hidden HudElems that the
                // engine keeps allocated with alpha=0 (e.g.
                // SCRIPT_NIGHTVISION_*).  Drill keys are exempt — their
                // probes always read alpha=0 by design.
                const bool hasObjRevAlphaEvidence =
                    (cfgAlpha > 0.01f);
                allowAuthorityOnlyPromptSynthesis =
                    (mixedPrompt || isDrillBootstrapKey) &&
                    (lockMatchesCurrent || lockLiveForDrill) &&
                    noVisibleSupport &&
                    (hasRealNativeScale || isDrillBootstrapKey) &&
                    (hasObjRevAlphaEvidence || isDrillBootstrapKey);
              }
              // ObjRev alpha fallback: if native probe has alpha=0 but
              // ObjRev scanner captured real alpha>0 from the render source
              // HudElem, allow synth with that alpha.  Restricted to callers
              // INSIDE CG_DrawHudElem (0x1F06B0..0x1F1B00) — only those
              // callers represent actual HudElem rendering.  Callers outside
              // CG_DrawHudElem (+0x34A835, +0x3D4B04, +0x328E42 etc.) fire
              // even for dormant HudElems, causing false-positive synth.
              if (!allowAuthorityOnlyPromptSynthesis) {
                const bool isCgDrawCaller =
                    (he.callerOffset >= IW6Offsets::CG_DrawHudElem_SP &&
                     he.callerOffset <= IW6Offsets::Profile::Rva_1F1B00);
                if (cfgAlpha > 0.01f && !he.korean.empty() &&
                    isCgDrawCaller) {
                  allowAuthorityOnlyPromptSynthesis = true;
                }
              }
              if (!allowAuthorityOnlyPromptSynthesis) {
                continue;
              }
              // If CG_DrawHudElem is actively rendering this key (real alpha
              // from the actual HudElem struct), suppress synth entries.
              // Synth is only needed when the engine does NOT process this
              // HudElem at all.  When CgDrawCapture exists with alpha > 0,
              // the CG-DRAW-OVERRIDE path handles rendering — synth would
              // create duplicates at wrong positions (ghost init data).
              {
                CgDrawHudElemCapture synthGuardCap{};
                if (TextHook_GetCgDrawCapture(key, synthGuardCap, 500)) {
                  const float cgAlpha =
                      (float)((synthGuardCap.colorPacked >> 24) & 0xFFu) /
                      255.0f;
                  if (cgAlpha > 0.01f) {
                    continue; // Real engine render active — skip synth.
                  }
                }
              }
              baseColor[0] = 1.0f;
              baseColor[1] = 1.0f;
              baseColor[2] = 1.0f;
            }
            // Block synth for non-CG_DrawHudElem callers.
            // SEH-captured entries (+0x34A835, +0x3D4B04, +0x328E42 etc.)
            // have unreliable default-center positions (320,240) and should
            // never synth-render — the real HudElem rendering comes from
            // CG_DrawHudElem callers (+0x1F06B0..+0x1F1B00) via the
            // CG-DRAW-OVERRIDE path.  CLOCKWORK_HINT_DRILL is exempt
            // because it never receives CG_DrawHudElem rendering.
            {
              const bool isCgDrawRange =
                  (he.callerOffset >= IW6Offsets::CG_DrawHudElem_SP &&
                   he.callerOffset <= IW6Offsets::Profile::Rva_1F1B00);
              const bool isDrillKey =
                  (key.rfind("CLOCKWORK_HINT_DRILL", 0) == 0);
              if (!isCgDrawRange && !isDrillKey) {
                continue;
              }
            }
            // Reverse-probed HUDHINT slots often surface with a valid native
            // geometry sample but a zero alpha field. Keep the raw sample in
            // the snapshot and synthesize only at render time.
            constexpr DWORD kSynthFadeInMs = 120;
            DWORD entryAge = now - he.firstSeen;
            // Try native fade interpolation first (reproduces engine pulsing).
            float nativeFadeA = TextHook_GetNativeFadeAlpha(key);
            float synthFade;
            if (nativeFadeA >= 0.0f) {
              // Use interpolated native alpha (from ObjRev fade params).
              synthFade = nativeFadeA;
            } else {
              // Fallback: simple fade-in.
              synthFade = (entryAge >= kSynthFadeInMs)
                              ? 1.0f
                              : ((float)entryAge / (float)kSynthFadeInMs);
            }
            finalAlpha = Clamp01(synthFade);
            synthAlpha = true;
            // Keep the authority lock alive while we're actively
            // synth-rendering this key (prevents 2s expiry).
            if (key == "CLOCKWORK_HINT_DRILL") {
              auto itLk = s_promptAuthorityLockByKey.find(key);
              if (itLk != s_promptAuthorityLockByKey.end()) {
                itLk->second.tick = now;
              }
            }
            // When authority layout was used, it already restored a
            // reliable scale/position from a prior visible render.
            // For pure-probe entries (e.g. DRILL) that never had a
            // visible frame: use baseline scale and lock position to
            // prevent frame-to-frame jumps from unstable winner selection.
            if (!usedAuthorityLayout) {
              // Use native cfg fontScale from ObjRev scanner if available;
              // else keep entry's own nativeYScale if reliable (> 1.01);
              // final fallback 1.0 for entries with default/unreliable scale.
              float cfgFs = TextHook_GetNativeCfgFontScale(key);
              if (cfgFs > 0.1f) {
                scale = cfgFs;
              } else if (scale <= 1.01f) {
                scale = 1.0f;
              }
              // else: keep entry's native scale (e.g. DRILL +0x204816 sx=2.5)
              struct SynthPosLock { float x, y; DWORD tick; };
              static std::unordered_map<std::string, SynthPosLock> s_synthPos;
              auto itSP = s_synthPos.find(key);
              if (itSP != s_synthPos.end() &&
                  (now - itSP->second.tick) < 5000) {
                x = itSP->second.x;
                y = itSP->second.y;
                itSP->second.tick = now;
              } else {
                s_synthPos[key] = {x, y, now};
              }
            }
          }
          if (nativeSlot == LUI_HINT_SLOT_MANTLE) {
            finalAlpha *= s_HintMantleVis;
          }

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
            // Tier 1 (base): re-resolved only when he.korean/he.english
            //   change or every 2 s as safety net for runtime-English drift.
            // Tier 2 (display+tail): recomputed only when tail text changes.
            struct SwapHintCached {
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
            static std::unordered_map<std::string, SwapHintCached> s_swapCache;
            static DWORD s_swapCachePruneTick = 0;

            char ckBuf[256];
            snprintf(ckBuf, sizeof(ckBuf), "%s|%u",
                     key.c_str(), (unsigned int)he.callerOffset);
            const std::string cacheKey(ckBuf);
            SwapHintCached &sc = s_swapCache[cacheKey];

            // ── Tier 1: base resolution (expensive, rarely needed) ───────
            if (sc.baseTick == 0 ||
                sc.inKorean != he.korean || sc.inEnglish != he.english ||
                (now - sc.baseTick) > 2000) {
              std::string cleanText = StripColorCodesSimple(he.korean);
              sc.engFallback = he.english;
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
                      he.korean, sc.engFallback);
              TextHook_ResolveLocalizedTemplateArgs(
                  key, sc.engFallback, sc.baseResolved);
              if (sc.baseResolved.empty() ||
                  StripColorCodesSimple(sc.baseResolved) == sc.engFallback ||
                  sc.baseResolved == key) {
                if (!cleanText.empty())
                  sc.baseResolved = cleanText;
              }
              if (!sc.engFallback.empty()) {
                std::string engDisp =
                    BindingResolver::ResolveBindingsInText(
                        sc.engFallback, sc.engFallback);
                sc.engSemanticSig = BuildPromptSemanticSig(engDisp);
              } else {
                sc.engSemanticSig.clear();
              }
              sc.inKorean = he.korean;
              sc.inEnglish = he.english;
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
                s_hintTailSticky;
            static DWORD s_hintTailPruneTick = 0;
            const DWORD kTailStickyMs = 500;

            std::string tailClean;
            {
              const float baseW = hudText.MeasureTextWidthEx(
                  sc.baseResolved, fontHeight, scale, atlasSlot);
              const float tailQueryX = customPosIsCenterAnchor
                  ? (x - baseW * 0.5f)
                  : (customPosIsRightAnchor ? (x - baseW) : x);
              std::string runtimeTail;
              bool haveTail = TextHook_GetHudRuntimeTail(
                  key, tailQueryX, y, he.callerOffset, runtimeTail);
              if (!haveTail) {
                auto itSticky = s_hintTailSticky.find(cacheKey);
                if (itSticky != s_hintTailSticky.end() &&
                    (now - itSticky->second.tick) <= kTailStickyMs) {
                  runtimeTail = itSticky->second.text;
                  haveTail = !runtimeTail.empty();
                }
              }
              if (haveTail) {
                tailClean =
                    TrimAscii(StripColorCodesSimple(runtimeTail));
                if (!tailClean.empty())
                  s_hintTailSticky[cacheKey] = {tailClean, now};
              }
              if ((now - s_hintTailPruneTick) > 2000) {
                s_hintTailPruneTick = now;
                for (auto it = s_hintTailSticky.begin();
                     it != s_hintTailSticky.end();) {
                  if ((now - it->second.tick) > 30000)
                    it = s_hintTailSticky.erase(it);
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
            if ((now - s_swapCachePruneTick) > 5000) {
              s_swapCachePruneTick = now;
              for (auto it = s_swapCache.begin();
                   it != s_swapCache.end();) {
                if ((now - it->second.tick) > 10000)
                  it = s_swapCache.erase(it);
                else ++it;
              }
            }
          } else {
            // ── Original path for non-tail-join hints ────────────────────
            std::string cleanText = StripColorCodesSimple(he.korean);
            std::string englishFallback = he.english;
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
                    he.korean, englishFallback);
            TextHook_ResolveLocalizedTemplateArgs(
                key, englishFallback, displayText);
            if (displayText.empty() ||
                StripColorCodesSimple(displayText) == englishFallback ||
                displayText == key) {
              if (!cleanText.empty()) {
                displayText = cleanText;
              }
            }
            displaySig = TrimAscii(displayText);
            displaySemanticSig = BuildPromptSemanticSig(displayText);
            if (!englishFallback.empty()) {
              std::string engDisplay =
                  BindingResolver::ResolveBindingsInText(
                      englishFallback, englishFallback);
              englishSemanticSig = BuildPromptSemanticSig(engDisplay);
            }
          }

          if (displayText.empty()) continue;
          const bool disableShadow =
              !shouldUseHudHintFamilyShadow(he, nativeSlot);
          if (!displaySig.empty() &&
              renderedHintSignatures.find(std::string("T:") + displaySig) !=
                  renderedHintSignatures.end()) {
            continue;
          }
          if (!displaySemanticSig.empty() &&
              renderedHintSignatures.find(std::string("S:") + displaySemanticSig) !=
                  renderedHintSignatures.end()) {
            continue;
          }
          if (!englishSemanticSig.empty() &&
              (renderedHintSignatures.find(std::string("E:") + englishSemanticSig) !=
                   renderedHintSignatures.end() ||
               renderedHintSignatures.find(std::string("S:") + englishSemanticSig) !=
                   renderedHintSignatures.end())) {
            continue;
          }

          {
            // 220ms EMA — smooths fade-in/out and dampens pulse oscillation.
            const std::string &alphaSig = key;
            HintAlphaSmoothState &st = s_hintAlphaSmooth[alphaSig];
            if (st.tick == 0 || now < st.tick || (now - st.tick) > 450) {
              st.alpha = finalAlpha;
              st.tick = now;
            } else {
              const DWORD dt = now - st.tick;
              const float blend = Clamp01((float)dt / 220.0f);
              st.alpha = st.alpha + (finalAlpha - st.alpha) * blend;
              st.tick = now;
            }
            finalAlpha = Clamp01(st.alpha);

            // Fade-in envelope for slot 2–3 hints (zoom, breath, etc.).
            // These LUI hint containers use animateToState("default", 500)
            // for a 500ms fade-in from alpha=0.  Slot 4+ (cursor/interaction)
            // and QTE/FAIL keys appear instantly — no fade-in.
            {
              const bool isFadeInSlot = (nativeSlot >= 2 && nativeSlot <= 3);
              const bool isFadeInExempt =
                  (key.find("_QTE") != std::string::npos ||
                   key.find("_FAIL") != std::string::npos ||
                   key.find("FAIL_") != std::string::npos);
              if (isFadeInSlot && !isFadeInExempt) {
                const DWORD entryAge =
                    (now >= he.firstSeen) ? (now - he.firstSeen) : 0;
                constexpr DWORD kFadeInMs = 500;
                if (entryAge < kFadeInMs) {
                  finalAlpha *= (float)entryAge / (float)kFadeInMs;
                }
              }
            }

            // Fade-out envelope: when the entry hasn't been seen recently,
            // quickly fade to 0 so Korean text doesn't linger after the
            // native English text has disappeared.
            {
              const DWORD sinceSeen = (now >= he.lastSeen) ? (now - he.lastSeen) : 0;
              constexpr DWORD kFadeOutStartMs = 50;
              constexpr DWORD kFadeOutDurationMs = 150;
              if (sinceSeen > kFadeOutStartMs) {
                const float fadeOut = 1.0f - Clamp01(
                    (float)(sinceSeen - kFadeOutStartMs) / (float)kFadeOutDurationMs);
                finalAlpha *= fadeOut;
              }
            }

            if (s_hintAlphaSmooth.size() > 256) {
              for (auto it = s_hintAlphaSmooth.begin();
                   it != s_hintAlphaSmooth.end();) {
                if (now >= it->second.tick &&
                    (now - it->second.tick) > 4000) {
                  it = s_hintAlphaSmooth.erase(it);
                } else {
                  ++it;
                }
              }
            }
          }
          if (finalAlpha <= 0.01f) continue;
          // Ghost synth: alpha flowed through pipeline+EMA, now skip render.
          if (suppressSynthRender) continue;

          bool unstablePromptEvidence = false;
          if (isPromptFamilyKey) {
            auto itPulseProbe = promptPulseProbeByKey.find(key);
            // ObjRev-backed synth is stable — don't suppress pulse.
            unstablePromptEvidence =
                synthAlpha && TextHook_GetNativeCfgAlpha(key) <= 0.01f;
            if (itPulseProbe != promptPulseProbeByKey.end()) {
              const PromptPulseProbe &probe = itPulseProbe->second;
              if (probe.sawZeroAlpha ||
                  ((probe.maxScale - probe.minScale) > 0.20f) ||
                  ((probe.maxY - probe.minY) > 8.0f) ||
                  probe.mixedCaller) {
                unstablePromptEvidence = true;
              }
            }
            auto itPromptCount = promptInstanceCountByKey.find(key);
            if (itPromptCount != promptInstanceCountByKey.end() &&
                itPromptCount->second > 1) {
              unstablePromptEvidence = true;
            }
          }

          if (isPromptFamilyKey && unstablePromptEvidence &&
              !usedAuthorityLayout &&
              (synthAlpha || rawNativeAlpha <= 0.01f)) {
            auto itStable = s_promptStableLayout.find(promptLayoutSig);
            if (itStable != s_promptStableLayout.end() &&
                now >= itStable->second.tick &&
                (now - itStable->second.tick) <= kPromptStableLayoutHoldMs) {
              x = itStable->second.x;
              y = itStable->second.y;
              // QTE: keep per-frame animated scale instead of cached baseline.
              if (!qteScaleActive && !qteAnimActive) {
                scale = itStable->second.scale;
                fontHeight = itStable->second.fontHeight;
              }
              baseColor[0] = Clamp01(itStable->second.color[0]);
              baseColor[1] = Clamp01(itStable->second.color[1]);
              baseColor[2] = Clamp01(itStable->second.color[2]);
              if (finalAlpha <= 0.01f) {
                finalAlpha = Clamp01(itStable->second.color[3]);
              }
            }
          }

          // Do not repopulate layout caches for QTE-suppressed keys —
          // they were purged on deactivation and must not be re-written
          // until a new QTE overlay starts (prevents post-QTE flash).
          const bool qteLayoutSuppressed = [&]() -> bool {
            auto itQo = s_qteOverlay.find(key);
            if (itQo != s_qteOverlay.end()) {
              if (itQo->second.active) return true;
              if (itQo->second.completionTick > 0 &&
                  (now - itQo->second.completionTick) < 3000)
                return true;
            }
            auto itComp = s_qteCompanionMap.find(key);
            if (itComp != s_qteCompanionMap.end()) {
              auto itQo2 = s_qteOverlay.find(itComp->second);
              if (itQo2 != s_qteOverlay.end()) {
                if (itQo2->second.active) return true;
                if (itQo2->second.completionTick > 0 &&
                    (now - itQo2->second.completionTick) < 3000)
                  return true;
              }
            }
            return false;
          }();
          if (isPromptFamilyKey && rawNativeAlpha > 0.01f &&
              !qteLayoutSuppressed) {
            PromptStableLayoutState &st = s_promptStableLayout[promptLayoutSig];
            st.x = x;
            st.y = y;
            // QTE: don't overwrite cached baseline with animated scale.
            if (!qteScaleActive && !qteAnimActive) {
              st.scale = scale;
              st.fontHeight = fontHeight;
            }
            st.color[0] = Clamp01(baseColor[0]);
            st.color[1] = Clamp01(baseColor[1]);
            st.color[2] = Clamp01(baseColor[2]);
            st.color[3] = Clamp01(finalAlpha);
            st.tick = now;

            PromptAuthorityLayoutState &auth =
                s_promptAuthorityLayout[promptLayoutSig];
            auth.callerOffset = he.callerOffset;
            auth.x = x;
            auth.y = y;
            if (!qteScaleActive && !qteAnimActive) {
              auth.scale = scale;
              auth.fontHeight = fontHeight;
            }
            auth.color[0] = Clamp01(baseColor[0]);
            auth.color[1] = Clamp01(baseColor[1]);
            auth.color[2] = Clamp01(baseColor[2]);
            auth.color[3] = Clamp01(finalAlpha);
            auth.tick = now;
          }

          if (s_promptStableLayout.size() > 128) {
            for (auto it = s_promptStableLayout.begin();
                 it != s_promptStableLayout.end();) {
              if (now >= it->second.tick &&
                  (now - it->second.tick) > 5000) {
                it = s_promptStableLayout.erase(it);
              } else {
                ++it;
              }
            }
          }
          if (s_promptAuthorityLayout.size() > 128) {
            for (auto it = s_promptAuthorityLayout.begin();
                 it != s_promptAuthorityLayout.end();) {
              if (now >= it->second.tick &&
                  (now - it->second.tick) > 120000) {
                it = s_promptAuthorityLayout.erase(it);
              } else {
                ++it;
              }
            }
          }
          // QTE alpha pulse: keys with "_QTE" in the name get a fast
          // breathing pulse matching the QTE_2 overlay parameters (3.5Hz,
          // 0.30–0.80).  These keys don't carry native pulse in he.color[3].
          if (key.find("_QTE") != std::string::npos &&
              key.find("QTE_4") == std::string::npos) {
            const float pulseT =
                sinf((float)now / 1000.0f * 3.5f * 6.2831853f);
            const float qteAlpha = 0.55f + 0.25f * pulseT; // 0.30–0.80
            finalAlpha *= qteAlpha;
          }

          const float familyRgbMax =
              (std::max)(baseColor[0], (std::max)(baseColor[1], baseColor[2]));
          if (!std::isfinite(baseColor[0]) || !std::isfinite(baseColor[1]) ||
              !std::isfinite(baseColor[2]) || familyRgbMax <= 0.08f) {
            logHintFamilyColorFix(he, nativeSlot, sourceToken, baseColor[0],
                                  baseColor[1], baseColor[2]);
            baseColor[0] = 1.0f;
            baseColor[1] = 1.0f;
            baseColor[2] = 1.0f;
          }



          float color[4] = {baseColor[0], baseColor[1], baseColor[2], finalAlpha};

          // === QTE overlay capture + skip ======================================
          // At this point displayText, x, y, fontHeight, scale, atlasSlot are
          // all resolved.  If this key has a pending QTE high-scale capture,
          // bootstrap the overlay and skip normal rendering.
          // Whitelist: only these keys get the QTE shrink overlay.
          if (isPromptFamilyKey &&
              s_qteShrinkWhitelistShared.count(key)) {
            // Keep lastSeenTick alive while the NHUD entry is visible.
            // Without this, the deactivation check (lastSeenTick > 0) never
            // fires for shrink keys, causing the overlay to run indefinitely.
            {
              auto itQoLS = s_qteOverlay.find(key);
              if (itQoLS != s_qteOverlay.end() && itQoLS->second.active) {
                itQoLS->second.lastSeenTick = now;
              }
            }
            float qteCapScale = 0.0f;
            unsigned long qteCapTick = 0;
            const float target = rawNativeScale;
            if (TextHook_ConsumeQteInitialHighScale(key, qteCapScale,
                                                    qteCapTick)) {
              if (std::isfinite(qteCapScale) && qteCapScale > 1.8f &&
                  std::isfinite(target) && target > 0.10f &&
                  target <= 1.60f) {
                auto itExist = s_qteOverlay.find(key);
                const bool canStart =
                    (itExist == s_qteOverlay.end()) ||
                    (!itExist->second.active &&
                     (now - itExist->second.completionTick) > 3000);
                if (canStart) {
                  QteOverlayState &qo = s_qteOverlay[key];
                  qo.initialScale = qteCapScale;
                  qo.targetScale = target;
                  qo.startTick = now;
                  qo.displayText = displayText;
                  qo.centerX = x;
                  qo.centerY = y;
                  qo.baseFontHeight = fontHeight;
                  qo.baseScale = scale;
                  qo.atlasSlot = atlasSlot;
                  qo.disableShadow = disableShadow;
                  qo.active = true;
                }
              }
            }
          }
          if (isPromptFamilyKey &&
              s_qtePulseOnlyShared.count(key)) {
            // Activate on first appearance — no scale threshold needed.
            auto itExist = s_qteOverlay.find(key);
            const bool canStart =
                (itExist == s_qteOverlay.end()) ||
                (!itExist->second.active &&
                 (now - itExist->second.completionTick) > 3000);
            if (canStart && finalAlpha > 0.01f) {
              QteOverlayState &qo = s_qteOverlay[key];
              qo.initialScale = rawNativeScale;
              qo.targetScale = rawNativeScale;
              qo.startTick = now;
              qo.displayText = displayText;
              qo.centerX = x;
              qo.centerY = y;
              qo.baseFontHeight = fontHeight;
              qo.baseScale = scale;
              qo.atlasSlot = atlasSlot;
              qo.disableShadow = disableShadow;
              qo.active = true;
            }
            auto itQo = s_qteOverlay.find(key);
            if (itQo != s_qteOverlay.end() && itQo->second.active) {
              // Update render data from this entry each frame it exists.
              itQo->second.displayText = displayText;
              itQo->second.centerX = x;
              itQo->second.centerY = y;
              itQo->second.baseFontHeight = fontHeight;
              itQo->second.baseScale = scale;
              itQo->second.atlasSlot = atlasSlot;
              itQo->second.disableShadow = disableShadow;
              itQo->second.lastSeenTick = now;
              // Skip normal rendering — standalone overlay handles it.
              continue;
            }
          }

          // === QTE companion capture ==========================================
          // If this key is a companion to an active QTE overlay (e.g.
          // PROMPT_TEXT → PROMPT), capture its displayText and skip normal
          // rendering so the QTE overlay renders them as a combined unit.
          {
            auto itComp = s_qteCompanionMap.find(key);
            if (itComp != s_qteCompanionMap.end()) {
              auto itQo = s_qteOverlay.find(itComp->second);
              if (itQo != s_qteOverlay.end() && itQo->second.active) {
                itQo->second.companionText = displayText;
                continue; // skip — QTE overlay renders combined text
              }
            }
          }

          // === Width measurement + center alignment ===========================
          float fullWidth = 0.0f;
          if (compactTailJoin && compactTailSplit != std::string::npos &&
              compactTailSplit < displayText.size()) {
            const std::string pre = displayText.substr(0, compactTailSplit);
            const std::string tail = displayText.substr(compactTailSplit);
            const float preW = hudText.MeasureTextWidthEx(
                pre, fontHeight, scale, atlasSlot);
            const float tailW = hudText.MeasureTextWidthEx(
                tail, fontHeight, scale, atlasSlot);
            compactTailGap =
                (std::min)(fontHeight * 0.16f,
                           (std::max)(1.5f * scaleY, fontHeight * 0.035f));
            fullWidth = preW + compactTailGap + tailW;
          } else {
            fullWidth =
                hudText.MeasureTextWidthEx(displayText, fontHeight, scale,
                                                   atlasSlot);
          }

          if (customPosIsCenterAnchor) {
            x = x - fullWidth * 0.5f;
          } else if (customPosIsRightAnchor) {
            x = x - fullWidth;
          }

          // === Safety net: suppress if QTE overlay owns this key =============
          // Also suppress for 500ms after completion to prevent flash-back.
          {
            auto qteStillSuppressed = [&](const QteOverlayState &qo) -> bool {
              if (qo.active) return true;
              return qo.completionTick > 0 &&
                     (now - qo.completionTick) < 500;
            };
            auto itQo = s_qteOverlay.find(key);
            if (itQo != s_qteOverlay.end() &&
                qteStillSuppressed(itQo->second))
              continue;
            auto itComp = s_qteCompanionMap.find(key);
            if (itComp != s_qteCompanionMap.end()) {
              auto itQo2 = s_qteOverlay.find(itComp->second);
              if (itQo2 != s_qteOverlay.end() &&
                  qteStillSuppressed(itQo2->second))
                continue;
            }
          }

          // === Render with key binding coloring ===============================
          queueHintText(displayText, x, y, scale, color, fontHeight,
                        disableShadow, atlasSlot, compactTailJoin,
                        compactTailSplit, compactTailGap, fullWidth);
          if (isPromptFamilyKey) {
            renderedPromptKeysThisFrame.insert(key);
            PromptFadeHoldState &fade = s_promptFadeHoldByKey[key];
            fade.displayText = displayText;
            fade.x = x;
            fade.y = y;
            fade.scale = scale;
            fade.fontHeight = fontHeight;
            fade.color[0] = color[0];
            fade.color[1] = color[1];
            fade.color[2] = color[2];
            fade.color[3] = color[3];
            fade.fadeAlpha = color[3]; // use rendered alpha, not raw native
            fade.disableShadow = disableShadow;
            fade.atlasSlot = atlasSlot;
            fade.compactTailJoin = compactTailJoin;
            fade.compactTailSplit = compactTailSplit;
            fade.compactTailGap = compactTailGap;
            fade.lastLiveTick = now;
            fade.fadeStartTick = 0;
          }
          auto itRenderLog = s_hintRenderLogTick.find(key);
          if (itRenderLog == s_hintRenderLogTick.end() ||
              (now - itRenderLog->second) > 900) {
            s_hintRenderLogTick[key] = now;
            char rbuf[512];
            sprintf_s(rbuf,
                      "[NHUD-RENDER] key=%s native=1 slot=%d x=%.1f y=%.1f a=%.2f seen=%lu synth=%d",
                      key.c_str(), nativeSlot, x, y, color[3],
                      (unsigned long)(now - he.lastSeen),
                      synthAlpha ? 1 : 0);
            LogToFile(rbuf);
          }
          {
            char dsBuf[256];
            snprintf(dsBuf, sizeof(dsBuf), "%s|%u|%d",
                     key.c_str(), (unsigned int)he.callerOffset, nativeSlot);
            const std::string diagSig(dsBuf);
            auto itDiag = s_hintDiagRenderLogTick.find(diagSig);
            if (itDiag == s_hintDiagRenderLogTick.end() ||
                (now - itDiag->second) > 1200) {
              s_hintDiagRenderLogTick[diagSig] = now;
              const DWORD seenAge =
                  (he.lastSeen != 0 && now >= he.lastSeen) ? (now - he.lastSeen) : 0;
              const DWORD nativeAge =
                  (he.lastNativeSeenTick != 0 && now >= he.lastNativeSeenTick)
                      ? (now - he.lastNativeSeenTick)
                      : 0;
              char dbuf[768];
              sprintf_s(
                  dbuf,
                  "[HUDHINT-RENDER] key=%s caller=+0x%X src=%u tok=%u slot=%d atlas=%u native=(%.1f,%.1f) pos=(%.1f,%.1f) scale=(%.3f,%.3f) h=%.1f a=%.2f seen=%lums nativeAge=%lums synth=%d sh=%d auth=%d qte=%d",
                  key.c_str(), he.callerOffset, (unsigned int)he.nativeSource,
                  (unsigned int)hintSourceToken(he), nativeSlot,
                  (unsigned int)atlasSlot, he.nativeX, he.nativeY, x, y,
                  he.nativeXScale, he.nativeYScale, fontHeight, color[3],
                  (unsigned long)seenAge, (unsigned long)nativeAge,
                  synthAlpha ? 1 : 0, disableShadow ? 0 : 1,
                  usedAuthorityLayout ? 1 : 0,
                  qteAnimActive ? 1 : 0);
              LogToFile(dbuf);
              if (qteAnimActive) {
                char qbuf[256];
                sprintf_s(
                    qbuf,
                    "[QTE-SHRINK] key=%s init=%.3f target=%.3f progress=%.2f scale=%.2f pulse=%.2f",
                    key.c_str(), qteInitScale, qteTargetScale, qteAnimProgress,
                    qteAnimScale, qtePulseMul);
                LogToFile(qbuf);
              }
            }
            if (s_hintDiagRenderLogTick.size() > 384) {
              for (auto it = s_hintDiagRenderLogTick.begin();
                   it != s_hintDiagRenderLogTick.end();) {
                if ((now - it->second) > 30000) {
                  it = s_hintDiagRenderLogTick.erase(it);
                } else {
                  ++it;
                }
              }
            }
          }
          if (!displaySig.empty()) {
            renderedHintSignatures.insert(std::string("T:") + displaySig);
          }
          if (!displaySemanticSig.empty()) {
            renderedHintSignatures.insert(std::string("S:") + displaySemanticSig);
          }
          if (!englishSemanticSig.empty()) {
            renderedHintSignatures.insert(std::string("E:") + englishSemanticSig);
          }
        }
        for (auto it = s_promptFadeHoldByKey.begin();
             it != s_promptFadeHoldByKey.end();) {
          if (it->second.lastLiveTick == 0 || now < it->second.lastLiveTick) {
            it = s_promptFadeHoldByKey.erase(it);
            continue;
          }
          if (renderedPromptKeysThisFrame.find(it->first) !=
              renderedPromptKeysThisFrame.end()) {
            ++it;
            continue;
          }
          if (it->second.fadeStartTick == 0) {
            it->second.fadeStartTick = it->second.lastLiveTick;
          }
          if (now < it->second.fadeStartTick ||
              (now - it->second.fadeStartTick) > kPromptFadeOutHoldMs) {
            it = s_promptFadeHoldByKey.erase(it);
            continue;
          }
          const float fadeT =
              Clamp01((float)(now - it->second.fadeStartTick) /
                      (float)kPromptFadeOutHoldMs);
          const float fadeMul = 1.0f - fadeT;
          const float fadeStartAlpha =
              (it->second.fadeAlpha > 0.05f) ? it->second.fadeAlpha : 0.45f;
          float fadeColor[4] = {
              Clamp01(it->second.color[0]),
              Clamp01(it->second.color[1]),
              Clamp01(it->second.color[2]),
              Clamp01(fadeStartAlpha * fadeMul),
          };
          if (fadeColor[3] > 0.01f && !it->second.displayText.empty()) {
            queueHintText(it->second.displayText, it->second.x, it->second.y,
                          it->second.scale, fadeColor, it->second.fontHeight,
                          it->second.disableShadow, it->second.atlasSlot,
                          it->second.compactTailJoin, it->second.compactTailSplit,
                          it->second.compactTailGap);
            const std::string fadeDisplaySig = TrimAscii(it->second.displayText);
            const std::string fadeSemanticSig =
                BuildPromptSemanticSig(it->second.displayText);
            if (!fadeDisplaySig.empty()) {
              renderedHintSignatures.insert(std::string("T:") + fadeDisplaySig);
            }
            if (!fadeSemanticSig.empty()) {
              renderedHintSignatures.insert(std::string("S:") + fadeSemanticSig);
            }
            auto itFadeLog = s_hintFadeRenderLogTick.find(it->first);
            if (itFadeLog == s_hintFadeRenderLogTick.end() ||
                (now - itFadeLog->second) > 180) {
              s_hintFadeRenderLogTick[it->first] = now;
              char fbuf[512];
              sprintf_s(
                  fbuf,
                  "[HUDHINT-FADE-RENDER] key=%s pos=(%.1f,%.1f) scale=%.3f h=%.1f a=%.2f age=%lums",
                  it->first.c_str(), it->second.x, it->second.y,
                  it->second.scale, it->second.fontHeight, fadeColor[3],
                  (unsigned long)(now - it->second.fadeStartTick));
              LogToFile(fbuf);
            }
          }
          ++it;
        }

        // === QTE Standalone Overlay Render ================================
        // Runs every frame regardless of hint entry lifecycle.  Entry data
        // was captured on the first frame the entry existed; we keep
        // rendering from that snapshot with smooth animation.
        {
          constexpr float kQteDurationMs = 1100.0f;
          // Grace period: deactivate when hint entries stop feeding data.
          constexpr DWORD kQteGraceMs = 300;
          for (auto it = s_qteOverlay.begin(); it != s_qteOverlay.end();) {
            QteOverlayState &qo = it->second;
            if (!qo.active || qo.displayText.empty()) {
              ++it;
              continue;
            }
            // Deactivate when hint entries stopped appearing.
            if (qo.lastSeenTick > 0 &&
                (now - qo.lastSeenTick) > kQteGraceMs) {
              qo.active = false;
              qo.completionTick = now;
              // Purge authority/stable layout caches for this key so the
              // 60-second cache cannot resurrect a stale alpha after the QTE
              // ends — this is the root cause of the post-QTE F flash.
              const std::string keyPrefix = it->first + "|";
              for (auto jt = s_promptAuthorityLayout.begin();
                   jt != s_promptAuthorityLayout.end();) {
                if (jt->first.rfind(keyPrefix, 0) == 0)
                  jt = s_promptAuthorityLayout.erase(jt);
                else
                  ++jt;
              }
              for (auto jt = s_promptStableLayout.begin();
                   jt != s_promptStableLayout.end();) {
                if (jt->first.rfind(keyPrefix, 0) == 0)
                  jt = s_promptStableLayout.erase(jt);
                else
                  ++jt;
              }
              ++it;
              continue;
            }
            const float elapsed =
                (now >= qo.startTick) ? (float)(now - qo.startTick) : 0.0f;
            float progress = elapsed / kQteDurationMs;
            const bool animDone = (progress >= 1.0f);
            if (animDone) progress = 1.0f;

            const bool isFloodQte =
                (it->first == "FLOOD_ENDING_QTE_2_PROMPT");
            const bool isPulseOnly = s_qtePulseOnlyShared.count(it->first) > 0;

            // --- Size computation per key type ---
            float sizeRatio;
            float sizePulseMul = 1.0f;
            if (isFloodQte || isPulseOnly) {
              // Fixed size at target + continuous ±12% pulse.
              sizeRatio = isFloodQte ? 2.0f : 1.0f;
              const float spT =
                  sinf(elapsed / 1000.0f * 2.5f * 6.2831853f);
              sizePulseMul = 1.0f + 0.12f * spT;
            } else {
              // SKYWAY etc: shrink from boosted initial → target over duration
              if (!animDone) {
                const float boosted = qo.initialScale * 1.5f;
                const float eased =
                    1.0f - (1.0f - progress) * (1.0f - progress);
                const float animScale =
                    boosted + (qo.targetScale - boosted) * eased;
                const float baseS =
                    (qo.targetScale > 0.01f) ? qo.targetScale : 1.0f;
                sizeRatio = (std::min)(animScale / baseS, 2.5f);
              } else {
                sizeRatio = 1.0f;
              }
            }
            const float fh =
                qo.baseFontHeight * sizeRatio * sizePulseMul;

            // Alpha: binding pulses (always), companion stays opaque.
            const float pulseT =
                sinf(elapsed / 1000.0f * 3.5f * 6.2831853f);
            const float bindAlpha = 0.55f + 0.25f * pulseT;
            const float compAlpha = 1.0f;

            // --- Split render: binding ([F]) animated, companion static ---
            std::string bindingText;
            std::string companionText;
            if (isFloodQte && !qo.companionText.empty()) {
              bindingText =
                  TrimAscii(StripColorCodesSimple(qo.displayText));
              while (!bindingText.empty() &&
                     (bindingText.back() == '\n' ||
                      bindingText.back() == '\r'))
                bindingText.pop_back();
              bindingText = TrimAscii(bindingText);
              companionText = qo.companionText;
            } else {
              bindingText = qo.displayText;
            }

            if (!companionText.empty() && !bindingText.empty()) {
              // === Two-part render: [F] animated, companion at base ===
              const float fhBase = qo.baseFontHeight;

              // Layout uses base fh (stable) so companion never jitters.
              const float bindWLayout = hudText.MeasureTextWidthEx(
                  bindingText, fhBase, qo.baseScale, qo.atlasSlot);
              const float compW = hudText.MeasureTextWidthEx(
                  companionText, fhBase, qo.baseScale, qo.atlasSlot);
              const float scaleBase =
                  hudText.CalculateFinalScale(fhBase, qo.baseScale);

              const float gap = fhBase * 0.15f;
              const float totalW = bindWLayout + gap + compW;
              const float startX = qo.centerX - totalW / 2.0f;

              // Actual binding width at animated fh (for centering within slot).
              KoreanRenderer::SetQteScaleMode(true);
              const float bindWActual = hudText.MeasureTextWidthEx(
                  bindingText, fh, qo.baseScale, qo.atlasSlot);
              const float scaleAnim =
                  hudText.CalculateFinalScale(fh, qo.baseScale);
              KoreanRenderer::SetQteScaleMode(false);

              // Binding renders centered within its layout slot.
              const float bindSlotCenter = startX + bindWLayout / 2.0f;
              const float bindRenderX = bindSlotCenter - bindWActual / 2.0f;
              // Companion at fixed position (never moves).
              const float compX = startX + bindWLayout + gap;

              // Vertical alignment: companion fixed at centerY,
              // binding centered vertically on the same midline.
              const float cellAnim = scaleAnim * 60.0f;
              const float cellBase = scaleBase * 60.0f;
              const float compY = qo.centerY;
              const float bindY =
                  qo.centerY + (cellBase - cellAnim) / 2.0f;

              // Binding: gold color, animated fontHeight, pulsing alpha
              float goldColor[4] = {1.0f, 0.85f, 0.2f, bindAlpha};
              KoreanRenderer::SetQteScaleMode(true);
              hudText.QueueText(
                  bindingText, bindRenderX, bindY, qo.baseScale,
                  goldColor, fh, 0, bindWActual, qo.disableShadow,
                  false, 1.5f, false, false, false, -1, 0.0f, false,
                  true, qo.atlasSlot);
              KoreanRenderer::SetQteScaleMode(false);

              // Companion: white color, base fontHeight, fixed alpha
              float whiteColor[4] = {1.0f, 1.0f, 1.0f, compAlpha};
              hudText.QueueText(
                  companionText, compX, compY, qo.baseScale, whiteColor,
                  fhBase, 0, compW, qo.disableShadow, false, 1.5f,
                  false, false, false, -1, 0.0f, false, true,
                  qo.atlasSlot);
            } else {
              // === Single text (SKYWAY_HINT_RELOAD etc) ===
              float qteColor[4] = {1.0f, 1.0f, 1.0f, bindAlpha};
              KoreanRenderer::SetQteScaleMode(true);
              const float w = hudText.MeasureTextWidthEx(
                  bindingText, fh, qo.baseScale, qo.atlasSlot);
              const float dx = qo.centerX - w / 2.0f;
              queueHintText(bindingText, dx, qo.centerY, qo.baseScale,
                            qteColor, fh, qo.disableShadow, qo.atlasSlot,
                            false, std::string::npos, 0.0f, w);
              KoreanRenderer::SetQteScaleMode(false);
            }

            // Diagnostic log (throttled).
            static DWORD s_qteOverlayLogTick = 0;
            if ((now - s_qteOverlayLogTick) > 100) {
              s_qteOverlayLogTick = now;
              char qbuf[512];
              sprintf_s(
                  qbuf,
                  "[QTE-OVERLAY] key=%s init=%.3f target=%.3f progress=%.2f "
                  "sizeRatio=%.2f sizePulse=%.3f fh=%.1f alpha=%.2f "
                  "companion=%d bind=\"%.20s\" comp=\"%.30s\"",
                  it->first.c_str(), qo.initialScale, qo.targetScale,
                  progress, sizeRatio, sizePulseMul, fh, bindAlpha,
                  companionText.empty() ? 0 : 1,
                  bindingText.c_str(), companionText.c_str());
              LogToFile(qbuf);
            }
            ++it;
          }
          // Prune completed overlays.
          // Use completionTick (not startTick) so the safety net entry
          // remains alive until well after any possible stray re-render.
          static DWORD s_qteOverlayPruneTick = 0;
          if ((now - s_qteOverlayPruneTick) > 5000) {
            s_qteOverlayPruneTick = now;
            for (auto it = s_qteOverlay.begin();
                 it != s_qteOverlay.end();) {
              if (!it->second.active &&
                  it->second.completionTick > 0 &&
                  (now - it->second.completionTick) > 60000) {
                it = s_qteOverlay.erase(it);
              } else {
                ++it;
              }
            }
          }
        }
      }
    } else if (!hintEntries.empty()) {
      static DWORD s_lastHintRenderGateLog = 0;
      if ((now - s_lastHintRenderGateLog) > 900) {
        s_lastHintRenderGateLog = now;
        char gbuf[256];
        sprintf_s(gbuf,
                  "[NHUD-RENDER-GATE] count=%d suppress=%d pause=%d trans=%d warm=%d credits=%d",
                  (int)hintEntries.size(), suppressHudOverlay ? 1 : 0,
                  gate.pauseMenu ? 1 : 0, gate.transitionBlack ? 1 : 0,
                  gate.warmupActive ? 1 : 0, gate.creditsActive ? 1 : 0);
        LogToFile(gbuf);
      }
    }

    // Phase 4: dedicated ObjHudHint pipeline renderer.
