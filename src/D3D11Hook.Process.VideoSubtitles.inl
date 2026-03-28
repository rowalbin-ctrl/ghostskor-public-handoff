  // 0. Process VIDEO Subtitles (from BinkHook - Bink video playback)
  if (videoPlaying) {
    std::string videoSub = BinkHook::GetCurrentSubtitle();

    if (!videoSub.empty()) {
      extern std::atomic<bool> g_bQueueKeepDuringVideo;
      g_bQueueKeepDuringVideo.store(true);

      static std::string s_lastPassDiagSub;
      static DWORD s_lastPassDiagTick = 0;
      static int s_lastPassDiagCount = 0;
      if (videoSub != s_lastPassDiagSub) {
        s_lastPassDiagSub = videoSub;
        s_lastPassDiagTick = now;
        s_lastPassDiagCount = 0;
      }

      if (g_FrameSwapChainDescValid) {
        const DXGI_SWAP_CHAIN_DESC &scDesc = g_FrameSwapChainDesc;
        float screenWidth = (float)scDesc.BufferDesc.Width;
        float screenHeight = (float)scDesc.BufferDesc.Height;

        // Video subtitle styling (render-format only; timing remains untouched)
        float subY = g_ActiveArea.offsetY + g_ActiveArea.height * 0.85f;
        float scale = ComputeVideoSubtitleScale(g_ActiveArea.width, g_ActiveArea.height);
        float fontHeight = 60.0f;

        // Engine-derived palette (best-effort): tame neon ^2 and keep a slightly
        // darker base tint for the dialogue portion.
        float friendlyName[4] = {0.90f, 1.00f, 0.90f, 0.70f};
        (void)TextHook_TryGetDvarVec4("friendlyNameFontColor", friendlyName);
        float nameGlow[4] = {0.0f, 0.30f, 0.0f, 1.0f};
        (void)TextHook_TryGetDvarVec4("friendlyNameFontGlowColor", nameGlow);

        // Base color = white for dialogue text; ^2 override handles name color.
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

        // Style diagnostics (throttled)
        {
          static DWORD s_lastStyleLog = 0;
          if (now - s_lastStyleLog > 3000) {
            s_lastStyleLog = now;
            char buf[256];
            sprintf_s(buf,
                      "[D3D11] VIDEO style base=(%.3f %.3f %.3f) "
                      "name=(%.3f %.3f %.3f a=%.3f) glow=(%.3f %.3f %.3f a=%.3f)",
                      color[0], color[1], color[2],
                      friendlyName[0], friendlyName[1], friendlyName[2],
                      friendlyName[3],
                      nameGlow[0], nameGlow[1], nameGlow[2], nameGlow[3]);
            LogToFile(buf);
          }
        }

        float safeMarginX = (std::max)(24.0f, g_ActiveArea.width * 0.045f);
        float maxTextWidth =
            ComputeSubtitleWrapWidth(g_ActiveArea.width, g_ActiveArea.height, true);
        std::vector<std::string> wrappedLines =
            WrapKoreanText(videoSub, maxTextWidth, fontHeight, scale, true);

        // If a tiny tail forces 3 lines, gently widen width (within safe area)
        // to recover a cleaner 2-line subtitle.
        if (wrappedLines.size() == 3) {
          float tailWidth = KoreanRenderer::MeasureTextWidthEx(
              wrappedLines.back(), fontHeight, scale);
          if (tailWidth < maxTextWidth * 0.40f) {
            float safeMaxWidth = g_ActiveArea.width - safeMarginX * 2.0f;
            if (safeMaxWidth < maxTextWidth) {
              safeMaxWidth = maxTextWidth;
            }
            for (int iter = 0; iter < 5; ++iter) {
              float t = (float)(iter + 1) / 5.0f;
              float widened =
                  maxTextWidth + (safeMaxWidth - maxTextWidth) * t;
              auto candidate = WrapKoreanText(videoSub, widened, fontHeight,
                                              scale, true);
              if (candidate.size() <= 2) {
                wrappedLines = std::move(candidate);
                break;
              }
            }
          }
        }
        float lineSpacing = fontHeight * (scale * 0.34f);
        // Keep first line anchored to the two-line top row so one-line and
        // two-line subtitles start from the same Y. For 3+ lines, keep the
        // previous bottom-anchor behavior.
        float topAnchorY = subY - lineSpacing;
        if (wrappedLines.size() >= 3) {
          topAnchorY = subY - (float)(wrappedLines.size() - 1) * lineSpacing;
        }

        DrawStylePatch subtitleStylePatch{};
        subtitleStylePatch.overrideColorCode1 = true;
        subtitleStylePatch.code1R = 1.0f;
        subtitleStylePatch.code1G = 0.361f;
        subtitleStylePatch.code1B = 0.361f;
        subtitleStylePatch.overrideColorCode2 = true;
        subtitleStylePatch.code2R = friendlyName[0];
        subtitleStylePatch.code2G = friendlyName[1];
        subtitleStylePatch.code2B = friendlyName[2];
        subtitleStylePatch.applyGlow = true;
        subtitleStylePatch.glowR = 0.0f;
        subtitleStylePatch.glowG = 0.0f;
        subtitleStylePatch.glowB = 0.0f;
        subtitleStylePatch.glowA = 0.95f;
        // Render wrapped lines from a stable top anchor.
        for (size_t wl = 0; wl < wrappedLines.size(); ++wl) {
          std::string wLine = wrappedLines[wl];
          wLine = EnsureNameHasResetCode(wLine);

          // Diagnose cases where readability/name pass appears to "attach"
          // late on video subtitles.
          if (wl == 0) {
            DWORD diagAge = now - s_lastPassDiagTick;
            if (diagAge <= 260 && s_lastPassDiagCount < 4) {
              std::string nameSeg = ExtractNameSegment(wLine);
              bool hasNameSeg = !nameSeg.empty();
              bool hasCode2 = (wLine.find("^2") != std::string::npos);
              bool hasHangul = ContainsHangul(wLine);
              std::ostringstream oss;
              oss << "[VIDPASS] age=" << diagAge
                  << "ms code2=" << (hasCode2 ? 1 : 0)
                  << " nameSeg=" << (hasNameSeg ? 1 : 0)
                  << " hangul=" << (hasHangul ? 1 : 0)
                  << " bytes=" << HexPrefix(wLine, 12)
                  << " text=" << Utf8Preview(wLine, 56);
              LogToFile(oss.str());
              s_lastPassDiagCount++;
            }
          }

          float wWidth = KoreanRenderer::MeasureTextWidthEx(wLine, fontHeight, scale);
          float wX = g_ActiveArea.offsetX + (g_ActiveArea.width - wWidth) * 0.5f;

          float minX = g_ActiveArea.offsetX + safeMarginX;
          float maxX = g_ActiveArea.offsetX + g_ActiveArea.width - safeMarginX - wWidth;
          if (maxX < minX) {
            maxX = minX;
          }
          if (wX < minX) {
            wX = minX;
          }
          if (wX > maxX) {
            wX = maxX;
          }

          float wY = topAnchorY + (float)wl * lineSpacing;

          QueueSubtitleReadabilityPass(wLine, wX, wY, scale, fontHeight, wWidth,
                                       color[3], friendlyName, nameGlow);

          // Main subtitle text with shadow and native-style glow.
          KoreanRenderer::QueueText(wLine, wX, wY, scale, color, fontHeight, 0,
                                    wWidth, false, false, 2.0f, false, true,
                                    false, -1, 0.0f, false, false, 0,
                                    &subtitleStylePatch);
        }

        // Log (throttled)
        static DWORD lastVideoSubLog = 0;
        static std::string lastVideoSubText;
        if (videoSub != lastVideoSubText || now - lastVideoSubLog > 2000) {
          lastVideoSubLog = now;
          lastVideoSubText = videoSub;
          LogToFile("[D3D11] VIDEO SUBTITLE: " + Utf8Preview(videoSub, 60) +
                    "...");
        }
      }

      g_bQueueKeepDuringVideo.store(false);
    }
  }
