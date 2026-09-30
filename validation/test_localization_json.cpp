#include "vendor/nlohmann/json.hpp"
#include <cassert>
#include <iostream>

static nlohmann::json Parse(const std::string &source) {
  nlohmann::json result; std::istringstream in(source); in >> result; return result;
}
int main(int argc, char **argv) {
  const auto map = Parse(R"({"A":"before","EMPTY":null,"B":"after","N2":null,"C":"last"})").data;
  assert(map.size() == 3 && map.at("A") == "before" && map.at("B") == "after" && map.at("C") == "last");
  const auto escaped = Parse(R"({"\u004B":"\uD55C\uAE00 \uD83D\uDE00\n\"\\\b\f"})");
  assert(escaped.data.at("K") == "한글 😀\n\"\\\b\f");
  assert(Parse("\xEF\xBB\xBF{}").data.empty());
  assert(Parse(R"({"A":"old","A":"new"})").data.at("A") == "new");
  assert(Parse(R"({"A":"old","A":null})").data.empty());
  for (const auto source : {"", "{", "[]", "{} trailing", "{\"A\":nullx}",
      "{\"A\":\"x\",}", "{\"A\" \"x\"}", "{\"A\":123}",
      "{\"A\":\"\\q\"}", "{\"A\":\"\\uXXXX\"}", "{\"A\":\"\\uD800\"}",
      "{\"A\":\"\\uDC00\"}", "{\"A\":\"unterminated}", "{\"A\":\"line\nbreak\"}"}) {
    nlohmann::json existing; existing.data["sentinel"] = "unchanged";
    bool rejected = false;
    try { std::istringstream in(source); in >> existing; }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected && existing.data.size() == 1 && existing.data.at("sentinel") == "unchanged");
  }
  if (argc == 3) {
    std::ifstream in(argv[1], std::ios::binary); assert(in);
    nlohmann::json actual; in >> actual;
    std::ofstream out(argv[2], std::ios::binary); assert(out);
    for (const auto &pair : actual.items()) {
      for (const auto *value : {&pair.first, &pair.second}) {
        const uint32_t size = static_cast<uint32_t>(value->size());
        out.write(reinterpret_cast<const char*>(&size), sizeof(size));
        out.write(value->data(), size);
      }
    }
    std::cout << "Parsed real localization file: " << actual.data.size() << " strings\n";
  }
  std::cout << "PASS: null boundaries, escaped keys/values, Unicode, malformed input, atomic map publication\n";
}
