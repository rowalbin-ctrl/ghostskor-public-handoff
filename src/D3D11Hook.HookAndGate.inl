// Native Hook Helper (Isolated SEH for C2712 Fix)
// =========================================================================
// =========================================================================
// Native Hook Helper (VTable Swap implementation)
// =========================================================================
// =========================================================================
// Native Hook Helper (VTable Swap implementation)
// =========================================================================

// Helper to isolate SEH from C++ Unwinding (Fix C2712)
bool SafeVTableSwap(void **pSlot, void *pNewFunc, void **pOriginalStore) {
  __try {
    // Validation: Check if address looks valid (User-mode range)
    uintptr_t addr = (uintptr_t)*pSlot;
    if ((addr >> 40) != 0x7FF && (addr >> 40) != 0x7F) {
      return false;
    }

    // Save Original
    *pOriginalStore = *pSlot;

    // VTable Swap
    DWORD oldProtect;
    if (VirtualProtect(pSlot, sizeof(void *), PAGE_EXECUTE_READWRITE,
                       &oldProtect)) {
      *pSlot = pNewFunc;
      VirtualProtect(pSlot, sizeof(void *), oldProtect, &oldProtect);
      return true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return false;
}

// =========================================================================
// =========================================================================
// Hook_Present (The Core)
// =========================================================================
void D3D11Hook_ProcessSubtitles(IDXGISwapChain *pSwapChain);

extern std::atomic<bool> g_bDXGIWrapperActive;

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain *pSwapChain,
                                       UINT SyncInterval, UINT Flags) {
  if (!GameBuild::RuntimeReady())
    return Original_Present(pSwapChain, SyncInterval, Flags);
  // 0. DXGIWrapper path is active: it owns rendering, so do not duplicate it.
  //    Hook_Present was installed on WrappedIDXGISwapChain::VTable[8], so
  //    Original_Present == WrappedIDXGISwapChain::Present which handles
  //    everything correctly (correct RTV, no stale g_pBackBufferRTV).
  if (g_bDXGIWrapperActive.load(std::memory_order_acquire)) {
    return Original_Present(pSwapChain, SyncInterval, Flags);
  }

  // 1. Get Device and Context direct from SwapChain
  ID3D11Device *pDevice = nullptr;
  ID3D11DeviceContext *pContext = nullptr;

  if (SUCCEEDED(
          pSwapChain->GetDevice(__uuidof(ID3D11Device), (void **)&pDevice))) {
    pDevice->GetImmediateContext(&pContext);

    // 2. Renderer Initialization
    static bool bRendererInit = false;
    if (!bRendererInit) {
      KoreanRenderer::Init(pDevice, pContext);
      bRendererInit = true;
      LogToFile("[D3D11Hook] Renderer Initialized in Present.");
    }

    // 3. Ensure RTV
    if (!g_pBackBufferRTV) {
      pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                            (void **)&g_pBackBuffer);
      if (g_pBackBuffer) {
        pDevice->CreateRenderTargetView(g_pBackBuffer, NULL, &g_pBackBufferRTV);
        g_pBackBuffer->Release();
        g_pBackBuffer = nullptr;
        LogToFile("[D3D11Hook] BackBuffer RTV Created.");
      }
    }

    // 4. Save State
    D3D11StateSaver saver(pContext);
    saver.Save();

    // 5. Force Bind RTV
    if (g_pBackBufferRTV) {
      pContext->OMSetRenderTargets(1, &g_pBackBufferRTV, NULL);

      g_FrameSwapChainDescValid = SUCCEEDED(pSwapChain->GetDesc(&g_FrameSwapChainDesc));
      const DXGI_SWAP_CHAIN_DESC &scDesc = g_FrameSwapChainDesc;
      g_ActiveArea = Compute16x9ActiveArea(
          (float)scDesc.BufferDesc.Width, (float)scDesc.BufferDesc.Height);

      D3D11_VIEWPORT vp;
      vp.TopLeftX = 0;
      vp.TopLeftY = 0;
      vp.Width = (float)scDesc.BufferDesc.Width;
      vp.Height = (float)scDesc.BufferDesc.Height;
      vp.MinDepth = 0.0f;
      vp.MaxDepth = 1.0f;
      pContext->RSSetViewports(1, &vp);
    }

    // 6. Process Subtitles (Native Effects + Video Subs)
    D3D11Hook_ProcessSubtitles(pSwapChain);

    // 7. Draw Overlay
    KoreanRenderer::Render();

    // 8. Restore State
    saver.Restore();

    pContext->Release();
    pDevice->Release();
  }

  // 0. Beep Verification (Removed)
  static bool bOnce = false;
  if (!bOnce) {
    LogToFile("[D3D11Hook] Present Hook Active.");
    bOnce = true;
  }

  return Original_Present(pSwapChain, SyncInterval, Flags);
}

