# CYD Live Preset (wire v2) — Design

**Date:** 2026-07-29
**Status:** Approved (brainstorm); user delegated the remainder ("trust you to finish"). Spec-review gate self-serviced.
**Sub-project:** D of the CYD dashboard build-out — the one decoy-side piece. Order: A → B → C → **D**.
A (NODE console `cafda9f`), B (THREAT detail `dca9f22`), C (INFO console `a803386`) shipped, all CYD-render-only. D adds the single wire-protocol bump.

## Goal

Let each decoy report the **preset it is actually running**, so the CONTROL page shows live-vs-pending state (and detects a mixed fleet) instead of only echoing what you're about to SEND. Adds one byte to the status wire, bumps `RADAR_WIRE_VER`, and requires a **full fleet reflash**.

## Motivation

CONTROL cycles 5 presets (PAUSE/STEALTH/NORMAL/DENSE/MAX) and sends a signed config, but the decoy never reports back which preset is active. The CYD only knows what it last *sent* — not what a late-joining or rebooted decoy is actually running, nor whether the fleet is consistent. Reporting the running preset closes that loop: CONTROL can show the true fleet state, flag a MIXED fleet, and confirm a SEND took effect.

## Non-goals (YAGNI)

- Preset id only — **no** free-heap / RSSI-floor / other telemetry this bump (a later bump can add them if a real need appears).
- No new CONTROL interactions (same cycle + SEND).
- No dual-version parser — the `RADAR_WIRE_VER` bump cleanly rejects cross-version frames (see Back-compat).
- No persistence change on the decoy.

## Key design decision: infer, don't store

`sim_settings_apply_preset(p)` applies a preset but stores only the **resolved numeric settings** in `s_cur` (and NVS persists those numbers, not the preset id). A rebooted decoy therefore has the right settings but no memory of the preset. Rather than add persisted preset state, the decoy **infers** its current preset by resolving each preset and matching against `s_cur`:

- Pure helper `sim_preset_t sim_settings_match_preset(const sim_settings_t *cur, uint8_t ceiling)`: for `p` in `PAUSE..MAX`, `sim_settings_resolve(p, ceiling, &r)`; return the first `p` where `r` equals `*cur`; else `SIM_PRESET_COUNT` (= CUSTOM).
- `sim_preset_t sim_settings_current_preset(void)` = `sim_settings_match_preset(&s_cur, CHURN_ACTIVE_SET)`.

This is reboot-safe (reads live settings), needs no new NVS state, and naturally yields CUSTOM when the (default-off) web UI set granular values. Ambiguity note: on a very low `CHURN_ACTIVE_SET` ceiling, DENSE/MAX can clamp to the same settings as NORMAL; the match returns the first (lowest-id) preset that matches — an accepted, harmless tie-break.

## Architecture

### Decoy side

- `main/settings.c` / `settings.h`: add `sim_settings_match_preset` (pure) + `sim_settings_current_preset`.
- `main/esp_now_link.c` `respond_once()`: after `espnow_status_from_webui(&r,&w)`, set `r.preset = (uint8_t)sim_settings_current_preset();`.

### Wire

- `components/simulacra_radar/radar_wire.h`:
  - Append `uint8_t preset;` to `radar_wire_status_t` (after `battery_pct`).
  - Bump `RADAR_WIRE_VER` `1 → 2`.

**Back-compat:** `radar_wire_open` already rejects any frame whose header version byte ≠ `RADAR_WIRE_VER`. After the bump, a v2 CYD silently drops v1-decoy frames and vice-versa — a not-yet-reflashed board reads SILENT, no garbage, no size-mismatch. This is why the rollout must reflash **every** board (already the standing baked-fleet rule).

### Aggregate (CYD)

- `cyd/main/fleet_status.c` `fleet_status_aggregate`: compute `out->preset` across **alive** nodes —
  - no alive node → `0xFF` (none)
  - all alive nodes share a preset value → that value
  - alive nodes disagree → `0xFE` (MIXED)

  Values: `0..4` presets, `5` CUSTOM (`SIM_PRESET_COUNT`), `0xFE` MIXED, `0xFF` none.

### CONTROL render (CYD)

