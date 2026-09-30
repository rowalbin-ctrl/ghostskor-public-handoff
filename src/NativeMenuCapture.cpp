#include "NativeMenuCapture.h"
#include "NativeMenuDraw.h"
#include "MenuTrace.h"
#include "GameBuild.h"
#include "IW6Offsets.h"
#include "KoreanRenderer.h"
#include "Utils.h"
#include "IW6Font.h"
#include "KoreanAtlas.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {
using FrameFn = void(__fastcall *)();
using ScissorFn = void(__fastcall *)(int, int, int, int);
FrameFn originalBegin = nullptr, originalEnd = nullptr, originalReset = nullptr;
ScissorFn originalScissor = nullptr;
std::atomic<bool> ready{false};
thread_local bool frameActive = false;
thread_local MenuClipRect clip;
struct TextOwner {
  bool valid = false;
  int alignment = 0;
  float width = 0, height = 0;
  int anchorAlignment = -1; // resolved single-line layout; -1 means no anchor
};
thread_local TextOwner owner;
thread_local NativeTextLayout currentText;
thread_local NativeMenuTextRun *currentRun = nullptr;
struct CachedOwner { void *command; TextOwner layout; std::shared_ptr<const std::string> source; bool joinedLiterals; };
thread_local std::unordered_map<void*, CachedOwner> cachedOwners;
using RenderTextFn = void(__fastcall *)(void*, float, float, float, float,
    float, float, const char*, void*, float, float, int, void*);
using CachedFn = bool(__fastcall *)(void*, void*);
using WidthFn = int(__fastcall *)(const char*, int, void*);
RenderTextFn originalText = nullptr;
CachedFn originalCached = nullptr;
WidthFn textWidth = nullptr;

