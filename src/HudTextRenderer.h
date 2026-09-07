#pragma once
#include "KoreanRenderer.h"
#include <cmath>

// HUD layout supplies pixel font heights. Normalize to the existing 1080p
// sizing rules before their clamps, then apply the output resolution once.
// Positions, measured widths and gaps remain in pixels throughout layout.
class HudTextRenderer {
public:
  explicit HudTextRenderer(float outputHeight)
      : resolutionScale_(std::isfinite(outputHeight) && outputHeight > 1.0f
                             ? outputHeight / 1080.0f : 1.0f) {}

  float CalculateFinalScale(float pixelFontHeight, float scale) const {
    return KoreanRenderer::CalculateFinalScale(
        pixelFontHeight / resolutionScale_, scale, resolutionScale_);
  }

  float MeasureTextWidthEx(const std::string &text, float pixelFontHeight,
                          float scale, uint8_t atlasSlot = 0) const {
    return KoreanRenderer::MeasureTextWidthEx(
        text, pixelFontHeight / resolutionScale_, scale, atlasSlot,
        resolutionScale_);
  }

  void QueueText(const std::string &text, float x, float y, float scale,
                 const float *color, float pixelFontHeight, int style,
                 float finalWidthEng, bool disableShadow, bool isSmallMenu,
                 float originalXScale, bool allowZeroAlpha = false,
                 bool code7ResetsToBase = false, bool disableAutoWrap = false,
                 int forceAlign = -1, float maxWidthPx = 0.0f,
                 bool skipMenuClipping = false, bool yIsTopOfText = false,
                 uint8_t atlasSlot = 0,
                 const DrawStylePatch *stylePatch = nullptr) const {
    KoreanRenderer::QueueText(
        text, x, y, scale, color, pixelFontHeight / resolutionScale_, style,
        finalWidthEng, disableShadow, isSmallMenu, originalXScale,
        allowZeroAlpha, code7ResetsToBase, disableAutoWrap, forceAlign,
        maxWidthPx, skipMenuClipping, yIsTopOfText, atlasSlot, stylePatch,
        resolutionScale_);
  }

private:
  float resolutionScale_;
};
