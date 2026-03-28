#pragma once
#include <cstdint>
#include <cstdio>

// =============================================================================
// IW6 Font Structure
// Layout verified against iw6x-client src/client/game/structs.hpp (x64).
//
// Font_s (40 bytes on x64):
//   +0x00  const char*  fontName       (8 bytes – pointer)
//   +0x08  int          glyphCount     (4 bytes)  ← iw6x-client: pixelHeight
//   +0x0C  int          fontHeight     (4 bytes)  ← iw6x-client: glyphCount
//   +0x10  Material*    material       (8 bytes – pointer)
//   +0x18  Material*    glowMaterial   (8 bytes – pointer)
//   +0x20  Glyph*       glyphs         (8 bytes – pointer)
//
// NOTE: fontHeight/glyphCount names are intentionally kept at the legacy
// offsets (+0x0C / +0x08) because CalculateFinalScale in KoreanRenderer is
// calibrated around the value at +0x0C (actual game glyphCount ≈ 96–256).
// GamepadGlyphAtlas.cpp reads raw offsets and is not affected by this naming.
//
// Glyph (24 bytes = 0x18, natural alignment):
//   +0x00  uint16_t  letter       (unicode codepoint)
//   +0x02  int8_t    x0           (atlas x offset)
//   +0x03  int8_t    y0           (atlas y offset)
//   +0x04  int8_t    dx           (advance width)
//   +0x05  int8_t    pixelWidth   (glyph pixel width)
//   +0x06  int8_t    pixelHeight  (glyph pixel height)
//   +0x07  (padding)
//   +0x08  float     s0           (UV left)
//   +0x0C  float     t0           (UV top)
//   +0x10  float     s1           (UV right)
//   +0x14  float     t1           (UV bottom)
// =============================================================================

namespace IW6Font {

struct Glyph {
  uint16_t letter;      // +0x00
  int8_t   x0;          // +0x02
  int8_t   y0;          // +0x03
  int8_t   dx;          // +0x04  advance
  int8_t   pixelWidth;  // +0x05
  int8_t   pixelHeight; // +0x06
  int8_t   _pad;        // +0x07
  float    s0;          // +0x08  UV left
  float    t0;          // +0x0C  UV top
  float    s1;          // +0x10  UV right
  float    t1;          // +0x14  UV bottom
};
static_assert(sizeof(Glyph) == 24, "IW6Font::Glyph must be 24 bytes");

struct Font_s {
  const char *fontName;    // +0x00
  int         glyphCount;  // +0x08  (legacy field name; actual game value: pixelHeight)
  int         fontHeight;  // +0x0C  (legacy field name; actual game value: glyphCount)
  void       *material;    // +0x10  (full Material* — layout is opaque)
  void       *glowMaterial;// +0x18
  Glyph      *glyphs;      // +0x20
};

// Helper: get glyph height from a Font_s pointer (safe — checks bounds)
inline int GetFontHeight(void *font) {
  if (!font) return 0;
  Font_s *f = reinterpret_cast<Font_s *>(font);
  int h = f->fontHeight;
  return (h >= 6 && h <= 200) ? h : 0;
}

// Helper: find a glyph by codepoint (linear scan)
inline Glyph *FindGlyph(void *font, uint16_t codepoint) {
  if (!font) return nullptr;
  Font_s *f = reinterpret_cast<Font_s *>(font);
  if (!f->glyphs || f->glyphCount <= 0) return nullptr;
  for (int i = 0; i < f->glyphCount; i++) {
    if (f->glyphs[i].letter == codepoint)
      return &f->glyphs[i];
  }
  return nullptr;
}

} // namespace IW6Font
