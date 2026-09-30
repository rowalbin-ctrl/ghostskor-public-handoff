#include "DXGIWrapper.h"
#include "GameBuild.h"
#include "KoreanRenderer.h"
#include "TextHook.h"
#include "StateSaver.h"
#include <dxgi1_3.h>
#include <cassert>
#include <iostream>
static int draws=0,processes=0;
std::atomic<uint64_t> g_presentFrameCount{0};
extern std::atomic<bool> g_bDXGIWrapperActive;
bool GameBuild::IsSupported(){return true;}
bool GameBuild::RuntimeReady(){return true;}
void KoreanRenderer::Init(ID3D11Device*,ID3D11DeviceContext*){}
void KoreanRenderer::Render(){++draws;}
void TextHook::Init(){}
void TextHook_UpdateScreenSize(int,int){}
void D3D11Hook_ProcessSubtitles(IDXGISwapChain*){++processes;}
int main(){
 WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"GhostsKorValidation";
 assert(RegisterClassW(&wc));
 HWND window=CreateWindowExW(0,wc.lpszClassName,L"GhostsKor WARP validation",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,wc.hInstance,nullptr);
 assert(window); // deliberately hidden; independent of the user's game window
 ID3D11Device *device=nullptr;ID3D11DeviceContext *context=nullptr;
 assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
 for(int restart=0;restart<9;++restart){
  const int api=restart%3;
  IDXGIFactory *factory=nullptr;
  HRESULT hr=api==0 ? CreateDXGIFactory(__uuidof(IDXGIFactory),(void**)&factory)
      : api==1 ? CreateDXGIFactory1(__uuidof(IDXGIFactory),(void**)&factory)
      : CreateDXGIFactory2(0,__uuidof(IDXGIFactory),(void**)&factory);
  assert(SUCCEEDED(hr) && factory);
  // Test the actual shipping proxy exports, including the game's DXGI 1.0 path.
  auto *same=(IDXGIFactory*)WrapFactory(factory);
  assert(same==factory);same->Release();
  IDXGIFactory1 *extended=nullptr;
  hr=factory->QueryInterface(__uuidof(IDXGIFactory1),(void**)&extended);
  if(api==0) assert(hr==E_NOINTERFACE && !extended);
  else {assert(SUCCEEDED(hr) && extended);extended->Release();}
  for(int repeat=0;repeat<10;++repeat){
   auto *again=(IDXGIFactory1*)WrapFactory(factory);
   assert(again==factory);assert(again->Release()==1);
  }
  void *identity=nullptr;assert(SUCCEEDED(factory->QueryInterface(__uuidof(IUnknown),&identity)));
  assert(identity==factory);((IUnknown*)identity)->Release();
  assert(factory->CreateSwapChain(nullptr,nullptr,nullptr)==E_POINTER);
  DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=1;desc.BufferDesc.Width=640;desc.BufferDesc.Height=480;
  desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.OutputWindow=window;desc.SampleDesc.Count=1;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain *chain=nullptr;
  assert(SUCCEEDED(factory->CreateSwapChain(device,&desc,&chain)));
  assert(g_bDXGIWrapperActive.load());
  for(int frame=0;frame<3;++frame){
   int before=draws;auto counter=g_presentFrameCount.load();
   assert(SUCCEEDED(chain->Present(0,0)));
   assert(draws==before+1 && processes==draws && g_presentFrameCount.load()==counter+1);
   assert(SUCCEEDED(chain->ResizeBuffers(1,640+frame*16,480+frame*16,DXGI_FORMAT_UNKNOWN,0)));
  }
  IDXGIFactory *parent=nullptr;assert(SUCCEEDED(chain->GetParent(__uuidof(IDXGIFactory),(void**)&parent)));
  auto *parentAgain=(IDXGIFactory1*)WrapFactory(parent);assert(parentAgain==parent);parentAgain->Release();parent->Release();
  chain->Release();assert(!g_bDXGIWrapperActive.load());assert(factory->Release()==0);
 }
 ID3D11Buffer *before=nullptr,*overlay=nullptr,*after=nullptr;
 D3D11_BUFFER_DESC bd{};bd.ByteWidth=64;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
 assert(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&before)));assert(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&overlay)));
 for(int unbound=0;unbound<2;++unbound){
  auto *expected=unbound?nullptr:before;context->VSSetConstantBuffers(0,1,&expected);
  {D3D11StateSaver saver(context);saver.Save();context->VSSetConstantBuffers(0,1,&overlay);saver.Restore();}
  context->VSGetConstantBuffers(0,1,&after);assert(after==expected);if(after)after->Release();
 }
 context->ClearState();before->Release();overlay->Release();context->Release();device->Release();DestroyWindow(window);UnregisterClassW(wc.lpszClassName,wc.hInstance);
 std::cout<<"PASS: 3 shipping DXGI exports, 9 factory/swap-chain recreations, 90 repeated wrap attempts, 27 Presents/resizes; one subtitle/render/frame update per Present; COM references and VS buffer restoration\n";
}
