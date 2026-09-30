#include "SubtitleRing.h"
#include <array>
#include <cassert>
#include <iostream>
#include <limits>

int main() {
  const char source[] = "abcdefgh";
  for (size_t length = 1; length <= 8; ++length) {
    for (size_t offset = 0; offset < 8; ++offset) {
      std::array<char, 12> target; target.fill('#');
      assert(SubtitleRing::CopyText(target.data() + 1, 10, source, 8, offset, length));
      for (size_t i = 0; i < length; ++i)
        assert(target[1 + i] == source[(offset + i) % 8]);
      assert(target[1 + length] == 0 && target.front() == '#' && target.back() == '#');
    }
  }
  for (size_t offset : {size_t(8), size_t(9), std::numeric_limits<size_t>::max()}) {
    std::array<char, 10> target; target.fill('#');
    const auto before = target;
    assert(!SubtitleRing::CopyText(target.data(), target.size(), source, 8, offset, 1));
    assert(target == before);
  }
  char target[10]{};
  assert(!SubtitleRing::CopyText(target, 10, source, 8, 0, 9));
  assert(!SubtitleRing::CopyText(target, 3, source, 8, 0, 3));
  assert(!SubtitleRing::CopyText(target, 10, source, 0, 0, 1));
  assert(!SubtitleRing::CopyText(target, 10, source, 8, 0, 0));
  std::cout << "PASS: circular subtitle copy, exact end, wrap, stale offsets, oversized entries and destination guards\n";
}
