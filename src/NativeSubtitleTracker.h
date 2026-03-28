#pragma once

// Prevent Windows.h min/max macro conflicts
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cmath>
#include <mutex>
#include "Utils.h"

// =============================================================================
// NativeSubtitleTracker - Tracks native subtitle render state from engine
// Captures subtitle animation state (alpha, position, timing) via
// R_AddCmdDrawText hook parameters.
// =============================================================================

namespace NativeSubtitleTracker {

struct SubtitleRenderState {
    // Position
    float x;
    float y;
    
    // Size
    float xScale;
    float yScale;
    float fontHeight;
    
    // Color/Alpha (MOST IMPORTANT FOR FADE SYNC)
    float color[4];  // RGBA, color[3] = alpha
    
    // Style
    int style;
    
    // Timing
    DWORD captureTime;
    DWORD lastChangeTime;
    
    // Animation detection
    float prevAlpha;
    bool isFadingIn;   // Alpha increasing
    bool isFadingOut;  // Alpha decreasing
    float fadeSpeed;   // Approximate fade rate per frame
    
    // Validity
    bool valid;
};

// Global subtitle state tracker
class SubtitleStateTracker {
public:
    static SubtitleStateTracker& Instance() {
        static SubtitleStateTracker instance;
        return instance;
    }
    
    // Update from R_AddCmdDrawText hook
    void UpdateFromRenderCall(float x, float y, float xScale, float yScale,
                              const float* color, float fontHeight, int style) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        DWORD now = GetTickCount();
        
        // Detect alpha changes for fade detection
        if (m_state.valid && color) {
            float alphaDelta = color[3] - m_state.prevAlpha;
            
            // Significant change threshold
            if (fabs(alphaDelta) > 0.01f) {
                m_state.isFadingIn = (alphaDelta > 0);
                m_state.isFadingOut = (alphaDelta < 0);
                
                // Calculate fade speed (alpha change per ms)
                DWORD timeDelta = now - m_state.lastChangeTime;
                if (timeDelta > 0) {
                    m_state.fadeSpeed = fabs(alphaDelta) / (float)timeDelta;
                }
                
                m_state.lastChangeTime = now;
            } else {
                // Alpha stable
                m_state.isFadingIn = false;
                m_state.isFadingOut = false;
            }
            
            m_state.prevAlpha = color[3];
        }
        
        // Update state
        m_state.x = x;
        m_state.y = y;
        m_state.xScale = xScale;
        m_state.yScale = yScale;
        m_state.fontHeight = fontHeight;
        m_state.style = style;
        m_state.captureTime = now;
        
        if (color) {
            m_state.color[0] = color[0];
            m_state.color[1] = color[1];
            m_state.color[2] = color[2];
            m_state.color[3] = color[3];
        }
        
        m_state.valid = true;
        
        // Diagnostic logging (throttled)
        LogStateChange();
    }
    
    // Get current state for rendering
    SubtitleRenderState GetState() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state;
    }
    
    // Check if subtitle is currently fading
    bool IsFading() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state.isFadingIn || m_state.isFadingOut;
    }
    
    // Predict alpha at future time (for smooth overlay sync)
    float PredictAlpha(DWORD futureTime) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (!m_state.valid || m_state.fadeSpeed == 0.0f) {
            return m_state.color[3];
        }
        
        DWORD timeDelta = futureTime - m_state.lastChangeTime;
        float predictedChange = m_state.fadeSpeed * (float)timeDelta;
        
        if (m_state.isFadingIn) {
            float result = m_state.color[3] + predictedChange;
            return (result > 1.0f) ? 1.0f : result;
        } else if (m_state.isFadingOut) {
            float result = m_state.color[3] - predictedChange;
            return (result < 0.0f) ? 0.0f : result;
        }
        
        return m_state.color[3];
    }
    
private:
    SubtitleStateTracker() : m_state{}, m_lastLogTime(0), m_logCount(0) {
        m_state.color[0] = 1.0f;
        m_state.color[1] = 1.0f;
        m_state.color[2] = 1.0f;
        m_state.color[3] = 1.0f;
    }
    
    void LogStateChange() {
        DWORD now = GetTickCount();
        
        // Log at most once per second, up to 50 total
        if ((now - m_lastLogTime) > 1000 && m_logCount < 50) {
            m_lastLogTime = now;
            m_logCount++;
            
            char buf[256];
            sprintf_s(buf, 
                "[NativeTracker] x=%.1f y=%.1f alpha=%.2f fading=%s speed=%.4f/ms",
                m_state.x, m_state.y, m_state.color[3],
                m_state.isFadingIn ? "IN" : (m_state.isFadingOut ? "OUT" : "NO"),
                m_state.fadeSpeed);
            LogToFile(buf);
        }
    }
    
    std::mutex m_mutex;
    SubtitleRenderState m_state;
    DWORD m_lastLogTime;
    int m_logCount;
};

// =============================================================================
// INITIALIZATION & DIAGNOSTIC
// =============================================================================

inline void Init() {
    LogToFile("[NativeSubtitleTracker] Initialized. "
              "Tracking subtitle render state from R_AddCmdDrawText.");
}

inline void LogDiagnostics() {
    auto state = SubtitleStateTracker::Instance().GetState();
    
    if (state.valid) {
        char buf[512];
        sprintf_s(buf,
            "[NativeTracker DIAG]\n"
            "  Position: (%.1f, %.1f)\n"
            "  Scale: (%.2f, %.2f)\n"
            "  FontHeight: %.1f\n"
            "  Color: (%.2f, %.2f, %.2f, %.2f)\n"
            "  Style: %d\n"
            "  Fading: %s (speed: %.4f/ms)\n"
            "  Age: %lu ms",
            state.x, state.y,
            state.xScale, state.yScale,
            state.fontHeight,
            state.color[0], state.color[1], state.color[2], state.color[3],
            state.style,
            state.isFadingIn ? "IN" : (state.isFadingOut ? "OUT" : "STABLE"),
            state.fadeSpeed,
            GetTickCount() - state.captureTime);
        LogToFile(buf);
    } else {
        LogToFile("[NativeTracker DIAG] No subtitle state captured yet.");
    }
}

} // namespace NativeSubtitleTracker
