# GhostsKor maintenance R3 — 2026-09-08

R3 extends R2 with a runtime `r_dlightLimit = 4` policy for the exact Steam
SP build 24723416. See MAINTENANCE_R2.md for the migration, HUD, timer and
build instructions; its lighting-guidance-only limitation is superseded here.

## Lighting

GameTweaks.cpp waits for the registered integer dvar, checks it once per
second, and queues `seta r_dlightLimit 4` through the game's synchronized
Cbuf_AddText path. The game command loop performs the write. Retries are
spaced by at least five seconds; observing value 4 stops command submission.
Later option changes can trigger reapplication. The DLL does not rewrite
config files or need a pre-existing players2 folder. Engine save behavior
handles persistence; runtime enforcement repeats each supported launch.

Runtime status is recorded beside the game executable in
ghostskor_lighting.txt. Verified means the observed setting is 4; it does
not certify the lighting output in the Ajax scene.

The setting registration, integer type/current offsets and command queue
locking were checked against local stock runtime code. There are now 36
runtime signature checks. validation/test_dynamic_light_policy.cpp includes
the actual platform-independent policy header and covers late registration,
poll/retry bounds, confirmation and reapplication.

## Validation and scope

MSVC x64 build succeeded without compiler warnings/errors. R3 passed the
local Defender custom file scan with definitions 1.459.93.0. It is unsigned.
Real game 0-to-4 transition and the Ajax scene still need validation.
R2's user-confirmed timer/performance result covers a loaded scene only.
4K visual checks and repeated startup menu checks remain pending.

Supported EXE SHA-256:
e9f62f2b780d2dce8180f4b5fd9f0022e10c9f2596cc8066128c21169b74f110

R3 DLL SHA-256:
eaf627a9602961fbc73d756f0c0d8ae32749943566c20eb9fbdfcbfbe31cde3c

Unknown executable names/hashes are logged when the proxy loads, but game
hooks remain disabled. These guards do not discover replacement addresses
or automatically support future EXEs. Game Pass support was excluded at
the user's request. See VERSION_SUPPORT.md for the distinction between
runtime HUD searches and executable compatibility checks.

The maintenance archive contains published assets/source, reviewed changes
and validation sources. It excludes game EXEs, process captures, saves,
personal configuration, raw font files and toolchains.
