# GhostsKor maintenance R4 — bounded lighting retries

R4 retains R3's exact Steam SP target and lighting command path, and bounds
unconfirmed submissions to THREE TOTAL: the first attempt plus two retries.
The minimum interval between submissions is five seconds, including across
a successful confirmation followed by another option change.

Missing/unregistered settings consume no attempt. Missing settings and
changes among non-4 values do not reset the count. Observing 4 re-arms the
policy for a later setting change; a delayed confirmation after the third
attempt is still accepted. After three unconfirmed attempts the patch only
observes the setting and logs the stopped state. Restarting the game creates
a fresh policy. This is an episode limit, not a lifetime limit after success.

If the queue function throws an SEH exception, the patch stops further queue
submissions for that process lifetime, even after a later observation of 4.
This does not repair engine state or guarantee recovery from an exception.

ghostskor_lighting.txt now includes the episode count and queue-exception
stop flag. Existing rendering, timer and wide HUD search behavior is unchanged.

Validation: the real policy header was exercised for late registration,
five-second spacing, exactly three submissions over ten simulated minutes,
non-reset on missing/incorrect values, delayed success and reapplication,
and a sticky queue-exception stop. MSVC build succeeded without compiler
warnings/errors. Local Defender definitions 1.459.93.0 found no threats.
Real game 0-to-4 command handling and the Ajax scene remain unvalidated.

DLL SHA-256:
5a9b7fc9600eba5c94c424171428dea3d693911f99dbb31c1b5c0c79cc778f8b

See MAINTENANCE_R3.md and MAINTENANCE_R2.md for previous changes and build
instructions. Always build with -NoDeploy. SCANNER_AUDIT.md explains the
old partial code scanner and the independently confirmed fixed-address crash.
