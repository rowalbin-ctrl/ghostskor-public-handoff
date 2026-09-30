#include "GamepadGlyphAtlas.h"
#include "IW6Offsets.h"
#include "Utils.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <windows.h>

namespace GamepadGlyphAtlas {
namespace {
std::mutex captureMutex;
Snapshot active;
Snapshot families[2];
DWORD nextAttempt[2]{};

bool ReadFamily(void* font, bool* playstation) {
  __try {
    if (!font) return false;
    const auto name = *reinterpret_cast<const char**>(font);
    if (!name) return false;
    char buffer[128]{};
    size_t i = 0;
    for (; i < sizeof(buffer)-1 && name[i]; ++i) {
      buffer[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + ('a'-'A') : name[i];
    }
    if (name[i] || std::strncmp(buffer,"fonts/",6)) return false;
    *playstation = std::strstr(buffer,"_playstation") != nullptr;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Verified in live Steam 24723416 and x64-zt's IW6 asset definitions:
// Material.textureCount +0x1c4, textureTable +0x1d8, 16-byte texture entries,
// GfxImage* +8, GfxImage.shaderView +8. No guessed offsets or asset pool scan.
bool ReadAtlas(void* font, Atlas* out) {
  __try {
    const auto f = static_cast<const unsigned char*>(font);
    const int height = *reinterpret_cast<const int*>(f+8);
    const int count = *reinterpret_cast<const int*>(f+12);
    if (height != 30 || count < 16 || count > 2048) return false;
    const auto material = *reinterpret_cast<const unsigned char* const*>(f+16);
    const auto glyphs = *reinterpret_cast<const NativeFontGlyph* const*>(f+32);
    if (!material || !glyphs || material[0x1c4] != 1) return false;
    const auto textures = *reinterpret_cast<const unsigned char* const*>(material+0x1d8);
    if (!textures || textures[7] != 0) return false;
    const auto image = *reinterpret_cast<const unsigned char* const*>(textures+8);
    if (!image) return false;
    auto srv = *reinterpret_cast<ID3D11ShaderResourceView* const*>(image+8);
    if (!srv) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    srv->GetDesc(&view);
    if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return false;
    ID3D11Resource* resource = nullptr;
    srv->GetResource(&resource);
    if (!resource) return false;
    ID3D11Texture2D* texture = nullptr;
    const HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    resource->Release();
    if (FAILED(hr) || !texture) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    texture->Release();
    if (!desc.Width || !desc.Height || desc.Width > 16384 || desc.Height > 16384) return false;
    int buttons = 0;
    for (int i = 0; i < count; ++i) {
      const auto& raw = glyphs[i];
      if (!((raw.letter >= 1 && raw.letter <= 6) || (raw.letter >= 14 && raw.letter <= 23))) continue;
      auto glyph = NormalizeButtonGlyph(raw, height);
      if (!glyph.valid) return false;
      out->glyphs[raw.letter] = glyph;
      ++buttons;
    }
    for (unsigned ch = 1; ch <= 23; ++ch) {
      if ((ch <= 6 || ch >= 14) && !out->glyphs[ch].valid) return false;
    }
    if (buttons != 16) return false;
    srv->AddRef();
    out->srv = srv;
    out->textureWidth = desc.Width;
    out->textureHeight = desc.Height;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

Snapshot Acquire() { return std::atomic_load_explicit(&active, std::memory_order_acquire); }
bool IsReady() { return static_cast<bool>(Acquire()); }
bool TryCapture(void* sourceFont) {
  bool playstation = false;
  if (!ReadFamily(sourceFont,&playstation)) return false;
  std::unique_lock<std::mutex> lock(captureMutex, std::try_to_lock);
  if (!lock.owns_lock()) return IsReady();
  const int family = playstation ? 1 : 0;
  if (families[family]) {
    if (Acquire() != families[family])
      std::atomic_store_explicit(&active, families[family], std::memory_order_release);
    return true;
  }
  const DWORD now = GetTickCount();
  if (nextAttempt[family] && static_cast<LONG>(now-nextAttempt[family]) < 0) return false;
  nextAttempt[family] = now+1000;
  const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  using RegisterFont = void*(*)(const char*,int);
  auto loadFont = reinterpret_cast<RegisterFont>(IW6Offsets::GetAddress(base,IW6Offsets::R_RegisterFont_SP));
  if (!loadFont) return false;
  const char* name = playstation ? "fonts/hudbigfont_playstation" : "fonts/hudbigfont";
  void* font = loadFont(name,0);
  bool actualFamily = false;
  if (!ReadFamily(font,&actualFamily) || actualFamily != playstation) return false;
  auto atlas = std::make_shared<Atlas>();
  if (!ReadAtlas(font,atlas.get())) return false;
  families[family] = atlas;
  std::atomic_store_explicit(&active, families[family], std::memory_order_release);
  char message[192];
  sprintf_s(message,"[NativeGamepad] %s %ux%u: native glyph metrics and 16 pad symbols",name,atlas->textureWidth,atlas->textureHeight);
  LogToFile(message);
  return true;
}
void Shutdown() {
  std::lock_guard<std::mutex> lock(captureMutex);
  std::atomic_store_explicit(&active, Snapshot{}, std::memory_order_release);
  families[0].reset(); families[1].reset();
  nextAttempt[0] = nextAttempt[1] = 0;
}
}
