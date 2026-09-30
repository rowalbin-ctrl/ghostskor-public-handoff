#include "KoreanRenderer.h"
#include "NativeMenuDraw.h"
#include "NativeCommandRegistry.h"
#include "NativeGlyphBridge.h"
#include "GameBuild.h"
#include "IW6Offsets.h"
#include "MenuTrace.h"
#include "Utils.h"
#include <atomic>
#include <memory>
#include <mutex>

namespace {
using Backend = void(__fastcall *)(const unsigned char**);
using EndSurface = void(__fastcall *)(void*);
Backend originalBackend = nullptr;
EndSurface endSurface = nullptr;
const volatile unsigned *tessIndexCount = nullptr;
std::atomic<bool> ready{false};
thread_local NativeMenuDraw::TextScope *current = nullptr;
NativeCommandRegistry<std::vector<DrawCommand>> batches;
std::mutex batchMutex;

void __fastcall RenderNativeTextCommand(const unsigned char **iterator) {
  const auto command = *iterator;
  const unsigned size = *reinterpret_cast<const unsigned short*>(command + 2);
  uint64_t id = 0;
  if (size < 96 || !NativeCommandRegistry<std::vector<DrawCommand>>::ReadMarker(
          reinterpret_cast<const char*>(command + 92), size - 92, id)) {
    NativeGlyphBridge::RenderBackend(originalBackend,iterator);
    return;
  }
  // The stock handler (5A50E0) advances by the uint16 size at +2. The marker
  // is private to this dispatcher and must never reach the native glyph loop.
  *iterator = command + size;
  std::shared_ptr<const std::vector<DrawCommand>> batch;
  {
    std::lock_guard<std::mutex> lock(batchMutex);
    batch = batches.Find(id);
  }
  if (!batch) return;
  // Native UI pictures/text are batched. Flush prior native geometry BEFORE
  // our draw, otherwise a queued background can cover the replacement later.
  if (*tessIndexCount) endSurface(nullptr);
  KoreanRenderer::RenderNativeBatch(*batch);
  NativeMenuCapture::Trace(20, nullptr, (float)batch->size());
}
}

#ifdef GHOSTSKOR_TESTING
void NativeMenuDraw_SetTessCounterForTesting(const volatile unsigned *counter) {
  tessIndexCount = counter;
}
#endif

bool NativeMenuDraw::Initialize() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!GameBuild::RuntimeReady()) return;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto target = IW6Offsets::GetAddress(base, IW6Offsets::RB_DrawTextCommand_SP);
    endSurface = reinterpret_cast<EndSurface>(IW6Offsets::GetAddress(base, IW6Offsets::RB_EndTessSurface_SP));
    if (!target || !endSurface) return;
    tessIndexCount = reinterpret_cast<const volatile unsigned*>(base + IW6Offsets::TessIndexCount_SP);
    if (!CreateHook(target, (void*)RenderNativeTextCommand, (void**)&originalBackend)) return;
    ready.store(true, std::memory_order_release);
    LogToFile("[NativeMenu] Korean text uses the engine's ordered UI command stream.");
  });
  return ready.load(std::memory_order_acquire);
}

void NativeMenuDraw::BeginFrame() {
  std::lock_guard<std::mutex> lock(batchMutex);
  batches.BeginFrame();
}
bool NativeMenuDraw::IsCapturing() { return current != nullptr; }
void NativeMenuDraw::FlushNativeGeometry() {
  if (tessIndexCount && *tessIndexCount && endSurface) endSurface(nullptr);
}
bool NativeMenuDraw::Queue(DrawCommand &command) {
  if (!current || !command.nativeText.valid || !KoreanRenderer::s_bInitialized) return false;
  current->commands.push_back(std::move(command));
  return true;
}

NativeMenuDraw::TextScope::TextScope(Producer original, void *font, float x,
    float y, float sx, float sy, float rotation, int style, bool nativeLayout)
    : previous(current), producer(original), font(font), x(x), y(y), sx(sx),
      sy(sy), rotation(rotation), style(style) {
  active = nativeLayout && original && ready.load(std::memory_order_acquire);
  current = active ? this : nullptr;
}
NativeMenuDraw::TextScope::~TextScope() {
  current = previous;
  if (!active || commands.empty()) return;
  uint64_t id;
  {
    std::lock_guard<std::mutex> lock(batchMutex);
    id = batches.Add(std::move(commands));
  }
  char marker[32];
  NativeCommandRegistry<std::vector<DrawCommand>>::WriteMarker(marker, sizeof(marker), id);
  float white[] = {1, 1, 1, 1};
  // Append at this draw's position, before subsequent native pictures. The
  // normal suppressed-space command remains harmless and needs no ABI change.
  producer(marker, 0x7fffffff, font, x, y, sx, sy, rotation, white, style);
}
