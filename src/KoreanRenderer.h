#pragma once
#include "Utils.h"
#include <cstdint>
#include <d3d11.h>
#include <mutex>
#include <string>
#include <vector>

struct DrawCommand {
  float x, y;
  float scale;
  float r, g, b, a;
  float fontHeight;
  int style;
  float finalWidthEng; // Added to store calculated English text width
  std::string text;
  bool disableShadow;
  bool isSmallMenu;
  bool code7ResetsToBase;
  bool disableAutoWrap;
  int forceAlign;         // -1 auto, 0 left, 1 right, 2 center
  float maxWidthPx;       // <=0 uses renderer default
  bool skipMenuClipping;  // bypass menu clipping heuristics for this command

  // Subtitle-only: allow overriding certain IW color codes with engine-driven
  // UI colors (avoids neon primary colors while keeping ^-markup).
  bool overrideColorCode1; // ^1 (hostile/enemy name)
  float code1R, code1G, code1B;
  bool overrideColorCode2; // ^2 (friendly speaker name)
  float code2R, code2G, code2B;

  // Original xScale from game engine (before GetMenuFontScaleMul adjustment)
  // Used to distinguish real header buttons from scroll content
  float originalXScale;

  // Glow effect (IW6 native: Font_s.glowMaterial rendered behind main text)
  float glowR, glowG, glowB, glowA; // Glow color+alpha (0 = no glow)
  bool glowUseGlyphColor;           // Use current glyph color (after ^x codes)
  float glowMul;                    // Optional multiplier when using glyph color
  bool glowIgnoreTextAlpha;         // Glow alpha ignores cmd.a (for glow-only)
  float glowOffsetX, glowOffsetY;   // Offset applied to glow quad only
  bool glowAdditive;                // Additive blend for glow pass (vs alpha blend)
  bool glowForceAll;               // Apply glow to ALL glyphs, not just ^2
  float glowSpread;                // Override glow offset distance (px), 0=default
  float glowAlphaMax;              // Override per-tap alpha cap, 0=default(0.25)

  // Source tracking: true when queued from R_AddCmdDrawText (menu text),
  // false when queued from D3D11Hook_ProcessSubtitles (in-game HUD/subtitles).
  bool isMenuSource;

  // Only video subtitle commands should survive while a Bink cutscene is active.
  bool keepDuringVideo;

  // Y coordinate semantics: when true, cmd.y is the TOP of the text box
  // (native engine convention for HUD elements). The renderer will add
  // ascent internally to convert to baseline. The global -7.0f menu
  // offset is NOT applied. When false (default), cmd.y is treated as
  // baseline with the legacy global offset applied.
  bool yIsTopOfText;

  // Atlas slot: 0 = base (Pretendard), 1 = header (PyeongChang).
  uint8_t atlasSlot;

  // QTE overlay: use relaxed CalculateFinalScale clamps for this command.
  bool qteScaleMode;
};

struct DrawStylePatch {
  bool overrideColorCode1 = false;
  float code1R = 1.0f;
  float code1G = 0.0f;
  float code1B = 0.0f;

  bool overrideColorCode2 = false;
  float code2R = 0.0f;
  float code2G = 1.0f;
  float code2B = 0.0f;

  bool applyGlow = false;
  float glowR = 0.0f;
  float glowG = 0.0f;
  float glowB = 0.0f;
  float glowA = 0.0f;
  bool glowUseGlyphColor = false;
  float glowMul = 1.0f;
  bool glowIgnoreTextAlpha = false;
  float glowOffsetX = 0.0f;
  float glowOffsetY = 0.0f;
  bool glowAdditive = false;
  bool glowForceAll = false;       // Apply glow to ALL glyphs, not just ^2
  float glowSpread = 0.0f;        // Override glow offset distance (px), 0=default
  float glowAlphaMax = 0.0f;      // Override per-tap alpha cap, 0=default(0.25)
};

