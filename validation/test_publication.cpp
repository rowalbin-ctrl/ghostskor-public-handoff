#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "GameAddresses.h"
#include "AddressPatternMatcher.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <string>
#include <cstdlib>
using AddressPatternMatcher::Pattern;
constexpr Pattern patterns[] = {
#include "GameAddressPatterns.inl"
};
int main(int argc, char** argv) {
  assert(argc >= 4 && (argc-2)%2==0);
  const std::string mode = argv[1];
  const uintptr_t shift = mode=="moved" ? 0x100000 : 0;
  struct Region { uintptr_t rva; std::vector<unsigned char> bytes; };
  std::vector<Region> regions;
  size_t imageSize = 4096;
  for (int i=2;i<argc;i+=2) {
    std::ifstream file(argv[i+1],std::ios::binary);
    assert(file);
    Region region{static_cast<uintptr_t>(std::strtoull(argv[i],nullptr,0))+shift,
                  std::vector<unsigned char>(std::istreambuf_iterator<char>(file),{})};
    const size_t end=region.rva+region.bytes.size();
    if(end>imageSize)imageSize=end;
    regions.push_back(std::move(region));
  }
  imageSize=(imageSize+4095)&~size_t(4095);
  if(mode=="duplicate") {
    regions.push_back({imageSize,std::vector<unsigned char>(patterns[0].bytes,
                          patterns[0].bytes+patterns[0].size)});
    imageSize+=4096;
  }
  auto* image=static_cast<unsigned char*>(VirtualAlloc(nullptr,imageSize,MEM_RESERVE,PAGE_NOACCESS));
  assert(image && VirtualAlloc(image,4096,MEM_COMMIT,PAGE_READWRITE));
  const uintptr_t base=reinterpret_cast<uintptr_t>(image);
  auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(image);
  dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=0x80;
  auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(image+0x80);
  nt->Signature=IMAGE_NT_SIGNATURE;
  nt->FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64);
  nt->FileHeader.NumberOfSections=static_cast<WORD>(regions.size());
  nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  nt->OptionalHeader.SizeOfImage=static_cast<DWORD>(imageSize);
  auto* headers=IMAGE_FIRST_SECTION(nt);
  for(size_t i=0;i<regions.size();++i){
    const auto& r=regions[i];
    assert(VirtualAlloc(image+r.rva,r.bytes.size(),MEM_COMMIT,PAGE_READWRITE));
    std::memcpy(image+r.rva,r.bytes.data(),r.bytes.size());
    headers[i].VirtualAddress=static_cast<DWORD>(r.rva);
    headers[i].Misc.VirtualSize=static_cast<DWORD>(r.bytes.size());
    headers[i].Characteristics=IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ;
  }
  assert(GameAddresses::Resolve(base,patterns[0].referenceRva)==nullptr);
  if(mode=="missing") std::memset(image+patterns[0].referenceRva,0,patterns[0].size);
  if(mode=="unreadable"){
    DWORD previous=0;
    assert(VirtualProtect(image+regions[0].rva,4096,PAGE_NOACCESS,&previous));
    assert(!GameAddresses::Initialize(base));
    assert(GameAddresses::Resolve(base,patterns[0].referenceRva)==nullptr);
    DWORD ignored=0;
    assert(VirtualProtect(image+regions[0].rva,4096,previous,&ignored));
  }
  const bool expected=mode=="stock"||mode=="unreadable";
  const bool actual=GameAddresses::Initialize(base);
  if(actual!=expected){std::cerr<<mode<<": "<<GameAddresses::Failure()<<'\n';return 1;}
  for(const auto& p:patterns)
    assert(GameAddresses::Resolve(base,p.referenceRva)==
           (expected?reinterpret_cast<void*>(base+p.referenceRva):nullptr));
  assert(GameAddresses::Resolve(base,0xDEADBEEF)==nullptr);
  assert(GameAddresses::Resolve(base+1,patterns[0].referenceRva)==nullptr);
  assert(GameAddresses::Initialize(base)==expected);
  std::cout<<"PASS publication "<<mode<<": "<<GameAddresses::Failure()<<'\n';
  // No captured instruction is executed: fixture pages stay non-executable.
  VirtualFree(image,0,MEM_RELEASE);
}
