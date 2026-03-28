#undef WIN32_LEAN_AND_MEAN
#include "D3D11Hook.h"
#include "BinkHook.h"
#include "DXGIWrapper.h"
#include "Detour.h"
#include "GameViewport.h"
#include "KoreanAtlas.h"
#include "KoreanRenderer.h"
#include "NativeSubtitleTracker.h"
#include "StateSaver.h"
#include "TextHook.h"
#include "HudClassify.h"
#include "BindingResolver.h"
#include "ObjectiveRendererUnified.h"
#include "Utils.h"
#include <d3d11.h>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <cctype>
#include <cstring>
#include <cmath>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>


// ScreenPlacement structure from IW6 engine (iw6x structs.hpp)
// Used to get authoritative safe-area and scaling values.
struct IW6ScreenPlacement {
  float scaleVirtualToReal[2];    // +0x00: virtual-to-real scale (x, y)
  float scaleVirtualToFull[2];    // +0x08
  float scaleRealToVirtual[2];    // +0x10
  float realViewportPosition[2];  // +0x18
  float realViewportSize[2];      // +0x20
  float virtualViewableMin[2];    // +0x28
  float virtualViewableMax[2];    // +0x30
  float realViewableMin[2];       // +0x38: safe area top-left (authoritative)
  float realViewableMax[2];       // +0x40: safe area bottom-right
  float virtualAdjustableMin[2];  // +0x48
  float virtualAdjustableMax[2];  // +0x50
  float realAdjustableMin[2];     // +0x58
  float realAdjustableMax[2];     // +0x60
  float subScreenLeft[2];         // +0x68
};
typedef IW6ScreenPlacement *(*ScrPlace_GetViewPlacement_t)();
static ScrPlace_GetViewPlacement_t s_ScrPlace_GetViewPlacement = nullptr;

// Cached ScreenPlacement values (updated per frame)
static float s_scrPlaceViewMin[2] = {0.0f, 0.0f};
static float s_scrPlaceScale[2] = {0.0f, 0.0f};
static bool s_scrPlaceValid = false;
static bool s_scrPlaceEverValid = false;
static DWORD s_scrPlaceLastGoodTick = 0;

// SEH-isolated helper: call ScrPlace_GetViewPlacement and read values.
// Must be in a separate function without C++ objects to use __try/__except.
static float s_scrPlaceAdjMin[2] = {0.0f, 0.0f}; // realAdjustableMin cache

