# GhostsKor R5 — validated function lookup and centralized address profile

R5 replaces unchecked construction of the reviewed game function pointers
with a shared resolver. It does NOT enable arbitrary future EXEs. The exact
Steam 24723416 file gate and 36 runtime layout/code checks remain necessary.

## Implementation

- GameAddressPatterns.inl contains 31 unique patterns for the reviewed game
  functions. Relative branch targets and RIP-relative displacements are masked;
  instruction, register, stack and member-offset bytes remain constrained.
- Short indistinguishable leaf functions are resolved by a unique caller
  context and decoding its rel32 target, rather than accepting the first
  generic prologue match.
- GameAddresses.cpp scans readable executable image sections before any game
  hooks are installed. Missing, duplicate or profile-inconsistent matches fail
  initialization. The table is published atomically only after all 31 pass.
- IW6Offsets::GetAddress now reads that table. It returns null before table
  publication or for an unknown reference ID/module. There is no unchecked
  base-plus-reference fallback for these callable addresses.
- 95 distinct inline function/caller/data/probe values were moved into
  GameAddressProfile.h; the existing large recovery-search bound was also
  centralized. Their numeric values were preserved. This does not mean every
  historical probe is a proven current data address. Those fallback probes
  and broad HUD recovery searches retain the existing behavior.
- The build runs verify_addresses.ps1 to catch direct additions of catalogued
  function IDs and large inline image RVAs. This is a source-pattern check,
  not a complete C++ semantic proof against every possible bypass.
- R4's three-total-attempt lighting policy and exception stop are retained.

## Validation

The actual production matcher found all 31 targets in local stock code
captures (.text sections), with the additional .interpr section read from
the matching disk EXE. Search tests also covered a synthetic 1 MiB section
shift, missing/duplicate matches, modified instruction bytes, changed masked
operands, final-position bounds and signed rel32 decoding.

The actual GameAddresses.cpp implementation was tested against synthetic PE
memory containing that code as non-executable data. Stock initialization
published all pointers; missing, duplicate and moved-profile cases published
none. An unreadable region deferred initialization and succeeded after it
became readable. Wrong module/unknown IDs returned null. No captured game
instruction was executed by these tests.

The build audit's positive/negative cases passed. All 95 extracted values
and the broad recovery-search bound were checked against the R4 baseline.
The 4K/HUD/timer condition harness and 36 stock code checks passed. These
checks do not replace real-game startup and mission validation.

## Future EXE updates

The scanner can discover a shifted function in the synthetic search test.
The runtime deliberately refuses to combine it with unverified caller/data
locations from the existing profile. Unknown EXEs still use original DXGI
without game hooks. Thus R5 improves coverage and maintenance and removes
unchecked callable-address fallbacks, but does not complete universal
automatic binary-update support.

A new release still requires reviewing its caller/data/structure layout and
updating the profile and code checks. If instruction shapes change, patterns
also require review. Pattern discovery alone is not ABI verification.

## Build and distribution

Use the R2/R3 toolchain instructions and always pass -NoDeploy. The R5 source
archive adds the matcher, resolver, profile, patterns and build audit. The
validation C++ sources are included; proprietary game captures and fixture
executables are excluded. R4 remains available as the previous candidate.
