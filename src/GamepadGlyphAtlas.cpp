#include "GamepadGlyphAtlas.h"
#include "IW6Offsets.h"
#include "Utils.h"
#include <psapi.h>
#include <windows.h>

#pragma comment(lib, "psapi.lib")

// =============================================================================
// GamepadGlyphAtlas implementation
//
// Walk:  Font_s (arg3 of R_AddCmdDrawText)
//          → .material  (+0x10) → .name (+0x00)
//          → DB_FindXAssetHeader(IMAGE, name, 0) → GfxImage*
//            → .textures.shaderView (+0x08) → ID3D11ShaderResourceView*
//        Font_s → .glyphs (+0x20) / .glyphCount (+0x0C)
//            → per Glyph (24 bytes): letter+0x00, s0-t1+0x08–+0x14
// =============================================================================

namespace GamepadGlyphAtlas {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static ID3D11ShaderResourceView *s_SRV   = nullptr;
static GlyphUV                   s_Glyphs[256] = {};
static bool                      s_Ready = false;

// ---------------------------------------------------------------------------
// POD-safe helper functions (all __try blocks are isolated here so that
// TryCaptureImpl / TryCapture never mix __try with C++ object unwinding)
// ---------------------------------------------------------------------------
// Dereference a pointer safely
static void *PodDerefPtr(const void *addr) {
  __try {
    return *(void *const *)addr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// Read first char of a string safely; returns 0 on fault
static char PodReadChar(const char *p) {
  __try {
    return *p;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return '\0';
  }
}

// Walk glyph array; returns number of valid button glyphs stored
static int PodWalkGlyphs(void *glyphsPtr, int glyphCount, GlyphUV out[256]) {
  __try {
    int n = 0;
    uint8_t *gp = (uint8_t *)glyphsPtr;
    constexpr int STRIDE = 24;
    for (int i = 0; i < glyphCount; i++, gp += STRIDE) {
      uint16_t letter = *(uint16_t *)(gp + 0x00);
      if (letter < 1 || letter > 23) continue;
      float s0 = *(float *)(gp + 0x08);
      float t0 = *(float *)(gp + 0x0C);
      float s1 = *(float *)(gp + 0x10);
      float t1 = *(float *)(gp + 0x14);
      auto okUV = [](float v) { return v >= 0.0f && v <= 1.0f; };
      if (!okUV(s0) || !okUV(t0) || !okUV(s1) || !okUV(t1)) continue;
      if (s1 <= s0 || t1 <= t0) continue;
      out[letter] = {s0, t0, s1, t1, true};
      n++;
    }
    return n;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// ---------------------------------------------------------------------------
static bool IsUserSpacePtr(const void *p) {
  uintptr_t v = (uintptr_t)p;
  return (v >= 0x10000 && v <= 0x7FFFFFFFFFFFllu);
}

// Verify COM vtable falls inside d3d11.dll  (no __try needed — uses VirtualQuery)
static bool IsLikelyD3D11COM(const void *ptr) {
  if (!IsUserSpacePtr(ptr)) return false;
  HMODULE d3d11 = GetModuleHandleA("d3d11.dll");
  if (!d3d11) return true; // can't verify → optimistically allow
  MODULEINFO mi = {};
  if (!GetModuleInformation(GetCurrentProcess(), d3d11, &mi, sizeof(mi)))
    return true;
  void *vtable = PodDerefPtr(ptr);
  if (!vtable) return false;
  uintptr_t base = (uintptr_t)mi.lpBaseOfDll;
  uintptr_t end  = base + mi.SizeOfImage;
  return ((uintptr_t)vtable >= base && (uintptr_t)vtable < end);
}

// Probe GfxMaterial struct for the font atlas SRV.
// IW6 GfxMaterial layout (x64):
//   +0x18: GfxTextureDef* textureTable  (primary; fallback: +0x10, +0x20, +0x28)
// IW6 GfxTextureDef layout (x64):
//   +0x08: GfxImage*                    (primary; fallback: +0x00, +0x10)
// GfxImage.textures.shaderView at +0x08.
static ID3D11ShaderResourceView *PodFindMaterialSRV(const void *matPtr) {
  __try {
    static const int kMatOff[] = { 0x18, 0x10, 0x20, 0x28 };
    static const int kDefOff[] = { 0x08, 0x00, 0x10 };
    for (int mi : kMatOff) {
      void *texTable = *(void **)((const uint8_t *)matPtr + mi);
      if (!IsUserSpacePtr(texTable)) continue;
      for (int di : kDefOff) {
        void *img = *(void **)((uint8_t *)texTable + di);
        if (!IsUserSpacePtr(img)) continue;
        void *srv = *(void **)((uint8_t *)img + 0x08);
        if (IsUserSpacePtr(srv) && IsLikelyD3D11COM(srv))
          return (ID3D11ShaderResourceView *)srv;
      }
    }
    return nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// ---------------------------------------------------------------------------
// DB_EnumXAssets_Internal callback infrastructure
// Used to find GfxImage* in the IMAGE asset pool by name when
// PodFindMaterialSRV (GfxMaterial traversal) fails.
//
// GfxImage layout (iw6x-client verified, sizeof=104):
//   +0x08: ID3D11ShaderResourceView* shaderView
//   +0x60: const char* name
// ---------------------------------------------------------------------------
struct FindImageCtx {
  const char *targetName;  // material name to match
  void       *foundImage;  // output: GfxImage* (or nullptr)
};

// Safely compare c-strings; stops at maxLen characters.
static bool PodStrEqN(const char *a, const char *b, int maxLen) {
  __try {
    for (int i = 0; i < maxLen; i++) {
      if (a[i] != b[i]) return false;
      if (a[i] == '\0') return true;
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static void __cdecl FindImageByNameCb(void *hdr, void *inData) {
  auto *ctx = (FindImageCtx *)inData;
  if (ctx->foundImage) return;
  if (!IsUserSpacePtr(hdr)) return;
  // GfxImage.name at +0x60
  void *nameVoid = PodDerefPtr((uint8_t *)hdr + 0x60);
  if (!IsUserSpacePtr(nameVoid)) return;
  if (PodReadChar((const char *)nameVoid) == '\0') return;
  const char *imgName = (const char *)nameVoid;
  const char *target  = ctx->targetName;
  // Try exact match
  if (PodStrEqN(imgName, target, 127)) { ctx->foundImage = hdr; return; }
  // Try stripping "fonts/" prefix from target
  if (PodStrEqN(target, "fonts/", 6) && PodStrEqN(imgName, target + 6, 121))
    ctx->foundImage = hdr;
}

// Enumerate IMAGE asset pool once; logs all "font"-named images.
// Returns SRV or nullptr. Caches result for subsequent calls.
static ID3D11ShaderResourceView *TryFindSRVViaEnum(const char *matName) {
  static bool   s_tried    = false;
  static void  *s_cachedImg = nullptr;

  if (!s_tried) {
    s_tried = true;
    typedef void (__cdecl *EnumCb)(void *, void *);
    typedef void (*DB_EnumXAssets_Internal_t)(int, EnumCb, const void *, bool);
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    if (base) {
      DB_EnumXAssets_Internal_t enumFn = reinterpret_cast<DB_EnumXAssets_Internal_t>(
          reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(base, IW6Offsets::DB_EnumXAssets_Internal_SP)));

      if (!enumFn) { s_tried = false; return nullptr; }
      // Enumerate all IMAGE assets and log names containing "font"
      struct LogCtx { char buf[512]; } lctx;
      struct AllImgCtx {
        int   count;
        char  firstName[128];
      } allCtx = {};
      enumFn(0xD /*IMAGE*/, [](void *hdr, void *data) {
        auto *c = (AllImgCtx *)data;
        c->count++;
        if (!IsUserSpacePtr(hdr)) return;
        void *nv = PodDerefPtr((uint8_t *)hdr + 0x60);
        if (!IsUserSpacePtr(nv) || PodReadChar((const char *)nv) == '\0') return;
        const char *nm = (const char *)nv;
        // Log any name containing "font"
        bool hasfont = false;
        __try {
          for (int i = 0; nm[i] && i < 64; i++)
            if ((nm[i]|0x20)=='f' && (nm[i+1]|0x20)=='o' &&
                (nm[i+2]|0x20)=='n' && (nm[i+3]|0x20)=='t')
              { hasfont = true; break; }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        if (hasfont && c->firstName[0] == '\0') {
          __try { strncpy_s(c->firstName, nm, 127); }
          __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
      }, &allCtx, true);

      char logBuf[512];
      sprintf_s(logBuf, "[GGA-Enum] total_images=%d first_font_name=%s",
                allCtx.count, allCtx.firstName[0] ? allCtx.firstName : "(none)");
      LogToFile(logBuf);

      // Now do the targeted search
      FindImageCtx ctx = { matName, nullptr };
      enumFn(0xD, FindImageByNameCb, &ctx, true);
      s_cachedImg = ctx.foundImage;

      sprintf_s(logBuf, "[GGA-Enum] targeted search for '%s' → %s",
                matName, s_cachedImg ? "FOUND" : "not found");
      LogToFile(logBuf);
    }
  }

  if (!s_cachedImg) return nullptr;
  // GfxImage.shaderView at +0x08
  void *srvVoid = PodDerefPtr((uint8_t *)s_cachedImg + 0x08);
  if (!srvVoid || !IsUserSpacePtr(srvVoid) || !IsLikelyD3D11COM(srvVoid))
    return nullptr;
  return (ID3D11ShaderResourceView *)srvVoid;
}

// ---------------------------------------------------------------------------
// TryCaptureImpl — NO C++ objects (no std::string, no LogToFile)
// All __try calls are via the isolated helpers above.
// Returns 0 on failure, or the captured button count on success.
// Outputs the material name into matNameBuf and a failure step into *failStep.
// ---------------------------------------------------------------------------
static int TryCaptureImpl(void *fontPtr, char *matNameBuf, int matNameBufLen,
                          int *failStep) {
  *failStep = 1;
  if (!IsSafeRead(fontPtr, 0x28)) return 0;

  // Font_s.material at +0x10
  *failStep = 2;
  void *matPtr = PodDerefPtr((uint8_t *)fontPtr + 0x10);
  if (!IsUserSpacePtr(matPtr)) return 0;
  if (!IsSafeRead(matPtr, 8))  return 0;

  // Material.name at +0x00
  *failStep = 3;
  const char *matName = (const char *)PodDerefPtr(matPtr);
  if (!IsUserSpacePtr(matName)) return 0;
  if (!IsSafeRead((void *)matName, 4)) return 0;
  if (PodReadChar(matName) == '\0') return 0;

  // Stash name for caller's logging (safe — matNameBuf is POD char[])
  strncpy_s(matNameBuf, matNameBufLen, matName, matNameBufLen - 1);

  // Walk GfxMaterial texture table to find GfxImage SRV.
  // IW6 layout: Material+0x18 → GfxTextureDef*, texDef+0x08 → GfxImage*,
  //             GfxImage+0x08 → ID3D11ShaderResourceView*.
  // Fallback: DB_EnumXAssets_Internal over IMAGE pool if struct traversal fails.
  *failStep = 4;
  auto *srv = PodFindMaterialSRV(matPtr);
  if (!srv) srv = TryFindSRVViaEnum(matNameBuf);
  if (!srv) return 0;

  // Font_s.glyphCount at +0x0C, .glyphs at +0x20
  *failStep = 5;
  if (!IsSafeRead((uint8_t *)fontPtr + 0x0C, 4)) return 0;
  int glyphCount = *(int *)((uint8_t *)fontPtr + 0x0C);
  if (glyphCount <= 0 || glyphCount > 65536) return 0;

  *failStep = 6;
  void *glyphsPtr = PodDerefPtr((uint8_t *)fontPtr + 0x20);
  if (!IsUserSpacePtr(glyphsPtr)) return 0;
  if (!IsSafeRead(glyphsPtr, (size_t)glyphCount * 24)) return 0;

  *failStep = 7;
  int captured = PodWalkGlyphs(glyphsPtr, glyphCount, s_Glyphs);
  if (captured == 0) return 0;

  srv->AddRef();
  s_SRV   = srv;
  s_Ready = true;
  return captured;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool TryCapture(void *fontPtr) {
  if (s_Ready)  return true;
  if (!fontPtr) return false;

  char matNameBuf[128] = {};
  int failStep = 0;
  int captured = TryCaptureImpl(fontPtr, matNameBuf, sizeof(matNameBuf),
                                &failStep);

  char buf[512];
  if (!captured) {
    // Log first failure only (avoid log spam per-frame)
    static int s_lastFailStep = -1;
    static char s_lastMatName[128] = {};
    if (failStep != s_lastFailStep ||
        strncmp(matNameBuf, s_lastMatName, 127) != 0) {
      s_lastFailStep = failStep;
      strncpy_s(s_lastMatName, matNameBuf, 127);
      sprintf_s(buf, "[GamepadGlyphAtlas] TryCapture FAILED step=%d mat=%s ptr=%p",
                failStep, matNameBuf[0] ? matNameBuf : "(none)", fontPtr);
      LogToFile(buf);

      // step=4: Material probing failed — dump raw pointer values at key offsets
      // Uses PodDerefPtr (__try-protected) so game memory pool quirks don't crash.
      if (failStep == 4 && matNameBuf[0]) {
        void *mp = PodDerefPtr((const uint8_t *)fontPtr + 0x10);
        if (mp) {
          void *v00 = PodDerefPtr((uint8_t*)mp+0x00);
          void *v08 = PodDerefPtr((uint8_t*)mp+0x08);
          void *v10 = PodDerefPtr((uint8_t*)mp+0x10);
          void *v18 = PodDerefPtr((uint8_t*)mp+0x18);
          void *v20 = PodDerefPtr((uint8_t*)mp+0x20);
          void *v28 = PodDerefPtr((uint8_t*)mp+0x28);
          void *v30 = PodDerefPtr((uint8_t*)mp+0x30);
          void *v38 = PodDerefPtr((uint8_t*)mp+0x38);
          void *v40 = PodDerefPtr((uint8_t*)mp+0x40);
          void *v48 = PodDerefPtr((uint8_t*)mp+0x48);
          sprintf_s(buf, sizeof(buf),
            "[GGA-MatDump] +00=%p +08=%p +10=%p +18=%p +20=%p +28=%p +30=%p +38=%p +40=%p +48=%p",
            v00, v08, v10, v18, v20, v28, v30, v38, v40, v48);
          LogToFile(buf);

          // Level-2: dump contents of candidate textureTable pointers
          // (v18 = most likely textureTable*, v08 = secondary candidate)
          void *candidates[] = { v18, v08, v00 };
          const char *candNames[] = { "v18", "v08", "v00" };
          for (int ci = 0; ci < 3; ci++) {
            void *tt = candidates[ci];
            if (!IsUserSpacePtr(tt)) continue;
            void *t00 = PodDerefPtr((uint8_t*)tt+0x00);
            void *t08 = PodDerefPtr((uint8_t*)tt+0x08);
            void *t10 = PodDerefPtr((uint8_t*)tt+0x10);
            void *t18 = PodDerefPtr((uint8_t*)tt+0x18);
            void *t20 = PodDerefPtr((uint8_t*)tt+0x20);
            sprintf_s(buf, sizeof(buf),
              "[GGA-TexDump] cand=%s(%p) +00=%p +08=%p +10=%p +18=%p +20=%p",
              candNames[ci], tt, t00, t08, t10, t18, t20);
            LogToFile(buf);
            // Level-3: if any of t00/t08/t10 look like GfxImage*, check for SRV
            void *imgs[] = { t00, t08, t10 };
            for (void *img : imgs) {
              if (!IsUserSpacePtr(img)) continue;
              void *s00 = PodDerefPtr((uint8_t*)img+0x00);
              void *s08 = PodDerefPtr((uint8_t*)img+0x08);
              void *s10 = PodDerefPtr((uint8_t*)img+0x10);
              sprintf_s(buf, sizeof(buf),
                "[GGA-ImgDump] img=%p +00=%p +08=%p +10=%p d3d11=%d/%d/%d",
                img,
                s00, s08, s10,
                (int)IsLikelyD3D11COM(s00),
                (int)IsLikelyD3D11COM(s08),
                (int)IsLikelyD3D11COM(s10));
              LogToFile(buf);
            }
          }
        }
      }
    }
    return false;
  }

  sprintf_s(buf, "[GamepadGlyphAtlas] Captured: mat=%s buttons=%d",
            matNameBuf, captured);
  LogToFile(buf);
  return true;
}

bool IsReady() { return s_Ready; }

ID3D11ShaderResourceView *GetSRV() { return s_SRV; }

GlyphUV GetGlyphUV(unsigned char ch) {
  if (ch == 0) return {};
  return s_Glyphs[ch];
}

void Shutdown() {
  if (s_SRV) { s_SRV->Release(); s_SRV = nullptr; }
  s_Ready = false;
  memset(s_Glyphs, 0, sizeof(s_Glyphs));
}

} // namespace GamepadGlyphAtlas
