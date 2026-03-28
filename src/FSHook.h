#pragma once
#include <string>
#include <unordered_map>

namespace FSHook {

// Hook DB_FindXAssetHeader to blank video subtitles and capture timing data
bool Initialize();

// Set English->Korean translation map for subtitle lookup
void SetEnglishToKoreanMap(const std::unordered_map<std::string, std::string>& map);

} // namespace FSHook