static bool ReadScreenPlacementSEH(float outViewMin[2], float outScale[2],
                                    float outAdjMin[2]) {
  __try {
    IW6ScreenPlacement *sp = s_ScrPlace_GetViewPlacement();
    if (!sp)
      return false;
    float vx = sp->realViewableMin[0];
    float vy = sp->realViewableMin[1];
    float sx = sp->scaleVirtualToReal[0];
    float sy = sp->scaleVirtualToReal[1];
    float ax = sp->realAdjustableMin[0];
    float ay = sp->realAdjustableMin[1];
    // Sanity checks (no std:: calls in SEH scope)
    if (sx > 0.1f && sx < 20.0f && sy > 0.1f && sy < 20.0f &&
        vx >= 0.0f && vx < 500.0f && vy >= 0.0f && vy < 500.0f) {
      outViewMin[0] = vx;
      outViewMin[1] = vy;
      outScale[0] = sx;
      outScale[1] = sy;
      // AdjustableMin might differ from ViewableMin if user changed safe area
      if (ax >= 0.0f && ax < 500.0f && ay >= 0.0f && ay < 500.0f) {
        outAdjMin[0] = ax;
        outAdjMin[1] = ay;
      } else {
        outAdjMin[0] = vx;
        outAdjMin[1] = vy;
      }
      return true;
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static void UpdateScreenPlacement() {
  static uintptr_t s_moduleBase_sp = (uintptr_t)GetModuleHandleA(NULL);
  if (!s_ScrPlace_GetViewPlacement && s_moduleBase_sp) {
    s_ScrPlace_GetViewPlacement =
        (ScrPlace_GetViewPlacement_t)(s_moduleBase_sp + 0x24D150);
  }
  if (!s_ScrPlace_GetViewPlacement) {
    // Keep the last known-good placement if resolver is temporarily unavailable.
    s_scrPlaceValid = s_scrPlaceEverValid;
    return;
  }
  float vm[2], sc[2], am[2];
  if (ReadScreenPlacementSEH(vm, sc, am)) {
    s_scrPlaceViewMin[0] = vm[0];
    s_scrPlaceViewMin[1] = vm[1];
    s_scrPlaceScale[0] = sc[0];
    s_scrPlaceScale[1] = sc[1];
    s_scrPlaceAdjMin[0] = am[0];
    s_scrPlaceAdjMin[1] = am[1];
    s_scrPlaceValid = true;
    s_scrPlaceEverValid = true;
    s_scrPlaceLastGoodTick = GetTickCount();
  } else {
    // Transient read failure must not force objective overlays to fallback
    // safe-area coordinates (causes one-time top-left shift on first load).
    s_scrPlaceValid = s_scrPlaceEverValid;
  }
}

// Typedefs
typedef HRESULT(WINAPI *D3D11CreateDeviceAndSwapChain_t)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
    UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **,
    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

typedef HRESULT(STDMETHODCALLTYPE *Present_t)(IDXGISwapChain *, UINT, UINT);

// Pointers
D3D11CreateDeviceAndSwapChain_t Original_D3D11CreateDeviceAndSwapChain =
    nullptr;
Present_t Original_Present = nullptr;

// Globals for RTV
ID3D11RenderTargetView *g_pBackBufferRTV = nullptr;
ID3D11Texture2D *g_pBackBuffer = nullptr;

// Per-frame swap chain descriptor cache (set once in HookAndGate, read by all subsystems)
static DXGI_SWAP_CHAIN_DESC g_FrameSwapChainDesc = {};
static bool g_FrameSwapChainDescValid = false;

// =========================================================================
// Overlay helpers (UTF-8 safe, lightweight)
// =========================================================================
static float Clamp01(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

static float EaseOut(float v) {
  v = Clamp01(v);
  return v * (2.0f - v);
}

static size_t CountUtf8Chars(const std::string &s) {
  size_t count = 0;
  const char *p = s.c_str();
  while (*p) {
    uint32_t cp = 0;
    int bytes = KoreanAtlas::DecodeUTF8(p, cp);
    if (bytes <= 0) break;
    p += bytes;
    count++;
  }
  return count;
}

static std::string Utf8PrefixByChars(const std::string &s, size_t chars) {
  if (chars == 0) return "";
  size_t count = 0;
  size_t bytePos = 0;
  const char *p = s.c_str();
  while (*p && count < chars) {
    uint32_t cp = 0;
    int bytes = KoreanAtlas::DecodeUTF8(p, cp);
    if (bytes <= 0) break;
    p += bytes;
    bytePos += (size_t)bytes;
    count++;
  }
  if (bytePos >= s.size()) return s;
  return s.substr(0, bytePos);
}

static std::string StripColorCodesSimple(const std::string &s) {
  // Fast path: no caret → no color codes possible, avoid loop entirely
  if (s.find('^') == std::string::npos) {
    return s;
  }
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '^' && i + 1 < s.size()) {
      char next = s[i + 1];
      if ((next >= '0' && next <= '9') || (next >= 'a' && next <= 'z') ||
          (next >= 'A' && next <= 'Z')) {
        ++i;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

// Wrap subtitle text using width-aware word layout.
// - Keeps timing/sync untouched (render-only formatting).
// - Handles Korean/English mixed lines.
// - Uses balanced split for 2-line cases to mimic native subtitle feel.
static std::vector<std::string> WrapKoreanText(const std::string &text,
                                               float maxWidth, float fontHeight,
                                               float scale,
                                               bool rebalanceTwoLine = true) {
    auto IsWs = [](uint32_t cp) -> bool {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r';
  };

  struct WidthCacheKey {
    std::string text;
    uint32_t fontBits;
    uint32_t scaleBits;
    uint8_t atlasSlot;

    bool operator==(const WidthCacheKey &o) const {
      return fontBits == o.fontBits && scaleBits == o.scaleBits &&
             atlasSlot == o.atlasSlot && text == o.text;
    }
  };
  struct WidthCacheKeyHash {
    size_t operator()(const WidthCacheKey &k) const {
      size_t h = (size_t)k.fontBits;
      h = (h * 1315423911u) ^ (size_t)k.scaleBits;
      h = (h * 2654435761u) ^ (size_t)k.atlasSlot;
      h ^= std::hash<std::string>{}(k.text);
      return h;
    }
  };
  static std::unordered_map<WidthCacheKey, float, WidthCacheKeyHash> widthCache;
  if (widthCache.size() > 1024) {
    widthCache.clear();
  }

  auto MeasureCached = [&](const std::string &s,
                           uint8_t atlasSlot = 0) -> float {
    uint32_t fontBits = 0;
    uint32_t scaleBits = 0;
    memcpy(&fontBits, &fontHeight, sizeof(uint32_t));
    memcpy(&scaleBits, &scale, sizeof(uint32_t));

    WidthCacheKey key{s, fontBits, scaleBits, atlasSlot};
    auto it = widthCache.find(key);
    if (it != widthCache.end()) {
      return it->second;
    }
    float w = KoreanRenderer::MeasureTextWidthEx(s, fontHeight, scale, atlasSlot);
    widthCache.emplace(std::move(key), w);
    return w;
  };

  auto SplitWords = [&](const std::string &s) -> std::vector<std::string> {
    std::vector<std::string> words;
    std::string cur;
    const char *p = s.c_str();
    while (*p) {
      uint32_t cp = 0;
      int bytes = KoreanAtlas::DecodeUTF8(p, cp);
      if (bytes <= 0)
        break;
      if (IsWs(cp)) {
        if (!cur.empty()) {
          words.push_back(cur);
          cur.clear();
        }
      } else {
        cur.append(p, (size_t)bytes);
      }
      p += bytes;
    }
    if (!cur.empty()) {
      words.push_back(cur);
    }
    return words;
  };

  auto BreakLongToken = [&](const std::string &token) -> std::vector<std::string> {
    std::vector<std::string> parts;
    if (token.empty()) {
      return parts;
    }

    float tokenWidth = MeasureCached(token);
    if (tokenWidth <= maxWidth) {
      parts.push_back(token);
      return parts;
    }

    std::string current;
    float currentWidth = 0.0f;
    const char *p = token.c_str();
    while (*p) {
      uint32_t cp = 0;
      int bytes = KoreanAtlas::DecodeUTF8(p, cp);
      if (bytes <= 0)
        break;
      std::string ch(p, (size_t)bytes);
      float chWidth = MeasureCached(ch);
      if (!current.empty() && (currentWidth + chWidth) > maxWidth) {
        parts.push_back(current);
        current.clear();
        currentWidth = 0.0f;
      }
      current += ch;
      currentWidth += chWidth;
      p += bytes;
    }
    if (!current.empty()) {
      parts.push_back(current);
    }
    if (parts.empty()) {
      parts.push_back(token);
    }
    return parts;
  };

  auto JoinRange = [](const std::vector<std::string> &words, size_t begin,
                      size_t endInclusive) -> std::string {
    if (words.empty() || begin > endInclusive || endInclusive >= words.size())
      return "";
    std::string out = words[begin];
    for (size_t i = begin + 1; i <= endInclusive; ++i) {
      out += ' ';
      out += words[i];
    }
    return out;
  };

  auto WrapParagraph = [&](const std::string &paragraph) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (paragraph.empty()) {
      out.push_back("");
      return out;
    }

    float fullWidth = MeasureCached(paragraph);
    if (fullWidth <= maxWidth) {
      out.push_back(paragraph);
      return out;
    }

    std::vector<std::string> words = SplitWords(paragraph);
    if (words.empty()) {
      out.push_back(paragraph);
      return out;
    }

    std::vector<std::string> expanded;
    expanded.reserve(words.size());
    for (const auto &w : words) {
      auto parts = BreakLongToken(w);
      expanded.insert(expanded.end(), parts.begin(), parts.end());
    }
    words.swap(expanded);

    std::vector<float> widths(words.size(), 0.0f);
    for (size_t i = 0; i < words.size(); ++i) {
      widths[i] = MeasureCached(words[i]);
    }
    float spaceWidth = MeasureCached(" ");

    auto LineWidth = [&](size_t begin, size_t endInclusive) -> float {
      float w = 0.0f;
      for (size_t i = begin; i <= endInclusive; ++i) {
        w += widths[i];
      }
      if (endInclusive > begin) {
        w += spaceWidth * (float)(endInclusive - begin);
      }
      return w;
    };

    // Greedy baseline with maxWidth guard.
    std::vector<std::pair<size_t, size_t>> segments;
    size_t start = 0;
    while (start < words.size()) {
      size_t end = start;
      float w = widths[start];
      while ((end + 1) < words.size()) {
        float candidate = w + spaceWidth + widths[end + 1];
        if (candidate <= maxWidth) {
          w = candidate;
          ++end;
        } else {
          break;
        }
      }
      segments.push_back({start, end});
      start = end + 1;
    }

    // For 2-line wraps, rebalance split to avoid ugly short tail lines.
    // This behavior is optional; in-game dialogue often looks more native
    // with greedy wrapping that allows a shorter 2nd line.
    if (rebalanceTwoLine && segments.size() == 2 && words.size() >= 3) {
      float bestScore = 1.0e30f;
      size_t bestBreak = segments[0].second + 1; // first index of line 2
      float target = maxWidth * 0.82f;

      for (size_t b = 1; b < words.size(); ++b) {
        float w1 = LineWidth(0, b - 1);
        float w2 = LineWidth(b, words.size() - 1);
        float overflow = 0.0f;
        if (w1 > maxWidth)
          overflow += (w1 - maxWidth) * (w1 - maxWidth) * 4000.0f;
        if (w2 > maxWidth)
          overflow += (w2 - maxWidth) * (w2 - maxWidth) * 4000.0f;

        float balance = std::fabs(w1 - w2) * 1.25f;
        float targetPenalty = 0.0f;
        if (w1 < target)
          targetPenalty += (target - w1) * 0.35f;
        if (w2 < target * 0.55f)
          targetPenalty += (target * 0.55f - w2) * 1.75f;

        bool badStart = false;
        if (!words[b].empty()) {
          char c = words[b][0];
          badStart = (c == ',' || c == '.' || c == '!' || c == '?' ||
                      c == ';' || c == ':' || c == ')' || c == ']');
        }

        float score = overflow + balance + targetPenalty + (badStart ? 240.0f : 0.0f);
        if (score < bestScore) {
          bestScore = score;
          bestBreak = b;
        }
      }

      segments.clear();
      segments.push_back({0, bestBreak - 1});
      segments.push_back({bestBreak, words.size() - 1});
    }

    for (const auto &seg : segments) {
      out.push_back(JoinRange(words, seg.first, seg.second));
    }
    if (out.empty()) {
      out.push_back(paragraph);
    }
    return out;
  };

  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    std::string part =
        (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
    if (!part.empty() && part.back() == '\r') {
      part.pop_back();
    }
    auto wrapped = WrapParagraph(part);
    lines.insert(lines.end(), wrapped.begin(), wrapped.end());
    if (nl == std::string::npos)
      break;
    pos = nl + 1;
  }

  if (lines.empty()) {
    lines.push_back(text);
  }
  return lines;
}

static float ComputeSubtitleWrapWidth(float screenWidth, float screenHeight,
                                      bool videoSubtitle) {
  if (screenWidth <= 1.0f || screenHeight <= 1.0f) {
    return screenWidth * 0.6f;
  }

  float aspect = screenWidth / screenHeight;
  float ratio = videoSubtitle ? 0.56f : 0.62f;

  // Wider aspect ratios should use narrower logical subtitle columns.
  if (aspect >= 2.30f) {
    ratio -= 0.09f;
  } else if (aspect >= 2.00f) {
    ratio -= 0.07f;
  } else if (aspect >= 1.85f) {
    ratio -= 0.04f;
  } else if (aspect <= 1.45f) {
    ratio += 0.05f;
  }

  float minRatio = videoSubtitle ? 0.42f : 0.48f;
  float maxRatio = videoSubtitle ? 0.68f : 0.74f;
  ratio = std::clamp(ratio, minRatio, maxRatio);

  float safeMarginX = (std::max)(24.0f, screenWidth * 0.045f);
  float safeWidth = (std::max)(120.0f, screenWidth - safeMarginX * 2.0f);

  float wrapWidth = screenWidth * ratio;
  if (wrapWidth > safeWidth) {
    wrapWidth = safeWidth;
  }

  float minWidth =
      (std::max)(220.0f, screenWidth * (videoSubtitle ? 0.38f : 0.44f));
  if (wrapWidth < minWidth) {
    wrapWidth = minWidth;
  }

  return wrapWidth;
}

static float ComputeVideoSubtitleScale(float screenWidth, float screenHeight) {
  float widthRatio = screenWidth / 1920.0f;
  float heightRatio = screenHeight / 1080.0f;
  float resRatio = (std::min)(widthRatio, heightRatio);

  // Clamp lower bound for readability at very low resolutions;
  // no upper clamp so text scales proportionally at 1440p / 4K / etc.
  if (resRatio < 0.88f) resRatio = 0.88f;
  return 2.5f * resRatio;
}

static std::string ReplaceFakeIntroSeconds(const std::string &s, DWORD now,
                                           DWORD introStart) {
  const std::string token = "[{FAKE_INTRO_SECONDS:";
  std::string out;
  size_t pos = 0;
  while (true) {
    size_t start = s.find(token, pos);
    if (start == std::string::npos) {
      out.append(s.substr(pos));
      break;
    }
    out.append(s.substr(pos, start - pos));
    size_t numStart = start + token.size();
    size_t end = s.find("}]", numStart);
    if (end == std::string::npos) {
      out.append(s.substr(start));
      break;
    }
    std::string numStr = s.substr(numStart, end - numStart);
    int base = atoi(numStr.c_str());
    int num = base;
    if (introStart > 0) {
      DWORD elapsed = now - introStart;
      num = (base + (int)(elapsed / 1000)) % 60;
    }
    char buf[4];
    sprintf_s(buf, "%02d", num);
    out.append(buf);
    pos = end + 2;
  }
  return out;
}

static bool IsIntroPlaceholderLine(const std::string &s) {
  std::string t = TrimAscii(s);
  if (t.empty())
    return true;
  std::string lower;
  lower.reserve(t.size());
  for (char c : t) {
    lower.push_back((char)std::tolower((unsigned char)c));
  }
  return (lower == "line2" || lower == "line3" || lower == "line4");
}

static bool LooksLikeIntroDate(const std::string &s) {
  if (s.find(':') != std::string::npos)
    return true;
  if (s.find("June") != std::string::npos || s.find("July") != std::string::npos ||
      s.find("August") != std::string::npos || s.find("May") != std::string::npos)
    return true;
  // Any digit + "/" or "-" pattern
  for (char c : s) {
    if (std::isdigit((unsigned char)c))
      return true;
  }
  return false;
}

static bool LooksLikeIntroLocation(const std::string &s) {
  if (s.find(",") != std::string::npos)
    return true;
  return false;
}

static bool LooksLikeIntroName(const std::string &s) {
  std::string t = TrimAscii(s);
  if (t.empty())
    return false;
  if (t.front() == '"' || t.back() == '"')
    return true;
  // Short non-numeric line without time/location hints
  if (t.size() <= 18 && t.find(':') == std::string::npos &&
      t.find(",") == std::string::npos) {
    for (char c : t) {
      if (std::isdigit((unsigned char)c))
        return false;
    }
    return true;
  }
  return false;
}

static bool LooksLikeTimeChyron(const std::string &s) {
  if (s.find(':') != std::string::npos)
    return true;
  if (s.find("minutes") != std::string::npos || s.find("minute") != std::string::npos ||
      s.find("hours") != std::string::npos || s.find("hour") != std::string::npos ||
      s.find("years") != std::string::npos || s.find("year") != std::string::npos)
    return true;
  return false;
}

// LUI fallback positions for hint prompts (from sp_hud/gameinfo.dec.lua)
enum LuiHintSlot {
  LUI_HINT_SLOT_NONE = -1,
  LUI_HINT_SLOT_MANTLE = 0,
  LUI_HINT_SLOT_CURSOR = 1,
  LUI_HINT_SLOT_INVALID = 2,
  LUI_HINT_SLOT_BREATH = 3,
  LUI_HINT_SLOT_ZOOM = 4,
  LUI_HINT_SLOT_TOGGLE = 5,
  LUI_HINT_SLOT_COUNT = 6
};

static bool GetLuiHintPosition(const std::string &key, float screenWidth,
                               float screenHeight, float &outX, float &outY,
                               int *outSlot = nullptr) {
  if (key.find("INTROSCREEN") != std::string::npos) {
    return false;
  }

  // Use the 16:9 active area for scaling and positioning so that hints
  // land within the game's actual rendering region, not the letterbox area.
  const float activeW = (g_ActiveArea.width > 1.0f) ? g_ActiveArea.width : screenWidth;
  const float activeH = (g_ActiveArea.height > 1.0f) ? g_ActiveArea.height : screenHeight;
  const float activeOffY = g_ActiveArea.offsetY;

  float scaleY = activeH / 720.0f;
  float centerX = screenWidth * 0.5f;

  // LUI hint containers are 64px tall in 720p space.
  // IMPORTANT: KoreanRenderer expects baseline Y. The previous implementation
  // used container center (+32), which renders too high ("above English").
  // Baseline offset is tuned from native draw capture:
  // cursorHint top=720-160-64=496, native baseline ~539 => +43 from top.
  constexpr float kHintHeight720 = 64.0f;
  constexpr float kBaselineFromTop720 = 43.0f;

  // safeTop/safeBottom define the 16:9 active area within the backbuffer.
  float safeTop = activeOffY;
  float safeBottom = activeOffY + activeH;

  auto SetTop = [&](int slot, float top) -> bool {
    outX = centerX;
    outY = safeTop + (top + kBaselineFromTop720) * scaleY;
    if (outSlot) *outSlot = slot;
    return true;
  };

  auto SetBottom = [&](int slot, float bottomAbs) -> bool {
    outX = centerX;
    outY =
        safeBottom - (bottomAbs + (kHintHeight720 - kBaselineFromTop720)) * scaleY;
    if (outSlot) *outSlot = slot;
    return true;
  };

  // --- LUI ownerdraw hints (exact positions from gameinfo.dec.lua) ---
  // mantleHint: bottomAnchor, bottom=-80, height=64 ??text center at 720-(80+32)=608
  if (key.find("MANTLE") != std::string::npos) {
    return SetBottom(LUI_HINT_SLOT_MANTLE, 80.0f);
  }
  // cursorHint: bottomAnchor, bottom=-160, height=64 ??text center at 720-(160+32)=528
  if (key.find("CURSOR") != std::string::npos &&
      key.find("CORNERED") == std::string::npos) {
    return SetBottom(LUI_HINT_SLOT_CURSOR, 160.0f);
  }
  // PLATFORM_* prompts: cursorHint slot.
  // EXCEPT PLATFORM_STANCEHINT_* — stance hints are hintprint() center-screen prompts,
  // not cursorHint interaction prompts. Let them fall through to renderer center default.
  if (key.rfind("PLATFORM_", 0) == 0 &&
      key.find("STANCEHINT") == std::string::npos) {
    return SetBottom(LUI_HINT_SLOT_CURSOR, 160.0f);
  }
  // invalidCmdHint: topAnchor, top=138, height=64 ??text center at 138+32=170
  if (key.find("INVALID") != std::string::npos) {
    return SetTop(LUI_HINT_SLOT_INVALID, 138.0f);
  }
  // breathHint: topAnchor, top=40, height=64 ??text center at 40+32=72
  if (key.find("BREATH") != std::string::npos) {
    return SetTop(LUI_HINT_SLOT_BREATH, 40.0f);
  }
  // zoomHint: topAnchor, top=70, height=64 ??text center at 70+32=102
  // NOT binoculars/cornered zoom
  if (key.find("ZOOM") != std::string::npos &&
      key.find("CORNERED") == std::string::npos &&
      key.find("BINOCULAR") == std::string::npos) {
    return SetTop(LUI_HINT_SLOT_ZOOM, 70.0f);
  }
  // toggleHybridHint / toggleThermalHint: topAnchor, top=40, height=64
  if (key.find("HYBRID") != std::string::npos ||
      key.find("THERMAL") != std::string::npos) {
    return SetTop(LUI_HINT_SLOT_TOGGLE, 40.0f);
  }

  // --- CORNERED_ action prompts: cursorHint position by default ---
  // Most CORNERED_ prompts appear at the cursorHint location
  if (key.find("CORNERED_") != std::string::npos) {
    // CORNERED objective lines are top-left objective text, not hint slots.
    if (key.rfind("CORNERED_OBJ_", 0) == 0 ||
        key.rfind("CORNERED_OBJECTIVE_", 0) == 0) {
      return false;
    }
    // Fail messages are center-screen overlays; don't reposition here.
    if (key.find("_FAIL") != std::string::npos ||
        key.find("FAIL_") != std::string::npos ||
        key.find("_KILLED") != std::string::npos ||
        key.find("GOT_AWAY") != std::string::npos) {
      return false;
    }
    if (key.find("BINOCULAR") != std::string::npos) {
      // Binocular hints are not cursorHint interaction prompts.
      // Keep them on the scanner/general HUD track (handled by fallback/native).
      return false;
    }
    // Scanning/status messages (SCANNING, IDENTIFIED, MATCH, etc): center
    if (key.find("SCANNING") != std::string::npos ||
        key.find("IDENTIFIED") != std::string::npos ||
        key.find("MATCH") != std::string::npos ||
        key.find("READY") != std::string::npos ||
        key.find("DATA") != std::string::npos ||
        key.find("_DOT_") != std::string::npos ||
        key.find("DEGREE_SYMBOL") != std::string::npos ||
        key.find("E3_TIME") != std::string::npos) {
      // These are HUD-status overlays; don't reposition, use default
      return false;
    }
    // Default CORNERED_ action prompts: cursorHint position
    return SetBottom(LUI_HINT_SLOT_CURSOR, 160.0f);
  }

  // Generic _HINT fallback: mission-specific hints (e.g., ODIN_INTRO_BUMPER_HINT_PC)
  // that don't match any specific pattern above ??cursorHint position.
  if (key.find("_HINT") != std::string::npos || key.rfind("HINT_", 0) == 0) {
    return SetBottom(LUI_HINT_SLOT_CURSOR, 160.0f);
  }

  return false;
}

static float PulseAlpha(DWORD now, float periodMs, float minA, float maxA,
                        float phase) {
  float t = (float)(now % (DWORD)periodMs) / periodMs;
  float s = sinf(6.2831853f * (t + phase));
  float v = (s * 0.5f + 0.5f);
  return minA + (maxA - minA) * v;
}

// GSC-style quick_pulse: every 50ms alpha = random(base*0.7, base)
// Uses hash-based pseudo-random to be deterministic per-entry per-tick.
static float QuickPulseAlpha(DWORD now, float base, uint32_t seed) {
  // Change every 50ms (GSC: wait 0.05)
  uint32_t tick = (uint32_t)(now / 50);
  // Simple hash for pseudo-random
  uint32_t h = tick ^ seed;
  h = h * 2654435761u; // Knuth multiplicative hash
  h = (h >> 16) ^ h;
  float t = (float)(h & 0xFFFF) / 65535.0f; // 0..1
  float lo = base * 0.7f;
  return lo + (base - lo) * t;
}

static void ConvertHudNativeToScreen(float nativeX, float nativeY,
                                     unsigned short virtualW,
                                     unsigned short virtualH, float screenWidth,
                                     float screenHeight, float &outX,
                                     float &outY) {
  if (virtualW > 0 && virtualH > 0) {
    outX = nativeX * (screenWidth / (float)virtualW);
    outY = nativeY * (screenHeight / (float)virtualH);
  } else if (virtualW == 0 && virtualH > 0) {
    float uni = screenHeight / (float)virtualH;
    outX = nativeX * uni;
    outY = nativeY * uni;
  } else {
    outX = nativeX;
    outY = nativeY;
  }
}

static float ComputeScaleForPixelHeight(float fontHeight, float desiredPxH,
                                        float minPxH, float maxPxH,
                                        float minCmd, float maxCmd) {
  if (desiredPxH < minPxH) desiredPxH = minPxH;
  if (desiredPxH > maxPxH) desiredPxH = maxPxH;
  float desiredFinal = desiredPxH / (float)KoreanAtlas::GLYPH_SIZE_BASE;
  float baseFinal = KoreanRenderer::CalculateFinalScale(fontHeight, 1.0f);
  if (baseFinal <= 0.0001f) return 1.0f;
  float cmd = desiredFinal / baseFinal;
  if (cmd < minCmd) cmd = minCmd;
  if (cmd > maxCmd) cmd = maxCmd;
  return cmd;
}

// Legacy multi-path objective subsystem — kept for reference but rendering
// is now handled by the unified single-path renderer below.
#include "D3D11Hook.ObjectiveSubsystem.inl"

// NEW: Unified single-path objective/status renderer.
#include "ObjectiveRendererUnified.inl"

static DWORD g_PostVideoHideHudFastUntil = 0;
static constexpr DWORD kPostVideoHideHudFastMs = 2000;
static DWORD g_PostVideoHintSuppressUntil = 0;


#include "D3D11Hook.HookAndGate.inl"
// Process Subtitles (Called by DXGIWrapper or Hook_Present)
// =========================================================================
// Track active subtitle Y region for hint clipping prevention
static float g_SubtitleActiveTopY = 0.0f;
static float g_SubtitleActiveBottomY = 0.0f;
static DWORD g_SubtitleActiveTime = 0;

namespace {
struct RuntimeSignalDiagState {
  bool initLogged = false;
  DWORD lastPollTick = 0;

  bool havePlayerDiedRecently = false;
  int lastPlayerDiedRecently = 0;
  bool lastDeathDvarActive = false;
  DWORD lastPlayerDiedRecentlyMissLogTick = 0;

  bool haveGscBootSeq = false;
  int lastGscBootSeq = 0;
  bool haveGscDeathSeq = false;
  int lastGscDeathSeq = 0;
  bool haveGscFailSeq = false;
  int lastGscFailSeq = 0;
  bool haveGscResumeSeq = false;
  int lastGscResumeSeq = 0;
};

void PollRuntimeSignalDiagnostics(DWORD now) {
  static RuntimeSignalDiagState s_state;

  if (!s_state.initLogged) {
    s_state.initLogged = true;
    LogToFile("[SIGMON] runtime signal monitor active");
  }

  if ((now - s_state.lastPollTick) < 200) {
    return;
  }
  s_state.lastPollTick = now;

  int diedRecently = 0;
  if (TextHook_TryGetDvarInt("player_died_recently", diedRecently)) {
    if (!s_state.havePlayerDiedRecently ||
        diedRecently != s_state.lastPlayerDiedRecently) {
      char buf[160];
      sprintf_s(buf,
                "[SIGMON][DVAR] player_died_recently=%d prev=%d",
                diedRecently,
                s_state.havePlayerDiedRecently ? s_state.lastPlayerDiedRecently
                                               : -9999);
      LogToFile(buf);
    }

    const bool deathNow = diedRecently > 0;
    if (!s_state.havePlayerDiedRecently ||
        deathNow != s_state.lastDeathDvarActive) {
      char buf[160];
      sprintf_s(buf,
                "[SIGMON][EVENT] death_dvar active=%d value=%d",
                deathNow ? 1 : 0, diedRecently);
      LogToFile(buf);
    }

    s_state.havePlayerDiedRecently = true;
    s_state.lastPlayerDiedRecently = diedRecently;
    s_state.lastDeathDvarActive = deathNow;
  } else if ((now - s_state.lastPlayerDiedRecentlyMissLogTick) > 5000) {
    s_state.lastPlayerDiedRecentlyMissLogTick = now;
    LogToFile("[SIGMON][DVAR] player_died_recently unreadable");
  }

  const auto pollCounter = [](const char *name, const char *tag, bool &haveValue,
                              int &lastValue) {
    int value = 0;
    if (!TextHook_TryGetDvarInt(name, value)) {
      return;
    }

    if (!haveValue || value != lastValue) {
      char buf[160];
      sprintf_s(buf, "[SIGMON][GSC] %s=%d prev=%d", tag, value,
                haveValue ? lastValue : -9999);
      LogToFile(buf);
      haveValue = true;
      lastValue = value;
    }
  };

  pollCounter("ghostskor_sig_boot_seq", "boot_seq", s_state.haveGscBootSeq,
              s_state.lastGscBootSeq);
  pollCounter("ghostskor_sig_death_seq", "death_seq", s_state.haveGscDeathSeq,
              s_state.lastGscDeathSeq);
  pollCounter("ghostskor_sig_fail_seq", "fail_seq", s_state.haveGscFailSeq,
              s_state.lastGscFailSeq);
  pollCounter("ghostskor_sig_resume_seq", "resume_seq",
              s_state.haveGscResumeSeq, s_state.lastGscResumeSeq);
}
} // namespace

void D3D11Hook_ProcessSubtitles(IDXGISwapChain *pSwapChain) {
  if (!pSwapChain)
    return;

  // Ensure swap-chain descriptor is available for all rendering subsystems.
  // Hook_Present sets this when DXGIWrapper is NOT active; when the wrapper IS
  // active it early-returns, so we must populate it here instead.
  // Always refresh — resolution can change mid-session via graphics options.
  g_FrameSwapChainDescValid =
      SUCCEEDED(pSwapChain->GetDesc(&g_FrameSwapChainDesc));

  DWORD now = GetTickCount();
  PollRuntimeSignalDiagnostics(now);

  // GSC sets player_died_recently on death and lets it decay.
  // Use its rising edge to immediately clear lingering fallback subtitles.
  {
    static bool s_DeathSignalActive = false;
    int diedRecently = 0;
    if (TextHook_TryGetDvarInt("player_died_recently", diedRecently)) {
      const bool deathNow = diedRecently > 0;
      if (deathNow && !s_DeathSignalActive) {
        TextHook_ClearSubtitleQueue();
        static DWORD s_lastDeathLogTick = 0;
        if ((now - s_lastDeathLogTick) > 1000) {
          s_lastDeathLogTick = now;
          char dbuf[128];
          sprintf_s(dbuf,
                    "[D3D11] death signal: cleared subtitle queue (player_died_recently=%d)",
                    diedRecently);
          LogToFile(dbuf);
        }
      }
      s_DeathSignalActive = deathNow;
    }
  }

  bool videoPlaying = BinkHook::IsCutsceneVideoPlaying();
  static bool s_LastVideoPlaying = false;
  static DWORD s_HudOverlaySuppressUntil = 0;
  if (videoPlaying && !s_LastVideoPlaying) {
    g_PostVideoHideHudFastUntil = 0;
    TextHook_ClearSubtitleQueue();
    LogToFile("[D3D11] Video transition: opened -> cleared subtitle queue once, keep caches, visibility gate only");
  } else if (!videoPlaying && s_LastVideoPlaying) {
    // Hide HUD overlays briefly after video close to avoid loading-screen flashes.
    s_HudOverlaySuppressUntil = now + 700;
    g_PostVideoHideHudFastUntil = now + kPostVideoHideHudFastMs;
    g_PostVideoHintSuppressUntil = now + 2000;
    LogToFile("[D3D11] Video transition: closed -> suppress HUD overlay 700ms, synthetic hideHudFast 2000ms, hint suppress 2000ms");
  }
  s_LastVideoPlaying = videoPlaying;

  // Subtitle style helpers: name-only glow on "^2NAME:" prefix.
  auto FindNamePrefixStart = [](const std::string &s, size_t &outStart) -> bool {
    outStart = 0;
    size_t i = 0;
    // Tolerate UTF-8 BOM at the beginning of the subtitle string.
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
      i = 3;
    }
    // Skip simple leading whitespace before color codes.
    while (i < s.size() &&
           (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
      ++i;
    }
    if ((i + 1) >= s.size())
      return false;
    if (s[i] == '^' && s[i + 1] == '2') {
      outStart = i;
      return true;
    }
    return false;
  };

  auto IsNameLine = [&](const std::string &s) -> bool {
    size_t start = 0;
    if (!FindNamePrefixStart(s, start))
      return false;
    for (size_t i = start; i < s.size();) {
      if (s[i] == '^' && (i + 1) < s.size()) {
        i += 2;
        continue;
      }
      if (s[i] == ':')
        return true;
      i++;
    }
    return false;
  };

  auto ExtractNameSegment = [&](const std::string &s) -> std::string {
    size_t start = 0;
    if (!FindNamePrefixStart(s, start))
      return std::string();
    size_t end = std::string::npos;
    for (size_t i = start; i < s.size();) {
      if (s[i] == '^' && (i + 1) < s.size()) {
        i += 2;
        continue;
      }
      if (s[i] == ':') {
        end = i + 1;
        break;
      }
      i++;
    }
    if (end == std::string::npos)
      return std::string();
    if (end < s.size() && s[end] == ' ')
      end++;
    return s.substr(start, end - start);
  };

  auto EnsureNameHasResetCode = [&](const std::string &s) -> std::string {
    // Normalize "^2NAME:^7 dialogue" shape so ':' is always part of name styling
    // and dialogue always starts after a single ^7 reset.
    if (!IsNameLine(s))
      return s;

    std::string nameSeg = ExtractNameSegment(s);
    if (nameSeg.empty())
      return s;

    std::string cleanName = StripColorCodesSimple(nameSeg);
    if (cleanName.empty())
      return s;

    size_t start = 0;
    if (!FindNamePrefixStart(s, start))
      return s;

    std::string fixed;
    fixed.reserve(s.size() + 6);
    if (start > 0)
      fixed.append(s.substr(0, start));
    fixed.append("^2");
    fixed.append(cleanName);
    fixed.append("^7");
    fixed.append(s.substr(start + nameSeg.size()));
    return fixed;
  };

  auto HexPrefix = [](const std::string &s, size_t maxBytes) -> std::string {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    size_t n = (std::min)(maxBytes, s.size());
    for (size_t i = 0; i < n; ++i) {
      if (i > 0)
        oss << ' ';
      oss << std::setw(2) << (unsigned int)(unsigned char)s[i];
    }
    return oss.str();
  };

  auto ContainsHangul = [](const std::string &s) -> bool {
    const char *p = s.c_str();
    while (*p) {
      uint32_t cp = 0;
      int bytes = KoreanAtlas::DecodeUTF8(p, cp);
      if (bytes <= 0) {
        ++p;
        continue;
      }
      if ((cp >= 0xAC00 && cp <= 0xD7AF) || // Hangul Syllables
          (cp >= 0x1100 && cp <= 0x11FF) || // Hangul Jamo
          (cp >= 0x3130 && cp <= 0x318F) || // Compatibility Jamo
          (cp >= 0xA960 && cp <= 0xA97F) || // Jamo Extended-A
          (cp >= 0xD7B0 && cp <= 0xD7FF)) { // Jamo Extended-B
        return true;
      }
      p += bytes;
    }
    return false;
  };

  auto QueueSubtitleReadabilityPass =
      [&](const std::string &rawLine, float x, float y, float scaleNow,
          float fontHeightNow, float widthNow, float baseAlpha,
          const float nameColor[4], const float nameGlowColor[4]) {
        if (baseAlpha <= 0.003f)
          return;

        if (ContainsHangul(rawLine)) {
          std::string plain = StripColorCodesSimple(rawLine);
          if (!plain.empty()) {
            float strokeA = Clamp01(baseAlpha * 0.12f);
            if (strokeA > 0.003f) {
              float strokeColor[4] = {0.0f, 0.0f, 0.0f, strokeA};
              float strokeOff = (std::max)(0.75f, 0.90f * scaleNow);

              KoreanRenderer::QueueText(plain, x - strokeOff, y, scaleNow,
                                        strokeColor, fontHeightNow, 0, widthNow,
                                        true, false, 2.0f, true);
              KoreanRenderer::QueueText(plain, x + strokeOff, y, scaleNow,
                                        strokeColor, fontHeightNow, 0, widthNow,
                                        true, false, 2.0f, true);
              KoreanRenderer::QueueText(plain, x, y - strokeOff, scaleNow,
                                        strokeColor, fontHeightNow, 0, widthNow,
                                        true, false, 2.0f, true);
              KoreanRenderer::QueueText(plain, x, y + strokeOff, scaleNow,
                                        strokeColor, fontHeightNow, 0, widthNow,
                                        true, false, 2.0f, true);
            }

            float blurA = Clamp01(baseAlpha * 0.05f);
            if (blurA > 0.003f) {
              float blurColor[4] = {0.0f, 0.0f, 0.0f, blurA};
              KoreanRenderer::QueueText(plain, x, y + (0.55f * scaleNow),
                                        scaleNow * 1.005f, blurColor,
                                        fontHeightNow, 0, widthNow, true, false,
                                        2.0f, true);
            }
          }
        }

        std::string nameSeg = ExtractNameSegment(rawLine);
        if (nameSeg.empty())
          return;

        std::string namePlain = StripColorCodesSimple(nameSeg);
        if (namePlain.empty())
          return;

        // Name ghost should include ':' and use ^2 pass so renderer's blurred
        // glow path also contributes (closer to native look).
        float nameWidth =
            KoreanRenderer::MeasureTextWidthEx(nameSeg, fontHeightNow, scaleNow);
        float ghostR = Clamp01(nameColor[0] * 0.25f + nameGlowColor[0] * 0.75f);
        float ghostG = Clamp01(
            (nameColor[1] * 0.32f + nameGlowColor[1] * 0.68f) * 1.22f);
        float ghostB = Clamp01(nameColor[2] * 0.25f + nameGlowColor[2] * 0.75f);
        float ghostA = Clamp01(baseAlpha * 0.30f);
        if (ghostA <= 0.004f)
          return;

        float left1 = (std::max)(1.25f, 1.55f * scaleNow);
        float left2 = (std::max)(2.05f, 2.55f * scaleNow);
        float ghostColorA[4] = {ghostR, ghostG, ghostB, ghostA};
        float ghostColorB[4] = {ghostR, ghostG, ghostB, ghostA * 0.62f};

        DrawStylePatch ghostStyleA{};
        ghostStyleA.overrideColorCode2 = true;
        ghostStyleA.code2R = ghostR;
        ghostStyleA.code2G = ghostG;
        ghostStyleA.code2B = ghostB;
        ghostStyleA.applyGlow = true;
        ghostStyleA.glowR = 0.0f;
        ghostStyleA.glowG = 0.0f;
        ghostStyleA.glowB = 0.0f;
        ghostStyleA.glowA = 1.22f;
        KoreanRenderer::QueueText(nameSeg, x - left1, y, scaleNow * 1.01f,
                                  ghostColorA, fontHeightNow, 0, nameWidth, true,
                                  false, 2.0f, true, false, false, -1, 0.0f, false,
                                  false, 0, &ghostStyleA);

        DrawStylePatch ghostStyleB = ghostStyleA;
        ghostStyleB.glowA = 1.05f;
        KoreanRenderer::QueueText(nameSeg, x - left2,
                                  y + (0.10f * scaleNow), scaleNow * 1.02f,
                                  ghostColorB, fontHeightNow, 0, nameWidth, true,
                                  false, 2.0f, true, false, false, -1, 0.0f, false,
                                  false, 0, &ghostStyleB);
      };

  #include "D3D11Hook.Process.VideoSubtitles.inl"
  #include "D3D11Hook.Process.InGameSubtitles.inl"
  #include "D3D11Hook.Process.IntroOverlay.inl"
  #include "D3D11Hook.Process.HudOverlayPrelude.inl"
  #include "D3D11Hook.Process.TimeScript.inl"
    #include "D3D11Hook.Process.DedicatedHint.inl"
    #include "D3D11Hook.Process.DedicatedObjHudHint.inl"
  }
}








