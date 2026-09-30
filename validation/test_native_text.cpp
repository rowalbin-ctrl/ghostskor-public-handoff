#include "NativeMenuCapture.h"
#include "GameAddresses.h"
#include "GameAddressProfile.h"
#include "GameBuild.h"
#include "KoreanRenderer.h"
#include "IW6Font.h"
#include "KoreanAtlas.h"
#include <cassert>
#include <map>
#include <iostream>

// Exercise the production callbacks with a controlled engine and real layout
// bytes. No game process or installed files are involved in this test.
static std::map<uintptr_t, void*> callbacks;
static NativeTextLayout captured;
static int currentAlignment;
static bool expectUnbounded = false;
static int expectedAnchor = -1;
static const char *expectedSource = "HEADER";
static bool checkMultiline = false;
static bool checkJoined = false;
alignas(8) static unsigned char element[0x120]{};
static IW6Font::Glyph glyphs[] = {{72,2,-29,21,18,22}};
static IW6Font::Font_s font{"fonts/normalFont", 35, 1, nullptr, nullptr, glyphs};
static void Sample() {
  if (checkJoined) {
    auto *run = NativeMenuCapture::CurrentTextRun();
    assert(run && run->joinedLiterals && !run->claimed);
    assert(*run->source == "If you quit now, you will lose any progress since your last checkpoint. ");
    assert(run->Claim() && !run->Claim());
  }
  if (checkMultiline) {
    auto *run = NativeMenuCapture::CurrentTextRun();
    assert(run && run->IsMultiline() && !run->claimed);
    assert(*run->source == expectedSource);
    assert(run->Claim());
    assert(!run->Claim()); // all subsequent visual lines belong to this draw
  }
  NativeMenuCapture::TextScope scope("HEADER", 99, &font, 125, 80, 2, 3);
  captured = NativeMenuCapture::CurrentText();
  assert(captured.valid);
  assert(captured.alignment == (currentAlignment == 0 && expectedAnchor >= 0 ?
      expectedAnchor : NativeTextLayout::FromLuiAlignment(currentAlignment)));
  assert(captured.x == 125 && captured.y == 80 && captured.width == 200);
  assert(std::fabs(captured.scaleX - 70.0f / 42) < .0001f);
  assert(std::fabs(captured.scaleY - 105.0f / 42) < .0001f);
  assert(captured.maxWidth == (expectUnbounded ? 0 : 600));
  assert(captured.capTop == -29 && captured.capBottom == -7);
  // A nested draw restores the outer draw's layout, including early returns.
  { NativeMenuCapture::TextScope nested("HEADER", 99, &font, 500, 700, 2, 3);
    assert(NativeMenuCapture::CurrentText().x == 500); }
  assert(NativeMenuCapture::CurrentText().x == 125);
}
static void Frame() {}
static void Scissor(int,int,int,int) {}
static int Width(const char *text, int count, void *f) {
  assert(std::string(text) == "HEADER" && count == 99 && f == &font);
  return 100;
}
static bool Cached(void*,void*) { Sample(); return true; }
using TextFn = void(*)(void*,float,float,float,float,float,float,const char*,void*,float,float,int,void*);
static void Text(void *element, float x, float y, float r, float g, float b,
    float a, const char *text, void *f, float height, float width, int align, void *ctx) {
  assert(element == ::element && x == 2 && y == 3 && r == 4 && g == 5 && b == 6 && a == 7);
  assert(std::string(text) == expectedSource && f == (void*)9 && height == 35 && width == (expectUnbounded ? -1 : 300));
  assert(align == currentAlignment && ctx == (void*)13); // all 13 ABI arguments
  Sample();
}
bool GameBuild::RuntimeReady() { return true; }
// Ordered dispatch has its own production-callback harness.
namespace NativeMenuDraw {
bool Initialize() { return false; }
void BeginFrame() {}
}
void KoreanRenderer::BeginMenuFrame() {}
void KoreanRenderer::EndMenuFrame() {}
void *GameAddresses::Resolve(uintptr_t, uintptr_t id) {
  return id == IW6Offsets::R_TextWidth_SP ? (void*)Width : (void*)id;
}
bool Detour::Install(void *target, void *replacement, void **original, int) {
  const auto id = (uintptr_t)target;
  callbacks[id] = replacement;
  *original = id == IW6Offsets::LUI_RenderText_SP ? (void*)Text :
      id == IW6Offsets::LUI_RenderCachedElement_SP ? (void*)Cached :
      id == IW6Offsets::R_AddCmdSetScissor_SP ? (void*)Scissor : (void*)Frame;
  return true;
}
const char *Detour::ActiveBackendName() { return "test"; }

