#include "NativeBindingPrompt.h"
#include "vendor/nlohmann/json.hpp"
#include <cassert>
#include <iostream>

int main(int argc, char **argv) {
  using namespace NativeBindingPrompt;
  assert(IsBindingOnly("^3[{+activate}]^7"));
  assert(IsBindingOnly("^3 [{+activate}] ^7\n"));
  assert(IsBindingOnly("[{+toggleads,+toggleads_throw,+speed_throw}]"));
  assert(IsBindingOnly("[{+attack}]\n[{+usereload}]"));
  for (auto bad : {"", " ", "^3^7", "Press [{+activate}]", "[{+activate}] 키",
                   "[{+}]", "[{+attack", "[{+attack]}", "[{+attack }]"})
    assert(!IsBindingOnly(bad));
  assert(!IsUnchanged("[{+activate}]", "[{+attack}]"));
  assert(!IsUnchanged("Press [{+activate}]", "Press [{+activate}]"));
  assert(argc == 3);
  nlohmann::json english, korean;
  std::ifstream en(argv[1], std::ios::binary), kr(argv[2], std::ios::binary);
  assert(en && kr); en >> english; kr >> korean;
  // Every button in the old shrink/pulse whitelist must use the engine.
  for (auto key : {"SKYWAY_HINT_RELOAD", "FLOOD_ENDING_QTE_2_PROMPT",
                  "NML_HINT_X", "NML_HINT_X_KB", "SHIP_GRAVEYARD_HINT_RT"}) {
    assert(IsUnchanged(english.data.at(key), korean.data.at(key)));
  }
  for (auto key : {"FLOOD_ENDING_QTE_2_PROMPT_TEXT", "FLOOD_ENDING_QTE_0_PROMPT",
                  "SHIP_GRAVEYARD_HINT_RT_TEXT", "SKYWAY_OBJ_FINDBOSS", "EXE_GAMESAVED"}) {
    assert(!IsUnchanged(english.data.at(key), korean.data.at(key)));
  }
  size_t count = 0;
  for (const auto &pair : english.items()) {
    auto k = korean.data.find(pair.first);
    if (k != korean.data.end() && IsUnchanged(pair.second, k->second)) {
      std::cout << pair.first << '\n'; ++count;
    }
  }
  std::cout << "PASS: " << count << " unchanged binding-only messages use native rendering; translated captions stay translated\n";
}
