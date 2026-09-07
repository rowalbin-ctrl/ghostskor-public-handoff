// D3D11Hook forward ref

// #include "InitHooks.h"
#include "BinkHook.h"
#include "D3D11Hook.h"
#include "FSHook.h"
#include "GameBuild.h"
#include "TextHook.h"
#include "TextReplacer.h"
#include "TranslationStore.h"
#include "Utils.h" // Must be first - includes Windows.h and defines GetModuleDirectory
#include <unordered_map>

// =============================================================
// PROXY EXPORT: dxgi.dll
// The game imports dxgi.dll for DirectX. We proxy it.
// =============================================================

// Helper from DXGIWrapper.cpp
extern "C" void *WrapFactory(void *pReal);

// IID_IDXGIFactory1: 770aae78-f26f-4dba-a829-253c83d1b387
const GUID IID_IDXGIFactory1_Local = {
    0x770aae78,
    0xf26f,
    0x4dba,
    {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};

// Valid DXGI Functions
HMODULE g_OriginalDXGI = NULL;
static HMODULE g_hModule = NULL;
static HANDLE g_InitThread = NULL;

static void ReleaseInitThreadHandle() {
  HANDLE threadHandle = (HANDLE)InterlockedExchangePointer(
      reinterpret_cast<PVOID *>(&g_InitThread), nullptr);
  if (threadHandle) {
    CloseHandle(threadHandle);
  }
}

// Load original DXGI
void LoadOriginalDXGI() {
  if (g_OriginalDXGI)
    return;

  wchar_t systemPath[MAX_PATH] = {0};
  if (!GetSystemDirectoryW(systemPath, MAX_PATH)) {
    return;
  }
  wcscat_s(systemPath, L"\\dxgi.dll");

  g_OriginalDXGI = LoadLibraryW(systemPath);
}

// Proxy exports
typedef HRESULT(WINAPI *PFN_CreateDXGIFactory)(REFIID, void **);
typedef HRESULT(WINAPI *PFN_CreateDXGIFactory1)(REFIID, void **);
typedef HRESULT(WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void **);

static bool CanWrapFactoryRiid(REFIID riid) {
  return riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIFactory) ||
         riid == __uuidof(IDXGIFactory1) || riid == IID_IDXGIFactory1_Local;
}

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory) {
  if (!ppFactory)
    return E_POINTER;
  LoadOriginalDXGI();
  auto func = (PFN_CreateDXGIFactory)GetProcAddress(g_OriginalDXGI,
                                                    "CreateDXGIFactory");
  if (!func)
    return E_FAIL;

  if (!GameBuild::IsSupported() || !CanWrapFactoryRiid(riid))
    return func(riid, ppFactory);

  // Wrap it
  void *pTemp = nullptr;
  HRESULT hr = func(IID_IDXGIFactory1_Local, &pTemp);
  if (SUCCEEDED(hr) && pTemp) {
    *ppFactory = WrapFactory(pTemp);
    ((IUnknown *)pTemp)->Release();
    return S_OK;
  }
  return func(riid, ppFactory);
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory) {
  if (!ppFactory)
    return E_POINTER;
  LoadOriginalDXGI();
  auto func = (PFN_CreateDXGIFactory1)GetProcAddress(g_OriginalDXGI,
                                                     "CreateDXGIFactory1");
  if (!func)
    return E_FAIL;

  if (!GameBuild::IsSupported() || !CanWrapFactoryRiid(riid))
    return func(riid, ppFactory);

  void *pTemp = nullptr;
  HRESULT hr = func(riid, &pTemp);
  if (SUCCEEDED(hr) && pTemp) {
    *ppFactory = WrapFactory(pTemp);
    ((IUnknown *)pTemp)->Release();
    return S_OK;
  }
  return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid,
                                             void **ppFactory) {
  LoadOriginalDXGI();
  auto func = (PFN_CreateDXGIFactory2)GetProcAddress(g_OriginalDXGI,
                                                     "CreateDXGIFactory2");
  if (!func)
    return E_FAIL;

  if (!ppFactory)
    return E_POINTER;

  if (GameBuild::IsSupported() && CanWrapFactoryRiid(riid)) {
    void *pTemp = nullptr;
    HRESULT hr = func(Flags, IID_IDXGIFactory1_Local, &pTemp);
    if (SUCCEEDED(hr) && pTemp) {
      *ppFactory = WrapFactory(pTemp);
      ((IUnknown *)pTemp)->Release();
      return S_OK;
    }
  }

  return func(Flags, riid, ppFactory);
}

