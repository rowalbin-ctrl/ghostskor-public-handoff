#include "AddressPatternMatcher.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <chrono>
#include <cstdlib>
using namespace AddressPatternMatcher;
constexpr Pattern patterns[] = {
#include "GameAddressPatterns.inl"
};
int main(int argc, char** argv) {
  const Pattern small{"small", 0x1234, 3, 0, 1, -1, "\xAA\x00\xBB", "x?x"};
  const unsigned char bytes[] = {0, 0xAA, 0x35, 0xBB};
  Match match;
  Search(bytes, sizeof(bytes), 0x1000, small, match);
  assert(match.count == 1 && match.rva == 0x1001); // final legal position
  Match shortMatch;
  Search(bytes, 2, 0, small, shortMatch);
  assert(shortMatch.count == 0);
  Search(nullptr, 10, 0, small, shortMatch);
  assert(shortMatch.count == 0);
  Search(bytes, sizeof(bytes), 0x9000, small, match);
  assert(match.count == 2); // reject duplicates across sections
  const unsigned char duplicate[] = {0xAA, 1, 0xBB, 0xAA, 2, 0xBB};
  Match duplicateMatch;
  Search(duplicate, sizeof(duplicate), 0, small, duplicateMatch);
  assert(duplicateMatch.count == 2);
  const Pattern rel{"rel", 0, 5, 0, 1, 1, "\xE8\x00\x00\x00\x00", "x????"};
  const unsigned char backwards[] = {0xE8, 0xF0, 0xFF, 0xFF, 0xFF};
  Match relMatch;
  Search(backwards, sizeof(backwards), 0x2000, rel, relMatch);
  assert(relMatch.count == 1 && relMatch.rva == 0x1FF5);

  struct Region { uintptr_t rva; std::vector<unsigned char> bytes; };
  std::vector<Region> regions;
  assert(argc > 1 && (argc - 1) % 2 == 0);
  for (int i = 1; i < argc; i += 2) {
    std::ifstream file(argv[i+1], std::ios::binary);
    if (!file) { std::cerr << "Cannot open capture\n"; return 1; }
    regions.push_back({static_cast<uintptr_t>(std::strtoull(argv[i], nullptr, 0)),
                      std::vector<unsigned char>(std::istreambuf_iterator<char>(file), {})});
  }
  const auto started = std::chrono::steady_clock::now();
  unsigned resolved = 0;
  for (const auto& p : patterns) {
    Match stock, shifted;
    for (const auto& region : regions) {
      Search(region.bytes.data(), region.bytes.size(), region.rva, p, stock);
      Search(region.bytes.data(), region.bytes.size(), region.rva + 0x100000, p, shifted);
    }
    if (stock.count != 1 || stock.rva != p.referenceRva ||
        shifted.count != 1 || shifted.rva != p.referenceRva + 0x100000) {
      std::cerr << p.name << " failed: count=" << stock.count << ", RVA="
                << std::hex << stock.rva << '\n'; return 1;
    }
    // Changes to an exact instruction byte are rejected; wildcard operands
    // remain accepted. This uses the actual production pattern and matcher.
    std::vector<unsigned char> copy(p.bytes, p.bytes + p.size);
    copy[p.anchorOffset] ^= 0xFF;
    Match bad;
    Search(copy.data(), copy.size(), 0x1000000, p, bad);
    assert(bad.count == 0);
    copy.assign(p.bytes, p.bytes + p.size);
    for (size_t j = 0; j < p.size; ++j)
      if (p.mask[j] == '?' && (p.relativeDispOffset < 0 ||
          j < static_cast<size_t>(p.relativeDispOffset) ||
          j >= static_cast<size_t>(p.relativeDispOffset) + 4)) copy[j] ^= 0x55;
    Match wild;
    Search(copy.data(), copy.size(), 0x1000000, p, wild);
    assert(wild.count == 1);
    ++resolved;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now()-started).count();
  std::cout << "PASS: " << resolved << " stock and synthetically relocated targets; "
               "duplicate, missing, exact-byte change, wildcard, bounds and rel32 checks. "
            << elapsed << " ms including doubled scans and mutation checks.\n";
  std::cout << "This does not validate changed ABIs, data layouts or a future game EXE.\n";
}
