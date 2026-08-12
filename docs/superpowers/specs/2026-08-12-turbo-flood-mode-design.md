# TURBO flood mode

**Date:** 2026-08-12
**Status:** design (approved, pre-plan)
**Area:** `main/settings.{c,h}`, `main/coexist.c`, `main/ble_devices.{c,h}`, `main/probe_agents.{c,h}`,
`main/roster.{c,h}`, CYD CONTROL page (`cyd/main/`, `components/simulacra_radar/`), `main/churn_selftest.c`.

## Goal

A sixth preset, shipped in main and in the public web-flasher build alongside PAUSE/STEALTH/NORMAL/
DENSE/MAX: **TURBO**. Every board in the fleet independently maxes its own BLE and Wi-Fi hardware —
churning identities as fast as the radios can sustain — rather than blending into an estimated room
population.

The premise (from a DEFCON conversation, not a guess): realism/population-matching optimizes for
*not being flagged*. TURBO optimizes for a different, complementary property — cost of processing.
Every device a scanner sees, real or synthetic, still has to be captured, deduped, and either
classified or discarded. A wall of syntactically valid, Law-3-safe, non-repeating devices imposes
that cost regardless of how "realistic" any single one looks. This is a field-use mode, not a demo:
you trigger it when you want maximum RF noise at a location, not a spectacle for scanners to admire.

## Constraints & honest notes

- **Shipped, unlike Flock-flood.** Flock-flood mimics a specific real vendor's BLE signature and was
  kept personal-branch-only because of that impersonation angle (see
  `2026-07-25-flock-flood-mode-design.md`). TURBO doesn't mimic anyone — it's a louder DENSE, not an
  impersonation of a real product — so it ships in main and the public flasher image like any other
  preset.
- **Law 3 is untouched.** Non-negotiable, bystander-safety rail (no Apple Continuity / Find My /
  Fast Pair / Swift Pair subtypes — those trigger real pairing prompts on real phones), orthogonal to
  "realism." Every TURBO-generated identity is gated exactly like every other mode's.
- **Detection stays enabled.** TURBO changes generation/presentation only. No reason to blind the
  fleet to a real tracker while it's flooding.
- **Not a DoS.** Non-connectable legacy-PDU BLE advertising and standard 802.11 probe requests, both
  within spec, no deauth, no jamming, no malformed frames aimed at breaking a parser. A handful of
  ESP32 boards at legal power cannot meaningfully deny service to nearby real APs/clients — the
  actual effect is cluttering scan results and burning analysis time, which is the intended design
  goal, not an unintended side effect.
- **Hardware ceiling, stated honestly.** `CHURN_HW_INSTANCES = 4` concurrent BLE adv slots transmit
  at once; the flood is produced by churning identities fast through those 4 slots, not N
  simultaneous radios. The *sustainable* churn rate — where it stops being "fast" and starts being
  "HCI command floods / TX-queue errors" — is a hardware-tuning question this design does not
  pre-answer (see Open question below).
- **Fleet-key dependency.** Same as every CONTROL preset: only reaches decoys enrolled to the
  broadcasting Vigil.

## Architecture

TURBO is an **orthogonal override**, not a preset variant threaded through the population-match
machinery built earlier this session (`sim_settings_floor`/`ceiling`, live fleet-share). That
machinery's entire job is fitting this node's crowd to an estimated room density divided across K
live peers. TURBO is the opposite regime: no room model, no K-division, every node independently at
its own hardware ceiling. Building an "ignore floor/ceiling" escape hatch inside the file whose job
is enforcing them would tangle two things that don't share a purpose.

So: a new boolean, `s_turbo`, owned by `coexist.c` (mirroring how `s_preset_req` already owns the
control-command inbox added earlier this session). `coexist_task` checks it **before** the normal
`d.fire_reprofile` / glide path each tick:

```c
if (s_turbo) {
    ble_devices_set_count(BLE_DEVICES_MAX, now);
    probe_agents_set_target(PROBE_AGENTS_MAX, now);
    // skip reprofile_start/finish and probe_agents_glide_* entirely this tick
} else {
    // existing reprofile/glide path, unchanged
}
```

No persona coupling: TURBO does not call `phantom_set_count`. Today's cross-protocol persona work
(BLE+Wi-Fi paired as one synthetic device) exists to defeat single-radio-ghost filtering — an
indistinguishability mechanism. TURBO isn't trying to be indistinguishable, so it maxes both radios
independently and skips the persona invariant rather than fighting it.

### Activation path (CYD → decoy)

Reuses the signed CONFIG preset infra hardened earlier this session — no new wire type, no new
attack surface:

- `SIM_PRESET_TURBO` added to `sim_preset_t` (`settings.h`), after `SIM_PRESET_MAX`.
- `sim_settings_resolve(SIM_PRESET_TURBO, …)` sets a `bool turbo` field on `sim_settings_t`; every
  other preset resolves it `false`.
- `sim_settings_apply` calls `coexist_set_turbo(s->turbo)` (new, mirrors `churn_set_paused`), which
  flips `s_turbo` for the next tick to act on.
- CYD CONTROL page: `RADAR_CTRL_PRESET_COUNT` 5 → 6, `CTRL_LABELS[6]` gains `"TURBO"` — the same
  6th-preset slot the flood branch already flagged as a build-aware merge point; resolved here by
  keeping everything keyed off the count constant, nothing hardcoded to 5.
