#pragma once
#include <utility>
#include <vector>

// The caller holds the renderer mutex. Readers only see complete frames;
// publishing an empty frame removes all previously visible menu commands.
template<class Command> class MenuFrameBuffer {
  std::vector<Command> pending_, complete_;
  bool available_ = false;
public:
  void Begin() { pending_.clear(); }
  void Add(Command command) { pending_.push_back(std::move(command)); }
  void End() { complete_.swap(pending_); pending_.clear(); available_ = true; }
  void Clear() { pending_.clear(); complete_.clear(); available_ = false; }
  bool Available() const { return available_; }
  const std::vector<Command> &Current() const { return complete_; }
};
