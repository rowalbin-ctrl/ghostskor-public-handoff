#include "KoreanRenderer.h"
#include "NativeMenuCapture.h"
#include "NativeMenuDraw.h"
#include "TranslationStore.h"
#include "GameAddresses.h"
#include "IW6Offsets.h"
#include "IW6Font.h"
#include "MenuTrace.h"
#include "NativeGamepad.h"
#include <atomic>
#include <cassert>
#include <cmath>
#include <iostream>
#include <mutex>
using Font_s=IW6Font::Font_s;
using Width=int(*)(const char*,int,Font_s*);
static std::atomic<Width> g_R_TextWidth;
static NativeMenuDraw::Producer Original_R_AddCmdDrawText;
static void EnsureTranslationsLoaded() {}
static bool ContainsKorean(const char* s) {for(;*s;++s)if((unsigned char)*s>=128)return true;return false;}
static std::unordered_map<uint32_t,std::string> configs;
template<size_t N>static bool TryReadNativeHudConfigText(uint32_t cfg,char(&text)[N]) {
  auto i=configs.find(cfg);if(i==configs.end() || i->second.size()>=N)return false;
  memcpy(text,i->second.c_str(),i->second.size()+1);return true;
}
#include "TextHook.NativeHudCaption.inl"
static NativeTextCatalog::Map english{{"PAIR","^3[{+activate}]^7"},{"PAIR_TEXT","to reach."},
  {"PLATFORM_HOLD_TO_USE","Press ^3[{+activate}]^7 to use"},
  {"ANY_UNCLASSIFIED_KEY","Defend the beach."},{"STATUS","Objective Completed."},
  {"COUNT","Collect &&1 of &&2 items."},{"SUBTITLE_TEST","Dialogue."},
  {"SUBTITLE_COLLISION","Move!"},{"NATIVE_COLLISION","Move!"},
  {"INTRO_TIME","June 15th - 0:41:[{FAKE_INTRO_SECONDS:09}]"},{"NATIVE_LABEL","Power down in "},
  {"MANTLE","Press^3 &&1 ^7to  "},{"SWAP","Press^3 &&1 ^7to swap for"},
  {"SENTRY_PLACE","Press ^3[{+attack}]^7 to place the turret."},
  {"AMBIG_A","Shared"},{"AMBIG_B","Shared"}};
static NativeTextCatalog::Map korean{{"PAIR","^3[{+activate}]^7"},{"PAIR_TEXT","손을 뻗으십시오."},
  {"PLATFORM_HOLD_TO_USE","^3[{+activate}]^7 키를 눌러 사용"},
  {"ANY_UNCLASSIFIED_KEY","해변을 방어하십시오."},{"STATUS","목표 완료."},
  {"COUNT","&&2개 중 &&1개 회수."},{"SUBTITLE_TEST","대사."},
  {"SUBTITLE_COLLISION","이동해!"},{"NATIVE_COLLISION","이동하십시오!"},
  {"INTRO_TIME","6월 15일 - 0:41:[{FAKE_INTRO_SECONDS:09}]"},{"NATIVE_LABEL","전원 차단까지 "},
  {"MANTLE","^3 &&1 ^7키를 눌러  "},{"SWAP","^3 &&1 ^7키를 눌러 교체:"},
  {"SENTRY_PLACE","^3[{+attack}]^7 키를 눌러 포탑을 설치합니다."},
  {"AMBIG_A","문맥 가"},{"AMBIG_B","문맥 나"}};