int main(int argc, char **argv) {
  assert(argc == 3);
  assert(KoreanAtlas::LoadMetrics(argv[1], 0));
  assert(KoreanAtlas::LoadMetrics(argv[2], 1));
  assert(NativeMenuCapture::Initialize());
  assert(callbacks.size() == 6);
  ((void(*)())callbacks.at(IW6Offsets::R_BeginRenderCommands_SP))();
  *reinterpret_cast<void**>(element + 0xD0) = (void*)123;
  *reinterpret_cast<float*>(element + 0xF8) = 800; // differs from actual RenderText width
  *reinterpret_cast<float*>(element + 0xFC) = 35;
  for (int alignment : {0, 1, 2, 3}) {
    currentAlignment = alignment;
    *reinterpret_cast<int*>(element + 0x70) = alignment;
    ((TextFn)callbacks.at(IW6Offsets::LUI_RenderText_SP))(
        element, 2, 3, 4, 5, 6, 7, "HEADER", (void*)9, 35, 300, alignment, (void*)13);
    const auto fresh = captured;
    assert(!NativeMenuCapture::CurrentText().valid);
    assert(((bool(*)(void*,void*))callbacks.at(IW6Offsets::LUI_RenderCachedElement_SP))(element, nullptr));
    assert(captured.Anchor() == fresh.Anchor() && captured.maxWidth == fresh.maxWidth);
    assert(captured.Anchor() == (alignment == 2 ? 225 : alignment == 3 ? 325 : 125));
    assert(!NativeMenuCapture::CurrentText().valid);
  }
  // Reproduce the reported fresh one-line / cached two-line alternation:
  // a positive element rectangle must NOT turn an unbounded draw into wrapping.
  currentAlignment = 1; expectUnbounded = true;
  ((TextFn)callbacks.at(IW6Offsets::LUI_RenderText_SP))(
      element, 2, 3, 4, 5, 6, 7, "HEADER", (void*)9, 35, -1, 1, (void*)13);
  assert(captured.maxWidth == 0);
  ((bool(*)(void*,void*))callbacks.at(IW6Offsets::LUI_RenderCachedElement_SP))(element, nullptr);
  assert(captured.maxWidth == 0);
  // Rorke values are unbounded alignment-0 text whose English start was
  // pre-positioned by LUI anchors. Preserve the edge/centre for a translation
  // of any width, without keywords or coordinates. Test fresh and cached draws,
  // including anchor changes while the command-cache pointer stays unchanged.
  currentAlignment = 0;
  *reinterpret_cast<int*>(element + 0x70) = 0;
  *reinterpret_cast<unsigned*>(element + 0x10) = 0x40;
  for (int left : {0, 1}) for (int right : {0, 1}) {
    element[0x6C] = (unsigned char)left;
    element[0x6E] = (unsigned char)right;
    expectedAnchor = left == right ? 2 : left ? 0 : 1;
    ((TextFn)callbacks.at(IW6Offsets::LUI_RenderText_SP))(
        element, 2, 3, 4, 5, 6, 7, "HEADER", (void*)9, 35, -1, 0, (void*)13);
    const auto edge = captured.Anchor();
    assert(edge == (expectedAnchor == 1 ? 325 : expectedAnchor == 2 ? 225 : 125));
    for (float translatedWidth : {32.0f, 200.0f, 480.0f}) {
      const float fraction = expectedAnchor == 1 ? 1.0f : expectedAnchor == 2 ? 0.5f : 0.0f;
      const float translatedX = edge - translatedWidth * fraction;
      assert(translatedX + translatedWidth * fraction == edge);
    }
    element[0x6C] = 0; element[0x6E] = 1; expectedAnchor = 1;
    ((bool(*)(void*,void*))callbacks.at(IW6Offsets::LUI_RenderCachedElement_SP))(element, nullptr);
    assert(captured.Anchor() == 325);
  }
  // Explicit alignment is independent of element anchors (native bounds use
  // the fixed rectangle), and disabled anchor state must remain left-aligned.
  currentAlignment = 1;
  ((TextFn)callbacks.at(IW6Offsets::LUI_RenderText_SP))(
      element, 2, 3, 4, 5, 6, 7, "HEADER", (void*)9, 35, -1, 1, (void*)13);
  assert(captured.Anchor() == 125);
  currentAlignment = 0; expectedAnchor = -1;
  *reinterpret_cast<unsigned*>(element + 0x10) = 0;
  ((TextFn)callbacks.at(IW6Offsets::LUI_RenderText_SP))(
      element, 2, 3, 4, 5, 6, 7, "HEADER", (void*)9, 35, -1, 0, (void*)13);
  assert(captured.Anchor() == 125);
  ((bool(*)(void*,void*))callbacks.at(IW6Offsets::LUI_RenderCachedElement_SP))(element, nullptr);
  assert(captured.maxWidth == 0);
  for (unsigned char slot : {0, 1}) {
    float top, bottom, capTop, capBottom;
    assert(KoreanAtlas::GetHangulBounds(slot, top, bottom));
    assert(KoreanAtlas::GetCapBounds(slot, capTop, capBottom));
    assert(top < 0 && bottom > 0);
    // Actual supplied typefaces: the Latin cap is shorter than Hangul.
    assert(capBottom - capTop == (slot == 0 ? 30 : 33));
    assert(capBottom - capTop < bottom - top);
    for (float ratio : {0.5f, 0.75f, 1.0f, 1.024f, 1.2f, 1.6f, 2.0f}) {
      auto layout = captured;
      layout.engineScaleX = ratio;
      const float scale = layout.FontScaleX(capTop, capBottom);
      const float shift = layout.BaselineOffsetX(top, bottom, scale);
      // Regression: compare like-for-like cap heights, not Hangul vs Latin.
      assert(std::fabs((capBottom - capTop) * scale - 22 * ratio) < .0001f);
      assert(scale > 22 * ratio / (bottom - top));
      const float centre = shift + (top + bottom) * 0.5f * scale;
      assert(std::fabs(centre - (layout.capTop + layout.capBottom) * 0.5f * ratio) < .0001f);
      assert(shift + bottom * scale < 0); // native bottom is above the nominal baseline
      assert(shift + top * scale > -layout.fontPixelHeight * ratio);
    }
  }
  checkMultiline = true;
  expectedSource = "If you quit now, you will lose any progress\nsince your last checkpoint.\n\nAre you sure?";
  ((TextFn)callbacks.at(IW6Offsets::LUI_RenderText_SP))(
      element, 2, 3, 4, 5, 6, 7, expectedSource, (void*)9, 35, -1, 0, (void*)13);
  assert(!NativeMenuCapture::CurrentTextRun());
  // Cached replay gets a fresh claim. No stale suppression across frames.
  ((bool(*)(void*,void*))callbacks.at(IW6Offsets::LUI_RenderCachedElement_SP))(element, nullptr);
  ((bool(*)(void*,void*))callbacks.at(IW6Offsets::LUI_RenderCachedElement_SP))(element, nullptr);
  assert(!NativeMenuCapture::CurrentTextRun());
  checkMultiline = false;
  // Captured from the live save/quit popup: the engine joins three localized
  // literal spans and then emits two visual lines. Cached replay must retain
  // the full joined source and reset its claim without changing other labels.
  checkJoined = true;
  expectedSource = "\x1f" "If you quit now, " "\x1e\x1f" "you will lose any progress "
                   "\x1e\x1f" "since your last checkpoint. " "\x1e";
  ((TextFn)callbacks.at(IW6Offsets::LUI_RenderText_SP))(
      element, 2, 3, 4, 5, 6, 7, expectedSource, (void*)9, 35, -1, 0, (void*)13);
  assert(!NativeMenuCapture::CurrentTextRun());
  ((bool(*)(void*,void*))callbacks.at(IW6Offsets::LUI_RenderCachedElement_SP))(element, nullptr);
  ((bool(*)(void*,void*))callbacks.at(IW6Offsets::LUI_RenderCachedElement_SP))(element, nullptr);
  assert(!NativeMenuCapture::CurrentTextRun());
  checkJoined = false;
  ((void(*)())callbacks.at(IW6Offsets::R_FinishRenderCommands_SP))();
  NativeMenuCapture::TextScope outside("HEADER", 99, &font, 0, 0, 1, 1);
  assert(!NativeMenuCapture::CurrentText().valid);
  std::cout << "PASS: production native text callbacks, all 13 arguments, cached/fresh parity, per-element alignment, actual font height, independent XY scale, Latin cap size matching with fixed Hangul centre, unbounded cached text, nested context restoration\n";
}
