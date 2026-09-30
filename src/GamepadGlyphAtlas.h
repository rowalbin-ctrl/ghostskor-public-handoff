#pragma once
#include "NativeButtonGlyph.h"
#include <array>
#include <d3d11.h>
#include <memory>

namespace GamepadGlyphAtlas {
using GlyphUV = NativeButtonGlyph;
// Immutable frame snapshot owns the native SRV through the last Draw.
struct Atlas {
  ID3D11ShaderResourceView* srv = nullptr;
  std::array<GlyphUV, 256> glyphs{};
  unsigned textureWidth = 0, textureHeight = 0;
  ~Atlas() { if (srv) srv->Release(); }
  Atlas() = default;
  Atlas(const Atlas&) = delete;
  Atlas& operator=(const Atlas&) = delete;
};
using Snapshot = std::shared_ptr<const Atlas>;
Snapshot Acquire();
// Called on the game's text producer thread. Select the native HUD big font
// in the same controller family as the engine source font.
bool TryCapture(void* fontPtr);
bool IsReady();
inline float Advance(const Atlas* atlas, unsigned ch, float lineHeight) {
  if (atlas && ch < atlas->glyphs.size() && atlas->glyphs[ch].valid)
    return atlas->glyphs[ch].advance * lineHeight;
  return 1.4f * lineHeight;
}
void Shutdown();
}
