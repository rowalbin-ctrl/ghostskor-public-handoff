# GhostsKor Public Handoff

This folder has been organized so it can be turned into a public maintenance
repo without exposing game-install binaries and local junk by default.

## Project Provenance

This is an AI-assisted community handoff.

- The organizer is not a professional programmer.
- Most implementation, debugging, and documentation work in this project was
  done through AI-assisted iteration plus in-game testing.
- The goal of publishing this repo is maintainability and preservation, so
  other people can continue the patch even if the original organizer cannot.

Please treat the codebase as a practical maintenance handoff, not as a polished
reference implementation.

## What This Repo Should Contain

- `src/` source code
- vendored source dependencies under `src/vendor/`
- build scripts and project notes
- generated font atlas resources already embedded by `src/resources.rc`

## What Should Not Be Published

- prebuilt DLLs from the game folder or release folder
- local build outputs under `src/build/`
- build logs / runtime logs
- raw TTF font files unless you later verify redistribution rights

## Build

Open PowerShell in `src` and run:

```powershell
.\build.ps1 -NoDeploy
```

`-NoDeploy` is recommended for maintainers first. The default script deploys to
the game directory on the machine where it is run.

## Font Assets

The build currently expects these generated resources to exist in `src/`:

- `font_atlas.png`
- `font_atlas2.png`
- `font_metrics.bin`
- `font_metrics2.bin`

These are embedded by `src/resources.rc`. The raw TTF files are intentionally
not part of the public handoff by default.

## Public Upload

If you want a clean folder to upload, run:

```powershell
.\export_public_handoff.ps1
```

That creates a separate `KOR_PATCH_public_handoff` folder next to this one with
only the files intended for public maintenance.
