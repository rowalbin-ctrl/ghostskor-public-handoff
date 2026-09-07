#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include "AddressPatternMatcher.h"
#include "GameAddresses.h"

namespace {
using AddressPatternMatcher::Pattern;
constexpr Pattern kPatterns[] = {
#include "GameAddressPatterns.inl"
};
constexpr size_t kCount = sizeof(kPatterns) / sizeof(kPatterns[0]);
uintptr_t g_addresses[kCount] = {};
uintptr_t g_moduleBase = 0;
std::atomic<bool> g_published{false};
bool g_attempted = false;
char g_failure[192] = "Not initialized";

bool Readable(uintptr_t start, size_t size) {
  const uintptr_t end = start + size;
  if (end < start) return false;
  while (start < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(start), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
      return false;
    const DWORD access = info.Protect & 0xff;
    if (access != PAGE_READONLY && access != PAGE_READWRITE &&
        access != PAGE_WRITECOPY && access != PAGE_EXECUTE_READ &&
        access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY)
      return false;
    const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (next <= start) return false;
    start = next;
  }
  return true;
}
struct Section { uintptr_t rva; size_t size; };
bool Scan(uintptr_t base) {
  __try {
    if (!Readable(base, sizeof(IMAGE_DOS_HEADER))) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x100000)
      return false;
    const uintptr_t ntAddress = base + dos->e_lfanew;
    if (!Readable(ntAddress, sizeof(IMAGE_NT_HEADERS64))) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(ntAddress);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.NumberOfSections == 0 || nt->FileHeader.NumberOfSections > 96)
      return false;
    const auto* headers = IMAGE_FIRST_SECTION(nt);
    if (!Readable(reinterpret_cast<uintptr_t>(headers),
                  nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER))) return false;
    Section sections[96] = {};
    size_t sectionCount = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
      const auto& s = headers[i];
      if (!(s.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
      const size_t length = s.Misc.VirtualSize;
      if (!length) continue;
      if (s.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
          length > nt->OptionalHeader.SizeOfImage - s.VirtualAddress ||
          !Readable(base + s.VirtualAddress, length)) return false;
      sections[sectionCount++] = {s.VirtualAddress, length};
    }
    if (!sectionCount) return false;
    g_attempted = true; // Do not rescan a failed signature set every frame.
    uintptr_t pending[kCount] = {};
    for (size_t i = 0; i < kCount; ++i) {
      AddressPatternMatcher::Match match;
      for (size_t j = 0; j < sectionCount && match.count < 2; ++j)
        AddressPatternMatcher::Search(
            reinterpret_cast<const unsigned char*>(base + sections[j].rva),
            sections[j].size, sections[j].rva, kPatterns[i], match);
      // A shifted match is diagnostic only until caller/data profiles are
      // verified too. Never mix discovered functions with another build's data.
      if (match.count != 1 || match.rva != kPatterns[i].referenceRva) {
        sprintf_s(g_failure, "%s: matches=%u, RVA=0x%llX, expected profile=0x%llX",
                  kPatterns[i].name, match.count, static_cast<unsigned long long>(match.rva),
                  static_cast<unsigned long long>(kPatterns[i].referenceRva));
        return false;
      }
      pending[i] = base + match.rva;
    }
    for (size_t i = 0; i < kCount; ++i) g_addresses[i] = pending[i];
    g_moduleBase = base;
    strcpy_s(g_failure, "None; all function patterns uniquely resolved and profile verified");
    g_published.store(true, std::memory_order_release);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_attempted = true;
    strcpy_s(g_failure, "Memory changed or became unreadable while resolving functions");
    return false;
  }
}
}
bool GameAddresses::Initialize(uintptr_t moduleBase) {
  if (g_published.load(std::memory_order_acquire)) return moduleBase == g_moduleBase;
  if (g_attempted || !moduleBase) return false;
  return Scan(moduleBase);
}
void* GameAddresses::Resolve(uintptr_t moduleBase, uintptr_t referenceRva) {
  if (!g_published.load(std::memory_order_acquire) || moduleBase != g_moduleBase) return nullptr;
  for (size_t i = 0; i < kCount; ++i)
    if (kPatterns[i].referenceRva == referenceRva)
      return reinterpret_cast<void*>(g_addresses[i]);
  return nullptr;
}
const char* GameAddresses::Failure() { return g_failure; }