// =========================================================================
// D3D11Hook::Init - Universal VTable Hook for Present
// =========================================================================
// =========================================================================
// Hook_D3D11CreateDeviceAndSwapChain
// =========================================================================
HRESULT WINAPI Hook_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext) {

  LogToFile("[D3D11Hook] Intercepted D3D11CreateDeviceAndSwapChain!");

  HRESULT hr = Original_D3D11CreateDeviceAndSwapChain(
      pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
      SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel,
      ppImmediateContext);

  if (SUCCEEDED(hr)) {
    LogToFile("[D3D11Hook] Game Device Created Successfully.");

    if (ppSwapChain && *ppSwapChain) {
      // We have the SwapChain! Hook Present VTable!
      // We can use the simple VTable hook here because we have the instance.
      void **vtable = *(void ***)(*ppSwapChain);
      if (SafeVTableSwap(&vtable[8], (void *)Hook_Present,
                         (void **)&Original_Present)) {
        LogToFile(
            "[D3D11Hook] VTable Hook Installed on Game SwapChain (Present).");
      } else {
        LogToFile("[D3D11Hook] Failed to VTable Hook Present.");
      }
    }

  }

  return hr;
}

// =========================================================================
// D3D11Hook::Init - IAT Hook
// =========================================================================
// =========================================================================
// D3D11Hook::Init - System API Detour (Safe Async)
// =========================================================================
void D3D11Hook::Init() {
  LogToFile("[D3D11Hook] Initializing via System API Detour (Safe Async)...");

  // 1. Wait for d3d11.dll to be loaded
  HMODULE hD3D11 = nullptr;
  for (int i = 0; i < 50; i++) {
    hD3D11 = GetModuleHandle("d3d11.dll");
    if (hD3D11)
      break;
    Sleep(100);
  }

  if (!hD3D11) {
    LogToFile("[D3D11Hook] WARNING: d3d11.dll not found. Compelling load...");
    hD3D11 = LoadLibrary("d3d11.dll");
  }

  if (!hD3D11) {
    LogToFile("[D3D11Hook] ERROR: Could not load d3d11.dll!");
    return;
  }

  // 2. Resolve Target Address
  void *targetAddr =
      (void *)GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain");
  if (!targetAddr) {
    LogToFile(
        "[D3D11Hook] ERROR: Could not find D3D11CreateDeviceAndSwapChain!");
    return;
  }

  LogToFile("[D3D11Hook] Target Address Found. Applying Detour...");

  // 3. Create Inline Hook (Detour)
  if (CreateHook(targetAddr, (void *)Hook_D3D11CreateDeviceAndSwapChain,
                     (void **)&Original_D3D11CreateDeviceAndSwapChain)) {
    LogToFile(
        "[D3D11Hook] SUCCESS: System D3D11CreateDeviceAndSwapChain Detoured!");
  } else {
    LogToFile(
        "[D3D11Hook] ERROR: Failed to Detour D3D11CreateDeviceAndSwapChain.");
  }
}

void D3D11Hook::HookSwapChain(IDXGISwapChain *pSwapChain) {
  // Legacy support
}

void D3D11Hook::Cleanup() {
  if (g_pBackBufferRTV) {
    g_pBackBufferRTV->Release();
    g_pBackBufferRTV = nullptr;
  }
  g_pBackBuffer = nullptr;
}

// =========================================================================
// DXGI Factory Hooking (Robust SwapChain Capture)
// =========================================================================