// Steam 24723416 LUIElement layout, verified in both text callbacks and the
// cached dispatcher: current alignment +0x70, resolved rectangle +0xF0..0xFC.
// LUI text layout 20D7E0 positions alignment-0 text by its left/right anchors
// (+6C/+6E) before RenderText. Its bounds helper 1FD770 uses the English width
// only for this branch; explicit text alignment uses a fixed rectangle width.
TextOwner ReadOwner(void *element) {
  if (!element) return {};
  auto p = static_cast<const unsigned char*>(element);
  TextOwner result;
  result.valid = true;
  result.alignment = *reinterpret_cast<const int*>(p + 0x70);
  if ((*reinterpret_cast<const unsigned*>(p + 0x10) & 0x40) != 0) {
    const bool left = p[0x6C] != 0, right = p[0x6E] != 0;
    result.anchorAlignment = left == right ? 2 : left ? 0 : 1;
  }
  // Alignment 0 uses the unbounded, single-line branch (20FBB5). The
  // rectangle's width is NOT necessarily the width passed to RenderText.
  result.width = result.alignment == 0 ? -1.0f :
      *reinterpret_cast<const float*>(p + 0xF8) -
      *reinterpret_cast<const float*>(p + 0xF0);
  result.height = *reinterpret_cast<const float*>(p + 0xFC) -
                  *reinterpret_cast<const float*>(p + 0xF4);
  return result;
}
struct OwnerScope {
  TextOwner previous = owner;
  explicit OwnerScope(TextOwner value) { owner = value; }
  ~OwnerScope() { owner = previous; }
};
struct RunScope {
  NativeMenuTextRun run;
  NativeMenuTextRun *previous = currentRun;
  explicit RunScope(std::shared_ptr<const std::string> source, bool joined = false)
      : run(std::move(source), joined) { currentRun = &run; }
  ~RunScope() { currentRun = previous; }
};
void TraceOwner(void *element) {
#if GHOSTSKOR_MENU_TRACE
  if (!frameActive || !element) return;
  char address[32];
  sprintf_s(address, "%p", element);
  const auto p = static_cast<const unsigned char*>(element);
  NativeMenuCapture::Trace(17, address,
      *reinterpret_cast<const float*>(p + 0xF0),
      *reinterpret_cast<const float*>(p + 0xF4),
      *reinterpret_cast<const float*>(p + 0xF8),
      *reinterpret_cast<const float*>(p + 0xFC));
#endif
}
void __fastcall RenderText(void *element, float x, float y, float r, float g,
    float b, float a, const char *text, void *font, float height, float width,
    int alignment, void *context) {
  TextOwner layout = ReadOwner(element);
  layout.valid = true;
  layout.alignment = alignment;
  layout.width = width;
  layout.height = height;
  OwnerScope scope(layout);
  std::string joined = NativeMenuTextRun::DecodeLiteralJoin(text);
  const bool isJoined = !joined.empty();
  RunScope textRun(isJoined ? std::make_shared<const std::string>(std::move(joined)) :
      text && strpbrk(text, "\r\n") ? std::make_shared<const std::string>(text) : nullptr, isJoined);
  NativeMenuCapture::TraceSource(1, text);
  TraceOwner(element);
  originalText(element, x, y, r, g, b, a, text, font, height, width, alignment, context);
  if (frameActive && element) {
    const auto command = *reinterpret_cast<void**>(static_cast<unsigned char*>(element) + 0xD0);
    // Keep layout and the complete source for the exact engine cache. Never retain translated text,
    // draw commands, visibility, or screen coordinates between engine frames.
    if (cachedOwners.size() >= 4096) cachedOwners.clear();
    cachedOwners[element] = {command, owner, textRun.run.source, textRun.run.joinedLiterals};
  }
}
bool __fastcall RenderCached(void *element, void *context) {
  TextOwner layout = ReadOwner(element);
  std::shared_ptr<const std::string> source;
  bool joinedLiterals = false;
  if (element) {
    const auto command = *reinterpret_cast<void**>(static_cast<unsigned char*>(element) + 0xD0);
    const auto found = cachedOwners.find(element);
    if (found != cachedOwners.end() && found->second.command == command) {
      const int currentAnchor = layout.anchorAlignment;
      layout = found->second.layout;
      layout.anchorAlignment = currentAnchor;
      source = found->second.source;
      joinedLiterals = found->second.joinedLiterals;
    }
  }
  OwnerScope scope(layout);
  RunScope textRun(std::move(source), joinedLiterals);
  TraceOwner(element);
  return originalCached(element, context);
}

void __fastcall Begin() {
  NativeMenuCapture::Trace(1);
  originalBegin(); // waits for the previous buffer before reusing it
  NativeMenuDraw::BeginFrame();
  clip = {};
  frameActive = ready.load(std::memory_order_acquire);
  if (frameActive) KoreanRenderer::BeginMenuFrame();
  NativeMenuCapture::Trace(2);
}
void __fastcall End() {
  NativeMenuCapture::Trace(3);
  if (frameActive) KoreanRenderer::EndMenuFrame();
  frameActive = false;
  clip = {};
  originalEnd();
  NativeMenuCapture::Trace(4);
}
void __fastcall Scissor(int x, int y, int width, int height) {
  originalScissor(x, y, width, height);
  clip = {true, (float)x, (float)y, (float)x + (float)width,
          (float)y + (float)height};
  NativeMenuCapture::Trace(5);
}
void __fastcall Reset() {
  originalReset();
  clip = {};
  NativeMenuCapture::Trace(6);
}
}

