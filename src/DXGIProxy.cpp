#include "DXGIWrapper.h"
#include "GameBuild.h"
#include <mutex>

namespace {
HMODULE SystemDXGI() {
  static HMODULE module = nullptr;
  static std::once_flag once;
  std::call_once(once, [] {
    wchar_t path[MAX_PATH] = {};
    if (!GetSystemDirectoryW(path, MAX_PATH)) return;
    if (wcscat_s(path, L"\\dxgi.dll") != 0) return;
    module = LoadLibraryW(path);
  });
  return module;
}

void WrapCreatedFactory(REFIID riid, void **result) {
  if (!result || !*result || !GameBuild::IsSupported()) return;
  if (riid != __uuidof(IUnknown) && riid != __uuidof(IDXGIObject) &&
      riid != __uuidof(IDXGIFactory) && riid != __uuidof(IDXGIFactory1)) return;

  // Keep the interface version created by the requested system entry point.
  // CreateDXGIFactory rejects IID_IDXGIFactory1 with E_NOINTERFACE on DXGI 1.0.
  auto *original = static_cast<IUnknown *>(*result);
  IDXGIFactory *base = nullptr;
  if (FAILED(original->QueryInterface(__uuidof(IDXGIFactory), (void**)&base))) return;
  auto *wrapper = static_cast<IDXGIFactory *>(WrapFactory(base));
  base->Release();
  void *requested = nullptr;
  const HRESULT hr = wrapper->QueryInterface(riid, &requested);
  wrapper->Release();
  if (SUCCEEDED(hr)) {
    original->Release();
    *result = requested;
  }
}
}

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **result) {
  if (!result) return E_POINTER;
  *result = nullptr;
  using Fn = HRESULT(WINAPI *)(REFIID, void **);
  const auto fn = (Fn)GetProcAddress(SystemDXGI(), "CreateDXGIFactory");
  if (!fn) return E_FAIL;
  const HRESULT hr = fn(riid, result);
  if (SUCCEEDED(hr)) WrapCreatedFactory(riid, result);
  return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **result) {
  if (!result) return E_POINTER;
  *result = nullptr;
  using Fn = HRESULT(WINAPI *)(REFIID, void **);
  const auto fn = (Fn)GetProcAddress(SystemDXGI(), "CreateDXGIFactory1");
  if (!fn) return E_FAIL;
  const HRESULT hr = fn(riid, result);
  if (SUCCEEDED(hr)) WrapCreatedFactory(riid, result);
  return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **result) {
  if (!result) return E_POINTER;
  *result = nullptr;
  using Fn = HRESULT(WINAPI *)(UINT, REFIID, void **);
  const auto fn = (Fn)GetProcAddress(SystemDXGI(), "CreateDXGIFactory2");
  if (!fn) return E_FAIL;
  const HRESULT hr = fn(flags, riid, result);
  if (SUCCEEDED(hr)) WrapCreatedFactory(riid, result);
  return hr;
}
