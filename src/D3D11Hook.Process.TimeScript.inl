    // Ensure the HudElem scanner runs every frame for objective/hint/countdown
    // detection.  ObjUnified_Render may skip TextHook_ReadLiveHudElems() when
    // objectives are hidden (objectiveHiddenByGate early-return).
    if (!hudPauseMenu) {
      TextHook_ReadLiveHudElems();
    }

    // Render the COMPLETE Korean countdown text (label + timer number) via
    // D3D11 overlay.  The engine's own rendering of the label and timer is
    // fully suppressed (skipOriginal=true in IAR + fontScale=0 + time=0 on
    // HudElem).  We self-render the timer number from cached time field so
    // the display stays perfectly in sync.
    // Try to discover the timer from HudElem array when FALLBACK is active
    // but timerComputed==false (e.g., after mission transition or mid-continue).
    if (!hudPauseMenu) {
      TextHook_TimeScriptDiscoverFallbackTimer();
    }

    TimeScriptRenderSnapshot timeScriptSnapshot{};
    if (!hudPauseMenu && TextHook_GetTimeScriptRenderSnapshot(timeScriptSnapshot)) {
      // Track whether a valid *computed* timer was ever shown for this key.
      // Only apply the "timer finished" guard when timerComputed==true so we
      // don't falsely suppress rendering after a mission section transition
      // where timerSeconds==0 because game time is unknown (FALLBACK path).
      static bool s_timerEverActive = false;
      static std::string s_timerActiveKey;

      // Reset tracking when key changes.
      if (timeScriptSnapshot.key != s_timerActiveKey) {
        s_timerEverActive = false;
        s_timerActiveKey = timeScriptSnapshot.key;
      }

      if (timeScriptSnapshot.timerComputed &&
          timeScriptSnapshot.timerSeconds > 0.05f) {
        s_timerEverActive = true;
      }

      // If timer was running (computed) but now reached 0, skip rendering
      // → instant disappearance (no lingering Korean label).
      // Do NOT apply when timerComputed==false (mission transition / unknown
      // game time) — in that case we still want to show the Korean label.
      if (s_timerEverActive && timeScriptSnapshot.timerComputed &&
          timeScriptSnapshot.timerSeconds <= 0.05f) {
        // Timer finished — don't render.  Snapshot expires via TTL.
      } else {
        // Format timer number from remaining seconds (e.g., 67.7 → "1:07.7")
        // SATFARM uses mm:ss without tenths (e.g., 536s → "8:56").
        // Skip timer number when game time not yet discovered (avoids "0:00.0"
        // flash during the first frames while the probe is still running).
        char timerBuf[32] = {0};
        if (timeScriptSnapshot.timerSeconds > 0.05f) {
          float secs = timeScriptSnapshot.timerSeconds;
          if (timeScriptSnapshot.noTenths) {
            int totalSecs = (int)(secs + 0.5f);
            int mins = totalSecs / 60;
            int secsI = totalSecs % 60;
            sprintf_s(timerBuf, "%d:%02d", mins, secsI);
          } else {
            int totalTenths = (int)(secs * 10.0f);
            int mins = totalTenths / 600;
            int secsI = (totalTenths % 600) / 10;
            int tenths = totalTenths % 10;
            sprintf_s(timerBuf, "%d:%02d.%d", mins, secsI, tenths);
          }
        }

        // Build complete composite text: Korean label + timer number
        const std::string finalRenderText =
            timeScriptSnapshot.renderText + timerBuf;

        // TimeScript snapshots carry logical 1080p font heights, unlike the
        // pixel heights supplied by the general HUD layout adapter.
        const float timerResolutionScale = g_ActiveArea.height / 1080.0f;
        // Measure final composite width for right-alignment positioning
        float englishWidth = KoreanRenderer::MeasureTextWidthEx(
            finalRenderText, timeScriptSnapshot.fontHeight,
            timeScriptSnapshot.scale, 0, timerResolutionScale);
        if (!(englishWidth > 1.0f)) {
          englishWidth = 300.0f;  // fallback
        }

        DrawStylePatch stylePatch{};
        stylePatch.applyGlow = true;
        stylePatch.glowForceAll = true;
        // Dark green background glow matching the original timer color
        stylePatch.glowR = 0.10f;
        stylePatch.glowG = 0.22f;
        stylePatch.glowB = 0.10f;
        stylePatch.glowA = 0.85f;
        stylePatch.glowSpread = 7.0f;
        stylePatch.glowAlphaMax = 0.35f;

        KoreanRenderer::QueueText(
            finalRenderText, timeScriptSnapshot.x,
            timeScriptSnapshot.y, timeScriptSnapshot.scale,
            timeScriptSnapshot.color, timeScriptSnapshot.fontHeight, 0,
            englishWidth, false, false, timeScriptSnapshot.scale, false, false,
            true, timeScriptSnapshot.forceAlign, 0.0f, false,
            timeScriptSnapshot.yIsTopOfText, 0, &stylePatch,
            timerResolutionScale);

        static DWORD s_lastTimeScriptRenderLog = 0;
        if ((now - s_lastTimeScriptRenderLog) > 900) {
          s_lastTimeScriptRenderLog = now;
          char tbuf[512];
          sprintf_s(
              tbuf,
              "[TIMESCRIPT-LIVE-RENDER] key=%s x=%.1f y=%.1f scale=%.3f fontH=%.1f "
              "timer=%.1f text=\"%.96s\"",
              timeScriptSnapshot.key.c_str(), timeScriptSnapshot.x,
              timeScriptSnapshot.y, timeScriptSnapshot.scale,
              timeScriptSnapshot.fontHeight, timeScriptSnapshot.timerSeconds,
              finalRenderText.c_str());
          LogToFile(tbuf);
        }
      }
    }
