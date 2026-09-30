#include "TextPerf.h"
#include "GameBuild.h"
#include "DXGIWrapper.h"
#include "D3D11Hook.h"
#include "GameViewport.h"
#include "KoreanRenderer.h"
#include "StateSaver.h"
#include "TextHook.h"
#include "Utils.h"
#include <atomic>
#include <cstdio>
#include <new>

// Forward declare helper from D3D11Hook.cpp

// True once a wrapped swap chain is live — tells Hook_Present to stand down.
std::atomic<bool> g_bDXGIWrapperActive{false};

// Global active area state (16:9 area within backbuffer).
GameActiveArea g_ActiveArea = {0.0f, 0.0f, 0.0f, 0.0f};

namespace {
std::atomic<ULONG> g_LiveWrappedSwapChains{0};
const GUID kFactoryWrapperTag = {0x424d0bfa, 0x1565, 0x4e0e, {0x97,0x1c,0xc2,0x03,0xaf,0xba,0xed,0x11}};
const GUID kSwapChainWrapperTag = {0x5b66c6fa, 0x579c, 0x4943, {0xb3,0x6c,0xcf,0xab,0xaf,0xfa,0xb9,0x01}};

IDXGISwapChain *WrapSwapChain(IDXGISwapChain *real) {
  if (!real) return nullptr;
  IDXGISwapChain *existing = nullptr;
  if (SUCCEEDED(real->QueryInterface(kSwapChainWrapperTag, (void**)&existing)))
    return existing;
  auto *wrapped = new (std::nothrow) WrappedIDXGISwapChain(real);
  if (wrapped) return wrapped;
  real->AddRef();
  return real;
}

bool IsSwapChainWrapperInterface(REFIID riid) {
  return riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
         riid == __uuidof(IDXGIDeviceSubObject) ||
         riid == __uuidof(IDXGISwapChain);
}

bool IsFactoryWrapperInterface(REFIID riid) {
  return riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
         riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1);
}

void MarkWrappedSwapChainCreated() {
  if (g_LiveWrappedSwapChains.fetch_add(1, std::memory_order_acq_rel) == 0) {
    g_bDXGIWrapperActive.store(true, std::memory_order_release);
  }
}

void MarkWrappedSwapChainDestroyed() {
  const ULONG prev =
      g_LiveWrappedSwapChains.fetch_sub(1, std::memory_order_acq_rel);
  if (prev <= 1) {
    g_bDXGIWrapperActive.store(false, std::memory_order_release);
  }
}
} // namespace

// =============================================================
// WrappedIDXGISwapChain
// =============================================================

WrappedIDXGISwapChain::WrappedIDXGISwapChain(IDXGISwapChain *pReal)
    : m_pReal(pReal), m_pDevice(nullptr), m_pContext(nullptr),
      m_pBackBufferRTV(nullptr), m_refCount(1), m_cachedWidth(0),
      m_cachedHeight(0) {
  if (m_pReal)
    m_pReal->AddRef();
  MarkWrappedSwapChainCreated();
  LogToFile("[DXGIWrapper] SwapChain Created.");
}

