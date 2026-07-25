# Adaptation-speed Option D — per-node population glide

**Date:** 2026-07-25
**Status:** design (approved, pre-plan)
**Area:** Wi-Fi decoy population sizing (`main/probe_agents.c`, `main/coexist.c`)

## Problem

The Wi-Fi decoy population tracks ambient device density (Law 4, population-match). Today a node
**snaps** its applied decoy count to the newly-computed target the instant the reprofile tick fires:

- `wifi_density.c` maintains an EWMA (α = 1/4) of observed density, clamped
  `WIFI_OBS_FLOOR..WIFI_OBS_CAP`.
- The coexist reprofile tick (C5 `reprofile_period_ms = 600000` / 10 min, C6 `300000` / 5 min)
  computes `target = fleet_pop_share_k(wifi_obs_target(), live_nodes)` and calls
  `probe_agents_set_target(target, now)`, which grows (spawns agents) or shrinks (dormants slots)
  in a single batch.

Two things step in unison as a result:

1. **Inter-node sync** — the census divisor `K = fleet_pop_live_size` is computed identically on
   every node from the same peer set, and co-located nodes observe the same ambient signal, so when
   a node joins/leaves or the density crosses a rounding boundary, every node's target changes at
   about the same time.
2. **Intra-node batch** — a single node applies its whole delta at one instant (a jump of several),
   not one device at a time.

Together this makes the **aggregate** fake-crowd size (what an observer actually sees — they cannot
attribute a fake to a specific node) change in clean synchronized **cliffs**. Real crowds never step
in unison; they change because many independent devices arrive and leave one at a time.

### Threat scoping (what this does and does NOT target)

The observer sees the aggregate `Σ (ambient / K) ≈ ambient`. That the crowd *tracks* ambient density
is **desired** behavior (Law 4) — a real crowd tracks it too — so "breathing with the environment" is
not the tell. This design targets **only signature (a): the synchronized, quantized step**. It does
**not** try to decorrelate the aggregate from ambient (signature (b)); that fights Law 4 and is out
of scope.

**Honest caveats:**
- The aggregate still tracks ambient density by design; only the synchronized-step artifact is removed.
- Whether that step is actually observable in the field is **unmeasured** — we have no capture of the
  tell. This is a low-urgency, defensible hardening, not a validated fix.
- It adds up to a few minutes of settling lag on top of the existing reprofile detection lag.
- Most relevant to a **stationary/following** observer watching over tens of minutes; a brief drive-by
  observer sees a snapshot and is largely indifferent to adaptation dynamics.

## Design — per-node population glide

Turn every target change into an independent per-node **staircase** of ±1 steps instead of a batch snap.

### Data flow

1. The reprofile tick computes `target` exactly as today, but records it as the **desired** target via
   a new `probe_agents_glide_set_target(target)` — it does **not** resize the population.
2. A **glide tick**, `probe_agents_glide_tick(now)`, is called from the existing Wi-Fi burst path
   (the burst branch already fires every `wifi_period_ms` = 2 s C5 / 7 s C6, so no new task or timer).
   On each call: if the node's jittered glide interval has elapsed **and** applied ≠ desired, move the
   applied count by **±1** toward the desired target (through the existing
   `probe_agents_set_target`), then draw a fresh independent interval and reschedule.
3. **Boot is instant:** the first desired target ever set applies immediately (applied = target, no
   ramp) so a fleet powering on does not visibly inflate over minutes. Only *subsequent* changes glide.

### Per-node desynchronization

The glide interval is drawn per step from `esp_random()` in `[GLIDE_MIN_MS, GLIDE_MAX_MS]`. Because
`esp_random` is seeded from hardware/RF entropy, each node's glide clock is genuinely independent —
even a fleet powered on together in one bag does not share a cadence. No node-id, MAC derivation, or
inter-node coordination is required.

### Tunable constants (Balanced profile)

- `GLIDE_STEP = 1` (agents per step — one device at a time, the device-faithful granularity).
- `GLIDE_MIN_MS = 30000`, `GLIDE_MAX_MS = 60000` (jittered ~30–60 s per step).

Rationale: target deltas are small (the count is clamped to a low range, then divided by K), so a
change closes in a couple of minutes — smooth without adding more than a few minutes of lag over the
5–10 min reprofile cadence. Gentler (~90 s) is stealthier but laggier; brisker (~15 s) barely smooths.
Constants are easy to retune.

### Solo (K = 1) behavior

The glide also smooths a single node's density-driven resizes into staircases, so the feature is **not
fleet-only** — it fixes intra-node batch stepping even standalone.

### Interaction with existing mechanisms

- **Census / clamp unchanged:** target is still `fleet_pop_share_k(wifi_obs_target(), live_nodes)`,
  clamped in `wifi_density`. Glide only smooths *application*; it never alters the computed target.
- **Manual presets:** a CONTROL preset that changes population still flows through the same target →
  glide path, so a manual change also glides rather than snapping. (Acceptable; no special-casing.)

## Components / boundaries

Fold the glide into `main/probe_agents.c` — it already owns the applied count (`s_n`) and
`probe_agents_set_target`, and is already host-tested by `tools/probe_audit`.

**New state (file-static in probe_agents.c):**
- `s_glide_target` — the desired applied count (init sentinel = "unset" so the first set is instant).
- `s_next_glide_ms` — when the next ±1 step is allowed.

**New public functions (`probe_agents.h`):**
- `void probe_agents_glide_set_target(int target, uint32_t now_ms);`
  Records the desired target. If no target has ever been set (boot), applies it immediately via
  `probe_agents_set_target` and seeds `s_next_glide_ms`.
- `void probe_agents_glide_tick(uint32_t now_ms);`
  If `now_ms >= s_next_glide_ms` and `probe_agents_count() != s_glide_target`, step applied by
  `GLIDE_STEP` toward the target (clamped so it never overshoots), realize it via
  `probe_agents_set_target`, and reschedule with a fresh jittered interval.

**Pure helper (host-testable without a clock/RNG):**
- `int probe_glide_next(int current, int target, int step);`
  Returns `current` moved toward `target` by at most `step` (no overshoot). Deterministic; the unit
  tests exercise the staircase logic through this, independent of timing/jitter.

**Coexist changes (`main/coexist.c`) — two call sites only:**
- Reprofile branch: `probe_agents_set_target(...)` → `probe_agents_glide_set_target(target, now)`.
- Wi-Fi burst branch: add `probe_agents_glide_tick(now)`.

## Testing

**Host (`tools/probe_audit`, extends the existing suite):**
- `probe_glide_next`: staircase up (+1 per call until target), staircase down, converges exactly,
  never overshoots, no-op when already at target, respects `step`.
- Glide integration via a `probe_dump` harness mode that feeds a target sequence + synthetic clock:
  - a target jump produces a ±1-per-interval staircase (not a batch jump);
  - the applied count converges to the target;
  - the **first** target set is applied instantly (boot-instant);
  - shrink direction works symmetrically;
  - the drawn interval always lands in `[GLIDE_MIN_MS, GLIDE_MAX_MS]`;
  - K = 1 still glides (no fleet required).

**Firmware:** compile-verify c5 (IDF 5.5) and c6 (IDF 5.4/5.5). No on-air validation is claimed by
this spec — hardware validation of the aggregate staircase would need a multi-node capture over time,
which we do not have; deferred and stated honestly.

## Out of scope

- Signature (b) decorrelation / adding noise to the target (fights Law 4).
- Any change to `wifi_density` sampling cadence or EWMA α.
- BLE/persona population glide (that population is set once at boot before the census exists — a
  separate future piece, same boundary as the census v1).
