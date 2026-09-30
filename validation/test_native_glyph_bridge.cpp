#include <windows.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include "NativeGlyphBridge.h"
#include "KoreanRenderer.h"
#include "NativeMenuDraw.h"
#include "GameAddresses.h"
#include "IW6Offsets.h"
#include "IW6Font.h"
#include "KoreanAtlas.h"
static float aspect=1.25f;
static HMODULE TestModule(const wchar_t*) { return (HMODULE)((uintptr_t)&aspect-IW6Offsets::TextPixelAspect_SP); }
#define GetModuleHandleW TestModule
#include "NativeGlyphBridge.cpp"
#undef GetModuleHandleW
using Glyph=IW6Font::Glyph;
using Font=IW6Font::Font_s;
static NativeGlyphBridge::Lookup lookupHook;
static NativeGlyphBridge::Paint paintHook;
static NativeGlyphBridge::Quad quadHook;
static Glyph latin{72,0,-22,16,15,22};
static Font font{"fonts/test",30,96,(void*)11,(void*)12,&latin};
static std::vector<NativeGlyphQuad> captured;
static int nativePaints=0,nativeQuads=0,flushes=0,backends=0;
bool KoreanRenderer::s_bInitialized=true;
void KoreanRenderer::RenderNativeGlyphQuads(const std::vector<NativeGlyphQuad>& q) { captured.insert(captured.end(),q.begin(),q.end()); }
void NativeMenuDraw::FlushNativeGeometry() { ++flushes; }
static Glyph* __fastcall Lookup(Font* f,unsigned) { assert(f==&font);return &latin; }
static void __fastcall Paint(void* m,float x,float y,float w,float h,float s,float c,const Glyph* g,uint32_t rgba) {
  assert(m==(void*)11 && g==&latin && rgba==0xffaabbcc && x==180 && y==250);
  assert(w==15 && h==22 && s==0 && c==1);
  assert(!captured.empty()); // pending Korean flushed before native F glyph
  ++nativePaints;
}
static int __fastcall Width(const char* text,int maxChars,void* f) {
  assert(f==&font);
  int width=0,count=0;
  for (size_t i=0;text[i] && (maxChars<=0 || count<maxChars);++count) {
    uint32_t cp=0;const auto n=KoreanAtlas::DecodeUTF8(text+i,cp);assert(n>0);i+=n;
    width+=(unsigned char)lookupHook(&font,cp)->dx;
  }
  return width;
}
static int __fastcall ScaledWidth(const char* text,int maxChars,void* f,float scale) {
  assert(maxChars==0 && scale==.75f && f==&font);
  return (int)std::lround(Width(text,maxChars,f)*scale);
}
static void __fastcall Quad(void* material,float x,float y,float w,float h,
    float u0,float v0,float u1,float v1,float s,float c,uint32_t rgba,int technique){
  assert(material==(void*)33 && x==310 && y==410 && w==25 && h==35);
  assert(u0==.1f && v0==.2f && u1==.3f && v1==.4f && s==.5f && c==.6f);
  assert(rgba==0x12345678 && technique==17);
  assert(captured.size()==2); // Korean glow was flushed before a native FX sprite
  ++nativeQuads;
}
static void __fastcall Layout(int client,void* rect,void* f,float scale,float* color,int style){
  assert(client==0 && rect==(void*)77 && f==&font && scale==.75f && color==(float*)88 && style==4);
  assert(lookupHook(&font,0xac00)!=&latin);
  assert(Width("가가",1,f)==Width("가",0,f)); // same UTF-8 glyph counts as native wrapping
}
void* GameAddresses::Resolve(uintptr_t,uintptr_t id) {
  if(id==IW6Offsets::RB_DrawTextQuad_SP)return (void*)Quad;
  return id==IW6Offsets::R_GetCharacterGlyph_SP?(void*)Lookup:
      id==IW6Offsets::RB_DrawChar_SP?(void*)Paint:(void*)Width;
}
bool Detour::Install(void* target,void* replacement,void** original,int) {
  *original=target;
  if(target==(void*)Lookup)lookupHook=(NativeGlyphBridge::Lookup)replacement;
  else if(target==(void*)Quad)quadHook=(NativeGlyphBridge::Quad)replacement;
  else {assert(target==(void*)Paint);paintHook=(NativeGlyphBridge::Paint)replacement;}
  return true;
}
const char* Detour::ActiveBackendName() { return "test"; }
static bool visible=true;
static void __fastcall Backend(const unsigned char** iterator) {
  ++backends;
  if (visible) {
    const Glyph* g=lookupHook(&font,0xac00);
    assert(g!=&latin && g->letter==0xac00);
    paintHook((void*)11,100,200,20,30,0,1,g,0x80402010);
    paintHook((void*)11,180,250,15,22,0,1,&latin,0xffaabbcc);
    paintHook((void*)12,300,400,20,30,.5f,.8660254f,g,0x11223344);
    quadHook((void*)33,310,410,25,35,.1f,.2f,.3f,.4f,.5f,.6f,0x12345678,17);
  }
  *iterator+=*reinterpret_cast<const unsigned short*>(*iterator+2);
}
int main(int argc,char**argv) {
  assert(argc==2 && KoreanAtlas::LoadMetrics(argv[1]));
  assert(NativeGlyphBridge::Initialize());
  assert(lookupHook(&font,0xac00)==&latin); // no global glyph replacement
  const int width=NativeGlyphBridge::TextWidth("가가",99,&font);
  assert(width>20 && width<80);
  assert(NativeGlyphBridge::TextWidthScaled(ScaledWidth,"가가",0,&font,.75f)==(int)std::lround(width*.75f));
  assert(!NativeGlyphBridge::current);
  NativeGlyphBridge::RenderLayout(Layout,0,(void*)77,&font,.75f,(float*)88,4);
  assert(!NativeGlyphBridge::current && lookupHook(&font,0xac00)==&latin);
  {
    NativeGlyphBridge::Scope scope(&font,false);
    // Typeface-wide ink centre must stay on the original Latin cap centre.
    // This catches the sign inversion between atlas bearingY and IW6 y0.
    int top=1000,bottom=-1000;
    for(unsigned cp=0xac00;cp<=0xd7a3;++cp) {
      const auto g=lookupHook(&font,cp);
      assert(g!=&latin && g->letter==cp);
      top=(std::min)(top,(int)g->y0);
      bottom=(std::max)(bottom,(int)g->y0+(unsigned char)g->pixelHeight);
    }
    const float nativeCentre=latin.y0+(unsigned char)latin.pixelHeight*.5f;
    assert(top<0 && std::abs((top+bottom)*.5f-nativeCentre)<=1);
  }
  alignas(8) unsigned char command[112]{};
  *reinterpret_cast<unsigned short*>(command+2)=sizeof(command);
  *reinterpret_cast<Font**>(command+16)=&font;
  std::strcpy(reinterpret_cast<char*>(command+92),"가");
  const unsigned char* cursor=command;
  NativeGlyphBridge::RenderBackend(Backend,&cursor);
  assert(cursor==command+sizeof(command) && backends==1 && captured.size()==2);
  assert(nativePaints==1 && nativeQuads==1 && flushes==2 && !NativeGlyphBridge::current);
  const auto &a=captured[0],&b=captured[1];
  assert(a.x[0]==100 && a.y[0]==200 && a.x[2]==120 && a.y[2]==230);
  assert(a.color==0x80402010 && !a.glow && b.color==0x11223344 && b.glow);
  assert(std::abs(b.x[3]-(300-30*.5f/aspect))<.001f);
  assert(std::abs(b.y[1]-(400+20*.5f*aspect))<.001f);
  assert(a.u1>a.u0 && a.v1>a.v0);
  visible=false;cursor=command;
  NativeGlyphBridge::RenderBackend(Backend,&cursor);
  assert(backends==2 && captured.size()==2 && flushes==2); // engine hid it: nothing recreated
  std::cout<<"PASS: production glyph bridge, native backend ownership, key/quad order, exact RGBA and geometry, glow, pixel aspect, no stale replay\n";
}
