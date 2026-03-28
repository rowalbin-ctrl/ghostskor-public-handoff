#pragma once
#include <d3d11.h>
#include <dxgi.h>

class D3D11Hook {
public:
  static void Init();
  static void InitDXGI();
  static void HookSwapChain(IDXGISwapChain *pSwapChain);
  static void Cleanup();
};

// Helper function to process subtitles (called by DXGIWrapper or Hook_Present)
void D3D11Hook_ProcessSubtitles(IDXGISwapChain *pSwapChain);

// Signal that a mission restart was detected (e.g., FSHook fires for a key
// that already completed its display lifecycle). Clears all render caches
// in the next frame so objectives re-render fresh.
void D3D11Hook_SignalMissionRestart();
