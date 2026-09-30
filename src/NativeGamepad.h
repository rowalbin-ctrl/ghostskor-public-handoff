#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace NativeGamepad {
// Mirrors the key subset tested by the native Steam SP binding resolver.
constexpr bool IsButton(unsigned key) {
  return (key >= 1 && key <= 6) || (key >= 14 && key <= 23) ||
         (key >= 28 && key <= 31);
}
struct BindingSnapshot {
  std::array<int32_t, 256> commands{};
  std::array<std::array<char, 64>, 101> names{};
};
inline std::array<unsigned char, 2> FindButtons(const BindingSnapshot& state,
                                                const char* command) {
  int action = 0;
  for (size_t i = 1; i < state.names.size(); ++i) {
    if (std::strcmp(state.names[i].data(), command) == 0) {
      action = static_cast<int>(i);
      break;
    }
  }
  std::array<unsigned char, 2> result{};
  if (!action) return result;
  size_t count = 0;
  for (unsigned key = 0; key < state.commands.size(); ++key) {
    if (IsButton(key) && state.commands[key] == action) {
      result[count++] = static_cast<unsigned char>(key);
      if (count == result.size()) break;
    }
  }
  return result;
}
inline std::string CanonicalCommand(std::string command) {
  if (command.rfind("+actionslot", 0) == 0 && command.size() == 12 &&
      command.back() >= '1' && command.back() <= '4') command.insert(11, " ");
  if (command == "+activate" || command == "+use" || command == "+reload") return "+usereload";
  if (command == "+togglecrouch") return "+stance";
  if (command == "+jump") return "+gostand";
  if (command == "+melee" || command == "+changezoom") return "+melee_zoom";
  if (command == "+sprint" || command == "+holdbreath") return "+breath_sprint";
  if (command == "+throw") return "+frag";
  if (command == "+attack2") return "+speed_throw";
  return command;
}
// false means the validated game state is unavailable, never "keyboard".
bool ReadEnabled(bool& enabled);
bool ReadBindings(BindingSnapshot& snapshot);
}