- `components/simulacra_radar/radar_render.h`: add `uint8_t live_preset;` to `radar_ctrl_info_t`.
- `cyd/main/cyd_main.c`: fill `ctrl.live_preset = agg.preset;` each frame.
- `radar_render.c` `draw_control`:
  - **LIVE** line (top, ~y=60): `LIVE  <name>` where name = `CTRL_LABELS[0..4]` / `CUSTOM` (5) / `MIXED` (0xFE, amber `COL_WARD`) / `—` (0xFF).
  - **PENDING** label (~y=100) above the existing preset cycler (`< [ NAME ] >`, `sel_preset`).
  - **Description** of the selected preset (~y=152, dim): `PRESET_DESC[5]` = `{"freeze on-air","min crowd","balanced","big crowd","max crowd"}`.
  - **SEND** button: reads `ACTIVE` (dim `COL_DIM`) when `live_preset == sel_preset` and `live_preset <= SIM_PRESET_MAX`; `SENT` during the flash; else `SEND`.
  - Footer `broadcast to all decoys` unchanged.

A name helper `ctrl_preset_name(uint8_t)` in `radar_render.c` maps 0–4/5/0xFE/0xFF → strings (reusing `CTRL_LABELS`).

## Error handling / edge cases

- **Mixed fleet** (rollout in progress, or a stuck node): LIVE shows `MIXED`; SEND stays `SEND` (never `ACTIVE`).
- **No decoys alive:** `agg.preset == 0xFF` → LIVE `—`.
- **CUSTOM** (web-UI granular, default off): LIVE `CUSTOM`; SEND `SEND`.
- **v1 decoy present** (pre-reflash): its frames are rejected at `open()` → it reads SILENT everywhere until reflashed. Expected during rollout.
- **`ctrl == NULL`** (non-control views): `draw_control` isn't called; unaffected.
- **Low-ceiling preset collapse:** match returns the lowest-id matching preset (documented tie-break).

## Testing

- **Host — CONTROL render** (`render_dump`): new `--control <sel_preset> <live_preset> <send_flash>` mode. Assert: `LIVE`, `PENDING` labels; the live name (`MAX`, `MIXED`, `CUSTOM`, `—`); the selected preset's description; `ACTIVE` when `sel==live` (both valid), `SEND` otherwise.
- **Host — aggregate preset** (`fleet_dump` / `test_fleet_status.py`): two alive nodes same preset → that value; differing → `0xFE`; none alive → `0xFF`.
- **On-device — inference** (`main/churn_selftest.c`): `sim_settings_apply_preset(X)` then `sim_settings_current_preset()==X` for a couple of presets; a granular `sim_settings_set` → `SIM_PRESET_COUNT` (CUSTOM). (settings.c isn't host-compilable — NVS/esp deps — so this lives in the existing on-device selftest, consistent with how the settings engine is already tested.)
- **Compile-verify** c5, c6, cyd (the wire struct + ver change rebuild everywhere).

## Files touched

- `components/simulacra_radar/radar_wire.h` — `preset` field + `RADAR_WIRE_VER` 2.
- `components/simulacra_radar/radar_render.h` — `radar_ctrl_info_t.live_preset`.
- `components/simulacra_radar/radar_render.c` — `draw_control` live/pending/description + `ctrl_preset_name`.
- `main/settings.c` / `settings.h` — `sim_settings_match_preset` + `sim_settings_current_preset`.
- `main/esp_now_link.c` — set `r.preset` in `respond_once`.
- `cyd/main/fleet_status.c` — aggregate `out->preset`.
- `cyd/main/cyd_main.c` — `ctrl.live_preset = agg.preset`.
- `main/churn_selftest.c` — inference selftest checks.
- `tools/radar_audit/render_dump.c` — `--control` mode + `ctrl.live_preset`.
- `tools/radar_audit/tests/test_control.py` — CONTROL render assertions.
- `tools/radar_audit/tests/test_fleet_status.py` — aggregate-preset assertions.

## Rollout

Wire v2 forces a **coordinated full-fleet reflash** — reflash **every** board (c5×2 COM12/COM16, c6 COM13, cyd COM10) in the same session, or the CYD (v2) shows un-reflashed decoys as SILENT. Sync the flood branch and reflash it too for DEFCON. **The reflash is a deliberate hardware step done with the operator present** (not during unattended work), because it is all-or-nothing and a partial flash blanks the dashboard.
