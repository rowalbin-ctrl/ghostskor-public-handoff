
// Generate a hash from text + position for color tracking
static uint32_t HashTextPosition(const char *text, float x, float y) {
  uint32_t hash = 2166136261u;
  // Hash text (first 32 chars for speed)
  int count = 0;
  while (*text && count < 32) {
    hash ^= (unsigned char)(*text++);
    hash *= 16777619u;
    count++;
  }
  // Hash position (rounded to nearest 10 pixels for tolerance)
  int ix = (int)(x / 10.0f);
  int iy = (int)(y / 10.0f);
  hash ^= (uint32_t)ix;
  hash *= 16777619u;
  hash ^= (uint32_t)iy;
  hash *= 16777619u;
  return hash;
}

// Update color cache and return true if color changed
static bool UpdateColorCache(uint32_t hash, float r, float g, float b,
                             float a) {
  if (!g_ColorCacheInitialized) {
    for (int i = 0; i < COLOR_CACHE_SIZE; i++) {
      g_ColorCache[i].valid = false;
    }
    g_ColorCacheInitialized = true;
  }

  int idx = hash % COLOR_CACHE_SIZE;
  TextColorEntry &entry = g_ColorCache[idx];

  DWORD now = GetTickCount();

  // Check if color changed significantly (threshold to avoid floating point
  // noise)
  bool colorChanged = false;
  if (entry.valid) {
    float dr = entry.r - r;
    float dg = entry.g - g;
    float db = entry.b - b;
    float da = entry.a - a;
    float diff = dr * dr + dg * dg + db * db + da * da;
    if (diff > 0.01f) { // Significant color change
      colorChanged = true;
    }
  }

  // Update cache
  entry.r = r;
  entry.g = g;
  entry.b = b;
  entry.a = a;
  entry.lastUpdate = now;
  entry.valid = true;

  return colorChanged;
}

// =============================================================================

// LOGGING POLICY: Minimal Spam, Logic Oriented

// =============================================================================

// - Filter 1 (Hot Path): Ring buffer of recent hashes (size 64) -> Drops

// immediate frame spam

// - Filter 2 (Logger): Session-wide std::set<uint32_t> -> Logs unique strings

// only ONCE per run

// =============================================================================

// FNV-1a Hash (XMM-safe, no allocations)

static uint32_t HashString(const char *str) {

  uint32_t hash = 2166136261u;

  while (*str) {

    hash ^= (unsigned char)(*str++);

    hash *= 16777619u;
  }

  return hash;
}

// Check if hash exists in recent history (Linear scan is fast for 64 items)

