#pragma once
#include <cstdint>

// Polling and retry policy only. No filesystem or game engine dependencies.
class DynamicLightPolicy {
public:
  static constexpr int RequiredValue = 4;
  // Initial submission plus at most two retries for one unconfirmed episode.
  static constexpr unsigned MaxAttempts = 3;
  bool BeginPoll(uint64_t now) {
    if (now < nextPoll_) return false;
    nextPoll_ = now + 1000;
    return true;
  }
  bool ShouldQueue(uint64_t now, bool registeredInteger, int current) {
    if (!registeredInteger) return false;
    if (current == RequiredValue) {
      attempts_ = 0;
      return false;
    }
    if (queueFault_ || attempts_ >= MaxAttempts) return false;
    if (now < nextRetry_) return false;
    nextRetry_ = now + 5000;
    ++attempts_;
    return true;
  }
  unsigned Attempts() const { return attempts_; }
  bool RetryLimitReached(uint64_t now) const {
    return attempts_ >= MaxAttempts && now >= nextRetry_;
  }
  void StopAfterQueueFault() { queueFault_ = true; }
  bool QueueFaulted() const { return queueFault_; }
private:
  uint64_t nextPoll_ = 0;
  uint64_t nextRetry_ = 0;
  unsigned attempts_ = 0;
  bool queueFault_ = false;
};