typedef HRESULT(WINAPI *CreateDXGIFactory_t)(REFIID riid, void **ppFactory);
typedef HRESULT(WINAPI *CreateDXGIFactory1_t)(REFIID riid, void **ppFactory);

CreateDXGIFactory_t Original_CreateDXGIFactory = nullptr;
CreateDXGIFactory1_t Original_CreateDXGIFactory1 = nullptr;

HRESULT WINAPI Hook_CreateDXGIFactory(REFIID riid, void **ppFactory) {
  LogToFile("[D3D11Hook] Intercepted CreateDXGIFactory!");
  HRESULT hr = Original_CreateDXGIFactory(riid, ppFactory);

  if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
    LogToFile("[D3D11Hook] Wrapping DXGI Factory...");
    *ppFactory = new WrappedIDXGIFactory((IDXGIFactory1 *)*ppFactory);
  }
  return hr;
}

HRESULT WINAPI Hook_CreateDXGIFactory1(REFIID riid, void **ppFactory) {
  LogToFile("[D3D11Hook] Intercepted CreateDXGIFactory1!");
  HRESULT hr = Original_CreateDXGIFactory1(riid, ppFactory);

  if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
    LogToFile("[D3D11Hook] Wrapping DXGI Factory1...");
    *ppFactory = new WrappedIDXGIFactory((IDXGIFactory1 *)*ppFactory);
  }
  return hr;
}

void D3D11Hook::InitDXGI() {
  LogToFile("[D3D11Hook] Initializing DXGI Factory Hooks (IAT)...");

  HMODULE hMod = GetModuleHandle(NULL); // Hook Main Module Imports
  if (!hMod)
    return;

  // We need the REAL dxgi.dll loaded
  HMODULE hDXGI = LoadLibrary("dxgi.dll");
  if (!hDXGI) {
    LogToFile("[D3D11Hook] ERROR: Could not load system dxgi.dll");
    return;
  }

  Original_CreateDXGIFactory =
      (CreateDXGIFactory_t)GetProcAddress(hDXGI, "CreateDXGIFactory");
  Original_CreateDXGIFactory1 =
      (CreateDXGIFactory1_t)GetProcAddress(hDXGI, "CreateDXGIFactory1");

  if (Original_CreateDXGIFactory) {
    if (IATHook(hMod, "dxgi.dll", "CreateDXGIFactory",
                (void *)Hook_CreateDXGIFactory, NULL)) {
      LogToFile("[D3D11Hook] SUCCESS: IAT Hooked CreateDXGIFactory");
    }
  }

  if (Original_CreateDXGIFactory1) {
    if (IATHook(hMod, "dxgi.dll", "CreateDXGIFactory1",
                (void *)Hook_CreateDXGIFactory1, NULL)) {
      LogToFile("[D3D11Hook] SUCCESS: IAT Hooked CreateDXGIFactory1");
    }
  }
}

// Phase 5: GateState unified visibility gate for all pipelines
// RULES:
//   Gate is applied as final render alpha ONLY.
//   Gate NEVER deletes entries, clears caches, resets timestamps,
//   or stops snapshot/cache updates.
struct GateState {
  bool videoPlaying;
  bool pauseMenu;
  bool frontMenu;      // Multi-frame debounced (500ms minimum)
  bool suppressUntil;  // Post-video suppress window
  bool transitionBlack;
  bool warmupActive;
  unsigned int warmupFrames;
  bool hideHudFast;
  bool creditsActive;
  bool uiSecuringActive;
  HudStateSignalSnapshot signals;
};

