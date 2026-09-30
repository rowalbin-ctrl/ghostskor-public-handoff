#include "NativeGlyphBridge.h"
#include "KoreanAtlas.h"
#include "KoreanRenderer.h"
#include "NativeMenuDraw.h"
#include "IW6Font.h"
#include "IW6Offsets.h"
#include "Utils.h"
#include "MenuTrace.h"
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace NativeGlyphBridge {
namespace {
using Glyph = IW6Font::Glyph;
using Font = IW6Font::Font_s;
using Lookup = Glyph*(__fastcall *)(Font*, unsigned);
using Paint = void(__fastcall *)(void*,float,float,float,float,float,float,const Glyph*,uint32_t);
using Width = int(__fastcall *)(const char*,int,void*);
using Quad = void(__fastcall *)(void*,float,float,float,float,float,float,float,float,float,float,uint32_t,int);
Lookup originalLookup = nullptr;
Paint originalPaint = nullptr;
Width originalWidth = nullptr;
Quad originalQuad = nullptr;
const float *pixelAspect = nullptr;
std::atomic<bool> ready{false};
struct FontMetrics {
  Font *font=nullptr;
  Glyph *sourceGlyphs=nullptr;
  int height=0, count=0;
  float scale=0, baseline=0;
  std::unordered_map<unsigned,Glyph> glyphs;
};
thread_local std::unordered_map<Font*,std::unique_ptr<FontMetrics>> metrics;
struct State {
  FontMetrics *metrics=nullptr;
  bool paint=false;
  std::vector<NativeGlyphQuad> quads;
};
thread_local State *current=nullptr;

FontMetrics *GetMetrics(Font *font) {
  if (!font || font->glyphCount<=0 || font->glyphCount>512) return nullptr;
  auto &entry=metrics[font];
  if (entry && entry->sourceGlyphs==font->glyphs && entry->height==font->glyphCount &&
      entry->count==font->fontHeight) return entry.get();
  float capTop,capBottom,inkTop,inkBottom;
  const auto h=originalLookup(font,'H');
  if (!h || !KoreanAtlas::GetCapBounds(0,capTop,capBottom) ||
      !KoreanAtlas::GetHangulBounds(0,inkTop,inkBottom) || capBottom<=capTop) return nullptr;
  auto next=std::make_unique<FontMetrics>();
  next->font=font; next->sourceGlyphs=font->glyphs;
  next->height=font->glyphCount; next->count=font->fontHeight;
  next->scale=(unsigned char)h->pixelHeight/(capBottom-capTop);
  next->baseline=(h->y0+(unsigned char)h->pixelHeight*.5f)-
      (inkTop+inkBottom)*.5f*next->scale;
  entry=std::move(next);
  return entry.get();
}
void Flush() {
  if (!current || current->quads.empty()) return;
  NativeMenuDraw::FlushNativeGeometry();
  KoreanRenderer::RenderNativeGlyphQuads(current->quads);
  current->quads.clear();
}
void __fastcall DrawNativeQuad(void *material,float x,float y,float w,float h,
    float u0,float v0,float u1,float v1,float sine,float cosine,uint32_t color,int technique) {
  // Native scramble/decay sprites and inline icons bypass PaintGlyph. Flush
  // any preceding Korean face/shadow before these native primitives too.
  Flush();
  originalQuad(material,x,y,w,h,u0,v0,u1,v1,sine,cosine,color,technique);
}
struct Scope {
  State state;
  State *previous=current;
  Scope(Font *font,bool paint) { state.metrics=GetMetrics(font); state.paint=paint; current=&state; }
  ~Scope() { if (state.paint) Flush(); current=previous; }
};
Glyph *__fastcall FindGlyph(Font *font,unsigned codepoint) {
  if (!current || !current->metrics || current->metrics->font!=font || codepoint<128)
    return originalLookup(font,codepoint);
  auto &cache=*current->metrics;
  const auto found=cache.glyphs.find(codepoint);
  if (found!=cache.glyphs.end()) return &found->second;
  const auto source=KoreanAtlas::GetGlyph(codepoint);
  if (!source || codepoint>65535) return originalLookup(font,codepoint);
  const float width=(source->u1-source->u0)/65535.f*KoreanAtlas::GetAtlasWidth(0);
  const float height=(source->v1-source->v0)/65535.f*KoreanAtlas::GetAtlasHeight(0);
  const int x=(int)std::lround(source->xOffset*cache.scale);
  // Atlas metrics store a positive bearing ABOVE the baseline. IW6 glyphs
  // instead store the signed top offset FROM the baseline.
  const int y=(int)std::lround(-source->yOffset*cache.scale+cache.baseline);
  const int w=(int)std::lround(width*cache.scale), h=(int)std::lround(height*cache.scale);
  const int advance=(int)std::lround(source->advance*cache.scale);
  if (x < -128 || x > 127 || y < -128 || y > 127 || w<0 || w>255 || h<0 || h>255 || advance<0 || advance>255)
    return originalLookup(font,codepoint);
  Glyph glyph{};
  glyph.letter=(uint16_t)codepoint; glyph.x0=(int8_t)x; glyph.y0=(int8_t)y;
  glyph.dx=(int8_t)advance; glyph.pixelWidth=(int8_t)w; glyph.pixelHeight=(int8_t)h;
  glyph.s0=source->u0/65535.f; glyph.t0=source->v0/65535.f;
  glyph.s1=source->u1/65535.f; glyph.t1=source->v1/65535.f;
  return &cache.glyphs.emplace(codepoint,glyph).first->second;
}
void __fastcall PaintGlyph(void *material,float x,float y,float width,float height,
    float sine,float cosine,const Glyph *glyph,uint32_t color) {
  bool owned=false;
  if (current && current->paint && current->metrics && glyph) {
    const auto found=current->metrics->glyphs.find(glyph->letter);
    owned=found!=current->metrics->glyphs.end() && &found->second==glyph;
  }
  if (!owned) {
    // Keep mixed native key glyphs and translated letters in the same order.
    Flush();
    originalPaint(material,x,y,width,height,sine,cosine,glyph,color);
    return;
  }
  const float aspect=*pixelAspect;
  if (!(std::isfinite(aspect) && aspect>0)) return;
  NativeGlyphQuad quad{};
  // 5A4C90: the engine's final 2D rotation uses its current pixel aspect.
  const float dx=width*cosine, dy=width*sine*aspect;
  const float hx=-height*sine/aspect, hy=height*cosine;
  quad.x[0]=x; quad.y[0]=y;
  quad.x[1]=x+dx; quad.y[1]=y+dy;
  quad.x[2]=x+dx+hx; quad.y[2]=y+dy+hy;
  quad.x[3]=x+hx; quad.y[3]=y+hy;
  quad.u0=glyph->s0; quad.v0=glyph->t0; quad.u1=glyph->s1; quad.v1=glyph->t1;
  quad.color=color;
  quad.glow=material && material==current->metrics->font->glowMaterial;
  NativeMenuCapture::Trace(43,nullptr,x,y,(color>>24)/255.f,quad.glow?1.f:0.f);
  current->quads.push_back(quad);
}
}
bool Initialize() {
  static std::once_flag once;
  std::call_once(once,[] {
    const auto base=(uintptr_t)GetModuleHandleW(nullptr);
    const auto lookup=IW6Offsets::GetAddress(base,IW6Offsets::R_GetCharacterGlyph_SP);
    const auto paint=IW6Offsets::GetAddress(base,IW6Offsets::RB_DrawChar_SP);
    const auto quad=IW6Offsets::GetAddress(base,IW6Offsets::RB_DrawTextQuad_SP);
    originalWidth=(Width)IW6Offsets::GetAddress(base,IW6Offsets::R_TextWidth_SP);
    if (!lookup || !paint || !quad || !originalWidth) return;
    pixelAspect=reinterpret_cast<const float*>(base+IW6Offsets::TextPixelAspect_SP);
    if (!CreateHook(lookup,(void*)FindGlyph,(void**)&originalLookup)) return;
    if (!CreateHook(paint,(void*)PaintGlyph,(void**)&originalPaint)) return;
    if (!CreateHook(quad,(void*)DrawNativeQuad,(void**)&originalQuad)) return;
    ready.store(true,std::memory_order_release);
  });
  return ready.load(std::memory_order_acquire);
}
bool IsReady() { return ready.load(std::memory_order_acquire) && KoreanRenderer::s_bInitialized; }
int TextWidth(const char *text,int maxChars,void *font) {
  if (!IsReady() || !text) return 0;
  Scope scope((Font*)font,false);
  return originalWidth(text,maxChars,font);
}
int TextWidthScaled(ScaledWidth original,const char *text,int maxChars,void *font,float scale) {
  if (!IsReady() || !text) return original(text,maxChars,font,scale);
  Scope scope((Font*)font,false);
  // The native wrapper retains font normalization, maxChars and rounding.
  // Only the glyph metrics consulted by its inner width loop are extended.
  return original(text,maxChars,font,scale);
}
void RenderLayout(Layout original,int client,void *rect,void *font,float scale,float *color,int style) {
  if (!IsReady()) {original(client,rect,font,scale,color,style);return;}
  Scope scope((Font*)font,false);
  original(client,rect,font,scale,color,style);
}
void RenderBackend(Backend original,const unsigned char **iterator) {
  const auto command=*iterator;
  const unsigned size=*reinterpret_cast<const unsigned short*>(command+2);
  if (!IsReady() || size<96) { original(iterator); return; }
  const char *text=reinterpret_cast<const char*>(command+92);
  bool korean=false;
  for (unsigned i=0;i+2<size-92 && text[i];++i) {
    const auto a=(unsigned char)text[i],b=(unsigned char)text[i+1],c=(unsigned char)text[i+2];
    if ((a&0xf0)==0xe0 && (b&0xc0)==0x80 && (c&0xc0)==0x80) {
      const unsigned cp=((a&15)<<12)|((b&63)<<6)|(c&63);
      if ((cp>=0xac00 && cp<=0xd7a3) || (cp>=0x3130 && cp<=0x318f)) { korean=true; break; }
    }
  }
  if (!korean) { original(iterator); return; }
  Scope scope(*reinterpret_cast<Font* const*>(command+16),true);
  NativeMenuCapture::TraceSource(8,text);
  original(iterator); // All reveal/decay/glow decisions remain inside the game.
}
}
