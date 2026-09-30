#pragma once
#include <cstdint>
#include <vector>

// Final glyph quads, after the engine's own text loop has evaluated reveal,
// decay, colour codes, shadow/glow passes and rotation. No lifetime is stored.
struct NativeGlyphQuad {
  float x[4]{}, y[4]{};
  float u0=0, v0=0, u1=0, v1=0;
  uint32_t color=0;
  bool glow=false;
};
namespace NativeGlyphBridge {
using Backend = void(__fastcall *)(const unsigned char**);
using ScaledWidth = int(__fastcall *)(const char*,int,void*,float);
using Layout = void(__fastcall *)(int,void*,void*,float,float*,int);
bool Initialize();
bool IsReady();
int TextWidth(const char *text, int maxChars, void *font);
int TextWidthScaled(ScaledWidth original,const char *text,int maxChars,void *font,float scale);
void RenderLayout(Layout original,int client,void *rect,void *font,float scale,float *color,int style);
void RenderBackend(Backend original, const unsigned char **iterator);
}
