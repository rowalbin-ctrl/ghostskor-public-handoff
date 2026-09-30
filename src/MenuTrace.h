#pragma once

#ifndef GHOSTSKOR_MENU_TRACE
#define GHOSTSKOR_MENU_TRACE 0
#endif

namespace NativeMenuCapture {
// Diagnostic builds only: bounded, in-memory events read externally while the
// game runs. Production builds contain neither the buffer nor stack capture.
#if GHOSTSKOR_MENU_TRACE
void Trace(unsigned event, const char *text = nullptr, float a = 0,
           float b = 0, float c = 0, float d = 0);
void TraceSource(unsigned stage, const char *source, const char *translation = nullptr,
                 const char *key = nullptr, int maxChars = -1, float x = 0, float y = 0);
#else
inline void Trace(unsigned, const char * = nullptr, float = 0,
                  float = 0, float = 0, float = 0) {}
inline void TraceSource(unsigned, const char *, const char * = nullptr,
                        const char * = nullptr, int = -1, float = 0, float = 0) {}
#endif
}