bool NativeMenuCapture::Initialize() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!GameBuild::RuntimeReady()) return;
    const auto base = (uintptr_t)GetModuleHandleW(nullptr);
    textWidth = reinterpret_cast<WidthFn>(IW6Offsets::GetAddress(base, IW6Offsets::R_TextWidth_SP));
    if (!textWidth) return;
    struct Hook { uintptr_t id; void *detour; void **original; };
    const Hook hooks[] = {
      {IW6Offsets::LUI_RenderText_SP, (void*)RenderText, (void**)&originalText},
      {IW6Offsets::LUI_RenderCachedElement_SP, (void*)RenderCached, (void**)&originalCached},
      {IW6Offsets::R_AddCmdSetScissor_SP, (void*)Scissor, (void**)&originalScissor},
      {IW6Offsets::R_AddCmdResetScissor_SP, (void*)Reset, (void**)&originalReset},
      {IW6Offsets::R_FinishRenderCommands_SP, (void*)End, (void**)&originalEnd},
      {IW6Offsets::R_BeginRenderCommands_SP, (void*)Begin, (void**)&originalBegin},
    };
    // Trampolines are published directly to their callback globals before
    // MinHook resumes game threads. Partial installation never enables capture.
    for (const auto &h : hooks) {
      void *target = IW6Offsets::GetAddress(base, h.id);
      if (!target || !CreateHook(target, h.detour, h.original)) {
        LogToFile("[NativeMenu] Capture unavailable; retaining legacy menu path.");
        return;
      }
    }
    NativeMenuDraw::Initialize();
    ready.store(true, std::memory_order_release);
    LogToFile("[NativeMenu] Engine scissor and complete-frame capture enabled.");
  });
  return ready.load(std::memory_order_acquire);
}

bool NativeMenuCapture::IsFrameActive() { return frameActive; }
MenuClipRect NativeMenuCapture::CurrentClip() { return clip; }
NativeTextLayout NativeMenuCapture::CurrentText() { return currentText; }
NativeMenuTextRun *NativeMenuCapture::CurrentTextRun() { return currentRun; }

NativeMenuCapture::TextScope::TextScope(const char *text, int maxChars, void *font,
    float x, float y, float xScale, float yScale) : previous(currentText) {
  currentText = {};
  if (!frameActive || !owner.valid || !font || !textWidth) return;
  const auto f = static_cast<const IW6Font::Font_s*>(font);
  // +8 is pixelHeight, despite the historical field name in IW6Font.h.
  const int height = f->glyphCount;
  if (height <= 0 || height > 512 || !std::isfinite(xScale) ||
      !std::isfinite(yScale) || xScale <= 0 || yScale <= 0) return;
  currentText.valid = true;
  currentText.x = x; currentText.y = y;
  currentText.width = static_cast<float>(textWidth(text, maxChars, font)) * xScale;
  currentText.scaleX = height * xScale / KoreanAtlas::GLYPH_SIZE_BASE;
  currentText.scaleY = height * yScale / KoreanAtlas::GLYPH_SIZE_BASE;
  currentText.fontPixelHeight = static_cast<float>(height);
  currentText.engineScaleX = xScale; currentText.engineScaleY = yScale;
  // Glyph +3 is signed y0, +6 is unsigned bitmap height (5A15C1/5A17A3).
  // A/H/M have the same cap bounds in the supported native fonts. H avoids
  // punctuation and descenders, and gives a stable size for every UI string.
  if (const auto glyph = IW6Font::FindGlyph(font, 'H')) {
    currentText.capTop = static_cast<float>(glyph->y0);
    currentText.capBottom = currentText.capTop + static_cast<unsigned char>(glyph->pixelHeight);
  }
  currentText.alignment = NativeTextLayout::FromLuiAlignment(owner.alignment);
  if (owner.alignment == 0 && owner.anchorAlignment >= 0)
    currentText.alignment = owner.anchorAlignment;
  if (owner.width > 0 && owner.height > 0)
    currentText.maxWidth = owner.width * height * xScale / owner.height;
  // Follow the engine's font selection, rather than matching translated words.
  if (f->fontName && (strstr(f->fontName, "extraBigFont") || strstr(f->fontName, "bigFont")))
    currentText.atlasSlot = KoreanAtlas::kAtlasSlotHeader;
  NativeMenuCapture::Trace(14, text, (float)owner.alignment, (float)height, xScale, yScale);
  NativeMenuCapture::Trace(15, text, x, y, currentText.width, currentText.maxWidth);
  NativeMenuCapture::Trace(16, text, owner.width, owner.height, currentText.capTop, currentText.capBottom);
}
NativeMenuCapture::TextScope::~TextScope() { currentText = previous; }

