#include "NativeSubtitleQueue.h"
#include <cassert>
#include <iostream>
int main() {
  std::vector<InGameSubtitleEntry> queue;
  const std::string key="SUBTITLE_CLOCKWORK_DIZ_NORTHERNRIDGE32";
  const std::string english="^2Kick: ^7You've got ten minutes to acquire transport and be inside Blackbird before we cut the power.";
  const std::string korean="^2킥: ^710분 안에 차량을 확보하고 블랙버드 내부로 진입해야 우리가 전력을 차단할 수 있어.";
  for(int window:{0,1,2,3,5})
    assert(!NativeSubtitleQueue::Enqueue(queue,window,key,english,korean,100,12345,789,32,4));
  assert(queue.empty());
  assert(!NativeSubtitleQueue::Enqueue(queue,4,key,english,"",100,12345,789,32,4));
  assert(NativeSubtitleQueue::Enqueue(queue,4,key,english,korean,100,12345,789,32,4));
  assert(queue.size()==1 && queue[0].text==korean && queue[0].english==english);
  assert(queue[0].firstSeen==100 && queue[0].msgTimeMs==12345 && queue[0].fadeOutMs==789 && queue[0].runtimeArg6==32);
  assert(!queue[0].nativeRemoveValid && queue[0].renderSlot==0);
  assert(NativeSubtitleQueue::Enqueue(queue,4,"SUBTITLE_OTHER","Picking it up","포착",500,1500,600,2,4));
  assert(queue[1].key==key && queue[1].firstSeen==100 && queue[1].pushAnimStart==500 && queue[1].renderSlot==1);
  queue[1].nativeRemoveValid=true;queue[1].nativeRemoveTick=999;
  // A repeated event has its own lifetime and cannot inherit removal from
  // the previous instance. Other dialogue keeps its first appearance time.
  assert(NativeSubtitleQueue::Enqueue(queue,4,key,english,korean,700,3456,456,16,4));
  assert(queue.size()==2 && queue[0].firstSeen==700 && queue[0].msgTimeMs==3456);
  assert(!queue[0].nativeRemoveValid && !queue[0].nativeSlotValid && queue[0].nativeRemoveTick==0);
  assert(queue[1].firstSeen==500 && queue[1].pushAnimStart==700);
  for(int i=0;i<8;++i)
    NativeSubtitleQueue::Enqueue(queue,4,"SUBTITLE_"+std::to_string(i),"Houston","휴스턴",1000+i,1000,200,0,4);
  assert(queue.size()==4 && queue.front().key=="SUBTITLE_7" && queue.back().key=="SUBTITLE_4");
  for(int i=0;i<4;++i)assert(queue[i].renderSlot==i);
  std::cout<<"PASS: native dialogue window ownership, no phrase blacklist, original duration/fade, repeated events, independent aging, bounded queue\n";
}
