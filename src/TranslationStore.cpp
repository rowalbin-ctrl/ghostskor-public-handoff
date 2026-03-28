#include "TranslationStore.h"

#include "Utils.h"
#include "vendor/nlohmann/json.hpp"

#include <unordered_map>
#include <mutex>
#include <sstream>
#include <string>

namespace TranslationStore {
namespace {

using json = nlohmann::json;

std::once_flag g_LoadOnce;
bool g_LoadSucceeded = false;
std::unordered_map<std::string, std::string> g_KeyToEnglish;
std::unordered_map<std::string, std::string> g_KeyToKorean;

// Rorke body text prefix map: first 20 uppercase chars of English body → {Korean body, pageId}
std::unordered_map<std::string, RorkeBodyEntry> g_RorkeBodyPrefix20;

std::string ResolveTranslationDir(const std::string &koreanPath) {
  if (!koreanPath.empty()) {
    const size_t slashPos = koreanPath.find_last_of("\\/");
    if (slashPos != std::string::npos) {
      return koreanPath.substr(0, slashPos);
    }
  }

  std::string moduleDir = GetModuleDirPath(nullptr);
  return moduleDir.empty() ? "." : moduleDir;
}

bool LoadJsonMap(const std::string &path, std::unordered_map<std::string, std::string> &out,
                 const char *label) {
  std::string fileText;
  if (!ReadTextUtf8(path, fileText)) {
    LogToFile(std::string("[TranslationStore] ERROR: Could not open ") + label +
              ": " + path);
    return false;
  }

  try {
    json j;
    std::istringstream stream(fileText);
    stream >> j;

    for (const auto &element : j.items()) {
      out[element.first] = element.second;
    }
    return true;
  } catch (const std::exception &e) {
    LogToFile(std::string("[TranslationStore] ERROR parsing ") + label + ": " +
              e.what());
    return false;
  } catch (...) {
    LogToFile(std::string("[TranslationStore] ERROR parsing ") + label);
    return false;
  }
}

bool LoadAllTranslations(const std::string &dir) {
  const std::string englishPath = dir + "\\localize.json";
  const std::string koreanPath = dir + "\\localize_kr.json";

  std::unordered_map<std::string, std::string> keyToEnglish;
  std::unordered_map<std::string, std::string> keyToKorean;

  if (!LoadJsonMap(englishPath, keyToEnglish, "localize.json")) {
    return false;
  }
  if (!LoadJsonMap(koreanPath, keyToKorean, "localize_kr.json")) {
    return false;
  }

  g_KeyToEnglish = std::move(keyToEnglish);
  g_KeyToKorean = std::move(keyToKorean);

  // Build Rorke body text prefix map (21 pages)
  // Use the first line (up to \n) as prefix, min 5 chars.
  // Games splits body at \n, so ADDCMD gets the first line only.
  g_RorkeBodyPrefix20.clear();
  for (int i = 1; i <= 21; i++) {
    char keyBuf[64];
    sprintf_s(keyBuf, "MENU_SP_VF_FILE_%02d_BODY", i);
    std::string key(keyBuf);
    auto engIt = g_KeyToEnglish.find(key);
    auto korIt = g_KeyToKorean.find(key);
    if (engIt != g_KeyToEnglish.end() && korIt != g_KeyToKorean.end()) {
      // Extract first line (before \n)
      std::string firstLine = engIt->second;
      size_t nlPos = firstLine.find('\n');
      if (nlPos != std::string::npos) firstLine = firstLine.substr(0, nlPos);
      // Trim trailing whitespace
      while (!firstLine.empty() && (firstLine.back() == ' ' || firstLine.back() == '\r'))
        firstLine.pop_back();
      // Store at multiple prefix lengths for reliable matching.
      // Hook tries 20,15,10,7,5 — store at each applicable length.
      // Determine header text for animated-header pages
      std::string hdrKor;
      switch (i) {
        case 1: hdrKor = "\xEC\x8B\xAC\xEB\xA6\xAC \xEB\xB3\xB4\xEA\xB3\xA0\xEC\x84\x9C - \xEA\xB0\x80\xEB\xB8\x8C\xEB\xA6\xAC\xEC\x97\x98 \xEB\xA1\x9C\xED\x81\xAC"; break; // 심리 보고서 - 가브리엘 로크
        case 6: hdrKor = "\xEC\x9E\x91\xEC\xA0\x84: \xEB\xB0\x98\xEC\x86\xA1"; break; // 작전: 반송
        case 8: case 9: hdrKor = "\xEC\x9E\x91\xEC\xA0\x84: \xEB\x8D\xB0\xEB\x93\x9C\xEB\xB3\xBC\xED\x8A\xB8"; break; // 작전: 데드볼트
        case 19: hdrKor = "\xED\x94\x84\xEB\xA1\x9C\xED\x95\x84: \xEC\x9D\xBC\xEB\x9D\xBC\xEC\x9D\xB4\xEC\x96\xB4\xEC\x8A\xA4 T. \xEC\x9B\x8C\xEC\xBB\xA4, \xEC\x98\x88\xEB\xB9\x84\xEC\x97\xAD \xEB\x8C\x80\xEC\x9C\x84"; break; // 프로필: 일라이어스 T. 워커, 예비역 대위
        case 20: hdrKor = "\xED\x94\x84\xEB\xA1\x9C\xED\x95\x84: \xEB\x8D\xB0\xEC\x9D\xB4\xEB\xB9\x84\xEB\x93\x9C \"헤쉬\" \xEC\x9B\x8C\xEC\xBB\xA4, \xEC\xA4\x91\xEC\x9C\x84"; break; // 프로필: 데이비드 "헤쉬" 워커, 중위
        case 21: hdrKor = "\xED\x94\x84\xEB\xA1\x9C\xED\x95\x84: \xED\x86\xA0\xEB\xA7\x88\xEC\x8A\xA4 A. \xEB\xA9\x94\xEB\xA6\xAD, \xEB\x8C\x80\xEC\x9C\x84"; break; // 프로필: 토마스 A. 메릭, 대위
      }
      for (size_t plen : {(size_t)20, (size_t)15, (size_t)10, (size_t)7, (size_t)5}) {
        if (firstLine.length() >= plen) {
          std::string prefix = firstLine.substr(0, plen);
          for (auto &c : prefix) c = (char)toupper((unsigned char)c);
          if (g_RorkeBodyPrefix20.find(prefix) == g_RorkeBodyPrefix20.end()) {
            g_RorkeBodyPrefix20[prefix] = {korIt->second, hdrKor, i};
          }
        }
      }
    }
  }

  LogToFile("[TranslationStore] Loaded " +
            std::to_string(g_KeyToEnglish.size()) + " English keys, " +
            std::to_string(g_KeyToKorean.size()) + " Korean keys, " +
            std::to_string(g_RorkeBodyPrefix20.size()) + " Rorke body prefixes.");
  return true;
}

} // namespace

bool EnsureLoaded() {
  return EnsureLoadedFromPath("");
}

bool EnsureLoadedFromPath(const std::string &koreanPath) {
  const std::string dir = ResolveTranslationDir(koreanPath);
  std::call_once(g_LoadOnce, [&]() { g_LoadSucceeded = LoadAllTranslations(dir); });
  return g_LoadSucceeded;
}

const std::unordered_map<std::string, std::string> &KeyToEnglish() {
  static const std::unordered_map<std::string, std::string> kEmpty;
  return g_LoadSucceeded ? g_KeyToEnglish : kEmpty;
}

const std::unordered_map<std::string, std::string> &KeyToKorean() {
  static const std::unordered_map<std::string, std::string> kEmpty;
  return g_LoadSucceeded ? g_KeyToKorean : kEmpty;
}

const std::unordered_map<std::string, RorkeBodyEntry> &RorkeBodyPrefix20() {
  static const std::unordered_map<std::string, RorkeBodyEntry> kEmpty;
  return g_LoadSucceeded ? g_RorkeBodyPrefix20 : kEmpty;
}

} // namespace TranslationStore
