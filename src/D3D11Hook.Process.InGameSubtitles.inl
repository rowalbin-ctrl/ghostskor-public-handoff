  // 1. Process In-Game Subtitles (from SEH hook queue)
  {
    // === Subtitle Ring Buffer Poll (checkpoint-restart recovery) ===
    // After checkpoint restart, engine restores ring buffer to checkpoint
    // state. We find ALL active subtitles at checkpoint time using:
    //   1. ctr-match: (entry[1]+entry[2]+1)%bufSize == ctr → last written entry
    //   2. ptr2 timing: ptr2[entry[0]] = {start_ms, end_ms} per entry
    //      Any entry whose end_ms > matched_start_ms is still active
    //
    // Slot: ptr1(+0)=ring, ptr2(+8)=timing, ptr3(+0x10)=textbuf
    // Ring entry (24B): [0]=seqIdx, [1]=textOff, [2]=textLen
    {
      static DWORD s_lastPoll = 0;
      static uint32_t s_prevCtr[3] = {0xFFFF, 0xFFFF, 0xFFFF};
      static bool s_initialized = false;

      // Helper lambda: read text from ptr3 circular buffer for a ring entry,
      // scanning backward to join multi-line subtitles (until ^2 speaker found)
      auto readSubtitleText = [](uintptr_t ptr1, uintptr_t ptr3,
                                 uint32_t bufSize, uint32_t capacity,
                                 int entryIdx) -> std::string {
        std::string joined;
        for (int back = 0; back < 4; back++) {
          int idx = entryIdx - back;
          if (idx < 0)
            idx += (int)capacity;
          uintptr_t ea = ptr1 + (uintptr_t)(idx * 24);
          if (!IsSafeRead((void *)ea, 24))
            break;
          const uint32_t *e = (const uint32_t *)ea;
          uint32_t off = e[1], len = e[2];
          if (len == 0 || len > 512)
            break;
          char buf[520];
          memset(buf, 0, sizeof(buf));
          if (off + len <= bufSize) {
            memcpy(buf, (const char *)(ptr3 + off), len);
          } else {
            uint32_t fp = bufSize - off;
            memcpy(buf, (const char *)(ptr3 + off), fp);
            memcpy(buf + fp, (const char *)ptr3, len - fp);
          }
          buf[len] = '\0';
          std::string line(buf);
          if (back == 0)
            joined = line;
          else
            joined = line + " " + joined;
          if (line.size() >= 2 && line[0] == '^' && line[1] == '2')
            break;
        }
        return joined;
      };

      if ((now - s_lastPoll) > 200) {
        s_lastPoll = now;
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        if (base) {
          for (int line = 0; line < 3; line++) {
            uintptr_t slotAddr = base + IW6Offsets::Profile::Rva_17BF4B8 + (uintptr_t)(line * 64);
            if (!IsSafeRead((void *)slotAddr, 64))
              continue;
            uint32_t capacity = *(uint32_t *)(slotAddr + 0x1C);
            uint32_t ctr48 = *(uint32_t *)(slotAddr + 0x30);
            uint32_t bufSize = *(uint32_t *)(slotAddr + 0x18);
            if (capacity == 0 || bufSize == 0)
              continue;

            uint32_t oldCtr = s_prevCtr[line];
            s_prevCtr[line] = ctr48;
            if (!s_initialized)
              continue;

            // Checkpoint restart = ctr decreased
            if (oldCtr == 0xFFFF || ctr48 >= oldCtr)
              continue;

            char diagBuf[256];
            sprintf_s(diagBuf, "[RINGPOLL] Restart L%d ctr=%u->%u",
                      line, oldCtr, ctr48);
            LogToFile(diagBuf);

            uintptr_t ptr1 = *(uintptr_t *)(slotAddr);
            uintptr_t ptr2 = *(uintptr_t *)(slotAddr + 8);
            uintptr_t ptr3 = *(uintptr_t *)(slotAddr + 0x10);
            if (!ptr1 || !ptr3)
              continue;
            if (!IsSafeRead((void *)ptr1, capacity * 24))
              continue;
            if (!IsSafeRead((void *)ptr3, bufSize))
              continue;

            // Step 1: Find the ctr-matched entry (last written at checkpoint)
            int matchIdx = -1;
            for (int i = 0; i < (int)capacity; i++) {
              uintptr_t ea = ptr1 + (uintptr_t)(i * 24);
              if (!IsSafeRead((void *)ea, 24))
                continue;
              const uint32_t *e = (const uint32_t *)ea;
              uint32_t off = e[1], len = e[2];
              if (len == 0 || len > 512)
                continue;
              if ((off + len + 1) % bufSize == ctr48) {
                matchIdx = i;
                break;
              }
            }

            if (matchIdx < 0) {
              LogToFile("[RINGPOLL] No entry matches restored ctr");
              continue;
            }

            // Step 2: Get ptr2 timing of matched entry to find checkpoint time
            uint32_t matchSeqIdx = 0;
            {
              uintptr_t ea = ptr1 + (uintptr_t)(matchIdx * 24);
              matchSeqIdx = *(const uint32_t *)ea; // entry[0] = seqIdx
            }
            uint32_t checkpointTime = 0; // start_ms of matched entry
            bool hasPtr2 = (ptr2 != 0);
            if (hasPtr2) {
              uintptr_t p2e = ptr2 + (uintptr_t)(matchSeqIdx * 8);
              if (IsSafeRead((void *)p2e, 8)) {
                const uint32_t *t = (const uint32_t *)p2e;
                checkpointTime = t[0]; // start_ms
              }
            }

            // Step 3: Inject matched entry
            std::string matchText =
                readSubtitleText(ptr1, ptr3, bufSize, capacity, matchIdx);
            if (!matchText.empty()) {
              bool inj = TextHook_PollInjectSubtitleByEnglish(matchText);
              char lb[768];
              sprintf_s(lb, "[RINGPOLL] ctr-match[%d] %s: \"%.250s\"",
                        matchIdx, inj ? "INJECT" : "skip", matchText.c_str());
              LogToFile(lb);
            }

            // Step 4: Check other entries via ptr2 timing — inject any
            // whose end_ms > checkpointTime (still active at checkpoint)
            if (hasPtr2 && checkpointTime > 0) {
              for (int i = 0; i < (int)capacity; i++) {
                if (i == matchIdx)
                  continue;
                uintptr_t ea = ptr1 + (uintptr_t)(i * 24);
                if (!IsSafeRead((void *)ea, 24))
                  continue;
                const uint32_t *e = (const uint32_t *)ea;
                uint32_t seqIdx = e[0], len = e[2];
                if (len == 0 || len > 512)
                  continue;
                uintptr_t p2e = ptr2 + (uintptr_t)(seqIdx * 8);
                if (!IsSafeRead((void *)p2e, 8))
                  continue;
                const uint32_t *t = (const uint32_t *)p2e;
                uint32_t startMs = t[0], endMs = t[1];
                // Active at checkpoint: started before and not yet ended
                if (startMs < checkpointTime && endMs > checkpointTime) {
                  std::string txt = readSubtitleText(
                      ptr1, ptr3, bufSize, capacity, i);
                  if (!txt.empty()) {
                    bool inj = TextHook_PollInjectSubtitleByEnglish(txt);
                    char lb[768];
                    sprintf_s(lb,
                              "[RINGPOLL] timing[%d] seq=%u t=[%u,%u] "
                              "chk=%u %s: \"%.250s\"",
                              i, seqIdx, startMs, endMs, checkpointTime,
                              inj ? "INJECT" : "skip", txt.c_str());
                    LogToFile(lb);
                  }
                }
              }
            }
          }
          s_initialized = true;
        }
      }
    }

    static DWORD lastD3DLog = 0;
    if (now - lastD3DLog > 2000) {
      lastD3DLog = now;
      // Trace log to confirm this function is running
      LogToFile("[D3D11] ProcessSubtitles RUNNING...");
    }

    std::vector<InGameSubtitleEntry> inGameSubs =
        TextHook_GetInGameSubtitleList();

    if (!inGameSubs.empty()) {
      if (g_FrameSwapChainDescValid) {
        const DXGI_SWAP_CHAIN_DESC &scDesc = g_FrameSwapChainDesc;
        float screenWidth = (float)scDesc.BufferDesc.Width;
        float screenHeight = (float)scDesc.BufferDesc.Height;

        // Engine-derived palette (best-effort): used for subtitle base tint and
        // to tame neon ^2 on speaker names.
        float dvarFriendlyName[4] = {0.90f, 1.00f, 0.90f, 0.70f};
        (void)TextHook_TryGetDvarVec4("friendlyNameFontColor", dvarFriendlyName);
        float dvarFriendlyNameGlow[4] = {0.0f, 0.30f, 0.0f, 1.0f};
        (void)TextHook_TryGetDvarVec4("friendlyNameFontGlowColor",
                                      dvarFriendlyNameGlow);
        {
          static DWORD s_lastStyleLog = 0;
          if (now - s_lastStyleLog > 3000) {
            s_lastStyleLog = now;
            constexpr float kBaseMul = 0.90f;
            float baseR = Clamp01(dvarFriendlyName[0] * kBaseMul);
            float baseG = Clamp01(dvarFriendlyName[1] * kBaseMul);
            float baseB = Clamp01(dvarFriendlyName[2] * kBaseMul);
            char buf[256];
            sprintf_s(buf,
                      "[D3D11] SUB style base=(%.3f %.3f %.3f) "
                      "name=(%.3f %.3f %.3f a=%.3f) glow=(%.3f %.3f %.3f a=%.3f)",
                      baseR, baseG, baseB,
                      dvarFriendlyName[0], dvarFriendlyName[1], dvarFriendlyName[2],
                      dvarFriendlyName[3],
                      dvarFriendlyNameGlow[0], dvarFriendlyNameGlow[1],
                      dvarFriendlyNameGlow[2], dvarFriendlyNameGlow[3]);
            LogToFile(buf);
          }
        }

        // Retrieve Native State using the new accessor (applies to top item)
        NativeSubtitleState state = TextHook_GetNativeSubtitleState();
        DWORD subtitleNow = TextHook_GetSubtitleClockNow();

        // Allow wider window to accommodate subtitle update cadence
        const DWORD nativeStateWindowMs = 2000;
        const bool nativeValid =
            TextHook_IsNativeSubtitleEnabled() && state.valid &&
            (now - state.lastUpdate < nativeStateWindowMs);

        if (!nativeValid) {
          // Diagnostic: detect native capture failure for the current subtitle
          static unsigned long lastDiagTime = 0;
          static std::string lastDiagKey;
          LastSubtitleDiag diag = TextHook_GetLastSubtitleDiag();
          if (diag.timestamp > 0 && (now - diag.timestamp) < 1500) {
            if (diag.key != lastDiagKey || (now - lastDiagTime) > 1500) {
              lastDiagKey = diag.key;
              lastDiagTime = now;
              LogToFile("[D3D11] Native capture missing for subtitle key='" +
                        diag.key + "' english='" + Utf8Preview(diag.english, 60) +
                        "...'");
            }
          }
        }

        constexpr int kSubtitleSlots = 4;
        float slotAnchorY = 0.0f;
        bool slotAnchorSet = false;
        const float screenScaleY = g_ActiveArea.height / 720.0f;
        const float minSlotAdvance = 20.0f * screenScaleY;
        const float maxSlotAdvance = 180.0f * screenScaleY;
        const float defaultSlotAdvance = 54.0f * screenScaleY;
        struct SubtitleSlotCache {
          float advance[kSubtitleSlots];
          float scaleY;
          bool initialized;
        };
        static SubtitleSlotCache s_SubtitleSlotCache = {
            {54.0f, 54.0f, 54.0f, 54.0f}, 1.0f, false};
        if (!s_SubtitleSlotCache.initialized ||
            fabsf(s_SubtitleSlotCache.scaleY - screenScaleY) > 0.01f) {
          for (int s = 0; s < kSubtitleSlots; ++s) {
            s_SubtitleSlotCache.advance[s] = defaultSlotAdvance;
          }
          s_SubtitleSlotCache.scaleY = screenScaleY;
          s_SubtitleSlotCache.initialized = true;
        }
        float slotAdvance[kSubtitleSlots];
        bool slotOccupied[kSubtitleSlots] = {false, false, false, false};
        for (int s = 0; s < kSubtitleSlots; ++s) {
          float adv = s_SubtitleSlotCache.advance[s];
          if (adv < minSlotAdvance) {
            adv = minSlotAdvance;
          } else if (adv > maxSlotAdvance) {
            adv = maxSlotAdvance;
          }
          slotAdvance[s] = adv;
        }

        std::vector<const InGameSubtitleEntry *> renderSubs;
        renderSubs.reserve(inGameSubs.size());
        for (const auto &entry : inGameSubs) {
          if (!entry.text.empty()) {
            renderSubs.push_back(&entry);
          }
        }
        std::stable_sort(renderSubs.begin(), renderSubs.end(),
                         [kSubtitleSlots](const InGameSubtitleEntry *a,
                            const InGameSubtitleEntry *b) {
                           int sa = a ? a->renderSlot : 0;
                           int sb = b ? b->renderSlot : 0;
                           if (sa < 0)
                             sa = 0;
                           if (sb < 0)
                             sb = 0;
                           if (sa >= kSubtitleSlots)
                             sa = kSubtitleSlots - 1;
                           if (sb >= kSubtitleSlots)
                             sb = kSubtitleSlots - 1;
                           return sa < sb;
                         });

        bool renderedSubtitle = false;
        float renderedTopY = screenHeight;
        float renderedBottomY = 0.0f;

          for (size_t i = 0; i < renderSubs.size(); ++i) {
            const auto &sub = *renderSubs[i];
            const std::string &inGameSub = sub.text;
            if (inGameSub.empty())
              continue;

          if (TextHook_IsMenuContext() || TextHook_IsMenuActiveRaw()) {
            if (inGameSub.find("킥") != std::string::npos &&
                inGameSub.find("블랙버드") != std::string::npos &&
                inGameSub.find("10") != std::string::npos) {
              continue;
            }
            if (inGameSub.find("Kick:") != std::string::npos &&
                inGameSub.find("Blackbird") != std::string::npos &&
                (inGameSub.find("ten minutes") != std::string::npos ||
                 inGameSub.find("ten minute") != std::string::npos)) {
              continue;
            }
          }

          // Base style - tuned to match native IW6 subtitle appearance
          // Resolution scaling: glyph pixel size must grow with screen size
          // so that text occupies the same proportion at any resolution.
          const float resMul = g_ActiveArea.height / 1080.0f;
          const float baseSubScale = 1.8f;
          float subScale = baseSubScale;
          float fontHeight = 72.0f * resMul;
          int style = 0;
          // Base color = white for dialogue text; ^2 override handles name color.
          float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

          // Subtitle position: centered at bottom of screen
          float safeMarginX = g_ActiveArea.width * 0.05f;
          float baseY = g_ActiveArea.offsetY + g_ActiveArea.height * 0.90f;
          float baseX = 0.0f; // 0 = center-aligned (handled in render loop)

          SubtitleNativeEntry lineState{};
          bool lineNativeValid = false;
          if (TextHook_IsNativeSubtitleEnabled()) {
            lineNativeValid =
                TextHook_GetSubtitleNativeState(sub.key, sub.english, lineState);
          }
          int renderSlot = sub.renderSlot;
          if (renderSlot < 0)
            renderSlot = 0;
          if (renderSlot >= kSubtitleSlots)
            renderSlot = kSubtitleSlots - 1;
          slotOccupied[renderSlot] = true;

          bool useGlobalNative =
              (!lineNativeValid && nativeValid && renderSlot == 0);
          bool anyNative = lineNativeValid || useGlobalNative;

          if (sub.nativeRemoveValid && sub.nativeRemoveTick > 0) {
            // Fail-open: ignore suspiciously-early native removal signals to
            // avoid hiding subtitles entirely when reverse binding is wrong.
            DWORD baseTick = sub.firstSeen > 0 ? sub.firstSeen : sub.timestamp;
            constexpr DWORD kMinNativeRemoveHoldMs = 220;
            if (sub.nativeRemoveTick > (baseTick + kMinNativeRemoveHoldMs) &&
                subtitleNow >= sub.nativeRemoveTick) {
              continue;
            }
          }

          if (lineNativeValid) {
            color[0] = lineState.color[0];
            color[1] = lineState.color[1];
            color[2] = lineState.color[2];
            color[3] = lineState.color[3];
            style = lineState.style;
          } else if (useGlobalNative) {
            color[0] = state.color[0];
            color[1] = state.color[1];
            color[2] = state.color[2];
            color[3] = state.color[3];
            style = state.style;

            // Use NativeSubtitleTracker for more accurate fade detection
            auto trackerState =
                NativeSubtitleTracker::SubtitleStateTracker::Instance()
                    .GetState();
            if (trackerState.valid) {
              // If tracker detected active fading, use its predicted alpha
              if (trackerState.isFadingIn || trackerState.isFadingOut) {
                float predictedAlpha =
                    NativeSubtitleTracker::SubtitleStateTracker::Instance()
                        .PredictAlpha(now);
                color[3] = predictedAlpha;

                // Log fade detection (throttled)
                static DWORD lastFadeLog = 0;
                if (now - lastFadeLog > 500) {
                  lastFadeLog = now;
                  char fadeLogBuf[128];
                  sprintf_s(fadeLogBuf,
                            "[D3D11] Native Fade: %s alpha=%.2f->%.2f",
                            trackerState.isFadingIn ? "IN" : "OUT",
                            state.color[3], predictedAlpha);
                  LogToFile(fadeLogBuf);
                }
              }
            }
          }

          // Stack spacing (tighter, matching native IW6 subtitle stacking)
          // Subtitle timing ??SEH fires ONCE per key (engine caches result).
          // We CANNOT detect when the engine removes a subtitle.
          // Strategy:
          //   stackIndex > 0 (pushed by newer subtitle): fadeOut from pushAnimStart
          //   stackIndex == 0 (newest): fadeOut from firstSeen + MsgTime(5s)
          const float fadeInMs = 750.0f;
          float fadeOutMs = 500.0f;
          float totalLifeMs = 5000.0f; // default con_gameMsgWindow2MsgTime
          if (sub.fadeOutMs > 50 && sub.fadeOutMs < 5000) {
            fadeOutMs = (float)sub.fadeOutMs;
          }
          if (sub.msgTimeMs >= 400 && sub.msgTimeMs <= 30000) {
            totalLifeMs = (float)sub.msgTimeMs;
          }
          float fadeStartMs = totalLifeMs - fadeOutMs;
          if (fadeStartMs < 0.0f)
            fadeStartMs = 0.0f;

          DWORD firstAge =
              (sub.firstSeen > 0) ? (subtitleNow - sub.firstSeen) : 0;

          float alpha = 1.0f;
          float yOffset = 0.0f;
          float scaleMul = 1.0f;

          if (!anyNative) {
            auto clamp01 = [](float v) -> float {
              if (v < 0.0f) return 0.0f;
              if (v > 1.0f) return 1.0f;
              return v;
            };

            auto easeOut = [](float v) -> float {
              v = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
              return v * (2.0f - v);
            };

            float t = (float)firstAge;

            // FadeIn (from firstSeen)
            if (t < fadeInMs) {
              alpha = t / fadeInMs;
              yOffset = 10.0f * (1.0f - easeOut(t / fadeInMs));
              scaleMul = 0.97f + 0.03f * easeOut(t / fadeInMs);
            }

            if (!sub.nativeRemoveValid) {
              // Fallback-only fadeOut logic.
              if (t > fadeStartMs) {
                float fadeProgress = (t - fadeStartMs) / fadeOutMs;
                float fadeAlpha = 1.0f - fadeProgress;
                fadeAlpha = clamp01(fadeAlpha);
                if (alpha > fadeAlpha) alpha = fadeAlpha;
                if (fadeProgress > 0.0f) {
                  yOffset = 6.0f * easeOut(clamp01(fadeProgress));
                  scaleMul = 1.0f - 0.02f * easeOut(clamp01(fadeProgress));
                }
              }
            }

            alpha = clamp01(alpha);

            // Skip fully transparent entries entirely
            if (alpha <= 0.0f) continue;
          }

          // No stackAlpha dimming ??each subtitle at full alpha for its state
          color[3] *= alpha;
          if (!anyNative) {
            subScale *= scaleMul;
          }
          // Keep wrap layout stable across fade-in/out scale animation.
          const float wrapScale = baseSubScale;

          // Typewriter sync from native per-line capture
          std::string renderSub = inGameSub;
          if (lineNativeValid && lineState.totalChars > 0 &&
              lineState.visibleChars > 0 &&
              lineState.visibleChars < lineState.totalChars) {
            float ratio =
                (float)lineState.visibleChars /
                (float)lineState.totalChars;
            size_t totalKor = CountUtf8Chars(inGameSub);
            size_t visibleKor = (size_t)ceilf((float)totalKor * ratio);
            if (visibleKor < 1)
              continue;
            if (visibleKor < totalKor) {
              renderSub = Utf8PrefixByChars(inGameSub, visibleKor);
            }
          }

          // Word-wrap if text is too wide for screen
          // Wrap within safe area (subtract left safe margin, plus right margin)
          float maxTextWidth =
              ComputeSubtitleWrapWidth(g_ActiveArea.width, g_ActiveArea.height, false);
          std::vector<std::string> wrappedLines =
              WrapKoreanText(renderSub, maxTextWidth, fontHeight, wrapScale,
                             false);
          float lineSpacing = fontHeight * (subScale * 0.35f);

          // Fixed slot stacking: do not compact on middle-slot removal.
          if (renderSlot == 0 || !slotAnchorSet) {
            slotAnchorY = baseY;
            slotAnchorSet = true;
          }
          float targetOffset = 0.0f;
          for (int s = 0; s < renderSlot; ++s) {
            targetOffset += slotAdvance[s];
          }
          float targetY = slotAnchorY - targetOffset;
          float subY = targetY;

          // Push-up animation from previous fixed slot to current slot.
          if (sub.pushAnimStart > 0 && renderSlot > 0) {
            const float pushDurationMs = 250.0f; // con_gameMsgWindow2ScrollTime
            DWORD pushAge = subtitleNow - sub.pushAnimStart;
            if (pushAge < (DWORD)pushDurationMs) {
              float progress = (float)pushAge / pushDurationMs;
              progress = progress * (2.0f - progress); // ease-out
              float prevOffset = targetOffset - slotAdvance[renderSlot - 1];
              float prevY = slotAnchorY - prevOffset;
              subY = prevY + (targetY - prevY) * progress;
            }
          }
          if (!anyNative) {
            subY += yOffset;
          }

          DrawStylePatch subtitleStylePatch{};
          subtitleStylePatch.overrideColorCode1 = true;
          subtitleStylePatch.code1R = 1.0f;
          subtitleStylePatch.code1G = 0.361f;
          subtitleStylePatch.code1B = 0.361f;
          subtitleStylePatch.overrideColorCode2 = true;
          subtitleStylePatch.code2R = dvarFriendlyName[0];
          subtitleStylePatch.code2G = dvarFriendlyName[1];
          subtitleStylePatch.code2B = dvarFriendlyName[2];
          subtitleStylePatch.applyGlow = true;
          subtitleStylePatch.glowR = 0.0f;
          subtitleStylePatch.glowG = 0.0f;
          subtitleStylePatch.glowB = 0.0f;
          subtitleStylePatch.glowA = 0.95f;
          // Render wrapped lines (bottom-up: last wrapped line at subY)
          for (size_t wl = 0; wl < wrappedLines.size(); ++wl) {
            std::string wLine = wrappedLines[wrappedLines.size() - 1 - wl];
            wLine = EnsureNameHasResetCode(wLine);
            float wWidth = KoreanRenderer::MeasureTextWidthEx(
                wLine, fontHeight, subScale);
            float wX = baseX > 0.0f ? baseX : g_ActiveArea.offsetX + (g_ActiveArea.width - wWidth) * 0.5f;
            float clampMarginX = (std::max)(12.0f, safeMarginX);
            float minX = g_ActiveArea.offsetX + clampMarginX;
            float maxX = g_ActiveArea.offsetX + g_ActiveArea.width - clampMarginX - wWidth;
            if (maxX < minX) {
              maxX = minX;
            }
            if (wX < minX) {
              wX = minX;
            }
            if (wX > maxX) {
              wX = maxX;
            }
            float wY = subY - (float)wl * lineSpacing;
            renderedSubtitle = true;
            if (wY < renderedTopY) {
              renderedTopY = wY;
            }
            if (wY > renderedBottomY) {
              renderedBottomY = wY;
            }

            QueueSubtitleReadabilityPass(wLine, wX, wY, subScale, fontHeight,
                                         wWidth, color[3], dvarFriendlyName,
                                         dvarFriendlyNameGlow);

            // Main subtitle text with shadow and native-style glow.
            KoreanRenderer::QueueText(wLine, wX, wY, subScale, color, fontHeight,
                                      style, wWidth, false, false, 2.0f, false,
                                      true, false, -1, 0.0f, false, false, 0,
                                      &subtitleStylePatch);
          }

          // Reserve stack height using full text (not typewriter prefix),
          // so vertical spacing stays stable during character reveal.
          std::vector<std::string> layoutLines =
              WrapKoreanText(inGameSub, maxTextWidth, fontHeight, wrapScale,
                             false);
          size_t lineCount = layoutLines.empty() ? wrappedLines.size()
                                                 : layoutLines.size();
          if (lineCount < 1) {
            lineCount = 1;
          }
          float blockHeight = 0.0f;
          if (lineCount > 1) {
            blockHeight = (float)(lineCount - 1) * lineSpacing;
          }
          float baseAdvance = fontHeight * (subScale * 0.40f);
          float entryAdvance =
              (std::max)(baseAdvance, lineSpacing * 1.02f) + blockHeight;
          if (entryAdvance < 8.0f) {
            entryAdvance = 8.0f;
          }
          if (entryAdvance < minSlotAdvance) {
            entryAdvance = minSlotAdvance;
          } else if (entryAdvance > maxSlotAdvance) {
            entryAdvance = maxSlotAdvance;
          }
          slotAdvance[renderSlot] = entryAdvance;

          // Log source (throttled)
          if (renderSlot == 0) {
            static DWORD lastSourceLog = 0;
            static std::string lastSourceKey;
            const char *source =
                lineNativeValid ? "native_draw"
                : (useGlobalNative ? "native_global" : "fallback");
            if (now - lastSourceLog > 2000 || lastSourceKey != sub.key) {
              lastSourceLog = now;
              lastSourceKey = sub.key;
              std::stringstream ss;
              ss << "[D3D11] SUBTITLE source=" << source << " key='" << sub.key
                 << "'";
              if (!anyNative) {
                ss << " msg=" << sub.msgTimeMs << " fade=" << sub.fadeOutMs;
                ss << " age=" << firstAge;
                ss << std::fixed << std::setprecision(2) << " alpha=" << alpha;
                ss.unsetf(std::ios::floatfield);
                ss << " life=" << totalLifeMs << " fadeStart=" << fadeStartMs;
              }
              LogToFile(ss.str());
            }
          }
        }

        for (int s = 0; s < kSubtitleSlots; ++s) {
          float v = slotAdvance[s];
          if (!slotOccupied[s]) {
            v = s_SubtitleSlotCache.advance[s];
          }
          if (v < minSlotAdvance) {
            v = minSlotAdvance;
          } else if (v > maxSlotAdvance) {
            v = maxSlotAdvance;
          }
          s_SubtitleSlotCache.advance[s] = v;
        }
        s_SubtitleSlotCache.scaleY = screenScaleY;
        s_SubtitleSlotCache.initialized = true;

        // Track subtitle Y region for hint clipping prevention
        if (renderedSubtitle) {
          g_SubtitleActiveTopY = renderedTopY - 14.0f * screenScaleY;
          g_SubtitleActiveBottomY = renderedBottomY + 22.0f * screenScaleY;
          g_SubtitleActiveTime = now;
        }

        // Log successful queue (throttled) - FIXED: changed < to >
        if (now - lastD3DLog > 2000 && !inGameSubs.empty() &&
            !inGameSubs.front().text.empty()) {
          lastD3DLog = now;
          LogToFile("[D3D11] Queued In-Game Subtitle: " +
                    inGameSubs.front().text.substr(0, 20) + "...");
        }
      }
    }
  }

  // ==========================================================================

