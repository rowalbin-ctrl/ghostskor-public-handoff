#include <cassert>
#include <iostream>
#include "FSHook.cpp"
#include "GameAddresses.h"
static std::string seenKey,seenEnglish;
static int loads=0;
void TextHook_RegisterRuntimeEnglishKey(const char* key,const char* english) {
  seenKey=key;seenEnglish=english;
}
void BinkHook::LoadSubtitlesFromEntries(const std::vector<VideoSubtitle>&) {}
void* GameAddresses::Resolve(uintptr_t,uintptr_t){return nullptr;}
bool Detour::Install(void*,void*,void**,int){return false;}
const char* Detour::ActiveBackendName(){return "test";}
static FSHook::LocalizeEntry entry{};
static void* __fastcall Original(int type,const char* key,int allow) {
  assert(type==0x21 && std::string(key)==entry.name && allow==0);
  ++loads;return &entry;
}
int main() {
  FSHook::g_Original_DB_FindXAssetHeader=Original;
  for(const char* key:{"CORNERED_INTROSCREEN_LINE_1","EXE_GAMESAVED","PLATFORM_HOLD_TO_USE"}) {
    const char* native="Original engine text";entry={native,key};
    for(int repeat=0;repeat<20;++repeat) {
      assert(FSHook::Hooked_DB_FindXAssetHeader(0x21,key,0)==&entry);
      assert(entry.value==native && seenKey==key && seenEnglish==native);
    }
  }
  assert(loads==60);
  std::cout<<"PASS: production asset lookup preserves intro/status/hint values through repeated lookups; registers content only\n";
}