- **Two-tap arm/confirm**, reusing the exact pattern already built for CLEAR THREATS (`s_clear_arm_ms`,
  3 s window): first tap arms, a second tap inside the window fires `send_config(SIM_PRESET_TURBO)`,
  anything else disarms. One stray tap on a public-build unit should not max out its radios.

### BLE behavior (turbo active)

- `ble_devices_set_count(BLE_DEVICES_MAX, now)` — every slot filled, independent of any fleet-share
  or room-density estimate.
- Identity draw bypasses `generate_roster`'s model-driven vendor sampling entirely (same *shape* of
  bypass as Flock-flood's `generate_set_flock_flood`, different target): `dev_spawn` draws from
  `roster_at()` — the existing template pool — rather than fitting to observed data. This is where
  most of the per-identity CPU cost lives in normal mode; TURBO doesn't pay it, and the pool still
  yields varied, Law-3-safe payload shapes rather than one repeated shape (unlike Flock-flood, which
  deliberately converges on one vendor's signature).
- Rotation interval: a new, much shorter TURBO-specific band (not `RPA_ROT_MIN_MS`/`MAX_MS`, not
  `PERSONA_RPA_ROT_MIN_MS`/`MAX_MS`). Exact values are an on-hardware tuning question — see Open
  question.

### Wi-Fi behavior (turbo active)

- `probe_agents_set_target(PROBE_AGENTS_MAX, now)`.
- Every agent forced `DUTY_ACTIVE` (normally ~33% via `esp_random() % 3u == 0u`) with interval
  floored to the minimum of the existing `ACTIVE_MIN_MS`/`MAX_MS` band, or a new tighter TURBO band
  if hardware testing shows headroom.
- Both bands where the board supports them (C5 has 5 GHz). Unlike Flock-flood, which silences Wi-Fi
  entirely to keep the picture "pure camera," TURBO fires both radios — "all signals," per the
  request.

### Identifier uniqueness — no new engineering

Random 48-bit MACs (46 bits of entropy after the two fixed address-type bits) have a birthday-bound
50%-collision point around ~9.4 million draws. No churn rate this hardware can sustain gets a field
session anywhere near that in realistic use. The existing `uniq_id` ring (2048 entries, `main/uniq_id.c`)
already prevents *adjacent* reuse on top of that. "Totally unique, don't repeat" is satisfied by the
existing randomness; no larger tracking structure is being built for this mode.

### Exit path

Tapping any other preset resolves `turbo=false`, `coexist_set_turbo(false)` clears `s_turbo`, and the
very next tick falls through to the normal reprofile/glide path — same honest-default behavior as
every existing preset transition. No lingering flood state.

## Components (each independently testable)

| Unit | Responsibility |
|------|----------------|
| `settings.{c,h}` | `SIM_PRESET_TURBO`, `turbo` field, resolve → true/false per preset |
| `coexist.{c,h}` | `s_turbo` state, `coexist_set_turbo(bool)`, tick-time override ahead of reprofile/glide |
| `roster.{c,h}` / `ble_devices.c` | `dev_spawn` bypass path drawing straight from the template pool when turbo is active |
| `probe_agents.c` | forced `DUTY_ACTIVE` + floored interval when turbo is active |
| CYD CONTROL | `TURBO` button, two-tap arm/confirm, `RADAR_CTRL_PRESET_COUNT`/`CTRL_LABELS` bump to 6 |

## Testing

- **Host (`decoy_audit`):** with turbo active, `ble_devices_count()` == `BLE_DEVICES_MAX` and
  `probe_agents_count()` == `PROBE_AGENTS_MAX` regardless of fleet-share/floor/ceiling inputs; every
  generated identity still passes `law3_forbidden() == false` — regression-critical, since this is
  the mode most likely to accidentally emit something unsafe if the generation bypass is wired wrong.
- **Host (`settings` resolve):** `sim_settings_resolve(SIM_PRESET_TURBO, …)` → `turbo=true`; every
  other preset → `turbo=false`.
- **Host (`radar_audit`):** CONTROL page renders 6 preset buttons including TURBO; the two-tap
  arm/confirm state machine behaves like CLEAR THREATS (armed → 3 s timeout → disarmed; armed →
  second tap → fires).
- **On-target self-test:** a TURBO assertion block alongside the existing preset checks — population
  ceilings hit, Law 3 still passes, `coexist_set_turbo(false)` actually clears the override.
- **On-hardware tuning pass (cannot be done in host tests):** find the actual sustainable BLE
  rotation interval and Wi-Fi injection cadence before HCI command floods / TX-queue errors appear —
  the same category of empirical limit the original 5 GHz excursion code already hit
  (`ESP_ERR_NO_MEM`/257 injecting all 8 channels back-to-back). This tuning pass produces the real
  TURBO interval constants; this spec deliberately does not guess them.

## Open question (resolve during implementation, not here)

**Exact TURBO rotation/injection intervals.** This spec establishes the mechanism (bypass
population-match, max both radios, template-pool draw) but not the numbers — they can only be found
by flashing a build with aggressive placeholder constants and watching for controller errors on real
hardware, the same way the existing 5 GHz excursion pacing was found. The implementation plan should
budget an explicit hardware-tuning task for this rather than shipping a guessed constant.

## Out of scope

- Any change to the normal blend mode, population-match logic, personas, or the detector.
- An auto-revert timer — TURBO is manual-only, sticky until changed, matching every other preset.
- Payload "richness" as a deliberate analysis-cost lever (e.g., deliberately parse-expensive
  structures) — noted as a possible future direction in conversation, not built here. v1 maximizes
  volume, which is the literal request; per-device processing cost is a separate lever for later.