const NativeTextCatalog::Map &TranslationStore::KeyToEnglish(){return english;}
const NativeTextCatalog::Map &TranslationStore::KeyToKorean(){return korean;}
bool KoreanRenderer::s_bInitialized=true;
bool NativeMenuDraw::Initialize(){return true;}
bool NativeGlyphBridge::Initialize(){return true;}
static bool glyphsReady=true;
bool NativeGlyphBridge::IsReady(){return glyphsReady;}
bool TextHook_GetLocalizedKeyText(const std::string&,std::string&,std::string&){return false;}
bool NativeGamepad::ReadEnabled(bool&){assert(false);return false;}
bool NativeGamepad::ReadBindings(BindingSnapshot&){assert(false);return false;}
int NativeGlyphBridge::TextWidth(const char*,int,void*){return 80;}
static bool measuringKorean=false;
void NativeGlyphBridge::RenderLayout(Layout original,int client,void* rect,void* font,float scale,float* color,int style){
  assert(!measuringKorean);measuringKorean=true;
  original(client,rect,font,scale,color,style);measuringKorean=false;
}
int NativeGlyphBridge::TextWidthScaled(ScaledWidth original,const char* text,int count,void* font,float scale){
  const bool previous=measuringKorean;measuringKorean=true;
  const int result=original(text,count,font,scale);
  measuringKorean=previous;return result;
}
static Font_s font{};
static float color[]{.8f,.7f,.6f,.5f},glow[]{.1f,.2f,.3f,.4f};
static NativeHudCaption::Draw drawHook;
static NativeHudCaption::Fx fxHook;
static NativeHudCaption::Prepare prepareHook;
static NativeGlyphBridge::ScaledWidth widthHook;
static NativeHudCaption::Names namesHook;
static NativeHudCaption::Localize localizeHook;
static NativeHudCaption::SafeTranslate safeHook;
static NativeGlyphBridge::Layout headerHook,listHook;
static NativeHudCaption::OwnerDraw ownerHook;
static NativeHudCaption::Convert convertHook;
static std::string actual;
static float actualX=0;
static int calls=0;
static float preparedX=400;
static const char* textToPrepare=nullptr;
static std::string measured;
static int measuredWidth=0;
static void __fastcall Fx(const char* text,int count,Font_s* f,float x,float y,float sx,float sy,
    float rotation,float* c,int style,float* g,void* material,void* glowMaterial,
    int birth,int letter,int decay,int duration) {
  ++calls;actual=text;actualX=x;
  assert(count==99 && f==&font && y==220 && sx==1.5f && sy==2);
  assert(rotation==15 && c==color && style==3 && g==glow);
  assert(material==(void*)21 && glowMaterial==(void*)22);
  assert(birth==12345 && letter==30 && decay==4000 && duration==700);
}
static void __fastcall Draw(int client,const char* text,uintptr_t element,void* context) {
  assert(client==0 && element && context==(void*)13);
  fxHook(text,99,&font,preparedX,220,1.5f,2,15,color,3,glow,(void*)21,(void*)22,12345,30,4000,700);
}
static int __fastcall ScaledWidth(const char* text,int count,void* f,float scale) {
  assert(count==0 && f==&font && scale==.75f);
  measured=text;
  if(ContainsKorean(text))assert(measuringKorean);
  return ContainsKorean(text)?61:90;
}
static void __fastcall Prepare(int client,uintptr_t element,void* state,void* scratch) {
  assert(client==0 && element && state==(void*)13 && scratch==(void*)14);
  assert(NativeHudCaption::current && NativeHudCaption::current->measuring);
  measuredWidth=widthHook(textToPrepare,0,&font,.75f);
  // Stand-in for the native layout output; the draw bridge must pass it on
  // unchanged, including fractional positions during an origin transition.
  preparedX=287.25f;
}
static int TextWidth(const char*,int,Font_s*){return 100;}
static void __fastcall Producer(const char* text,int,void*,float x,float,float,float,float,float*,int) {
  actual=text;actualX=x;++calls;
}
static void __fastcall Names(int client) {
  assert(client==0);
  NativeHudCaption::Plain("Objective Completed.",99,&font,30,40,1,1,0,color,0);
  assert(actual=="Objective Completed."); // scope, not a character-name blacklist
}
static const char* __fastcall Localize(const char* text,const char* label,int flags){
  assert(flags==7 && !strcmp(label,"objective"));
  return english.at(text).c_str();
}
static const char* __fastcall Safe(const char* text){return english.at(text).c_str();}
static const char* __fastcall Convert(const char* text,const char* value){
  static std::string converted;
  converted=text;const auto at=converted.find("&&1");
  assert(at!=std::string::npos);converted.replace(at,3,value);return converted.c_str();
}
static void __fastcall Owner(int client,void* window,float x,float y,float w,float h,
    int horizontal,int vertical,float textX,float textY,int owner,int flags,float special,
    void* f,float scale,float* c,void* material,int style,int textAlign){
  assert(client==0 && window==(void*)73 && x==1.25f && y==2.5f && w==3.75f && h==4.5f);
  assert(horizontal==5 && vertical==6 && textX==7.25f && textY==8.75f);
  assert(owner==9 && flags==10 && special==11.5f && f==&font && scale==.75f);
  assert(c==color && material==(void*)77 && style==13 && textAlign==14);
  auto mantle=convertHook(safeHook("MANTLE"),"Space");
  assert(!strcmp(mantle,"^3 SPACE ^7키를 눌러  "));
  assert(widthHook(mantle,0,f,scale)==61);
  NativeHudCaption::Plain(mantle,99,&font,300,40,1,1,0,color,0);
  assert(actual==mantle); // prelocalized text is not translated a second time
  auto swap=convertHook(safeHook("SWAP"),"Left Mouse or Middle Mouse");
  assert(!strcmp(swap,"^3 마우스 왼쪽 또는 마우스 가운데 ^7키를 눌러 교체:"));
  const int promptWidth=widthHook(swap,0,f,scale);
  const int weaponWidth=widthHook("Bizon Holographic",0,f,scale);
  assert(promptWidth==61 && weaponWidth==90);
  const float start=1000-(promptWidth+weaponWidth)*.5f;
  NativeHudCaption::Plain(swap,99,&font,start,40,1,1,0,color,0);
  assert(actualX==924.5f);
  NativeHudCaption::Plain("Bizon Holographic",99,&font,start+promptWidth,40,1,1,0,color,0);
  assert(actualX==985.5f && actual=="Bizon Holographic");
  // Another owner expands [{+attack}] after Korean localization. Its binding
  // names must be completed before native measurement as well as drawing.
  auto sentry=std::string(safeHook("SENTRY_PLACE"));
  const auto alias=sentry.find("[{+attack}]");assert(alias!=std::string::npos);
  sentry.replace(alias,11,"Left Mouse");
  const auto expected="^3마우스 왼쪽^7 키를 눌러 포탑을 설치합니다.";
  widthHook(sentry.c_str(),0,f,scale);assert(measured==expected);
  NativeHudCaption::Plain(sentry.c_str(),99,&font,250,40,1,1,0,color,0);
  assert(actual==expected && actualX==250);
  NativeHudCaption::Plain(expected,99,&font,250,40,1,1,0,color,0);
  assert(actual==expected); // completion remains idempotent
}
static void __fastcall Header(int client,void* rect,void* f,float scale,float* c,int style){
  assert(client==0 && rect==(void*)77 && f==&font && scale==.75f && c==color && style==4);
  const auto text=safeHook("STATUS");
  assert(!strcmp(text,"목표 완료."));
  assert(widthHook(text,0,f,scale)==61 && measured==text);
}
static void __fastcall List(int client,void* rect,void* f,float scale,float* c,int style){
  Header(client,rect,f,scale,c,style);
  const auto first=localizeHook("ANY_UNCLASSIFIED_KEY","objective",7);
  const auto second=localizeHook("STATUS","objective",7);
  assert(!strcmp(first,"해변을 방어하십시오.") && !strcmp(second,"목표 완료."));
  assert(widthHook(first,0,f,scale)==61 && measured==first);
  NativeHudCaption::Plain(first,99,&font,123.5f,40,1,1,0,color,0);
  assert(actual==first && actualX==123.5f);
}
void* GameAddresses::Resolve(uintptr_t,uintptr_t id) {
  if(id==IW6Offsets::NativeOwnerDraw_SP)return (void*)Owner;
  if(id==IW6Offsets::UI_ReplaceConversionString_SP)return (void*)Convert;
  if(id==IW6Offsets::NativeActorNames_SP)return (void*)Names;
  if(id==IW6Offsets::NativeObjectiveHeader_SP)return (void*)Header;
  if(id==IW6Offsets::NativeObjectiveList_SP)return (void*)List;
  if(id==IW6Offsets::IntroTextLayout_SP)return (void*)Localize;
  if(id==IW6Offsets::UI_SafeTranslateString_SP)return (void*)Safe;
  if(id==IW6Offsets::NativeHudTextDraw_SP)return (void*)Draw;
  if(id==IW6Offsets::NativeHudTextFx_SP)return (void*)Fx;
  if(id==IW6Offsets::CG_DrawHudElem_SP)return (void*)Prepare;
  assert(id==IW6Offsets::UI_TextWidth_SP);return (void*)ScaledWidth;
}
bool Detour::Install(void* target,void* replacement,void** original,int) {
  *original=target;
  if(target==(void*)Owner)ownerHook=(NativeHudCaption::OwnerDraw)replacement;
  else if(target==(void*)Convert)convertHook=(NativeHudCaption::Convert)replacement;
  else if(target==(void*)Names)namesHook=(NativeHudCaption::Names)replacement;
  else if(target==(void*)Header)headerHook=(NativeGlyphBridge::Layout)replacement;
  else if(target==(void*)List)listHook=(NativeGlyphBridge::Layout)replacement;
  else if(target==(void*)Localize)localizeHook=(NativeHudCaption::Localize)replacement;
  else if(target==(void*)Safe)safeHook=(NativeHudCaption::SafeTranslate)replacement;
  else if(target==(void*)Draw)drawHook=(NativeHudCaption::Draw)replacement;
  else if(target==(void*)Fx)fxHook=(NativeHudCaption::Fx)replacement;
  else if(target==(void*)Prepare)prepareHook=(NativeHudCaption::Prepare)replacement;
  else {assert(target==(void*)ScaledWidth);widthHook=(NativeGlyphBridge::ScaledWidth)replacement;}
  return true;
}
const char* Detour::ActiveBackendName(){return "test";}
int main() {
  g_R_TextWidth=TextWidth;Original_R_AddCmdDrawText=Producer;
  NativeHudCaption::Initialize();assert(TextHook_UsesNativeGameplayText());
  ownerHook(0,(void*)73,1.25f,2.5f,3.75f,4.5f,5,6,7.25f,8.75f,9,10,11.5f,&font,.75f,color,(void*)77,13,14);
  assert(!NativeHudCaption::current && !measuringKorean);
  assert(!strcmp(convertHook("Press &&1 to use","Left Mouse"),"Press Left Mouse to use"));
  namesHook(0);assert(!NativeHudCaption::current);
  headerHook(0,(void*)77,&font,.75f,color,4);
  listHook(0,(void*)77,&font,.75f,color,4);
  assert(!NativeHudCaption::current && !measuringKorean);
  assert(!strcmp(safeHook("STATUS"),"Objective Completed."));
  assert(!strcmp(localizeHook("STATUS","objective",7),"Objective Completed."));
  alignas(8) unsigned char element[0xa8]{};
  configs[36]="PLATFORM_HOLD_TO_USE";
  *reinterpret_cast<unsigned*>(element)=1;
  *reinterpret_cast<unsigned*>(element+0x84)=36;
  drawHook(0,"Press ^3G^7 to use",(uintptr_t)element,(void*)13);
  assert(actual=="^3G^7 키를 눌러 사용" && actualX==400);
  *reinterpret_cast<unsigned*>(element+0x84)=0;
  drawHook(0,"Defend the beach.",(uintptr_t)element,(void*)13);
  assert(actual=="해변을 방어하십시오."); // actual text draw, no key-prefix routing
  for(unsigned origin=0;origin<3;++origin) {
    *reinterpret_cast<unsigned*>(element+0x28)=origin<<2;
    drawHook(0,"to reach.",(uintptr_t)element,(void*)13);
    assert(actual=="손을 뻗으십시오." && actualX==400);
  }
  assert(widthHook("to reach.",0,&font,.75f)==90 && measured=="to reach."); // other UI untouched
  textToPrepare="to reach.";
  prepareHook(0,(uintptr_t)element,(void*)13,(void*)14);
  assert(measured=="손을 뻗으십시오." && measuredWidth==61 && !NativeHudCaption::current);
  drawHook(0,textToPrepare,(uintptr_t)element,(void*)13);
  assert(actual==measured && actualX==preparedX && actualX==287.25f);
  assert(!NativeHudCaption::current);
  drawHook(0,"native F",(uintptr_t)element,(void*)13);assert(actual=="native F");
  drawHook(0,"Dialogue.",(uintptr_t)element,(void*)13);assert(actual=="Dialogue.");
  drawHook(0,"Move!",(uintptr_t)element,(void*)13);assert(actual=="이동하십시오!");
  NativeHudCaption::Plain("Move!",99,&font,30,40,1,1,0,color,0);assert(actual=="Move!");
  drawHook(0,"Collect 2 of 7 items.",(uintptr_t)element,(void*)13);assert(actual=="7개 중 2개 회수.");
  drawHook(0,"Shared",(uintptr_t)element,(void*)13);assert(actual=="Shared");
  configs[5]="AMBIG_B";*reinterpret_cast<unsigned*>(element+0x84)=5;
  drawHook(0,"Shared",(uintptr_t)element,(void*)13);assert(actual=="문맥 나");
  NativeHudCaption::Plain("Objective Completed.",99,&font,30,40,1,1,0,color,0);
  assert(actual=="목표 완료." && actualX==30);
  assert(NativeHudCaption::catalog.Translate("Press ^3\x11^7 to use")=="^3\x11^7 키를 눌러 사용");
  assert(NativeHudCaption::catalog.Translate("Press ^3[{+activate}]^7 to use").empty());
  assert(NativeHudCaption::catalog.Translate("Defend the beach. Extra text").empty());
  assert(NativeHudCaption::catalog.Translate("June 15th - 0:41:17","INTRO_TIME")=="6월 15일 - 0:41:17");
  assert(NativeHudCaption::catalog.Translate("Power down in 0:59").empty());
  configs[7]="NATIVE_LABEL";*reinterpret_cast<unsigned*>(element+0x40)=7;
  textToPrepare="Power down in 0:59";
  prepareHook(0,(uintptr_t)element,(void*)13,(void*)14);
  assert(measured=="전원 차단까지 0:59");
  drawHook(0,textToPrepare,(uintptr_t)element,(void*)13);
  assert(actual==measured && actualX==preparedX);
  textToPrepare="to reach.";
  glyphsReady=false;
  assert(TextHook_UsesNativeGameplayText());
  assert(TextHook_UsesNativeHudCaption("PLATFORM_HOLD_TO_USE"));
  NativeHudCaption::Plain("Objective Completed.",99,&font,30,40,1,1,0,color,0);
  assert(actual=="Objective Completed."); // resource reset never reactivates old queues
  prepareHook(0,(uintptr_t)element,(void*)13,(void*)14);
  assert(measured=="to reach." && measuredWidth==90 && !NativeHudCaption::current);
  std::cout<<"PASS: production native draw/FX/plain paths, all 17 original effect arguments, native anchors, generic keys, dynamic bindings/values, ambiguity, separate dialogue\n";
}
