
#include "HudTextRenderer.h"
#include <cassert>
#include <limits>
#include <iostream>
static bool s_QteScaleMode = false;
constexpr float GLOBAL_FONT_SCALE = 0.45f;
static float queuedScale, queuedX, queuedY, queuedWidth;
float KoreanRenderer::CalculateFinalScale(float fontHeight, float cmdScale,
                                          float resolutionScale) {
  float safeHeight = fontHeight;
  if (s_QteScaleMode) {
    // QTE overlay: relaxed clamps so the dramatic shrink animation
    // (sizeRatio >> 1) doesn't get flattened.
    if (safeHeight > 600.0f || safeHeight < 5.0f) {
      if (safeHeight <= 1.0f)
        safeHeight = 48.0f;
      else if (safeHeight > 600.0f)
        safeHeight = 500.0f;
      else if (safeHeight < 5.0f)
        safeHeight = 20.0f;
    }
  } else {
    if (safeHeight > 150.0f || safeHeight < 5.0f) {
      if (safeHeight <= 1.0f)
        safeHeight = 48.0f;
      else if (safeHeight > 150.0f)
        safeHeight = 100.0f;
      else if (safeHeight < 5.0f)
        safeHeight = 20.0f;
    }
  }
  float baseScale = (safeHeight / 60.0f);
  float finalScale = baseScale * cmdScale * GLOBAL_FONT_SCALE;
  const float cap = s_QteScaleMode ? 5.0f : 2.2f;
  if (finalScale > cap)
    finalScale = cap;
  // Validate logical font values first; then convert to output pixels.
  // Otherwise 78 * 2 = 156 at 4K hits the >150 fallback above and becomes 100.
  const float pixelScale =
      (std::isfinite(resolutionScale) && resolutionScale > 0.0f)
          ? resolutionScale : 1.0f;
  return finalScale * pixelScale;
}
// Capture only the adapter boundary; actual glyph rasterization needs the game.
float KoreanRenderer::MeasureTextWidthEx(const std::string &, float h, float s,
                                        uint8_t, float r) {
  return CalculateFinalScale(h, s, r) * 60.0f;
}
void KoreanRenderer::QueueText(const std::string &, float x, float y, float s,
    const float *, float h, int, float width, bool, bool, float, bool, bool,
    bool, int, float, bool, bool, uint8_t, const DrawStylePatch *, float r) {
  queuedScale = CalculateFinalScale(h, s, r);
  queuedX = x; queuedY = y; queuedWidth = width;
}
static DWORD testTick = 5000;
static DWORD TestTickCount() { return testTick; }
struct TestSnapshot {
  bool active = true, timerComputed = true;
  std::string renderText = "label";
  DWORD lastSeenTick = 5000;
  float timerSeconds = 60, x = 10, y = 20, scale = 1, fontHeight = 72;
  float color[4] = {1,1,1,1};
};
struct TestRuntime {
  std::mutex mutex;
  TestSnapshot renderSnapshot;
  DWORD directOwnerTick = 5000, iarFallbackOwnerTick = 0;
} g_TimeScriptRuntimeState;
constexpr DWORD kTimeScriptSnapshotTtlMs = 500;
#define GetTickCount TestTickCount
bool TextHook_HasTimeScriptTimerReplacement() {
  // Called for native HUD draws: avoid allocating/copying the text snapshot.
  std::lock_guard<std::mutex> lock(g_TimeScriptRuntimeState.mutex);
  const auto &snap = g_TimeScriptRuntimeState.renderSnapshot;
  const DWORD now = GetTickCount();
  const bool ownerLive =
      (g_TimeScriptRuntimeState.directOwnerTick != 0 &&
       now - g_TimeScriptRuntimeState.directOwnerTick <= kTimeScriptSnapshotTtlMs) ||
      (g_TimeScriptRuntimeState.iarFallbackOwnerTick != 0 &&
       now - g_TimeScriptRuntimeState.iarFallbackOwnerTick <= kTimeScriptSnapshotTtlMs);
  return ownerLive && snap.active && !snap.renderText.empty() &&
         snap.lastSeenTick != 0 && now - snap.lastSeenTick <= kTimeScriptSnapshotTtlMs &&
         snap.timerComputed && std::isfinite(snap.timerSeconds) &&
         snap.timerSeconds > 0.05f && std::isfinite(snap.x) &&
         std::isfinite(snap.y) && std::isfinite(snap.scale) &&
         std::isfinite(snap.fontHeight) && snap.color[3] > 0.01f;
}
#undef GetTickCount
static bool NearlyEqual(float a, float b) { return std::fabs(a-b) < 0.0001f; }
int main() {
  const float color[] = {1,1,1,1};
  for (bool qte : {false, true}) {
    s_QteScaleMode = qte;
    for (float h : {48.0f, 72.0f, 78.0f, 90.0f, 99.0f, 160.0f, 500.0f}) {
      for (float s : {0.8f, 1.0f, 1.6f, 4.0f}) {
        const float reference = KoreanRenderer::CalculateFinalScale(h,s);
        for (float r : {1.0f, 4.0f/3.0f, 2.0f}) {
          const HudTextRenderer hud(1080.0f*r);
          assert(NearlyEqual(hud.CalculateFinalScale(h*r,s),reference*r));
          float width = hud.MeasureTextWidthEx("test",h*r,s);
          hud.QueueText("test",123,456,s,color,h*r,0,width,false,false,1);
          assert(NearlyEqual(queuedScale,reference*r));
          assert(NearlyEqual(queuedWidth,width) && queuedX == 123 && queuedY == 456);
        }
      }
    }
  }
  s_QteScaleMode = false;
  assert(!NearlyEqual(KoreanRenderer::CalculateFinalScale(156,1),
               HudTextRenderer(2160).CalculateFinalScale(156,1)));
  auto &snap = g_TimeScriptRuntimeState.renderSnapshot;
  assert(TextHook_HasTimeScriptTimerReplacement());
  snap.timerComputed=false; assert(!TextHook_HasTimeScriptTimerReplacement());
  snap.timerComputed=true; snap.timerSeconds=0;
  assert(!TextHook_HasTimeScriptTimerReplacement());
  snap.timerSeconds=std::numeric_limits<float>::quiet_NaN();
  assert(!TextHook_HasTimeScriptTimerReplacement());
  snap.timerSeconds=60; snap.lastSeenTick=4000;
  assert(!TextHook_HasTimeScriptTimerReplacement());
  snap.lastSeenTick=5000; g_TimeScriptRuntimeState.directOwnerTick=0;
  assert(!TextHook_HasTimeScriptTimerReplacement());
  g_TimeScriptRuntimeState.iarFallbackOwnerTick=5000;
  assert(TextHook_HasTimeScriptTimerReplacement());
  snap.renderText.clear(); assert(!TextHook_HasTimeScriptTimerReplacement());
  std::cout << "PASS: 1080p/1440p/4K scale and queue-width consistency; "
               "missing, expired and invalid timer replacement guards.\n";
}