DWORD WINAPI MainThread(LPVOID lpParam) {
  auto finish = [](DWORD exitCode) -> DWORD {
    ReleaseInitThreadHandle();
    return exitCode;
  };

  // Wait a tiny bit (Safety)
  Sleep(50);

  if (!GameBuild::IsSupported()) {
    GameBuild::WriteStatus("Unsupported EXE; Korean patch hooks skipped.");
    return finish(0);
  }
  // Steam unpacks code after DLL loading. File identity alone is insufficient.
  for (int i = 0; i < 600 && !GameBuild::RuntimeReady(); ++i) Sleep(50);
  if (!GameBuild::RuntimeReady()) {
    GameBuild::WriteStatus("Expected runtime code not ready; game hooks skipped.");
    return finish(0);
  }
  GameBuild::WriteStatus("Runtime signatures verified; initializing Korean patch.");

  // CreateConsole();
  LogToFile("DLL Loaded. MainThread Started (Async). [FSHook+VideoSub v2]");

  // Load runtime feature flags before installing hooks.
  RuntimeFlags_EnsureLoaded(g_hModule);

  // Load Translations (dynamic path)
  std::string baseDir = GetModuleDirPath(g_hModule);
  std::string jsonPath = baseDir + "\\localize_kr.json";
  if (!TranslationStore::EnsureLoadedFromPath(jsonPath)) {
    LogToFile("[CRITICAL] Failed to load translations: " + jsonPath);
    MessageBoxUtf8(NULL,
                   "localize_kr.json 파일을 찾을 수 없습니다.\n패치를 재설치하세요.",
                   "초기화 실패", MB_ICONERROR);
    return finish(1);
  }

  // Initialize Hooks
  // Initialize Hooks (Inlined from InitHooks.cpp to fix build)
  LogToFile("Initializing Hooks [RELEASE BUILD]...");
  TextHook::Init();
  D3D11Hook::InitDXGI();
  D3D11Hook::Init();

  // FSHook - intercept subtitles.csv and blank the text fields
  LogToFile("[Main] Initializing FSHook (video subtitles)...");

  // Build English->Korean map from localize.json + localize_kr.json
  // Files are in game root directory (where exe is), not DLL directory
  // 1. localize.json: KEY -> English
  // 2. localize_kr.json: KEY -> Korean
  // Result: English -> Korean
  try {
    const auto &keyToEnglish = TranslationStore::KeyToEnglish();
    const auto &keyToKorean = TranslationStore::KeyToKorean();
    std::unordered_map<std::string, std::string> eng2kor;

    for (const auto &pair : keyToEnglish) {
      const std::string &key = pair.first;
      bool isRelevant =
          (key.find("VIDSUBTITLES_") == 0) || (key.find("GAME_") == 0) ||
          (key.find("PLATFORM_") == 0) || (key.find("CGAME_") == 0) ||
          (key.find("OBJECTIVE_") == 0) || (key.find("HINT_") == 0) ||
          (key.find("MENU_") == 0) || (key.find("MP_") == 0) ||
          (key.find("KILLSTREAK_") == 0);
      if (!isRelevant) {
        continue;
      }

      const std::string &english = pair.second;
      auto krIt = keyToKorean.find(key);
      if (krIt == keyToKorean.end()) {
        continue;
      }

      std::string normalized;
      for (size_t i = 0; i < english.size(); ++i) {
        if (english[i] == '^' && i + 1 < english.size()) {
          char next = english[i + 1];
          if ((next >= '0' && next <= '9') ||
              (next >= 'a' && next <= 'z') ||
              (next >= 'A' && next <= 'Z')) {
            ++i;
            continue;
          }
        }
        normalized += (char)toupper((unsigned char)english[i]);
      }

      eng2kor[normalized] = krIt->second;
      if (normalized.find("DIFFERENT TIME") != std::string::npos) {
        LogToFile("[Main] DEBUG MAP: [" + normalized + "]");
      }
    }

    LogToFile("[Main] Built " + std::to_string(eng2kor.size()) +
              " English->Korean mappings");
    FSHook::SetEnglishToKoreanMap(eng2kor);
  } catch (const std::exception &e) {
    LogToFile("[Main] ERROR loading video subtitle translations: " +
              std::string(e.what()));
  }

  if (FSHook::Initialize()) {
    LogToFile(
        "[Main] FSHook ready - video subtitles will be replaced with Korean");
  } else {
    LogToFile("[Main] WARNING: FSHook failed");
  }

  // Initialize Bink Video Hook for video subtitle timing (IAT Hook method)
  if (BinkHook::Init()) {
    LogToFile("[Main] BinkHook initialized for video subtitle display");
  } else {
    LogToFile(
        "[Main] WARNING: BinkHook init failed - video subtitles disabled.");
  }

  LogToFile("[Main] All hooks initialized successfully.");
  GameBuild::WriteStatus("Initialization dispatched; gameplay validation still required.");
  return finish(0);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  switch (ul_reason_for_call) {
  case DLL_PROCESS_ATTACH:
    // Avoid loader-lock reentry on per-thread notifications.
    g_hModule = hModule;
    DisableThreadLibraryCalls(hModule);
    g_InitThread = CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
    break;
  case DLL_PROCESS_DETACH:
    // Do not wait here: DllMain runs under the loader lock.
    ReleaseInitThreadHandle();
    break;
  }
  return TRUE;
}
