## Objective/Status Rebuild Ledger

This document is the SSOT for the 2026-03-12 top-left objective/status rebuild.

### Root Mistakes To Avoid

- Do not decide top-left objective keys with `pref=any`.
- Do not allow non-authority SLC paths to create owners or lines.
- Do not use `cfg trust` as line-generation authority.
- Do not emit both `objective` and `status` for the same live instance.
- Do not clamp top-left lane count to 3.
- Do not treat `old_src` as authority-generation code.

### Design Rules

- Top-left lines are created only from native visual samples plus native text events.
- Join is exact by native instance identity.
- Bootstrap may enrich keys, but it must not create lines.
- Save/status probes may enqueue transient status events, but they must not overwrite native objective ownership.
- Renderer consumes only owner-resolved top-left snapshot lines.

### Legacy Paths Marked For Removal/Bypass

- `STATUS-SLC-PRODUCER`
- `slc_status_producer_trusted_rp`
- `status_probe -> line`
- `objective lastline fallback`
- `live lane rescue`
- `cfg trust -> line reissue`
- `pref=any / english_map / slc_cache` objective line decisions
- `OBJ-OBS-ORDINAL` line creation or owner mutation

### old_src Usage Policy

- Allowed:
  - line ordering and non-reflow behavior
  - typewriter/alpha/drift parity
  - load/checkpoint/restart restore behavior
- Forbidden:
  - synthetic producer generation
  - cfg-trust owner creation
  - any-key bootstrap
  - status/objective cross-overwrite
