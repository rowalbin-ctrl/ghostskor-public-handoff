static float FloatFromParamBits(void *p) {
  float f = 0.0f;
  memcpy(&f, &p, sizeof(float));
  return f;
}
static bool TryReadFloat4Ptr(void *p, float out[4]) {
  if (!p || !out)
    return false;
  uintptr_t v = (uintptr_t)p;
  if (v < 0x10000 || v > 0x7FFFFFFFFFFF)
    return false;
  if (!IsSafeRead((void *)v, 16))
    return false;
  __try {
    float r = *(volatile float *)((uintptr_t)p + 0);
    float g = *(volatile float *)((uintptr_t)p + 4);
    float b = *(volatile float *)((uintptr_t)p + 8);
    float a = *(volatile float *)((uintptr_t)p + 12);
    auto finite = [](float x) -> bool { return std::isfinite(x) != 0; };
    if (!finite(r) || !finite(g) || !finite(b) || !finite(a))
      return false;
    if (r < 0.0f || r > 1.0f || g < 0.0f || g > 1.0f || b < 0.0f || b > 1.0f ||
        a < 0.0f || a > 1.0f)
      return false;
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = a;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
static bool TryGetFontHeightFromPtr(void *p, float &outFontH) {
  if (!p)
    return false;
  uintptr_t v = (uintptr_t)p;
  if (v < 0x10000 || v > 0x7FFFFFFFFFFF)
    return false;
  if (!IsSafeRead((void *)v, 0x20))
    return false;
  __try {
    IW6Font::Font_s *f = (IW6Font::Font_s *)p;
    int h = f->fontHeight;
    if (h < 6 || h > 200)
      return false;
    outFontH = (float)h;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static const char *HudRouteChannelName(HudRouteChannel ch);
static bool IsLikelyHudRuntimeTailText(const std::string &rawText);
static bool HasRecentHudRuntimeTailJoinPromptActive(DWORD maxAgeMs);
static bool IsDirectAddCmdObjHudHintKey(const std::string &key);
static bool ShouldForceCenterAlignGameplayTranslation(const std::string &key, const std::string &resolvedKorText = "");
static bool GetRecentHudRuntimeTailJoinPromptKey(DWORD maxAgeMs,
                                                 std::string &outKey);
static bool TryBuildDirectAddCmdObjHudHintText(
    const std::string &key, const std::string &runtimeEnglish, float hintX,
    float hintY, unsigned int callerOffset, std::string &outText);
static bool IsObjectiveHudKeyType(HudKeyType t);
static bool HasRecentObjectiveObservationForKey(const std::string &key,
                                                DWORD maxAgeMs = 4000);
static OverlaySource OverlaySourceFromHintNativeSource(unsigned char nativeSource);
static OverlaySource OverlaySourceFromNativeSourceToken(uint8_t sourceToken);
static uint8_t HintSourceTokenFromRouteSource(unsigned char sourceTag);
static uint8_t HintSourceTokenFromHudNativeEntry(const HudNativeEntry &entry);
static bool IsObjHudHintFamilySourceToken(uint8_t sourceToken);
static bool ResolveHudFamilyRouteFromNative(const HudNativeEntry &entry,
                                            HudRouteChannel &outChannel);
static int GetHintEntrySlotBucket(const HintOverlayEntry &entry);
static uint64_t GetHintEntryConsumerId(const HintOverlayEntry &entry);
static uint64_t MakeHintConsumerIdFromNative(const HudNativeEntry &entry,
                                             int slotBucket);
static void RemoveHintEntryByKey(const std::string &key);
static void RemoveObjHudHintEntryByKey(const std::string &key);
static void RemoveHudNativeEntriesByKey(const std::string &key);
static bool ClassifyNativeHudDisplayChannelDirect(const HudNativeEntry &entry,
                                                  HudRouteChannel &outChannel);
static bool ApplyNativeHudEntryDirect(const std::string &key,
                                      const HudNativeEntry &entry,
                                      const std::string *englishRuntime);
static bool HasMixedHudHintPromptCallers(const std::string &key,
                                         unsigned int currentCallerOffset,
                                         DWORD maxAgeMs);
static void NoteRecentHudRuntimeTailJoinPrompt(const std::string &key, float x,
                                               float y,
                                               unsigned int callerOffset);
static bool IsHud560NonPromptGameplayCaptureKey(const std::string &key) {
  return key == "PLATFORM_THROWBACKGRENADE" ||
         key == "WEAPON_NO_WEAPON_AMMO" ||
         ShouldForceCenterAlignGameplayTranslation(key);
}

struct Hud560LayoutProbe {
  float x = 0.0f;
  float y = 0.0f;
  float sx = 1.0f;
  float sy = 1.0f;
  int style = 0;
  int score = -1;
  int layout = 0;
  bool hasColor = false;
  float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float fontH = 0.0f;
};

static bool IsPlausibleHud560XY(float x, float y) {
  if (!std::isfinite(x) || !std::isfinite(y)) {
    return false;
  }
  return (x > -200.0f && x < 5000.0f && y > -200.0f && y < 3000.0f);
}

static bool IsPlausibleHud560Scale(float s) {
  if (!std::isfinite(s)) {
    return false;
  }
  return (s > 0.02f && s < 10.0f);
}

static Hud560LayoutProbe ProbeHud560DrawLayout(void *fontPtr, void *p5, void *p6,
                                               void *p7, void *p8, void *p9,
                                               void *p10, void *p11,
                                               void *p12) {
  const float f5 = FloatFromParamBits(p5);
  const float f6 = FloatFromParamBits(p6);
  const float f7 = FloatFromParamBits(p7);
  const float f8 = FloatFromParamBits(p8);
  const float f9 = FloatFromParamBits(p9);
  const float f10 = FloatFromParamBits(p10);

  auto scoreLayout = [&](float x, float y, float sx, float sy, void *colorPtr,
                         void *font, uintptr_t styleVal,
                         int layoutId) -> Hud560LayoutProbe {
    Hud560LayoutProbe c;
    c.x = x;
    c.y = y;
    c.sx = sx;
    c.sy = sy;
    c.layout = layoutId;

    int score = 0;
    if (IsPlausibleHud560XY(x, y)) {
      score += 3;
    }
    if (IsPlausibleHud560Scale(sx) && IsPlausibleHud560Scale(sy)) {
      score += 3;
    }

    float col[4] = {0};
    if (TryReadFloat4Ptr(colorPtr, col)) {
      score += 3;
      c.hasColor = true;
      c.color[0] = col[0];
      c.color[1] = col[1];
      c.color[2] = col[2];
      c.color[3] = col[3];
    }

    float fh = 0.0f;
    if (TryGetFontHeightFromPtr(font, fh)) {
      score += 2;
      c.fontH = fh;
    }

    const int style = (int)(styleVal & 0xFFFFFFFFu);
    if (style >= 0 && style <= 64) {
      score += 1;
      c.style = style;
    }

    c.score = score;
    return c;
  };

  Hud560LayoutProbe best =
      scoreLayout(f5, f6, f7, f8, p10, fontPtr, (uintptr_t)p11, 1);
  {
    const Hud560LayoutProbe c2 =
        scoreLayout(f6, f7, f8, f9, p11, fontPtr, (uintptr_t)p12, 2);
    if (c2.score > best.score) {
      best = c2;
    }
  }
  {
    const Hud560LayoutProbe c3 =
        scoreLayout(f5, f6, f7, f8, p11, fontPtr, (uintptr_t)p12, 3);
    if (c3.score > best.score) {
      best = c3;
    }
  }
  {
    const Hud560LayoutProbe c4 =
        scoreLayout(f7, f8, f9, f10, p11, fontPtr, (uintptr_t)p12, 4);
    if (c4.score > best.score) {
      best = c4;
    }
  }
  return best;
}

void __fastcall Detour_HUD_DrawText560(void *p1, void *p2, void *p3, void *p4,
                                        void *p5, void *p6, void *p7, void *p8,
                                        void *p9, void *p10, void *p11,
                                        void *p12) {
  // Prove the hook is live without spamming. Useful when native capture isn't
  // producing key-matched logs yet.
  static bool s_detourLiveLogged = false;
  if (!s_detourLiveLogged) {
    s_detourLiveLogged = true;
    LogToFile("[HUD560] Detour active");
  }

  bool suppressOriginalHud560 = false;

  // --- Native HUD prompt capture (center hints / object prompts) ---
  // This hook should be gameplay-only (menus use R_AddCmdDrawText), so we can
  // safely use it to align Korean overlays with the exact native draw params.
  // IMPORTANT: Keep this lightweight and avoid allocating/logging excessively.
  if (TextHook_IsHudNativeEnabled()) {
    static uintptr_t s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
    const uintptr_t hud560CallerOffset =
        (uintptr_t)_ReturnAddress() - s_moduleBase;
    std::string raw;
    const char *rawPtr = nullptr;
    int rawParamIndex = -1;

    // Candidate layout is not stable across all HUD paths.
    // Probe all params as potential text pointers and prefer Press/Hold-like text.
    // This avoids missing alternate HUD560 call signatures used by some prompts.
    const char *bestAnyPtr = nullptr;
    std::string bestAnyRaw;
    int bestAnyIdx = -1;
    const void *ptrs[12] = {p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12};
    for (int idx = 0; idx < 12; ++idx) {
      std::string candRaw;
      if (!TryReadCStringPreview((const char *)ptrs[idx], 192, candRaw)) {
        continue;
      }
      const bool candPressHold = LooksLikePressHoldPrompt(candRaw);
      const bool candHudKey = LooksLikeHudLocalizationKey(candRaw);
      if (candPressHold || candHudKey) {
        raw = candRaw;
        rawPtr = (const char *)ptrs[idx];
        rawParamIndex = idx;
        break;
      }
      if (!bestAnyPtr) {
        bestAnyPtr = (const char *)ptrs[idx];
        bestAnyRaw = candRaw;
        bestAnyIdx = idx;
      }
    }
    if (raw.empty() && bestAnyPtr) {
      raw = bestAnyRaw;
      rawPtr = bestAnyPtr;
      rawParamIndex = bestAnyIdx;
    }

    bool handledScriptCountdown = false;
    const bool isPressOrHold = LooksLikePressHoldPrompt(raw);
    const bool isHudKeyText = LooksLikeHudLocalizationKey(raw);
    if (!raw.empty()) {
      static std::unordered_map<uint64_t, DWORD> s_callsiteDiagLogTick;
      const DWORD nowDiag = GetTickCount();
      const uint64_t sig =
          (((uint64_t)hud560CallerOffset & 0xFFFFFFull) << 8) ^
          (uint64_t)(unsigned int)(rawParamIndex + 1);
      auto itDiag = s_callsiteDiagLogTick.find(sig);
      if (itDiag == s_callsiteDiagLogTick.end() ||
          (nowDiag - itDiag->second) > 1800) {
        s_callsiteDiagLogTick[sig] = nowDiag;
        std::string preview = TrimSpaces(StripColorCodes(raw));
        if (preview.size() > 96) {
          preview = Utf8Preview(preview, 96);
        }
        char dbuf[320];
        sprintf_s(dbuf,
                  "[HUD560-CALLSITE] caller=+0x%llX p=%d press=%d hudkey=%d raw=\"%s\"",
                  (unsigned long long)hud560CallerOffset, rawParamIndex,
                  isPressOrHold ? 1 : 0, isHudKeyText ? 1 : 0, preview.c_str());
        LogToFile(dbuf);
      }
    }

    // ── EARLY TAIL-ONLY FAST PATH ──────────────────────────────────────
    // When a tail-join prompt is active (weapon swap/pickup) and this text
    // looks like a standalone weapon name (not a prompt, not a loc key),
    // skip the entire key resolution chain and jump directly to tail
    // capture + suppress.  This eliminates the per-frame full-failure
    // traversal of g_RuntimeBindingTemplateToKey / g_BindingTemplates /
    // g_PrefixToKey for short weapon-name texts that always fail resolve.
    if (!raw.empty() && !isPressOrHold && !isHudKeyText &&
        HasRecentHudRuntimeTailJoinPromptActive(350) &&
        IsLikelyHudRuntimeTailText(raw)) {
      const Hud560LayoutProbe tailProbe =
          ProbeHud560DrawLayout(p4, p5, p6, p7, p8, p9, p10, p11, p12);
      if (tailProbe.score >= 3 &&
          IsPlausibleHud560XY(tailProbe.x, tailProbe.y)) {
        float tailX = tailProbe.x;
        float tailY = tailProbe.y;
        // Scale virtual 480-space coords to active area + offset so that
        // the fragment Y matches the OBJHUDHINT query Y at any aspect ratio.
        const float areaH = (g_ActiveArea.height > 1.0f)
                                ? g_ActiveArea.height
                                : GetScreenHeightApprox();
        if (areaH > 0.0f) {
          const float scale = areaH / 480.0f;
          tailX *= scale;
          tailY *= scale;
          tailX += g_ActiveArea.offsetX;
          tailY += g_ActiveArea.offsetY;
        }
        CaptureHudRuntimeTailFragment("", raw, tailX, tailY,
                                      (unsigned int)hud560CallerOffset, true,
                                      !TextHook_IsPauseMenuLikely());
        static std::unordered_map<unsigned int, DWORD>
            s_earlyTailSuppressLogTick;
        const DWORD nowEarlySuppress = GetTickCount();
        const unsigned int earlyKey = (unsigned int)hud560CallerOffset;
        auto itEarly = s_earlyTailSuppressLogTick.find(earlyKey);
        if (itEarly == s_earlyTailSuppressLogTick.end() ||
            (nowEarlySuppress - itEarly->second) > 1500) {
          s_earlyTailSuppressLogTick[earlyKey] = nowEarlySuppress;
          char sbuf[320];
          sprintf_s(
              sbuf,
              "[HUD-TAIL-SUPPRESS-EARLY] caller=+0x%llX text=\"%.96s\"",
              (unsigned long long)hud560CallerOffset, raw.c_str());
          LogToFile(sbuf);
        }
        return;
      }
      // Tail probe failed; fall through to normal processing.
    }

    if (!raw.empty() && !TextHook_IsPauseMenuLikely()) {
      HudScriptCountdownMatch countdownMatch{};
      if (TryResolveHudScriptCountdownText(raw, countdownMatch)) {
        const Hud560LayoutProbe timerProbe =
            ProbeHud560DrawLayout(p4, p5, p6, p7, p8, p9, p10, p11, p12);
        if (timerProbe.score >= 3 &&
            IsPlausibleHud560XY(timerProbe.x, timerProbe.y)) {
          const float screenWidth = GetScreenWidthApprox();
          const float screenHeight = GetScreenHeightApprox();
          float renderX = timerProbe.x;
          float renderY = timerProbe.y;
          if (screenWidth > 0.0f) {
            renderX = (timerProbe.x / 640.0f) * screenWidth;
          }
          if (screenHeight > 0.0f) {
            renderY = (timerProbe.y / 480.0f) * screenHeight;
          }

          float renderFontHeight = timerProbe.fontH;
          if (!std::isfinite(renderFontHeight) || renderFontHeight <= 0.01f) {
            renderFontHeight = 35.0f;
          }
          if (screenHeight > 0.0f) {
            renderFontHeight = (renderFontHeight / 480.0f) * screenHeight;
          }

          float timerColor[4] = {0.8f, 1.0f, 0.8f, 1.0f};
          if (timerProbe.hasColor) {
            timerColor[0] = timerProbe.color[0];
            timerColor[1] = timerProbe.color[1];
            timerColor[2] = timerProbe.color[2];
            timerColor[3] = timerProbe.color[3];
          }

          const float renderScale =
              (std::isfinite(timerProbe.sy) && timerProbe.sy > 0.01f &&
               timerProbe.sy < 6.0f)
                  ? timerProbe.sy
                  : 1.0f;
          QueueHudScriptCountdownOverlay(
              countdownMatch, renderX, renderY, renderScale, renderFontHeight,
              timerProbe.style, timerColor, true,
              (unsigned int)hud560CallerOffset, "hud560");
          suppressOriginalHud560 = true;
          handledScriptCountdown = true;
        } else {
          static std::unordered_map<std::string, DWORD> s_timerLayoutMissLogTick;
          const DWORD nowTimer = GetTickCount();
          auto itTimer = s_timerLayoutMissLogTick.find(countdownMatch.key);
          if (itTimer == s_timerLayoutMissLogTick.end() ||
              (nowTimer - itTimer->second) > 1500) {
            s_timerLayoutMissLogTick[countdownMatch.key] = nowTimer;
            char tbuf[320];
            sprintf_s(
                tbuf,
                "[HUD-TIMER-LAYOUT-MISS] src=hud560 caller=+0x%llX key=%s score=%d x=%.2f y=%.2f raw=\"%.96s\"",
                (unsigned long long)hud560CallerOffset,
                countdownMatch.key.c_str(), timerProbe.score, timerProbe.x,
                timerProbe.y, raw.c_str());
            LogToFile(tbuf);
          }
        }
      }
    }

    if (!raw.empty() && !handledScriptCountdown) {
      EnsureTranslationsLoaded();

      std::string key;
      std::string hudAutoReason;
      bool resolvedHudKey = ResolveKeyFromEnglishInternal(raw, key);
      bool resolvedFromRawKeyText = false;
      if (!resolvedHudKey && isHudKeyText) {
        const std::string rawKey = TrimSpaces(StripColorCodes(raw));
        std::string rawReason;
        if (!rawKey.empty() && IsGameplayHudKeyName(rawKey, &rawReason)) {
          key = rawKey;
          resolvedHudKey = true;
          resolvedFromRawKeyText = true;
          hudAutoReason = rawReason;
        }
      }
      const bool allowResolvedNonPromptGameplay =
          resolvedHudKey && IsHud560NonPromptGameplayCaptureKey(key);
      const bool isGameplayHudKey =
          resolvedHudKey && key.find("SUBTITLE_") != 0 &&
          (IsGameplayHudKeyName(key, &hudAutoReason) ||
           allowResolvedNonPromptGameplay);
      if (isGameplayHudKey) {
        // Keep runtime english?봩ey map fresh for draw-time variants.
        TextHook_RegisterRuntimeEnglishKey(key.c_str(), raw.c_str());
        if (resolvedFromRawKeyText && isGameplayHudKey) {
          static std::unordered_map<std::string, DWORD> s_rawKeyResolveLogTick;
          const DWORD nowRawKey = GetTickCount();
          auto itRaw = s_rawKeyResolveLogTick.find(key);
          if (itRaw == s_rawKeyResolveLogTick.end() ||
              (nowRawKey - itRaw->second) > 2000) {
            s_rawKeyResolveLogTick[key] = nowRawKey;
            char rbuf[320];
            sprintf_s(rbuf,
                      "[HUD560-RAWKEY] caller=+0x%llX p=%d key=%s",
                      (unsigned long long)hud560CallerOffset, rawParamIndex,
                      key.c_str());
            LogToFile(rbuf);
          }
        }
        if (!hudAutoReason.empty() && isGameplayHudKey) {
          MaybeLogHudKeyAuto(key, hud560CallerOffset,
                             std::string("hud560_") + hudAutoReason);
        }
        if (IsDirectAddCmdObjHudHintKey(key)) {
          // Direct AddCmd keys still need HUD560 fallback when the engine
          // does not emit the matching AddCmd prompt on a given frame.
          TextHook_OnHudKey(key.c_str());
        }
        const Hud560LayoutProbe best =
            ProbeHud560DrawLayout(p4, p5, p6, p7, p8, p9, p10, p11, p12);

        // Press/Hold prompts usually provide full params.
        // Key-style payloads (CORNERED_*/PLATFORM_*) often provide sparse params;
        // accept plausible XY in that case and fall back on default scale later.
        int minScore = 6;
        if (!isPressOrHold || key.rfind("CORNERED_BINOCULARS_", 0) == 0) {
          minScore = 3;
        }

        const bool canUseSparseNonPromptLayout =
            allowResolvedNonPromptGameplay &&
            IsPlausibleHud560XY(best.x, best.y);
        if (best.score >= minScore || canUseSparseNonPromptLayout) {
          HudNativeEntry entry{};
          entry.key = key;
          entry.x = best.x;
          entry.y = best.y;
          if (ShouldForceCenterAlignGameplayTranslation(key) &&
              !IsPlausibleHud560XY(entry.x, entry.y)) {
            entry.x = 320.0f;
            entry.y = 240.0f;
          }
          entry.xScale = IsPlausibleHud560Scale(best.sx) ? best.sx : 0.0f;
          entry.yScale = IsPlausibleHud560Scale(best.sy) ? best.sy : 0.0f;
          // Native HUD hints commonly use ~35px base font on this path.
          entry.fontHeight = (best.fontH > 0.0f) ? best.fontH : 35.0f;
          entry.style = best.style;
          entry.lastUpdate = GetTickCount();
          entry.source = HUD_NATIVE_RENDER;
          entry.sourceToken = NATIVE_HUD_SOURCE_HUD560;
          entry.callerOffset = (unsigned int)hud560CallerOffset;
          // Objective overlays are owned by the dedicated objective subsystem.
          // Do not let HUD560 feed objective fallback state into native HUD.
          // HUD560 cannot detect objectives, and this has been confirmed through multiple experiments.
          entry.objectiveLike = false;
          entry.objectiveChannel = (unsigned char)OBJ_CHANNEL_NONE;
          // HUD draw coordinates are height-relative in a 480-high virtual space.
          // Let the renderer apply a uniform Y-based scale for X/Y (aspect-safe).
          entry.virtualW = allowResolvedNonPromptGameplay ? 640 : 0;
          entry.virtualH = 480;
          entry.valid = true;
          if (best.hasColor) {
            entry.color[0] = best.color[0];
            entry.color[1] = best.color[1];
            entry.color[2] = best.color[2];
            entry.color[3] = best.color[3];
          } else {
            entry.color[0] = 1.0f;
            entry.color[1] = 1.0f;
            entry.color[2] = 1.0f;
            entry.color[3] = 0.6f;
          }

          TextHook_UpdateHudNativeEntry(entry);
          OverlayDrawEvent evt{};
          {
            float normY = -1.0f;
            if (entry.virtualH > 0) {
              normY = entry.y / (float)entry.virtualH;
            }
            int slotBucket = 0;
            if (normY >= 0.0f) {
              if (normY < 0.18f) {
                slotBucket = 1;
              } else if (normY < 0.42f) {
                slotBucket = 2;
              } else if (normY < 0.62f) {
                slotBucket = 3;
              } else if (normY < 0.78f) {
                slotBucket = 4;
              } else {
                slotBucket = 5;
              }
            }
            evt.frameId = 0;
            evt.tick = entry.lastUpdate;
            evt.source = OVERLAY_SOURCE_HUD560;
            evt.callerOffset = entry.callerOffset;
            evt.key = key;
            evt.english = raw;
            evt.x = entry.x;
            evt.y = entry.y;
            evt.sx = entry.xScale;
            evt.sy = entry.yScale;
            evt.fontH = entry.fontHeight;
            evt.alpha = entry.color[3];
            evt.style = entry.style;
            evt.virtualW = entry.virtualW;
            evt.virtualH = entry.virtualH;
            evt.flags =
                (((unsigned int)((unsigned short)(slotBucket & 0xFFFF))) << 16) |
                ((unsigned int)(best.layout & 0xFFFF));
            evt.fxBirth = 0;
            evt.fxLetter = 0;
            evt.serverTime = 0;
            evt.channelHint = OBJ_CHANNEL_NONE;
            evt.consumerId = TextHook_MakeHudConsumerId(
                evt.source, evt.callerOffset, slotBucket, evt.virtualW,
                evt.virtualH);
            evt.routeChannel = HUD_ROUTE_UNSPECIFIED;
            evt.routeConfidence = 0.0f;
          }

          // Signal tail-join prompt active from HUD560 path as well as
          // AddCmd.  Without this, anonymous tail captures (weapon names)
          // fail the 200ms gate check when the prompt arrives via HUD560
          // before AddCmd has processed it.
          if (IsHudRuntimeTailJoinPromptKey(key)) {
            NoteRecentHudRuntimeTailJoinPrompt(
                key, entry.x, entry.y, (unsigned int)hud560CallerOffset);
          }
          const bool queued = TextHook_OnHudKey(key.c_str());
          CaptureHudRuntimeTailFragment(key, raw, entry.x, entry.y,
                                        entry.callerOffset, true,
                                        !TextHook_IsPauseMenuLikely());
          ApplyNativeHudEntryDirect(key, entry, &raw);
          if (!allowResolvedNonPromptGameplay &&
              (isPressOrHold || key.find("_PROMPT") != std::string::npos) &&
              HasMixedHudHintPromptCallers(key,
                                           (unsigned int)hud560CallerOffset,
                                           900)) {
            suppressOriginalHud560 = true;
            static std::unordered_map<std::string, DWORD>
                s_promptSuppressLogTick;
            const DWORD nowSuppress = GetTickCount();
            auto itSuppress = s_promptSuppressLogTick.find(key);
            if (itSuppress == s_promptSuppressLogTick.end() ||
                (nowSuppress - itSuppress->second) > 1500) {
              s_promptSuppressLogTick[key] = nowSuppress;
              char sbuf[320];
              sprintf_s(
                  sbuf,
                  "[HUD560-PROMPT-SUPPRESS] key=%s caller=+0x%llX reason=mixed_hudhintsinglewinner",
                  key.c_str(), (unsigned long long)hud560CallerOffset);
              LogToFile(sbuf);
            }
          }
          if (allowResolvedNonPromptGameplay) {
            suppressOriginalHud560 = true;
          }

          // --- HUDHINT English suppression ---
          // Suppress English for ALL resolved keys that have Korean translations.
          // Data capture (ApplyNativeHudEntryDirect) already ran above, so
          // the Korean renderer's pipeline is unaffected.
          if (!suppressOriginalHud560 && !key.empty()) {
            EnsureTranslationsLoaded();
            auto itKr = g_KeyToKorean.find(ToUpper(key));
            if (itKr != g_KeyToKorean.end() && !itKr->second.empty()) {
              suppressOriginalHud560 = true;
            }
          }

          if (!queued) {
            static std::mutex s_missMtx;
            static std::unordered_set<std::string> s_logged;
            bool shouldLog = false;
            {
              std::lock_guard<std::mutex> lk(s_missMtx);
              if (s_logged.size() < 600 && s_logged.insert(key).second) {
                shouldLog = true;
              }
            }
            if (shouldLog) {
              std::string preview = StripColorCodes(raw);
              preview = TrimSpaces(preview);
              if (preview.size() > 140)
                preview.resize(140);
              char buf[512];
              sprintf_s(buf, "[HUD560-MISS] key=%s eng=\"%s\"",
                        key.c_str(), preview.c_str());
              LogToFile(buf);
            }
          }

          static std::unordered_map<std::string, DWORD> s_lastLogByKey;
          const DWORD kLogIntervalMs = 3000;
          DWORD now = GetTickCount();
          auto it = s_lastLogByKey.find(key);
          if (it == s_lastLogByKey.end() || (now - it->second) > kLogIntervalMs) {
            s_lastLogByKey[key] = now;
            if (s_lastLogByKey.size() > 96) {
              for (auto it2 = s_lastLogByKey.begin(); it2 != s_lastLogByKey.end();) {
                if ((now - it2->second) > 30000) it2 = s_lastLogByKey.erase(it2);
                else ++it2;
              }
            }

            std::string preview = StripColorCodes(raw);
            preview = TrimSpaces(preview);
            if (preview.size() > 80) preview.resize(80);

            char buf[512];
            sprintf_s(buf,
                      "[HUD560] key='%s' layout=%d x=%.1f y=%.1f sx=%.3f sy=%.3f a=%.2f h=%.1f text=\"%s\"",
                      key.c_str(), best.layout,
                      entry.x, entry.y, entry.xScale, entry.yScale,
                      entry.color[3], entry.fontHeight, preview.c_str());
            LogToFile(buf);
          }
        } else {
          // Resolution succeeded but params look odd. Log once per key for diagnostics.
          static std::unordered_set<std::string> s_badLayoutLogged;
          if (s_badLayoutLogged.size() < 256 &&
              s_badLayoutLogged.insert(key).second) {
            char buf[256];
            sprintf_s(buf,
                      "[HUD560] WARN key='%s' candidate params rejected (score=%d layout=%d x=%.2f y=%.2f)",
                      key.c_str(), best.score, best.layout, best.x, best.y);
            LogToFile(buf);
          }
        }
      } else if (isPressOrHold || isHudKeyText) {
        // Missing English->key mapping. Log a few to help improve templates.
        static int s_resolveFailLog = 0;
          if (s_resolveFailLog < 50) {
            s_resolveFailLog++;
            std::string preview = StripColorCodes(raw);
            preview = TrimSpaces(preview);
            if (preview.size() > 140) preview.resize(140);
            char buf[512];
            sprintf_s(
                buf,
                "[HUD560-RESOLVE-FAIL] caller=+0x%llX p=%d press=%d hudkey=%d eng=\"%s\"",
                (unsigned long long)hud560CallerOffset, rawParamIndex,
                isPressOrHold ? 1 : 0, isHudKeyText ? 1 : 0, preview.c_str());
            LogToFile(buf);
          }
        }
      }

    // Reordered: cheapest checks first (caller offset, booleans),
    // then atomic prompt gate, then the expensive string-scanning
    // IsLikelyHudRuntimeTailText last — avoids per-call string work
    // when no tail-join prompt is active.
    const bool suppressHud560TailOnly =
        !raw.empty() && !isPressOrHold && !isHudKeyText &&
        HasRecentHudRuntimeTailJoinPromptActive(350) &&
        IsLikelyHudRuntimeTailText(raw);
    if (suppressHud560TailOnly) {
      const Hud560LayoutProbe tailProbe =
          ProbeHud560DrawLayout(p4, p5, p6, p7, p8, p9, p10, p11, p12);
      bool capturedTail = false;
      if (tailProbe.score >= 3 && IsPlausibleHud560XY(tailProbe.x, tailProbe.y)) {
        float tailX = tailProbe.x;
        float tailY = tailProbe.y;
        const float areaH2 = (g_ActiveArea.height > 1.0f)
                                  ? g_ActiveArea.height
                                  : GetScreenHeightApprox();
        if (areaH2 > 0.0f) {
          const float scale = areaH2 / 480.0f;
          tailX *= scale;
          tailY *= scale;
          tailX += g_ActiveArea.offsetX;
          tailY += g_ActiveArea.offsetY;
        }
        CaptureHudRuntimeTailFragment("", raw, tailX, tailY,
                                      (unsigned int)hud560CallerOffset, true,
                                      !TextHook_IsPauseMenuLikely());
        capturedTail = true;
      }
      if (!capturedTail) {
        goto hud560_tail_passthrough;
      }
      // Throttled suppress log — use callerOffset as cheap key to avoid
      // per-frame string allocation (StripColorCodes was already done inside
      // CaptureHudRuntimeTailFragment above).
      static std::unordered_map<unsigned int, DWORD> s_hud560TailSuppressLogTick;
      const DWORD nowTailSuppress = GetTickCount();
      const unsigned int suppressKey = (unsigned int)hud560CallerOffset;
      auto itSuppress = s_hud560TailSuppressLogTick.find(suppressKey);
      if (itSuppress == s_hud560TailSuppressLogTick.end() ||
          (nowTailSuppress - itSuppress->second) > 1500) {
        s_hud560TailSuppressLogTick[suppressKey] = nowTailSuppress;
        char sbuf[320];
        sprintf_s(
            sbuf,
            "[HUD-TAIL-SUPPRESS] src=hud560 caller=+0x%llX text=\"%.96s\"",
            (unsigned long long)hud560CallerOffset, raw.c_str());
        LogToFile(sbuf);
      }
      return;
    }
hud560_tail_passthrough:;
  }

  // Consume g_lastIntroSlcIdx (set by IntroRenderFn) to prevent stale data
  if (g_lastIntroSlcIdx >= 0) {
    g_lastIntroSlcIdx = -1;
  }

  if (suppressOriginalHud560) {
    return;
  }

  // Call original with ALL 12 params ??preserves full register+stack state
  if (Original_HUD_DrawText560) {
    Original_HUD_DrawText560(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11,
                             p12);
  }
}
static bool IsLikelyHudRuntimeTailText(const std::string &rawText) {
  std::string cleaned = TrimSpaces(StripColorCodes(rawText));
  if (cleaned.size() < 2 || cleaned.size() > 40) {
    return false;
  }
  if (LooksLikePressHoldPrompt(cleaned) || LooksLikeHudLocalizationKey(cleaned)) {
    return false;
  }
  if (HasBindingPlaceholderToken(cleaned)) {
    return false;
  }
  if (!cleaned.empty() && cleaned[0] == '@') {
    return false;
  }
  if (CountWords(cleaned) > 7) {
    return false;
  }
  // Reject distance/measurement texts like "0.3m", "1.5m", "2.0ft", "15"
  // that pollute the tail fragment pool from gameplay HUD elements.
  {
    bool allNumericUnit = true;
    bool hasDigit = false;
    for (size_t ci = 0; ci < cleaned.size(); ++ci) {
      unsigned char ch = (unsigned char)cleaned[ci];
      if (ch >= '0' && ch <= '9') {
        hasDigit = true;
      } else if (ch == '.' || ch == ' ') {
        // allowed in numeric measurements
      } else if ((ch == 'm' || ch == 'M' || ch == 'f' || ch == 'F' ||
                  ch == 't' || ch == 'T') &&
                 ci >= cleaned.size() - 2) {
        // unit suffix at or near end
      } else {
        allNumericUnit = false;
        break;
      }
    }
    if (allNumericUnit && hasDigit) {
      return false;
    }
  }
  bool hasReadable = false;
  for (char c : cleaned) {
    unsigned char uc = (unsigned char)c;
    if (std::isalnum(uc) || c == '_' || c == '-' || c == '+' || c == '.' || c == '/') {
      hasReadable = true;
      break;
    }
  }
  return hasReadable;
}
static bool IsHudRuntimeTailJoinPromptKey(const std::string &key) {
  return (key == "PLATFORM_SWAPWEAPONS" ||
          key == "PLATFORM_SWAPWEAPONSGAMEPAD" ||
          key == "PLATFORM_PICKUPNEWWEAPON" ||
          key == "PLATFORM_PICKUPNEWWEAPONGAMEPAD");
}

struct RecentHudRuntimeTailJoinPromptState {
  std::string key;
  float x = 0.0f;
  float y = 0.0f;
  unsigned int callerOffset = 0;
  DWORD tick = 0;
};

static std::mutex g_RecentHudRuntimeTailJoinPromptMutex;
static std::unordered_map<std::string, RecentHudRuntimeTailJoinPromptState>
    g_RecentHudRuntimeTailJoinPromptByKey;
static std::atomic<DWORD> g_lastTailJoinPromptTick{0};

static void NoteRecentHudRuntimeTailJoinPrompt(const std::string &key, float x,
                                               float y,
                                               unsigned int callerOffset) {
  const std::string keyNorm = ToUpperAscii(TrimSpaces(key));
  if (!IsHudRuntimeTailJoinPromptKey(keyNorm)) {
    return;
  }
  const DWORD now = GetTickCount();
  g_lastTailJoinPromptTick.store(now, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_RecentHudRuntimeTailJoinPromptMutex);
  RecentHudRuntimeTailJoinPromptState &state =
      g_RecentHudRuntimeTailJoinPromptByKey[keyNorm];
  state.key = keyNorm;
  state.x = x;
  state.y = y;
  state.callerOffset = callerOffset;
  state.tick = now;
  if (g_RecentHudRuntimeTailJoinPromptByKey.size() > 16) {
    for (auto it = g_RecentHudRuntimeTailJoinPromptByKey.begin();
         it != g_RecentHudRuntimeTailJoinPromptByKey.end();) {
      const DWORD age = (now >= it->second.tick) ? (now - it->second.tick) : 0;
      if (age > 4000) {
        it = g_RecentHudRuntimeTailJoinPromptByKey.erase(it);
      } else {
        ++it;
      }
    }
  }
}

static bool TryGetRecentHudRuntimeTailJoinPromptState(
    DWORD maxAgeMs, RecentHudRuntimeTailJoinPromptState &out) {
  out = {};
  const DWORD now = GetTickCount();
  std::lock_guard<std::mutex> lock(g_RecentHudRuntimeTailJoinPromptMutex);
  DWORD bestTick = 0;
  for (const auto &kv : g_RecentHudRuntimeTailJoinPromptByKey) {
    const DWORD age = (now >= kv.second.tick) ? (now - kv.second.tick) : 0;
    if (age > maxAgeMs) {
      continue;
    }
    if (out.key.empty() || kv.second.tick > bestTick) {
      out = kv.second;
      bestTick = kv.second.tick;
    }
  }
  return !out.key.empty();
}

static bool IsDirectAddCmdObjHudHintKey(const std::string &key) {
  if (key.empty()) {
    return false;
  }
  const std::string up = ToUpperAscii(TrimSpaces(key));
  return (IsHudRuntimeTailJoinPromptKey(up) || up == "PLATFORM_MANTLE" ||
          up == "PLATFORM_MANTLEGAMEPAD" ||
          up == "DEER_HUNT_MATV_HINT" ||
          up == "DEER_HUNT_MATV_HINT_RELOAD");
}

static std::string GetDirectAddCmdObjHudHintBindingFallback(
    const std::string &keyNorm, const std::string &englishFallback) {
  std::string inferred =
      BindingResolver::ExtractKeyFromEnglish(TrimSpaces(englishFallback));
  if (!inferred.empty()) {
    return inferred;
  }

  if (keyNorm == "PLATFORM_MANTLE") {
    inferred = BindingResolver::ResolveBindingDisplay("+gostand");
    if (inferred.empty()) {
      inferred = "SPACE";
    }
    return inferred;
  }

  if (keyNorm == "DEER_HUNT_MATV_HINT") {
    inferred = BindingResolver::ResolveBindingDisplay("+activate");
    if (inferred.empty()) {
      inferred = "F";
    }
    return inferred;
  }

  if (keyNorm == "DEER_HUNT_MATV_HINT_RELOAD") {
    inferred = BindingResolver::ResolveBindingDisplay("+usereload");
    if (inferred.empty()) {
      inferred = "R";
    }
    return inferred;
  }

  if (keyNorm == "PLATFORM_SWAPWEAPONS" ||
      keyNorm == "PLATFORM_PICKUPNEWWEAPON") {
    inferred = BindingResolver::ResolveBindingDisplay("+activate");
    if (inferred.empty()) {
      inferred = "F";
    }
    return inferred;
  }

  return "";
}

static bool TryBuildDirectAddCmdObjHudHintText(
    const std::string &key, const std::string &runtimeEnglish, float hintX,
    float hintY, unsigned int callerOffset, std::string &outText) {
  outText.clear();
  const std::string keyNorm = ToUpperAscii(TrimSpaces(key));
  if (!IsDirectAddCmdObjHudHintKey(keyNorm)) {
    return false;
  }

  auto itKor = g_KeyToKorean.find(keyNorm);
  if (itKor == g_KeyToKorean.end() || itKor->second.empty()) {
    return false;
  }

  std::string englishFallback = runtimeEnglish;
  if (englishFallback.empty()) {
    auto itEng = g_KeyToEnglish.find(keyNorm);
    if (itEng != g_KeyToEnglish.end()) {
      englishFallback = itEng->second;
    }
  }
  const bool needsRuntimeEnglish =
      englishFallback.empty() || englishFallback == keyNorm ||
      englishFallback.find("&&") != std::string::npos ||
      englishFallback.find("$$") != std::string::npos ||
      englishFallback.find('{') != std::string::npos ||
      englishFallback.find("[{+") != std::string::npos;
  if (needsRuntimeEnglish) {
    std::string runtimeRecentEnglish;
    if (TextHook_GetRuntimeEnglishForKeyRecent(keyNorm, runtimeRecentEnglish,
                                               1800)) {
      runtimeRecentEnglish = TrimSpaces(runtimeRecentEnglish);
      if (!runtimeRecentEnglish.empty()) {
        englishFallback = runtimeRecentEnglish;
      }
    }
  }

  std::string displayText =
      BindingResolver::ResolveBindingsInText(itKor->second, englishFallback);
  TextHook_ResolveLocalizedTemplateArgs(keyNorm, englishFallback, displayText);
  if (displayText.find("&&") != std::string::npos ||
      displayText.find("$$") != std::string::npos) {
    const std::string fallbackKeyDisplay =
        GetDirectAddCmdObjHudHintBindingFallback(keyNorm, englishFallback);
    if (!fallbackKeyDisplay.empty()) {
      displayText = BindingResolver::ResolveBindingsInText(
          displayText, std::string("Press ") + fallbackKeyDisplay + " to");
    }
  }

  if (IsHudRuntimeTailJoinPromptKey(keyNorm)) {
    struct TailStickyEntry {
      std::string text;
      DWORD tick = 0;
    };
    static std::unordered_map<std::string, TailStickyEntry>
        s_directObjHudTailSticky;
    static DWORD s_directObjHudTailPruneTick = 0;
    const DWORD now = GetTickCount();
    const DWORD kTailStickyMs = 150;

    std::string runtimeTail;
    const std::string tailStickyKey =
        keyNorm + "|" + std::to_string(callerOffset);
    bool haveTail = TextHook_GetHudRuntimeTail(keyNorm, hintX, hintY,
                                               callerOffset, runtimeTail);
    if (!haveTail) {
      auto itSticky = s_directObjHudTailSticky.find(tailStickyKey);
      if (itSticky != s_directObjHudTailSticky.end() &&
          (now - itSticky->second.tick) <= kTailStickyMs) {
        runtimeTail = itSticky->second.text;
        haveTail = !runtimeTail.empty();
      }
    }
    if (haveTail) {
      std::string tailClean = TrimSpaces(StripColorCodes(runtimeTail));
      if (!tailClean.empty()) {
        s_directObjHudTailSticky[tailStickyKey] = {tailClean, now};
        const std::string basePlain = TrimSpaces(StripColorCodes(displayText));
        if (basePlain.find(tailClean) == std::string::npos) {
          const bool compactJoin =
              !basePlain.empty() && basePlain.back() == ':';
          if (!compactJoin) {
            displayText += " ";
          }
          displayText += tailClean;
        }
      }
    }
    if ((now - s_directObjHudTailPruneTick) > 2000) {
      s_directObjHudTailPruneTick = now;
      for (auto it = s_directObjHudTailSticky.begin();
           it != s_directObjHudTailSticky.end();) {
        if ((now - it->second.tick) > 30000) {
          it = s_directObjHudTailSticky.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  outText = TrimSpaces(displayText);
  return !TrimSpaces(StripColorCodes(outText)).empty();
}
static bool ClassifyNativeHudDisplayChannelDirect(const HudNativeEntry &entry,
                                                  HudRouteChannel &outChannel) {
  return ResolveHudFamilyRouteFromNative(entry, outChannel);
}

static bool IsObjHudHintFamilySourceToken(uint8_t sourceToken) {
  return sourceToken == NATIVE_HUD_SOURCE_HUD560 ||
         sourceToken == NATIVE_HUD_SOURCE_ADDCMD;
}

static bool ResolveHudFamilyRouteFromNative(const HudNativeEntry &entry,
                                            HudRouteChannel &outChannel) {
  outChannel = HUD_ROUTE_UNRESOLVED;
  if (entry.key.empty()) {
    return false;
  }

  const HudKeyType kt = ClassifyHudKey(entry.key);
  if (kt == HUD_KEY_INTRO || kt == HUD_KEY_MENU || kt == HUD_KEY_SUBTITLE ||
      kt == HUD_KEY_EXCLUDE) {
    return false;
  }
  const bool recentObjectiveObservation =
      HasRecentObjectiveObservationForKey(entry.key, 6000);
  const bool hasObjectiveChannel =
      (entry.objectiveChannel >= OBJ_CHANNEL_GAMEPLAY_LIST &&
       entry.objectiveChannel <= OBJ_CHANNEL_SAVE_PROGRESS);
  const bool objectiveFromKey =
      IsObjectiveHudKeyType(kt) && entry.source != HUD_NATIVE_RENDER;
  if (entry.objectiveLike || hasObjectiveChannel || recentObjectiveObservation ||
      objectiveFromKey) {
    outChannel = HUD_ROUTE_OBJECTIVE;
    return true;
  }

  if (kt == HUD_KEY_OBJ_HUD_HINT && entry.source == HUD_NATIVE_RENDER) {
    CgDrawHudElemCapture cap{};
    if (TextHook_GetCgDrawCapture(entry.key, cap, 2200) &&
        std::isfinite(cap.x) && std::isfinite(cap.y) &&
        std::isfinite(cap.fontScale) && std::fabs(cap.x - 6.0f) <= 8.0f &&
        cap.fontScale >= 0.85f && cap.fontScale <= 1.40f) {
      static const float kObjectiveLaneYs[] = {10.0f, 30.0f, 50.0f, 70.0f};
      for (float laneY : kObjectiveLaneYs) {
        if (std::fabs(cap.y - laneY) <= 12.0f) {
          outChannel = HUD_ROUTE_IGNORE;
          return true;
        }
      }
    }
  }

  if (entry.source == HUD_NATIVE_SCANNER) {
    return false;
  }

  // SSOT 6F: CLOCKWORK_HINT_DRILL* keys are always HUDHINT family,
  // regardless of source token.  The clockwork vault drill scene emits
  // these from mixed sources with zero-alpha; force HINT routing so the
  // HUDHINT authority bootstrap can synthesize-render.
  // Only CLOCKWORK_HINT_DRILL itself — CLOCKWORK_HINT_DRILL_PICKUP is a
  // small interaction prompt that belongs in OBJHUDHINT.
  if (entry.key == "CLOCKWORK_HINT_DRILL") {
    outChannel = HUD_ROUTE_HINT;
    return true;
  }

  // SATFARM_INITIALIZING arrives via HUD560/ADDCMD (IsObjHudHintFamily=true)
  // but must render in the HUDHINT position (top-center), not OBJHUDHINT.
  if (entry.key == "SATFARM_INITIALIZING") {
    outChannel = HUD_ROUTE_HINT;
    return true;
  }

  const uint8_t sourceToken = HintSourceTokenFromHudNativeEntry(entry);
  if (IsObjHudHintFamilySourceToken(sourceToken)) {
    outChannel = HUD_ROUTE_OBJHUDHINT;
    return true;
  }

  outChannel = HUD_ROUTE_HINT;
  return true;
}
static bool ApplyNativeHudEntryDirect(const std::string &key,
                                      const HudNativeEntry &entry,
                                      const std::string *englishRuntime) {
  HudRouteChannel channel = HUD_ROUTE_UNRESOLVED;
  if (!ClassifyNativeHudDisplayChannelDirect(entry, channel)) {
    return false;
  }
  {
    const DWORD now = GetTickCount();
    const int slot = TextHook_HudSlotFromEvent(entry.y, entry.virtualH, 0);
    static std::mutex s_routeDiagMtx;
    static std::unordered_map<uint64_t, DWORD> s_routeDiagTick;
    uint64_t sig = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
      sig ^= v;
      sig *= 1099511628211ull;
    };
    for (unsigned char c : key) {
      mix((uint64_t)c);
    }
    mix((uint64_t)(unsigned int)entry.callerOffset);
    mix((uint64_t)(unsigned int)(channel & 0xFF));
    mix((uint64_t)(unsigned int)(slot & 0xFF));
    bool shouldLog = false;
    {
      std::lock_guard<std::mutex> lk(s_routeDiagMtx);
      auto it = s_routeDiagTick.find(sig);
      if (it == s_routeDiagTick.end() || (now - it->second) > 1200) {
        s_routeDiagTick[sig] = now;
        shouldLog = true;
      }
      if (s_routeDiagTick.size() > 640) {
        for (auto it = s_routeDiagTick.begin(); it != s_routeDiagTick.end();) {
          if ((now - it->second) > 30000) {
            it = s_routeDiagTick.erase(it);
          } else {
            ++it;
          }
        }
      }
    }
    if (shouldLog) {
      const float alpha = (std::isfinite(entry.color[3])) ? entry.color[3] : 0.0f;
      const uint8_t sourceToken = HintSourceTokenFromHudNativeEntry(entry);
      char rbuf[512];
      sprintf_s(
          rbuf,
          "[HUD-NATIVE-ROUTE] key=%s route=%s caller=+0x%X src=%d tok=%u slot=%d xy=(%.1f,%.1f) scale=(%.3f,%.3f) h=%.1f a=%.2f v=%ux%u",
          key.c_str(), HudRouteChannelName(channel),
          (unsigned int)entry.callerOffset, (int)entry.source,
          (unsigned int)sourceToken, slot, entry.x, entry.y, entry.xScale,
          entry.yScale, entry.fontHeight, alpha, (unsigned int)entry.virtualW,
          (unsigned int)entry.virtualH);
      LogToFile(rbuf);
    }
  }

  if (channel == HUD_ROUTE_HINT) {
    RemoveObjHudHintEntryByKey(key);
    RefreshHintEntryNative(key, entry, englishRuntime);
    return true;
  }
  if (channel == HUD_ROUTE_OBJHUDHINT) {
    RemoveHintEntryByKey(key);
    RefreshObjHudHintEntryNative(key, entry, englishRuntime);
    return true;
  }
  if (channel == HUD_ROUTE_OBJECTIVE) {
    RemoveHintEntryByKey(key);
    RemoveObjHudHintEntryByKey(key);
    RefreshObjectiveOverlayEntry(key, (ObjectiveChannel)entry.objectiveChannel,
                                 englishRuntime);
    return true;
  }
  if (channel == HUD_ROUTE_IGNORE) {
    RemoveHintEntryByKey(key);
    RemoveObjHudHintEntryByKey(key);
    return true;
  }
  return false;
}
static void PruneHudRuntimeTailLocked(DWORD now) {
  while (!g_HudRuntimeTailFragments.empty()) {
    const DWORD age = now - g_HudRuntimeTailFragments.front().tick;
    if (age <= HUD_RUNTIME_TAIL_TTL_MS) {
      break;
    }
    g_HudRuntimeTailFragments.pop_front();
  }
  while (g_HudRuntimeTailFragments.size() > HUD_RUNTIME_TAIL_MAX) {
    g_HudRuntimeTailFragments.pop_front();
  }
}
static void CaptureHudRuntimeTailFragment(const std::string &key,
                                          const std::string &rawText, float x,
                                          float y, unsigned int callerOffset,
                                          bool trustedCaller,
                                          bool gameplayContext) {
  if (!trustedCaller || !gameplayContext) {
    return;
  }
  if (!std::isfinite(x) || !std::isfinite(y)) {
    return;
  }
  if (x < -200.0f || x > 5000.0f || y < -200.0f || y > 3000.0f) {
    return;
  }
  // Fast atomic gate: skip all string work when no tail-join prompt is active.
  // When key is non-empty the caller already resolved it, but anonymous tail
  // captures (key=="") are the high-frequency font-hook path and can be
  // skipped entirely when no PLATFORM_SWAP*/PICKUP* prompt was noted recently.
  if (key.empty()) {
    const DWORD lastPrompt =
        g_lastTailJoinPromptTick.load(std::memory_order_relaxed);
    const DWORD now0 = GetTickCount();
    if (lastPrompt == 0 || (now0 - lastPrompt) > 350) {
      return;
    }
  }

  // Cheap pre-filter on raw text BEFORE any string allocation.
  // StripColorCodes removes ^X pairs (2 chars each), so raw text can be at
  // most ~2x the cleaned length.  IsLikelyHudRuntimeTailText rejects
  // cleaned.size() < 2 || > 40, so raw < 2 || > 80 is a safe fast reject.
  // Also reject obvious non-tail patterns without allocating strings.
  {
    const size_t rawLen = rawText.size();
    if (rawLen < 2 || rawLen > 80) {
      return;
    }
    // Quick scan: reject prompt-like and localization-key-like texts.
    // These are the most common non-tail texts on the hot path.
    const char *r = rawText.c_str();
    if (r[0] == '@') return;
    // "Press " / "Hold " prefix → prompt, not a weapon name tail
    if (rawLen >= 6 && (r[0] == 'P' || r[0] == 'p') &&
        (r[1] == 'r' || r[1] == 'R') && (r[2] == 'e' || r[2] == 'E') &&
        (r[3] == 's' || r[3] == 'S') && (r[4] == 's' || r[4] == 'S') &&
        r[5] == ' ') {
      return;
    }
    if (rawLen >= 5 && (r[0] == 'H' || r[0] == 'h') &&
        (r[1] == 'o' || r[1] == 'O') && (r[2] == 'l' || r[2] == 'L') &&
        (r[3] == 'd' || r[3] == 'D') && r[4] == ' ') {
      return;
    }
    // Binding placeholders → not a weapon name
    if (rawText.find("[{") != std::string::npos ||
        rawText.find("&&") != std::string::npos ||
        rawText.find("$$") != std::string::npos) {
      return;
    }
  }

  std::string cleaned = TrimSpaces(StripColorCodes(rawText));
  if (!IsLikelyHudRuntimeTailText(cleaned)) {
    return;
  }
  std::string fragmentKey = key;
  if (fragmentKey.empty()) {
    GetRecentHudRuntimeTailJoinPromptKey(350, fragmentKey);
  }

  const DWORD now = GetTickCount();
  {
    std::lock_guard<std::mutex> lock(g_HudRuntimeTailMutex);
    PruneHudRuntimeTailLocked(now);

    HudRuntimeTailFragment frag{};
    frag.key = fragmentKey;
    frag.text = cleaned;
    frag.x = x;
    frag.y = y;
    frag.callerOffset = callerOffset;
    frag.tick = now;
    g_HudRuntimeTailFragments.push_back(std::move(frag));
    PruneHudRuntimeTailLocked(now);
  }

  // Throttled logging — use callerOffset as cheap key to avoid per-frame
  // string allocation; only build full sig when we actually decide to log.
  static std::mutex s_tailCapLogMtx;
  static std::unordered_map<unsigned int, DWORD> s_tailCapLogTick;
  {
    std::lock_guard<std::mutex> lk(s_tailCapLogMtx);
    auto it = s_tailCapLogTick.find(callerOffset);
    if (it == s_tailCapLogTick.end() || (now - it->second) > 2500) {
      s_tailCapLogTick[callerOffset] = now;
      char buf[512];
      sprintf_s(buf, "[HUD-TAIL-CAP] caller=+0x%X x=%.1f y=%.1f text=\"%.80s\"",
                callerOffset, x, y, cleaned.c_str());
      LogToFile(buf);
    }
    if (s_tailCapLogTick.size() > 64) {
      for (auto jt = s_tailCapLogTick.begin(); jt != s_tailCapLogTick.end();) {
        if ((now - jt->second) > 30000) {
          jt = s_tailCapLogTick.erase(jt);
        } else {
          ++jt;
        }
      }
    }
  }
}

// Gameplay-only HUD key filter (mirrors FSHook::IsGameplayHudKey)
static bool IsGameplayHudKeyName(const std::string &name,
                                 std::string *outAutoReason) {
  if (outAutoReason) {
    outAutoReason->clear();
  }
  if (name.empty())
    return false;
  if (HasRecentObjectiveObservationForKey(name, 6000)) {
    return false;
  }

  // Mission intro keys are handled by the intro overlay pipeline only.
  if (name.find("INTROSCREEN") != std::string::npos)
    return false;

  if (IsExcludedHudAutoPrefix(name))
    return false;

  // PLATFORM_* keys are shared with menus, but we gate actual usage by callsites
  // (SLC lifecycle) and menu context. Include them here so native draw capture
  // can position gameplay prompts correctly.
  if (name.rfind("PLATFORM_", 0) == 0)
    return true;
  if (name == "WEAPON_NO_WEAPON_AMMO")
    return true;

  if (name.rfind("GAME_OBJECTIVE", 0) == 0)
    return true;

  if (name == "CGAME_MISSIONOBJECTIVES")
    return true;
  if (name.rfind("CGAME_OBJECTIVE", 0) == 0)
    return true;
  if (name == "CGAME_NOW_SAVING")
    return true;

  if (name.find("_OBJ_") != std::string::npos)
    return true;
  if (name.find("_OBJECTIVE_") != std::string::npos &&
      name.rfind("MENU_", 0) != 0)
    return true;
  if (HasObjectiveSuffixOnlyKey(name))
    return true;

  if (name.rfind("MENU_", 0) != 0 && name.rfind("PLATFORM_", 0) != 0 &&
      name.find("_HINT") != std::string::npos)
    return true;
  if (name.rfind("MENU_", 0) != 0 &&
      (name.find("_PROMPT") != std::string::npos ||
       name.rfind("PROMPT_", 0) == 0))
    return true;
  if (name.rfind("SCRIPT_HINT_", 0) == 0)
    return true;
  if (name.rfind("HINT_", 0) == 0)
    return true;
  if (name == "EXE_GAMESAVED")
    return true;

  // CORNERED_ mission-specific prompts (gameplay only)
  if (name.rfind("CORNERED_", 0) == 0)
    return true;

  // Auto-accept mission-specific hint keys when their localized English is
  // clearly prompt-like or contains runtime binding placeholders.
  if (TextHook_IsPauseMenuLikely())
    return false;

  std::string localizedKor;
  std::string localizedEng;
  if (!TextHook_GetLocalizedKeyText(name, localizedKor, localizedEng) ||
      localizedEng.empty())
    return false;

  const std::string english = TrimSpaces(StripColorCodes(localizedEng));
  const bool promptLike = LooksLikePressHoldPrompt(english);
  const bool bindingLike = HasBindingPlaceholderToken(localizedEng);
  const bool bindingLikeKor = HasBindingPlaceholderToken(localizedKor);
  if (promptLike || bindingLike || bindingLikeKor) {
    if (outAutoReason) {
      if (promptLike && (bindingLike || bindingLikeKor))
        *outAutoReason = "english_prompt+binding";
      else if (promptLike)
        *outAutoReason = "english_prompt";
      else if (bindingLikeKor)
        *outAutoReason = "korean_binding";
      else
        *outAutoReason = "english_binding";
    }
    return true;
  }

  return false;
}

// Determine scale from key
static float DetermineHudScale(const std::string &key) {
  if (key.find("OBJECTIVES_") != std::string::npos ||
      key.find("OBJECTIVE_") != std::string::npos) return 1.22f;
  if (key.rfind("CORNERED_OBJ_", 0) == 0 ||
      key.rfind("CORNERED_OBJECTIVE_", 0) == 0) return 1.22f;
  if (key.rfind("CORNERED_BINOCULARS_", 0) == 0 &&
      key.find("_HINT") != std::string::npos) {
    // Scanner action hints should be larger than interaction cursor hints.
    return 1.22f;
  }
  if (key == "CORNERED_READY" || key == "CORNERED_NO" ||
      key == "CORNERED_MATCH" || key == "CORNERED_DATA" ||
      key.find("SCANNING") != std::string::npos ||
      key.find("IDENTIFIED") != std::string::npos ||
      key.find("DEGREE_SYMBOL") != std::string::npos) {
    return 0.82f;
  }
  if (key.find("HINT_") != std::string::npos ||
      key.find("_HINT") != std::string::npos ||
      key.find("PLATFORM_") != std::string::npos) return 1.08f;
  if (key.find("CGAME_") != std::string::npos ||
      key.find("GAME_") != std::string::npos ||
      key.find("EXE_") == 0) return 1.45f; // chyron size
  return 1.16f;
}

// Determine HUD text color from key
static void DetermineHudColor(const std::string &key, float outColor[4]) {
  // Default: chyron pale cyan
  outColor[0] = 0.85f;
  outColor[1] = 0.93f;
  outColor[2] = 0.92f;
  outColor[3] = 1.0f;

  // Objectives / mission list: white
  if (key.find("OBJECTIVES_") != std::string::npos ||
      key.find("OBJECTIVE_") != std::string::npos ||
      key.find("MISSIONOBJECTIVE") != std::string::npos ||
      key.find("MISSION_OBJECTIVE") != std::string::npos ||
      HasObjectiveSuffixOnlyKey(key) ||
      key.rfind("CORNERED_OBJ_", 0) == 0 ||
      key.rfind("CORNERED_OBJECTIVE_", 0) == 0 ||
      key.find("CGAME_MISSIONOBJECTIVES") != std::string::npos ||
      key.find("CGAME_OBJECTIVE") != std::string::npos) {
    outColor[0] = 1.0f;
    outColor[1] = 1.0f;
    outColor[2] = 1.0f;
    return;
  }

  // Hints / prompts: slightly brighter
  if (key.find("HINT_") != std::string::npos ||
      key.find("_HINT") != std::string::npos ||
      key.find("PLATFORM_") != std::string::npos) {
    outColor[0] = 0.95f;
    outColor[1] = 0.98f;
    outColor[2] = 0.98f;
    return;
  }
}
int TextHook_HudSlotFromEvent(float y, unsigned short virtualH,
                              unsigned int packedFlags) {
  const int packedSlot =
      (int)(short)((unsigned int)((packedFlags >> 16) & 0xFFFFu));
  if (packedSlot != 0) {
    return packedSlot;
  }
  if (virtualH > 0 && std::isfinite(y)) {
    const float ny = y / (float)virtualH;
    if (ny < 0.18f) return 1;
    if (ny < 0.42f) return 2;
    if (ny < 0.62f) return 3;
    if (ny < 0.78f) return 4;
    return 5;
  }
  if (std::isfinite(y)) {
    if (y < 160.0f) return 1;
    if (y < 260.0f) return 2;
    if (y < 360.0f) return 3;
    if (y < 520.0f) return 4;
    return 5;
  }
  return 0;
}

uint64_t TextHook_MakeHudConsumerId(OverlaySource source,
                                    unsigned int callerOffset, int slotBucket,
                                    unsigned short virtualW,
                                    unsigned short virtualH) {
  const uint16_t virtualTag = (virtualW != 0 || virtualH != 0) ? 1 : 0;
  uint64_t h = 1469598103934665603ull; // FNV-1a 64
  auto mix = [&](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  mix((uint64_t)((unsigned int)source & 0xFFu));
  mix((uint64_t)(callerOffset & 0xFFFFFFu));
  mix((uint64_t)((unsigned int)(slotBucket & 0xFF)));
  mix((uint64_t)virtualTag);
  return h;
}

static OverlaySource OverlaySourceFromHintNativeSource(unsigned char nativeSource) {
  if (nativeSource == (unsigned char)HUD_NATIVE_RENDER) {
    return OVERLAY_SOURCE_HUD560;
  }
  if (nativeSource == (unsigned char)HUD_NATIVE_ADDCMD) {
    return OVERLAY_SOURCE_ADDCMD;
  }
  return OVERLAY_SOURCE_SLC;
}

static OverlaySource OverlaySourceFromNativeSourceToken(uint8_t sourceToken) {
  if (sourceToken == NATIVE_HUD_SOURCE_HUD560) {
    return OVERLAY_SOURCE_HUD560;
  }
  if (sourceToken == NATIVE_HUD_SOURCE_ADDCMD) {
    return OVERLAY_SOURCE_ADDCMD;
  }
  return OVERLAY_SOURCE_SLC;
}

static uint8_t HintSourceTokenFromRouteSource(unsigned char sourceTag) {
  if (sourceTag == (unsigned char)HUD_NATIVE_ADDCMD) {
    return NATIVE_HUD_SOURCE_ADDCMD;
  }
  if (sourceTag == (unsigned char)HUD_NATIVE_RENDER ||
      sourceTag == (unsigned char)HUD_NATIVE_SCANNER) {
    return NATIVE_HUD_SOURCE_BRIDGE;
  }
  return 0;
}

static uint8_t HintSourceTokenFromHudNativeEntry(const HudNativeEntry &entry) {
  if (entry.sourceToken != 0) {
    return entry.sourceToken;
  }
  if (entry.source == HUD_NATIVE_ADDCMD) {
    return NATIVE_HUD_SOURCE_ADDCMD;
  }
  if (entry.source == HUD_NATIVE_RENDER) {
    return NATIVE_HUD_SOURCE_HUD560;
  }
  return NATIVE_HUD_SOURCE_BRIDGE;
}

static int GetHintEntrySlotBucket(const HintOverlayEntry &entry) {
  if (entry.slotBucket > 0) {
    return entry.slotBucket;
  }
  return TextHook_HudSlotFromEvent(entry.nativeY, entry.nativeVirtualH, 0);
}

static uint64_t GetHintEntryConsumerId(const HintOverlayEntry &entry) {
  if (entry.consumerId != 0) {
    return entry.consumerId;
  }
  const OverlaySource overlaySource =
      (entry.nativeSourceToken != 0)
          ? OverlaySourceFromNativeSourceToken(entry.nativeSourceToken)
          : OverlaySourceFromHintNativeSource(entry.nativeSource);
  return TextHook_MakeHudConsumerId(
      overlaySource, entry.callerOffset, GetHintEntrySlotBucket(entry),
      entry.nativeVirtualW, entry.nativeVirtualH);
}

static uint64_t MakeHintConsumerIdFromNative(const HudNativeEntry &entry,
                                             int slotBucket) {
  const OverlaySource overlaySource =
      (entry.sourceToken != 0)
          ? OverlaySourceFromNativeSourceToken(entry.sourceToken)
          : OverlaySourceFromHintNativeSource((unsigned char)entry.source);
  return TextHook_MakeHudConsumerId(
      overlaySource, entry.callerOffset, slotBucket, entry.virtualW,
      entry.virtualH);
}

static uint64_t MakeHudNativeEntryMapKey(const std::string &key,
                                         uint64_t consumerId) {
  std::string keyNorm = ToUpperAscii(TrimSpaces(key));
  uint64_t h = consumerId ^ 1469598103934665603ull;
  for (unsigned char c : keyNorm) {
    h ^= (uint64_t)c;
    h *= 1099511628211ull;
  }
  return h;
}

static const char *HudRouteChannelName(HudRouteChannel ch) {
  switch (ch) {
  case HUD_ROUTE_HINT:
    return "hint";
  case HUD_ROUTE_OBJHUDHINT:
    return "objhudhint";
  case HUD_ROUTE_OBJECTIVE:
    return "objective";
  case HUD_ROUTE_IGNORE:
    return "ignore";
  case HUD_ROUTE_UNRESOLVED:
    return "unresolved";
  default:
    return "unspecified";
  }
}

static OverlaySource HudOverlaySourceFromNative(HudNativeSource src) {
  if (src == HUD_NATIVE_RENDER) {
    return OVERLAY_SOURCE_HUD560;
  }
  if (src == HUD_NATIVE_ADDCMD) {
    return OVERLAY_SOURCE_ADDCMD;
  }
  return OVERLAY_SOURCE_SLC;
}

static bool IsObjectiveHudKeyType(HudKeyType t) {
  return t == HUD_KEY_OBJECTIVE_LIST || t == HUD_KEY_OBJECTIVE_UPDATE ||
         t == HUD_KEY_OBJECTIVE_HEADER || t == HUD_KEY_GAME_SAVED ||
         t == HUD_KEY_NOW_SAVING;
}

static bool HasRecentObjectiveObservationForKey(const std::string &key,
                                                DWORD maxAgeMs) {
  if (key.empty()) {
    return false;
  }

  const DWORD now = GetTickCount();
  const DWORD windowMs =
      (maxAgeMs < 250) ? 250 : ((maxAgeMs > 20000) ? 20000 : maxAgeMs);

  static std::mutex s_recentObjectiveKeyCacheMtx;
  static DWORD s_recentObjectiveKeyCacheTick = 0;
  static DWORD s_recentObjectiveKeyCacheWindowMs = 0;
  static std::unordered_set<std::string> s_recentObjectiveKeys;

  std::lock_guard<std::mutex> lock(s_recentObjectiveKeyCacheMtx);
  if (s_recentObjectiveKeyCacheTick == 0 || now < s_recentObjectiveKeyCacheTick ||
      (now - s_recentObjectiveKeyCacheTick) > 120 ||
      s_recentObjectiveKeyCacheWindowMs != windowMs) {
    s_recentObjectiveKeys.clear();
    const auto recentObjectiveKeys = TextHook_GetRecentObjectiveKeys(windowMs);
    s_recentObjectiveKeys.reserve(recentObjectiveKeys.size() * 2 + 8);
    for (const auto &obs : recentObjectiveKeys) {
      if (!obs.key.empty()) {
        s_recentObjectiveKeys.insert(obs.key);
      }
    }
    s_recentObjectiveKeyCacheTick = now;
    s_recentObjectiveKeyCacheWindowMs = windowMs;
  }

  return s_recentObjectiveKeys.find(key) != s_recentObjectiveKeys.end();
}

// Scanner HUD hints can be emitted by mixed SLC callsites that also produce
// objective text. Never let objective authority override this key family.
static bool IsScannerHintAuthorityKey(const std::string &key) {
  if (key.empty()) {
    return false;
  }
  const std::string up = ToUpper(key);
  if (up.rfind("CORNERED_BINOCULARS_", 0) == 0) {
    return true;
  }
  if (up.rfind("CORNERED_SCANNING", 0) == 0) {
    return true;
  }
  if (up == "CORNERED_READY" || up == "CORNERED_NO" ||
      up == "CORNERED_MATCH" || up == "CORNERED_DATA" ||
      up == "CORNERED_IDENTIFIED") {
    return true;
  }
  if (up.rfind("CORNERED_", 0) == 0 &&
      up.find("DEGREE_SYMBOL") != std::string::npos) {
    return true;
  }
  return false;
}

// Movement tutorial prompts (e.g. "Press C to crouch") are often emitted from
// mixed SLC callers that also produce objective-like strings. If these keys are
// forced through strict consumer quarantine they can flap between unresolved and
// dropped despite stable gameplay display.
static bool IsMovementTutorialHintKey(const std::string &key) {
  if (key.empty()) {
    return false;
  }
  const std::string up = ToUpper(key);
  if (up.rfind("DEER_HUNT_", 0) == 0 &&
      (up.find("CROUCH") != std::string::npos ||
       up.find("SLIDE") != std::string::npos ||
       up.find("SPRINT") != std::string::npos ||
       up.find("PRONE") != std::string::npos ||
       up.find("JUMP") != std::string::npos)) {
    return true;
  }
  if (up.find("TUTORIAL_") != std::string::npos &&
      (up.find("CROUCH") != std::string::npos ||
       up.find("SLIDE") != std::string::npos ||
       up.find("SPRINT") != std::string::npos)) {
    return true;
  }
  return false;
}

// Generic PLATFORM hold/use placeholders are over-shared in cfg-reverse
// snapshots and routinely surface without a real live interaction prompt.
static bool IsAmbiguousGenericPlatformHoldKey(const std::string &key) {
  if (key.empty()) {
    return false;
  }
  const std::string up = ToUpper(key);
  return up == "PLATFORM_HOLD_TO_USE" || up == "PLATFORM_HOLD_TO_DROP";
}

static bool IsStrictHintCaptureCandidateKey(const std::string &key) {
  if (key.empty()) {
    return false;
  }
  if (IsAmbiguousGenericPlatformHoldKey(key)) {
    return false;
  }

  const HudKeyType keyType = ClassifyHudKey(key);
  // Fallback-classified keys (HUD_KEY_OBJ_HUD_HINT) bypass the objective
  // observation gate — they are gameplay text the objective system cannot
  // display (lane < 0), so blocking them here silently drops them.
  const bool isFallbackType = (keyType == HUD_KEY_OBJ_HUD_HINT);
  if (!isFallbackType && HasRecentObjectiveObservationForKey(key, 6000)) {
    return false;
  }

  if (keyType == HUD_KEY_HINT) {
    return true;
  }
  if (ShouldForceCenterAlignGameplayTranslation(key) ||
      IsMovementTutorialHintKey(key)) {
    return true;
  }

  EnsureTranslationsLoaded();
  std::string localizedKor;
  std::string localizedEng;
  if (!TextHook_GetLocalizedKeyText(key, localizedKor, localizedEng)) {
    return false;
  }

  localizedEng = TrimSpaces(StripColorCodes(localizedEng));
  if (LooksLikePressHoldPrompt(localizedEng)) {
    return true;
  }

  if (HasBindingPlaceholderToken(localizedKor) ||
      HasBindingPlaceholderToken(localizedEng)) {
    return true;
  }

  // Catch-all: fallback-classified keys (HUD_KEY_OBJ_HUD_HINT) with Korean
  // translation are gameplay text that matched no specific hint pattern.
  // Route to HUDHINT so they are not silently dropped by the objective system.
  if (isFallbackType) {
    return true;
  }

  return false;
}

struct RecentHudHintCfgKeyState {
  std::string key;
  unsigned long tick = 0;
  unsigned int callerOffset = 0;
  uint32_t slcIndex = 0;
};

static std::mutex g_HudHintRecentCfgKeyMutex;
static std::unordered_map<uint32_t, RecentHudHintCfgKeyState>
    g_HudHintRecentCfgKeyMap;
static constexpr DWORD HUD_HINT_RECENT_CFG_KEY_TTL = 6000;

static void TextHook_RecordRecentHudHintCfgKey(uint32_t cfg,
                                               const std::string &key,
                                               unsigned int callerOffset,
                                               uint32_t slcIndex,
                                               const char *sourceTag) {
  if (!ObjRev_IsPlausibleConfigIndex(cfg) || key.empty()) {
    return;
  }
  if (IsAmbiguousGenericPlatformHoldKey(key) ||
      !IsStrictHintCaptureCandidateKey(key)) {
    return;
  }

  EnsureTranslationsLoaded();
  const auto itKor = g_KeyToKorean.find(key);
  if (itKor == g_KeyToKorean.end() || itKor->second.empty()) {
    return;
  }

  const DWORD now = GetTickCount();
  bool shouldLog = false;
  std::string prevKey;
  {
    std::lock_guard<std::mutex> lock(g_HudHintRecentCfgKeyMutex);
    RecentHudHintCfgKeyState &entry = g_HudHintRecentCfgKeyMap[cfg];
    prevKey = entry.key;
    shouldLog = entry.key.empty() || entry.key != key;
    entry.key = key;
    entry.tick = now;
    entry.callerOffset = callerOffset;
    entry.slcIndex = slcIndex;

    if (g_HudHintRecentCfgKeyMap.size() > 256) {
      for (auto it = g_HudHintRecentCfgKeyMap.begin();
           it != g_HudHintRecentCfgKeyMap.end();) {
        const DWORD age = (now >= it->second.tick) ? (now - it->second.tick) : 0;
        if (age > HUD_HINT_RECENT_CFG_KEY_TTL * 2) {
          it = g_HudHintRecentCfgKeyMap.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  if (shouldLog) {
    char buf[384];
    sprintf_s(
        buf,
        "[HUDHINT-CFG-CACHE] cfg=0x%X key=%s prev=%s caller=+0x%X slc=0x%X src=%s",
        (unsigned int)cfg, key.c_str(), prevKey.empty() ? "-" : prevKey.c_str(),
        callerOffset, (unsigned int)slcIndex, sourceTag ? sourceTag : "-");
    LogToFile(buf);
  }
}

static bool TextHook_TryResolveRecentHudHintCfgKey(
    uint32_t cfg, std::string &outKey, unsigned int *outCallerOffset,
    uint32_t *outSlcIndex, const char *logTag) {
  outKey.clear();
  if (!ObjRev_IsPlausibleConfigIndex(cfg)) {
    return false;
  }

  RecentHudHintCfgKeyState snapshot{};
  {
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lock(g_HudHintRecentCfgKeyMutex);
    const auto it = g_HudHintRecentCfgKeyMap.find(cfg);
    if (it == g_HudHintRecentCfgKeyMap.end()) {
      return false;
    }
    const DWORD age = (now >= it->second.tick) ? (now - it->second.tick) : 0;
    if (age > HUD_HINT_RECENT_CFG_KEY_TTL) {
      return false;
    }
    snapshot = it->second;
  }

  if (snapshot.key.empty() || IsAmbiguousGenericPlatformHoldKey(snapshot.key) ||
      !IsStrictHintCaptureCandidateKey(snapshot.key)) {
    return false;
  }

  EnsureTranslationsLoaded();
  const auto itKor = g_KeyToKorean.find(snapshot.key);
  if (itKor == g_KeyToKorean.end() || itKor->second.empty()) {
    return false;
  }

  outKey = snapshot.key;
  if (outCallerOffset) {
    *outCallerOffset = snapshot.callerOffset;
  }
  if (outSlcIndex) {
    *outSlcIndex = snapshot.slcIndex;
  }
  if (logTag) {
    char buf[320];
    sprintf_s(buf,
              "[HUDHINT-CFG-RESOLVE] tag=%s cfg=0x%X key=%s caller=+0x%X slc=0x%X src=hudhint_cache",
              logTag, (unsigned int)cfg, snapshot.key.c_str(),
              snapshot.callerOffset, (unsigned int)snapshot.slcIndex);
    LogToFile(buf);
  }
  return true;
}

static void TextHook_RecordRecentHudHintCfgKeyBySlc(uint32_t slcIndex,
                                                    const std::string &key,
                                                    unsigned int callerOffset,
                                                    const char *sourceTag) {
  if (slcIndex == 0 || key.empty()) {
    return;
  }
  if (IsAmbiguousGenericPlatformHoldKey(key) ||
      !IsStrictHintCaptureCandidateKey(key)) {
    return;
  }

  const DWORD now = GetTickCount();
  uint32_t bestCfg = 0;
  int bestScore = -1000000000;
  {
    std::lock_guard<std::mutex> lock(g_cfgDirectMapMutex);
    for (const auto &kv : g_cfgDirectMap) {
      const uint32_t cfg = kv.first;
      const CfgDirectMapEntry &entry = kv.second;
      if (!ObjRev_IsPlausibleConfigIndex(cfg) || entry.slcIdx != slcIndex) {
        continue;
      }
      const DWORD age = (now >= entry.tick) ? (now - entry.tick) : 0;
      if (age > 9000) {
        continue;
      }
      if (entry.keyClass != 0) {
        continue;
      }
      if (entry.objectiveHits > 0 && entry.objectiveTick != 0 &&
          now >= entry.objectiveTick && (now - entry.objectiveTick) < 15000) {
        continue;
      }

      int score = 0;
      score += (int)entry.quality * 12;
      score += (int)std::min<uint32_t>(entry.hits, 24u);
      score -= (int)(age / 90);
      if (entry.callerOffset == callerOffset) {
        score += 24;
      }
      if (cfg <= 0x03FFu) {
        score += 6;
      }
      if ((entry.bias & 0xFFu) == 0xACu) {
        score += 4;
      }

      if (score > bestScore) {
        bestScore = score;
        bestCfg = cfg;
      }
    }
  }

  if (bestCfg != 0) {
    TextHook_RecordRecentHudHintCfgKey(bestCfg, key, callerOffset, slcIndex,
                                       sourceTag);
  }
}

static void RemoveHintEntryByKey(const std::string &key) {
  if (key.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_HintMutex);
  g_HintEntries.erase(
      std::remove_if(g_HintEntries.begin(), g_HintEntries.end(),
                     [&](const HintOverlayEntry &e) { return e.key == key; }),
      g_HintEntries.end());
}

static void RemoveObjHudHintEntryByKey(const std::string &key) {
  if (key.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_ObjHudHintMutex);
  g_ObjHudHintEntries.erase(
      std::remove_if(g_ObjHudHintEntries.begin(), g_ObjHudHintEntries.end(),
                     [&](const ObjHudHintEntry &e) { return e.key == key; }),
      g_ObjHudHintEntries.end());
}

static void RemoveHudNativeEntriesByKey(const std::string &key) {
  if (key.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_HudNativeMutex);
  for (auto it = g_HudNativeMap.begin(); it != g_HudNativeMap.end();) {
    if (it->second.key == key) {
      it = g_HudNativeMap.erase(it);
    } else {
      ++it;
    }
  }
}

struct RouteOnlyPulseEvidence {
  DWORD firstTick = 0;
  DWORD lastTick = 0;
  unsigned short pulses = 0;
};

static bool IsRouteOnlyPulseStable(const std::string &key,
                                   unsigned int callerOffset,
                                   HudRouteChannel route, DWORD now,
                                   DWORD minStableMs,
                                   unsigned short minPulses) {
  if (key.empty()) {
    return false;
  }
  static std::mutex s_routePulseMutex;
  static std::unordered_map<std::string, RouteOnlyPulseEvidence> s_routePulse;
  static DWORD s_routePulsePruneTick = 0;

  (void)callerOffset;
  // Caller can legitimately drift for the same in-game prompt.
  // Track pulse stability by route+key to avoid starving valid no-param hints.
  const std::string sig = std::to_string((int)route) + "|" + key;
  std::lock_guard<std::mutex> lk(s_routePulseMutex);
  RouteOnlyPulseEvidence &ev = s_routePulse[sig];
  if (ev.lastTick == 0 || now < ev.lastTick || (now - ev.lastTick) > 1400) {
    ev.firstTick = now;
    ev.lastTick = now;
    ev.pulses = 1;
  } else {
    ev.lastTick = now;
    if (ev.pulses < 0xFFFFu) {
      ++ev.pulses;
    }
  }

  if ((now - s_routePulsePruneTick) > 2000) {
    s_routePulsePruneTick = now;
    for (auto it = s_routePulse.begin(); it != s_routePulse.end();) {
      if ((now - it->second.lastTick) > 12000) {
        it = s_routePulse.erase(it);
      } else {
        ++it;
      }
    }
  }

  return (ev.pulses >= minPulses) &&
         (ev.firstTick != 0) &&
         ((now - ev.firstTick) >= minStableMs);
}

static bool HasRecentRouteNativeEvidence(const std::string &key,
                                         HudRouteChannel route,
                                         DWORD maxAgeMs) {
  HudNativeEntry native{};
  if (!TextHook_GetHudNativeEntry(key, native)) {
    return false;
  }
  const DWORD now = GetTickCount();
  if (native.lastUpdate == 0 || now < native.lastUpdate ||
      (now - native.lastUpdate) > maxAgeMs) {
    return false;
  }
  if (route == HUD_ROUTE_UNSPECIFIED) {
    return true;
  }
  HudRouteChannel nativeRoute = HUD_ROUTE_UNRESOLVED;
  if (!TextHook_GetHudRouteForNativeEntry(native, nativeRoute)) {
    return false;
  }
  return nativeRoute == route;
}

static bool HasMixedHudHintPromptCallers(const std::string &key,
                                         unsigned int currentCallerOffset,
                                         DWORD maxAgeMs) {
  if (key.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_HudNativeMutex);
  const DWORD now = GetTickCount();
  bool sawCurrent = false;
  std::unordered_set<unsigned int> callers;
  callers.reserve(4);
  for (const auto &kv : g_HudNativeMap) {
    const HudNativeEntry &cand = kv.second;
    if (cand.key != key) {
      continue;
    }
    if (cand.lastUpdate == 0 || now < cand.lastUpdate ||
        (now - cand.lastUpdate) > maxAgeMs) {
      continue;
    }
    HudRouteChannel route = HUD_ROUTE_UNRESOLVED;
    if (!ResolveHudFamilyRouteFromNative(cand, route) ||
        route != HUD_ROUTE_HINT) {
      continue;
    }
    callers.insert(cand.callerOffset);
    if (cand.callerOffset == currentCallerOffset) {
      sawCurrent = true;
    }
    if (callers.size() > 1 && sawCurrent) {
      return true;
    }
  }
  return false;
}

static bool HasRecentHudRuntimeTailJoinPromptActive(DWORD maxAgeMs) {
  std::string key;
  return GetRecentHudRuntimeTailJoinPromptKey(maxAgeMs, key);
}

static bool GetRecentHudRuntimeTailJoinPromptKey(DWORD maxAgeMs,
                                                 std::string &outKey) {
  outKey.clear();
  RecentHudRuntimeTailJoinPromptState recentState{};
  if (TryGetRecentHudRuntimeTailJoinPromptState(maxAgeMs, recentState)) {
    outKey = recentState.key;
    return true;
  }
  static const char *kTailJoinKeys[] = {
      "PLATFORM_SWAPWEAPONS",
      "PLATFORM_SWAPWEAPONSGAMEPAD",
      "PLATFORM_PICKUPNEWWEAPON",
      "PLATFORM_PICKUPNEWWEAPONGAMEPAD",
  };
  const DWORD now = GetTickCount();
  DWORD bestTick = 0;
  for (const char *key : kTailJoinKeys) {
    HudNativeEntry native{};
    if (!TextHook_GetHudNativeEntry(key, native)) {
      continue;
    }
    if (native.lastUpdate == 0 || now < native.lastUpdate ||
        (now - native.lastUpdate) > maxAgeMs) {
      continue;
    }
    if (outKey.empty() || native.lastUpdate > bestTick) {
      outKey = key;
      bestTick = native.lastUpdate;
    }
  }
  return !outKey.empty();
}

static void RefreshHintEntryRouteOnly(const std::string &key,
                                      const std::string *englishRuntime,
                                      unsigned int callerOffset,
                                      unsigned char sourceTag,
                                      int observedSlotBucket,
                                      float alphaHint) {
  if (TextHook_IsHudNativeEnabled()) {
    return;
  }
  if (key.empty()) {
    return;
  }

  auto itKor = g_KeyToKorean.find(key);
  if (itKor == g_KeyToKorean.end() || itKor->second.empty()) {
    return;
  }

  std::string english = key;
  if (englishRuntime && !englishRuntime->empty()) {
    std::string trimmed = TrimSpaces(*englishRuntime);
    if (!trimmed.empty()) {
      english = trimmed;
    }
  } else {
    auto itEng = g_KeyToEnglish.find(key);
    if (itEng != g_KeyToEnglish.end()) {
      std::string trimmed = TrimSpaces(itEng->second);
      if (!trimmed.empty()) {
        english = trimmed;
      }
    }
  }

  float hintColor[4] = {1.0f, 1.0f, 1.0f, 0.6f};
  DetermineHudColor(key, hintColor);
  const DWORD now = GetTickCount();
  std::string runtimeRecentEnglish;
  bool hasRecentRuntimeEnglish = false;
  if (englishRuntime && !englishRuntime->empty()) {
    runtimeRecentEnglish = TrimSpaces(*englishRuntime);
    hasRecentRuntimeEnglish = !runtimeRecentEnglish.empty();
  }
  if (!hasRecentRuntimeEnglish) {
    hasRecentRuntimeEnglish =
        TextHook_GetRuntimeEnglishForKeyRecent(key, runtimeRecentEnglish, 320);
  }
  if (hasRecentRuntimeEnglish && !runtimeRecentEnglish.empty()) {
    english = runtimeRecentEnglish;
  }
  const bool runtimeLooksPrompt =
      (!runtimeRecentEnglish.empty() &&
       (LooksLikePressHoldPrompt(runtimeRecentEnglish) ||
        HasBindingPlaceholderToken(runtimeRecentEnglish)));
  const bool tutorialHintKey = IsMovementTutorialHintKey(key);
  const bool hasRecentNativeHint =
      HasRecentRouteNativeEvidence(key, HUD_ROUTE_HINT, 440);
  const bool pulseStable =
      IsRouteOnlyPulseStable(key, callerOffset, HUD_ROUTE_HINT, now, 25, 2);
  const bool allowRouteRefresh =
      hasRecentNativeHint || runtimeLooksPrompt || tutorialHintKey ||
      (hasRecentRuntimeEnglish && pulseStable);
  if (!allowRouteRefresh) {
    static std::unordered_map<std::string, DWORD> s_routeDropLogTick;
    const std::string sig = key + "|" + std::to_string(callerOffset);
    auto it = s_routeDropLogTick.find(sig);
    if (it == s_routeDropLogTick.end() || (now - it->second) > 1200) {
      s_routeDropLogTick[sig] = now;
      char dbuf[384];
      sprintf_s(dbuf,
                "[HUD-ROUTE-ONLY-DROP] track=hint key=%s caller=+0x%X "
                "nativeRecent=%d runtimeRecent=%d pulseStable=%d tutorial=%d slot=%d",
                key.c_str(), callerOffset, hasRecentNativeHint ? 1 : 0,
                hasRecentRuntimeEnglish ? 1 : 0, pulseStable ? 1 : 0,
                tutorialHintKey ? 1 : 0,
                observedSlotBucket);
      LogToFile(dbuf);
    }
    return;
  }

  const uint8_t routeSourceToken = HintSourceTokenFromRouteSource(sourceTag);
  const OverlaySource routeOverlaySource =
      (routeSourceToken != 0)
          ? OverlaySourceFromNativeSourceToken(routeSourceToken)
          : OverlaySourceFromHintNativeSource(sourceTag);
  const uint64_t routeConsumerId = TextHook_MakeHudConsumerId(
      routeOverlaySource, callerOffset, observedSlotBucket, 0, 0);
  std::lock_guard<std::mutex> lock(g_HintMutex);
  for (auto &e : g_HintEntries) {
    if (e.key != key) {
      continue;
    }
    if (routeConsumerId != 0 && e.consumerId != 0 &&
        e.consumerId != routeConsumerId) {
      continue;
    }
    e.lastSeen = now;
    e.consumerId = routeConsumerId;
    e.slotBucket = observedSlotBucket;
    if (!english.empty()) {
      e.english = english;
      e.lastEnglishSeenTick = now;
    }
    if (e.nativeSource == 0) {
      e.nativeSource = sourceTag;
    }
    if (e.nativeSourceToken == 0) {
      e.nativeSourceToken = routeSourceToken;
    }
    if (e.callerOffset == 0 && callerOffset != 0) {
      e.callerOffset = callerOffset;
    }
    if (std::isfinite(alphaHint) && alphaHint >= 0.0f && alphaHint <= 1.0f) {
      e.color[3] = alphaHint;
    }
    // If no-param route repeatedly says this key lives in a lower slot
    // (cursor/bottom lanes) but the current native sample is SLC-center,
    // drop that stale SLC native sample to prevent size/position oscillation.
    if (sourceTag == (unsigned char)HUD_NATIVE_SCANNER &&
        observedSlotBucket >= 4 &&
        e.hasNativeParams &&
        e.nativeSource == (unsigned char)HUD_NATIVE_SCANNER) {
      const int nativeSlot =
          TextHook_HudSlotFromEvent(e.nativeY, e.nativeVirtualH, 0);
      if (nativeSlot > 0 && nativeSlot < observedSlotBucket) {
        e.hasNativeParams = false;
        e.nativeX = 0.0f;
        e.nativeY = 0.0f;
        e.nativeXScale = 0.0f;
        e.nativeYScale = 0.0f;
        e.fontHeight = 0.0f;
        e.nativeVirtualW = 0;
        e.nativeVirtualH = 0;
        e.lastNativeSeenTick = 0;
      }
    }
    return;
  }

  if (g_HintEntries.size() >= HINT_ENTRY_MAX) {
    auto oldest =
        std::min_element(g_HintEntries.begin(), g_HintEntries.end(),
                         [](const HintOverlayEntry &a, const HintOverlayEntry &b) {
                           return a.lastSeen < b.lastSeen;
                         });
    if (oldest != g_HintEntries.end()) {
      g_HintEntries.erase(oldest);
    }
  }

  HintOverlayEntry hEntry{};
  hEntry.key = key;
  hEntry.korean = itKor->second;
  hEntry.english = english;
  hEntry.consumerId = routeConsumerId;
  hEntry.slotBucket = observedSlotBucket;
  hEntry.nativeX = 0.0f;
  hEntry.nativeY = 0.0f;
  hEntry.nativeXScale = 0.0f;
  hEntry.nativeYScale = 0.0f;
  hEntry.fontHeight = 0.0f;
  hEntry.color[0] = hintColor[0];
  hEntry.color[1] = hintColor[1];
  hEntry.color[2] = hintColor[2];
  hEntry.color[3] =
      (std::isfinite(alphaHint) && alphaHint >= 0.0f && alphaHint <= 1.0f)
          ? alphaHint
          : hintColor[3];
  hEntry.firstSeen = now;
  hEntry.lastSeen = now;
  hEntry.hasNativeParams = false;
  hEntry.nativeSource = sourceTag;
  hEntry.nativeSourceToken = routeSourceToken;
  hEntry.nativeVirtualW = 0;
  hEntry.nativeVirtualH = 0;
  hEntry.callerOffset = callerOffset;
  hEntry.lastNativeSeenTick = 0;
  hEntry.lastEnglishSeenTick = english.empty() ? 0 : now;
  hEntry.lastRenderedTick = 0;
  g_HintEntries.push_back(std::move(hEntry));
}

static void RefreshObjHudHintEntryRouteOnly(const std::string &key,
                                            const std::string *englishRuntime,
                                            unsigned int callerOffset,
                                            unsigned char sourceTag,
                                            int observedSlotBucket) {
  if (TextHook_IsHudNativeEnabled()) {
    return;
  }
  if (key.empty()) {
    return;
  }
  const DWORD now = GetTickCount();
  std::string runtimeRecentEnglish;
  bool hasRecentRuntimeEnglish = false;
  if (englishRuntime && !englishRuntime->empty()) {
    runtimeRecentEnglish = TrimSpaces(*englishRuntime);
    hasRecentRuntimeEnglish = !runtimeRecentEnglish.empty();
  }
  if (!hasRecentRuntimeEnglish) {
    hasRecentRuntimeEnglish =
        TextHook_GetRuntimeEnglishForKeyRecent(key, runtimeRecentEnglish, 260);
  }
  const bool runtimeLooksPrompt =
      (!runtimeRecentEnglish.empty() &&
       (LooksLikePressHoldPrompt(runtimeRecentEnglish) ||
        HasBindingPlaceholderToken(runtimeRecentEnglish)));
  const bool hasRecentNativeObj =
      HasRecentRouteNativeEvidence(key, HUD_ROUTE_OBJHUDHINT, 360);
  const bool pulseStable = IsRouteOnlyPulseStable(
      key, callerOffset, HUD_ROUTE_OBJHUDHINT, now, 35, 2);
  const bool allowRouteRefresh =
      hasRecentNativeObj ||
      (runtimeLooksPrompt && hasRecentRuntimeEnglish && pulseStable);
  if (!allowRouteRefresh) {
    return;
  }
  const uint8_t routeSourceToken = HintSourceTokenFromRouteSource(sourceTag);

  std::lock_guard<std::mutex> lock(g_ObjHudHintMutex);
  for (auto &e : g_ObjHudHintEntries) {
    if (e.key != key) {
      continue;
    }
    const bool staleNative =
        (e.lastNativeSeenTick == 0 || now < e.lastNativeSeenTick ||
         (now - e.lastNativeSeenTick) > 520);
    if (e.hasNativeParams && observedSlotBucket <= 0 &&
        e.lastNativeSeenTick != 0 && now >= e.lastNativeSeenTick &&
        (now - e.lastNativeSeenTick) > 260) {
      // Unknown-slot no-param refresh must not keep stale native object prompts alive.
      return;
    }
    if (!e.hasNativeParams && staleNative && !hasRecentNativeObj) {
      return;
    }
    e.lastSeen = now;
    if (!runtimeRecentEnglish.empty()) {
      std::string trimmed = TrimSpaces(runtimeRecentEnglish);
      if (!trimmed.empty()) {
        e.english = trimmed;
        e.lastEnglishSeenTick = now;
      }
    }
    if (e.nativeSource == 0) {
      e.nativeSource = sourceTag;
    }
    if (e.nativeSourceToken == 0) {
      e.nativeSourceToken = routeSourceToken;
    }
    if (e.callerOffset == 0 && callerOffset != 0) {
      e.callerOffset = callerOffset;
    }
    return;
  }
}

// ??? Phase 3: Refresh hint entry with native params from HUD560 ?????????????
bool TextHook_PromoteStrictHintSnapshotByCfg(uint32_t textCfg,
                                             uint32_t labelCfg,
                                             float x,
                                             float y,
                                             float fontScale,
                                             uint32_t horzAlign,
                                             uint32_t vertAlign,
                                             uint32_t colorPacked,
                                             int elemTime,
                                             int fxBirthTime,
                                             int fxLetterTime,
                                             int flags,
                                             unsigned int callerOffset,
                                             const char *logTag) {
  bool resolvedFromHintCfgCache = false;
  auto resolveStrictHintKey = [&](uint32_t cfg) -> std::string {
    if (!ObjRev_IsPlausibleConfigIndex(cfg)) {
      return "";
    }
    std::string key;
    if (TextHook_TryResolveRecentHudHintCfgKey(cfg, key, nullptr, nullptr,
                                               logTag)) {
      resolvedFromHintCfgCache = true;
      return key;
    }
    // Try direct first, then allow full probe (negative caching prevents
    // repeated 768-iteration scans for cfgIndices that already failed).
    key = ObjRev_ResolveConfigKey(cfg, false, true);
    if (key.empty()) {
      key = ObjRev_ResolveConfigKey(cfg, false, false);
    }
    if (key.empty()) {
      return "";
    }
    if (IsAmbiguousGenericPlatformHoldKey(key)) {
      return "";
    }
    // Fallback-classified keys (HUD_KEY_OBJ_HUD_HINT) bypass the objective
    // observation gate — they are gameplay text the objective system cannot
    // render (lane < 0). Blocking them here silently drops them.
    const bool isFallbackKey =
        (ClassifyHudKey(key) == HUD_KEY_OBJ_HUD_HINT);
    if (IsObjectiveOverlayKeyName(key) ||
        (!isFallbackKey && HasRecentObjectiveObservationForKey(key, 6000)) ||
        IsObjectiveStatusMessageKeyName(key) ||
        key.find("INTROSCREEN") != std::string::npos ||
        IsDirectAddCmdObjHudHintKey(key) ||
        IsHudRuntimeTailJoinPromptKey(key)) {
      return "";
    }
    if (!IsStrictHintCaptureCandidateKey(key)) {
      return "";
    }
    EnsureTranslationsLoaded();
    auto itKor = g_KeyToKorean.find(key);
    if (itKor == g_KeyToKorean.end() || itKor->second.empty()) {
      return "";
    }
    // Guardrail:
    // SCRIPT_* press/hold prompts must have fresh hint-path cfg evidence.
    // Without this, shared cfg reverse can resurrect stale mission prompts
    // (e.g. SCRIPT_RORKEFILE_PICKUP) from unrelated top-left/status traffic.
    if (!resolvedFromHintCfgCache && key.rfind("SCRIPT_", 0) == 0 &&
        TextHook_OSRev_IsPromptLikeLocalizedKey(key)) {
      return "";
    }
    return key;
  };

  std::string strictHintKey = resolveStrictHintKey(textCfg);
  if (strictHintKey.empty() && labelCfg != textCfg) {
    strictHintKey = resolveStrictHintKey(labelCfg);
  }
  if (strictHintKey.empty()) {
    return false;
  }

  auto LooksLikeObjectiveLaneGeometry = [](float rawX, float rawY,
                                           float rawFontScale,
                                           int rawFlags) -> bool {
    if (!std::isfinite(rawX) || !std::isfinite(rawY) ||
        !std::isfinite(rawFontScale)) {
      return false;
    }
    if (!(rawFlags == 0x5 || rawFlags == 0x7)) {
      return false;
    }
    if (fabsf(rawX - 6.0f) > 8.0f) {
      return false;
    }
    if (rawFontScale < 0.85f || rawFontScale > 1.40f) {
      return false;
    }
    static const float kObjectiveLaneYs[] = {10.0f, 30.0f, 50.0f, 70.0f};
    for (float laneY : kObjectiveLaneYs) {
      if (fabsf(rawY - laneY) <= 12.0f) {
        return true;
      }
    }
    return false;
  };

  const HudKeyType strictHintKeyType = ClassifyHudKey(strictHintKey);
  const bool fallbackObjectiveLaneLeak =
      (strictHintKeyType == HUD_KEY_OBJ_HUD_HINT) &&
      LooksLikeObjectiveLaneGeometry(x, y, fontScale, flags);
  if (fallbackObjectiveLaneLeak) {
    RemoveHintEntryByKey(strictHintKey);
    RemoveObjHudHintEntryByKey(strictHintKey);

    static std::unordered_map<std::string, unsigned long> s_objLaneSkipLogTick;
    const unsigned long nowSkip = GetTickCount();
    auto itSkip = s_objLaneSkipLogTick.find(strictHintKey);
    if (itSkip == s_objLaneSkipLogTick.end() ||
        (nowSkip - itSkip->second) > 1200) {
      s_objLaneSkipLogTick[strictHintKey] = nowSkip;
      char sbuf[512];
      sprintf_s(
          sbuf,
          "[HUD-STRICT-OBJLANE-SKIP] key=%s caller=+0x%X raw=(%.1f,%.1f) fs=%.2f flags=0x%X cfg=0x%X",
          strictHintKey.c_str(), callerOffset, x, y, fontScale, flags,
          textCfg != 0 ? textCfg : labelCfg);
      LogToFile(sbuf);
    }
    return false;
  }

  float nx = x;
  float ny = y;
  bool usedRelative = false;
  const bool maybeCenterRelativeX =
      std::isfinite(nx) && nx >= -160.0f && nx <= 160.0f;
  const bool maybeCenterRelativeY =
      std::isfinite(ny) && ny >= -240.0f && ny <= 160.0f;
  if ((horzAlign != 0 || vertAlign != 0 || callerOffset == IW6Offsets::Profile::Rva_2316DB) &&
      maybeCenterRelativeX && maybeCenterRelativeY) {
    nx += 320.0f;
    ny += 240.0f;
    usedRelative = true;
  }

  const float clampedFontScale =
      (std::isfinite(fontScale) && fontScale >= 0.20f && fontScale <= 3.20f)
          ? fontScale
          : 1.0f;
  const float colorR = (float)((colorPacked >> 0) & 0xFFu) / 255.0f;
  const float colorG = (float)((colorPacked >> 8) & 0xFFu) / 255.0f;
  const float colorB = (float)((colorPacked >> 16) & 0xFFu) / 255.0f;
  const float colorA = (float)((colorPacked >> 24) & 0xFFu) / 255.0f;

  HudNativeEntry entry{};
  entry.key = strictHintKey;
  entry.x = nx;
  entry.y = ny;
  entry.xScale = clampedFontScale;
  entry.yScale = clampedFontScale;
  entry.fontHeight = 35.0f;
  entry.color[0] = colorR;
  entry.color[1] = colorG;
  entry.color[2] = colorB;
  entry.color[3] = colorA;
  entry.style = 0;
  entry.lastUpdate = GetTickCount();
  entry.source = HUD_NATIVE_RENDER;
  entry.sourceToken = NATIVE_HUD_SOURCE_BRIDGE;
  entry.virtualW = 640;
  entry.virtualH = 480;
  entry.callerOffset = callerOffset;
  entry.objectiveChannel = (unsigned char)OBJ_CHANNEL_NONE;
  entry.slotBucket = TextHook_HudSlotFromEvent(entry.y, entry.virtualH, 0);
  entry.objectiveLike = false;
  entry.valid = true;
  TextHook_UpdateHudNativeEntry(entry);
  RefreshHintEntryNative(strictHintKey, entry, nullptr);

  static std::unordered_map<std::string, unsigned long> s_strictHintLogTick;
  const unsigned long nowHint = GetTickCount();
  auto itHintLog = s_strictHintLogTick.find(strictHintKey);
  if (itHintLog == s_strictHintLogTick.end() ||
      (nowHint - itHintLog->second) > 1200) {
    s_strictHintLogTick[strictHintKey] = nowHint;
    char hbuf[512];
    sprintf_s(
        hbuf,
        "[%s] key=%s caller=+0x%X raw=(%.1f,%.1f) norm=(%.1f,%.1f) fs=%.2f a=%.2f cfg=0x%X align=(0x%X,0x%X) rel=%d t=%d fxB=%d fxL=%d flags=0x%X",
        (logTag && *logTag) ? logTag : "HUD-STRICT-NATIVE",
        strictHintKey.c_str(), callerOffset, x, y, nx, ny, clampedFontScale,
        colorA, textCfg != 0 ? textCfg : labelCfg, horzAlign, vertAlign,
        usedRelative ? 1 : 0, elemTime, fxBirthTime, fxLetterTime, flags);
    LogToFile(hbuf);
  }

  return true;
}

static void RefreshHintEntryNative(const std::string &key,
                                   const HudNativeEntry &native,
                                   const std::string *englishRuntime) {
  DWORD now = GetTickCount();
  // Prefer ADDCMD screen-space geometry for tail-join keys and
  // CLOCKWORK_HINT_DRILL keys.  Drill keys arrive via both HUD560
  // (HudElem virtual coords) and ADDCMD (actual draw position);
  // HUD560 Y maps to a wrong screen position, causing misalignment
  // and jumping.  ADDCMD coords match the English text exactly.
  const bool preferAddCmdLayout =
      IsHudRuntimeTailJoinPromptKey(key) ||
      (key.rfind("CLOCKWORK_HINT_DRILL", 0) == 0);
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
  const uint64_t nativeConsumerId = MakeHintConsumerIdFromNative(native, nativeSlot);
  auto LogHintNativeDecision = [&](const char *action,
                                   const HintOverlayEntry *existing,
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
      static std::mutex s_hintLogMtx;
      static std::unordered_map<uint64_t, DWORD> s_hintLogTick;
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
      const uint64_t hVerbose = h ^ 0xA5A5A5A5A5A5A5A5ull;
      bool shouldLogVerbose = false;
      bool shouldLogDiag = false;
      {
        std::lock_guard<std::mutex> lk(s_hintLogMtx);
        if (kVerboseRuntimeLogs) {
          auto it = s_hintLogTick.find(hVerbose);
          if (it == s_hintLogTick.end() ||
              (now - it->second) > minIntervalMs) {
            s_hintLogTick[hVerbose] = now;
            shouldLogVerbose = true;
          }
        }
        {
          auto it = s_hintLogTick.find(h);
          if (it == s_hintLogTick.end() ||
              (now - it->second) > minIntervalMs) {
            s_hintLogTick[h] = now;
            shouldLogDiag = true;
          }
        }
        if (s_hintLogTick.size() > 640) {
          for (auto jt = s_hintLogTick.begin();
               jt != s_hintLogTick.end();) {
            if ((now - jt->second) > 30000) {
              jt = s_hintLogTick.erase(jt);
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
            "[NHUD-NATIVE-%s] key=%s reason=%s inCaller=+0x%X inSlot=%d inSrc=%d inXY=(%.1f,%.1f) inAge=%lums curCaller=+0x%X curSlot=%d curSrc=%u curXY=(%.1f,%.1f) curAge=%lums dt=%lums",
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
            "[HUDHINT-NATIVE-%s] key=%s reason=%s inCaller=+0x%X inSrc=%d inSlot=%d inXY=(%.1f,%.1f) inScale=(%.3f,%.3f) inH=%.1f inA=%.2f inAge=%lums curCaller=+0x%X curSrc=%u curSlot=%d curXY=(%.1f,%.1f) curScale=(%.3f,%.3f) curH=%.1f curA=%.2f curAge=%lums dt=%lums",
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
    std::lock_guard<std::mutex> lock(g_HintMutex);
    for (auto &e : g_HintEntries) {
      if (e.key == key) {
        const uint64_t entryConsumerId = GetHintEntryConsumerId(e);
        const int entrySlot = SlotFromNative(e.nativeY, e.nativeVirtualH);
        if (nativeConsumerId != 0 && entryConsumerId != 0 &&
            entryConsumerId != nativeConsumerId) {
          continue;
        }
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
                   incomingFromAddCmd && (now - prevSeen) <= 450);

        if (keepExistingNative) {
          e.lastSeen = now;
          e.lastNativeSeenTick = now;
          const char *keepReason =
              preferAddCmdLayout ? "prefer_addcmd_layout"
                                 : "prefer_existing_virtual";
          const DWORD keepDelta = (now >= prevSeen) ? (now - prevSeen) : 0;
          LogHintNativeDecision("KEEP", &e, keepReason, entrySlot, keepDelta);
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
          LogHintNativeDecision("DROP", &e, "slot_mismatch", entrySlot, 0);
          continue;
        }
        if (e.callerOffset != 0 && native.callerOffset != 0 &&
            e.callerOffset != native.callerOffset) {
          LogHintNativeDecision("DROP", &e, "caller_mismatch", entrySlot, 0);
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
        e.consumerId = nativeConsumerId;
        e.slotBucket = nativeSlot;
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
        if (std::isfinite(native.color[3]) && native.color[3] > 0.01f) {
          e.color[3] = clamp01(native.color[3], e.color[3]);
        }
        if (englishRuntime && !englishRuntime->empty()) {
          std::string trimmed = TrimSpaces(*englishRuntime);
          if (!trimmed.empty())
            e.english = trimmed;
          e.lastEnglishSeenTick = now;
        }
        {
          const DWORD applyDelta = (now >= prevSeen) ? (now - prevSeen) : 0;
          LogHintNativeDecision("APPLY", &e, "native_refresh", entrySlot,
                                applyDelta);
        }
        refreshedExisting = true;
        break;
      }
    }
    if (!refreshedExisting) {
      // Entry not yet created by FSHook -- create it from HUD560 data (bridge)
      auto itKor = g_KeyToKorean.find(key);
      if (itKor == g_KeyToKorean.end() || itKor->second.empty())
        return;

      if (g_HintEntries.size() >= HINT_ENTRY_MAX) {
        auto oldest = std::min_element(
            g_HintEntries.begin(), g_HintEntries.end(),
            [](const HintOverlayEntry &a, const HintOverlayEntry &b) {
              return a.lastSeen < b.lastSeen;
            });
        if (oldest != g_HintEntries.end())
          g_HintEntries.erase(oldest);
      }

      HintOverlayEntry hEntry{};
      hEntry.key = key;
      hEntry.korean = itKor->second;
      hEntry.english = (englishRuntime && !englishRuntime->empty())
                           ? TrimSpaces(*englishRuntime)
                           : key;
      hEntry.consumerId = nativeConsumerId;
      hEntry.slotBucket = nativeSlot;
      hEntry.nativeX = native.x;
      hEntry.nativeY = native.y;
      hEntry.nativeXScale = native.xScale;
      hEntry.nativeYScale = native.yScale;
      hEntry.fontHeight = native.fontHeight;
      hEntry.color[0] = native.color[0];
      hEntry.color[1] = native.color[1];
      hEntry.color[2] = native.color[2];
      hEntry.color[3] = native.color[3];
      hEntry.firstSeen = now;
      hEntry.lastSeen = now;
      hEntry.hasNativeParams = true;
      hEntry.nativeSource = (unsigned char)native.source;
      hEntry.nativeSourceToken = nativeSourceToken;
      hEntry.nativeVirtualW = native.virtualW;
      hEntry.nativeVirtualH = native.virtualH;
      hEntry.callerOffset = native.callerOffset;
      hEntry.lastNativeSeenTick = now;
      hEntry.lastEnglishSeenTick = hEntry.english.empty() ? 0 : now;
      hEntry.lastRenderedTick = 0;
      g_HintEntries.push_back(hEntry);
      LogHintNativeDecision("CREATE", nullptr, "new_entry", nativeSlot, 0);
    }
  }
}
static int HudNativeSourcePriority(HudNativeSource src) {
  if (src == HUD_NATIVE_RENDER) {
    return 3;
  }
  if (src == HUD_NATIVE_SCANNER) {
    return 2;
  }
  if (src == HUD_NATIVE_ADDCMD) {
    return 1;
  }
  return 0;
}

static int HudNativeSourcePriorityForKey(const std::string &key,
                                         HudNativeSource src) {
  if (IsHudRuntimeTailJoinPromptKey(key)) {
    if (src == HUD_NATIVE_ADDCMD) {
      return 3;
    }
    if (src == HUD_NATIVE_RENDER) {
      return 2;
    }
    if (src == HUD_NATIVE_SCANNER) {
      return 1;
    }
    return 0;
  }
  return HudNativeSourcePriority(src);
}

static int HudOverlaySourcePriority(OverlaySource src) {
  if (src == OVERLAY_SOURCE_HUD560) {
    return 3;
  }
  if (src == OVERLAY_SOURCE_ADDCMD) {
    return 2;
  }
  return 1;
}

static int HudOverlaySourcePriorityForKey(const std::string &key,
                                          OverlaySource src) {
  if (IsHudRuntimeTailJoinPromptKey(key)) {
    if (src == OVERLAY_SOURCE_ADDCMD) {
      return 3;
    }
    if (src == OVERLAY_SOURCE_HUD560) {
      return 2;
    }
    return 1;
  }
  return HudOverlaySourcePriority(src);
}

bool TextHook_GetHudNativeEntry(const std::string &key, HudNativeEntry &out) {
  if (key.empty())
    return false;
  std::lock_guard<std::mutex> lock(g_HudNativeMutex);
  DWORD now = GetTickCount();

  // Prune stale entries
  if (g_HudNativeMap.size() > 200) {
    for (auto it = g_HudNativeMap.begin(); it != g_HudNativeMap.end();) {
      if ((now - it->second.lastUpdate) > HUD_NATIVE_STALE)
        it = g_HudNativeMap.erase(it);
      else
        ++it;
    }
  }

  const HudNativeEntry *best = nullptr;
  constexpr DWORD kSourcePreferWindowMs = 450;
  auto hasUsableScreenPos = [](const HudNativeEntry &e) -> bool {
    return std::isfinite(e.x) && std::isfinite(e.y) &&
           (std::fabs(e.x) > 1.0f || std::fabs(e.y) > 1.0f);
  };
  for (const auto &kv : g_HudNativeMap) {
    const HudNativeEntry &cand = kv.second;
    if (cand.key != key) {
      continue;
    }
    if ((now - cand.lastUpdate) >= HUD_NATIVE_TIMEOUT) {
      continue;
    }
    if (!best) {
      best = &cand;
      continue;
    }

    const int candPri = HudNativeSourcePriorityForKey(key, cand.source);
    const int bestPri = HudNativeSourcePriorityForKey(key, best->source);
    const DWORD ageDiff =
        (cand.lastUpdate >= best->lastUpdate)
            ? (cand.lastUpdate - best->lastUpdate)
            : (best->lastUpdate - cand.lastUpdate);

    if (candPri != bestPri && ageDiff <= kSourcePreferWindowMs) {
      if (candPri > bestPri) {
        best = &cand;
      }
      continue;
    }

    // Same producer can emit slot-noise samples (e.g. HUD560 slot-1 origin
    // alongside real slot-5 mantle prompt). Prefer usable position and
    // lower-screen slot to keep one stable native candidate.
    if (cand.source == best->source &&
        cand.callerOffset == best->callerOffset) {
      const bool candUsable = hasUsableScreenPos(cand);
      const bool bestUsable = hasUsableScreenPos(*best);
      if (candUsable != bestUsable) {
        if (candUsable) {
          best = &cand;
        }
        continue;
      }

      const int candSlot = TextHook_HudSlotFromEvent(cand.y, cand.virtualH, 0);
      const int bestSlot = TextHook_HudSlotFromEvent(best->y, best->virtualH, 0);
      if (candSlot != bestSlot) {
        if (candSlot > bestSlot) {
          best = &cand;
        }
        continue;
      }
    }

    if (cand.lastUpdate > best->lastUpdate ||
        (cand.lastUpdate == best->lastUpdate && candPri > bestPri)) {
      best = &cand;
    }
  }
  if (best) {
    out = *best;
    return true;
  }
  return false;
}
std::vector<HudNativeEntry> TextHook_GetHudNativeEntries() {
  std::lock_guard<std::mutex> lock(g_HudNativeMutex);
  std::vector<HudNativeEntry> out;
  out.reserve(g_HudNativeMap.size());
  for (const auto &kv : g_HudNativeMap) {
    out.push_back(kv.second);
  }
  return out;
}

static uint64_t NativeHudHash64(const std::string &text) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : text) {
    h ^= (uint64_t)c;
    h *= 1099511628211ull;
  }
  return h;
}

static uint64_t MakeNativeHudInstanceIdFromEntry(const HudNativeEntry &entry) {
  const uint8_t sourceToken = HintSourceTokenFromHudNativeEntry(entry);
  if (entry.consumerId != 0) {
    return 0xA100000000000000ull ^
           ((uint64_t)(unsigned int)sourceToken << 56) ^
           ((uint64_t)(unsigned int)entry.source << 52) ^
           entry.consumerId ^
           (NativeHudHash64(entry.key) & 0x0000FFFFFFFFFFFFull);
  }
  const int slot = TextHook_HudSlotFromEvent(entry.y, entry.virtualH, 0);
  const int qx = std::isfinite(entry.x) ? (int)lroundf(entry.x * 4.0f) : 0;
  const int qy = std::isfinite(entry.y) ? (int)lroundf(entry.y * 4.0f) : 0;
  uint64_t v = 0xB200000000000000ull;
  v ^= ((uint64_t)(unsigned int)sourceToken & 0xFFull) << 56;
  v ^= ((uint64_t)(unsigned int)entry.source & 0xFFull) << 48;
  v ^= ((uint64_t)entry.callerOffset & 0xFFFFFFull) << 24;
  v ^= ((uint64_t)(unsigned int)(slot & 0xFF)) << 16;
  v ^= ((uint64_t)(unsigned int)(qx & 0xFFFF)) << 32;
  v ^= ((uint64_t)(unsigned int)(qy & 0xFFFF)) << 0;
  v ^= NativeHudHash64(entry.key);
  return v;
}

static NativeHudTrack ClassifyNativeHudTrack(const HudNativeEntry &entry,
                                             int slot) {
  (void)slot;
  HudRouteChannel channel = HUD_ROUTE_UNRESOLVED;
  if (!ResolveHudFamilyRouteFromNative(entry, channel)) {
    return NATIVE_HUD_TRACK_MISC;
  }
  switch (channel) {
  case HUD_ROUTE_OBJECTIVE:
    return NATIVE_HUD_TRACK_OBJECTIVE;
  case HUD_ROUTE_OBJHUDHINT:
    return NATIVE_HUD_TRACK_OBJHINT;
  case HUD_ROUTE_HINT:
    return NATIVE_HUD_TRACK_HINT;
  default:
    return NATIVE_HUD_TRACK_MISC;
  }
}

#if GHOSTSKOR_OBJECTIVE_REVERSE
static uint64_t MakeNativeHudInstanceIdFromLiveElem(const LiveHudElem &elem) {
  uint64_t v = 0xC300000000000000ull;
  v ^= ((uint64_t)(elem.cfgIndex & 0xFFFFu)) << 40;
  v ^= ((uint64_t)(unsigned int)(elem.arrayIndex & 0xFFFF)) << 16;
  v ^= ((uint64_t)(unsigned int)(elem.fxBirthTime & 0xFFFF));
  v ^= NativeHudHash64(elem.key);
  return v;
}

static NativeHudTrack ClassifyNativeHudTrack(const LiveHudElem &elem) {
  // Center-screen warnings (va=5) must use HUDHINT — the objective
  // renderer cannot handle center-aligned negative-Y coordinates.
  if (elem.key == "ODIN_STRAY_INTRO_WARNING") {
    return NATIVE_HUD_TRACK_HINT;
  }
  if (elem.objectiveLane) {
    return NATIVE_HUD_TRACK_OBJECTIVE;
  }
  if (elem.hintBridgeLane) {
    return NATIVE_HUD_TRACK_HINT;
  }
  return NATIVE_HUD_TRACK_MISC;
}
#endif

static void NativeHudUnpackColorABGR(uint32_t packed, float out[4]) {
  out[0] = (float)((packed >> 0) & 0xFFu) / 255.0f;
  out[1] = (float)((packed >> 8) & 0xFFu) / 255.0f;
  out[2] = (float)((packed >> 16) & 0xFFu) / 255.0f;
  out[3] = (float)((packed >> 24) & 0xFFu) / 255.0f;
}

bool TextHook_GetNativeHudFrameSnapshot(NativeHudFrameSnapshot &out) {
  const DWORD now = GetTickCount();
  const DWORD perfStart = now;
  out = {};
  out.sampleTick = now;

  std::vector<HudNativeEntry> liveNativeEntries;
  {
    std::lock_guard<std::mutex> lock(g_HudNativeMutex);
    liveNativeEntries.reserve(g_HudNativeMap.size());
    for (auto it = g_HudNativeMap.begin(); it != g_HudNativeMap.end();) {
      const DWORD age = (now >= it->second.lastUpdate)
                            ? (now - it->second.lastUpdate)
                            : 0;
      if (age > HUD_NATIVE_STALE) {
        it = g_HudNativeMap.erase(it);
        continue;
      }
      if (age <= HUD_NATIVE_TIMEOUT) {
        liveNativeEntries.push_back(it->second);
      }
      ++it;
    }
  }

  out.nativeEntries = liveNativeEntries;
  out.lastHudNativeTick = 0;
  for (const auto &e : liveNativeEntries) {
    if (e.lastUpdate > out.lastHudNativeTick) {
      out.lastHudNativeTick = e.lastUpdate;
    }
  }

  std::unordered_map<uint64_t, NativeHudInstance> frameById;
  frameById.reserve(liveNativeEntries.size() * 2 + 32);
  out.events.reserve(liveNativeEntries.size() * 2 + 32);

  for (const auto &entry : liveNativeEntries) {
    if (entry.key.empty()) {
      continue;
    }
    const DWORD age = (now >= entry.lastUpdate) ? (now - entry.lastUpdate) : 0;
    if (age > 420) {
      continue;
    }

    const uint64_t iid = MakeNativeHudInstanceIdFromEntry(entry);
    const int slot = TextHook_HudSlotFromEvent(entry.y, entry.virtualH, 0);
    NativeHudInstance inst{};
    inst.instanceId = iid;
    inst.track = ClassifyNativeHudTrack(entry, slot);
    inst.sourceToken = HintSourceTokenFromHudNativeEntry(entry);
    inst.hudElemPtr = 0;
    inst.ownerdrawId = 0;
    inst.cfgIndex = 0;
    inst.arrayIndex = -1;
    inst.callerOffset = entry.callerOffset;
    inst.keyToken = entry.key;
    inst.x = entry.x;
    inst.y = entry.y;
    inst.virtualW = entry.virtualW;
    inst.virtualH = entry.virtualH;
    inst.xScale = entry.xScale;
    inst.yScale = entry.yScale;
    inst.fontHeight = entry.fontHeight;
    inst.color[0] = entry.color[0];
    inst.color[1] = entry.color[1];
    inst.color[2] = entry.color[2];
    inst.color[3] = entry.color[3];
    inst.fromColor[0] = entry.color[0];
    inst.fromColor[1] = entry.color[1];
    inst.fromColor[2] = entry.color[2];
    inst.fromColor[3] = entry.color[3];
    inst.fadeStartTime = 0;
    inst.fadeTime = 0;
    inst.fxBirthTime = 0;
    inst.fxLetterTime = 0;
    inst.serverTime = 0;
    inst.style = entry.style;
    inst.flags = 0;
    inst.firstSeenTick = entry.lastUpdate;
    inst.lastSeenTick = entry.lastUpdate;
    inst.seenFrames = 1;
    inst.hasNativeEvidence = entry.valid;
    inst.objectiveLaneEvidence = (inst.track == NATIVE_HUD_TRACK_OBJECTIVE);

    std::string runtimeEnglish;
    if (TextHook_GetRuntimeEnglishForKeyRecent(entry.key, runtimeEnglish, 320)) {
      runtimeEnglish = TrimSpaces(StripColorCodes(runtimeEnglish));
      inst.rawText = runtimeEnglish;
    }
    inst.hasEnglishEvidence = !inst.rawText.empty();

    auto it = frameById.find(iid);
    if (it == frameById.end() || inst.lastSeenTick >= it->second.lastSeenTick) {
      frameById[iid] = inst;
    }

    NativeHudEvent ev{};
    ev.sourceToken = inst.sourceToken;
    ev.track = inst.track;
    ev.tick = inst.lastSeenTick;
    ev.instanceId = inst.instanceId;
    ev.hudElemPtr = 0;
    ev.ownerdrawId = 0;
    ev.cfgIndex = 0;
    ev.arrayIndex = -1;
    ev.x = inst.x;
    ev.y = inst.y;
    ev.virtualW = inst.virtualW;
    ev.virtualH = inst.virtualH;
    ev.xScale = inst.xScale;
    ev.yScale = inst.yScale;
    ev.fontHeight = inst.fontHeight;
    ev.color[0] = inst.color[0];
    ev.color[1] = inst.color[1];
    ev.color[2] = inst.color[2];
    ev.color[3] = inst.color[3];
    ev.fromColor[0] = inst.fromColor[0];
    ev.fromColor[1] = inst.fromColor[1];
    ev.fromColor[2] = inst.fromColor[2];
    ev.fromColor[3] = inst.fromColor[3];
    ev.fadeStartTime = 0;
    ev.fadeTime = 0;
    ev.fxBirthTime = 0;
    ev.fxLetterTime = 0;
    ev.serverTime = 0;
    ev.style = inst.style;
    ev.flags = inst.flags;
    ev.callerOffset = inst.callerOffset;
    ev.keyToken = inst.keyToken;
    ev.rawText = inst.rawText;
    ev.hasNativeEvidence = inst.hasNativeEvidence;
    ev.hasEnglishEvidence = inst.hasEnglishEvidence;
    out.events.push_back(std::move(ev));
  }

#if GHOSTSKOR_OBJECTIVE_REVERSE
  std::vector<LiveHudElem> liveElems;
  {
    std::lock_guard<std::mutex> lock(g_liveHudElemsMutex);
    liveElems = g_liveHudElems;
    out.lastHudElemTick = g_liveHudElemsLastUpdate;
  }
  {
    std::vector<LiveHudElem> runtimeHintElems =
        TextHook_GetRuntimeHudHintLiveElems();
    if (!runtimeHintElems.empty()) {
      liveElems.reserve(liveElems.size() + runtimeHintElems.size());
      std::move(runtimeHintElems.begin(), runtimeHintElems.end(),
                std::back_inserter(liveElems));
      out.lastHudElemTick = now;
    }
  }
  for (const auto &elem : liveElems) {
    if (!elem.active) {
      continue;
    }
    if (elem.key.empty()) {
      continue;
    }
    const NativeHudTrack track = ClassifyNativeHudTrack(elem);
    if (track == NATIVE_HUD_TRACK_MISC) {
      continue;
    }

    NativeHudInstance inst{};
    inst.instanceId = MakeNativeHudInstanceIdFromLiveElem(elem);
    inst.track = track;
    inst.sourceToken = NATIVE_HUD_SOURCE_HUDELEM;
    inst.hudElemPtr = 0;
    inst.ownerdrawId = 0;
    inst.cfgIndex = elem.cfgIndex;
    inst.arrayIndex = elem.arrayIndex;
    inst.callerOffset = 0;
    inst.keyToken = elem.key;
    inst.x = elem.x;
    inst.y = elem.y;
    inst.virtualW = 640;
    inst.virtualH = 480;
    inst.xScale = elem.fontScale;
    inst.yScale = elem.fontScale;
    inst.fontHeight = 35.0f;
    NativeHudUnpackColorABGR(elem.color, inst.color);
    NativeHudUnpackColorABGR(elem.fromColor, inst.fromColor);
    inst.fadeStartTime = elem.fadeStartTime;
    inst.fadeTime = elem.fadeTime;
    inst.fxBirthTime = elem.fxBirthTime;
    inst.fxLetterTime = elem.fxLetterTime;
    inst.serverTime = 0;
    inst.style = 0;
    inst.flags = (unsigned int)elem.flags;
    inst.firstSeenTick = now;
    inst.lastSeenTick = now;
    inst.seenFrames = 1;
    inst.hasNativeEvidence = (elem.alpha > 0.01f || elem.fxBirthTime > 0);
    inst.objectiveLaneEvidence = elem.objectiveLane;
    std::string runtimeEnglish;
    if (TextHook_GetRuntimeEnglishForKeyRecent(elem.key, runtimeEnglish, 320)) {
      runtimeEnglish = TrimSpaces(StripColorCodes(runtimeEnglish));
      inst.rawText = runtimeEnglish;
    }
    inst.hasEnglishEvidence = !inst.rawText.empty();

    auto it = frameById.find(inst.instanceId);
    const bool shouldReplace =
        (it == frameById.end()) ||
        (inst.track == NATIVE_HUD_TRACK_OBJECTIVE &&
         it->second.track != NATIVE_HUD_TRACK_OBJECTIVE) ||
        (inst.lastSeenTick >= it->second.lastSeenTick);
    if (shouldReplace) {
      frameById[inst.instanceId] = inst;
    }

    NativeHudEvent ev{};
    ev.sourceToken = inst.sourceToken;
    ev.track = inst.track;
    ev.tick = inst.lastSeenTick;
    ev.instanceId = inst.instanceId;
    ev.hudElemPtr = 0;
    ev.ownerdrawId = 0;
    ev.cfgIndex = inst.cfgIndex;
    ev.arrayIndex = inst.arrayIndex;
    ev.x = inst.x;
    ev.y = inst.y;
    ev.virtualW = inst.virtualW;
    ev.virtualH = inst.virtualH;
    ev.xScale = inst.xScale;
    ev.yScale = inst.yScale;
    ev.fontHeight = inst.fontHeight;
    ev.color[0] = inst.color[0];
    ev.color[1] = inst.color[1];
    ev.color[2] = inst.color[2];
    ev.color[3] = inst.color[3];
    ev.fromColor[0] = inst.fromColor[0];
    ev.fromColor[1] = inst.fromColor[1];
    ev.fromColor[2] = inst.fromColor[2];
    ev.fromColor[3] = inst.fromColor[3];
    ev.fadeStartTime = inst.fadeStartTime;
    ev.fadeTime = inst.fadeTime;
    ev.fxBirthTime = inst.fxBirthTime;
    ev.fxLetterTime = inst.fxLetterTime;
    ev.serverTime = inst.serverTime;
    ev.style = inst.style;
    ev.flags = inst.flags;
    ev.callerOffset = inst.callerOffset;
    ev.keyToken = inst.keyToken;
    ev.rawText = inst.rawText;
    ev.hasNativeEvidence = inst.hasNativeEvidence;
    ev.hasEnglishEvidence = inst.hasEnglishEvidence;
    out.events.push_back(std::move(ev));
  }
#else
  out.lastHudElemTick = 0;
#endif

  {
    std::lock_guard<std::mutex> lock(g_NativeHudSnapshotMutex);
    std::unordered_set<uint64_t> activeIds;
    activeIds.reserve(frameById.size() * 2 + 8);
    for (auto &kv : frameById) {
      const uint64_t iid = kv.first;
      NativeHudInstance &inst = kv.second;
      activeIds.insert(iid);

      auto itCached = g_NativeHudInstanceCache.find(iid);
      if (itCached == g_NativeHudInstanceCache.end()) {
        inst.firstSeenTick = inst.lastSeenTick;
        inst.seenFrames = 1;
      } else {
        const DWORD age =
            (inst.lastSeenTick >= itCached->second.lastSeenTick)
                ? (inst.lastSeenTick - itCached->second.lastSeenTick)
                : 0;
        if (age > 1200 || itCached->second.track != inst.track) {
          inst.firstSeenTick = inst.lastSeenTick;
          inst.seenFrames = 1;
        } else {
          inst.firstSeenTick = itCached->second.firstSeenTick;
          inst.seenFrames = itCached->second.seenFrames + 1;
        }
      }
      g_NativeHudInstanceCache[iid] = inst;
    }

    for (auto it = g_NativeHudInstanceCache.begin();
         it != g_NativeHudInstanceCache.end();) {
      const bool active = (activeIds.find(it->first) != activeIds.end());
      const DWORD age = (now >= it->second.lastSeenTick)
                            ? (now - it->second.lastSeenTick)
                            : 0;
      if (!active && age > 1800) {
        it = g_NativeHudInstanceCache.erase(it);
      } else {
        ++it;
      }
    }
    g_NativeHudLastSnapshotTick = now;
  }

  out.instances.reserve(frameById.size());
  for (auto &kv : frameById) {
    NativeHudInstance inst = kv.second;
    if (!inst.hasNativeEvidence && !inst.hasEnglishEvidence) {
      continue;
    }
    out.instances.push_back(std::move(inst));
  }
  std::sort(out.instances.begin(), out.instances.end(),
            [](const NativeHudInstance &a, const NativeHudInstance &b) {
              if (a.lastSeenTick != b.lastSeenTick) {
                return a.lastSeenTick > b.lastSeenTick;
              }
              return a.instanceId < b.instanceId;
            });

  if (kVerboseRuntimeLogs) {
    static DWORD s_lastSnapshotLog = 0;
    if ((now - s_lastSnapshotLog) > 1200) {
      s_lastSnapshotLog = now;
      int hintCount = 0;
      int objHintCount = 0;
      int objectiveCount = 0;
      for (const auto &inst : out.instances) {
        if (inst.track == NATIVE_HUD_TRACK_HINT) {
          ++hintCount;
        } else if (inst.track == NATIVE_HUD_TRACK_OBJHINT) {
          ++objHintCount;
        } else if (inst.track == NATIVE_HUD_TRACK_OBJECTIVE) {
          ++objectiveCount;
        }
      }
      char buf[256];
      sprintf_s(buf,
                "[NHUD-SNAP] instances=%d hint=%d objhint=%d objective=%d events=%d",
                (int)out.instances.size(), hintCount, objHintCount, objectiveCount,
                (int)out.events.size());
      LogToFile(buf);
      if (!out.events.empty()) {
        const NativeHudEvent &ev = out.events.front();
        char ebuf[256];
        sprintf_s(ebuf,
                  "[NHUD-EVT] src=%u track=%d key=%s x=%.1f y=%.1f a=%.2f",
                  (unsigned int)ev.sourceToken, (int)ev.track, ev.keyToken.c_str(),
                  ev.x, ev.y, ev.color[3]);
        LogToFile(ebuf);
      }
      const DWORD perfMs = GetTickCount() - perfStart;
      char pbuf[160];
      sprintf_s(pbuf, "[NHUD-PERF] snapshot_ms=%lu entries=%d",
                (unsigned long)perfMs, (int)liveNativeEntries.size());
      LogToFile(pbuf);
    }
  }

  return true;
}

bool TextHook_GetHudRouteForNativeEntry(const HudNativeEntry &entry,
                                        HudRouteChannel &outChannel) {
  return ResolveHudFamilyRouteFromNative(entry, outChannel);
}
bool TextHook_GetHudRuntimeTail(const std::string &key, float hintX, float hintY,
                                unsigned int callerOffset, std::string &outTail) {
  outTail.clear();
  if (key.empty() || !std::isfinite(hintX) || !std::isfinite(hintY)) {
    return false;
  }

  const DWORD now = GetTickCount();

  // ── Per-frame cache: avoid redundant mutex+deque work when multiple
  //    rendering paths (DedicatedHint, DedicatedObjHudHint,
  //    TryBuildDirectAddCmdObjHudHintText) query the same key in one frame.
  //    At 60fps one frame ≈ 16ms; a 4ms window catches all same-frame callers.
  static std::string s_tailCacheKey;
  static std::string s_tailCacheTail;
  static DWORD       s_tailCacheTick = 0;
  static bool        s_tailCacheValid = false;

  if (s_tailCacheValid && s_tailCacheKey == key &&
      s_tailCacheTick != 0 && now >= s_tailCacheTick &&
      (now - s_tailCacheTick) <= 4) {
    outTail = s_tailCacheTail;
    return !outTail.empty();
  }

  const bool relaxedTailJoinKey = IsHudRuntimeTailJoinPromptKey(key);
  const float maxTailDy = relaxedTailJoinKey ? 28.0f : 8.0f;

  // ── Copy fragments under short lock, then score without holding mutex.
  //    This eliminates cross-thread contention between the capture side
  //    (game/client thread) and the render side (D3D11 Present thread).
  static thread_local std::vector<HudRuntimeTailFragment> t_localFrags;
  t_localFrags.clear();
  {
    std::lock_guard<std::mutex> lock(g_HudRuntimeTailMutex);
    PruneHudRuntimeTailLocked(now);
    t_localFrags.assign(g_HudRuntimeTailFragments.begin(),
                         g_HudRuntimeTailFragments.end());
  }
  // Mutex released — scoring runs lock-free.

  const HudRuntimeTailFragment *best = nullptr;
  float bestScore = -1.0e30f;
  float bestDx = 0.0f;
  float bestDy = 0.0f;
  DWORD bestAge = 0;

  for (auto it = t_localFrags.rbegin(); it != t_localFrags.rend(); ++it) {
    const HudRuntimeTailFragment &frag = *it;
    const bool callerMatch =
        (callerOffset != 0 && frag.callerOffset == callerOffset);
    const bool keyMatch = (!frag.key.empty() && frag.key == key);
    if (!callerMatch && !keyMatch) {
      continue;
    }
    const DWORD age = now - frag.tick;
    if (age > HUD_RUNTIME_TAIL_TTL_MS) {
      continue;
    }
    const float dy = std::fabs(frag.y - hintY);
    if (dy > maxTailDy) {
      continue;
    }
    const float dx = frag.x - hintX;
    const bool xPreferred = (frag.x >= (hintX - 8.0f));
    float score = 0.0f;
    if (xPreferred) {
      score += 500.0f;
    }
    if (callerMatch) {
      score += 260.0f;
    } else {
      score -= 120.0f;
    }
    score += (float)(HUD_RUNTIME_TAIL_TTL_MS - age) * 0.6f;
    score -= std::fabs(dx) * 0.9f;
    score -= dy * 12.0f;
    if (keyMatch) {
      score += 180.0f;
    }
    if (frag.text.size() >= 2 && frag.text.size() <= 24) {
      score += 8.0f;
    }

    if (!best || score > bestScore) {
      best = &frag;
      bestScore = score;
      bestDx = dx;
      bestDy = dy;
      bestAge = age;
    }
  }

  if (!best || best->text.empty()) {
    // Update cache with negative result
    s_tailCacheKey = key;
    s_tailCacheTail.clear();
    s_tailCacheTick = now;
    s_tailCacheValid = true;
    return false;
  }

  outTail = best->text;

  // Update per-frame cache
  s_tailCacheKey = key;
  s_tailCacheTail = outTail;
  s_tailCacheTick = now;
  s_tailCacheValid = true;

  // Throttled logging — use callerOffset as cheap key to avoid per-frame
  // string allocation for the signature.
  static std::mutex s_tailMatchLogMtx;
  static std::unordered_map<unsigned int, DWORD> s_tailMatchLogTick;
  {
    std::lock_guard<std::mutex> lk(s_tailMatchLogMtx);
    auto it = s_tailMatchLogTick.find(callerOffset);
    if (it == s_tailMatchLogTick.end() || (now - it->second) > 2500) {
      s_tailMatchLogTick[callerOffset] = now;
      char buf[640];
      sprintf_s(buf,
                "[HUD-TAIL-MATCH] key=%s tail=\"%.80s\" caller=+0x%X dx=%.1f dy=%.1f age=%lums",
                key.c_str(), outTail.c_str(), callerOffset, bestDx, bestDy,
                (unsigned long)bestAge);
      LogToFile(buf);
    }
    if (s_tailMatchLogTick.size() > 64) {
      for (auto jt = s_tailMatchLogTick.begin();
           jt != s_tailMatchLogTick.end();) {
        if ((now - jt->second) > 30000) {
          jt = s_tailMatchLogTick.erase(jt);
        } else {
          ++jt;
        }
      }
    }
  }

  return true;
}
void TextHook_UpdateHudNativeEntry(const HudNativeEntry &entry) {
  if (entry.key.empty())
    return;

  HudNativeEntry normalized = entry;
  bool objectiveLike =
      (IsObjectiveOverlayKeyName(normalized.key) &&
       !IsObjectiveStatusMessageKeyName(normalized.key));
  if (normalized.source == HUD_NATIVE_RENDER) {
    objectiveLike = false;
    normalized.objectiveChannel = (unsigned char)OBJ_CHANNEL_NONE;
  }
  normalized.objectiveLike = objectiveLike;
  if (!objectiveLike && normalized.source == HUD_NATIVE_SCANNER) {
    // HUD hint/object-hint pipeline must not consume SLC-scanner samples.
    return;
  }
  if (objectiveLike && normalized.objectiveChannel == OBJ_CHANNEL_NONE) {
    normalized.objectiveChannel =
        (unsigned char)DetermineObjectiveChannelForKey(
            normalized.key, TextHook_IsPauseMenuLikely());
  }
  if (normalized.lastUpdate == 0) {
    normalized.lastUpdate = GetTickCount();
  }
  const DWORD nowWall = GetTickCount();
  const DWORD sampleAgeMs =
      (nowWall >= normalized.lastUpdate) ? (nowWall - normalized.lastUpdate) : 0;
  const int slotBucket = TextHook_HudSlotFromEvent(
      normalized.y, normalized.virtualH, 0);
  normalized.slotBucket = slotBucket;
  const OverlaySource normalizedOverlaySource =
      (normalized.sourceToken != 0)
          ? OverlaySourceFromNativeSourceToken(normalized.sourceToken)
          : HudOverlaySourceFromNative(normalized.source);
  normalized.consumerId = TextHook_MakeHudConsumerId(
      normalizedOverlaySource, normalized.callerOffset, slotBucket,
      normalized.virtualW, normalized.virtualH);
  const uint64_t nativeMapKey =
      MakeHudNativeEntryMapKey(normalized.key, normalized.consumerId);

  if (objectiveLike) {
    static std::mutex s_objNativeUpdLogMtx;
    static std::unordered_map<std::string, DWORD> s_objNativeUpdLog;
    DWORD nowObjUpd = nowWall;
    std::string sig = normalized.key + "|" +
                      std::to_string((unsigned int)normalized.callerOffset) + "|" +
                      std::to_string((int)normalized.source) + "|" +
                      std::to_string((int)normalized.objectiveChannel);
    bool shouldLog = false;
    {
      std::lock_guard<std::mutex> lk(s_objNativeUpdLogMtx);
      auto it = s_objNativeUpdLog.find(sig);
      if (it == s_objNativeUpdLog.end() || (nowObjUpd - it->second) > 700) {
        s_objNativeUpdLog[sig] = nowObjUpd;
        shouldLog = true;
      }
      if (s_objNativeUpdLog.size() > 320) {
        for (auto it = s_objNativeUpdLog.begin(); it != s_objNativeUpdLog.end();) {
          if ((nowObjUpd - it->second) > 30000) {
            it = s_objNativeUpdLog.erase(it);
          } else {
            ++it;
          }
        }
      }
    }
    if (shouldLog) {
      char obuf[560];
      sprintf_s(
          obuf,
          "[OBJ-NATIVE-UPD] key=%s ch=%d caller=+0x%X src=%d valid=%d x=%.2f y=%.2f "
          "sx=%.3f sy=%.3f a=%.2f vh=%u vw=%u fh=%.1f style=%d slot=%d cid=0x%llX age=%lums",
          normalized.key.c_str(), (int)normalized.objectiveChannel,
          normalized.callerOffset, (int)normalized.source,
          normalized.valid ? 1 : 0, normalized.x, normalized.y,
          normalized.xScale, normalized.yScale, normalized.color[3],
          (unsigned int)normalized.virtualH, (unsigned int)normalized.virtualW,
          normalized.fontHeight, normalized.style, normalized.slotBucket,
          (unsigned long long)normalized.consumerId, (unsigned long)sampleAgeMs);
      LogToFile(obuf);
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_HudNativeMutex);
    auto itExisting = g_HudNativeMap.find(nativeMapKey);
    if (itExisting != g_HudNativeMap.end()) {
      const HudNativeEntry &old = itExisting->second;
      const bool oldHasVirtual = (old.virtualW != 0 || old.virtualH != 0);
      const bool newHasVirtual =
          (normalized.virtualW != 0 || normalized.virtualH != 0);
      DWORD age =
          (normalized.lastUpdate >= old.lastUpdate)
              ? (normalized.lastUpdate - old.lastUpdate)
              : 0;

      bool keepOld = false;
      const bool preferAddCmdLayout =
          IsHudRuntimeTailJoinPromptKey(normalized.key) ||
          (normalized.key.rfind("CLOCKWORK_HINT_DRILL", 0) == 0);
      const bool newFromAddCmd = (normalized.source == HUD_NATIVE_ADDCMD);
      const bool oldFromAddCmd = (old.source == HUD_NATIVE_ADDCMD);
      if (!objectiveLike && age <= 450) {
        if (preferAddCmdLayout) {
          if (oldFromAddCmd && !newFromAddCmd) {
            keepOld = true;
          }
        } else if (oldHasVirtual && !newHasVirtual && newFromAddCmd) {
          keepOld = true;
        }
      }

      if (keepOld) {
        itExisting->second.lastUpdate = normalized.lastUpdate;
        if (old.key != normalized.key) {
          char dbuf[384];
          sprintf_s(dbuf,
                    "[HUD-DUP-BLOCK] consumer=0x%llX key=%s kept=%s dropped=%s",
                    (unsigned long long)normalized.consumerId,
                    normalized.key.c_str(), old.key.c_str(),
                    normalized.key.c_str());
          LogToFile(dbuf);
        }
      } else {
        if (old.key != normalized.key && age <= 900) {
          char dbuf[384];
          sprintf_s(dbuf,
                    "[HUD-DUP-BLOCK] consumer=0x%llX key=%s kept=%s dropped=%s",
                    (unsigned long long)normalized.consumerId,
                    normalized.key.c_str(), normalized.key.c_str(),
                    old.key.c_str());
          LogToFile(dbuf);
        }
        itExisting->second = normalized;
      }
    } else {
      g_HudNativeMap.emplace(nativeMapKey, normalized);
    }
  }

  if (objectiveLike) {
    int updatedCount = 0;
    int skippedConsumerOwned = 0;
    ObjectiveRuntime::WithOverlayState(
        [&](ObjectiveRuntime::OverlayState &overlayState) {
          auto &g_ObjectiveOverlay = overlayState.entries;
          for (auto &e : g_ObjectiveOverlay) {
            if (e.key != normalized.key) {
              continue;
            }
            if (e.consumerOwned) {
              skippedConsumerOwned++;
              continue;
            }
            if (normalized.objectiveChannel != OBJ_CHANNEL_NONE) {
              e.channel = (ObjectiveChannel)normalized.objectiveChannel;
            }
            e.nativeValid = normalized.valid;
            e.nativeX = normalized.x;
            e.nativeY = normalized.y;
            e.nativeXScale = normalized.xScale;
            e.nativeYScale = normalized.yScale;
            e.nativeVirtualW = normalized.virtualW;
            e.nativeVirtualH = normalized.virtualH;
            e.callerOffset = normalized.callerOffset;
            e.nativeSource = normalized.source;
            if (normalized.fontHeight > 0.0f) {
              e.fontHeight = normalized.fontHeight;
            }
            e.style = normalized.style;
            e.color[0] = normalized.color[0];
            e.color[1] = normalized.color[1];
            e.color[2] = normalized.color[2];
            e.color[3] = normalized.color[3];
            e.lastSeen = normalized.lastUpdate;
            e.ownerdrawId = 0;
            updatedCount++;
          }
        });

    static std::mutex s_objNativeMergeLogMtx;
    static std::unordered_map<std::string, DWORD> s_objNativeMergeLog;
    DWORD nowMerge = normalized.lastUpdate;
    std::string mergeSig = normalized.key + "|" +
                           std::to_string((unsigned int)normalized.callerOffset);
    bool shouldMergeLog = false;
    {
      std::lock_guard<std::mutex> lk(s_objNativeMergeLogMtx);
      auto it = s_objNativeMergeLog.find(mergeSig);
      if (it == s_objNativeMergeLog.end() || (nowMerge - it->second) > 900) {
        s_objNativeMergeLog[mergeSig] = nowMerge;
        shouldMergeLog = true;
      }
      if (s_objNativeMergeLog.size() > 320) {
        for (auto it = s_objNativeMergeLog.begin(); it != s_objNativeMergeLog.end();) {
          if ((nowMerge - it->second) > 30000) {
            it = s_objNativeMergeLog.erase(it);
          } else {
            ++it;
          }
        }
      }
    }
    if (shouldMergeLog) {
      char mbuf[320];
      sprintf_s(mbuf,
                "[OBJ-NATIVE-MERGE] key=%s caller=+0x%X upd=%d skipConsumer=%d x=%.1f y=%.1f age=%lums",
                normalized.key.c_str(), normalized.callerOffset, updatedCount,
                skippedConsumerOwned, normalized.x, normalized.y,
                (unsigned long)sampleAgeMs);
      LogToFile(mbuf);
    }
  }
}

// Called by FSHook when gameplay HUD LOCALIZE key is loaded.
// Returns true if Korean translation found and queued, false otherwise.
bool TextHook_OnHudKey(const char *key) {
  if (!key)
    return false;

  EnsureTranslationsLoaded();

  g_LastHudActivityTime.store(GetTickCount());

  std::string keyStr(key);
  const HudKeyType keyType = ClassifyHudKey(keyStr);

  // Intro keys are rendered by the intro overlay system, never HUD hints.
  if (keyType == HUD_KEY_INTRO) {
    return false;
  }

  // Find Korean translation
  auto itKor = g_KeyToKorean.find(keyStr);
  if (itKor == g_KeyToKorean.end()) {
    return false;
  }

  std::string korean = itKor->second;
  auto itEng = g_KeyToEnglish.find(keyStr);
  std::string english = (itEng != g_KeyToEnglish.end()) ? itEng->second : keyStr;
  DWORD now = GetTickCount();
  // Objective/save keys: translation mapping only.
  // Lifecycle/timing ownership is handled by objective runtime/consumer paths.
  const bool isObjectiveKey = IsObjectivePipelineKey(keyStr);
  if (isObjectiveKey) {
    const bool pauseLikely = TextHook_IsPauseMenuLikely();
    ObjectiveChannel channel =
        DetermineObjectiveChannelForKey(keyStr, pauseLikely);
    ObjectiveRuntime::WithOverlayState(
        [&](ObjectiveRuntime::OverlayState &overlayState) {
          auto &g_ObjectiveOverlay = overlayState.entries;
          auto &g_ObjectiveKeyObservations = overlayState.keyObservations;
          bool allowObservation = true;
          if (keyStr == "CGAME_MISSIONOBJECTIVES") {
            bool gameplayTrafficRecent = false;
            for (const auto &kv : g_ObjectiveKeyObservations) {
              const ObjectiveKeyObservation &obs2 = kv.second;
              if (obs2.key.empty() || obs2.key == "CGAME_MISSIONOBJECTIVES") {
                continue;
              }
              ObjectiveChannel ch2 =
                  (obs2.channel != OBJ_CHANNEL_NONE)
                      ? obs2.channel
                      : DetermineObjectiveChannelForKey(obs2.key, false);
              if ((ch2 == OBJ_CHANNEL_GAMEPLAY_UPDATE ||
                   ch2 == OBJ_CHANNEL_GAMEPLAY_LIST) &&
                  (now - obs2.lastSeen) <= 700) {
                gameplayTrafficRecent = true;
                break;
              }
            }
            // CGAME_MISSIONOBJECTIVES is frequently looked up in gameplay.
            // Only treat it as an objective-consumer signal during true pause.
            if (!pauseLikely || gameplayTrafficRecent) {
              allowObservation = false;
            }
          }

          if (allowObservation) {
            ObjectiveKeyObservation &obs = g_ObjectiveKeyObservations[keyStr];
            obs.key = keyStr;
            obs.channel = channel;
            obs.lastSeen = now;
            if (g_ObjectiveKeyObservations.size() > 256) {
              for (auto it = g_ObjectiveKeyObservations.begin();
                   it != g_ObjectiveKeyObservations.end();) {
                if ((now - it->second.lastSeen) > 12000) {
                  it = g_ObjectiveKeyObservations.erase(it);
                } else {
                  ++it;
                }
              }
            }
          } else if (keyStr == "CGAME_MISSIONOBJECTIVES") {
            g_ObjectiveKeyObservations.erase(keyStr);
          }
          // FSHook gap detection REMOVED ??caused 46+ false restarts per session.
          // Keys like CGAME_MISSIONOBJECTIVES, GAME_OBJECTIVECOMPLETED, EXE_GAMESAVED
          // naturally re-appear with 2-90s gaps during normal gameplay.
          // Death/restart detection deferred to a future fundamental approach.

          // === Update existing entries OR create new one ===
          // lastSeen IS refreshed here ??entries stay alive in TextHook as long
          // as FSHook fires. Display lifecycle (5s+fade) is controlled entirely
          // by D3D11Hook's renderAge + completedKeys. No creation cooldown needed.
          bool foundExisting = false;
          for (auto &existing : g_ObjectiveOverlay) {
            if (existing.key != keyStr) {
              continue;
            }
            existing.korean = korean;
            existing.english = english;
            if (channel != OBJ_CHANNEL_NONE) {
              existing.channel = channel;
            }
            existing.lastSeen = now;
            existing.lastFSHookSeen = now;
            existing.frameSeen += 1;
            foundExisting = true;
          }
          if (!foundExisting && channel != OBJ_CHANNEL_NONE) {
            ObjectiveOverlayEntry entry{};
            entry.key = keyStr;
            entry.korean = korean;
            entry.english = english;
            entry.channel = channel;
            DetermineHudColor(keyStr, entry.color);
            entry.scale = DetermineHudScale(keyStr);
            entry.fontHeight = 78.0f;
            entry.style = 0;
            entry.nativeX = 0.0f;
            entry.nativeY = 0.0f;
            entry.nativeXScale = entry.scale;
            entry.nativeYScale = entry.scale;
            entry.nativeVirtualW = 0;
            entry.nativeVirtualH = 0;
            entry.callerOffset = 0;
            entry.nativeSource = HUD_NATIVE_RENDER;
            entry.nativeValid = false;
            entry.consumerOwned = false;
            entry.ownerdrawId = 0;
            entry.visibleChars = -1;
            entry.totalChars = -1;
            entry.frameSeen = 1;
            entry.nativeClockStart = 0;
            entry.nativeClockEnd = 0;
            entry.runtimeInstanceId = NextObjectiveRuntimeInstanceId();
            entry.generation = NextObjectiveGeneration();
            entry.firstSeen = now;
            entry.lastSeen = now;
            entry.lastFSHookSeen = now;
            entry.lastForceSeen = 0;
            if (g_ObjectiveOverlay.size() >= OBJECTIVE_ENTRY_MAX) {
              auto oldest = std::min_element(
                  g_ObjectiveOverlay.begin(), g_ObjectiveOverlay.end(),
                  [](const ObjectiveOverlayEntry &a,
                     const ObjectiveOverlayEntry &b) {
                    return a.lastSeen < b.lastSeen;
                  });
              if (oldest != g_ObjectiveOverlay.end()) {
                g_ObjectiveOverlay.erase(oldest);
              }
            }
            g_ObjectiveOverlay.push_back(entry);
          }
        });
    return true;
  }

  // ?? Phase 3: Route hint keys to dedicated g_HintEntries ??????????????????
  if (keyType == HUD_KEY_HINT || keyType == HUD_KEY_OBJ_HUD_HINT) {
    return true;
  }

  // Legacy key-only hint/objhudhint intake has been removed.
  // Runtime HUD should now enter through objective/hint/objhudhint native
  // containers only.
  if (keyType == HUD_KEY_UNKNOWN) {
    return true;
  }
  return true;
}