static bool IsRecentlySeen(uint32_t hash) {

  for (int i = 0; i < 64; i++) {

    if (g_RecentHashes[i] == hash)

      return true;
  }

  return false;
}
static void MarkAsSeen(uint32_t hash) {

  int idx = g_RecentHashIndex.fetch_add(1) % 64;

  g_RecentHashes[idx] = hash;
}
DWORD WINAPI LoggerThread(LPVOID lpParam) {
  if (!kEnableRuntimeTextCapture) {
    return 0;
  }

  LogToFile("[TextHook] Logger thread started (Minimal Policy).");

  std::set<uint32_t> sessionHashes;

  while (g_LoggingEnabled.load()) {

    for (int i = 0; i < MAX_QUEUE_SIZE; i++) {

      if (g_Queue[i].ready.load()) {

        // Calculate hash of the queued string

        uint32_t hash = HashString(g_Queue[i].text);

        // Filter 2: Session Check

        if (sessionHashes.find(hash) == sessionHashes.end()) {

          sessionHashes.insert(hash);

          // Log unique event

          std::string msg = "[TextHook] New Text: \"";

          msg += g_Queue[i].text;

          msg += "\"";

          LogToFile(msg);
        }

        g_Queue[i].ready.store(false);
      }
    }

    Sleep(50); // Check frequently to drain queue
  }

  LogToFile("[TextHook] Logger thread stopped.");

  return 0;
}
static bool FileExistsSimple(const std::string &path) {
  if (path.empty())
    return false;
  std::wstring widePath = Utf8ToWide(path);
  if (widePath.empty())
    return false;
  DWORD attrs = GetFileAttributesW(widePath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES)
    return false;
  if (attrs & FILE_ATTRIBUTE_DIRECTORY)
    return false;
  return true;
}
static void SaveMissingSubtitles() {
  std::lock_guard<std::mutex> lock(g_MissingKeysMutex);
  if (g_MissingSubtitleKeys.empty())
    return;

  std::string basePath = GetModuleDirPath(nullptr);
  if (basePath.empty()) {
    return;
  }
  std::string filePath = basePath + "\\missing_subtitles.json";

  // Read existing file to merge
  std::set<std::string> existingKeys;
  std::string existingJson;
  if (ReadTextUtf8(filePath, existingJson)) {
    std::istringstream inFile(existingJson);
    std::string line;
    while (std::getline(inFile, line)) {
      // Parse existing keys from JSON format
      size_t keyStart = line.find("\"key\":");
      if (keyStart != std::string::npos) {
        size_t quoteStart = line.find("\"", keyStart + 6);
        size_t quoteEnd = line.find("\"", quoteStart + 1);
        if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
          existingKeys.insert(
              line.substr(quoteStart + 1, quoteEnd - quoteStart - 1));
        }
      }
    }
  }

  // Merge new keys
  for (const auto &key : g_MissingSubtitleKeys) {
    existingKeys.insert(key);
  }

  // Write all keys
  std::ostringstream outFile;
  outFile << "{\n  \"missing_subtitles\": [\n";
  bool first = true;
  for (const auto &key : existingKeys) {
    if (!first)
      outFile << ",\n";
    first = false;
    outFile << "    { \"key\": \"" << key << "\", \"korean\": \"\" }";
  }
  outFile << "\n  ]\n}\n";
  if (WriteTextUtf8(filePath, outFile.str())) {
    LogToFile("[MissingCollector] Saved " +
              std::to_string(existingKeys.size()) +
              " missing subtitle keys to missing_subtitles.json");
  }
}
static void LogCodeBytes(const char *tag, uintptr_t addr, int len) {
  if (!tag || !addr || len <= 0)
    return;
  if (!IsSafeRead((void *)addr, (size_t)len)) {
    std::stringstream ss;
    ss << "[SUBREV] " << tag << " unreadable addr=0x" << std::hex << addr;
    LogToFile(ss.str());
    return;
  }

  const unsigned char *b = (const unsigned char *)addr;
  for (int off = 0; off < len; off += 32) {
    int row = (len - off > 32) ? 32 : (len - off);
    std::stringstream ss;
    ss << "[SUBREV] " << tag << " +0x" << std::hex << std::setw(2)
       << std::setfill('0') << off << ": ";
    for (int j = 0; j < row; ++j) {
      ss << std::hex << std::setw(2) << std::setfill('0')
         << (int)b[off + j] << " ";
    }
    LogToFile(ss.str());
  }
}
static void SubRev_LogRipDetailsOnce(uintptr_t rip, uintptr_t ripOff) {
  if (rip == 0 || ripOff == 0)
    return;

  {
    std::lock_guard<std::mutex> lock(g_SubRevRipDumpMutex);
    if (!g_SubRevRipDumped.insert(ripOff).second) {
      return;
    }
  }

  char tag[64] = {};
  sprintf_s(tag, "RIP+0x%llX", (unsigned long long)ripOff);

  // Dump a small window around RIP to help offline reversing.
  uintptr_t start = (rip > 64) ? (rip - 64) : rip;
  LogCodeBytes(tag, start, 128);
  LogRelativeCalls(tag, start, 128);
}
static void SubRev_LogFloat4Candidates(const char *tag, uintptr_t addr,
                                       int scanBytes, int limit) {
  if (!tag || !addr || scanBytes < 16 || limit <= 0)
    return;
  if (!IsSafeRead((void *)addr, (size_t)scanBytes)) {
    std::stringstream ss;
    ss << "[SUBREV_SLOT] " << tag << " unreadable addr=0x" << std::hex << addr;
    LogToFile(ss.str());
    return;
  }

  const unsigned char *p = (const unsigned char *)addr;
  int found = 0;
  for (int off = 0; off <= (scanBytes - 16); off += 4) {
    float r = *(const float *)(p + off + 0);
    float g = *(const float *)(p + off + 4);
    float b = *(const float *)(p + off + 8);
    float a = *(const float *)(p + off + 12);

    auto finite = [](float v) -> bool { return std::isfinite(v) != 0; };
    auto in01 = [](float v) -> bool { return v >= 0.0f && v <= 1.0f; };
    if (!finite(r) || !finite(g) || !finite(b) || !finite(a))
      continue;
    if (!in01(r) || !in01(g) || !in01(b) || !in01(a))
      continue;
    if ((r + g + b) < 0.20f)
      continue;

    char buf[256];
    sprintf_s(buf, "[SUBREV_SLOT] %s vec4@+0x%X = (%.3f, %.3f, %.3f, %.3f)",
              tag, off, r, g, b, a);
    LogToFile(buf);
    if (++found >= limit)
      break;
  }
  if (found == 0) {
    LogToFile(std::string("[SUBREV_SLOT] ") + tag +
              " vec4 candidates: none");
  }
}
static void LogRelativeCalls(const char *tag, uintptr_t addr, int len) {
  if (!tag || !addr || len < 5)
    return;
  if (!IsSafeRead((void *)addr, (size_t)len))
    return;

  const unsigned char *b = (const unsigned char *)addr;
  int logged = 0;
  for (int i = 0; i <= (len - 5); ++i) {
    if (b[i] != 0xE8)
      continue; // call rel32
    int32_t rel = *(const int32_t *)(b + i + 1);
    uintptr_t src = addr + (uintptr_t)i;
    uintptr_t dst = src + 5 + (intptr_t)rel;
    std::stringstream ss;
    ss << "[SUBREV] " << tag << " call@+0x" << std::hex << i << " -> +0x"
       << (dst - (uintptr_t)GetModuleHandleA(NULL));
    LogToFile(ss.str());
    if (++logged >= 12)
      break;
  }
}
