#include "KoreanRenderer.h"
#include "BinkHook.h"
#include "GameViewport.h"
#include "TextHook.h"
#include "GamepadGlyphAtlas.h"
#include <DirectXMath.h> // For matrix
#include <algorithm>     // For std::sort
#include <array>         // For static keyword tables
#include <atomic>
#include <cctype>        // For std::isspace/std::isdigit
#include <cmath>         // For std::fabs
#include <d3dcompiler.h> // Requires d3dcompiler.lib
#include <map>           // For right edge clustering
#include <mutex>         // Not used here but good practice
#include <set>           // For anchor points
#include <sstream>       // For logging
#include <unordered_set>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

#define STB_IMAGE_IMPLEMENTATION
#include "EmbeddedResources.h"
#include "KoreanAtlas.h"
#include "vendor/stb_image.h"

// =============================================================================
// Static Member Definition
// =============================================================================
ID3D11Device *KoreanRenderer::s_pDevice = nullptr;
ID3D11DeviceContext *KoreanRenderer::s_pContext = nullptr;

ID3D11VertexShader *KoreanRenderer::s_pVS = nullptr;
ID3D11PixelShader *KoreanRenderer::s_pPS = nullptr;
ID3D11InputLayout *KoreanRenderer::s_pInputLayout = nullptr;
ID3D11RasterizerState *KoreanRenderer::s_pRasterizerState = nullptr;
ID3D11BlendState *KoreanRenderer::s_pBlendState = nullptr;
ID3D11BlendState *KoreanRenderer::s_pBlendStateAdditive = nullptr;
ID3D11DepthStencilState *KoreanRenderer::s_pNoDepthStencilState = nullptr;
ID3D11Buffer *KoreanRenderer::s_pVertexBuffer = nullptr;
ID3D11Buffer *KoreanRenderer::s_pConstantBuffer = nullptr;
ID3D11ShaderResourceView *KoreanRenderer::s_pTextureView = nullptr;
ID3D11ShaderResourceView *KoreanRenderer::s_pGlowTextureView = nullptr;
ID3D11ShaderResourceView *KoreanRenderer::s_pTextureView2 = nullptr;
ID3D11ShaderResourceView *KoreanRenderer::s_pGlowTextureView2 = nullptr;
ID3D11SamplerState *KoreanRenderer::s_pSamplerState = nullptr;

std::vector<DrawCommand> KoreanRenderer::s_DrawList;
std::mutex KoreanRenderer::s_RenderMutex;
bool KoreanRenderer::s_bInitialized = false;

// Staging buffer: QueueText pushes here; Render splits by source each frame.
static std::vector<DrawCommand> s_StageList;

// QTE overlay: temporarily relax CalculateFinalScale clamps for QTE commands.
static bool s_QteScaleMode = false;
void KoreanRenderer::SetQteScaleMode(bool on) { s_QteScaleMode = on; }

// Global flag: set true by R_AddCmdDrawText detour, false otherwise.
// QueueText reads this to tag each DrawCommand with its source.
std::atomic<bool> g_bQueueFromMenu{false};
std::atomic<bool> g_bQueueKeepDuringVideo{false};
static std::atomic<int> g_RorkeDetailPageId{0};
static std::atomic<DWORD> g_RorkeDetailPageTick{0};
static std::atomic<DWORD> g_RorkeDetailActiveTick{0};
static std::atomic<int> g_RorkeDetailHeaderPageId{0};
static std::atomic<DWORD> g_RorkeDetailHeaderTick{0};
static std::atomic<float> g_RorkeDetailHeaderX{0.0f};
static std::atomic<float> g_RorkeDetailHeaderY{0.0f};
static std::atomic<float> g_RorkeDetailHeaderFontHeight{60.0f};
static std::atomic<float> g_RorkeDetailHeaderScale{1.0f};
static std::atomic<DWORD> g_RorkeReferenceHeaderTick{1};
static std::atomic<float> g_RorkeReferenceHeaderX{170.0f};
static std::atomic<float> g_RorkeReferenceHeaderY{120.0f};
static std::atomic<float> g_RorkeReferenceHeaderFontHeight{76.0f};
static std::atomic<float> g_RorkeReferenceHeaderScale{1.96f};
static std::atomic<int> g_RorkeDetailBodyPageId{0};
static std::atomic<DWORD> g_RorkeDetailBodyTick{0};
static std::atomic<float> g_RorkeDetailBodyX{0.0f};
static std::atomic<float> g_RorkeDetailBodyY{0.0f};
static std::atomic<float> g_RorkeDetailBodyFontHeight{60.0f};
static std::atomic<float> g_RorkeDetailBodyScale{1.0f};
static constexpr int RORKE_BODY_CACHE_SIZE = 21;
static std::array<std::string, RORKE_BODY_CACHE_SIZE> g_RorkeDetailBodyTextCache;
static std::array<bool, RORKE_BODY_CACHE_SIZE> g_RorkeDetailBodyTextCached{};
static bool s_Atlas2MetricsLoaded = false;
static bool s_Atlas2FallbackWarned = false;

// Popup bounds from previous frame — shared with TextHook for precise center detection.
// Updated at the end of each Render() call.
static std::atomic<bool> s_PopupActiveLastFrame{false};
static std::atomic<float> s_PopupTopYLastFrame{0.0f};
static std::atomic<float> s_PopupBottomYLastFrame{0.0f};
static std::atomic<float> s_PopupCenterXLastFrame{0.0f};
static std::atomic<float> s_PopupBandXLastFrame{0.0f};

bool KoreanRenderer_GetPopupBounds(float &topY, float &bottomY,
                                   float &centerX, float &bandX) {
  if (!s_PopupActiveLastFrame.load())
    return false;
  topY = s_PopupTopYLastFrame.load();
  bottomY = s_PopupBottomYLastFrame.load();
  centerX = s_PopupCenterXLastFrame.load();
  bandX = s_PopupBandXLastFrame.load();
  return true;
}

void KoreanRenderer_LatchRorkeDetailPage(int pageId) {
  if (pageId <= 0)
    return;
  const DWORD now = GetTickCount();
  g_RorkeDetailPageId.store(pageId);
  g_RorkeDetailPageTick.store(now);
  g_RorkeDetailActiveTick.store(now);
}

void KoreanRenderer_ClearRorkeDetailState() {
  g_RorkeDetailActiveTick.store(0);
  g_RorkeDetailPageId.store(0);
  g_RorkeDetailPageTick.store(0);
  g_RorkeDetailHeaderPageId.store(0);
  g_RorkeDetailHeaderTick.store(0);
  g_RorkeDetailBodyPageId.store(0);
  g_RorkeDetailBodyTick.store(0);
}

int KoreanRenderer_GetRecentRorkeDetailPage(unsigned long maxAgeMs) {
  const int pageId = g_RorkeDetailPageId.load();
  if (pageId <= 0)
    return 0;
  const DWORD age = GetTickCount() - g_RorkeDetailPageTick.load();
  if (age > maxAgeMs)
    return 0;
  return pageId;
}

bool KoreanRenderer_IsRecentRorkeDetailActive(unsigned long maxAgeMs) {
  const DWORD lastTick = g_RorkeDetailActiveTick.load();
  if (lastTick == 0)
    return false;
  return (GetTickCount() - lastTick) <= maxAgeMs;
}

void KoreanRenderer_LatchRorkeDetailHeaderLayout(int pageId, float x, float y,
                                                 float fontHeight,
                                                 float scale) {
  if (pageId <= 0)
    return;
  const DWORD now = GetTickCount();
  const int currentPageId = g_RorkeDetailHeaderPageId.load();
  const DWORD currentTick = g_RorkeDetailHeaderTick.load();
  const bool sameRecentPage =
      (currentPageId == pageId) && ((now - currentTick) <= 300);
  if (!sameRecentPage) {
    g_RorkeDetailHeaderPageId.store(pageId);
    g_RorkeDetailHeaderTick.store(now);
    if (x > 1.0f)
      g_RorkeDetailHeaderX.store(x);
    if (y > 1.0f)
      g_RorkeDetailHeaderY.store(y);
    if (fontHeight > 1.0f)
      g_RorkeDetailHeaderFontHeight.store(fontHeight);
    if (scale > 0.01f)
      g_RorkeDetailHeaderScale.store(scale);
    return;
  }

  g_RorkeDetailHeaderTick.store(now);
  if (x > g_RorkeDetailHeaderX.load())
    g_RorkeDetailHeaderX.store(x);
  if (y > 1.0f) {
    float oldY = g_RorkeDetailHeaderY.load();
    if (!(oldY > 1.0f) || y < oldY)
      g_RorkeDetailHeaderY.store(y);
  }
  if (fontHeight > g_RorkeDetailHeaderFontHeight.load())
    g_RorkeDetailHeaderFontHeight.store(fontHeight);
  if (scale > g_RorkeDetailHeaderScale.load())
    g_RorkeDetailHeaderScale.store(scale);
}

void KoreanRenderer_LatchRorkeReferenceHeaderLayout(float x, float y,
                                                    float fontHeight,
                                                    float scale) {
  if (x <= 1.0f || y <= 1.0f || fontHeight <= 1.0f || scale <= 0.01f)
    return;
  const DWORD now = GetTickCount();
  g_RorkeReferenceHeaderTick.store(now);
  g_RorkeReferenceHeaderX.store(x);
  g_RorkeReferenceHeaderY.store(y);
  g_RorkeReferenceHeaderFontHeight.store(fontHeight);
  g_RorkeReferenceHeaderScale.store(scale);
}

void KoreanRenderer_LatchRorkeDetailBodyLayout(int pageId, float x, float y,
                                               float fontHeight,
                                               float scale) {
  if (pageId <= 0)
    return;
  const DWORD now = GetTickCount();
  const int currentPageId = g_RorkeDetailBodyPageId.load();
  const DWORD currentTick = g_RorkeDetailBodyTick.load();
  const bool sameRecentPage =
      (currentPageId == pageId) && ((now - currentTick) <= 120);
  if (!sameRecentPage) {
    g_RorkeDetailBodyPageId.store(pageId);
    g_RorkeDetailBodyTick.store(now);
    if (x > 1.0f)
      g_RorkeDetailBodyX.store(x);
    if (y > 1.0f)
      g_RorkeDetailBodyY.store(y);
    if (fontHeight > 1.0f)
      g_RorkeDetailBodyFontHeight.store(fontHeight);
    if (scale > 0.01f)
      g_RorkeDetailBodyScale.store(scale);
    return;
  }

  g_RorkeDetailBodyTick.store(now);
  const float oldY = g_RorkeDetailBodyY.load();
  if (!(oldY > 1.0f) || (y > 1.0f && y < oldY)) {
    if (x > 1.0f)
      g_RorkeDetailBodyX.store(x);
    if (y > 1.0f)
      g_RorkeDetailBodyY.store(y);
  }
  const float oldFontHeight = g_RorkeDetailBodyFontHeight.load();
  if (!(oldFontHeight > 1.0f) ||
      (fontHeight > 1.0f && fontHeight > oldFontHeight)) {
    g_RorkeDetailBodyFontHeight.store(fontHeight);
  }
  const float oldScale = g_RorkeDetailBodyScale.load();
  if (!(oldScale > 0.01f) || (scale > 0.01f && scale > oldScale)) {
    g_RorkeDetailBodyScale.store(scale);
  }
}

// Per-item menu cache: each unique (text+position) is cached independently.
// When fresh menu items arrive, old cache is flushed first (prevents ghosting
// on menu transitions). When R_AddCmdDrawText fires with partial data (some
// items rejected by alpha threshold), uncached items retain their last good
// version. HUD/subtitle items bypass the cache entirely.
struct CachedMenuCmd {
  uint64_t key;
  uint64_t posKey;
  DrawCommand cmd;
  unsigned long lastSeen;
};
static std::vector<CachedMenuCmd> s_MenuItemCache;
static constexpr unsigned long kMenuItemTTLMs = 30;

struct MenuLayoutMetrics {
  float contentLeft;
  float contentTop;
  float contentRight;
  float contentBottom;

  float clusterVerticalFitPx;
  float anchorTolerancePx;
  float bucketPx;
  float cacheBucketXPx;
  float cacheBucketYPx;
  float leftVarianceThresholdPx;

  float globalYOffsetPx;
  float smallMenuYOffsetPx;

  float topClipY;
  float bottomClipY;
  float missionBLWidth;
  float missionBLHeight;
};

template <size_t N>
static bool ContainsAnyToken(const std::string &text,
                             const std::array<const char *, N> &tokens) {
  for (const char *token : tokens) {
    if (token && text.find(token) != std::string::npos)
      return true;
  }
  return false;
}

template <size_t N>
static bool EqualsAnyToken(const std::string &text,
                           const std::array<const char *, N> &tokens) {
  for (const char *token : tokens) {
    if (token && text == token)
      return true;
  }
  return false;
}