#if GHOSTSKOR_MENU_TRACE
// Distinct sources remain readable after menu transitions. No keyboard
// polling, disk logging, or changes to rendering decisions are involved.
struct MenuSourceRecord {
  unsigned stage;
  int maxChars;
  float x, y;
  void *stack[4];
  char key[128], source[2048], translation[2048];
};
static_assert(sizeof(MenuSourceRecord) == 4272, "reader layout");
struct MenuSourceBuffer {
  unsigned magic = 0x4D535231, recordSize = sizeof(MenuSourceRecord), capacity = 1024;
  volatile unsigned count = 0;
  MenuSourceRecord records[1024] = {};
};
extern "C" __declspec(dllexport) MenuSourceBuffer GhostsKor_MenuSources;
MenuSourceBuffer GhostsKor_MenuSources;
void NativeMenuCapture::TraceSource(unsigned stage, const char *source, const char *translation,
                                    const char *key, int maxChars, float x, float y) {
  if (!source || !*source) return;
  static std::mutex guard;
  static std::unordered_set<std::string> seen;
  std::lock_guard<std::mutex> lock(guard);
  auto &buffer = GhostsKor_MenuSources;
  if (buffer.count >= buffer.capacity) return;
  std::string identity = std::to_string(stage) + ":" + std::to_string(maxChars) + ":" + source;
  identity.push_back('\x1f');
  if (key) identity += key;
  identity.push_back('\x1f');
  if (translation) identity += translation;
  if (!seen.insert(std::move(identity)).second) return;
  auto &r = buffer.records[buffer.count];
  r.stage = stage; r.maxChars = maxChars; r.x = x; r.y = y;
  CaptureStackBackTrace(1, 4, r.stack, nullptr);
  if (key) strncpy_s(r.key, key, _TRUNCATE);
  strncpy_s(r.source, source, _TRUNCATE);
  if (translation) strncpy_s(r.translation, translation, _TRUNCATE);
  MemoryBarrier();
  ++buffer.count;
}
#endif

#if GHOSTSKOR_MENU_TRACE
struct MenuTraceRecord {
  unsigned long long sequence;
  unsigned long tick, thread, event, active, clipped, reserved;
  float values[4], rectangle[4];
  void *stack[4];
  char text[64];
  unsigned long long committed;
};
struct MenuTraceBuffer {
  unsigned magic = 0x4d4e5531, recordSize = sizeof(MenuTraceRecord);
  unsigned capacity = 16384, version = 1;
  volatile LONG64 next = 0;
  MenuTraceRecord records[16384] = {};
};
extern "C" __declspec(dllexport) MenuTraceBuffer GhostsKor_MenuTrace;
MenuTraceBuffer GhostsKor_MenuTrace;

void NativeMenuCapture::Trace(unsigned event, const char *text,
                              float a, float b, float c, float d) {
  const auto sequence = InterlockedIncrement64(&GhostsKor_MenuTrace.next);
  auto &r = GhostsKor_MenuTrace.records[(sequence - 1) % 16384];
  r.committed = 0;
  r.sequence = sequence;
  r.tick = GetTickCount();
  r.thread = GetCurrentThreadId();
  r.event = event;
  r.active = frameActive;
  r.clipped = clip.enabled;
  r.values[0] = a; r.values[1] = b; r.values[2] = c; r.values[3] = d;
  r.rectangle[0] = clip.left; r.rectangle[1] = clip.top;
  r.rectangle[2] = clip.right; r.rectangle[3] = clip.bottom;
  CaptureStackBackTrace(1, 4, r.stack, nullptr);
  r.text[0] = 0;
  if (text) strncpy_s(r.text, sizeof(r.text), text, _TRUNCATE);
  MemoryBarrier();
  r.committed = sequence;
}
#endif
