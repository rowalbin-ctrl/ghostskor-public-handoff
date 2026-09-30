#pragma once
#include <cmath>

// Screen coordinates from R_AddCmdDrawText, with alignment from its LUI owner.
// No text keywords, viewport bands, or neighbouring commands participate.
struct NativeTextLayout {
  bool valid = false;
  float x = 0, y = 0, width = 0;
  float scaleX = 1, scaleY = 1;
  float maxWidth = 0;
  float capTop = 0, capBottom = 0; // native glyph bounds, in unscaled font pixels
  float fontPixelHeight = 0;
  float engineScaleX = 1, engineScaleY = 1;
  int alignment = 0; // renderer convention: 0 left, 1 right, 2 centre
  unsigned char atlasSlot = 0;

  float Anchor() const {
    return x + width * (alignment == 1 ? 1.0f : alignment == 2 ? 0.5f : 0.0f);
  }
  static int FromLuiAlignment(int value) {
    return value == 3 ? 1 : value == 2 ? 2 : 0;
  }
  // Compare the same Latin cap glyph in both fonts. Fitting the tallest
  // Hangul syllable into a Latin cap box shrinks the whole Korean typeface.
  float FontScaleX(float atlasCapTop, float atlasCapBottom) const {
    return (capBottom - capTop) * engineScaleX / (atlasCapBottom - atlasCapTop);
  }
  // Retain the established visual centre when changing font size; do not
  // lower the glyph baseline into the engine's clip edge. Use typeface-wide
  // Hangul bounds so the baseline cannot jump as the menu text changes.
  float BaselineOffsetX(float atlasTop, float atlasBottom, float fontScaleX) const {
    return (capTop + capBottom) * 0.5f * engineScaleX -
        (atlasTop + atlasBottom) * 0.5f * fontScaleX;
  }
};
