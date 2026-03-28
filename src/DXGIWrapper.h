#pragma once
#include <atomic>
#include <d3d11.h>
#include <dxgi.h>

class WrappedIDXGISwapChain : public IDXGISwapChain {
public:
  WrappedIDXGISwapChain(IDXGISwapChain *pReal);
  virtual ~WrappedIDXGISwapChain();

  // IUnknown
  virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj);
  virtual ULONG STDMETHODCALLTYPE AddRef();
  virtual ULONG STDMETHODCALLTYPE Release();

  // IDXGIObject
  virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize,
                                                   const void *pData);
  virtual HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID Name, const IUnknown *pUnknown);
  virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name,
                                                   UINT *pDataSize,
                                                   void *pData);
  virtual HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent);

  // IDXGIDeviceSubObject
  virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppDevice);

  // IDXGISwapChain
  virtual HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags);
  virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid,
                                              void **ppSurface);
  virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen,
                                                       IDXGIOutput *pTarget);
  virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *pFullscreen,
                                                       IDXGIOutput **ppTarget);
  virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc);
  virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width,
                                                  UINT Height,
                                                  DXGI_FORMAT NewFormat,
                                                  UINT SwapChainFlags);
  virtual HRESULT STDMETHODCALLTYPE
  ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters);
  virtual HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **ppOutput);
  virtual HRESULT STDMETHODCALLTYPE
  GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats);
  virtual HRESULT STDMETHODCALLTYPE
  GetLastPresentCount(UINT *pLastPresentCount);

private:
  void ReleaseBackBufferResources();
  bool RefreshBackBufferSize();
  bool EnsureBackBufferRTV();

  IDXGISwapChain *m_pReal;
  ID3D11Device *m_pDevice;
  ID3D11DeviceContext *m_pContext;
  ID3D11RenderTargetView *m_pBackBufferRTV;
  std::atomic<ULONG> m_refCount;
  UINT m_cachedWidth;
  UINT m_cachedHeight;
};

class WrappedIDXGIFactory : public IDXGIFactory1 {
public:
  WrappedIDXGIFactory(IDXGIFactory1 *pReal);
  virtual ~WrappedIDXGIFactory();

  // IUnknown
  virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj);
  virtual ULONG STDMETHODCALLTYPE AddRef();
  virtual ULONG STDMETHODCALLTYPE Release();

  // IDXGIObject
  virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize,
                                                   const void *pData);
  virtual HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID Name, const IUnknown *pUnknown);
  virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name,
                                                   UINT *pDataSize,
                                                   void *pData);
  virtual HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent);

  // IDXGIFactory
  virtual HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter,
                                                 IDXGIAdapter **ppAdapter);
  virtual HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle,
                                                          UINT Flags);
  virtual HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pWindowHandle);
  virtual HRESULT STDMETHODCALLTYPE
  CreateSwapChain(IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc,
                  IDXGISwapChain **ppSwapChain);
  virtual HRESULT STDMETHODCALLTYPE
  CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter **ppAdapter);

  // IDXGIFactory1
  virtual HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter,
                                                  IDXGIAdapter1 **ppAdapter);
  virtual BOOL STDMETHODCALLTYPE IsCurrent();

private:
  IDXGIFactory1 *m_pReal;
  std::atomic<ULONG> m_refCount;
};
