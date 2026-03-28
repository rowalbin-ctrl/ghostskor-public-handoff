  // 3. Process HUD OVERLAY (Objectives, Hints, Game Messages)
  // Objective/save custom overlay is hidden during pause menu.
  // ==========================================================================
  {
    // IMPORTANT:
    // Do NOT hard-skip HUD overlay from menu flags here.
    // In practice, TextHook menu/raw flags can be noisy in gameplay and would
    // suppress legitimate HUD hints/prompts (center Press/Hold, object prompts).
    // Menu pollution is handled by per-key/per-caller filtering upstream.

    // Phase 5: Unified gate state for all pipelines
    GateState gate = ComputeGateState(now, s_HudOverlaySuppressUntil);
    bool hudPauseMenu = gate.pauseMenu;
    bool suppressHudOverlay = gate.transitionBlack;
    static uint64_t s_overlayFrameCounter = 0;
    const uint64_t overlayFrameId = ++s_overlayFrameCounter;
    OverlaySessionState overlaySessionState = OVERLAY_SESSION_GAMEPLAY;
    if (gate.frontMenu) {
      overlaySessionState = OVERLAY_SESSION_FRONTEND;
    } else if (gate.pauseMenu) {
      overlaySessionState = OVERLAY_SESSION_PAUSE;
    } else if (gate.transitionBlack) {
      overlaySessionState = OVERLAY_SESSION_LOADING;
    } else if (gate.warmupActive) {
      overlaySessionState = OVERLAY_SESSION_WARMUP;
    }
    TextHook_BeginOverlayFrame(overlayFrameId, overlaySessionState);

    NativeHudFrameSnapshot nativeHudSnapshot{};
    TextHook_GetNativeHudFrameSnapshot(nativeHudSnapshot);

    static std::vector<HintOverlayEntry> hintEntries;
    static std::vector<ObjHudHintEntry> objHudHintEntries;
    static std::vector<HudNativeEntry> hudNativeEntries;
    hintEntries.clear();
    objHudHintEntries.clear();
    hudNativeEntries.clear();
    for (auto &e : nativeHudSnapshot.nativeEntries) {
        hudNativeEntries.push_back(std::move(e));
      }
      hintEntries.reserve(nativeHudSnapshot.instances.size());
      objHudHintEntries.reserve(nativeHudSnapshot.instances.size());

      static std::unordered_map<std::string, size_t> hintIndexBySig;
      static std::unordered_map<std::string, size_t> objIndexBySig;
      hintIndexBySig.clear();
      objIndexBySig.clear();

      auto sourceToNative = [](uint8_t sourceToken) -> unsigned char {
        if (sourceToken == NATIVE_HUD_SOURCE_HUD560) {
          return (unsigned char)HUD_NATIVE_RENDER;
        }
        if (sourceToken == NATIVE_HUD_SOURCE_ADDCMD) {
          return (unsigned char)HUD_NATIVE_ADDCMD;
        }
        if (sourceToken == NATIVE_HUD_SOURCE_HUDELEM) {
          // Live HudElem reverse evidence is native struct data, not SLC.
          return (unsigned char)HUD_NATIVE_RENDER;
        }
        if (sourceToken == NATIVE_HUD_SOURCE_BRIDGE) {
          return (unsigned char)HUD_NATIVE_RENDER;
        }
        return (unsigned char)HUD_NATIVE_SCANNER;
      };

      for (const auto &inst : nativeHudSnapshot.instances) {
        if (inst.keyToken.empty()) {
          continue;
        }
        const DWORD age = (now >= inst.lastSeenTick) ? (now - inst.lastSeenTick) : 0;
        if (age > 520) {
          continue;
        }
        if (!inst.hasNativeEvidence && !inst.hasEnglishEvidence) {
          continue;
        }
        if (inst.track != NATIVE_HUD_TRACK_HINT &&
            inst.track != NATIVE_HUD_TRACK_OBJHINT) {
          continue;
        }

        std::string korean;
        std::string english;
        if (!TextHook_GetLocalizedKeyText(inst.keyToken, korean, english) ||
            korean.empty()) {
          continue;
        }

        std::string runtimeEnglish = inst.rawText;
        if (runtimeEnglish.empty()) {
          TextHook_GetRuntimeEnglishForKeyRecent(inst.keyToken, runtimeEnglish, 260);
        }
        runtimeEnglish = TrimAscii(StripColorCodesSimple(runtimeEnglish));
        std::string finalEnglish = !runtimeEnglish.empty() ? runtimeEnglish : english;
        finalEnglish = TrimAscii(StripColorCodesSimple(finalEnglish));
        if (!inst.hasNativeEvidence && finalEnglish.empty()) {
          continue;
        }

        const int slot =
            TextHook_HudSlotFromEvent(inst.y, inst.virtualH, inst.flags);
        const std::string sig =
            inst.keyToken + "|" + std::to_string(slot) + "|" +
            std::to_string((unsigned long long)inst.consumerId) + "|" +
            std::to_string((unsigned int)inst.sourceToken);

        if (inst.track == NATIVE_HUD_TRACK_HINT) {
          HintOverlayEntry he{};
          he.key = inst.keyToken;
          he.korean = korean;
          he.english = finalEnglish.empty() ? inst.keyToken : finalEnglish;
          he.consumerId = inst.consumerId;
          he.slotBucket = slot;
          he.nativeX = inst.x;
          he.nativeY = inst.y;
          he.nativeXScale = inst.xScale;
          he.nativeYScale = inst.yScale;
          he.fontHeight = inst.fontHeight;
          he.nativeVirtualW = inst.virtualW;
          he.nativeVirtualH = inst.virtualH;
          // Center-aligned warning (va=5): x/y are offsets from screen
          // center in virtual space.  Convert to absolute virtual coords
          // so HUDHINT nativeCoordToScreen produces the right position.
          // Must come AFTER generic nativeVirtualW/H assignment above.
          if (inst.keyToken == "ODIN_STRAY_INTRO_WARNING" && inst.y < 0) {
            he.nativeX = 320.0f;
            he.nativeY = 240.0f + inst.y;
            he.nativeVirtualW = 640;
            he.nativeVirtualH = 480;
          }
          he.color[0] = inst.color[0];
          he.color[1] = inst.color[1];
          he.color[2] = inst.color[2];
          he.color[3] = inst.color[3];
          he.firstSeen = inst.firstSeenTick;
          he.lastSeen = inst.lastSeenTick;
          he.hasNativeParams = inst.hasNativeEvidence;
          he.nativeSource = sourceToNative(inst.sourceToken);
          he.nativeSourceToken = inst.sourceToken;
          he.callerOffset = inst.callerOffset;
          he.lastNativeSeenTick = inst.hasNativeEvidence ? inst.lastSeenTick : 0;
          he.lastEnglishSeenTick = he.english.empty() ? 0 : inst.lastSeenTick;
          he.lastRenderedTick = 0;

          auto it = hintIndexBySig.find(sig);
          if (it == hintIndexBySig.end()) {
            hintIndexBySig[sig] = hintEntries.size();
            hintEntries.push_back(std::move(he));
          } else if (he.lastSeen >= hintEntries[it->second].lastSeen) {
            hintEntries[it->second] = std::move(he);
          }
        } else {
          ObjHudHintEntry oe{};
          oe.key = inst.keyToken;
          oe.korean = korean;
          oe.english = finalEnglish.empty() ? inst.keyToken : finalEnglish;
          oe.nativeX = inst.x;
          oe.nativeY = inst.y;
          oe.nativeXScale = inst.xScale;
          oe.nativeYScale = inst.yScale;
          oe.fontHeight = inst.fontHeight;
          oe.color[0] = inst.color[0];
          oe.color[1] = inst.color[1];
          oe.color[2] = inst.color[2];
          oe.color[3] = 1.0f;
          oe.firstSeen = inst.firstSeenTick;
          oe.lastSeen = inst.lastSeenTick;
          oe.hasNativeParams = inst.hasNativeEvidence;
          oe.nativeSource = sourceToNative(inst.sourceToken);
          oe.nativeSourceToken = inst.sourceToken;
          oe.nativeVirtualW = inst.virtualW;
          oe.nativeVirtualH = inst.virtualH;
          oe.callerOffset = inst.callerOffset;
          oe.lastNativeSeenTick = inst.hasNativeEvidence ? inst.lastSeenTick : 0;
          oe.lastEnglishSeenTick = oe.english.empty() ? 0 : inst.lastSeenTick;
          oe.lastRenderedTick = 0;

          auto it = objIndexBySig.find(sig);
          if (it == objIndexBySig.end()) {
            objIndexBySig[sig] = objHudHintEntries.size();
            objHudHintEntries.push_back(std::move(oe));
          } else if (oe.lastSeen >= objHudHintEntries[it->second].lastSeen) {
            objHudHintEntries[it->second] = std::move(oe);
          }
        }
      }

      // Native-only HUD hint mode:
      // Do not merge consumer/SLC-derived fallback snapshots.
      // All hint instances must come from reversed native structure evidence.
    if (kVerboseRuntimeLogs) {
      static DWORD s_lastNativeTrackLog = 0;
      if ((now - s_lastNativeTrackLog) > 1200) {
        s_lastNativeTrackLog = now;
        char nbuf[256];
        sprintf_s(
            nbuf,
            "[NHUD-TRACK] hint=%d objhint=%d native=%d snapshot=%d",
            (int)hintEntries.size(), (int)objHudHintEntries.size(),
            (int)hudNativeEntries.size(),
            (int)nativeHudSnapshot.instances.size());
        LogToFile(nbuf);
      }
    }

    // Suppression guard refinement:
    // In-world Bink playback can coexist with live gameplay hints.
    // If native hint draws are active, do not suppress HUD hint overlays.
    if (suppressHudOverlay) {
      bool hasRecentNativeHint = false;
      std::string recentHintKey;
      for (const auto &n : hudNativeEntries) {
        if (n.key.empty() || n.objectiveLike) {
          continue;
        }
        if ((now - n.lastUpdate) <= 500) {
          hasRecentNativeHint = true;
          recentHintKey = n.key;
          break;
        }
      }

      const bool hasOverlayHintEvidence =
          !hintEntries.empty() || !objHudHintEntries.empty();
      bool hasRecentHintEntry = false;
      for (const auto &he : hintEntries) {
        if ((now - he.lastSeen) <= 520) {
          hasRecentHintEntry = true;
          if (recentHintKey.empty()) recentHintKey = he.key;
          break;
        }
      }
      if (!hasRecentHintEntry) {
        for (const auto &oe : objHudHintEntries) {
          if ((now - oe.lastSeen) <= 520) {
            hasRecentHintEntry = true;
            if (recentHintKey.empty()) recentHintKey = oe.key;
            break;
          }
        }
      }
      const bool allowSuppressBypass =
          (gate.videoPlaying || gate.suppressUntil || gate.hideHudFast) &&
          !gate.warmupActive &&
          !gate.signals.uiPrevMapChanged;
      if (allowSuppressBypass &&
          (hasRecentNativeHint ||
           (hasOverlayHintEvidence && hasRecentHintEntry))) {
        suppressHudOverlay = false;
        static DWORD s_lastSuppressBypassLog = 0;
        if ((now - s_lastSuppressBypassLog) > 2000) {
          s_lastSuppressBypassLog = now;
          LogToFile("[D3D11] HUD suppress bypass (live native hint): " +
                    recentHintKey);
        }
      } else {
        static DWORD s_LastSuppressHudLog = 0;
        if ((now - s_LastSuppressHudLog) > 2000) {
          s_LastSuppressHudLog = now;
          LogToFile("[D3D11] HUD overlay suppressed during transition black");
        }
      }
    }

    const bool objectiveHiddenByGate = (ObjectiveVisibility(gate) <= 0.0f);
    static int s_hintOnlyMode = -1;
    if (s_hintOnlyMode < 0) {
      char envBuf[16] = {0};
      DWORD envLen = GetEnvironmentVariableA("GHOSTSKOR_HINT_ONLY", envBuf,
                                             (DWORD)sizeof(envBuf));
      const bool envOn = (envLen > 0 && envBuf[0] != '0');
      s_hintOnlyMode = envOn ? 1 : 0;
      char hbuf[128];
      sprintf_s(hbuf, "[HUD-OBJECTIVE-MODE] hint_only=%d", s_hintOnlyMode);
      LogToFile(hbuf);
    }
    bool runObjectiveSubsystem = (s_hintOnlyMode == 0);
    if (runObjectiveSubsystem && objectiveHiddenByGate) {
      const bool objLaneHot = TextHook_HasRecentObjectiveLaneActivity(900);
      static uint64_t s_objIdleFrameCounter = 0;
      if (!objLaneHot) {
        ++s_objIdleFrameCounter;
        runObjectiveSubsystem = ((s_objIdleFrameCounter % 6ull) == 0ull);
      }
    }
    if (runObjectiveSubsystem) {
      // NEW: Unified single-path objective renderer.
      ObjUnified_Render(pSwapChain, now, suppressHudOverlay, hudPauseMenu,
                        objectiveHiddenByGate);
    }

    // Legacy objective renderer block removed (dedicated native objective pipeline only).

    // De-duplicate ObjHudHint against already-rendered hint content.
    std::unordered_set<std::string> renderedHintSignatures;
    renderedHintSignatures.reserve(hintEntries.size() * 2 + 8);
    auto BuildPromptSemanticSig = [](const std::string &rawText) -> std::string {
      std::string s = TrimAscii(StripColorCodesSimple(rawText));
      if (s.empty()) {
        return "";
      }

      std::string out;
      out.reserve(s.size() + 8);
      bool prevSpace = false;
      for (size_t i = 0; i < s.size();) {
        if (s[i] == '[') {
          size_t rb = s.find(']', i + 1);
          if (rb != std::string::npos && rb > i + 1) {
            if (!out.empty() && out.back() != ' ') {
              out.push_back(' ');
            }
            out.append("{B}");
            i = rb + 1;
            prevSpace = false;
            continue;
          }
        }
        if (i + 2 < s.size() &&
            ((s[i] == '&' && s[i + 1] == '&') ||
             (s[i] == '$' && s[i + 1] == '$')) &&
            std::isdigit((unsigned char)s[i + 2])) {
          if (!out.empty() && out.back() != ' ') {
            out.push_back(' ');
          }
          out.append("{B}");
          i += 2;
          while (i < s.size() && std::isdigit((unsigned char)s[i])) {
            ++i;
          }
          prevSpace = false;
          continue;
        }

        unsigned char uc = (unsigned char)s[i];
        if (std::isspace(uc)) {
          if (!prevSpace && !out.empty()) {
            out.push_back(' ');
          }
          prevSpace = true;
          ++i;
          continue;
        }
        prevSpace = false;
        out.push_back((char)std::toupper(uc));
        ++i;
      }

      return TrimAscii(out);
    };

    // Phase 3: dedicated HUD Hint pipeline renderer.
