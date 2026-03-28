#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>

// =============================================================================
// BinkHook - Hooks bink2w64.dll to detect video playback and timing
// =============================================================================

namespace BinkHook {

// Initialize hooks on bink2w64.dll
bool Init();

// Shutdown and unhook
void Shutdown();

// Check if a video is currently playing
bool IsVideoPlaying();

// Check if the current Bink session matches a known subtitle-backed cutscene.
bool IsCutsceneVideoPlaying();

// Get current video name (e.g., "black_ice_load")
std::string GetCurrentVideoName();

// Get current playback time in seconds
double GetCurrentPlaybackTime();

// Get current frame number
unsigned int GetCurrentFrame();

// Video subtitle entry with timing
struct VideoSubtitle {
    std::string videoName;
    double startTime;
    double endTime;
    std::string englishText;
    std::string koreanText;
};

// Load subtitles from CSV file
bool LoadSubtitlesCSV(const std::string& csvPath);

// Load subtitles from STRINGTABLE extraction (in-memory)
void LoadSubtitlesFromEntries(const std::vector<VideoSubtitle>& entries);

// Get the subtitle that should be displayed at current time
// Returns empty string if no subtitle
std::string GetCurrentSubtitle();

// Get current English subtitle (for suppression)
std::string GetCurrentEnglishSubtitle();

// Returns true only for tiny empty gaps where previous and next subtitle
// resolve to the same displayed sentence (bridge-safe).
bool ShouldBridgeCurrentSubtitleGap(unsigned int holdMs);

// Run deferred state transitions that are unsafe in Bink callbacks.
void PumpDeferredWork();

// Load Korean translations for matching (by English text)
void LoadKoreanTranslations(const std::unordered_map<std::string, std::string>& translations);

} // namespace BinkHook
