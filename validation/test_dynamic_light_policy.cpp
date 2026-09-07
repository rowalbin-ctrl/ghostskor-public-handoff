#include "DynamicLightPolicy.h"
#include <cassert>
#include <iostream>

int main() {
  DynamicLightPolicy p;
  // First run: no registered setting yet, independent of config folder/files.
  assert(p.BeginPoll(0));
  assert(!p.ShouldQueue(0, false, 0));
  assert(!p.BeginPoll(999));
  // Late registration with 0: one command, then bounded retries if delayed.
  assert(p.BeginPoll(1000));
  assert(p.ShouldQueue(1000, true, 0));
  for (uint64_t t = 2000; t < 6000; t += 1000) {
    assert(p.BeginPoll(t));
    assert(!p.ShouldQueue(t, true, 0));
  }
  assert(p.BeginPoll(6000));
  assert(p.ShouldQueue(6000, true, 0));
  // Actual read-back confirmation stops commands; no one-time latch prevents
  // reapplying after the graphics menu changes the setting later.
  assert(p.BeginPoll(7000));
  assert(!p.ShouldQueue(7000, true, 4));
  assert(p.BeginPoll(8000));
  assert(!p.ShouldQueue(8000, true, 2)); // preserve 5-second minimum even after success
  assert(p.BeginPoll(9000));
  assert(!p.ShouldQueue(9000, false, 0));
  assert(p.BeginPoll(10000));
  assert(!p.ShouldQueue(10000, true, 4));
  assert(!p.ShouldQueue(20000, true, 4));
  assert(p.ShouldQueue(21000, true, 2));

  // No confirmation: stop after exactly three attempts, including the first.
  DynamicLightPolicy capped;
  unsigned submissions = 0;
  for (uint64_t t = 0; t <= 600000; t += 1000) {
    assert(capped.BeginPoll(t));
    if (capped.ShouldQueue(t, true, 0)) ++submissions;
  }
  assert(submissions == 3);
  assert(capped.Attempts() == 3);
  assert(capped.RetryLimitReached(600000));
  // Changing among wrong values or losing the dvar does not reset the cap.
  assert(!capped.ShouldQueue(601000, false, 0));
  assert(!capped.ShouldQueue(602000, true, 2));
  assert(!capped.ShouldQueue(603000, true, 0));
  // Late confirmation re-arms the policy for a genuinely new episode.
  assert(!capped.ShouldQueue(604000, true, 4));
  assert(capped.Attempts() == 0);
  assert(capped.ShouldQueue(605000, true, 1));
  assert(capped.Attempts() == 1);

  DynamicLightPolicy finalAttempt;
  assert(finalAttempt.ShouldQueue(0, true, 0));
  assert(finalAttempt.ShouldQueue(5000, true, 0));
  assert(finalAttempt.ShouldQueue(10000, true, 0));
  assert(!finalAttempt.RetryLimitReached(14999)); // allow final read-back interval
  assert(finalAttempt.RetryLimitReached(15000));
  assert(!finalAttempt.ShouldQueue(15000, true, 4));
  assert(!finalAttempt.RetryLimitReached(15000));

  // A queue exception is a sticky stop, even after observing 4 later.
  DynamicLightPolicy fault;
  assert(fault.ShouldQueue(0, true, 0));
  fault.StopAfterQueueFault();
  assert(fault.QueueFaulted());
  assert(!fault.ShouldQueue(5000, true, 0));
  assert(!fault.ShouldQueue(6000, true, 4));
  assert(!fault.ShouldQueue(12000, true, 2));
  std::cout << "PASS: late registration, polling, 5-second spacing, exactly 3 "
               "unconfirmed attempts, no reset on missing/wrong values, late "
               "confirmation and reapplication, sticky queue-exception stop.\n";
}
