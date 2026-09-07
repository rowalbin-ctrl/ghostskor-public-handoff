#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include "GameBuild.h"
#include "GameAddresses.h"
#pragma comment(lib, "bcrypt.lib")

namespace {
constexpr char kExpectedHash[] =
    "e9f62f2b780d2dce8180f4b5fd9f0022e10c9f2596cc8066128c21169b74f110";
std::once_flag g_hashOnce;
bool g_supported = false;
char g_hash[65] = "unavailable";
char g_exeName[512] = "unavailable";
std::atomic<bool> g_ready{false};
std::mutex g_readyMutex;

void CheckFile() {
  wchar_t path[32768] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, _countof(path));
  if (!length || length >= _countof(path)) return;
  const wchar_t* name = wcsrchr(path, L'\\');
  name = name ? name + 1 : path;
  const bool expectedName = _wcsicmp(name, L"iw6sp64_ship.exe") == 0;
  WideCharToMultiByte(CP_UTF8, 0, name, -1, g_exeName, sizeof(g_exeName), nullptr, nullptr);
  // Identify unknown store builds in the report too; this does not allow hooks.
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                        nullptr, 0) >= 0;
  if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
  unsigned char buffer[32768];
  while (ok) {
    DWORD read = 0;
    if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) { ok = false; break; }
    if (!read) break;
    ok = BCryptHashData(hash, buffer, read, 0) >= 0;
  }
  unsigned char digest[32] = {};
  if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
  if (hash) BCryptDestroyHash(hash);
  if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  CloseHandle(file);
  if (!ok) return;
  for (size_t i = 0; i < sizeof(digest); ++i)
    sprintf_s(g_hash + i * 2, sizeof(g_hash) - i * 2, "%02x", digest[i]);
  g_supported = expectedName && strcmp(g_hash, kExpectedHash) == 0;
}

struct Signature { uintptr_t rva; size_t size; const char* bytes; };
const Signature kSignatures[] = {
#include "GameBuildSignatures.inl"
};

bool Matches(uintptr_t base, const Signature& signature) {
  __try {
    const void* p = reinterpret_cast<void*>(base + signature.rva);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protection = mbi.Protect & 0xff;
    if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
      return false;
    return memcmp(p, signature.bytes, signature.size) == 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

bool GameBuild::IsSupported() {
  std::call_once(g_hashOnce, CheckFile);
  return g_supported;
}

bool GameBuild::RuntimeReady() {
  if (g_ready.load(std::memory_order_acquire)) return true;
  if (!IsSupported()) return false;
  std::lock_guard<std::mutex> lock(g_readyMutex);
  if (g_ready.load(std::memory_order_relaxed)) return true;
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  for (const auto& signature : kSignatures)
    if (!Matches(base, signature)) return false;
  if (!GameAddresses::Initialize(base)) return false;
  // Hooks may modify these bytes after publication; do not compare them again.
  g_ready.store(true, std::memory_order_release);
  return true;
}

void GameBuild::WriteStatus(const char* status) {
  IsSupported();
  wchar_t path[32768] = {};
  DWORD length = GetModuleFileNameW(nullptr, path, _countof(path));
  if (!length || length >= _countof(path)) return;
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!slash) return;
  const size_t remaining = _countof(path) - (slash + 1 - path);
  if (wcscpy_s(slash + 1, remaining, L"ghostskor_compatibility.txt") != 0) return;
  HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  char report[1536] = {};
  int n = sprintf_s(report,
      "GhostsKor R5 validated function resolver and bounded lighting candidate\r\n"
      "EXE name: %s\r\nEXE SHA256: %s\r\nSupported: %s\r\nStatus: %s\r\n"
      "Function resolver: %s\r\n"
      "Unrecognized builds use original DXGI without game hooks.\r\n",
      g_exeName, g_hash, g_supported ? "yes (Steam SP build 24723416)" : "no", status,
      GameAddresses::Failure());
  DWORD written = 0;
  if (n > 0) WriteFile(file, report, static_cast<DWORD>(n), &written, nullptr);
  CloseHandle(file);
}