static GateState ComputeGateState(DWORD now, DWORD suppressUntil) {
  GateState g{};
  BinkHook::PumpDeferredWork();
  g.videoPlaying = BinkHook::IsCutsceneVideoPlaying();
  g.pauseMenu = TextHook_IsPauseMenuLikely();
  g.suppressUntil = (now < suppressUntil);
  g.signals = TextHook_GetHudStateSignalSnapshot();
  // Do not trust the engine hideHudFast dvar here: it is noisy in menus and
  // some gameplay states. Keep a short synthetic tail only after confirmed
  // cutscene video closes.
  g.hideHudFast = (now < g_PostVideoHideHudFastUntil);
  g.creditsActive = g.signals.creditsActive;
  g.uiSecuringActive = g.signals.uiSecuringActive;

  static bool s_inTransitionBlack = false;
  static unsigned int s_warmupFramesLeft = 0;
  const bool hudRecent = TextHook_HasRecentHudActivity(600);
  const bool objRecent = TextHook_HasRecentObjectiveLaneActivity(1200);
  // Keep transition-black only when HUD/objective lanes are actually idle.
  const bool transitionByVideo =
      (g.videoPlaying || g.suppressUntil) && !hudRecent && !objRecent;
  const bool transitionByMapEdge =
      (g.signals.uiPrevMapChanged && !hudRecent && !objRecent);
  const bool frontendLikely =
      (!g.pauseMenu && TextHook_IsFrontendMenuLikely());
  const bool transitionByHideFast =
      (g.hideHudFast && !hudRecent && !objRecent && !g.uiSecuringActive &&
       !frontendLikely);
  const bool transitionSignal =
      transitionByVideo || transitionByMapEdge || transitionByHideFast;
  if (transitionSignal) {
    g.transitionBlack = true;
    g.warmupActive = false;
    g.warmupFrames = 0;
    s_inTransitionBlack = true;
    s_warmupFramesLeft = 0;
  } else {
    g.transitionBlack = false;
    if (s_inTransitionBlack) {
      s_inTransitionBlack = false;
      s_warmupFramesLeft = 8;
    }
    g.warmupActive = (s_warmupFramesLeft > 0);
    g.warmupFrames = s_warmupFramesLeft;
    if (s_warmupFramesLeft > 0) {
      --s_warmupFramesLeft;
    }
  }
  static bool s_lastTransitionBlack = false;
  if (g.transitionBlack != s_lastTransitionBlack) {
    s_lastTransitionBlack = g.transitionBlack;
    char gbuf[320];
    sprintf_s(gbuf,
              "[HUD-GATE] transition=%d video=%d suppress=%d mapEdge=%d hideFastEdge=%d hudRecent=%d objRecent=%d hideFast=%d uiSecuring=%d",
              g.transitionBlack ? 1 : 0, g.videoPlaying ? 1 : 0,
              g.suppressUntil ? 1 : 0, transitionByMapEdge ? 1 : 0,
              transitionByHideFast ? 1 : 0, hudRecent ? 1 : 0,
              objRecent ? 1 : 0, g.hideHudFast ? 1 : 0,
              g.uiSecuringActive ? 1 : 0);
    LogToFile(gbuf);
  }

  // Front menu: trust dedicated frontend keyword context (root menu text set),
  // independent of noisy raw menu flags / objective-lane recency.
  // Keep a short debounce to avoid one-frame flips.
  static DWORD s_fmSince = 0;
  const bool fmCandidate =
      (!g.pauseMenu && !g.transitionBlack && TextHook_IsFrontendMenuLikely());
  if (fmCandidate) {
    if (s_fmSince == 0)
      s_fmSince = now;
    g.frontMenu = ((now - s_fmSince) > 220);
  } else {
    s_fmSince = 0;
    g.frontMenu = false;
  }
  return g;
}

// Per-pipeline visibility (0.0 = hidden, 1.0 = visible)
// transitionBlack is the single gate authority for video/loading suppression.
static float ObjectiveVisibility(const GateState &g) {
  if (g.pauseMenu || g.frontMenu || g.transitionBlack || g.warmupActive ||
      g.creditsActive || !g.signals.hudShowObjectives)
    return 0.0f;
  return 1.0f;
}

static float HintVisibility(const GateState &g) {
  if (g.pauseMenu || g.frontMenu || g.transitionBlack || g.warmupActive ||
      g.creditsActive)
    return 0.0f;
  return 1.0f;
}

static float ObjHudHintVisibility(const GateState &g) {
  if (g.pauseMenu || g.frontMenu || g.transitionBlack || g.warmupActive ||
      g.creditsActive)
    return 0.0f;
  return 1.0f;
}

// =========================================================================

