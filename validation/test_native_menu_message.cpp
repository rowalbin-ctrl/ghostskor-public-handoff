#include "NativeMenuTextRun.h"
#include "vendor/nlohmann/json.hpp"
#include <cassert>
#include <iostream>

int main(int argc, char **argv) {
  assert(argc == 2);
  nlohmann::json kr;
  std::ifstream f(argv[1], std::ios::binary); assert(f); f >> kr;
  // Exact LUI source recorded from the current Steam SP executable.
  const char *source = "\x1f" "If you quit now, " "\x1e\x1f" "you will lose any progress "
                       "\x1e\x1f" "since your last checkpoint. " "\x1e";
  auto decoded = NativeMenuTextRun::DecodeLiteralJoin(source);
  assert(decoded == "If you quit now, you will lose any progress since your last checkpoint. ");
  decoded.pop_back(); // the existing lookup normalizes outer whitespace
  const auto &body = kr.data.at(decoded);
  assert(body == "지금 종료하면 마지막 체크포인트 이후의 모든 진행 상황을 잃게 됩니다.");
  assert(body.find("정말 종료하시겠습니까?") == std::string::npos);
  assert(kr.data.at("CGAME_SAVE_WARNING").find("정말 종료하시겠습니까?") != std::string::npos);
  // A single literal menu label must use its established translation route.
  for (const char *label : {"CAMPAIGN", "\x1f" "CAMPAIGN" "\x1e", "\x1f" "EXTINCTION" "\x1e",
                           "\x1f" "Are you sure you want to quit?" "\x1e"})
    assert(NativeMenuTextRun::DecodeLiteralJoin(label).empty());
  for (const char *invalid : {"\x1f" "missing close", "\x1f" "a" "\x1e" "raw tail",
                             "\x1e" "MENU_X" "\x1f" " literal" "\x1e",
                             "\x1f" "a" "\x1e\x1f" "b", "\x1f" "a" "\x1e\x1f\x16" "b" "\x1e"})
    assert(NativeMenuTextRun::DecodeLiteralJoin(invalid).empty());
  assert(NativeMenuTextRun::DecodeLiteralJoin("\x1f" "ab" "\x1e\x1f" "cd" "\x1e") == "abcd");
  assert(NativeMenuTextRun::DecodeLiteralJoin("\x1f" "one\n" "\x1e\x1f" "two" "\x1e") == "one\ntwo");
  std::cout << "PASS: captured LUI literal spans, exact installed warning, preserved standalone labels/question, strict mixed-stream rejection\n";
}
