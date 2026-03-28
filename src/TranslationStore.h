#pragma once

#include <string>
#include <unordered_map>

namespace TranslationStore {

struct RorkeBodyEntry {
  std::string korean;
  std::string headerKorean; // empty if no animated header for this page
  int pageId;
};

bool EnsureLoaded();
bool EnsureLoadedFromPath(const std::string &koreanPath);

const std::unordered_map<std::string, std::string> &KeyToEnglish();
const std::unordered_map<std::string, std::string> &KeyToKorean();
const std::unordered_map<std::string, RorkeBodyEntry> &RorkeBodyPrefix20();

} // namespace TranslationStore