static float ClampFloat(float v, float lo, float hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

static int BucketPxInt(float bucketPx) {
  int b = (int)(bucketPx + 0.5f);
  if (b < 1)
    b = 1;
  return b;
}

static int QuantizeToBucket(float value, float bucketPx) {
  const int bucket = BucketPxInt(bucketPx);
  return (int)(value / (float)bucket) * bucket;
}

// Always-active header keywords (generic menus)
static const std::array<const char *, 16> kMenuHeaderKeywords = {
    "고급 비디오", "로크 파일", "Rorke Files", "비디오 옵션", "오디오 옵션",
    "컨트롤 옵션", "캠페인",    "옵션",      "게임 재개",
    "임무 선택",   "일시 정지됨", "일시 중지됨", "게임패드",    "채팅",
    "비디오 최적화", "게임 설명서"};

// Header words that should be treated as header only in top-strip context.
static const std::array<const char *, 10> kTopOnlyHeaderKeywords = {
    "스틱 배치", "스택 배치", "버튼 배치", "STICK LAYOUT",
    "BUTTON LAYOUT", "Stick Layout", "Button Layout",
    "이동", "시선", "동작"};

// Rorke Files only — classified file mission name tokens (top-zone headers)
// These are only active when "로크 파일" is detected in current frame.
static const std::array<const char *, 16> kClassifiedHeaderKeywords = {
    "작전",       "프로필:",    "보고서",     "무인지대",    "화력 기지",
    "기념일",     "빙판",       "송장벌레",   "프로토타입",  "발각",
    "성명서",     "워커,",      "벽에 대한 위협", "피해 추정",
    "THREATS AGAINST THE WALL", "DAMAGE ESTIMATES"};

static const std::array<const char *, 23> kMenuFooterKeywords = {
    "뒤로",   "종료",     "친구",     "ESC",      "BACK",     "EXIT",
    "QUIT",   "FRIENDS",  "쉬움",     "일반",     "보통",     "어려움",   "베테랑",
    "Recruit", "Regular", "Hardened", "Veteran",
    "RECRUIT", "REGULAR", "HARDENED", "VETERAN",
    // Classified file image navigation
    "이전 이미지", "다음 이미지"};

static const std::array<const char *, 6> kMainMenuDescKeywords = {
    "마지막으로 저장된", "체크포인트에서", "분대를 구성하고",
    "온라인에서 승급하고", "잠금 해제하십시오", "다른 분대와 대결"};

static const std::array<const char *, 4> kPopupAllowOutsideKeywords = {
    "고급 비디오", "Advanced Video", "임무 선택", "MISSION SELECT"};

static bool IsMenuHeaderKeyword(const std::string &text,
                                bool includeClassified = false) {
  if (ContainsAnyToken(text, kMenuHeaderKeywords))
    return true;
  if (includeClassified && ContainsAnyToken(text, kClassifiedHeaderKeywords))
    return true;
  return false;
}

static bool IsTopOnlyHeaderKeyword(const std::string &text) {
  return ContainsAnyToken(text, kTopOnlyHeaderKeywords);
}

static bool IsMenuFooterKeyword(const std::string &text) {
  return ContainsAnyToken(text, kMenuFooterKeywords);
}

static bool IsMainMenuDescKeyword(const std::string &text) {
  return ContainsAnyToken(text, kMainMenuDescKeywords);
}

static bool IsPopupOutsideAllowKeyword(const std::string &text) {
  return ContainsAnyToken(text, kPopupAllowOutsideKeywords);
}

static bool IsDifficultyLabel(const std::string &norm, const std::string &upper) {
  if (norm.find("쉬움") != std::string::npos ||
      norm.find("보통") != std::string::npos ||
      norm.find("일반") != std::string::npos ||
      norm.find("어려움") != std::string::npos ||
      norm.find("베테랑") != std::string::npos) {
    return true;
  }
  if (upper.find("RECRUIT") != std::string::npos ||
      upper.find("REGULAR") != std::string::npos ||
      upper.find("HARDENED") != std::string::npos ||
      upper.find("VETERAN") != std::string::npos) {
    return true;
  }
  return false;
}

static bool IsPauseRorkeStatusTextStrict(const std::string &text) {
  if (text.find("이 지역에서 로크 파일을 회수했습니다") != std::string::npos ||
      text.find("로크 파일을 회수했습니다") != std::string::npos ||
      text.find("로크 파일을 찾을 수 없습니다") != std::string::npos ||
      text.find("로크 파일을 찾지 못했습니다") != std::string::npos ||
      text.find("이 지역에서 로그 파일을 회수했습니다") != std::string::npos ||
      text.find("로그 파일을 회수했습니다") != std::string::npos ||
      text.find("로그 파일을 찾을 수 없습니다") != std::string::npos ||
      text.find("로그 파일을 찾지 못했습니다") != std::string::npos) {
    return true;
  }

  std::string upper;
  upper.reserve(text.size());
  for (unsigned char c : text) {
    upper.push_back((char)std::toupper(c));
  }
  if (upper.find("RORKE FILE RECOVERED FROM THIS AREA") != std::string::npos ||
      upper.find("RORKE FILE RECOVERED") != std::string::npos ||
      upper.find("RORKE FILE NOT FOUND") != std::string::npos) {
    return true;
  }
  return false;
}

static const char *GetStaticRorkeHeaderTextById(int headerId) {
  switch (headerId) {
  case 1:
    return "심리 보고서 - 가브리엘 로크";
  case 6:
    return "작전: 반송";
  case 8:
  case 9:
    return "작전: 데드볼트";
  case 19:
    return "프로필: 일라이어스 T. 워커, 예비역 대위";
  case 20:
    return "프로필: 데이비드 \"헤쉬\" 워커, 중위";
  case 21:
    return "프로필: 토마스 A. 메릭, 대위";
  default:
    return nullptr;
  }
}

static bool IsStaticRorkeHeaderPageId(int pageId) {
  return pageId >= 1 && pageId <= 21;
}

static const char *GetRorkeDetailBodyKeyById(int pageId) {
  static const char *keys[] = {
      nullptr,
      "MENU_SP_VF_FILE_01_BODY", "MENU_SP_VF_FILE_02_BODY",
      "MENU_SP_VF_FILE_03_BODY", "MENU_SP_VF_FILE_04_BODY",
      "MENU_SP_VF_FILE_05_BODY", "MENU_SP_VF_FILE_06_BODY",
      "MENU_SP_VF_FILE_07_BODY", "MENU_SP_VF_FILE_08_BODY",
      "MENU_SP_VF_FILE_09_BODY", "MENU_SP_VF_FILE_10_BODY",
      "MENU_SP_VF_FILE_11_BODY", "MENU_SP_VF_FILE_12_BODY",
      "MENU_SP_VF_FILE_13_BODY", "MENU_SP_VF_FILE_14_BODY",
      "MENU_SP_VF_FILE_15_BODY", "MENU_SP_VF_FILE_16_BODY",
      "MENU_SP_VF_FILE_17_BODY", "MENU_SP_VF_FILE_18_BODY",
      "MENU_SP_VF_FILE_19_BODY", "MENU_SP_VF_FILE_20_BODY",
      "MENU_SP_VF_FILE_21_BODY",
  };
  if (pageId >= 1 && pageId <= 21)
    return keys[pageId];
  return nullptr;
}

static const std::string *GetCachedRorkeDetailBodyTextById(int pageId) {
  if (pageId < 1 || pageId > 21)
    return nullptr;
  const int cacheIndex = pageId - 1; // 0-based

  if (!g_RorkeDetailBodyTextCached[cacheIndex]) {
    std::string localizedBody;
    std::string ignoredEnglish;
    const char *bodyKey = GetRorkeDetailBodyKeyById(pageId);
    if (bodyKey &&
        TextHook_GetLocalizedKeyText(bodyKey, localizedBody, ignoredEnglish) &&
        !localizedBody.empty()) {
      g_RorkeDetailBodyTextCache[cacheIndex] = std::move(localizedBody);
      g_RorkeDetailBodyTextCached[cacheIndex] = true;
    }
  }

  return g_RorkeDetailBodyTextCached[cacheIndex]
             ? &g_RorkeDetailBodyTextCache[cacheIndex]
             : nullptr;
}

static MenuLayoutMetrics ComputeMenuLayoutMetrics(const D3D11_VIEWPORT &vp,
                                                  const EngineSafeArea &safeArea) {
  MenuLayoutMetrics m = {};
  const float vpW = (vp.Width > 1.0f) ? vp.Width : 1.0f;
  const float vpH = (vp.Height > 1.0f) ? vp.Height : 1.0f;

  // Use 16:9 active area for content bounds so that header/footer/body
  // classification zones match the game's actual rendering area, not the
  // full backbuffer (which may include letterbox/pillarbox regions).
  const float areaW = (g_ActiveArea.width > 1.0f) ? g_ActiveArea.width : vpW;
  const float areaH = (g_ActiveArea.height > 1.0f) ? g_ActiveArea.height : vpH;
  const float areaOffX = g_ActiveArea.offsetX;
  const float areaOffY = g_ActiveArea.offsetY;

  const float safeX = ClampFloat(safeArea.x, 0.0f, areaW * 0.20f);
  const float safeY = ClampFloat(safeArea.y, 0.0f, areaH * 0.20f);

  m.contentLeft = areaOffX + safeX;
  m.contentTop = areaOffY + safeY;
  m.contentRight = areaOffX + areaW - safeX;
  m.contentBottom = areaOffY + areaH - safeY;

  if (m.contentRight <= m.contentLeft + 1.0f) {
    m.contentLeft = 0.0f;
    m.contentRight = vpW;
  }
  if (m.contentBottom <= m.contentTop + 1.0f) {
    m.contentTop = 0.0f;
    m.contentBottom = vpH;
  }

  const float contentW = m.contentRight - m.contentLeft;
  const float contentH = m.contentBottom - m.contentTop;

  // Golden behavior ratios (resolution-agnostic, content-space based).
  constexpr float kClusterVerticalFitRatio = 0.0833333f;
  constexpr float kAnchorToleranceRatio = 0.0078125f;
  constexpr float kBucketRatio = 0.00078125f;
  constexpr float kCacheBucketXRatio = 0.0625f;
  constexpr float kCacheBucketYRatio = 0.0138889f;
  constexpr float kLeftVarianceRatio = 0.001953125f;
  constexpr float kGlobalYOffsetRatio = -0.00486111f;
  constexpr float kSmallYOffsetRatio = -0.00277778f;

  m.clusterVerticalFitPx = ClampFloat(contentH * kClusterVerticalFitRatio, 72.0f, 220.0f);
  m.anchorTolerancePx = ClampFloat(contentW * kAnchorToleranceRatio, 10.0f, 42.0f);
  m.bucketPx = ClampFloat(contentW * kBucketRatio, 1.0f, 6.0f);
  m.cacheBucketXPx = ClampFloat(contentW * kCacheBucketXRatio, 64.0f, 320.0f);
  m.cacheBucketYPx = ClampFloat(contentH * kCacheBucketYRatio, 8.0f, 42.0f);
  m.leftVarianceThresholdPx = ClampFloat(contentW * kLeftVarianceRatio, 2.0f, 12.0f);

  m.globalYOffsetPx = contentH * kGlobalYOffsetRatio;
  m.smallMenuYOffsetPx = contentH * kSmallYOffsetRatio;

  m.topClipY = m.contentTop + contentH * 0.18f;
  m.bottomClipY = m.contentTop + contentH * 0.806f;
  m.missionBLWidth = contentW * 0.336f;
  m.missionBLHeight = contentH * 0.424f;

  return m;
}

// Hash on text + coarse X/Y buckets.
// We keep buckets coarse enough to absorb per-frame jitter while still
// separating independent UI columns (left list vs right header).
static uint64_t MenuCmdHash(const std::string &text, float x, float y,
                            float xBucketPx = 160.0f,
                            float yBucketPx = 20.0f) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < text.size(); ++i) {
    h ^= (uint64_t)(unsigned char)text[i];
    h *= 0x100000001b3ULL;
  }
  const float safeXBucket = (xBucketPx > 1.0f) ? xBucketPx : 1.0f;
  const float safeYBucket = (yBucketPx > 1.0f) ? yBucketPx : 1.0f;
  int xBucket = (int)(x / safeXBucket);
  h ^= (uint64_t)(unsigned)xBucket;
  h *= 0x100000001b3ULL;
  int yBucket = (int)(y / safeYBucket);
  h ^= (uint64_t)(unsigned)yBucket;
  h *= 0x100000001b3ULL;
  return h;
}

// Position-only hash (coarse X/Y buckets).
// Used to replace stale cached entries when text changes at the same anchor
// (prevents short-lived "ghost" trails on rapidly updating menu labels).
static uint64_t MenuCmdPosHash(float x, float y, float xBucketPx = 160.0f,
                               float yBucketPx = 20.0f) {
  uint64_t h = 0xcbf29ce484222325ULL;
  const float safeXBucket = (xBucketPx > 1.0f) ? xBucketPx : 1.0f;
  const float safeYBucket = (yBucketPx > 1.0f) ? yBucketPx : 1.0f;
  int xBucket = (int)(x / safeXBucket);
  h ^= (uint64_t)(unsigned)xBucket;
  h *= 0x100000001b3ULL;
  int yBucket = (int)(y / safeYBucket);
  h ^= (uint64_t)(unsigned)yBucket;
  h *= 0x100000001b3ULL;
  return h;
}

// Vertex Structure
struct SimpleVertex {
  XMFLOAT2 Pos;
  XMFLOAT4 Color;
  XMFLOAT2 UV;
};

// Shader Code (Texture Support)
const char *VS_CODE = R"(
cbuffer MatrixBuffer : register(b0) { matrix projection; };
struct VS_INPUT { float2 Pos : POSITION; float4 Color : COLOR; float2 UV : TEXCOORD; };
struct PS_INPUT { float4 Pos : SV_POSITION; float4 Color : COLOR; float2 UV : TEXCOORD; };
PS_INPUT VS(VS_INPUT input) {
    PS_INPUT output;
    output.Pos = mul(projection, float4(input.Pos, 0, 1));
    output.Color = input.Color;
    output.UV = input.UV;
    return output;
}
)";

const char *PS_CODE = R"(
Texture2D shaderTexture : register(t0);
SamplerState SampleType : register(s0);
struct PS_INPUT { float4 Pos : SV_POSITION; float4 Color : COLOR; float2 UV : TEXCOORD; };
float4 PS(PS_INPUT input) : SV_Target {
    float4 texColor = shaderTexture.Sample(SampleType, input.UV);
    // Alpha blending: Font is white on transparent. 
    // Multiply vertex color (usually white/yellow) with texture alpha.
    return input.Color * texColor;
}
)";

// =============================================================================
// Implementation
// =============================================================================

void KoreanRenderer::Init(ID3D11Device *pDevice,
                          ID3D11DeviceContext *pContext) {
  if (!pDevice || !pContext)
    return;

  // Check for Device Change (Resolution Switch, Alt+Enter, etc.)
  if (s_bInitialized && s_pDevice != pDevice) {
    LogToFile("[KoreanRenderer] Device change detected! Re-initializing...");
    Cleanup();
  }

  if (s_bInitialized)
    return;

  s_pDevice = pDevice;
  s_pContext = pContext;

  LogToFile("[KoreanRenderer] Initializing Resources...");

  // Load font metrics from embedded resources
  HMODULE hSelf = GetModuleHandleW(L"dxgi.dll");

  // Slot 0 (required)
  {
    HRSRC hRes = FindResourceA(hSelf, MAKEINTRESOURCEA(IDR_FONT_METRICS_0), RT_RCDATA);
    if (!hRes) {
      LogToFile("[CRITICAL] Embedded font_metrics.bin not found in DLL resources.");
      MessageBoxUtf8(NULL, "DLL에 폰트 메트릭이 내장되어 있지 않습니다.\n패치를 재설치하세요.",
                     "초기화 실패", MB_ICONERROR);
      return;
    }
    HGLOBAL hData = LoadResource(hSelf, hRes);
    DWORD dataSize = SizeofResource(hSelf, hRes);
    const void *pData = LockResource(hData);
    if (!pData || dataSize == 0 ||
        !KoreanAtlas::LoadMetricsFromMemory(pData, dataSize, KoreanAtlas::kAtlasSlotBase)) {
      LogToFile("[CRITICAL] Failed to load embedded font_metrics.bin.");
      MessageBoxUtf8(NULL, "내장 폰트 메트릭 로드 실패.\n패치를 재설치하세요.",
                     "초기화 실패", MB_ICONERROR);
      return;
    }
    std::ostringstream oss;
    oss << "[KoreanRenderer] Metrics slot0 loaded (embedded). glyphs="
        << KoreanAtlas::GetGlyphCount(KoreanAtlas::kAtlasSlotBase);
    LogToFile(oss.str());
  }

  // Slot 1 (optional header atlas)
  {
    HRSRC hRes = FindResourceA(hSelf, MAKEINTRESOURCEA(IDR_FONT_METRICS_1), RT_RCDATA);
    if (hRes) {
      HGLOBAL hData = LoadResource(hSelf, hRes);
      DWORD dataSize = SizeofResource(hSelf, hRes);
      const void *pData = LockResource(hData);
      s_Atlas2MetricsLoaded =
          (pData && dataSize > 0) &&
          KoreanAtlas::LoadMetricsFromMemory(pData, dataSize, KoreanAtlas::kAtlasSlotHeader);
    } else {
      s_Atlas2MetricsLoaded = false;
    }
    if (s_Atlas2MetricsLoaded) {
      std::ostringstream oss;
      oss << "[KoreanRenderer] Metrics slot1 loaded (embedded). glyphs="
          << KoreanAtlas::GetGlyphCount(KoreanAtlas::kAtlasSlotHeader);
      LogToFile(oss.str());
    } else {
      LogToFile("[KoreanRenderer] WARN: Embedded font_metrics2.bin missing/invalid. slot1 -> slot0 fallback.");
    }
  }

  CreateShaders();
  CreateBuffers();
  CreateStates();
  CreateTexture();

  // Validate Texture
  if (!s_pTextureView) {
    LogToFile("[KoreanRenderer] Critical Error: Texture Creation Failed!");
  }

  s_bInitialized = true;
  LogToFile("[KoreanRenderer] Initialization Complete.");
}

void KoreanRenderer::Cleanup() {
  if (s_pVS)
    s_pVS->Release();
  s_pVS = nullptr;
  if (s_pPS)
    s_pPS->Release();
  s_pPS = nullptr;
  if (s_pInputLayout)
    s_pInputLayout->Release();
  s_pInputLayout = nullptr;
  if (s_pRasterizerState)
    s_pRasterizerState->Release();
  s_pRasterizerState = nullptr;
  if (s_pBlendState)
    s_pBlendState->Release();
  s_pBlendState = nullptr;
  if (s_pBlendStateAdditive)
    s_pBlendStateAdditive->Release();
  s_pBlendStateAdditive = nullptr;
  if (s_pNoDepthStencilState)
    s_pNoDepthStencilState->Release();
  s_pNoDepthStencilState = nullptr;
  if (s_pVertexBuffer)
    s_pVertexBuffer->Release();
  s_pVertexBuffer = nullptr;
  if (s_pConstantBuffer)
    s_pConstantBuffer->Release();
  s_pConstantBuffer = nullptr;
  if (s_pTextureView)
    s_pTextureView->Release();
  s_pTextureView = nullptr;
  if (s_pGlowTextureView)
    s_pGlowTextureView->Release();
  s_pGlowTextureView = nullptr;
  if (s_pTextureView2)
    s_pTextureView2->Release();
  s_pTextureView2 = nullptr;
  if (s_pGlowTextureView2)
    s_pGlowTextureView2->Release();
  s_pGlowTextureView2 = nullptr;
  if (s_pSamplerState)
    s_pSamplerState->Release();
  s_pSamplerState = nullptr;
  s_DrawList.clear();
  s_StageList.clear();
  s_MenuItemCache.clear();
  s_Atlas2MetricsLoaded = false;
  s_Atlas2FallbackWarned = false;
  s_bInitialized = false;
}

void KoreanRenderer::QueueText(const std::string &text, float x, float y,
                               float scale, const float *color,
                               float fontHeight, int style, float finalWidthEng,
                               bool disableShadow, bool isSmallMenu,
                               float originalXScale, bool allowZeroAlpha,
                               bool code7ResetsToBase, bool disableAutoWrap,
                               int forceAlign, float maxWidthPx,
                               bool skipMenuClipping, bool yIsTopOfText,
                               uint8_t atlasSlot,
                               const DrawStylePatch *stylePatch,
                               float resolutionScale) {
  // Hard suppression for Clockwork Kick subtitle when menu is active
  if (TextHook_IsMenuContext() || TextHook_IsMenuActiveRaw()) {
    std::string clean = text;
    // Strip color codes
    for (size_t i = 0; i < clean.size();) {
      if (clean[i] == '^' && i + 1 < clean.size()) {
        clean.erase(i, 2);
        continue;
      }
      i++;
    }
    if ((clean.find("킥") != std::string::npos &&
         clean.find("블랙버드") != std::string::npos &&
         clean.find("10분") != std::string::npos) ||
        (clean.find("Kick:") != std::string::npos &&
         clean.find("Blackbird") != std::string::npos &&
         (clean.find("ten minutes") != std::string::npos ||
          clean.find("ten minute") != std::string::npos))) {
      return;
    }
  }
  // Ideally use mutex here if multithreading Queue vs Render
  std::lock_guard<std::mutex> lock(s_RenderMutex);
  DrawCommand cmd;
  cmd.text = text;
  cmd.x = x;
  cmd.y = y;
  cmd.scale = scale;
  cmd.fontHeight = fontHeight;
  cmd.resolutionScale = resolutionScale;
  cmd.style = style;
  cmd.finalWidthEng = finalWidthEng;
  cmd.disableShadow = disableShadow;
  cmd.isSmallMenu = isSmallMenu;
  cmd.code7ResetsToBase = code7ResetsToBase;
  cmd.disableAutoWrap = disableAutoWrap;
  cmd.forceAlign = forceAlign;
  cmd.maxWidthPx = maxWidthPx;
  cmd.skipMenuClipping = skipMenuClipping;
  cmd.originalXScale = originalXScale;
  cmd.glowR = 0.0f;
  cmd.glowG = 0.0f;
  cmd.glowB = 0.0f;
  cmd.glowA = 0.0f;
  cmd.glowUseGlyphColor = false;
  cmd.glowMul = 1.0f;
  cmd.glowIgnoreTextAlpha = false;
  cmd.glowOffsetX = 0.0f;
  cmd.glowOffsetY = 0.0f;
  cmd.glowAdditive = false;
  cmd.glowForceAll = false;
  cmd.glowSpread = 0.0f;
  cmd.glowAlphaMax = 0.0f;
  cmd.overrideColorCode1 = false;
  cmd.code1R = 1.0f;
  cmd.code1G = 0.0f;
  cmd.code1B = 0.0f;
  cmd.overrideColorCode2 = false;
  cmd.code2R = 0.0f;
  cmd.code2G = 1.0f;
  cmd.code2B = 0.0f;
  if (stylePatch) {
    if (stylePatch->overrideColorCode1) {
      cmd.overrideColorCode1 = true;
      cmd.code1R = stylePatch->code1R;
      cmd.code1G = stylePatch->code1G;
      cmd.code1B = stylePatch->code1B;
    }
    if (stylePatch->overrideColorCode2) {
      cmd.overrideColorCode2 = true;
      cmd.code2R = stylePatch->code2R;
      cmd.code2G = stylePatch->code2G;
      cmd.code2B = stylePatch->code2B;
    }
    if (stylePatch->applyGlow) {
      cmd.glowR = stylePatch->glowR;
      cmd.glowG = stylePatch->glowG;
      cmd.glowB = stylePatch->glowB;
      cmd.glowA = stylePatch->glowA;
      cmd.glowUseGlyphColor = stylePatch->glowUseGlyphColor;
      cmd.glowMul = stylePatch->glowMul;
      cmd.glowIgnoreTextAlpha = stylePatch->glowIgnoreTextAlpha;
      cmd.glowOffsetX = stylePatch->glowOffsetX;
      cmd.glowOffsetY = stylePatch->glowOffsetY;
      cmd.glowAdditive = stylePatch->glowAdditive;
      cmd.glowForceAll = stylePatch->glowForceAll;
      cmd.glowSpread = stylePatch->glowSpread;
      cmd.glowAlphaMax = stylePatch->glowAlphaMax;
    }
  }
  cmd.isMenuSource = g_bQueueFromMenu.load();
  cmd.keepDuringVideo = g_bQueueKeepDuringVideo.load();
  cmd.yIsTopOfText = yIsTopOfText;
  cmd.atlasSlot = (atlasSlot == KoreanAtlas::kAtlasSlotHeader)
                      ? KoreanAtlas::kAtlasSlotHeader
                      : KoreanAtlas::kAtlasSlotBase;
  cmd.qteScaleMode = s_QteScaleMode;
  if (color) {
    cmd.r = color[0];
    cmd.g = color[1];
    cmd.b = color[2];
    cmd.a = color[3];
  } else {
    cmd.r = 1.0f;
    cmd.g = 1.0f;
    cmd.b = 0.0f;
    cmd.a = 1.0f;
  }

  // === MICRO-THRESHOLD ALPHA CHECK ===
  // If alpha is nearly zero, do not render.
  if (!allowZeroAlpha && cmd.a < 0.005f) {
    return;
  }

  s_StageList.push_back(cmd);
}

