#pragma once
#include "TextHook.h"
#include <algorithm>

namespace NativeSubtitleQueue {
// Call only for the engine's dialogue-window enqueue event. A localization
// lookup has no authority to create, refresh or postpone a visible subtitle.
inline bool Enqueue(std::vector<InGameSubtitleEntry> &queue,int window,
    const std::string &key,const std::string &english,const std::string &korean,
    unsigned long now,int duration,int fade,int flags,size_t capacity) {
  if(window!=4 || key.empty() || korean.empty() || !capacity)return false;
  // A repeated native event is a new lifetime (including checkpoint replay),
  // even when its text matches an earlier event still waiting to be pruned.
  queue.erase(std::remove_if(queue.begin(),queue.end(),[&](const InGameSubtitleEntry &e) {
    return e.key==key;
  }),queue.end());
  for(auto &e:queue) {
    int slot=e.renderSlot;
    if(slot<0 || slot>=(int)capacity)slot=std::clamp(e.stackIndex,0,(int)capacity-1);
    e.renderSlot=(std::min)(slot+1,(int)capacity-1);
    e.stackIndex=e.renderSlot;
    if(!e.pushAnimStart)e.pushAnimStart=now;
  }
  InGameSubtitleEntry entry{};
  entry.text=korean;entry.key=key;entry.english=english;
  entry.timestamp=entry.firstSeen=entry.runtimeTick=now;
  entry.msgTimeMs=duration;entry.fadeOutMs=fade;entry.runtimeArg6=flags;
  entry.stackIndex=entry.renderSlot=0;
  queue.insert(queue.begin(),std::move(entry));
  if(queue.size()>capacity)queue.resize(capacity);
  return true;
}
}
