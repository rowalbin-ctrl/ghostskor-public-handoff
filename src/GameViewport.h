#pragma once

// Computes the 16:9 active rendering area within the backbuffer.
// Call of Duty: Ghosts always renders at 16:9 internally and adds
// letterboxing (top/bottom black bars) for narrower aspect ratios
// like 4:3 or 16:10, or pillarboxing for ultrawide displays.
//
// For 16:9 backbuffers the output matches the input exactly (offset = 0).

struct GameActiveArea {
  float offsetX; // left offset of 16:9 area within backbuffer
  float offsetY; // top offset of 16:9 area within backbuffer
  float width;   // width of the 16:9 active area
  float height;  // height of the 16:9 active area
};

inline GameActiveArea Compute16x9ActiveArea(float bufferW, float bufferH) {
  GameActiveArea area;
  if (bufferW <= 0.0f || bufferH <= 0.0f) {
    area.offsetX = 0.0f;
    area.offsetY = 0.0f;
    area.width = bufferW;
    area.height = bufferH;
    return area;
  }

  constexpr float kTargetAspect = 16.0f / 9.0f;
  const float currentAspect = bufferW / bufferH;

  if (currentAspect > kTargetAspect + 0.01f) {
    // Wider than 16:9 (e.g. ultrawide 21:9) — pillarbox (bars on sides)
    area.height = bufferH;
    area.width = bufferH * kTargetAspect;
    area.offsetX = (bufferW - area.width) * 0.5f;
    area.offsetY = 0.0f;
  } else if (currentAspect < kTargetAspect - 0.01f) {
    // Taller than 16:9 (e.g. 4:3, 16:10) — letterbox (bars top/bottom)
    area.width = bufferW;
    area.height = bufferW / kTargetAspect;
    area.offsetX = 0.0f;
    area.offsetY = (bufferH - area.height) * 0.5f;
  } else {
    // Already 16:9 — no offset
    area.width = bufferW;
    area.height = bufferH;
    area.offsetX = 0.0f;
    area.offsetY = 0.0f;
  }

  return area;
}

// Global active area state — updated once per frame before rendering.
// Used by KoreanRenderer for menu metrics and by .inl overlay code
// for scaling and position calculations.
// Defined in DXGIWrapper.cpp, declared here for cross-file access.
extern GameActiveArea g_ActiveArea;
