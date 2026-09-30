#pragma once
#include "GameViewport.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

// Subtitle-only geometry. Legacy UI metrics must not select the dialogue or
// movie viewport: ScreenPlacement and the cinematic renderer use different areas.
namespace SubtitleViewport {
struct NativeInputs {
  GameActiveArea viewport{};
  uint32_t canvasWidth = 0, canvasHeight = 0;
  float pixelAspect = 0, movieAspect = 0;
};
struct Areas {
  GameActiveArea dialogue{}, video{};
  bool nativeDialogue = false, nativeVideo = false;
};
inline bool ValidSize(float w, float h) {
  return std::isfinite(w) && std::isfinite(h) && w >= 1 && h >= 1;
}
inline bool Fits(const GameActiveArea &a, float w, float h) {
  return ValidSize(a.width, a.height) && std::isfinite(a.offsetX) &&
      std::isfinite(a.offsetY) && a.offsetX >= 0 && a.offsetY >= 0 &&
      a.offsetX + a.width <= w + 0.5f && a.offsetY + a.height <= h + 0.5f;
}
inline Areas Select(float bufferW, float bufferH, const NativeInputs *native) {
  Areas result;
  if (!ValidSize(bufferW, bufferH)) return result;
  result.dialogue = result.video = {0, 0, bufferW, bufferH};
  // Never retain an old resolution's placement during ResizeBuffers. The native
  // renderer's canvas and this swap chain must refer to the same pixel surface.
  if (!native || std::fabs(float(native->canvasWidth) - bufferW) > 0.5f ||
      std::fabs(float(native->canvasHeight) - bufferH) > 0.5f) return result;
  if (Fits(native->viewport, bufferW, bufferH)) {
    result.dialogue = native->viewport;
    result.nativeDialogue = true;
  }
  if (std::isfinite(native->pixelAspect) && native->pixelAspect > 0 &&
      std::isfinite(native->movieAspect) && native->movieAspect > 0) {
    // Exact rectangle passed to DrawStretchPic at cinematic draw +0x168:
    // min(canvasHeight, canvasWidth * pixelAspect / movieAspect). In particular,
    // an ultrawide surface does NOT imply pillarboxing in the stock renderer.
    const float movieHeight = (std::min)(bufferH,
        bufferW * native->pixelAspect / native->movieAspect);
    if (ValidSize(bufferW, movieHeight)) {
      result.video = {0, (bufferH - movieHeight) * 0.5f, bufferW, movieHeight};
      result.nativeVideo = true;
    }
  }
  return result;
}

inline float WrapWidth(float w, float h, bool video) {
  if (!ValidSize(w, h)) return 0;
  const float aspect = w / h;
  float ratio = video ? 0.56f : 0.62f;
  if (aspect >= 2.30f) ratio -= 0.09f;
  else if (aspect >= 2.00f) ratio -= 0.07f;
  else if (aspect >= 1.85f) ratio -= 0.04f;
  else if (aspect <= 1.45f) ratio += 0.05f;
  ratio = std::clamp(ratio, video ? 0.42f : 0.48f, video ? 0.68f : 0.74f);
  const float margin = (std::min)(w * 0.10f, (std::max)(24.0f, w * 0.045f));
  const float safeWidth = w - margin * 2;
  const float preferredMin = (std::max)(220.0f, w * (video ? 0.38f : 0.44f));
  return (std::min)(safeWidth, (std::max)(w * ratio, preferredMin));
}
}