void KoreanRenderer::SetLastCommandGlow(float r, float g, float b, float a) {
  std::lock_guard<std::mutex> lock(s_RenderMutex);
  if (!s_StageList.empty()) {
    auto &cmd = s_StageList.back();
    cmd.glowR = r;
    cmd.glowG = g;
    cmd.glowB = b;
    cmd.glowA = a;
    cmd.glowUseGlyphColor = false;
    cmd.glowMul = 1.0f;
    cmd.glowIgnoreTextAlpha = false;
    cmd.glowOffsetX = 0.0f;
    cmd.glowOffsetY = 0.0f;
  }
}

void KoreanRenderer::SetLastCommandGlowAdvanced(float r, float g, float b,
                                                float a, bool useGlyphColor,
                                                float mul, bool ignoreTextAlpha,
                                                float offX, float offY) {
  std::lock_guard<std::mutex> lock(s_RenderMutex);
  if (!s_StageList.empty()) {
    auto &cmd = s_StageList.back();
    cmd.glowR = r;
    cmd.glowG = g;
    cmd.glowB = b;
    cmd.glowA = a;
    cmd.glowUseGlyphColor = useGlyphColor;
    cmd.glowMul = mul;
    cmd.glowIgnoreTextAlpha = ignoreTextAlpha;
    cmd.glowOffsetX = offX;
    cmd.glowOffsetY = offY;
  }
}

void KoreanRenderer::SetLastCommandGlowAdditive(bool additive) {
  std::lock_guard<std::mutex> lock(s_RenderMutex);
  if (!s_StageList.empty()) {
    s_StageList.back().glowAdditive = additive;
  }
}

void KoreanRenderer::SetLastCommandColorCode1Override(float r, float g,
                                                      float b) {
  std::lock_guard<std::mutex> lock(s_RenderMutex);
  if (!s_StageList.empty()) {
    auto &cmd = s_StageList.back();
    cmd.overrideColorCode1 = true;
    cmd.code1R = r;
    cmd.code1G = g;
    cmd.code1B = b;
  }
}

void KoreanRenderer::SetLastCommandColorCode2Override(float r, float g,
                                                      float b) {
  std::lock_guard<std::mutex> lock(s_RenderMutex);
  if (!s_StageList.empty()) {
    auto &cmd = s_StageList.back();
    cmd.overrideColorCode2 = true;
    cmd.code2R = r;
    cmd.code2G = g;
    cmd.code2B = b;
  }
}

void KoreanRenderer::CreateShaders() {
  ID3DBlob *pVSBlob = nullptr;
  ID3DBlob *pErrorBlob = nullptr;
  HRESULT hr = D3DCompile(VS_CODE, strlen(VS_CODE), nullptr, nullptr, nullptr,
                          "VS", "vs_4_0", 0, 0, &pVSBlob, &pErrorBlob);
  if (FAILED(hr)) {
    if (pErrorBlob)
      LogToFile((char *)pErrorBlob->GetBufferPointer());
    LogToFile("Failed to compile VS");
    return;
  }
  s_pDevice->CreateVertexShader(pVSBlob->GetBufferPointer(),
                                pVSBlob->GetBufferSize(), nullptr, &s_pVS);

  // Input Layout
  D3D11_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  s_pDevice->CreateInputLayout(layout, 3, pVSBlob->GetBufferPointer(),
                               pVSBlob->GetBufferSize(), &s_pInputLayout);
  pVSBlob->Release();

  ID3DBlob *pPSBlob = nullptr;
  hr = D3DCompile(PS_CODE, strlen(PS_CODE), nullptr, nullptr, nullptr, "PS",
                  "ps_4_0", 0, 0, &pPSBlob, &pErrorBlob);
  if (FAILED(hr)) {
    if (pErrorBlob)
      LogToFile((char *)pErrorBlob->GetBufferPointer());
    LogToFile("Failed to compile PS");
    return;
  }
  s_pDevice->CreatePixelShader(pPSBlob->GetBufferPointer(),
                               pPSBlob->GetBufferSize(), nullptr, &s_pPS);
  pPSBlob->Release();
}

void KoreanRenderer::CreateBuffers() {
  // Dynamic Vertex Buffer:
  // main 4 regions: 4 * 49152 = 196608 vertices
  // button glyph region: 512 vertices  (≈85 button icons per frame)
  D3D11_BUFFER_DESC bd = {};
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.ByteWidth = sizeof(SimpleVertex) * (6 * 32768 + 512);
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  s_pDevice->CreateBuffer(&bd, nullptr, &s_pVertexBuffer);

  // Constant Buffer (Matrix)
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.ByteWidth = sizeof(XMMATRIX); // 64 bytes
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = 0;
  s_pDevice->CreateBuffer(&bd, nullptr, &s_pConstantBuffer);
}

void KoreanRenderer::CreateStates() {
  // Blend State
  D3D11_BLEND_DESC blendDesc = {};
  blendDesc.RenderTarget[0].BlendEnable = TRUE;
  blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
  blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blendDesc.RenderTarget[0].RenderTargetWriteMask =
      D3D11_COLOR_WRITE_ENABLE_ALL;
  s_pDevice->CreateBlendState(&blendDesc, &s_pBlendState);

  // Additive blend (for glow) - makes subtle glows visible without darkening.
  D3D11_BLEND_DESC addDesc = {};
  addDesc.RenderTarget[0].BlendEnable = TRUE;
  addDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  addDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
  addDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  addDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  addDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  addDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  addDesc.RenderTarget[0].RenderTargetWriteMask =
      D3D11_COLOR_WRITE_ENABLE_ALL;
  s_pDevice->CreateBlendState(&addDesc, &s_pBlendStateAdditive);

  // Rasterizer State (with scissor enabled for clipping)
  D3D11_RASTERIZER_DESC rsDesc = {};
  rsDesc.FillMode = D3D11_FILL_SOLID;
  rsDesc.CullMode = D3D11_CULL_NONE;
  rsDesc.ScissorEnable = true; // Enable for proper clipping in scroll menus
  rsDesc.DepthClipEnable = true;
  s_pDevice->CreateRasterizerState(&rsDesc, &s_pRasterizerState);

  // Depth-Stencil State — depth testing DISABLED for overlay rendering.
  // Without this, in-game pause menu text flickers because the 3D scene
  // leaves depth testing enabled (IW6 reverse-Z: z=0 = far plane → fail).
  D3D11_DEPTH_STENCIL_DESC dsDesc = {};
  dsDesc.DepthEnable = FALSE;
  dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  dsDesc.StencilEnable = FALSE;
  s_pDevice->CreateDepthStencilState(&dsDesc, &s_pNoDepthStencilState);
}

// =============================================================================
// CPU-side separable Gaussian blur for glow atlas generation.
// Operates on RGBA pixels; blurs only the alpha channel (RGB stays white).
// =============================================================================
static void GaussianBlurAlpha(unsigned char *pixels, int w, int h,
                              float sigma) {
  // Build 1-D Gaussian kernel (half-size = 3*sigma, at least 1)
  int radius = (int)ceilf(sigma * 3.0f);
  if (radius < 1)
    radius = 1;
  int kernelSize = radius * 2 + 1;
  std::vector<float> kernel(kernelSize);
  float sum = 0.0f;
  for (int i = 0; i < kernelSize; ++i) {
    float x = (float)(i - radius);
    kernel[i] = expf(-(x * x) / (2.0f * sigma * sigma));
    sum += kernel[i];
  }
  for (int i = 0; i < kernelSize; ++i)
    kernel[i] /= sum;

  // Temporary buffer for intermediate pass
  std::vector<float> tmp(w * h, 0.0f);

  // Extract alpha channel to float
  std::vector<float> alphaIn(w * h);
  for (int i = 0; i < w * h; ++i)
    alphaIn[i] = pixels[i * 4 + 3] / 255.0f;

  // Horizontal pass
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float val = 0.0f;
      for (int k = -radius; k <= radius; ++k) {
        int sx = x + k;
        if (sx < 0)
          sx = 0;
        if (sx >= w)
          sx = w - 1;
        val += alphaIn[y * w + sx] * kernel[k + radius];
      }
      tmp[y * w + x] = val;
    }
  }

  // Vertical pass (tmp → alphaIn)
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float val = 0.0f;
      for (int k = -radius; k <= radius; ++k) {
        int sy = y + k;
        if (sy < 0)
          sy = 0;
        if (sy >= h)
          sy = h - 1;
        val += tmp[sy * w + x] * kernel[k + radius];
      }
      alphaIn[y * w + x] = val;
    }
  }

  // Write back: RGB=255 (white), A=blurred alpha
  for (int i = 0; i < w * h; ++i) {
    pixels[i * 4 + 0] = 255; // R
    pixels[i * 4 + 1] = 255; // G
    pixels[i * 4 + 2] = 255; // B
    float a = alphaIn[i];
    if (a > 1.0f)
      a = 1.0f;
    pixels[i * 4 + 3] = (unsigned char)(a * 255.0f + 0.5f);
  }
}

void KoreanRenderer::CreateTexture() {
  HMODULE hSelf = GetModuleHandleW(L"dxgi.dll");

  auto LoadAtlasAndGlow = [&](int resourceId, const char *tag,
                              ID3D11ShaderResourceView **outMain,
                              ID3D11ShaderResourceView **outGlow) -> bool {
    HRSRC hRes = FindResourceA(hSelf, MAKEINTRESOURCEA(resourceId), RT_RCDATA);
    if (!hRes) {
      LogToFile(std::string("[KoreanRenderer] Embedded atlas resource not found: ") + tag);
      return false;
    }
    HGLOBAL hData = LoadResource(hSelf, hRes);
    DWORD dataSize = SizeofResource(hSelf, hRes);
    const unsigned char *pData = static_cast<const unsigned char *>(LockResource(hData));
    if (!pData || dataSize == 0) {
      LogToFile(std::string("[KoreanRenderer] Failed to lock embedded atlas: ") + tag);
      return false;
    }

    int width = 0, height = 0, channels = 0;
    unsigned char *pixels = stbi_load_from_memory(pData, (int)dataSize, &width, &height, &channels, 4);
    if (!pixels) {
      LogToFile(std::string("[KoreanRenderer] Failed to decode embedded atlas: ") + tag);
      return false;
    }

    size_t pixelBytes = (size_t)width * height * 4;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subResource = {};
    subResource.pSysMem = pixels;
    subResource.SysMemPitch = width * 4;

    ID3D11Texture2D *pTexture = nullptr;
    HRESULT hr = s_pDevice->CreateTexture2D(&desc, &subResource, &pTexture);
    if (FAILED(hr)) {
      stbi_image_free(pixels);
      LogToFile(std::string("[KoreanRenderer] Failed to CreateTexture2D (main, ") + tag + ")");
      return false;
    }

    hr = s_pDevice->CreateShaderResourceView(pTexture, nullptr, outMain);
    pTexture->Release();
    if (FAILED(hr) || !*outMain) {
      stbi_image_free(pixels);
      LogToFile(std::string("[KoreanRenderer] Failed to create SRV (main, ") + tag + ")");
      return false;
    }

    // Glow atlas: blurred copy of alpha channel
    unsigned char *glowPixels = (unsigned char *)malloc(pixelBytes);
    if (glowPixels) {
      memcpy(glowPixels, pixels, pixelBytes);
      constexpr float GLOW_BLUR_SIGMA = 2.0f;
      GaussianBlurAlpha(glowPixels, width, height, GLOW_BLUR_SIGMA);

      D3D11_SUBRESOURCE_DATA glowSub = {};
      glowSub.pSysMem = glowPixels;
      glowSub.SysMemPitch = width * 4;

      ID3D11Texture2D *pGlowTex = nullptr;
      hr = s_pDevice->CreateTexture2D(&desc, &glowSub, &pGlowTex);
      if (SUCCEEDED(hr) && pGlowTex) {
        hr = s_pDevice->CreateShaderResourceView(pGlowTex, nullptr, outGlow);
        pGlowTex->Release();
        if (SUCCEEDED(hr) && *outGlow) {
          LogToFile(std::string("[KoreanRenderer] Glow atlas created (sigma=2.0, ") + tag + ").");
        } else {
          LogToFile(std::string("[KoreanRenderer] Failed to create SRV (glow, ") + tag + ")");
        }
      } else {
        LogToFile(std::string("[KoreanRenderer] Failed to CreateTexture2D (glow, ") + tag + ")");
      }
      free(glowPixels);
    }

    stbi_image_free(pixels);
    return true;
  };

  // Slot 0 (required)
  if (!LoadAtlasAndGlow(IDR_FONT_ATLAS_0, "slot0",
                        &s_pTextureView, &s_pGlowTextureView)) {
    return;
  }

  // Slot 1 (optional, header atlas)
  if (!LoadAtlasAndGlow(IDR_FONT_ATLAS_1, "slot1",
                        &s_pTextureView2, &s_pGlowTextureView2)) {
    LogToFile("[KoreanRenderer] WARN: Embedded font_atlas2.png unavailable. slot1 -> slot0 fallback.");
  }

  // Sampler (shared)
  D3D11_SAMPLER_DESC samplerDesc = {};
  samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  samplerDesc.MinLOD = 0;
  samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

  s_pDevice->CreateSamplerState(&samplerDesc, &s_pSamplerState);
  LogToFile("[KoreanRenderer] Texture Loaded Successfully (slot0/slot1).");
}

// -----------------------------------------------------------------------------
// Unified Scale Calculation
// -----------------------------------------------------------------------------
// [USER REQUEST] Global font scale adjustment.
// 2026-01-31: Reduced to 0.45f.
// Note: Base glyph size increased from 32->42, so 0.45f gives roughly 60% of
// original 32px size.
constexpr float GLOBAL_FONT_SCALE = 0.45f;

float KoreanRenderer::CalculateFinalScale(float fontHeight, float cmdScale,
                                          float resolutionScale) {
  float safeHeight = fontHeight;
  if (s_QteScaleMode) {
    // QTE overlay: relaxed clamps so the dramatic shrink animation
    // (sizeRatio >> 1) doesn't get flattened.
    if (safeHeight > 600.0f || safeHeight < 5.0f) {
      if (safeHeight <= 1.0f)
        safeHeight = 48.0f;
      else if (safeHeight > 600.0f)
        safeHeight = 500.0f;
      else if (safeHeight < 5.0f)
        safeHeight = 20.0f;
    }
  } else {
    if (safeHeight > 150.0f || safeHeight < 5.0f) {
      if (safeHeight <= 1.0f)
        safeHeight = 48.0f;
      else if (safeHeight > 150.0f)
        safeHeight = 100.0f;
      else if (safeHeight < 5.0f)
        safeHeight = 20.0f;
    }
  }
  float baseScale = (safeHeight / 60.0f);
  float finalScale = baseScale * cmdScale * GLOBAL_FONT_SCALE;
  const float cap = s_QteScaleMode ? 5.0f : 2.2f;
  if (finalScale > cap)
    finalScale = cap;
  // Validate logical font values first; then convert to output pixels.
  // Otherwise 78 * 2 = 156 at 4K hits the >150 fallback above and becomes 100.
  const float pixelScale =
      (std::isfinite(resolutionScale) && resolutionScale > 0.0f)
          ? resolutionScale : 1.0f;
  return finalScale * pixelScale;
}

// -----------------------------------------------------------------------------
// Precise Text Width Measurement (Ex)
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Precise Text Width Measurement (Ex)
// -----------------------------------------------------------------------------
float KoreanRenderer::MeasureTextWidthEx(const std::string &text,
                                         float fontHeight, float scale,
                                         uint8_t atlasSlot,
                                         float resolutionScale) {
  float finalScale = CalculateFinalScale(fontHeight, scale, resolutionScale);

  float curWidth = 0.0f;
  float maxWidth = 0.0f;
  const char *p = text.c_str();

  while (*p) {
    if (*p == '^' && *(p + 1)) {
      p += 2;
      continue;
    }
    if (*p == '\n' || *p == '\r') {
      maxWidth = (std::max)(maxWidth, curWidth);
      curWidth = 0.0f;
      p++;
      continue;
    }

    uint32_t cp = 0;
    int bytes = KoreanAtlas::DecodeUTF8(p, cp);
    p += bytes;

    // Button glyph control chars: one square
    if (cp >= 1 && cp <= 23) {
      curWidth += KoreanAtlas::GLYPH_SIZE_BASE * finalScale * 1.4f * 1.08f;
      continue;
    }

    const KoreanAtlas::GlyphInfo *g = KoreanAtlas::GetGlyph(cp, atlasSlot);
    if (g) {
      curWidth += g->advance * finalScale;
    } else {
      // Fallback for missing glyphs (space or unknown)
      if (cp == ' ') {
        curWidth += (KoreanAtlas::GLYPH_SIZE_BASE * 0.25f) * finalScale;
      } else {
        curWidth += (KoreanAtlas::GLYPH_SIZE_BASE * 0.5f) * finalScale;
      }
    }
  }
  return (std::max)(maxWidth, curWidth);
}