WrappedIDXGISwapChain::~WrappedIDXGISwapChain() {
  MarkWrappedSwapChainDestroyed();
  ReleaseBackBufferResources();
  if (m_pDevice)
    m_pDevice->Release();
  if (m_pContext)
    m_pContext->Release();
  if (m_pReal)
    m_pReal->Release();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::QueryInterface(REFIID riid,
                                                                void **ppvObj) {
  if (!ppvObj) {
    return E_POINTER;
  }
  *ppvObj = nullptr;

  if (riid == kSwapChainWrapperTag || IsSwapChainWrapperInterface(riid)) {
    *ppvObj = static_cast<IDXGISwapChain *>(this);
    AddRef();
    return S_OK;
  }

  return m_pReal->QueryInterface(riid, ppvObj);
}
ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain::AddRef() {
  return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}
ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain::Release() {
  const ULONG refCount =
      m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (refCount == 0) {
    delete this;
  }
  return refCount;
}

void WrappedIDXGISwapChain::ReleaseBackBufferResources() {
  if (m_pBackBufferRTV) {
    m_pBackBufferRTV->Release();
    m_pBackBufferRTV = nullptr;
  }
}

bool WrappedIDXGISwapChain::RefreshBackBufferSize() {
  if (!m_pReal) {
    return false;
  }

  DXGI_SWAP_CHAIN_DESC scDesc = {};
  if (FAILED(m_pReal->GetDesc(&scDesc))) {
    return false;
  }

  if (scDesc.BufferDesc.Width != m_cachedWidth ||
      scDesc.BufferDesc.Height != m_cachedHeight) {
    ReleaseBackBufferResources();
    m_cachedWidth = scDesc.BufferDesc.Width;
    m_cachedHeight = scDesc.BufferDesc.Height;
  }

  return m_cachedWidth != 0 && m_cachedHeight != 0;
}

bool WrappedIDXGISwapChain::EnsureBackBufferRTV() {
  if (m_pBackBufferRTV) {
    return true;
  }
  if (!m_pReal || !m_pDevice) {
    return false;
  }

  ID3D11Texture2D *pBackBuffer = nullptr;
  const HRESULT hr =
      m_pReal->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&pBackBuffer);
  if (FAILED(hr) || !pBackBuffer) {
    return false;
  }

  const HRESULT rtvHr =
      m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &m_pBackBufferRTV);
  pBackBuffer->Release();
  return SUCCEEDED(rtvHr) && m_pBackBufferRTV != nullptr;
}

// Ensure Render Init happens once lazily
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::Present(UINT SyncInterval,
                                                         UINT Flags) {
  TextPerf::Frame();
  if (!GameBuild::RuntimeReady()) return m_pReal->Present(SyncInterval, Flags);
  if (!m_pDevice) {
    if (SUCCEEDED(
            m_pReal->GetDevice(__uuidof(ID3D11Device), (void **)&m_pDevice))) {
      m_pDevice->GetImmediateContext(&m_pContext);

      // Initialize Renderer
      KoreanRenderer::Init(m_pDevice, m_pContext);
      LogToFile(
          "[DXGIWrapper] KoreanRenderer Initialized via SwapChain Present.");

      // Install Text Hook (Safe from DllMain)
      TextHook::Init();
    }
  }

  // Process Subtitles (In-Game & Video) - Handled in D3D11Hook.cpp
  if (m_pReal) {
    if (RefreshBackBufferSize()) {
      g_ActiveArea = Compute16x9ActiveArea((float)m_cachedWidth, (float)m_cachedHeight);
      TextHook_UpdateScreenSize((int)m_cachedWidth, (int)m_cachedHeight);
    }
    D3D11Hook_ProcessSubtitles(m_pReal);
  }

  // Save D3D11 state, bind backbuffer RTV with NO depth stencil, set viewport.
  if (m_pContext) {
    D3D11StateSaver saver(m_pContext);
    saver.Save();

    if (EnsureBackBufferRTV()) {
      m_pContext->OMSetRenderTargets(1, &m_pBackBufferRTV, NULL);

      if (m_cachedWidth != 0 && m_cachedHeight != 0) {
        D3D11_VIEWPORT vp = {};
        vp.Width = (float)m_cachedWidth;
        vp.Height = (float)m_cachedHeight;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        m_pContext->RSSetViewports(1, &vp);
      }
    }

    KoreanRenderer::Render(); // Custom Draw

    saver.Restore();
  } else {
    KoreanRenderer::Render(); // Fallback: no state management
  }

  extern std::atomic<uint64_t> g_presentFrameCount;
  const uint64_t frameCount = g_presentFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;
  if (frameCount % 600 == 0)
    LogToFile("[DXGIWrapper] Present Hook Alive");



  return m_pReal->Present(SyncInterval, Flags);
}

