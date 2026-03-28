#pragma once
#include <cstdint>
#include <d3d11.h>

// =============================================================================
// GamepadGlyphAtlas
// Captures the gamepad button glyph SRV + UV coords from the game's Font_s
// material system (Font_s → Material → DB_FindXAssetHeader(IMAGE) → GfxImage).
//
// Called once from Detour_R_AddCmdDrawText when the game renders text that
// contains control chars \x01–\x17 (button glyphs).  After capture, IsReady()
// returns true and KoreanRenderer uses the SRV for inline button rendering.
// =============================================================================

namespace GamepadGlyphAtlas {

// UV + pixel dimensions for one button glyph
struct GlyphUV {
  float s0, t0, s1, t1; // normalized UV coords
  bool  valid;
};

// Attempt to capture the atlas from a Font_s pointer (thread-safe: runs once).
// fontPtr  – raw Font_s* passed as arg 3 of R_AddCmdDrawText
// Returns true if the capture succeeded (or was already done).
bool TryCapture(void *fontPtr);

// True after a successful capture.
bool IsReady();

// The captured SRV — do NOT Release() it; lifetime is the game process.
ID3D11ShaderResourceView *GetSRV();

// UV coords for button char ch (1–23).  Returns {valid=false} if unavailable.
GlyphUV GetGlyphUV(unsigned char ch);

// Release resources on DLL unload.
void Shutdown();

} // namespace GamepadGlyphAtlas
