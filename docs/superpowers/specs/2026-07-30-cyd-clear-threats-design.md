# CYD "Clear Threats" Control — Design

**Date:** 2026-07-30
**Status:** Approved (brainstorm); user delegated the build ("write the spec then build it"). Spec-review gate self-serviced.

## Goal

A CYD control that tells the whole fleet to wipe its persisted detection/threat table (RAM + NVS), so stale threats (e.g. the Flock self-detections that accumulate during flood testing — see `private/THREAT-NVS-STALE-FLOCK.md`) can be cleared in the field without a laptop / `esptool erase_flash`.

## Motivation

Confirmed threats persist in each decoy's NVS (`detect.c`, namespace `"splinter"`, key `detect_thr`) and survive firmware reflashing. There is a runtime wipe function — `detect_clear_threats()` (RAM memset + `nvs_erase_key` + commit) — but **no runtime trigger**. This feature adds a signed, fleet-wide trigger from the CYD.

## Non-goals (YAGNI)

- Fleet-wide only — no per-node clear.
- No per-decoy acknowledgement — the operator sees `threats` drop on the dashboard.
- No new wire message type and **no wire-version bump** — reuse the existing signed config path.
- No new persisted state.

## Architecture

### Command (reuse the signed config path)

The CYD already sends **Ed25519-signed, replay-protected** `RADAR_TYPE_CONFIG` commands (`config_cmd_t{version, preset_id}`, `config_wire_pack_signed`/`open_signed`), via `cyd_main` `send_config(uint8_t preset)`. Reserve a sentinel:

```c
#define CONFIG_CLEAR_THREATS 0xFF   // config_wire.h: preset_id sentinel = "wipe the threat table"
```

- **CYD** sends it with the existing function: `send_config(CONFIG_CLEAR_THREATS)`.
- **Decoy** (`esp_now_link.c` config handler) branches on it before applying a preset:

```c
if (cmd.version != CONFIG_WIRE_VER) return;
if (cmd.preset_id == CONFIG_CLEAR_THREATS) {
    detect_clear_threats();
    ESP_LOGW(ETAG, "config: CLEAR THREATS");
} else if (sim_settings_apply_preset((sim_preset_t)cmd.preset_id) == 0) {
    ESP_LOGW(ETAG, "config: applied preset %u", (unsigned)cmd.preset_id);
}
```

**Security:** the wipe rides the same signature + nonce-replay gate as a preset change — it cannot be spoofed or replayed. **Back-compat:** old firmware ignores `0xFF` (`sim_settings_apply_preset` returns -1, no-op), so a v-mismatched decoy simply doesn't clear (safe). Presets are 0–5 (flood build) / 0–4; `0xFF` is unused. `detect_clear_threats()` is declared in `detect.h` (currently only defined in `detect.c`).

### CYD UI — CONTROL page, 2-tap arm/confirm

The CONTROL page (preset cycler + LIVE/PENDING + SEND from sub-project D) gains a **CLEAR THREATS** button below SEND, with a REVOKE-style arm/confirm so an accidental tap can't wipe follower history:

- `radar_ctrl_info_t` gains `bool clear_armed`.
- `draw_control`: SEND button drawn at y≈205–239; a **CLEAR** button rect at y≈252–282. When `clear_armed`, it turns red (`COL_WARN`) and reads `CONFIRM CLEAR?`, else neutral `CLEAR THREATS`. The old "broadcast to all decoys" footer is dropped (the two buttons are self-explanatory).
- `cyd_main`: a `static uint32_t s_clear_arm_ms`. In the CONTROL touch handler:
  - Tap the **CLEAR band** (`ty >= 246`): if armed and within 3 s → `send_config(CONFIG_CLEAR_THREATS)` + `radar_ctrl_mark_sent` + disarm; else arm (`s_clear_arm_ms = now`).
  - Any other CONTROL action (SEND, prev/next preset, BACK) **disarms** (`s_clear_arm_ms = 0`).
  - `ctrl.clear_armed = (s_clear_arm_ms && (uint32_t)(now - s_clear_arm_ms) < 3000)`.

Touch-zone order in the CONTROL handler (highest-y first so CLEAR wins its band):
`ty<40` BACK (disarm) → `ty>=246` CLEAR arm/confirm → `ty>200 && tx 60–180` SEND (disarm) → `tx<80` prev (disarm) → `tx>160` next (disarm) → else stay.

## Error handling / edge cases

- **Un-armed accidental tap:** only arms (shows `CONFIRM CLEAR?`); no wipe. Auto-disarms after 3 s (the render window and next-tap logic both enforce it).
- **Leaving CONTROL / BACK:** disarms.
- **Old-firmware decoy:** ignores `0xFF` (no-op) — safe during a partial rollout.
- **`ctrl == NULL`:** `draw_control` treats `clear_armed` as false.
- **`detect_clear_threats` with nothing persisted:** `nvs_erase_key` returns NOT_FOUND, handled — harmless.
- **Non-CONFIG_CTRL build:** the whole CONTROL send path is already under `SIMULACRA_CONFIG_CTRL`; the CLEAR button/handler live under the same guard.

## Testing

- **Host — CONTROL render** (`render_dump --control` gains a 4th arg `clear_armed`): assert the `CLEAR THREATS` label renders; with `clear_armed=1` it reads `CONFIRM CLEAR?`. Existing `--control` tests keep passing (default `clear_armed=0`).
- **On-device — `churn_selftest`:**
  - Sign a `config_cmd_t{version, preset_id=CONFIG_CLEAR_THREATS}` and `config_wire_open_signed` it → assert `preset_id == 0xFF` (the wire carries the sentinel through the signed path).
  - Note a threat, call `detect_clear_threats()`, assert `detect_threat_count() == 0` (RAM wipe; `detect.c` NVS calls aren't host-compilable).
- **Compile-verify** c5/c6/cyd.
- **Rollout:** full-fleet reflash (decoy handler change). No wire-ver bump, so not version-fragile, but decoys need the new handler to actually clear.

## Files touched

- `components/simulacra_radar/config_wire.h` — `CONFIG_CLEAR_THREATS` sentinel.
- `main/detect.h` — declare `detect_clear_threats`.
- `main/esp_now_link.c` — config handler branch on the sentinel.
- `components/simulacra_radar/radar_render.h` — `radar_ctrl_info_t.clear_armed`.
- `components/simulacra_radar/radar_render.c` — `draw_control` CLEAR button + armed state.
- `cyd/main/cyd_main.c` — `s_clear_arm_ms`, CONTROL touch (arm/confirm + disarm), `ctrl.clear_armed`.
- `main/churn_selftest.c` — sentinel round-trip + `detect_clear_threats` checks.
- `tools/radar_audit/render_dump.c` — `--control` `clear_armed` arg.
- `tools/radar_audit/tests/test_control.py` — CLEAR label + armed assertions.

## Rollout

Full-fleet reflash (c5×2 COM12/COM16, c6 COM13, cyd COM10) from the flood branch, done with the operator present. No wire-version bump. After flashing, tapping CLEAR THREATS → CONFIRM on the CYD wipes every decoy's threat table (verify `threats` drops to 0 on the dashboard).