// Forwarders
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::SetPrivateData(
    REFGUID Name, UINT DataSize, const void *pData) {
  return m_pReal->SetPrivateData(Name, DataSize, pData);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::SetPrivateDataInterface(
    REFGUID Name, const IUnknown *pUnknown) {
  return m_pReal->SetPrivateDataInterface(Name, pUnknown);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetPrivateData(REFGUID Name,
                                                                UINT *pDataSize,
                                                                void *pData) {
  return m_pReal->GetPrivateData(Name, pDataSize, pData);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetParent(REFIID riid,
                                                           void **ppParent) {
  if (!ppParent) {
    return E_POINTER;
  }
  *ppParent = nullptr;

  if (IsFactoryWrapperInterface(riid)) {
    IDXGIFactory *pFactory = nullptr;
    const HRESULT hr =
        m_pReal->GetParent(__uuidof(IDXGIFactory), (void **)&pFactory);
    if (FAILED(hr) || !pFactory) {
      return hr;
    }

    auto *wrapper = (IDXGIFactory1*)WrapFactory(pFactory);
    pFactory->Release();

    const HRESULT qiHr = wrapper->QueryInterface(riid, ppParent);
    wrapper->Release();
    return qiHr;
  }

  return m_pReal->GetParent(riid, ppParent);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetDevice(REFIID riid,
                                                           void **ppDevice) {
  return m_pReal->GetDevice(riid, ppDevice);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetBuffer(UINT Buffer,
                                                           REFIID riid,
                                                           void **ppSurface) {
  return m_pReal->GetBuffer(Buffer, riid, ppSurface);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::SetFullscreenState(
    BOOL Fullscreen, IDXGIOutput *pTarget) {
  const HRESULT hr = m_pReal->SetFullscreenState(Fullscreen, pTarget);
  if (SUCCEEDED(hr)) {
    ReleaseBackBufferResources();
    m_cachedWidth = 0;
    m_cachedHeight = 0;
    RefreshBackBufferSize();
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetFullscreenState(
    BOOL *pFullscreen, IDXGIOutput **ppTarget) {
  return m_pReal->GetFullscreenState(pFullscreen, ppTarget);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGISwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) {
  return m_pReal->GetDesc(pDesc);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::ResizeBuffers(
    UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
    UINT SwapChainFlags) {
  ReleaseBackBufferResources();
  m_cachedWidth = 0;
  m_cachedHeight = 0;

  const HRESULT hr = m_pReal->ResizeBuffers(BufferCount, Width, Height,
                                            NewFormat, SwapChainFlags);
  if (SUCCEEDED(hr)) {
    // Immediately update g_ActiveArea and screen size — not just cached
    // dimensions. Without this, a ResizeBuffers→Present gap leaves stale
    // values that poison GetMenuFontScaleMul coordinate thresholds.
    if (RefreshBackBufferSize()) {
      g_ActiveArea = Compute16x9ActiveArea((float)m_cachedWidth, (float)m_cachedHeight);
      TextHook_UpdateScreenSize((int)m_cachedWidth, (int)m_cachedHeight);
    }
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::ResizeTarget(
    const DXGI_MODE_DESC *pNewTargetParameters) {
  const HRESULT hr = m_pReal->ResizeTarget(pNewTargetParameters);
  if (SUCCEEDED(hr)) {
    if (RefreshBackBufferSize()) {
      g_ActiveArea = Compute16x9ActiveArea((float)m_cachedWidth, (float)m_cachedHeight);
      TextHook_UpdateScreenSize((int)m_cachedWidth, (int)m_cachedHeight);
    }
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGISwapChain::GetContainingOutput(IDXGIOutput **ppOutput) {
  return m_pReal->GetContainingOutput(ppOutput);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGISwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats) {
  return m_pReal->GetFrameStatistics(pStats);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGISwapChain::GetLastPresentCount(UINT *pLastPresentCount) {
  return m_pReal->GetLastPresentCount(pLastPresentCount);
}

// =============================================================
// WrappedIDXGIFactory
// =============================================================

WrappedIDXGIFactory::WrappedIDXGIFactory(IDXGIFactory *pReal)
    : m_pReal(pReal), m_pReal1(nullptr), m_refCount(1) {
  if (m_pReal) {
    m_pReal->AddRef();
    m_pReal->QueryInterface(__uuidof(IDXGIFactory1), (void**)&m_pReal1);
  }
}
WrappedIDXGIFactory::~WrappedIDXGIFactory() {
  if (m_pReal1) m_pReal1->Release();
  if (m_pReal)
    m_pReal->Release();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::QueryInterface(REFIID riid,
                                                              void **ppvObj) {
  if (!ppvObj) {
    return E_POINTER;
  }
  *ppvObj = nullptr;

  if (riid == __uuidof(IDXGIFactory1) && !m_pReal1) return E_NOINTERFACE;
  if (riid == kFactoryWrapperTag || IsFactoryWrapperInterface(riid)) {
    *ppvObj = static_cast<IDXGIFactory1 *>(this);
    AddRef();
    return S_OK;
  }

  return m_pReal->QueryInterface(riid, ppvObj);
}
ULONG STDMETHODCALLTYPE WrappedIDXGIFactory::AddRef() {
  return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}
ULONG STDMETHODCALLTYPE WrappedIDXGIFactory::Release() {
  const ULONG refCount =
      m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (refCount == 0) {
    delete this;
  }
  return refCount;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSwapChain(
    IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc,
    IDXGISwapChain **ppSwapChain) {
  if (!ppSwapChain) return E_POINTER;
  *ppSwapChain = nullptr;
  IDXGISwapChain *pRealChain = nullptr;
  HRESULT hr = m_pReal->CreateSwapChain(pDevice, pDesc, &pRealChain);

  if (SUCCEEDED(hr) && pRealChain) {
    // WRAP IT
    *ppSwapChain = WrapSwapChain(pRealChain);
    // Release real one since wrapper holds ref
    pRealChain->Release();
  } else {
    *ppSwapChain = nullptr;
  }
  return hr;
}

// Forwarders
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::SetPrivateData(
    REFGUID Name, UINT DataSize, const void *pData) {
  return m_pReal->SetPrivateData(Name, DataSize, pData);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::SetPrivateDataInterface(
    REFGUID Name, const IUnknown *pUnknown) {
  return m_pReal->SetPrivateDataInterface(Name, pUnknown);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::GetPrivateData(REFGUID Name,
                                                              UINT *pDataSize,
                                                              void *pData) {
  return m_pReal->GetPrivateData(Name, pDataSize, pData);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::GetParent(REFIID riid,
                                                         void **ppParent) {
  return m_pReal->GetParent(riid, ppParent);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::EnumAdapters(UINT Adapter, IDXGIAdapter **ppAdapter) {
  return m_pReal->EnumAdapters(Adapter, ppAdapter);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::MakeWindowAssociation(HWND WindowHandle, UINT Flags) {
  return m_pReal->MakeWindowAssociation(WindowHandle, Flags);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::GetWindowAssociation(HWND *pWindowHandle) {
  return m_pReal->GetWindowAssociation(pWindowHandle);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSoftwareAdapter(
    HMODULE Module, IDXGIAdapter **ppAdapter) {
  return m_pReal->CreateSoftwareAdapter(Module, ppAdapter);
}

HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::EnumAdapters1(UINT Adapter, IDXGIAdapter1 **ppAdapter) {
  if (!m_pReal1) { if (ppAdapter) *ppAdapter = nullptr; return E_NOINTERFACE; }
  return m_pReal1->EnumAdapters1(Adapter, ppAdapter);
}
BOOL STDMETHODCALLTYPE WrappedIDXGIFactory::IsCurrent() {
  return m_pReal1 ? m_pReal1->IsCurrent() : FALSE;
}

extern "C" void *WrapFactory(void *pReal) {
  if (!pReal)
    return nullptr;
  auto *real = (IDXGIFactory*)pReal;
  IDXGIFactory1 *existing = nullptr;
  if (SUCCEEDED(real->QueryInterface(kFactoryWrapperTag, (void**)&existing)))
    return existing;
  auto *wrapped = new (std::nothrow) WrappedIDXGIFactory(real);
  if (wrapped) return wrapped;
  real->AddRef();
  return real;
}
