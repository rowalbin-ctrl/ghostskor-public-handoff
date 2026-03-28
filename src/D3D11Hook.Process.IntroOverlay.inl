  // 2. Process INTRO OVERLAY (Mission intro screens)
  // Fully native-driven: position, color, alpha, fontScale from engine's
  // per-frame HudElem state (RBX struct in IntroRenderFn).
  //
  // Key insight: HudElem +0x30 stores TARGET for *overtime calls,
  // but CURRENT value for direct writes (quick_pulse, hud_offset).
  // Alpha flickering (quick_pulse) and position jitter (offset_thread)
  // use direct writes => captured in struct per-frame => use as-is.
  // Only drift's moveovertime(12)/changefontscaleovertime(6)/fadeovertime(2)
  // need client-side interpolation.
  //
  // GSC 999.gsc behavior:
  //   create_chyron_text: color=(0.85,0.93,0.92), fontScale=1.4, alpha=0
  //   title_line: quick_pulse after 0.8s, 90% offset_thread(-30..30, -8..8)
  //   sub_line:   typewriter setpulsefx(30,50000,700), quick_pulse, alpha=1
  //   line[2],[3]: color=(0.68,0.744,0.736)
  //   name_drift: moveovertime(12), changefontscaleovertime(6),
  //              color white->orig 2s, glow white->(0.106,0.116,0.115) 2s,
  //              alpha 1->0.8 2s, hold, 0.8->0 2s, destroy at 6s
  //   faze_out:  1s after drift, hud_destroy(0.06) on lines[0,1,2]
  //   offset_thread: direct x/y snap then restore (50-200ms hold)
  //   quick_pulse: self.alpha = random(0.7*base, base) every 50ms
  // ==========================================================================
  {
    IntroOverlaySnapshot introSnap = TextHook_GetIntroOverlaySnapshot();

    // Animation state statics ??must be in scope for both rendering and cleanup
    enum IntroPhase { PH_VISIBLE=0, PH_FAZEOUT, PH_DRIFT };
    struct IntroLineAnim {
      IntroPhase phase;
      DWORD phaseStart;
      DWORD nativeFirstSeen;
      float initialX, initialY, initialFS;
      float origR, origG, origB;
      float driftTargetX, driftTargetY;
      float driftTargetFS;
      float prePhaseAlpha;
      float lastNativeAlpha;
      bool initialized;
    };
    static std::map<uint32_t, IntroLineAnim> lineAnims;
    static DWORD lastIntroStartTime = 0;
    static DWORD globalDriftStartTime = 0;
    static uint32_t driftLeaderSlc = 0;

    bool pauseMenuLikely = TextHook_IsPauseMenuLikely();
    if (introSnap.active && !introSnap.lines.empty() && !pauseMenuLikely) {
      if (g_FrameSwapChainDescValid) {
        const DXGI_SWAP_CHAIN_DESC &scDesc = g_FrameSwapChainDesc;
        float screenWidth  = (float)scDesc.BufferDesc.Width;
        float screenHeight = (float)scDesc.BufferDesc.Height;
        float scaleF = g_ActiveArea.height / 480.0f;

        UpdateScreenPlacement();

        const float safeAreaFrac = 0.045f;
        EngineSafeArea sa = TextHook_GetEngineSafeArea();
        float safeOffX, safeOffY;
        if (s_scrPlaceValid) {
          safeOffX = s_scrPlaceViewMin[0];
          safeOffY = s_scrPlaceViewMin[1];
        } else if (sa.x > 1.0f && sa.x < screenWidth * 0.5f) {
          safeOffX = sa.x;
          safeOffY = (sa.y > 1.0f && sa.y < screenHeight * 0.5f)
                         ? sa.y : (screenHeight * safeAreaFrac);
        } else {
          safeOffX = screenWidth * safeAreaFrac;
          safeOffY = screenHeight * safeAreaFrac;
        }

        {
          static bool s_scrPlaceLogged = false;
          if (s_scrPlaceValid && !s_scrPlaceLogged) {
            s_scrPlaceLogged = true;
            char spBuf[256];
            sprintf_s(spBuf,
                      "[SCRPLACE] viewMin=(%.2f, %.2f) safeOff=(%.2f, %.2f) scaleF=%.4f",
                      s_scrPlaceViewMin[0], s_scrPlaceViewMin[1],
                      safeOffX, safeOffY, scaleF);
            LogToFile(spBuf);
          }
        }

        const float resMul = g_ActiveArea.height / 1080.0f;
        float baseFontH = 78.0f * resMul;
        const float introScaleMul = 1.3f;

        // Throttled diagnostic log
        static DWORD lastIntroLog = 0;
        if (now - lastIntroLog > 1500) {
          lastIntroLog = now;
          auto allNative = TextHook_GetAllNativeIntroStates();
          std::string nativeInfo;
          for (const auto &ns : allNative) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     " 0x%X(a=%.2f,rgb=%.2f/%.2f/%.2f,x=%.0f,y=%.0f,fs=%.2f,ha=%u,va=%u,stale=%u)",
                     ns.slcIdx, ns.a, ns.r, ns.g, ns.b,
                     ns.x, ns.y, ns.fontScale,
                     ns.horzAlign, ns.vertAlign,
                     (unsigned)(now - ns.lastTick));
            nativeInfo += buf;
          }
          LogToFile("[D3D11] INTRO " +
                    std::to_string(introSnap.lines.size()) + " lines," +
                    std::to_string(allNative.size()) + " native:" + nativeInfo);
        }

        // =================================================================
        // Animation state: minimal ??only what native struct can't provide.
        // VISIBLE: 100% native alpha, position, color. No heuristics.
        // DRIFT:   interpolate position/scale (moveovertime/changefontscaleovertime),
        //          synthesize alpha (fadeovertime target not interpolated),
        //          synthesize color (white->original over 2s),
        //          synthesize glow (white->(0.106,0.116,0.115) over 2s).
        // FAZEOUT: 60ms fade (GSC: hud_destroy(0.06)) for non-drift lines.
        // =================================================================

        // Reset on new intro session
        if (introSnap.startTime != lastIntroStartTime) {
          lineAnims.clear();
          lastIntroStartTime = introSnap.startTime;
          globalDriftStartTime = 0;
          driftLeaderSlc = 0;
        }

        const float centerX = screenWidth * 0.5f;
        const float centerBandX = screenWidth * 0.12f;
        const float centerBandTop = screenHeight * 0.35f;
        const float centerBandBottom = screenHeight * 0.65f;

        for (const auto &srcLine : introSnap.lines) {
          if (srcLine.text.empty()) continue;

          NativeIntroRenderState nrs;
          bool hasNative = (srcLine.slcIdx >= 0) &&
                           TextHook_GetNativeIntroRender(srcLine.slcIdx, nrs);
          if (!hasNative) continue;

          // Text processing
          std::string text = ReplaceFakeIntroSeconds(
              srcLine.text, now, introSnap.startTime);
          text = StripColorCodesSimple(text);
          text = TrimAscii(text);
          if (IsIntroPlaceholderLine(text)) continue;

          // Center-card detection (YEARSLATER, 12_YEARS, etc.)
          float approxLineX = 0.0f, approxLineY = 0.0f;
          switch (nrs.horzAlign) {
            case 1:  approxLineX = centerX + nrs.x * scaleF; break;
            case 2:  approxLineX = (screenWidth - safeOffX) + nrs.x * scaleF; break;
            default: approxLineX = safeOffX + nrs.x * scaleF; break;
          }
          switch (nrs.vertAlign) {
            case 1:  approxLineY = screenHeight * 0.5f + nrs.y * scaleF; break;
            case 2:  approxLineY = (screenHeight - safeOffY) + nrs.y * scaleF; break;
            default: approxLineY = safeOffY + nrs.y * scaleF; break;
          }

          auto IsRegularIntroLayoutKey = [&](const std::string &key) {
            auto EndsWith = [&](const char *suffix) {
              size_t suffixLen = strlen(suffix);
              return key.size() >= suffixLen &&
                     key.compare(key.size() - suffixLen, suffixLen, suffix) == 0;
            };
            if (EndsWith("_INTROSCREEN_TITLE")) return true;
            if (EndsWith("_INTROSCREEN_LEVELNAME")) return true;
            if (EndsWith("_INTROSCREEN_LOC")) return true;
            if (EndsWith("_INTROSCREEN_LOCATION")) return true;
            if (EndsWith("_INTROSCREEN_TIME")) return true;
            if (EndsWith("_INTROSCREEN_DATE")) return true;
            if (EndsWith("_INTROSCREEN_NAME")) return true;
            if (EndsWith("_INTROSCREEN_PLAYERNAME")) return true;
            if (key.find("_INTROSCREEN_LINE_") != std::string::npos) return true;
            if (key.find("_INTROSCREEN_GHOST_LINE_") != std::string::npos) return true;
            // Non-INTROSCREEN intro variants (regular layout, not center-card)
            if (key.find("_INTRO_LINE_") != std::string::npos) return true;
            if (key.find("_INTROLINE_") != std::string::npos) return true;
            size_t linePos = key.find("_INTROSCREEN_LINE");
            if (linePos != std::string::npos) {
              size_t digitPos = linePos + strlen("_INTROSCREEN_LINE");
              if (digitPos < key.size() &&
                  key[digitPos] >= '0' && key[digitPos] <= '9') {
                return true;
              }
            }
            return false;
          };

          bool isCenterCard = (srcLine.order >= 50);
          if (!isCenterCard) {
            bool isCenteredAlign = (nrs.horzAlign == 1 && nrs.vertAlign == 1);
            bool inCenterBand =
                (std::fabs(approxLineX - centerX) < centerBandX &&
                 approxLineY > centerBandTop && approxLineY < centerBandBottom);
            bool nativeCenterPos =
                (nrs.x > 240.0f && nrs.x < 400.0f &&
                 nrs.y > 130.0f && nrs.y < 260.0f);
            bool nativeCenterStyle =
                (nrs.fontScale >= 1.75f &&
                 (nrs.vertAlign == 5 || nrs.vertAlign == 0 || nrs.vertAlign == 1));
            bool allowHeuristicCenterCard =
                !IsRegularIntroLayoutKey(srcLine.english);
            if (allowHeuristicCenterCard &&
                (isCenteredAlign || inCenterBand ||
                 (nativeCenterPos && nativeCenterStyle)) &&
                introSnap.lines.size() <= 3) {
              isCenterCard = true;
            }
          }

          bool isTitle = (srcLine.order <= 1);
          if (isCenterCard) isTitle = false;
          // ODIN specialist name is keyed as LINE_1, but native intro timing
          // still uses per-character reveal for this line.
          bool forceTypewriterReveal =
              (srcLine.english == "ODIN_INTROSCREEN_LINE_1");

          // --- Per-line animation state ---
          uint32_t slcKey = (uint32_t)srcLine.slcIdx;
          auto &anim = lineAnims[slcKey];
          // Reinitialize when native firstSeen changed AND animation was in
          // terminal phase (FAZEOUT done or DRIFT done).  Fresh native data
          // for a completed animation means genuine restart (new HudElem).
          if (anim.initialized && nrs.firstSeen != anim.nativeFirstSeen &&
              nrs.firstSeen > anim.nativeFirstSeen) {
            DWORD phaseDur = now - anim.phaseStart;
            bool wasTerminal = (anim.phase == PH_FAZEOUT && phaseDur > 200) ||
                               (anim.phase == PH_DRIFT && phaseDur > 6000);
            if (wasTerminal) {
              anim.initialized = false;
              globalDriftStartTime = 0;
              driftLeaderSlc = 0;
            }
          }
          if (!anim.initialized) {
            anim.phase = PH_VISIBLE;
            anim.phaseStart = now;
            anim.nativeFirstSeen = nrs.firstSeen;
            anim.initialX = nrs.x;
            anim.initialY = nrs.y;
            anim.initialFS = nrs.fontScale;
            anim.origR = nrs.r;
            anim.origG = nrs.g;
            anim.origB = nrs.b;
            anim.driftTargetX = nrs.x;
            anim.driftTargetY = nrs.y;
            anim.driftTargetFS = nrs.fontScale;
            anim.prePhaseAlpha = 0.0f;
            anim.lastNativeAlpha = 0.0f;
            anim.initialized = true;
          }

          DWORD staleMs = now - nrs.lastTick;

          // Track last known good alpha and pre-drift color
          if (staleMs < 100) {
            if (nrs.a > 0.03f) {
              anim.lastNativeAlpha = nrs.a;
            }
            if (anim.phase != PH_DRIFT) {
              anim.origR = nrs.r;
              anim.origG = nrs.g;
              anim.origB = nrs.b;
            }
          }

          // --- Phase transitions ---

          // VISIBLE -> DRIFT: detect fontScale increase (target shows in struct immediately)
          if (anim.phase == PH_VISIBLE && !isTitle && !isCenterCard) {
            float dfs = nrs.fontScale - anim.initialFS;
            float lineAge = (float)(now - nrs.firstSeen);
            if (lineAge > 1200.0f && dfs > 0.02f) {
              if (driftLeaderSlc == 0 || driftLeaderSlc == slcKey) {
                driftLeaderSlc = slcKey;
                anim.phase = PH_DRIFT;
                anim.phaseStart = now;
                anim.prePhaseAlpha = (nrs.a > 0.1f) ? nrs.a : anim.lastNativeAlpha;
                // Native struct now shows moveovertime TARGET values
                anim.driftTargetX = nrs.x;
                anim.driftTargetY = nrs.y;
                anim.driftTargetFS = nrs.fontScale;
                if (globalDriftStartTime == 0) globalDriftStartTime = now;
              }
            }
          }

          // VISIBLE -> FAZEOUT: ~1s after drift starts (GSC: wait 1 then faze_out)
          const float kFazeOutDelayMs = 1000.0f;
          if (anim.phase == PH_VISIBLE && !isCenterCard &&
              driftLeaderSlc != 0 && slcKey != driftLeaderSlc &&
              globalDriftStartTime > 0 &&
              (now - globalDriftStartTime) > (DWORD)kFazeOutDelayMs) {
            float capturedAlpha = (nrs.a > 0.1f) ? nrs.a : anim.lastNativeAlpha;
            anim.prePhaseAlpha = (capturedAlpha > 0.01f) ? capturedAlpha : 0.85f;
            anim.phase = PH_FAZEOUT;
            anim.phaseStart = now;
          }

          float tMs = (float)(now - anim.phaseStart);

          // --- Alpha ---
          float alpha = 0.0f;

          if (isCenterCard) {
            // Center card alpha: use native engine quick_pulse directly.
            // Reference: DEER_HUNT "10년 후" — all center cards must match
            // this effect (ghost spread, alpha pulse, disappearance).
            // staleMs > 90 = Detour stopped → engine destroyed the HudElem.
            // If a mission's center card disappears too early/late compared
            // to DEER_HUNT, investigate that mission's fxBirthTime and
            // FSHook text suppression timing — do NOT change this threshold.
            // Reject ghost HudElem captured as main: ghost elements have
            // low alpha (~0.1-0.2) and offset position (x=335,y=196).
            // Using them makes the center card jump and dim.
            if (nrs.a < 0.3f) continue;
            alpha = nrs.a;
            // ODIN mission: bright background makes native quick_pulse
            // hard to see. Apply mild synthetic pulse for visual interest.
            if (srcLine.english.find("ODIN") != std::string::npos) {
              alpha = QuickPulseAlpha(now, 1.0f, (uint32_t)srcLine.slcIdx);
            }
            if (staleMs > 90) alpha = 0.0f;
          } else {
            switch (anim.phase) {
              case PH_VISIBLE:
                // 100% native alpha ??includes quickPulse flickering
                alpha = nrs.a;
                // When not in drift context, stale = engine destroyed the element
                if (staleMs > 220) alpha = 0.0f;
                break;

              case PH_FAZEOUT: {
                // GSC: hud_destroy(0.06) = 60ms fade to 0
                // Stale = engine destroyed elem (loading/restart) ??hide immediately
                if (staleMs > 200) { alpha = 0.0f; break; }
                float env = 1.0f - (tMs / 60.0f);
                if (env < 0.0f) env = 0.0f;
                alpha = anim.prePhaseAlpha * env;
                break;
              }

              case PH_DRIFT: {
                // GSC name_drift() alpha:
                //   T+0 -> T+2000: 1.0 -> 0.8 (fadeovertime(2))
                //   T+2000 -> T+4000: hold 0.8
                //   T+4000 -> T+6000: 0.8 -> 0.0 (fadeovertime(2))
                // Stale = engine destroyed elem (loading/restart) ??hide immediately
                if (staleMs > 200) { alpha = 0.0f; break; }
                if (tMs < 2000.0f) {
                  alpha = 1.0f - (0.2f * tMs / 2000.0f);
                } else if (tMs < 4000.0f) {
                  alpha = 0.8f;
                } else if (tMs < 6000.0f) {
                  alpha = 0.8f * (1.0f - (tMs - 4000.0f) / 2000.0f);
                } else {
                  alpha = 0.0f;
                }
                break;
              }
            }
          }

          if (alpha < 0.01f) continue;

          // Debug log (throttled)
          {
            static std::map<uint32_t, DWORD> lastPhaseLog;
            auto &lpl = lastPhaseLog[slcKey];
            if (now - lpl > 1500) {
              lpl = now;
              const char *phNames[] = {"VISIBLE","FAZEOUT","DRIFT"};
              char pbuf[256];
              snprintf(pbuf, sizeof(pbuf),
                       "[D3D11] LINE ord=%d slc=0x%X ph=%s a=%.2f nA=%.2f "
                       "x=%.1f y=%.1f fs=%.2f stale=%u title=%d center=%d",
                       srcLine.order, slcKey, phNames[anim.phase],
                       alpha, nrs.a, nrs.x, nrs.y, nrs.fontScale,
                       (unsigned)staleMs, isTitle ? 1 : 0,
                       isCenterCard ? 1 : 0);
              LogToFile(pbuf);
            }
          }

          // --- Position & fontScale ---
          // Non-drift: use native x/y directly (includes hud_offset jitter from engine)
          // Drift: interpolate from initial to target (struct shows moveovertime TARGET)
          float useX = nrs.x, useY = nrs.y, useFS = nrs.fontScale;

          if (anim.phase == PH_DRIFT) {
            // GSC: moveovertime(12) for position
            float posProgress = tMs / 12000.0f;
            if (posProgress > 1.0f) posProgress = 1.0f;
            useX = anim.initialX + (anim.driftTargetX - anim.initialX) * posProgress;
            useY = anim.initialY + (anim.driftTargetY - anim.initialY) * posProgress;

            // GSC: changefontscaleovertime(6) for scale
            float fsProgress = tMs / 6000.0f;
            if (fsProgress > 1.0f) fsProgress = 1.0f;
            useFS = anim.initialFS + (anim.driftTargetFS - anim.initialFS) * fsProgress;
          }

          // Virtual coords -> screen coords
          float lineX = 0.0f, lineY = 0.0f;
          switch (nrs.horzAlign) {
            case 1:  lineX = screenWidth * 0.5f + useX * scaleF; break;
            case 2:  lineX = (screenWidth - safeOffX) + useX * scaleF; break;
            default: lineX = safeOffX + useX * scaleF; break;
          }
          switch (nrs.vertAlign) {
            case 1:  lineY = screenHeight * 0.5f + useY * scaleF; break;
            case 2:  lineY = (screenHeight - safeOffY) + useY * scaleF; break;
            default: lineY = safeOffY + useY * scaleF; break;
          }

          // --- Color ---
          float color[4] = { nrs.r, nrs.g, nrs.b, alpha };

          if (anim.phase == PH_DRIFT) {
            // GSC: color starts white (1,1,1), fades to original over 2s
            float oR = anim.origR, oG = anim.origG, oB = anim.origB;
            if (tMs < 2000.0f) {
              float t = tMs / 2000.0f;
              color[0] = 1.0f + (oR - 1.0f) * t;
              color[1] = 1.0f + (oG - 1.0f) * t;
              color[2] = 1.0f + (oB - 1.0f) * t;
            } else {
              color[0] = oR;
              color[1] = oG;
              color[2] = oB;
            }
          }

          // --- Text reveal ---
          // Wall-clock typewriter with ESC pause-gap compensation.
          // firstVisible is shifted forward by staleGap in Detour when
          // ESC pause is detected (gap > 200ms), so lineAge correctly
          // excludes time spent in the pause menu.
          // CPS = 33.33 (native: setpulsefx(30ms/char) = 1000/30).
          std::string renderText;
          if ((isTitle && !forceTypewriterReveal) || anim.phase == PH_DRIFT) {
            renderText = text;
          } else {
            size_t totalChars = CountUtf8Chars(text);
            if (nrs.firstVisible == 0) continue;
            float lineAge = (float)(now - nrs.firstVisible);
            const float cps = 33.33f;
            size_t visibleChars = (size_t)(lineAge / (1000.0f / cps));

            if (visibleChars < 1) continue;
            if (visibleChars > totalChars) visibleChars = totalChars;
            renderText = (visibleChars >= totalChars)
                ? text : Utf8PrefixByChars(text, visibleChars);
          }
          if (renderText.empty()) continue;

          // --- Render ---
          float lineScale = useFS * introScaleMul;
          uint8_t introAtlas = isCenterCard ? 1 : 0;
          float textWidth = KoreanRenderer::MeasureTextWidthEx(
              renderText, baseFontH, lineScale, introAtlas);

          // Title ghost pass (native multipass: dupe_hud / location_dupes_thread)
          // Center-card uses the same pass and is always rendered.
          bool drawAltPass = false;
          if (isCenterCard) {
            drawAltPass = true;
          } else if (isTitle && nrs.hasAlt && nrs.altA > 0.01f &&
                     (now - nrs.altTick) < 80) {
            // Validate alt position: HudElem reuse can leave stale
            // center-card coordinates on a title's ghost element during
            // the transition.  Discard if alt is too far from main.
            float adx = nrs.altX - nrs.x, ady = nrs.altY - nrs.y;
            if (adx * adx + ady * ady < 2500.0f) {  // within ~50 virt units
              drawAltPass = true;
            }
          }
          if (drawAltPass) {
            auto VirtualToScreen = [&](float inX, float inY,
                                       float &outX, float &outY) {
              switch (nrs.horzAlign) {
                case 1:  outX = screenWidth * 0.5f + inX * scaleF; break;
                case 2:  outX = (screenWidth - safeOffX) + inX * scaleF; break;
                default: outX = safeOffX + inX * scaleF; break;
              }
              switch (nrs.vertAlign) {
                case 1:  outY = screenHeight * 0.5f + inY * scaleF; break;
                case 2:  outY = (screenHeight - safeOffY) + inY * scaleF; break;
                default: outY = safeOffY + inY * scaleF; break;
              }
            };

            float altX = 0.0f, altY = 0.0f;
            VirtualToScreen(nrs.altX, nrs.altY, altX, altY);

            float altScale = nrs.altFontScale * introScaleMul;
            if (altScale < 0.1f) altScale = lineScale;
            float altColor[4] = { nrs.altR, nrs.altG, nrs.altB, nrs.altA };

            // Center-card: always use deterministic spread for ghost
            // motion (matching old_src behavior).  Native alt scale is
            // used when captured, but position is always synthetic so the
            // dupe-movement effect is reliably visible.
            float centerSpreadEase = 0.0f;
            float centerSplitY = 0.0f;
            float centerGhostX = 0.0f;
            float centerGhostY = 0.0f;
            if (isCenterCard) {
              bool hasNativeAlt1 = nrs.hasAlt && nrs.altA > 0.005f &&
                                   (now - nrs.altTick) < 180;
              if (hasNativeAlt1) {
                altScale = nrs.altFontScale * introScaleMul;
              }

              float centerAgeMs = 0.0f;
              if (nrs.firstVisible > 0 && now > nrs.firstVisible) {
                centerAgeMs = (float)(now - nrs.firstVisible);
              } else {
                centerAgeMs = tMs;
              }
              if (centerAgeMs < 0.0f) centerAgeMs = 0.0f;

              float spreadT = centerAgeMs / 3200.0f;
              if (spreadT <= 1.0f) {
                centerSpreadEase = spreadT * spreadT * (3.0f - 2.0f * spreadT);
              } else {
                // Continue at same velocity beyond 3200ms so ghosts never
                // freeze on long-lived center cards (e.g. ODIN fxB=143050).
                // smoothstep derivative at t=1.0 = 0, so use the average
                // velocity (1.0/3200ms) to keep consistent movement.
                centerSpreadEase = 1.0f + (spreadT - 1.0f);
              }

              float centerBaseDx = (6.5f * centerSpreadEase) * scaleF;
              float centerBaseDy = (0.3f * centerSpreadEase) * scaleF;
              centerGhostX = lineX + centerBaseDx;
              centerGhostY = lineY + centerBaseDy;
              centerSplitY = (1.8f * centerSpreadEase) * scaleF;

              altX = centerGhostX;
              altY = centerGhostY - centerSplitY;
              altScale = lineScale;
              altColor[0] = 0x66 / 255.0f;
              altColor[1] = 0x6f / 255.0f;
              altColor[2] = 0x6f / 255.0f;
              float centerBaseA = alpha * (0.19f + 0.04f * centerSpreadEase);
              altColor[3] = centerBaseA;
              if (altColor[3] < 0.08f) altColor[3] = 0.08f;
              if (altColor[3] > 0.34f) altColor[3] = 0.34f;
            }

            float altWidth = KoreanRenderer::MeasureTextWidthEx(
                renderText, baseFontH, altScale, introAtlas);
            KoreanRenderer::QueueText(renderText, altX, altY, altScale,
                                      altColor, baseFontH, 0, altWidth,
                                      true, false, 2.0f, false, false, false,
                                      -1, 0.0f, false, false, introAtlas);

            // Center-card second ghost dupe (GSC: stylized_line creates two dupes)
            if (isCenterCard) {
              float alt2X = centerGhostX;
              float alt2Y = centerGhostY + centerSplitY;
              float alt2Scale = lineScale;
              float alt2Color[4] = {
                  altColor[0], altColor[1], altColor[2],
                  altColor[3] * 0.82f
              };
              alt2Color[0] = 0x66 / 255.0f;
              alt2Color[1] = 0x6f / 255.0f;
              alt2Color[2] = 0x6f / 255.0f;
              if (alt2Color[3] < 0.075f) alt2Color[3] = 0.075f;
              if (alt2Color[3] > 0.40f)  alt2Color[3] = 0.40f;

              if (alt2Color[3] > 0.002f) {
                float alt2Width = KoreanRenderer::MeasureTextWidthEx(
                    renderText, baseFontH, alt2Scale, introAtlas);
                KoreanRenderer::QueueText(renderText, alt2X, alt2Y, alt2Scale,
                                          alt2Color, baseFontH, 0, alt2Width,
                                          true, false, 2.0f, false, false, false,
                                          -1, 0.0f, false, false, introAtlas);
              }
            }
          }

          // Center-card bleed is handled by the shared alt pass above.

          // Glow pass
          if (!isCenterCard) {
            if (anim.phase == PH_DRIFT) {
              // GSC drift glow: starts white glowalpha=1,
              // fades to (0.10625, 0.11625, 0.115) glowalpha=0.1 over 2s
              float glowR, glowG, glowB, glowA;
              if (tMs < 2000.0f) {
                float t = tMs / 2000.0f;
                glowR = 1.0f + (0.10625f - 1.0f) * t;
                glowG = 1.0f + (0.11625f - 1.0f) * t;
                glowB = 1.0f + (0.115f   - 1.0f) * t;
                glowA = 1.0f + (0.1f     - 1.0f) * t;
              } else {
                glowR = 0.10625f;
                glowG = 0.11625f;
                glowB = 0.115f;
                glowA = 0.1f;
              }
              glowA *= alpha; // modulate with overall drift alpha
              if (glowA > 0.003f) {
                float glowColor[4] = { glowR, glowG, glowB, glowA };
                float glowOff = 1.5f * scaleF;
                KoreanRenderer::QueueText(renderText, lineX - glowOff, lineY,
                                          lineScale, glowColor, baseFontH, 0,
                                          textWidth, true, false, 2.0f);
                KoreanRenderer::QueueText(renderText, lineX + glowOff, lineY,
                                          lineScale, glowColor, baseFontH, 0,
                                          textWidth, true, false, 2.0f);
                KoreanRenderer::QueueText(renderText, lineX, lineY - glowOff,
                                          lineScale, glowColor, baseFontH, 0,
                                          textWidth, true, false, 2.0f);
                KoreanRenderer::QueueText(renderText, lineX, lineY + glowOff,
                                          lineScale, glowColor, baseFontH, 0,
                                          textWidth, true, false, 2.0f);
              }
            } else if (anim.phase == PH_VISIBLE && alpha > 0.03f) {
              // GSC: glowalpha=0.05, glowcolor=(0.425,0.465,0.46)
              float glowAlpha = alpha * 0.05f;
              float glowColor[4] = { 0.425f, 0.465f, 0.46f, glowAlpha };
              float glowOff = 1.0f * scaleF;
              KoreanRenderer::QueueText(renderText, lineX - glowOff, lineY,
                                        lineScale, glowColor, baseFontH, 0,
                                        textWidth, true, false, 2.0f);
              KoreanRenderer::QueueText(renderText, lineX + glowOff, lineY,
                                        lineScale, glowColor, baseFontH, 0,
                                        textWidth, true, false, 2.0f);
            }
          }

          // Center-card custom upward bleed disabled; shared alt pass is used.

          // ODIN: diagonal drop-shadow for bright-background contrast.
          // Single lower-right pass (matches in-game subtitle shadow style).
          if (isCenterCard &&
              srcLine.english.find("ODIN") != std::string::npos &&
              alpha > 0.03f) {
            float outA = alpha * 0.30f;
            float outColor[4] = { 0.0f, 0.06f, 0.06f, outA };
            float outOff = 1.5f * scaleF;
            KoreanRenderer::QueueText(renderText, lineX + outOff, lineY + outOff,
                                      lineScale, outColor, baseFontH, 0,
                                      textWidth, true, false, 2.0f,
                                      false, false, false, -1,
                                      0.0f, false, false, introAtlas);
          }

          // Main text (center card: atlas2, no diagonal shadow)
          // Multi-line intros: the auto-alignment heuristic clusters the
          // center card with non-center lines, left-aligning both.
          // Override with forceAlign=2 so the center card is independently
          // centered at screen center. Single-line intros are unaffected
          // (heuristic already centers them correctly).
          int mainAlign = -1;
          float mainWidthEng = textWidth;
          if (srcLine.english == "DEER_HUNT_JEEP_INTROLINE_1" ||
              srcLine.english == "DEER_HUNT_JEEP_INTROLINE_2") {
            // JEEP intro: two lines at same x=320 but heuristic clusters
            // them as left-aligned. Force center at screen center.
            mainAlign = 2;
            float screenCenterX = screenWidth * 0.5f;
            mainWidthEng = (screenCenterX - lineX) * 2.0f;
            if (mainWidthEng < 0.0f) mainWidthEng = 0.0f;
            // Add spacing between the two lines (native y gap=20 virtual
            // units is too tight for Korean font at fs=2.00)
            if (srcLine.english == "DEER_HUNT_JEEP_INTROLINE_2") {
              lineY += 8.0f * scaleF;
            }
          }
          KoreanRenderer::QueueText(renderText, lineX, lineY, lineScale, color,
                                    baseFontH, 0, mainWidthEng, isCenterCard, false,
                                    2.0f, false, false, false, mainAlign,
                                    0.0f, false, false, introAtlas);
        }

      }
    } else {
      static DWORD lastIntroSkipLog = 0;
      if (now - lastIntroSkipLog > 1500 &&
          (pauseMenuLikely || !introSnap.lines.empty() || introSnap.active)) {
        lastIntroSkipLog = now;
        if (pauseMenuLikely) {
          LogToFile("[D3D11] INTRO hidden_by_pause active=" +
                    std::to_string(introSnap.active ? 1 : 0) +
                    " lines=" + std::to_string(introSnap.lines.size()));
        } else {
          LogToFile("[D3D11] INTRO skipped active=" +
                    std::to_string(introSnap.active ? 1 : 0) +
                    " lines=" + std::to_string(introSnap.lines.size()));
        }
      }
    }

    // Clean up animation state when intro session ends.
    // This MUST be outside the rendering block (was dead code inside it).
    if (!introSnap.active) {
      lineAnims.clear();
      globalDriftStartTime = 0;
      driftLeaderSlc = 0;
    }
  }

  // ==========================================================================
