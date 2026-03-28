# Maintainer Handoff

This project is a source-based Korean patch for Call of Duty: Ghosts.

## Provenance Note

This repository is being handed off by a non-programmer project organizer.
Much of the coding and technical iteration was done with AI assistance, then
validated by direct gameplay testing and bug reports.

That means:

- some naming and internal architecture may be uneven
- comments and SSOT documents matter a lot
- maintainers should prefer verification in-game over assuming the current
  heuristics are universally correct

## Included In The Public Handoff

- C++ source under `src/`
- build scripts
- generated atlas/metrics resources required by `src/resources.rc`
- vendored MinHook source and license
- project design notes / SSOT markdown files

## Intentionally Excluded From The Public Handoff

- root release binaries such as `dxgi.dll`, `dxgi.next.dll`, `d3d11.dll`,
  `dinput8.dll`
- `src/build/` artifacts
- build logs
- raw TTF font files

## Build Notes

- `src/build.ps1` builds `build/dxgi.dll`
- use `.\build.ps1 -NoDeploy` for a safe local compile
- without `-NoDeploy`, the script copies the DLL into the game directory

## Important Caveat

The public handoff keeps generated atlas resources in `src/` because the
current build embeds them directly from `src/resources.rc`. If future
maintainers want to regenerate or replace those assets, they should do so using
fonts that they are comfortable redistributing.

## Recommended First Steps For A New Maintainer

1. Read the SSOT markdown files in the repo root.
2. Read `src/build.ps1`.
3. Build once with `.\build.ps1 -NoDeploy`.
4. If they need to test in-game, only then use the normal deploy path.
