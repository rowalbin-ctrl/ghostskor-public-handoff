#pragma once
#include "MenuClip.h"
#include "NativeTextLayout.h"
#include "NativeMenuTextRun.h"

namespace NativeMenuCapture {
// Install only after GameBuild's executable and instruction checks succeed.
bool Initialize();
bool IsFrameActive();
MenuClipRect CurrentClip();
NativeTextLayout CurrentText();
NativeMenuTextRun *CurrentTextRun();
// Lives across the translation hook, so early-return paths preserve native layout.
class TextScope {
  NativeTextLayout previous;
public:
  TextScope(const char *text, int maxChars, void *font, float x, float y,
            float xScale, float yScale);
  ~TextScope();
};
}