void KoreanRenderer::Render() {
  if (!s_bInitialized || !s_pTextureView)
    return;

  std::lock_guard<std::mutex> lock(s_RenderMutex);

  UINT numViewports = 1;
  D3D11_VIEWPORT vp = {};
  s_pContext->RSGetViewports(&numViewports, &vp);
  if (vp.Width == 0.0f || vp.Height == 0.0f)
    return;

  const EngineSafeArea engineSafeArea = TextHook_GetEngineSafeArea();
  const MenuLayoutMetrics menuMetrics =
      ComputeMenuLayoutMetrics(vp, engineSafeArea);
  const bool inMenuContextGlobal =
      TextHook_IsMenuContext() || TextHook_IsMenuActiveRaw();
  const bool videoPlaying = BinkHook::IsCutsceneVideoPlaying();
  static bool s_LastVideoPlaying = false;
  if (videoPlaying && !s_LastVideoPlaying) {
    s_MenuItemCache.clear();
    s_DrawList.clear();
  }
  s_LastVideoPlaying = videoPlaying;

#if GHOSTSKOR_RUNTIME_DIAG
  if (inMenuContextGlobal) {
    static DWORD s_lastMenuMetricsLogTick = 0;
    const DWORD nowTick = GetTickCount();
    if (nowTick - s_lastMenuMetricsLogTick > 1200) {
      std::ostringstream oss;
      oss << "[MENU-METRICS] res=" << (int)vp.Width << "x" << (int)vp.Height
          << " safe=(" << engineSafeArea.x << "," << engineSafeArea.y << ")"
          << " content=(" << menuMetrics.contentLeft << "," << menuMetrics.contentTop
          << ")-(" << menuMetrics.contentRight << "," << menuMetrics.contentBottom
          << ") tol=" << menuMetrics.anchorTolerancePx
          << " vf=" << menuMetrics.clusterVerticalFitPx
          << " bucket=" << BucketPxInt(menuMetrics.bucketPx)
          << " hash=(" << menuMetrics.cacheBucketXPx << ","
          << menuMetrics.cacheBucketYPx << ")"
          << " clipY=(" << menuMetrics.topClipY << "," << menuMetrics.bottomClipY
          << ")";
      LogToFile(oss.str());
      s_lastMenuMetricsLogTick = nowTick;
    }
  }
#endif

  // Split staged items by source: menu (R_AddCmdDrawText) vs HUD (ProcessSubtitles).
  // Menu items use per-item cache (each item survives independently by hash).
  // HUD items always render directly (refreshed every frame by ProcessSubtitles).
  {
    std::vector<DrawCommand> menuItems, hudItems;
    for (auto &cmd : s_StageList) {
      if (videoPlaying && !cmd.keepDuringVideo)
        continue;
      if (cmd.isMenuSource)
        menuItems.push_back(std::move(cmd));
      else
        hudItems.push_back(std::move(cmd));
    }
    s_StageList.clear();

    unsigned long now = GetTickCount();
    const bool menuDiagActive =
        inMenuContextGlobal && TextHook_IsMenuDiagActive();
    const unsigned long long menuDiagSession =
        menuDiagActive ? TextHook_GetMenuDiagSessionId() : 0ull;
    const size_t cacheBeforeCount = s_MenuItemCache.size();
    int transitionNewCount = -1;
    bool transitionFlushed = false;

    // Per-item menu cache with upsert + transition detection:
    // - Each item is cached independently by (text + coarseY) hash.
    // - Fresh items are upserted: existing entries updated, new entries added.
    // - Items NOT in the current frame (alpha dip) persist from cache → no flicker.
    // - Menu transition (>50% new items): flush old items first → no ghosting.
    // - Stale items always expire after kMenuItemTTLMs → handles menu close.
    if (!menuItems.empty()) {
      // Precompute keys for fresh items
      std::vector<uint64_t> freshKeys;
      std::vector<uint64_t> freshPosKeys;
      freshKeys.reserve(menuItems.size());
      freshPosKeys.reserve(menuItems.size());
      for (const auto &cmd : menuItems) {
        freshKeys.push_back(MenuCmdHash(cmd.text, cmd.x, cmd.y,
                                        menuMetrics.cacheBucketXPx,
                                        menuMetrics.cacheBucketYPx));
        freshPosKeys.push_back(MenuCmdPosHash(cmd.x, cmd.y,
                                              menuMetrics.cacheBucketXPx,
                                              menuMetrics.cacheBucketYPx));
      }

      // Transition detection: if majority of fresh items are NOT in cache,
      // this is a menu transition → flush old items to prevent ghosting.
      if (!s_MenuItemCache.empty()) {
        std::unordered_set<uint64_t> cacheKeys;
        cacheKeys.reserve(s_MenuItemCache.size() * 2 + 8);
        for (const auto &c : s_MenuItemCache) {
          cacheKeys.insert(c.key);
        }

        int newCount = 0;
        for (auto fk : freshKeys) {
          if (cacheKeys.find(fk) == cacheKeys.end()) {
            ++newCount;
          }
        }
        transitionNewCount = newCount;
        if (newCount * 2 > (int)freshKeys.size()) {
          transitionFlushed = true;
          s_MenuItemCache.clear();
        }
      }

      // Upsert: update existing items in-place, add new ones.
      for (size_t i = 0; i < menuItems.size(); ++i) {
        uint64_t key = freshKeys[i];
        uint64_t posKey = freshPosKeys[i];
        bool found = false;
        for (auto &c : s_MenuItemCache) {
          if (c.key == key) {
            c.cmd = std::move(menuItems[i]);
            c.posKey = posKey;
            c.lastSeen = now;
            found = true;
            break;
          }
        }
        if (!found) {
          // If text changed at the same coarse position, replace the old cache
          // entry immediately (prevents 1-2 frame duplicate trails).
          for (auto &c : s_MenuItemCache) {
            if (c.posKey == posKey) {
              c.key = key;
              c.cmd = std::move(menuItems[i]);
              c.posKey = posKey;
              c.lastSeen = now;
              found = true;
              break;
            }
          }
        }
        if (!found) {
          s_MenuItemCache.push_back({key, posKey, std::move(menuItems[i]), now});
        }
      }
    }

    // Always expire stale entries (runs every frame, not just when menuItems
    // is empty — critical for menu-close cleanup and transition residue).
    {
      auto it = std::remove_if(s_MenuItemCache.begin(), s_MenuItemCache.end(),
        [now](const CachedMenuCmd &c) { return (now - c.lastSeen) > kMenuItemTTLMs; });
      s_MenuItemCache.erase(it, s_MenuItemCache.end());
    }

    // Build draw list: cached menu items + fresh HUD items.
    s_DrawList.clear();
    for (const auto &c : s_MenuItemCache)
      s_DrawList.push_back(c.cmd);
    for (auto &cmd : hudItems)
      s_DrawList.push_back(std::move(cmd));

    if (menuDiagActive) {
      static unsigned long long s_LastMenuDiagRenderSession = 0;
      static int s_MenuDiagRenderFrame = 0;
      if (menuDiagSession != s_LastMenuDiagRenderSession) {
        s_LastMenuDiagRenderSession = menuDiagSession;
        s_MenuDiagRenderFrame = 0;
      }
      if (s_MenuDiagRenderFrame < 24) {
        std::ostringstream fss;
        fss << "[MENU-DIAG-FRAME] sess=" << menuDiagSession
            << " frame=" << s_MenuDiagRenderFrame
            << " fresh=" << menuItems.size()
            << " hud=" << hudItems.size()
            << " cacheBefore=" << cacheBeforeCount
            << " cacheAfter=" << s_MenuItemCache.size()
            << " draw=" << s_DrawList.size()
            << " newCount=" << transitionNewCount
            << " flushed=" << (transitionFlushed ? 1 : 0)
            << " front=" << (TextHook_IsFrontendMenuLikely() ? 1 : 0)
            << " menu=" << (TextHook_IsMenuContext() ? 1 : 0);
        LogToFile(fss.str());

        int freshLogged = 0;
        for (size_t i = 0; i < menuItems.size() && freshLogged < 16; ++i) {
          const auto &cmd = menuItems[i];
          std::ostringstream oss;
          oss << "[MENU-DIAG-FRESH] sess=" << menuDiagSession
              << " frame=" << s_MenuDiagRenderFrame
              << " idx=" << freshLogged
              << " x=" << cmd.x
              << " y=" << cmd.y
              << " w=" << cmd.finalWidthEng
              << " a=" << cmd.a
              << " force=" << cmd.forceAlign
              << " atlas=" << (unsigned int)cmd.atlasSlot
              << " text=\"" << Utf8Preview(cmd.text, 64) << "\"";
          LogToFile(oss.str());
          ++freshLogged;
        }

        int drawLogged = 0;
        for (size_t i = 0; i < s_DrawList.size() && drawLogged < 16; ++i) {
          const auto &cmd = s_DrawList[i];
          if (!cmd.isMenuSource)
            continue;
          std::ostringstream oss;
          oss << "[MENU-DIAG-DRAW] sess=" << menuDiagSession
              << " frame=" << s_MenuDiagRenderFrame
              << " idx=" << drawLogged
              << " x=" << cmd.x
              << " y=" << cmd.y
              << " w=" << cmd.finalWidthEng
              << " a=" << cmd.a
              << " force=" << cmd.forceAlign
              << " atlas=" << (unsigned int)cmd.atlasSlot
              << " text=\"" << Utf8Preview(cmd.text, 64) << "\"";
          LogToFile(oss.str());
          ++drawLogged;
        }
        ++s_MenuDiagRenderFrame;
      }
    }
  }

  if (s_DrawList.empty())
    return;

  // 1. Setup Matrix (Use Identity + Manual NDC)
  // Identity Matrix
  XMMATRIX ortho = XMMatrixIdentity();
  ortho = XMMatrixTranspose(ortho);
  s_pContext->UpdateSubresource(s_pConstantBuffer, 0, nullptr, &ortho, 0, 0);

  // 2. Build Vertex Data
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(s_pContext->Map(s_pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0,
                             &mapped)))
    return;

  SimpleVertex *vPtr = (SimpleVertex *)mapped.pData;
  constexpr int REGION_SIZE = 49152; // 196608 / 4
  constexpr int MAIN0_START = 0;
  constexpr int MAIN1_START = REGION_SIZE;
  constexpr int GLOW0_START = REGION_SIZE * 2;
  constexpr int GLOW1_START = REGION_SIZE * 3;
  constexpr int BTN_START   = REGION_SIZE * 4; // 5th region: gamepad button glyphs
  constexpr int MAIN_MAX = REGION_SIZE;
  constexpr int GLOW_MAX = REGION_SIZE;
  constexpr int BTN_MAX  = 512; // max ~85 button icons per frame

  SimpleVertex *mainPtr0 = vPtr + MAIN0_START;
  SimpleVertex *mainPtr1 = vPtr + MAIN1_START;
  SimpleVertex *glowPtr0 = vPtr + GLOW0_START;
  SimpleVertex *glowPtr1 = vPtr + GLOW1_START;
  SimpleVertex *btnPtr   = vPtr + BTN_START;
  int mainCount0 = 0;
  int mainCount1 = 0;
  int glowCount0 = 0;
  int glowCount1 = 0;
  int btnCount   = 0;

  // === ADVANCED ALIGNMENT DETECTION (CLUSTERING) ===
  // 1. Group commands by Y-coordinate (Menu items usually share or are close in
  // Y)
  // 2. Analyze each group for alignment patterns (Left, Right, Center)

  struct AlignmentInfo {
    int type; // 0: Left, 1: Right, 2: Center
    float anchorX;
  };
  std::vector<AlignmentInfo> alignInfos(s_DrawList.size());
  for (size_t i = 0; i < s_DrawList.size(); ++i) {
    alignInfos[i].type = 0;
    alignInfos[i].anchorX = s_DrawList[i].x;
  }

  // Helper to round to bucket (resolution-aware).
  auto RoundToBucket = [&](float val) -> int {
    return QuantizeToBucket(val, menuMetrics.bucketPx);
  };

  // Simple clustering by proximity
  // We can't easily re-sort s_DrawList because draw order matters for layering.
  // So we just iterate and build groups on the fly or pre-pass.
  // Pre-pass: Build clusters
  // Pre-pass: Build clusters using Vertical Chaining
  // We want to group items that are part of the same visual block (e.g. valid
  // menu list). Strategy: Sort by Y, then group items that are vertically close
  // (< 80px).

  struct SortItem {
    int index;
    float y;
  };
  std::vector<SortItem> sortedItems;
  sortedItems.reserve(s_DrawList.size());

  // Footer/menu description classification is centralized in static keyword
  // tables (file-local helpers) so clipping and clustering stay in sync.

  // Popup overlay detection (Yes/No + center header)
  bool popupActive = false;
  bool difficultySelectPopupActive = false;
  float popupTopY = 0.0f;
  float popupBottomY = 0.0f;
  float popupCenterX = vp.Width * 0.5f;
  float popupCenterY = vp.Height * 0.5f;
  float popupBandX = vp.Width * 0.18f;

  auto TrimAscii = [](const std::string &s) -> std::string {
    size_t start = s.find_first_not_of(' ');
    if (start == std::string::npos)
      return "";
    size_t end = s.find_last_not_of(' ');
    return s.substr(start, end - start + 1);
  };

  auto StripColorCodesSimple = [](const std::string &s) -> std::string {
    std::string ret;
    ret.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '^' && i + 1 < s.size()) {
        char next = s[i + 1];
        if ((next >= '0' && next <= '9') || (next >= 'a' && next <= 'z') ||
            (next >= 'A' && next <= 'Z')) {
          i++;
          continue;
        }
      }
      ret.push_back(s[i]);
    }
    return ret;
  };

  auto StripVariantMarkersSimple = [](const std::string &s) -> std::string {
    std::string out = s;
    const std::string open = "⟦";
    const std::string close = "⟧";
    size_t start = out.find(open);
    while (start != std::string::npos) {
      size_t end = out.find(close, start + open.size());
      if (end == std::string::npos) {
        out.erase(start);
        break;
      }
      out.erase(start, end - start + close.size());
      start = out.find(open, start);
    }
    return out;
  };

  auto StripTrailingIndexSimple = [&](const std::string &s) -> std::string {
    size_t end = s.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(s[end - 1])))
      end--;

    size_t digitEnd = end;
    size_t digitStart = digitEnd;
    while (digitStart > 0 &&
           std::isdigit(static_cast<unsigned char>(s[digitStart - 1])))
      digitStart--;

    if (digitStart < digitEnd) {
      size_t spaceStart = digitStart;
      while (spaceStart > 0 &&
             std::isspace(static_cast<unsigned char>(s[spaceStart - 1])))
        spaceStart--;
      if (spaceStart < digitStart) {
        return TrimAscii(s.substr(0, spaceStart));
      }
    }

    return TrimAscii(s.substr(0, end));
  };

  auto ToUpperAsciiSimple = [](const std::string &s) -> std::string {
    std::string ret = s;
    for (char &c : ret) {
      if (c >= 'a' && c <= 'z')
        c -= 32;
    }
    return ret;
  };

  auto NormalizePopupText = [&](const std::string &s) -> std::string {
    std::string cleaned = StripColorCodesSimple(s);
    cleaned = TrimAscii(cleaned);
    cleaned = StripVariantMarkersSimple(cleaned);
    cleaned = TrimAscii(cleaned);
    cleaned = StripTrailingIndexSimple(cleaned);
    cleaned = TrimAscii(cleaned);
    return cleaned;
  };

  // --- Render-level perf diagnostic ---
  LARGE_INTEGER renderPerfStart = {}, renderPerfNormEnd = {}, renderPerfLogicEnd = {};
  LARGE_INTEGER renderPerfFreq = {};
  const int renderDetailPageId = KoreanRenderer_GetRecentRorkeDetailPage(200);
  const bool renderPerfActive = (renderDetailPageId != 0);
  if (renderPerfActive) QueryPerformanceCounter(&renderPerfStart);

  // Pre-compute normalized text cache — eliminates redundant
  // NormalizePopupText/ToUpperAsciiSimple allocations across 10+ scan passes.
  struct NormCacheEntry { std::string normalized; std::string upper; };
  std::vector<NormCacheEntry> normCache(s_DrawList.size());
  for (size_t nc_i = 0; nc_i < s_DrawList.size(); ++nc_i) {
    normCache[nc_i].normalized = NormalizePopupText(s_DrawList[nc_i].text);
    normCache[nc_i].upper = ToUpperAsciiSimple(normCache[nc_i].normalized);
  }
  if (renderPerfActive) QueryPerformanceCounter(&renderPerfNormEnd);
  // Helper: get cache index from a DrawCommand reference within s_DrawList
  auto NCIdx = [&](const DrawCommand &c) -> size_t {
    return static_cast<size_t>(&c - s_DrawList.data());
  };

  auto IsYesNoPopup = [&](const std::string &s) -> bool {
    std::string upper = ToUpperAsciiSimple(s);
    if (upper == "YES" || upper == "NO")
      return true;
    if (s == "예" || s == "예." || s == "예!" || s == "예?" ||
        s == "아니오" || s == "아니요" || s == "아뇨")
      return true;
    return false;
  };

  auto ComputeTextCenterX = [&](const DrawCommand &cmd) -> float {
    float w = cmd.finalWidthEng;
    if (w <= 1.0f)
      return cmd.x;
    return cmd.x + w * 0.5f;
  };

  auto IsNearCenterX = [&](float x) -> bool {
    return std::fabs(x - popupCenterX) <= popupBandX;
  };

  bool friendsOverlayActive = false;
  if (inMenuContextGlobal) {
    float friendsHeaderBandY = vp.Height * 0.40f;
    float friendsCenterBandX = vp.Width * 0.35f;
    for (const auto &cmd : s_DrawList) {
      if (cmd.a < 0.005f)
        continue;
      const std::string &norm = normCache[NCIdx(cmd)].normalized;
      const std::string &upper = normCache[NCIdx(cmd)].upper;
      if (norm.find("Call of Duty: Ghosts") != std::string::npos ||
          norm.find("플레이 중") != std::string::npos) {
        friendsOverlayActive = true;
        break;
      }
      float textCenterX = ComputeTextCenterX(cmd);
      bool nearCenter = std::fabs(textCenterX - popupCenterX) <= friendsCenterBandX;
      bool nearTop = cmd.y <= friendsHeaderBandY;
      if (nearCenter && nearTop && (norm.find("친구") != std::string::npos ||
          upper.find("FRIENDS") != std::string::npos)) {
        friendsOverlayActive = true;
        break;
      }
    }
  }

  if (inMenuContextGlobal) {
    int yesNoCount = 0;
    float yesNoMinY = 1e9f;
    float yesNoMaxY = -1e9f;
    float headerBestDist = 1e9f;
    float headerY = 0.0f;
    bool headerFound = false;

    float headerBandY = vp.Height * 0.35f;
    float yesNoBandY = vp.Height * 0.30f;

    for (const auto &cmd : s_DrawList) {
      if (cmd.a < 0.005f)
        continue;
      const std::string &norm = normCache[NCIdx(cmd)].normalized;
      if (norm.empty())
        continue;

      float textCenterX = ComputeTextCenterX(cmd);
      if (!IsNearCenterX(textCenterX))
        continue;

      if (IsYesNoPopup(norm)) {
        if (std::fabs(cmd.y - popupCenterY) > yesNoBandY)
          continue;
        yesNoCount++;
        if (cmd.y < yesNoMinY)
          yesNoMinY = cmd.y;
        if (cmd.y > yesNoMaxY)
          yesNoMaxY = cmd.y;
        continue;
      }

      float dist = std::fabs(cmd.y - popupCenterY);
      if (dist <= headerBandY && dist < headerBestDist) {
        headerBestDist = dist;
        headerY = cmd.y;
        headerFound = true;
      }
    }

    bool hasYesNo = (yesNoCount >= 2);
    bool hasHeader = headerFound;
    if (hasHeader && headerY > yesNoMinY + vp.Height * 0.05f)
      hasHeader = false; // header should be above yes/no

    if (hasYesNo && hasHeader) {
      float yesNoCenterY = (yesNoMinY + yesNoMaxY) * 0.5f;
      if (std::fabs(yesNoCenterY - popupCenterY) <= vp.Height * 0.25f) {
        popupActive = true;
        float padTop = vp.Height * 0.06f;
        float padBottom = vp.Height * 0.10f;
        popupTopY = (std::min)(headerY, yesNoMinY) - padTop;
        popupBottomY = (std::max)(headerY, yesNoMaxY) + padBottom;
        if (popupTopY < 0.0f)
          popupTopY = 0.0f;
        if (popupBottomY > vp.Height)
          popupBottomY = vp.Height;
        float popupHeight = popupBottomY - popupTopY;
        if (popupHeight > vp.Height * 0.55f) {
          popupActive = false; // too large (friends list etc.)
        }
      }
    }
  }

  // Difficulty selection popup detection (no Yes/No)
  if (inMenuContextGlobal && !popupActive) {
    bool diffHeaderFound = false;
    float diffHeaderY = 0.0f;
    float headerBandY = vp.Height * 0.35f;

    for (const auto &cmd : s_DrawList) {
      const std::string &norm = normCache[NCIdx(cmd)].normalized;
      if (norm.empty())
        continue;
      const std::string &upper = normCache[NCIdx(cmd)].upper;
      if (norm == "난이도 선택" || upper == "SELECT DIFFICULTY") {
        float textCenterX = ComputeTextCenterX(cmd);
        if (!IsNearCenterX(textCenterX))
          continue;
        if (std::fabs(cmd.y - popupCenterY) > headerBandY)
          continue;
        diffHeaderFound = true;
        diffHeaderY = cmd.y;
        break;
      }
    }

    if (diffHeaderFound) {
      int optionCount = 0;
      float optMinY = 1e9f;
      float optMaxY = -1e9f;
      float optionBandY = vp.Height * 0.45f;

      for (const auto &cmd : s_DrawList) {
        const std::string &norm = normCache[NCIdx(cmd)].normalized;
        if (norm.empty())
          continue;
        if (cmd.y <= diffHeaderY)
          continue;
        if (cmd.y > diffHeaderY + optionBandY)
          continue;
        float textCenterX = ComputeTextCenterX(cmd);
        if (!IsNearCenterX(textCenterX))
          continue;

        // Skip long description lines
        if (norm.size() > 16 && norm.find(' ') != std::string::npos)
          continue;
        if (norm.find('?') != std::string::npos ||
            norm.find('.') != std::string::npos)
          continue;

        optionCount++;
        if (cmd.y < optMinY)
          optMinY = cmd.y;
        if (cmd.y > optMaxY)
          optMaxY = cmd.y;
      }

      if (optionCount >= 3) {
        popupActive = true;
        difficultySelectPopupActive = true;
        float padTop = vp.Height * 0.06f;
        float padBottom = vp.Height * 0.10f;
        popupTopY = diffHeaderY - padTop;
        popupBottomY = optMaxY + padBottom;
        if (popupTopY < 0.0f)
          popupTopY = 0.0f;
        if (popupBottomY > vp.Height)
          popupBottomY = vp.Height;
        float popupHeight = popupBottomY - popupTopY;
        if (popupHeight > vp.Height * 0.55f) {
          popupActive = false;
          difficultySelectPopupActive = false;
        }
      }
    }
  }



  // Restart mission popup detection (no Yes/No)
  if (inMenuContextGlobal && !popupActive) {
    bool restartHeaderFound = false;
    float restartHeaderY = 1e9f;
    float headerBandY = vp.Height * 0.35f;

    for (const auto &cmd : s_DrawList) {
      const std::string &norm = normCache[NCIdx(cmd)].normalized;
      if (norm.empty())
        continue;
      const std::string &upper = normCache[NCIdx(cmd)].upper;
      if (norm == "임무 다시 시작" || upper == "RESTART MISSION" || upper == "MISSION RESTART") {
        float textCenterX = ComputeTextCenterX(cmd);
        if (!IsNearCenterX(textCenterX))
          continue;
        if (std::fabs(cmd.y - popupCenterY) > headerBandY)
          continue;
        restartHeaderFound = true;
        if (cmd.y < restartHeaderY)
          restartHeaderY = cmd.y;
      }
    }

    if (restartHeaderFound) {
      int optionCount = 0;
      float optMinY = 1e9f;
      float optMaxY = -1e9f;
      float optionBandY = vp.Height * 0.45f;

      for (const auto &cmd : s_DrawList) {
        const std::string &norm = normCache[NCIdx(cmd)].normalized;
        if (norm.empty())
          continue;
        if (cmd.y <= restartHeaderY)
          continue;
        if (cmd.y > restartHeaderY + optionBandY)
          continue;
        float textCenterX = ComputeTextCenterX(cmd);
        if (!IsNearCenterX(textCenterX))
          continue;

        // Skip long description lines
        if (norm.size() > 24 && norm.find(' ') != std::string::npos)
          continue;
        if (norm.find('?') != std::string::npos ||
            norm.find('.') != std::string::npos)
          continue;

        optionCount++;
        if (cmd.y < optMinY)
          optMinY = cmd.y;
        if (cmd.y > optMaxY)
          optMaxY = cmd.y;
      }

      if (optionCount >= 2) {
        popupActive = true;
        float padTop = vp.Height * 0.06f;
        float padBottom = vp.Height * 0.10f;
        popupTopY = restartHeaderY - padTop;
        popupBottomY = optMaxY + padBottom;
        if (popupTopY < 0.0f)
          popupTopY = 0.0f;
        if (popupBottomY > vp.Height)
          popupBottomY = vp.Height;
        float popupHeight = popupBottomY - popupTopY;
        if (popupHeight > vp.Height * 0.55f) {
          popupActive = false;
        }
      }
    }
  }

  // Quit/Save & Quit popup detection (no Yes/No)
  if (inMenuContextGlobal && !popupActive) {
    bool quitHeaderFound = false;
    float quitHeaderY = 1e9f;
    float headerBandY = vp.Height * 0.35f;

    for (const auto &cmd : s_DrawList) {
      const std::string &norm = normCache[NCIdx(cmd)].normalized;
      if (norm.empty())
        continue;
      const std::string &upper = normCache[NCIdx(cmd)].upper;
      bool isQuitHeader = false;
      if (norm.find("종료") != std::string::npos &&
          norm.find("겠습니까") != std::string::npos)
        isQuitHeader = true;
      if (upper.find("ARE YOU SURE") != std::string::npos &&
          (upper.find("QUIT") != std::string::npos ||
           upper.find("EXIT") != std::string::npos))
        isQuitHeader = true;
      if (upper.find("SAVE AND QUIT") != std::string::npos)
        isQuitHeader = true;
      if (!isQuitHeader)
        continue;

      float textCenterX = ComputeTextCenterX(cmd);
      if (!IsNearCenterX(textCenterX))
        continue;
      if (std::fabs(cmd.y - popupCenterY) > headerBandY)
        continue;
      quitHeaderFound = true;
      if (cmd.y < quitHeaderY)
        quitHeaderY = cmd.y;
    }

    if (quitHeaderFound) {
      int optionCount = 0;
      float optMinY = 1e9f;
      float optMaxY = -1e9f;
      float optionBandY = vp.Height * 0.45f;

      for (const auto &cmd : s_DrawList) {
        const std::string &norm = normCache[NCIdx(cmd)].normalized;
        if (norm.empty())
          continue;
        if (cmd.y <= quitHeaderY)
          continue;
        if (cmd.y > quitHeaderY + optionBandY)
          continue;
        float textCenterX = ComputeTextCenterX(cmd);
        if (!IsNearCenterX(textCenterX))
          continue;

        // Skip long description lines
        if (norm.size() > 24 && norm.find(' ') != std::string::npos)
          continue;
        if (norm.find('?') != std::string::npos ||
            norm.find('.') != std::string::npos)
          continue;

        optionCount++;
        if (cmd.y < optMinY)
          optMinY = cmd.y;
        if (cmd.y > optMaxY)
          optMaxY = cmd.y;
      }

      if (optionCount >= 2) {
        popupActive = true;
        float padTop = vp.Height * 0.06f;
        float padBottom = vp.Height * 0.10f;
        popupTopY = quitHeaderY - padTop;
        popupBottomY = optMaxY + padBottom;
        if (popupTopY < 0.0f)
          popupTopY = 0.0f;
        if (popupBottomY > vp.Height)
          popupBottomY = vp.Height;
        float popupHeight = popupBottomY - popupTopY;
        if (popupHeight > vp.Height * 0.55f) {
          popupActive = false;
        }
      }
    }
  }


  // Lower difficulty popup detection (no Yes/No)
  if (inMenuContextGlobal && !popupActive) {
    bool lowerHeaderFound = false;
    float lowerHeaderY = 0.0f;
    float headerBandY = vp.Height * 0.35f;

    for (const auto &cmd : s_DrawList) {
      const std::string &norm = normCache[NCIdx(cmd)].normalized;
      if (norm.empty())
        continue;
      const std::string &upper = normCache[NCIdx(cmd)].upper;
      if (norm == "난이도 낮추기" || upper == "LOWER DIFFICULTY" || upper == "DECREASE DIFFICULTY") {
        float textCenterX = ComputeTextCenterX(cmd);
        if (!IsNearCenterX(textCenterX))
          continue;
        if (std::fabs(cmd.y - popupCenterY) > headerBandY)
          continue;
        lowerHeaderFound = true;
        lowerHeaderY = cmd.y;
        break;
      }
    }

    if (lowerHeaderFound) {
      int optionCount = 0;
      float optMinY = 1e9f;
      float optMaxY = -1e9f;
      float optionBandY = vp.Height * 0.45f;

      for (const auto &cmd : s_DrawList) {
        const std::string &norm = normCache[NCIdx(cmd)].normalized;
        if (norm.empty())
          continue;
        if (cmd.y <= lowerHeaderY)
          continue;
        if (cmd.y > lowerHeaderY + optionBandY)
          continue;
        float textCenterX = ComputeTextCenterX(cmd);
        if (!IsNearCenterX(textCenterX))
          continue;

        // Skip long description lines
        if (norm.size() > 24 && norm.find(' ') != std::string::npos)
          continue;
        if (norm.find('?') != std::string::npos ||
            norm.find('.') != std::string::npos)
          continue;

        optionCount++;
        if (cmd.y < optMinY)
          optMinY = cmd.y;
        if (cmd.y > optMaxY)
          optMaxY = cmd.y;
      }

      if (optionCount >= 2) {
        popupActive = true;
        float padTop = vp.Height * 0.06f;
        float padBottom = vp.Height * 0.10f;
        popupTopY = lowerHeaderY - padTop;
        popupBottomY = optMaxY + padBottom;
        if (popupTopY < 0.0f)
          popupTopY = 0.0f;
        if (popupBottomY > vp.Height)
          popupBottomY = vp.Height;
        float popupHeight = popupBottomY - popupTopY;
        if (popupHeight > vp.Height * 0.55f) {
          popupActive = false;
        }
      }
    }
  }

  if (friendsOverlayActive) {
    popupActive = false;
  }
  if (inMenuContextGlobal && popupActive && !difficultySelectPopupActive) {
    const float headerBandY = vp.Height * 0.35f;
    for (const auto &cmd : s_DrawList) {
      const std::string &norm = normCache[NCIdx(cmd)].normalized;
      if (norm.empty())
        continue;
      const std::string &upper = normCache[NCIdx(cmd)].upper;
      if (norm != "난이도 선택" && upper != "SELECT DIFFICULTY")
        continue;
      float textCenterX = ComputeTextCenterX(cmd);
      if (!IsNearCenterX(textCenterX))
        continue;
      if (std::fabs(cmd.y - popupCenterY) > headerBandY)
        continue;
      difficultySelectPopupActive = true;
      break;
    }
  }

  // Share popup bounds with TextHook for next-frame precise detection
  s_PopupActiveLastFrame.store(popupActive);
  if (popupActive) {
    s_PopupTopYLastFrame.store(popupTopY);
    s_PopupBottomYLastFrame.store(popupBottomY);
    s_PopupCenterXLastFrame.store(popupCenterX);
    s_PopupBandXLastFrame.store(popupBandX);
  }

  if (inMenuContextGlobal) {
    for (size_t i = 0; i < s_DrawList.size(); ++i) {
    const auto &cmd = s_DrawList[i];

    // Menu alignment clustering must never touch HUD/video subtitle commands.
    // If this runs on subtitle readability passes, anchor quantization can
    // drift during loading/video and appear as left-offset blur growth.
    if (!cmd.isMenuSource) {
      alignInfos[i].type = 0;      // Left
      alignInfos[i].anchorX = cmd.x;
      continue;
    }

    if (cmd.a < 0.005f) {
      // Glow-only commands (alpha=0) are render helpers and should not affect
      // menu alignment clustering.
      continue;
    }

    // [FIX] Main menu descriptions should be left-aligned at their original
    // position
    if (IsMainMenuDescKeyword(cmd.text)) {
      alignInfos[i].type = 0; // Left
      alignInfos[i].anchorX = cmd.x;
      continue; // Skip clustering
    }

    // [FIX] Footer buttons should be isolated from clustering.
    // Exception: "저장 후 종료" / "SAVE AND QUIT" contain "종료"/"QUIT"
    // (footer keywords) but are pause menu BUTTONS, not footer items.
    if (IsMenuFooterKeyword(cmd.text)) {
      bool isSaveQuit = false;
      if (cmd.text.find("\xEC\xA0\x80\xEC\x9E\xA5") != std::string::npos &&
          cmd.text.find("\xEC\xA2\x85\xEB\xA3\x8C") != std::string::npos)
        isSaveQuit = true; // "저장" + "종료"
      if (!isSaveQuit) {
        std::string upper = ToUpperAsciiSimple(cmd.text);
        if (upper.find("SAVE") != std::string::npos &&
            upper.find("QUIT") != std::string::npos)
          isSaveQuit = true;
      }
      if (!isSaveQuit) {
        alignInfos[i].type = 0; // Left
        alignInfos[i].anchorX = cmd.x;
        continue; // Skip clustering
      }
    }

    // [FIX] Menu headers - just skip clustering, don't force alignment
    // This preserves original game alignment (some headers are center-aligned)
    // [FIX] Menu headers - just skip clustering
    // Robust check: Text Match AND Top of Screen (Top 22%)
    // This distinguishes "Options" header from "Options" menu item
    if (IsMenuHeaderKeyword(cmd.text, true) ||
        (IsTopOnlyHeaderKeyword(cmd.text) && cmd.y < menuMetrics.topClipY)) {
      float headerThresholdY = menuMetrics.topClipY;
      if (cmd.y < headerThresholdY) {
        continue; // It's a header -> Skip clustering
      }
    }

    // High-scale draw calls can be true headers/footers, but at higher
    // resolutions ordinary menu items may also report larger scales. Isolate
    // only when they are near top/bottom UI bands.
    if (cmd.originalXScale >= 1.15f) {
      bool nearTopUiBand = (cmd.y <= menuMetrics.topClipY);
      bool nearBottomUiBand = (cmd.y >= menuMetrics.bottomClipY);
      if (nearTopUiBand || nearBottomUiBand) {
        continue;
      }
    }

    sortedItems.push_back({(int)i, cmd.y});
    }

    // Lambda for sorting
    std::sort(sortedItems.begin(), sortedItems.end(),
              [](const SortItem &a, const SortItem &b) { return a.y < b.y; });

    struct Cluster {
      std::vector<int> indices;
      float maxY;

      // Track averages for all 3 potential anchors
      float avgLeft;
      float avgRight;
      float avgCenter;
      float count;
    };
    auto BuildClusters = [&](float anchorTolPx,
                             float verticalFitPx) -> std::vector<Cluster> {
      std::vector<Cluster> out;
      for (const auto &item : sortedItems) {
        const auto &cmd = s_DrawList[item.index];
        float l = cmd.x;
        float r = cmd.x + cmd.finalWidthEng;
        float c = (l + r) * 0.5f;

        int bestClusterIdx = -1;
        for (size_t k = 0; k < out.size(); ++k) {
          Cluster &cluster = out[k];
          bool verticalFit = (item.y - cluster.maxY < verticalFitPx);
          if (!verticalFit)
            continue;

          bool matchLeft = std::fabs(l - cluster.avgLeft) < anchorTolPx;
          bool matchRight = std::fabs(r - cluster.avgRight) < anchorTolPx;
          bool matchCenter = std::fabs(c - cluster.avgCenter) < anchorTolPx;
          if (matchLeft || matchRight || matchCenter) {
            bestClusterIdx = (int)k;
            break;
          }
        }

        if (bestClusterIdx != -1) {
          Cluster &cluster = out[bestClusterIdx];
          cluster.indices.push_back(item.index);
          cluster.maxY = item.y;
          float n = cluster.count;
          cluster.avgLeft = (cluster.avgLeft * n + l) / (n + 1);
          cluster.avgRight = (cluster.avgRight * n + r) / (n + 1);
          cluster.avgCenter = (cluster.avgCenter * n + c) / (n + 1);
          cluster.count += 1.0f;
        } else {
          out.push_back({{item.index}, item.y, l, r, c, 1.0f});
        }
      }
      return out;
    };

    float clusterAnchorTolerancePx = menuMetrics.anchorTolerancePx;
    float clusterVerticalFitPx = menuMetrics.clusterVerticalFitPx;
    std::vector<Cluster> clusters =
        BuildClusters(clusterAnchorTolerancePx, clusterVerticalFitPx);

    int multiItemClusters = 0;
    for (const auto &cluster : clusters) {
      if (cluster.indices.size() >= 2)
        multiItemClusters++;
    }

    bool relaxedClustering = false;
    if (multiItemClusters == 0 && sortedItems.size() >= 3) {
      clusterAnchorTolerancePx = ClampFloat(clusterAnchorTolerancePx * 1.75f,
                                            menuMetrics.anchorTolerancePx,
                                            menuMetrics.anchorTolerancePx * 3.0f);
      clusterVerticalFitPx = ClampFloat(clusterVerticalFitPx * 1.25f,
                                        menuMetrics.clusterVerticalFitPx,
                                        menuMetrics.clusterVerticalFitPx * 2.0f);
      clusters = BuildClusters(clusterAnchorTolerancePx, clusterVerticalFitPx);
      relaxedClustering = true;
    }


    int alignClusterCount = 0;
    int singletonClusterCount = 0;
    int alignLeftCount = 0;
    int alignRightCount = 0;
    int alignCenterCount = 0;
    float alignSampleAnchor = 0.0f;
    int alignSampleType = 0;

    // Analyze each cluster
    for (const auto &cluster : clusters) {
    if (cluster.indices.empty())
      continue;

    // [FIX] Single item -> Force Left Align (Avoid False Positives)
    if (cluster.indices.size() == 1) {
      singletonClusterCount++;
      int idx = cluster.indices[0];
      const auto &cmd = s_DrawList[idx];
      // Exception: SSAO option label should be right-aligned
      if (cmd.text.find("화면 공간 주변 폐색") != std::string::npos) {
        alignInfos[idx].type = 1; // Right
        alignInfos[idx].anchorX = cmd.x + cmd.finalWidthEng;
      } else {
        alignInfos[idx].type = 0; // Left
        alignInfos[idx].anchorX = cmd.x;
      }
      continue;
    }

    std::map<int, int> leftCounts;
    std::map<int, int> rightCounts;
    std::map<int, int> centerCounts;

    // Also track min/max leftX for variance check (to distinguish Right vs
    // Coincidental Left)
    float leftMin = 3.402823466e+38F;  // FLT_MAX
    float leftMax = -3.402823466e+38F; // -FLT_MAX

    for (int idx : cluster.indices) {
      const auto &cmd = s_DrawList[idx];
      if (cmd.finalWidthEng <= 1.0f)
        continue;

      float left = cmd.x;
      float right = cmd.x + cmd.finalWidthEng;
      float center = (left + right) * 0.5f;

      leftCounts[RoundToBucket(left)]++;
      rightCounts[RoundToBucket(right)]++;
      centerCounts[RoundToBucket(center)]++;

      if (left < leftMin)
        leftMin = left;
      if (left > leftMax)
        leftMax = left;
    }

    // Find dominant anchors (Bucket Index)
    int bestRightC = 0, bestRightBucket = 0;
    for (auto p : rightCounts)
      if (p.second > bestRightC) {
        bestRightC = p.second;
        bestRightBucket = p.first;
      }

    int bestLeftC = 0, bestLeftBucket = 0;
    for (auto p : leftCounts)
      if (p.second > bestLeftC) {
        bestLeftC = p.second;
        bestLeftBucket = p.first;
      }

    int bestCenterC = 0, bestCenterBucket = 0;
    for (auto p : centerCounts)
      if (p.second > bestCenterC) {
        bestCenterC = p.second;
        bestCenterBucket = p.first;
      }

    // [FIX] Calculate TRUE AVERAGE ANCHOR (Not Bucket Index!)
    auto CalculateTrueAnchor = [&](int bucket, int mode) -> float {
      float sum = 0.0f;
      int count = 0;
      for (int idx : cluster.indices) {
        const auto &cmd = s_DrawList[idx];
        if (cmd.finalWidthEng <= 1.0f)
          continue;

        float val = 0.0f;
        if (mode == 0)
          val = cmd.x; // Left
        else if (mode == 1)
          val = cmd.x + cmd.finalWidthEng; // Right
        else if (mode == 2)
          val = (cmd.x * 2.0f + cmd.finalWidthEng) * 0.5f; // Center

        if (RoundToBucket(val) == bucket) {
          sum += val;
          count++;
        }
      }
      return count > 0 ? (sum / count) : 0.0f;
    };

    // [FIX] Correct Priority: VOTE BASED (Max Score Wins)
    // Rationale: Do not force Center if Right score is overwhelmingly higher.

    int scoreRight = (leftMax - leftMin > menuMetrics.leftVarianceThresholdPx)
                         ? bestRightC
                         : 0; // Variance check mandatory for Right
    int scoreCenter = bestCenterC;
    int scoreLeft = bestLeftC;

    // Determine winner
    int winner = 0; // Default Left
    int maxScore = scoreLeft;

    // Right vs Left (Favor Right if tie or greater)
    if (scoreRight > maxScore) {
      winner = 1;
      maxScore = scoreRight;
    }

    // Center vs Current Max (Favor Center if strictly greater)
    // Ghost's Center alignment usually has exact matches, so score should be
    // high.
    if (scoreCenter > maxScore) {
      winner = 2;
      maxScore = scoreCenter;
    } else if (scoreCenter == maxScore && winner == 1) {
      // Tie Right vs Center logic... prefer Right for menu lists
      winner = 1;
    }

    // Apply winner
    int type = winner;
    float anchor = 0.0f;

    if (type == 1)
      anchor = CalculateTrueAnchor(bestRightBucket, 1);
    else if (type == 2)
      anchor = CalculateTrueAnchor(bestCenterBucket, 2);
    else
      anchor = CalculateTrueAnchor(bestLeftBucket, 0);

      alignClusterCount++;
      if (type == 1)
        alignRightCount++;
      else if (type == 2)
        alignCenterCount++;
      else
        alignLeftCount++;
      if (alignSampleAnchor == 0.0f) {
        alignSampleAnchor = anchor;
        alignSampleType = type;
      }

      // Apply to all in cluster
      for (int idx : cluster.indices) {
      alignInfos[idx].type = type;
      if (bestCenterC >= 2 ||
          (bestRightC >= 2 &&
           (leftMax - leftMin > menuMetrics.leftVarianceThresholdPx)) ||
          bestLeftC >= 2) {
        alignInfos[idx].anchorX = anchor;
      } else {
        alignInfos[idx].anchorX = s_DrawList[idx].x;
      }
    }
  }

    static DWORD s_lastMenuAlignLogTick = 0;
    DWORD nowAlignTick = GetTickCount();
    if (nowAlignTick - s_lastMenuAlignLogTick > 1200) {
      std::ostringstream oss;
      oss << "[MENU-ALIGN] clusters=" << alignClusterCount << " L="
          << alignLeftCount << " R=" << alignRightCount
          << " C=" << alignCenterCount << " single=" << singletonClusterCount
          << " relaxed=" << (relaxedClustering ? 1 : 0)
          << " sampleType=" << alignSampleType
          << " sampleAnchor=" << alignSampleAnchor
          << " tol=" << clusterAnchorTolerancePx
          << " vf=" << clusterVerticalFitPx;
      LogToFile(oss.str());
      s_lastMenuAlignLogTick = nowAlignTick;
    }
  }

  std::vector<bool> popupMask;
  if (popupActive) {
    popupMask.assign(s_DrawList.size(), false);
    int popupCount = 0;
    for (size_t i = 0; i < s_DrawList.size(); ++i) {
      const auto &cmd = s_DrawList[i];
      if (cmd.y < popupTopY || cmd.y > popupBottomY)
        continue;
      const std::string &norm = normCache[i].normalized;
      const std::string &upper = normCache[i].upper;
      bool isPopupDesc = (!norm.empty() &&
          (norm.find("?") != std::string::npos ||
           norm.find("\uC2B5\uB2C8\uAE4C") != std::string::npos ||
           norm.find("\uACA0\uC2B5\uB2C8\uAE4C") != std::string::npos ||
           norm.find("\uB09C\uC774\uB3C4") != std::string::npos ||
           norm.find("\uCCB4\uD06C\uD3EC\uC778\uD2B8") != std::string::npos ||
           norm.find("\uC9C4\uD589 \uC0C1\uD669") != std::string::npos ||
           norm.find("\uC800\uC7A5") != std::string::npos ||
           norm.find("\uC885\uB8CC") != std::string::npos ||
           upper.find("ARE YOU SURE") != std::string::npos ||
           upper.find("CHECKPOINT") != std::string::npos ||
           upper.find("PROGRESS") != std::string::npos ||
           upper.find("SAVE") != std::string::npos ||
           upper.find("QUIT") != std::string::npos ||
           upper.find("RESTART") != std::string::npos ||
           upper.find("DIFFICULTY") != std::string::npos ||
           upper.find("APPLY") != std::string::npos ||
           upper.find("SETTINGS") != std::string::npos));
      bool isPopupSpecial =
          (norm == "\uC784\uBB34 \uB2E4\uC2DC \uC2DC\uC791" ||
           upper == "RESTART MISSION" || upper == "MISSION RESTART" ||
           norm == "\uB09C\uC774\uB3C4 \uB0AE\uCD94\uAE30" ||
           upper == "LOWER DIFFICULTY" || upper == "DECREASE DIFFICULTY");
      if (norm.find("\uACE0\uAE09 \uBE44\uB514\uC624") != std::string::npos ||
          upper.find("ADVANCED VIDEO") != std::string::npos) {
        continue; // never treat Advanced Video as popup content
      }
      float textCenterX = ComputeTextCenterX(cmd);
      bool nearCenter = IsNearCenterX(textCenterX);
      if (!nearCenter && !isPopupDesc && !isPopupSpecial)
        continue;
      float anchorX = alignInfos[i].anchorX;
      bool anchorNear = std::fabs(anchorX - popupCenterX) <= popupBandX * 0.85f;
      if (!isPopupDesc && !anchorNear && alignInfos[i].type != 2 &&
          !isPopupSpecial)
        continue;
      popupMask[i] = true;
      popupCount++;
    }
    if (popupCount < 2) {
      popupActive = false;
      popupMask.clear();
    }
  }

  if (popupActive) {
    for (size_t i = 0; i < s_DrawList.size(); ++i) {
      if (i < popupMask.size() && popupMask[i]) {
        alignInfos[i].type = 2; // Center
        alignInfos[i].anchorX = popupCenterX;
      }
    }
  }

  // Process Commands
  int cmdIndex = -1;
  int clipTopCount = 0;
  int clipBottomCount = 0;
  int clipMissionCount = 0;
  int clipPopupCount = 0;
  const float missionBLRight = menuMetrics.contentLeft + menuMetrics.missionBLWidth;
  const float missionBLTopY = menuMetrics.contentBottom - menuMetrics.missionBLHeight;
  const float missionHeaderBandY =
      menuMetrics.contentTop + (menuMetrics.topClipY - menuMetrics.contentTop) * 1.8f;

  static bool s_MissionSelectSticky = false;
  static DWORD s_MissionSelectLastSeenTick = 0;
  bool missionSelectSeenThisFrame = false;
  static bool s_RorkeFilesSticky = false;
  static DWORD s_RorkeFilesLastSeenTick = 0;
  bool rorkeFilesSeenThisFrame = false;
  static bool s_RorkeDetailSticky = false;
  static DWORD s_RorkeDetailLastSeenTick = 0;
  bool rorkeDetailSeenThisFrame = false;
  if (inMenuContextGlobal) {
    for (const auto &scanCmd : s_DrawList) {
      if (scanCmd.a < 0.005f)
        continue;
      const std::string &norm = normCache[NCIdx(scanCmd)].normalized;
      if (norm.empty())
        continue;
      const std::string &upper = normCache[NCIdx(scanCmd)].upper;
      if ((norm.find("임무 선택") != std::string::npos ||
           upper.find("MISSION SELECT") != std::string::npos) &&
          scanCmd.y <= missionHeaderBandY) {
        missionSelectSeenThisFrame = true;
      }
      // "로크 파일" as page HEADER only (top zone) — NOT as campaign list item
      if ((norm.find("로크 파일") != std::string::npos ||
           upper.find("RORKE FILES") != std::string::npos) &&
          scanCmd.y <= missionHeaderBandY) {
        rorkeFilesSeenThisFrame = true;
      }
      // "이전/다음 이미지" footer — exclusive to Rorke Files detail view
      if (norm.find("이전 이미지") != std::string::npos ||
          norm.find("다음 이미지") != std::string::npos ||
          upper.find("PREVIOUS IMAGE") != std::string::npos ||
          upper.find("NEXT IMAGE") != std::string::npos) {
        rorkeFilesSeenThisFrame = true;
        rorkeDetailSeenThisFrame = true;
      }
      if (missionSelectSeenThisFrame && rorkeFilesSeenThisFrame)
        break;
    }
  }
  DWORD missionNowTick = GetTickCount();
  if (missionSelectSeenThisFrame) {
    s_MissionSelectSticky = true;
    s_MissionSelectLastSeenTick = missionNowTick;
  } else if (missionNowTick - s_MissionSelectLastSeenTick > 500) {
    s_MissionSelectSticky = false;
  }
  const bool missionSelectActive = s_MissionSelectSticky;

  if (rorkeFilesSeenThisFrame) {
    s_RorkeFilesSticky = true;
    s_RorkeFilesLastSeenTick = missionNowTick;
  } else if (missionNowTick - s_RorkeFilesLastSeenTick > 500) {
    s_RorkeFilesSticky = false;
  }
  const bool rorkeFilesActive = s_RorkeFilesSticky;
  if (rorkeFilesSeenThisFrame && !rorkeDetailSeenThisFrame) {
    s_RorkeDetailSticky = false;
    s_RorkeDetailLastSeenTick = 0;
    KoreanRenderer_ClearRorkeDetailState();
  }
  if (rorkeDetailSeenThisFrame) {
    s_RorkeDetailSticky = true;
    s_RorkeDetailLastSeenTick = missionNowTick;
    g_RorkeDetailActiveTick.store(missionNowTick);
  } else if (missionNowTick - s_RorkeDetailLastSeenTick > 120) {
    s_RorkeDetailSticky = false;
    KoreanRenderer_ClearRorkeDetailState();
  }
  const int rorkeDetailPageId = KoreanRenderer_GetRecentRorkeDetailPage(120);
  const bool rorkeDetailActive =
      s_RorkeDetailSticky || (rorkeDetailPageId != 0);
  // Rorke header/body: handled by O(1) prefix map in TextHook (ADDCMD hook).
  // Header animation fix: x-latch + piggyback from body text calls.
  // Legacy Render()-side injection/erase removed.

  if (alignInfos.size() < s_DrawList.size()) {
    const size_t oldSize = alignInfos.size();
    alignInfos.resize(s_DrawList.size());
    for (size_t i = oldSize; i < s_DrawList.size(); ++i) {
      alignInfos[i].type = 0;
      alignInfos[i].anchorX = s_DrawList[i].x;
    }
  }
  if (renderPerfActive) QueryPerformanceCounter(&renderPerfLogicEnd);
  bool advancedVideoActive = false;
  if (inMenuContextGlobal) {
    for (const auto &scanCmd : s_DrawList) {
      if (scanCmd.a < 0.005f)
        continue;
      const std::string &norm = normCache[NCIdx(scanCmd)].normalized;
      if (norm.empty())
        continue;
      const std::string &upper = normCache[NCIdx(scanCmd)].upper;
      if ((norm.find("고급 비디오") != std::string::npos ||
           upper.find("ADVANCED VIDEO") != std::string::npos) &&
          scanCmd.y <= missionHeaderBandY) {
        advancedVideoActive = true;
        break;
      }
    }
  }

  static bool s_lastMissionSelectLogged = false;
  static DWORD s_lastMissionStateLogTick = 0;
  if (inMenuContextGlobal &&
      (missionSelectActive != s_lastMissionSelectLogged ||
       missionNowTick - s_lastMissionStateLogTick > 1500)) {
    std::ostringstream moss;
    moss << "[MENU-MISSION] active=" << (missionSelectActive ? 1 : 0)
         << " seenFrame=" << (missionSelectSeenThisFrame ? 1 : 0)
         << " headerBandY=" << missionHeaderBandY
         << " rectRight=" << missionBLRight
         << " rectTopY=" << missionBLTopY;
    LogToFile(moss.str());
    s_lastMissionSelectLogged = missionSelectActive;
    s_lastMissionStateLogTick = missionNowTick;
  }
  for (const auto &cmd : s_DrawList) {
    cmdIndex++;
    std::string renderText = cmd.text;
    const std::string &normalizedCmdText = normCache[cmdIndex].normalized;
    const std::string &normalizedCmdUpper = normCache[cmdIndex].upper;
    const bool isRegularDifficultyLabel =
        (normalizedCmdText == "일반" || normalizedCmdUpper == "REGULAR");
    const bool replaceRegularWithNormal =
        inMenuContextGlobal && cmd.isMenuSource &&
        (missionSelectActive || difficultySelectPopupActive) &&
        isRegularDifficultyLabel;
    if (replaceRegularWithNormal) {
      renderText = "보통";
    }

    s_QteScaleMode = cmd.qteScaleMode;
    const float finalScale = CalculateFinalScale(
        cmd.fontHeight, cmd.scale, cmd.resolutionScale);
    s_QteScaleMode = false;
    const float effectiveGlobalOffset = menuMetrics.globalYOffsetPx;
    const float effectiveSmallOffset = menuMetrics.smallMenuYOffsetPx;
    const float effectiveYOffset =
        effectiveGlobalOffset + (cmd.isSmallMenu ? effectiveSmallOffset : 0.0f);

    const bool skipClipping =
        !inMenuContextGlobal || !cmd.isMenuSource || friendsOverlayActive ||
        cmd.skipMenuClipping;

    if (!skipClipping) {
      const float textY = cmd.y + effectiveYOffset;
      const float textX = cmd.x;
      const bool isHeader =
          IsMenuHeaderKeyword(renderText, rorkeFilesActive) ||
          (IsTopOnlyHeaderKeyword(renderText) && textY < menuMetrics.topClipY);
      const std::string diffNorm = replaceRegularWithNormal ? NormalizePopupText(renderText) : normCache[cmdIndex].normalized;
      const std::string diffUpper = replaceRegularWithNormal ? ToUpperAsciiSimple(diffNorm) : normCache[cmdIndex].upper;
      const bool isDifficultyLike = IsDifficultyLabel(diffNorm, diffUpper);
      bool isFooter = IsMenuFooterKeyword(renderText);
      // Advanced Video detail list can include off-screen "일반/NORMAL" items
      // while scrolling. Do not treat those as footer exemptions.
      if (advancedVideoActive && isDifficultyLike) {
        isFooter = false;
      }
      const bool isDifficultyLabelPopup =
          popupActive && isDifficultyLike;

      if (!isDifficultyLabelPopup) {
        if (popupActive) {
          const bool allowOutside = IsPopupOutsideAllowKeyword(renderText);
          if ((cmdIndex < 0 || cmdIndex >= (int)popupMask.size() ||
               !popupMask[cmdIndex]) &&
              !allowOutside) {
            clipPopupCount++;
            continue; // Hide non-popup text when overlay is active
          }
        }

        // Keep clipping strict and resolution-stable: whitelist controls the
        // escape path, not originalXScale (which varies across resolutions).
        if (textY < menuMetrics.topClipY && !isHeader) {
          clipTopCount++;
          continue; // Scroll content in header zone - skip
        }

        if (textY > menuMetrics.bottomClipY && !isFooter) {
          clipBottomCount++;
          continue; // Scroll content in footer zone - skip
        }

        if (missionSelectActive) {
          if (textX < missionBLRight && textY > missionBLTopY && !isFooter &&
              !isHeader) {
            clipMissionCount++;
            continue; // Skip rendering - protect map area
          }
        }
      }
    }

    uint8_t renderAtlasSlot =
        (cmd.atlasSlot == KoreanAtlas::kAtlasSlotHeader)
            ? KoreanAtlas::kAtlasSlotHeader
            : KoreanAtlas::kAtlasSlotBase;
    bool forcedHeaderByMissionCenterTitle = false;
    const bool atlas1Ready =
        s_Atlas2MetricsLoaded && (KoreanAtlas::GetGlyphCount(KoreanAtlas::kAtlasSlotHeader) > 0) &&
        (s_pTextureView2 != nullptr);
    if (renderAtlasSlot == KoreanAtlas::kAtlasSlotHeader && !atlas1Ready) {
      renderAtlasSlot = KoreanAtlas::kAtlasSlotBase;
      if (!s_Atlas2FallbackWarned) {
        LogToFile("[KoreanRenderer] WARN: slot1 requested but atlas2 not ready. Falling back to slot0.");
        s_Atlas2FallbackWarned = true;
      }
    }

    // Mission Select detail title (center panel) should use header atlas.
    // Keep left mission list untouched by constraining to center-title band.
    if (inMenuContextGlobal && missionSelectActive && cmd.isMenuSource) {
      const std::string norm = replaceRegularWithNormal ? NormalizePopupText(renderText) : normCache[cmdIndex].normalized;
      const std::string upper = replaceRegularWithNormal ? ToUpperAsciiSimple(norm) : normCache[cmdIndex].upper;
      const float contentW = menuMetrics.contentRight - menuMetrics.contentLeft;
      const float contentH = menuMetrics.contentBottom - menuMetrics.contentTop;
      const bool inCenterTitleBand =
          cmd.x >= (menuMetrics.contentLeft + contentW * 0.32f) &&
          cmd.x <= (menuMetrics.contentLeft + contentW * 0.70f) &&
          cmd.y >= (menuMetrics.contentTop + contentH * 0.66f) &&
          cmd.y <= (menuMetrics.contentTop + contentH * 0.84f);
      const bool looksLikeMissionTitle =
          !norm.empty() && norm.size() <= 24 &&
          norm.find('?') == std::string::npos &&
          norm.find('.') == std::string::npos &&
          norm.find('!') == std::string::npos &&
          upper.find("RORKE FILE") == std::string::npos &&
          !IsDifficultyLabel(norm, upper) &&
          cmd.finalWidthEng > 1.0f &&
          cmd.finalWidthEng <= contentW * 0.45f;
      if (inCenterTitleBand && looksLikeMissionTitle) {
        renderAtlasSlot = KoreanAtlas::kAtlasSlotHeader;
        forcedHeaderByMissionCenterTitle = true;
      }
    }

    // Per-command glyph fallback: if slot1 misses any glyph in this command,
    // route the whole command to slot0 for stability.
    if (renderAtlasSlot == KoreanAtlas::kAtlasSlotHeader) {
      const char *scan = renderText.c_str();
      bool hasMissingGlyph = false;
      while (*scan) {
        if (*scan == '^' && *(scan + 1)) {
          char code = *(scan + 1);
          if ((code >= '0' && code <= '9') || (code >= 'a' && code <= 'z') ||
              (code >= 'A' && code <= 'Z')) {
            scan += 2;
            continue;
          }
        }
        if (*scan == ' ' || *scan == '\n' || *scan == '\r') {
          scan++;
          continue;
        }
        uint32_t cp = 0;
        int bytes = KoreanAtlas::DecodeUTF8(scan, cp);
        if (bytes <= 0) {
          scan++;
          continue;
        }
        scan += bytes;
        if (!KoreanAtlas::GetGlyph(cp, KoreanAtlas::kAtlasSlotHeader)) {
          hasMissingGlyph = true;
          break;
        }
      }
      if (hasMissingGlyph) {
        // Narrow exception only for pause-menu Rorke status lines.
        const bool keepHeaderAtlasForPauseRorkeStatus =
            inMenuContextGlobal && !missionSelectActive && cmd.isMenuSource &&
            IsPauseRorkeStatusTextStrict(renderText);
        if (!keepHeaderAtlasForPauseRorkeStatus &&
            !forcedHeaderByMissionCenterTitle) {
          renderAtlasSlot = KoreanAtlas::kAtlasSlotBase;
        }
      }
    }

    SimpleVertex *mainPtr = (renderAtlasSlot == KoreanAtlas::kAtlasSlotHeader)
                                ? mainPtr1
                                : mainPtr0;
    int *mainCountPtr = (renderAtlasSlot == KoreanAtlas::kAtlasSlotHeader)
                            ? &mainCount1
                            : &mainCount0;
    SimpleVertex *glowPtr = (renderAtlasSlot == KoreanAtlas::kAtlasSlotHeader)
                                ? glowPtr1
                                : glowPtr0;
    int *glowCountPtr = (renderAtlasSlot == KoreanAtlas::kAtlasSlotHeader)
                            ? &glowCount1
                            : &glowCount0;
    const int atlasWidth = KoreanAtlas::GetAtlasWidth(renderAtlasSlot);
    const int atlasHeight = KoreanAtlas::GetAtlasHeight(renderAtlasSlot);

    // === ALIGNMENT APPLICATION ===
    int alignType = alignInfos[cmdIndex].type;
    float anchorX = alignInfos[cmdIndex].anchorX;

    if (popupActive) {
      const std::string &norm = normCache[cmdIndex].normalized;
      const std::string &upper = normCache[cmdIndex].upper;
      if (norm == "\uC784\uBB34 \uB2E4\uC2DC \uC2DC\uC791" ||
          upper == "RESTART MISSION" || upper == "MISSION RESTART") {
        float textCenterX = ComputeTextCenterX(cmd);
        if (std::fabs(textCenterX - popupCenterX) <= popupBandX * 1.2f) {
          alignType = 2;
          anchorX = popupCenterX;
        }
      }
    }

    if (cmd.forceAlign >= 0 && cmd.forceAlign <= 2) {
      alignType = cmd.forceAlign;
      if (cmd.forceAlign == 1)
        anchorX = cmd.x + cmd.finalWidthEng; // Right: anchor at English right edge
      else if (cmd.forceAlign == 2)
        anchorX = cmd.x + cmd.finalWidthEng * 0.5f; // Center
      else
        anchorX = cmd.x; // Left
    }

    // For fallback cases (single items), ensure anchor is correct
    if (alignType == 0 && anchorX == 0.0f)
      anchorX = cmd.x;

    // === WRAPPING LOGIC ===
    // 1. Tokenize and measure to build lines
    struct Line {
      std::string text;
      float width;
    };
    std::vector<Line> lines;

    // Heuristic: Max width before wrapping.
    // [FIX] Use 95% of screen width to avoid premature wrapping of long
    // sentences.
    float maxWidth = (cmd.maxWidthPx > 1.0f) ? cmd.maxWidthPx
                                              : ((float)vp.Width * 0.95f);

    if (!cmd.disableAutoWrap && popupActive && cmdIndex >= 0 &&
        cmdIndex < (int)popupMask.size() && popupMask[cmdIndex]) {
      maxWidth = (float)vp.Width * 0.65f;
    }

    // Autosave notice popup: force line break, center align, nudge down.
    // Only active when "알림" header is present in the same frame.
    bool isAutosaveBody = false;
    bool hasNoticeHeader = false;
    for (const auto &other : s_DrawList) {
      if (other.a < 0.005f) continue;
      const std::string &otherNorm = normCache[NCIdx(other)].normalized;
      if (otherNorm == "알림") { hasNoticeHeader = true; break; }
    }
    if (hasNoticeHeader) {
      if (renderText.find("저장합니다") != std::string::npos &&
          renderText.find("전원을 끄지") != std::string::npos &&
          renderText.find('\n') == std::string::npos) {
        size_t pos = renderText.find("'저장");
        if (pos != std::string::npos && pos > 0) {
          renderText.insert(pos, "\n");
        }
        alignType = 2;
        anchorX = (float)vp.Width * 0.5f;
        isAutosaveBody = true;
      }
      if (renderText == "알림" || renderText == "계속") {
        alignType = 2;
        anchorX = (float)vp.Width * 0.5f;
      }
    }


    if (cmd.disableAutoWrap) {
      lines.push_back(
          {renderText, KoreanRenderer::MeasureTextWidthEx(renderText, cmd.fontHeight,
                                                          cmd.scale, renderAtlasSlot,
                                                          cmd.resolutionScale)});
    } else {
      std::string currentLine;
      float currentLineWidth = 0.0f;

      const char *p = renderText.c_str();

      // === ROBUST WRAPPING LOGIC (FIXED) ===
      std::string word;
      float wordWidth = 0.0f;

      // Updated space width
      float spaceWidth = (KoreanAtlas::GLYPH_SIZE_BASE * 0.25f) * finalScale;
      const KoreanAtlas::GlyphInfo *spaceG =
          KoreanAtlas::GetGlyph(' ', renderAtlasSlot);
      if (spaceG) {
        spaceWidth = spaceG->advance * finalScale;
      }

      auto CommitLine = [&](const std::string &txt, float w) {
        if (!txt.empty() || w > 0)
          lines.push_back({txt, w});
      };

      while (*p) {
        // 1. Handle Newline (Highest Priority)
        if (*p == '\n') {
          if (!word.empty()) {
            currentLine += word;
            currentLineWidth += wordWidth;
            word = "";
            wordWidth = 0.0f;
          }
          // Always push line on \n, even if empty (preserves \n\n paragraph gaps)
          lines.push_back({currentLine, currentLineWidth});
          currentLine = "";
          currentLineWidth = 0.0f;
          p++;
          continue;
        }
        if (*p == '\r') {
          p++;
          continue;
        }

        // 2. Handle Color Codes
        if (*p == '^' && *(p + 1)) {
          word += *p;
          word += *(p + 1);
          p += 2;
          continue;
        }

        // 3. Handle Space
        if (*p == ' ') {
          if (currentLineWidth + wordWidth > maxWidth && !currentLine.empty()) {
            CommitLine(currentLine, currentLineWidth);
            currentLine = "";
            currentLineWidth = 0.0f;
          }
          currentLine += word;
          currentLineWidth += wordWidth;
          currentLine += ' ';
          currentLineWidth += spaceWidth;
          word = "";
          wordWidth = 0.0f;
          p++;
          continue;
        }

        // 4. Normal Char
        uint32_t cp = 0;
        int bytes = KoreanAtlas::DecodeUTF8(p, cp);
        for (int k = 0; k < bytes; k++)
          word += *(p + k);
        p += bytes;

        // Button glyph control chars: advance by one button-size square
        if (cp >= 1 && cp <= 23) {
          float btnSz = KoreanAtlas::GLYPH_SIZE_BASE * finalScale * 1.4f;
          wordWidth += btnSz + KoreanAtlas::GLYPH_SIZE_BASE * finalScale * 1.4f * 0.08f;
          continue;
        }

        const KoreanAtlas::GlyphInfo *g =
            KoreanAtlas::GetGlyph(cp, renderAtlasSlot);
        if (g)
          wordWidth += g->advance * finalScale;
        else
          wordWidth += (KoreanAtlas::GLYPH_SIZE_BASE * 0.5f) * finalScale;

        // Auto-wrap for very long words
        if (currentLineWidth + wordWidth > maxWidth && !currentLine.empty()) {
          CommitLine(currentLine, currentLineWidth);
          currentLine = "";
          currentLineWidth = 0.0f;
        }
      } // End While

      // Flush remaining
      if (!word.empty()) {
        if (currentLineWidth + wordWidth > maxWidth && !currentLine.empty()) {
          CommitLine(currentLine, currentLineWidth);
          currentLine = "";
          currentLineWidth = 0.0f;
        }
        currentLine += word;
        currentLineWidth += wordWidth;
      }
      if (!currentLine.empty()) {
        CommitLine(currentLine, currentLineWidth);
      }
    }

    // Fallback for empty text (space?)
    if (lines.empty())
      continue;

    // === RENDER LINES ===
    float lineHeight = KoreanAtlas::GLYPH_SIZE_BASE * finalScale * 1.2f;
    float cursorY;
    if (cmd.yIsTopOfText) {
      // Caller passes Y as top-of-text (native engine convention for HUD).
      // Convert to baseline by adding ascent. Skip the global menu offset.
      float ascent = KoreanAtlas::GLYPH_SIZE_BASE * finalScale * 0.85f;
      cursorY = cmd.y + ascent;
    } else {
      // Legacy path: Y is approximate baseline, apply global menu offset.
      cursorY = cmd.y + effectiveYOffset;
    }

    // Autosave body: nudge down by half a line to balance vertical space.
    if (isAutosaveBody) {
      cursorY += lineHeight * 0.5f;
    }

    int &mainCount = *mainCountPtr;
    int &glowCount = *glowCountPtr;

    for (const auto &line : lines) {
      if (mainCount + 6 >= MAIN_MAX)
        break;

      float cursorX = 0.0f;

      // Calculate cursor position based on detected alignment
      if (alignType == 1) {
        // Right-Aligned
        cursorX = anchorX - line.width;
      } else if (alignType == 2) {
        // Center-Aligned
        cursorX = anchorX - (line.width * 0.5f);
      } else {
        // Left-Aligned (Default)
        cursorX = anchorX;
      }

      // Render the line content
      const char *lp_start = line.text.c_str();
      const char *lp = lp_start;

      // Shadow Check - Always draw shadow unless explicitly disabled
      bool drawShadow = !cmd.disableShadow;
      float shadowOffset = 2.0f * finalScale; // 2px offset for subtle shadow

      // MAIN TEXT PASS (with integrated shadow)
      float curR = cmd.r, curG = cmd.g, curB = cmd.b, curA = cmd.a;
      char curCode = '7'; // Track active ^X color code for per-glyph glow

      // Fix Vertical Alignment (from previous fix)
      float drawY =
          cursorY - (KoreanAtlas::GLYPH_SIZE_BASE * finalScale * 0.85f);

      while (*lp) {
        if (mainCount + 6 >= MAIN_MAX)
          break;

        if (*lp == '^' && *(lp + 1)) {

          char code = *(lp + 1);
          // Color logic (Copied from original)
          if ((code >= '0' && code <= '9') || (code >= 'a' && code <= 'z') ||
              (code >= 'A' && code <= 'Z')) {
            curCode = code;
            switch (code) {
            case '0':
              curR = 0;
              curG = 0;
              curB = 0;
              break;
            case '1':
              if (cmd.overrideColorCode1) {
                curR = cmd.code1R;
                curG = cmd.code1G;
                curB = cmd.code1B;
              } else {
                curR = 1;
                curG = 0;
                curB = 0;
              }
              break;
            case '2':
              if (cmd.overrideColorCode2) {
                curR = cmd.code2R;
                curG = cmd.code2G;
                curB = cmd.code2B;
              } else {
                curR = 0;
                curG = 1;
                curB = 0;
              }
              break;
            case '3':
              curR = 1;
              curG = 1;
              curB = 0;
              break;
            case '4':
              curR = 0;
              curG = 0;
              curB = 1;
              break;
            case '5':
              curR = 0;
              curG = 1;
              curB = 1;
              break;
            case '6':
              curR = 1;
              curG = 0;
              curB = 1;
              break;
            case '7':
              // IW6: ^7 is "default" for the current draw call (not always pure
              // white). For subtitles we want ^7 to reset to the base text tint.
              if (cmd.code7ResetsToBase) {
                curR = cmd.r;
                curG = cmd.g;
                curB = cmd.b;
              } else {
                curR = 1;
                curG = 1;
                curB = 1;
              }
              break;
            default:
              curR = 1;
              curG = 1;
              curB = 1;
              break;
            }
            lp += 2;
            continue;
          }
        }

        if (*lp == ' ') {
          // Space width fallback or from metrics?
          // Pretendard space is usually handled by metrics if we mapped 0x20.
          const KoreanAtlas::GlyphInfo *g =
              KoreanAtlas::GetGlyph(' ', renderAtlasSlot);
          if (g) {
            cursorX += g->advance * finalScale;
          } else {
            cursorX += (KoreanAtlas::GLYPH_SIZE_BASE * 0.25f) * finalScale;
          }
          lp++;
          continue;
        }

        // Skip newline and carriage return characters
        if (*lp == '\n' || *lp == '\r') {
          lp++;
          continue;
        }

        uint32_t cp = 0;
        int bytes = KoreanAtlas::DecodeUTF8(lp, cp);
        lp += bytes;

        // ── Gamepad button glyph (codepoints 1–23) ───────────────────────────
        // Rendered using the captured game font atlas SRV via the btn region.
        if (cp >= 1 && cp <= 23) {
          if (btnCount + 6 < BTN_MAX && GamepadGlyphAtlas::IsReady()) {
            GamepadGlyphAtlas::GlyphUV guv =
                GamepadGlyphAtlas::GetGlyphUV((unsigned char)cp);
            if (guv.valid) {
              // Render as a 1.4x square, vertically centered on the text line
              float lineH = KoreanAtlas::GLYPH_SIZE_BASE * finalScale;
              float btnSz = lineH * 1.4f;
              float bL    = cursorX;
              float bT    = drawY - (btnSz - lineH) * 0.5f; // center on text line
              float bR    = bL + btnSz;
              float bB    = bT + btnSz;
              float ndcL  = (bL / vp.Width)  * 2.0f - 1.0f;
              float ndcR  = (bR / vp.Width)  * 2.0f - 1.0f;
              float ndcT  = 1.0f - (bT / vp.Height) * 2.0f;
              float ndcB  = 1.0f - (bB / vp.Height) * 2.0f;
              // White vertex color — let the texture's full color show through
              XMFLOAT4 white{curA, curA, curA, curA};
              // Triangle 1
              btnPtr[btnCount++] = {XMFLOAT2(ndcL,ndcT), white, XMFLOAT2(guv.s0,guv.t0)};
              btnPtr[btnCount++] = {XMFLOAT2(ndcR,ndcT), white, XMFLOAT2(guv.s1,guv.t0)};
              btnPtr[btnCount++] = {XMFLOAT2(ndcL,ndcB), white, XMFLOAT2(guv.s0,guv.t1)};
              // Triangle 2
              btnPtr[btnCount++] = {XMFLOAT2(ndcR,ndcT), white, XMFLOAT2(guv.s1,guv.t0)};
              btnPtr[btnCount++] = {XMFLOAT2(ndcR,ndcB), white, XMFLOAT2(guv.s1,guv.t1)};
              btnPtr[btnCount++] = {XMFLOAT2(ndcL,ndcB), white, XMFLOAT2(guv.s0,guv.t1)};

              cursorX += btnSz + KoreanAtlas::GLYPH_SIZE_BASE * finalScale * 1.4f * 0.08f;
              continue;
            }
          }
          // Atlas not ready or glyph missing — skip char silently
          cursorX += KoreanAtlas::GLYPH_SIZE_BASE * finalScale * 1.4f * 0.6f;
          continue;
        }

        // Get Glyph Info
        const KoreanAtlas::GlyphInfo *g =
            KoreanAtlas::GetGlyph(cp, renderAtlasSlot);
        if (g) {
          // Normalized UVs from metrics
          float fU0 = g->u0 / 65535.0f;
          float fV0 = g->v0 / 65535.0f;
          float fU1 = g->u1 / 65535.0f;
          float fV1 = g->v1 / 65535.0f;

          // Calculate geometric size
          // Note: The atlas generator defines u1-u0 as width.
          // We can infer width/height from UVs or use stored logic?
          // The metrics don't store w/h directly in GlyphInfo struct I defined
          // in C++ (wait, I removed w/h from struct to save space, but I have
          // u0/u1). Actually I kept w/h in python but removed from C++ struct.
          // Let's use UV difference times Atlas Size.

          // Better: Use the UVs directly.
          // Quad Width = (g->u1 - g->u0) / 65535.0f * ATLAS_WIDTH * finalScale?
          // No. Use the physical pixel size if we had it. We can deduce pixel
          // size from UVs. w_px = (g->u1 - g->u0) / 65535.0f *
          // KoreanAtlas::g_atlasWidth;

          // However, scaling:
          // We render at 'finalScale'.
          // The glyph in atlas is 'height' pixels tall (GLYPH_SIZE_BASE).
          // So we should maintain the aspect ratio.

          // To be safe and crisp:
          // Draw width = metric.width * finalScale
          // Draw height = metric.height * finalScale
          // I don't have width/height in GlyphInfo struct.
          // I should rely on UVs.

          // But I need to apply offsets (Bearings).
          // DrawX = cursorX + (g->xOffset * finalScale)
          // DrawY = drawY + (g->yOffset * finalScale) (Note: yOffset is usually
          // from baseline up, or from top down? Python script `bearingY` is
          // usually from baseline UP in freetype. Wait. FreeType bitmap_top is
          // distance from baseline to top-most pixel. So if we draw from
          // Top-Left: Top = BaselineY - bitmap_top But our `drawY` is top of
          // the line? Let's define the baseline reference.

          // Let's assume `drawY` is the "Line Top" or "Line Center"?
          // Current code: `float drawY = cursorY - (KoreanAtlas::GLYPH_SIZE *
          // finalScale * 0.85f);` This implies cursorY is the BASELINE
          // (roughly).

          // Let's assume cursorY is the BASELINE.
          // Then:
          // Quad Top = cursorY - (g->yOffset * finalScale);
          // Quad Left = cursorX + (g->xOffset * finalScale);

          // UV Dimensions
          float texW = (g->u1 - g->u0) / 65535.0f * atlasWidth;
          float texH = (g->v1 - g->v0) / 65535.0f * atlasHeight;

          float screenW = texW * finalScale;
          float screenH = texH * finalScale;

          // Position (Baseline relative)
          // yOffset in FreeType is "bitmap_top" (distance from baseline to
          // top). So Top is (Baseline - yOffset).
          float finalX = cursorX + (g->xOffset * finalScale);
          float finalY = cursorY - (g->yOffset * finalScale);

          // Per-glyph clipping is now handled by the hardcoded region check
          // at the beginning of the command processing loop

          // NDC Conversion
          float ndcL = (finalX / vp.Width) * 2.0f - 1.0f;
          float ndcR = ((finalX + screenW) / vp.Width) * 2.0f - 1.0f;
          float ndcT = 1.0f - (finalY / vp.Height) * 2.0f;
          float ndcB = 1.0f - ((finalY + screenH) / vp.Height) * 2.0f;

          // GLOW: IW6 GSC native — ^2 name glyphs only (or all glyphs
          // when glowForceAll is set for countdown timer background).
          if (cmd.glowA > 0.005f && (curCode == '2' || cmd.glowForceAll)) {
            auto clamp01 = [](float v) -> float {
              if (v < 0.0f) return 0.0f;
              if (v > 1.0f) return 1.0f;
              return v;
            };

            float gR = 0.30f;
            float gG = 0.60f;
            float gB = 0.30f;
            if (cmd.glowUseGlyphColor) {
              gR = clamp01(curR * cmd.glowMul);
              gG = clamp01(curG * cmd.glowMul);
              gB = clamp01(curB * cmd.glowMul);
            } else if (cmd.glowR > 0.001f || cmd.glowG > 0.001f ||
                       cmd.glowB > 0.001f) {
              gR = clamp01(cmd.glowR);
              gG = clamp01(cmd.glowG);
              gB = clamp01(cmd.glowB);
            }

            float glowStrength = cmd.glowA / 0.70f;
            if (glowStrength < 0.0f) {
              glowStrength = 0.0f;
            } else if (glowStrength > 2.0f) {
              glowStrength = 2.0f;
            }

            float maxGa = (cmd.glowAlphaMax > 0.0f) ? cmd.glowAlphaMax : 0.25f;
            float gA = 0.10f * glowStrength;
            if (!cmd.glowIgnoreTextAlpha) {
              gA *= curA;
            }
            if (gA > maxGa) {
              gA = maxGa;
            }

            if (gA > 0.003f) {
              float glowOff = (cmd.glowSpread > 0.0f) ? cmd.glowSpread
                                                       : 2.0f * finalScale;
              float glowShiftX = cmd.glowOffsetX * finalScale;
              float glowShiftY = cmd.glowOffsetY * finalScale;
              static const float dirs8[8][2] = {
                  {-1.0f, 0.0f},      {1.0f, 0.0f},
                  {0.0f, -1.0f},      {0.0f, 1.0f},
                  {-0.707f, -0.707f}, {0.707f, -0.707f},
                  {-0.707f, 0.707f},  {0.707f, 0.707f}};

              for (int d = 0; d < 8; d++) {
                if (glowCount + 6 >= GLOW_MAX)
                  break;
                float gx = finalX + glowShiftX + dirs8[d][0] * glowOff;
                float gy = finalY + glowShiftY + dirs8[d][1] * glowOff;
                float gl = (gx / vp.Width) * 2.0f - 1.0f;
                float gr = ((gx + screenW) / vp.Width) * 2.0f - 1.0f;
                float gt = 1.0f - (gy / vp.Height) * 2.0f;
                float gb = 1.0f - ((gy + screenH) / vp.Height) * 2.0f;

                glowPtr[glowCount++] = {
                    XMFLOAT2(gl, gt), XMFLOAT4(gR, gG, gB, gA),
                    XMFLOAT2(fU0, fV0)};
                glowPtr[glowCount++] = {
                    XMFLOAT2(gr, gt), XMFLOAT4(gR, gG, gB, gA),
                    XMFLOAT2(fU1, fV0)};
                glowPtr[glowCount++] = {
                    XMFLOAT2(gl, gb), XMFLOAT4(gR, gG, gB, gA),
                    XMFLOAT2(fU0, fV1)};
                glowPtr[glowCount++] = {
                    XMFLOAT2(gr, gt), XMFLOAT4(gR, gG, gB, gA),
                    XMFLOAT2(fU1, fV0)};
                glowPtr[glowCount++] = {
                    XMFLOAT2(gr, gb), XMFLOAT4(gR, gG, gB, gA),
                    XMFLOAT2(fU1, fV1)};
                glowPtr[glowCount++] = {
                    XMFLOAT2(gl, gb), XMFLOAT4(gR, gG, gB, gA),
                    XMFLOAT2(fU0, fV1)};
              }
            }
          }

          // SHADOW: Draw shadow first (offset to bottom-right)
          if (drawShadow && mainCount + 12 < MAIN_MAX) {
            float shadowX = finalX + shadowOffset;
            float shadowY = finalY + shadowOffset;
            float sndcL = (shadowX / vp.Width) * 2.0f - 1.0f;
            float sndcR = ((shadowX + screenW) / vp.Width) * 2.0f - 1.0f;
            float sndcT = 1.0f - (shadowY / vp.Height) * 2.0f;
            float sndcB = 1.0f - ((shadowY + screenH) / vp.Height) * 2.0f;

            float shadowR = 0.0f, shadowG = 0.0f, shadowB = 0.0f;
            float shadowA = curA * 0.7f; // Slightly transparent shadow

            mainPtr[mainCount++] = {XMFLOAT2(sndcL, sndcT),
                              XMFLOAT4(shadowR, shadowG, shadowB, shadowA),
                              XMFLOAT2(fU0, fV0)};
            mainPtr[mainCount++] = {XMFLOAT2(sndcR, sndcT),
                              XMFLOAT4(shadowR, shadowG, shadowB, shadowA),
                              XMFLOAT2(fU1, fV0)};
            mainPtr[mainCount++] = {XMFLOAT2(sndcL, sndcB),
                              XMFLOAT4(shadowR, shadowG, shadowB, shadowA),
                              XMFLOAT2(fU0, fV1)};
            mainPtr[mainCount++] = {XMFLOAT2(sndcR, sndcT),
                              XMFLOAT4(shadowR, shadowG, shadowB, shadowA),
                              XMFLOAT2(fU1, fV0)};
            mainPtr[mainCount++] = {XMFLOAT2(sndcR, sndcB),
                              XMFLOAT4(shadowR, shadowG, shadowB, shadowA),
                              XMFLOAT2(fU1, fV1)};
            mainPtr[mainCount++] = {XMFLOAT2(sndcL, sndcB),
                              XMFLOAT4(shadowR, shadowG, shadowB, shadowA),
                              XMFLOAT2(fU0, fV1)};
          }

          // Draw Quad (Triangle List: TL, TR, BL, TR, BR, BL)
          // Triangle 1: TL, TR, BL
          mainPtr[mainCount++] = {XMFLOAT2(ndcL, ndcT),
                            XMFLOAT4(curR, curG, curB, curA),
                            XMFLOAT2(fU0, fV0)};
          mainPtr[mainCount++] = {XMFLOAT2(ndcR, ndcT),
                            XMFLOAT4(curR, curG, curB, curA),
                            XMFLOAT2(fU1, fV0)};
          mainPtr[mainCount++] = {XMFLOAT2(ndcL, ndcB),
                            XMFLOAT4(curR, curG, curB, curA),
                            XMFLOAT2(fU0, fV1)};

          // Triangle 2: TR, BR, BL
          mainPtr[mainCount++] = {XMFLOAT2(ndcR, ndcT),
                            XMFLOAT4(curR, curG, curB, curA),
                            XMFLOAT2(fU1, fV0)};
          mainPtr[mainCount++] = {XMFLOAT2(ndcR, ndcB),
                            XMFLOAT4(curR, curG, curB, curA),
                            XMFLOAT2(fU1, fV1)};
          mainPtr[mainCount++] = {XMFLOAT2(ndcL, ndcB),
                            XMFLOAT4(curR, curG, curB, curA),
                            XMFLOAT2(fU0, fV1)};

          cursorX += g->advance * finalScale;
        } else {
          // Missing glyph?
          cursorX += (KoreanAtlas::GLYPH_SIZE_BASE * 0.5f) * finalScale;
        }
      } // End Char Loop

      // Next Line
      cursorY += lineHeight;

    } // End Line Loop
  } // End Command Loop

  if (inMenuContextGlobal) {
    static DWORD s_lastMenuClipLogTick = 0;
    DWORD nowClipTick = GetTickCount();
    int totalClip = clipTopCount + clipBottomCount + clipMissionCount + clipPopupCount;
    if (totalClip > 0 && nowClipTick - s_lastMenuClipLogTick > 1200) {
      std::ostringstream oss;
      oss << "[MENU-CLIP-REASON] top=" << clipTopCount
          << " bottom=" << clipBottomCount << " mission=" << clipMissionCount
          << " popup=" << clipPopupCount << " total=" << totalClip
          << " missionActive=" << (missionSelectActive ? 1 : 0)
          << " missionRect=(" << missionBLRight << "," << missionBLTopY << ")";
      LogToFile(oss.str());
      s_lastMenuClipLogTick = nowClipTick;
    }
  }

  {
    static DWORD s_lastAtlasUseLogTick = 0;
    DWORD nowAtlasTick = GetTickCount();
    if (nowAtlasTick - s_lastAtlasUseLogTick > 2000) {
      std::ostringstream aoss;
      aoss << "[KoreanRenderer] AtlasUsage main0=" << mainCount0
           << " main1=" << mainCount1
           << " glow0=" << glowCount0
           << " glow1=" << glowCount1
           << " slot1_ready=" << ((s_Atlas2MetricsLoaded && s_pTextureView2) ? 1 : 0);
      LogToFile(aoss.str());
      s_lastAtlasUseLogTick = nowAtlasTick;
    }
  }

  // --- Rorke detail render perf summary (throttled 1/sec) ---
  if (renderPerfActive && renderPerfStart.QuadPart != 0) {
    LARGE_INTEGER renderPerfEnd;
    QueryPerformanceCounter(&renderPerfEnd);
    QueryPerformanceFrequency(&renderPerfFreq);
    static DWORD s_lastRenderPerfLogTick = 0;
    static int64_t s_renderTotalUs = 0;
    static int64_t s_renderNormUs = 0;
    static int64_t s_renderLogicUs = 0;
    static int s_renderFrames = 0;
    const int64_t totalUs = ((renderPerfEnd.QuadPart - renderPerfStart.QuadPart) * 1000000) / renderPerfFreq.QuadPart;
    const int64_t normUs = (renderPerfNormEnd.QuadPart > 0)
        ? ((renderPerfNormEnd.QuadPart - renderPerfStart.QuadPart) * 1000000) / renderPerfFreq.QuadPart : 0;
    const int64_t logicUs = (renderPerfLogicEnd.QuadPart > 0)
        ? ((renderPerfLogicEnd.QuadPart - renderPerfNormEnd.QuadPart) * 1000000) / renderPerfFreq.QuadPart : 0;
    s_renderTotalUs += totalUs;
    s_renderNormUs += normUs;
    s_renderLogicUs += logicUs;
    s_renderFrames++;
    const DWORD rpNow = GetTickCount();
    if (rpNow - s_lastRenderPerfLogTick > 1000) {
      char rpBuf[400];
      sprintf_s(rpBuf,
                "[RENDER-PERF] page=%d frames=%d drawList=%zu totalUs/sec=%lld normUs/sec=%lld logicUs/sec=%lld restUs/sec=%lld avgUs/frame=%lld",
                renderDetailPageId, s_renderFrames, s_DrawList.size(),
                (long long)s_renderTotalUs, (long long)s_renderNormUs,
                (long long)s_renderLogicUs,
                (long long)(s_renderTotalUs - s_renderNormUs - s_renderLogicUs),
                s_renderFrames > 0 ? (long long)(s_renderTotalUs / s_renderFrames) : 0LL);
      LogToFile(rpBuf);
      s_renderTotalUs = 0;
      s_renderNormUs = 0;
      s_renderLogicUs = 0;
      s_renderFrames = 0;
      s_lastRenderPerfLogTick = rpNow;
    }
  }

  s_pContext->Unmap(s_pVertexBuffer, 0);

  // 3. Set States
  float blendFactor[] = {0, 0, 0, 0};
  s_pContext->OMSetBlendState(s_pBlendState, blendFactor, 0xFFFFFFFF);
  s_pContext->RSSetState(s_pRasterizerState);

  // Disable depth testing — overlay must render on top of 3D scene.
  // Without this, in-game pause menu text flickers due to leftover depth state.
  if (s_pNoDepthStencilState)
    s_pContext->OMSetDepthStencilState(s_pNoDepthStencilState, 0);

  // Set full-screen scissor rect (since ScissorEnable is true)
  D3D11_RECT fullScreenScissor = {0, 0, (LONG)vp.Width, (LONG)vp.Height};
  s_pContext->RSSetScissorRects(1, &fullScreenScissor);

  // 4. Set Shaders
  s_pContext->VSSetShader(s_pVS, nullptr, 0);
  s_pContext->PSSetShader(s_pPS, nullptr, 0);
  s_pContext->VSSetConstantBuffers(0, 1, &s_pConstantBuffer);

  s_pContext->PSSetSamplers(0, 1, &s_pSamplerState);

  // 5. Set Input Assembly
  UINT stride = sizeof(SimpleVertex);
  UINT offset = 0;
  s_pContext->IASetVertexBuffers(0, 1, &s_pVertexBuffer, &stride, &offset);
  s_pContext->IASetInputLayout(s_pInputLayout);
  s_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // 6. Draw — IW engine order: GLOW (blurred atlas) → MAIN (sharp atlas).
  // Glow uses pre-blurred atlas (like IW Font_s.glowMaterial) for soft halo.
  // Main uses sharp atlas for shadow + text.  Both standard alpha blending.

  if (glowCount0 > 0 && s_pGlowTextureView) {
    s_pContext->PSSetShaderResources(0, 1, &s_pGlowTextureView);
    s_pContext->OMSetBlendState(s_pBlendState, blendFactor, 0xFFFFFFFF);
    s_pContext->Draw(glowCount0, GLOW0_START);
  }

  if (glowCount1 > 0 && s_pGlowTextureView2) {
    s_pContext->PSSetShaderResources(0, 1, &s_pGlowTextureView2);
    s_pContext->OMSetBlendState(s_pBlendState, blendFactor, 0xFFFFFFFF);
    s_pContext->Draw(glowCount1, GLOW1_START);
  }

  if (mainCount0 > 0) {
    s_pContext->PSSetShaderResources(0, 1, &s_pTextureView);
    s_pContext->OMSetBlendState(s_pBlendState, blendFactor, 0xFFFFFFFF);
    s_pContext->Draw(mainCount0, MAIN0_START);
  }

  if (mainCount1 > 0 && s_pTextureView2) {
    s_pContext->PSSetShaderResources(0, 1, &s_pTextureView2);
    s_pContext->OMSetBlendState(s_pBlendState, blendFactor, 0xFFFFFFFF);
    s_pContext->Draw(mainCount1, MAIN1_START);
  }

  // Button glyph pass — drawn last (on top) using the game's font atlas SRV.
  if (btnCount > 0) {
    ID3D11ShaderResourceView *btnSRV = GamepadGlyphAtlas::GetSRV();
    if (btnSRV) {
      s_pContext->PSSetShaderResources(0, 1, &btnSRV);
      s_pContext->OMSetBlendState(s_pBlendState, blendFactor, 0xFFFFFFFF);
      s_pContext->Draw(btnCount, BTN_START);
    }
  }

  // s_DrawList is NOT cleared here — retained for next frame if no new commands
  // arrive (prevents flicker when R_AddCmdDrawText fires intermittently).
}



