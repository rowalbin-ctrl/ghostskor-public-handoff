#pragma once
#include <cmath>
#include <cstdint>

// Signed native bearings, unsigned dimensions/advance; 24-byte IW6 glyph.
struct NativeFontGlyph {
  uint16_t letter;
  int8_t x0, y0;
  uint8_t dx, width, height, pad;
  float s0, t0, s1, t1;
};
static_assert(sizeof(NativeFontGlyph) == 24, "Steam SP font glyph layout");
struct NativeButtonGlyph {
  float s0 = 0, t0 = 0, s1 = 0, t1 = 0;
  float x = 0, y = 0, width = 0, height = 0, advance = 0;
  bool valid = false;
};
inline NativeButtonGlyph NormalizeButtonGlyph(const NativeFontGlyph& raw, int fontHeight) {
  if (fontHeight <= 0 || fontHeight > 256 || !raw.width || !raw.height || !raw.dx ||
      !std::isfinite(raw.s0) || !std::isfinite(raw.t0) ||
      !std::isfinite(raw.s1) || !std::isfinite(raw.t1) ||
      raw.s0 < 0 || raw.t0 < 0 || raw.s1 > 1 || raw.t1 > 1 ||
      raw.s0 >= raw.s1 || raw.t0 >= raw.t1) return {};
  const float scale = 1.0f / fontHeight;
  return {raw.s0,raw.t0,raw.s1,raw.t1,raw.x0*scale,raw.y0*scale,
          raw.width*scale,raw.height*scale,raw.dx*scale,true};
}
