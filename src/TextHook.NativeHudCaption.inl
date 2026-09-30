#include "NativeTextCatalog.h"
#include "NativeGlyphBridge.h"
#include "BindingResolver.h"
#include <cstring>
#include <deque>
namespace NativeHudCaption {
using Draw=void(__fastcall *)(int,const char*,uintptr_t,void*);
using Prepare=void(__fastcall *)(int,uintptr_t,void*,void*);
using Fx=void(__fastcall *)(const char*,int,Font_s*,float,float,float,float,float,
    float*,int,float*,void*,void*,int,int,int,int);
static Draw originalDraw=nullptr;
static Fx originalFx=nullptr;
static Prepare originalPrepare=nullptr;
static NativeGlyphBridge::ScaledWidth originalScaledWidth=nullptr;
using Names=void(__fastcall *)(int);
using Localize=const char*(__fastcall *)(const char*,const char*,int);
using SafeTranslate=const char*(__fastcall *)(const char*);
using Convert=const char*(__fastcall *)(const char*,const char*);
using OwnerDraw=void(__fastcall *)(int,void*,float,float,float,float,int,int,
    float,float,int,int,float,void*,float,float*,void*,int,int);
static Names originalNames=nullptr;
static Localize originalLocalize=nullptr;
static SafeTranslate originalSafeTranslate=nullptr;
static Convert originalConvert=nullptr;
static OwnerDraw originalOwnerDraw=nullptr;
static NativeGlyphBridge::Layout originalObjectiveHeader=nullptr,originalObjectiveList=nullptr;
static std::atomic<bool> ready{false};
static NativeTextCatalog::Catalog catalog;
struct Context {
  uintptr_t element=0;std::string key,label;bool measuring=false;
  bool names=false,layout=false;
  mutable std::deque<std::string> strings;
};
static thread_local const Context *current=nullptr;
struct ContextScope {
  const Context *previous=current;
  ContextScope(const Context &context){current=&context;}
  ~ContextScope(){current=previous;}
};
static uint32_t ReadConfig(uintptr_t element,size_t offset) {
  __try {return element?*reinterpret_cast<const uint32_t*>(element+offset):0;}
  __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
static std::string ConfigText(uint32_t cfg) {
  char text[1024]{};
  return cfg && TryReadNativeHudConfigText(cfg,text)?text:std::string{};
}
static void __fastcall DrawText(int client,const char *text,uintptr_t element,void *renderContext) {
  Context context{element,ConfigText(ReadConfig(element,0x84)),ConfigText(ReadConfig(element,0x40))};
  ContextScope scope(context);
  if(!context.key.empty() || !context.label.empty())
    NativeMenuCapture::TraceSource(5,text && *text?text:"(empty)",nullptr,context.key.c_str());
  originalDraw(client,text,element,renderContext);
}
static bool IsReady() {return ready.load(std::memory_order_acquire) && NativeGlyphBridge::IsReady();}
static bool IsDialogue(const char *text) {return text && catalog.IsDialogue(text);}
static bool OwnsHudDraw() {return current && !current->measuring;}
static std::string Translate(const char *text,std::string &key) {
  if(!text || !*text || !IsReady() || (current && current->names) || (!current && IsDialogue(text)))return {};
  struct Cached {std::string text,key;};
  static thread_local std::unordered_map<std::string,Cached> cache;
  std::string identity=current?current->key:std::string{};
  identity.push_back('\0');
  if(current)identity+=current->label;
  identity.push_back('\0');identity+=text;
  const auto found=cache.find(identity);
  if(found!=cache.end()) {key=found->second.key;return found->second.text;}
  // A prelocalized native owner may expand its binding aliases afterwards.
  // Complete only binding slots in a matching Korean template. Ordinary
  // Korean text must never run through English message translation again.
  const bool alreadyKorean=ContainsKorean(text);
  auto result=alreadyKorean
      ? catalog.LocalizeRenderedBindings(text,current?current->key:std::string{},&key)
      : catalog.Translate(text,current?current->key:std::string{},&key);
  if(result.empty() && current && !current->label.empty()) {
    result=alreadyKorean?catalog.LocalizeRenderedBindings(text,current->label,&key)
                        :catalog.Translate(text,current->label,&key);
    if(result.empty() && !alreadyKorean)result=catalog.TranslateNativeLabel(text,current->label,&key);
  }
  if(!result.empty() && !ContainsKorean(result.c_str()))result.clear();
  if(cache.size()>=2048)cache.clear();
  cache.emplace(std::move(identity),Cached{result,key});
  return result;
}
static void __fastcall DrawNames(int client) {
  Context context;context.names=true;ContextScope scope(context);
  originalNames(client);
}
static const char *TranslateLayout(const char *english,const char *source) {
  if(!current || !current->layout || !IsReady() || !english)return english;
  std::string key;
  auto korean=catalog.Translate(english,source?source:"",&key);
  if(korean.empty() && source) {
    korean=catalog.OwnedTemplate(english,source);
    if(!korean.empty())key=source;
  }
  if(korean.empty())return english;
  NativeMenuCapture::TraceSource(10,english,korean.c_str(),key.c_str());
  current->strings.push_back(std::move(korean));
  return current->strings.back().c_str();
}
static const char *__fastcall LocalizeMessage(const char *text,const char *label,int flags) {
  return TranslateLayout(originalLocalize(text,label,flags),text);
}
static const char *__fastcall SafeTranslateString(const char *text) {
  return TranslateLayout(originalSafeTranslate(text),text);
}
static const char *__fastcall ConvertString(const char *text,const char *value) {
  // The native owner has already selected and localized this exact template.
  // Preserve the actual assigned key; only localize its display name.
  std::string localized;
  if(current && current->layout && IsReady() && text && value && ContainsKorean(text))
    localized=BindingResolver::LocalizeNativeBindingDisplay(value);
  return originalConvert(text,localized.empty()?value:localized.c_str());
}
static void __fastcall DrawOwner(int client,void *window,float x,float y,float width,float height,
    int horizontal,int vertical,float textX,float textY,int owner,int flags,float special,
    void *font,float scale,float *color,void *material,int style,int textAlign) {
  Context context;context.layout=true;context.measuring=true;ContextScope scope(context);
  // 23C6D0 (called by 446E1C) is the native owner-draw dispatcher. Its mantle,
  // pickup and other branches retain visibility tests, animation and icon
  // placement, using the same translated text for their width and draw calls.
  originalOwnerDraw(client,window,x,y,width,height,horizontal,vertical,textX,textY,
      owner,flags,special,font,scale,color,material,style,textAlign);
}
static void DrawObjective(NativeGlyphBridge::Layout original,int client,void *rect,void *font,float scale,float *color,int style) {
  Context context;context.layout=true;context.measuring=true;ContextScope scope(context);
  // Translate before the native UTF-8 line-break loop and measure those same
  // glyphs. No post-hoc X correction or English character-count clipping.
  NativeGlyphBridge::RenderLayout(original,client,rect,font,scale,color,style);
}
static void __fastcall DrawObjectiveHeader(int client,void *rect,void *font,float scale,float *color,int style) {
  DrawObjective(originalObjectiveHeader,client,rect,font,scale,color,style);
}
static void __fastcall DrawObjectiveList(int client,void *rect,void *font,float scale,float *color,int style) {
  DrawObjective(originalObjectiveList,client,rect,font,scale,color,style);
}
static void __fastcall PrepareText(int client,uintptr_t element,void *renderContext,void *scratch) {
  Context context{element,ConfigText(ReadConfig(element,0x84)),ConfigText(ReadConfig(element,0x40)),true};
  ContextScope scope(context);
  // 2314F0 measures label/body via 44B1C0 before its own alignment and
  // old-to-new-origin interpolation. Leave the element and render state alone.
  originalPrepare(client,element,renderContext,scratch);
}
static int __fastcall MeasureText(const char *text,int maxChars,void *font,float scale) {
  if(!current || !current->measuring || !font || !std::isfinite(scale))
    return originalScaledWidth(text,maxChars,font,scale);
  std::string key;
  const auto korean=Translate(text,key);
  if(korean.empty() && current->layout && text && ContainsKorean(text) && IsReady())
    return NativeGlyphBridge::TextWidthScaled(originalScaledWidth,text,maxChars,font,scale);
  if(korean.empty())return originalScaledWidth(text,maxChars,font,scale);
  const int width=NativeGlyphBridge::TextWidthScaled(originalScaledWidth,korean.c_str(),maxChars,font,scale);
  NativeMenuCapture::TraceSource(9,text,korean.c_str(),key.c_str(),maxChars,(float)width,scale);
  return width;
}
static void __fastcall DrawFx(const char *text,int maxChars,Font_s *font,float x,float y,
    float sx,float sy,float rotation,float *color,int style,float *glow,void *fxMaterial,
    void *fxGlow,int birth,int letterTime,int decayStart,int decayDuration) {
  std::string key;
  const auto korean=Translate(text,key);
  const bool replace=!korean.empty() && font && color && std::isfinite(sx) && std::isfinite(sy);
  if(replace || (text && std::strpbrk(text,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")))
    NativeMenuCapture::TraceSource(6,text && *text?text:"(empty)",replace?korean.c_str():nullptr,key.c_str(),maxChars,x,y);
  // Keep all seventeen arguments, including real typewriter/decay materials
  // and times. The native backend interprets UTF-8 and animates each glyph.
  originalFx(replace?korean.c_str():text,maxChars,font,x,y,sx,sy,rotation,color,style,
      glow,fxMaterial,fxGlow,birth,letterTime,decayStart,decayDuration);
}
static void Plain(const char *text,int maxChars,Font_s *font,float x,float y,
    float sx,float sy,float rotation,float *color,int style) {
  std::string key;
  const auto korean=Translate(text,key);
  const bool replace=!korean.empty() && font && color;
  if(replace || (text && std::strpbrk(text,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")))
    NativeMenuCapture::TraceSource(7,text,replace?korean.c_str():nullptr,key.c_str(),maxChars,x,y);
  Original_R_AddCmdDrawText(replace?korean.c_str():text,maxChars,font,x,y,sx,sy,rotation,color,style);
}
static void Initialize() {
  static std::once_flag once;
  std::call_once(once,[] {
    EnsureTranslationsLoaded();
    catalog.Build(TranslationStore::KeyToEnglish(),TranslationStore::KeyToKorean(),BindingResolver::LocalizeNativeBindingDisplay);
    const auto base=(uintptr_t)GetModuleHandleW(nullptr);
    const auto draw=IW6Offsets::GetAddress(base,IW6Offsets::NativeHudTextDraw_SP);
    const auto fx=IW6Offsets::GetAddress(base,IW6Offsets::NativeHudTextFx_SP);
    const auto prepare=IW6Offsets::GetAddress(base,IW6Offsets::CG_DrawHudElem_SP);
    const auto width=IW6Offsets::GetAddress(base,IW6Offsets::UI_TextWidth_SP);
    const auto names=IW6Offsets::GetAddress(base,IW6Offsets::NativeActorNames_SP);
    const auto objectiveHeader=IW6Offsets::GetAddress(base,IW6Offsets::NativeObjectiveHeader_SP);
    const auto objectiveList=IW6Offsets::GetAddress(base,IW6Offsets::NativeObjectiveList_SP);
    const auto localize=IW6Offsets::GetAddress(base,IW6Offsets::IntroTextLayout_SP);
    const auto safeTranslate=IW6Offsets::GetAddress(base,IW6Offsets::UI_SafeTranslateString_SP);
    const auto ownerDraw=IW6Offsets::GetAddress(base,IW6Offsets::NativeOwnerDraw_SP);
    const auto convert=IW6Offsets::GetAddress(base,IW6Offsets::UI_ReplaceConversionString_SP);
    if(!draw || !fx || !prepare || !width || !names || !objectiveHeader || !objectiveList || !localize || !safeTranslate || !ownerDraw || !convert || !Original_R_AddCmdDrawText || !NativeMenuDraw::Initialize() || !NativeGlyphBridge::Initialize())return;
    if(!CreateHook(ownerDraw,(void*)DrawOwner,(void**)&originalOwnerDraw))return;
    if(!CreateHook(convert,(void*)ConvertString,(void**)&originalConvert))return;
    if(!CreateHook(names,(void*)DrawNames,(void**)&originalNames))return;
    if(!CreateHook(localize,(void*)LocalizeMessage,(void**)&originalLocalize))return;
    if(!CreateHook(safeTranslate,(void*)SafeTranslateString,(void**)&originalSafeTranslate))return;
    if(!CreateHook(objectiveHeader,(void*)DrawObjectiveHeader,(void**)&originalObjectiveHeader))return;
    if(!CreateHook(objectiveList,(void*)DrawObjectiveList,(void**)&originalObjectiveList))return;
    if(!CreateHook(width,(void*)MeasureText,(void**)&originalScaledWidth))return;
    if(!CreateHook(prepare,(void*)PrepareText,(void**)&originalPrepare))return;
    if(!CreateHook(fx,(void*)DrawFx,(void**)&originalFx))return;
    if(!CreateHook(draw,(void*)DrawText,(void**)&originalDraw))return;
    ready.store(true,std::memory_order_release);
  });
}
}
bool TextHook_UsesNativeGameplayText() {
  // This build selects the native path permanently. Missing hooks or graphics
  // resources keep the original English; they never select lookup-driven
  // queues, guessed positions or persistent suppression as a fallback.
  return true;
}
bool TextHook_UsesNativeHudCaption(const std::string &keyOrText) {
  // Compatibility for old callers while the legacy capture code is removed.
  if(!TextHook_UsesNativeGameplayText())return false;
  return keyOrText.compare(0,9,"SUBTITLE_")!=0 && keyOrText.compare(0,13,"VIDSUBTITLES_")!=0;
}