class KoreanRenderer {
public:
  // Core
  static ID3D11Device *s_pDevice;
  static ID3D11DeviceContext *s_pContext;

  // Resources (Initialized ONCE)
  static ID3D11VertexShader *s_pVS;
  static ID3D11PixelShader *s_pPS;
  static ID3D11InputLayout *s_pInputLayout;
  static ID3D11RasterizerState *s_pRasterizerState;
  static ID3D11BlendState *s_pBlendState;
  static ID3D11BlendState *s_pBlendStateAdditive;
  static ID3D11DepthStencilState *s_pNoDepthStencilState;

  // Buffers (Initialized Once)
  static ID3D11Buffer *s_pVertexBuffer;
  static ID3D11Buffer *s_pConstantBuffer;

  // Texture Resources
  static ID3D11ShaderResourceView *s_pTextureView;
  static ID3D11ShaderResourceView *s_pGlowTextureView; // Blurred atlas for glow
  static ID3D11ShaderResourceView *s_pTextureView2;
  static ID3D11ShaderResourceView *s_pGlowTextureView2;
  static ID3D11SamplerState *s_pSamplerState;

  // Data
  static std::vector<DrawCommand> s_DrawList;
  static std::mutex s_RenderMutex;
  static bool s_bInitialized;

  // Methods
  static void Init(ID3D11Device *pDevice, ID3D11DeviceContext *pContext);
  static void Cleanup(); // To release resources

  // Unified Scale Calculation Helper
  static float CalculateFinalScale(float fontHeight, float cmdScale);
  // QTE overlay: temporarily raise CalculateFinalScale clamps.
  static void SetQteScaleMode(bool on);

  // Measure Text Width with unified scale
  static float MeasureTextWidthEx(const std::string &text, float fontHeight,
                                  float scale, uint8_t atlasSlot = 0);

  static void QueueText(const std::string &text, float x, float y, float scale,
                        const float *color, float fontHeight, int style,
                        float finalWidthEng, bool disableShadow,
                        bool isSmallMenu, float originalXScale,
                        bool allowZeroAlpha = false,
                        bool code7ResetsToBase = false,
                        bool disableAutoWrap = false, int forceAlign = -1,
                        float maxWidthPx = 0.0f,
                        bool skipMenuClipping = false,
                        bool yIsTopOfText = false, uint8_t atlasSlot = 0,
                        const DrawStylePatch *stylePatch = nullptr);

  // Set glow on the most recently queued command (IW6 TextStyle.Shadowed)
  static void SetLastCommandGlow(float r, float g, float b, float a);
  static void SetLastCommandGlowAdvanced(float r, float g, float b, float a,
                                         bool useGlyphColor, float mul,
                                         bool ignoreTextAlpha, float offX,
                                         float offY);
  static void SetLastCommandGlowAdditive(bool additive);
  static void SetLastCommandColorCode1Override(float r, float g, float b);
  static void SetLastCommandColorCode2Override(float r, float g, float b);

  static void Render();

private:
  static void CreateShaders();
  static void CreateBuffers();
  static void CreateStates();
  static void CreateTexture();
};

void KoreanRenderer_LatchRorkeDetailPage(int pageId);
void KoreanRenderer_ClearRorkeDetailState();
int KoreanRenderer_GetRecentRorkeDetailPage(unsigned long maxAgeMs);
bool KoreanRenderer_IsRecentRorkeDetailActive(unsigned long maxAgeMs);
void KoreanRenderer_LatchRorkeDetailHeaderLayout(int pageId, float x, float y,
                                                 float fontHeight,
                                                 float scale);
void KoreanRenderer_LatchRorkeReferenceHeaderLayout(float x, float y,
                                                    float fontHeight,
                                                    float scale);
void KoreanRenderer_LatchRorkeDetailBodyLayout(int pageId, float x, float y,
                                               float fontHeight,
                                               float scale);

