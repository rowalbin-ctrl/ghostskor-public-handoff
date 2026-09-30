#include "KoreanRenderer.h"
#include "NativeMenuDraw.h"
#include "NativeCommandRegistry.h"
#include "GameBuild.h"
#include "GameAddresses.h"
#include "GameAddressProfile.h"
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>

using Backend = void(*)(const unsigned char**);
static Backend dispatch;
static volatile unsigned pendingGeometry;
static std::vector<std::string> events;
using Command = std::array<unsigned char, 124>;
static std::vector<Command> nativeCommands;
static Command NativeCommand(const char *text) {
  Command cmd{};
  unsigned short id = 17, size = (unsigned short)cmd.size();
  memcpy(cmd.data(), &id, 2); memcpy(cmd.data() + 2, &size, 2);
  strcpy_s(reinterpret_cast<char*>(cmd.data() + 92), 32, text);
  return cmd;
}
static void Original(const unsigned char **iterator) {
  events.push_back("native text");
  unsigned short size;
  memcpy(&size, *iterator + 2, 2);
  *iterator += size;
}
static void Flush(void *argument) {
  assert(!argument && pendingGeometry);
  pendingGeometry = 0; events.push_back("flush prior pictures");
}
static void Producer(const char *text, int count, void *font, float x, float y,
                     float sx, float sy, float rotation, float *color, int style) {
  assert(count == 0x7fffffff && font == (void*)1 && x == 12 && y == 34);
  assert(sx == 2 && sy == 3 && rotation == 0 && color[3] == 1 && style == 3);
  nativeCommands.push_back(NativeCommand(text));
}
bool GameBuild::RuntimeReady() { return true; }
void *GameAddresses::Resolve(uintptr_t, uintptr_t id) {
  return id == IW6Offsets::RB_DrawTextCommand_SP ? (void*)Original :
      id == IW6Offsets::RB_EndTessSurface_SP ? (void*)Flush : nullptr;
}
bool Detour::Install(void *target, void *replacement, void **original, int) {
  assert(target == (void*)Original);
  *original = target; dispatch = (Backend)replacement;
  return true;
}
const char *Detour::ActiveBackendName() { return "test"; }
bool KoreanRenderer::s_bInitialized = true;
void KoreanRenderer::RenderNativeBatch(const std::vector<DrawCommand> &batch) {
  assert(!pendingGeometry);
  for (const auto &cmd : batch) events.push_back(cmd.text);
}
void NativeMenuDraw_SetTessCounterForTesting(const volatile unsigned *);
static DrawCommand Text(const char *text) {
  DrawCommand cmd{}; cmd.text = text; cmd.nativeText.valid = true;
  return cmd;
}
static void Execute(const Command &cmd) {
  const unsigned char *iterator = cmd.data(); dispatch(&iterator);
  assert(iterator == cmd.data() + cmd.size());
}
int main() {
  assert(NativeMenuDraw::Initialize());
  NativeMenuDraw_SetTessCounterForTesting(&pendingGeometry);
  NativeMenuDraw::BeginFrame();
  assert(!NativeMenuDraw::IsCapturing());
  {
    NativeMenuDraw::TextScope scope(Producer, (void*)1, 12, 34, 2, 3, 0, 3, true);
    auto text = Text("background Korean"); assert(NativeMenuDraw::Queue(text));
    assert(nativeCommands.empty()); // appended at the end of the original draw
    {
      NativeMenuDraw::TextScope nested(Producer, (void*)1, 12, 34, 2, 3, 0, 3, false);
      assert(!NativeMenuDraw::IsCapturing());
      auto untouched = Text("fallback"); assert(!NativeMenuDraw::Queue(untouched));
      assert(untouched.text == "fallback");
    }
    assert(NativeMenuDraw::IsCapturing());
  }
  assert(!NativeMenuDraw::IsCapturing() && nativeCommands.size() == 1);
  pendingGeometry = 6;
  Execute(nativeCommands[0]);
  events.push_back("native popup picture");
  {
    NativeMenuDraw::TextScope scope(Producer, (void*)1, 12, 34, 2, 3, 0, 3, true);
    auto a = Text("popup Korean"), b = Text("popup button");
    assert(NativeMenuDraw::Queue(a) && NativeMenuDraw::Queue(b));
  }
  pendingGeometry = 6; Execute(nativeCommands[1]);
  assert((events == std::vector<std::string>{"flush prior pictures", "background Korean",
      "native popup picture", "flush prior pictures", "popup Korean", "popup button"}));
  events.clear(); Execute(nativeCommands[0]);
  assert((events == std::vector<std::string>{"background Korean"})); // valid engine replay
  Execute(NativeCommand("ordinary English"));
  assert(events.back() == "native text");
  for (int i = 0; i < 9; ++i) NativeMenuDraw::BeginFrame();
  events.clear(); Execute(nativeCommands[0]); assert(events.empty()); // stale marker never prints

  using Registry = NativeCommandRegistry<std::string>;
  Registry registry; registry.BeginFrame();
  auto id = registry.Add("held"), next = registry.Add("other");
  auto held = registry.Find(id);
  assert(next > id);
  char marker[32]; Registry::WriteMarker(marker, sizeof(marker), id);
  uint64_t decoded = 0;
  assert(Registry::ReadMarker(marker, 20, decoded) && decoded == id);
  for (size_t size = 0; size < 20; ++size) assert(!Registry::ReadMarker(marker, size, decoded));
  marker[4] = 'Z'; assert(!Registry::ReadMarker(marker, 20, decoded));
  for (int i = 0; i < 9; ++i) registry.BeginFrame();
  assert(!registry.Find(id) && *held == "held"); // consumer owns lifetime after pruning
  assert(registry.Add("new") > next);
  std::cout << "PASS: production ordered dispatcher, prior-picture flush, native/popup order, original ABI and iterator advance, multi-command batches, nested scopes, replay, stale-frame expiry, bounded marker parsing\n";
}
#include "NativeGlyphBridge.h"
void NativeGlyphBridge::RenderBackend(Backend original,const unsigned char** iterator) {original(iterator);}
