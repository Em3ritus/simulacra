# TURBO Flood Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a sixth preset, TURBO, that makes every board in the fleet independently flood its own BLE and Wi-Fi hardware at maximum churn — ignoring room-density population-match — activated fleet-wide from the CYD CONTROL page and shipped in main and the public web-flasher build.

**Architecture:** TURBO is an orthogonal override living at the `coexist.c` tick level, not threaded through the existing floor/ceiling population-match machinery in `settings.c`. Activation still flows through the existing signed-CONFIG preset pipeline (`SIM_PRESET_TURBO` → `sim_settings_apply` → `coexist_set_turbo`). Two lower-level modules (`ble_devices.c`, `probe_agents.c`) each grow their own `_set_turbo(bool)` toggle that shortens their respective churn intervals; `coexist.c` forces both radios to their hardware ceiling and skips reprofile/glide while turbo is active.

**Tech Stack:** ESP-IDF (NimBLE + esp_wifi), C, host-testable pure cores under `tools/decoy_audit` (BLE) and `tools/probe_audit` (Wi-Fi) and `tools/radar_audit` (CYD render), on-target self-test (`main/churn_selftest.c`, `CHURN_SELFTEST=1` build).

## Global Constraints

- **Law 3 stays enforced, no exceptions.** TURBO never changes what payload content is generated (still drawn from the existing roster/template pool via `roster_at()`), only how often devices respawn — the Law-3 gate at generation time is untouched by this plan.
- **Detection stays enabled** while turbo is active. TURBO only changes generation/presentation.
- **No persona coupling.** TURBO does not bind BLE+Wi-Fi personas; any currently-bound personas are released the moment turbo activates.
- **Manual only, no auto-revert.** TURBO is sticky until the operator selects a different preset, same as every other preset.
- **Two-tap arm/confirm** on the CYD CONTROL SEND button when the pending preset is TURBO, reusing the exact `s_clear_arm_ms` pattern already built for CLEAR THREATS (arm on first tap, fire on a second tap within 3 s, disarm on anything else).
- **Ships in main and the public web-flasher build**, not a personal/flag-gated branch — unlike Flock-flood, TURBO does not impersonate a real vendor.
- **Exact TURBO churn/probe interval constants are provisional.** Every new interval constant introduced in this plan is explicitly commented as a starting value pending the on-hardware tuning pass in Task 8 — do not treat any of them as final without running that task.

---

### Task 1: `SIM_PRESET_TURBO` — settings enum, field, resolve, and display matching

**Files:**
- Modify: `main/settings.h`
- Modify: `main/settings.c`

**Interfaces:**
- Produces: `SIM_PRESET_TURBO` (enum value `5`, between `SIM_PRESET_MAX` and `SIM_PRESET_COUNT`), `sim_settings_t.turbo` (`bool`), `sim_settings_resolve()`/`sim_settings_match_preset()` updated signatures (same signatures as today, new behavior).
- Consumes: nothing new — this task only touches the pure preset-resolution core, no coexist/churn wiring yet.

- [ ] **Step 1: Add the enum value and struct field**

In `main/settings.h`, change:

```c
typedef enum {
    SIM_PRESET_PAUSE = 0, SIM_PRESET_STEALTH, SIM_PRESET_NORMAL,
    SIM_PRESET_DENSE, SIM_PRESET_MAX, SIM_PRESET_COUNT
} sim_preset_t;
```

to:

```c
typedef enum {
    SIM_PRESET_PAUSE = 0, SIM_PRESET_STEALTH, SIM_PRESET_NORMAL,
    SIM_PRESET_DENSE, SIM_PRESET_MAX, SIM_PRESET_TURBO, SIM_PRESET_COUNT
} sim_preset_t;
```

And change:

```c
typedef struct {
    uint8_t  active_target;                       // concurrent phantom crowd size
    bool     paused;                              // freeze rotation (phantoms stay on-air)
    float    accel;                               // lifetime divisor: >1.0 = faster arrivals/departures
} sim_settings_t;
```

to:

```c
typedef struct {
    uint8_t  active_target;                       // concurrent phantom crowd size
    bool     paused;                              // freeze rotation (phantoms stay on-air)
    float    accel;                               // lifetime divisor: >1.0 = faster arrivals/departures
    bool     turbo;                                // TURBO active: coexist_set_turbo owns the REAL
                                                   // population/churn rate, bypassing floor/ceiling
} sim_settings_t;
```

- [ ] **Step 2: Resolve TURBO in `sim_settings_resolve`**

In `main/settings.c`, change:

```c
int sim_settings_resolve(sim_preset_t p, uint8_t floor, uint8_t ceiling, sim_settings_t *out)
{
    if (p >= SIM_PRESET_COUNT) return -1;
    uint8_t stealth = (uint8_t)((ceiling * 4) / 10);   // ~40% of ceiling (raised to floor below)
    sim_settings_t s = { .active_target = ceiling, .paused = false, .accel = 1.0f };
    switch (p) {
    case SIM_PRESET_PAUSE:                                  // NORMAL values, rotation frozen
        s.paused = true; break;
    case SIM_PRESET_STEALTH:
        s.active_target = stealth; break;                   // smaller crowd, unhurried turnover
    case SIM_PRESET_NORMAL:
        break;                                              // firmware defaults
    case SIM_PRESET_DENSE:
        s.accel = 1.5f; break;                              // full crowd, 1.5x turnover
    case SIM_PRESET_MAX:
        s.accel = 2.5f; break;                              // full crowd, 2.5x turnover
    default: return -1;
    }
    sim_settings_clamp(&s, floor, ceiling);
    *out = s;
    return 0;
}
```

to:

```c
int sim_settings_resolve(sim_preset_t p, uint8_t floor, uint8_t ceiling, sim_settings_t *out)
{
    if (p >= SIM_PRESET_COUNT) return -1;
    uint8_t stealth = (uint8_t)((ceiling * 4) / 10);   // ~40% of ceiling (raised to floor below)
    sim_settings_t s = { .active_target = ceiling, .paused = false, .accel = 1.0f, .turbo = false };
    switch (p) {
    case SIM_PRESET_PAUSE:                                  // NORMAL values, rotation frozen
        s.paused = true; break;
    case SIM_PRESET_STEALTH:
        s.active_target = stealth; break;                   // smaller crowd, unhurried turnover
    case SIM_PRESET_NORMAL:
        break;                                              // firmware defaults
    case SIM_PRESET_DENSE:
        s.accel = 1.5f; break;                              // full crowd, 1.5x turnover
    case SIM_PRESET_MAX:
        s.accel = 2.5f; break;                              // full crowd, 2.5x turnover
    case SIM_PRESET_TURBO:
        // active_target/accel below are irrelevant once turbo=true: sim_settings_match_preset
        // (below) short-circuits on the turbo flag alone. The REAL population/churn rate is forced
        // directly by coexist_set_turbo (added in Task 5), bypassing the fleet-share floor/ceiling
        // entirely -- every board floods at its own hardware max, not a room-density estimate
        // divided across K nodes. That bypass is the whole point of the mode.
        s.turbo = true; break;
    default: return -1;
    }
    sim_settings_clamp(&s, floor, ceiling);
    *out = s;
    return 0;
}
```

- [ ] **Step 3: Make `sim_settings_match_preset` identify TURBO by flag, not by active_target**

In `main/settings.c`, change:

```c
sim_preset_t sim_settings_match_preset(const sim_settings_t *cur, uint8_t floor, uint8_t ceiling)
{
    for (sim_preset_t p = SIM_PRESET_PAUSE; p < SIM_PRESET_COUNT; p++) {
        sim_settings_t r;
        if (sim_settings_resolve(p, floor, ceiling, &r) != 0) continue;
        if (r.active_target == cur->active_target && r.paused == cur->paused &&
            r.accel == cur->accel)
            return p;
    }
    return SIM_PRESET_COUNT;   // CUSTOM
}
```

to:

```c
sim_preset_t sim_settings_match_preset(const sim_settings_t *cur, uint8_t floor, uint8_t ceiling)
{
    for (sim_preset_t p = SIM_PRESET_PAUSE; p < SIM_PRESET_COUNT; p++) {
        sim_settings_t r;
        if (sim_settings_resolve(p, floor, ceiling, &r) != 0) continue;
        // Turbo is identified by the flag alone. While turbo is running, coexist_set_turbo forces
        // the real population directly (bypassing floor/ceiling), so cur->active_target is NOT the
        // fleet-shared value resolve() computed above -- requiring it to also match would always
        // report CUSTOM instead of TURBO while the mode is genuinely active.
        if (r.turbo != cur->turbo) continue;
        if (cur->turbo) return p;
        if (r.active_target == cur->active_target && r.paused == cur->paused &&
            r.accel == cur->accel)
            return p;
    }
    return SIM_PRESET_COUNT;   // CUSTOM
}
```

- [ ] **Step 4: Build to confirm it compiles**

This task touches only pure C with no new external dependency, so a normal firmware build proves it compiles. Run (from the repo root, PowerShell):

```powershell
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Fleet -Do build
```

Expected: `BUILD: Project build complete.`

- [ ] **Step 5: Commit**

```bash
git add main/settings.h main/settings.c
git commit -m "feat(turbo): SIM_PRESET_TURBO enum, turbo field, resolve + display matching

Preset resolves to a bare turbo=true flag; sim_settings_match_preset
identifies it by that flag alone rather than active_target, since the
coexist-level override (added in a later task) bypasses the fleet-share
floor/ceiling entirely and would never produce a byte-matching
active_target for the display to compare against."
```

---

### Task 2: `churn_set_slice_ms` — runtime-adjustable BLE presentation cadence

**Files:**
- Modify: `main/churn.h`
- Modify: `main/churn.c`

**Interfaces:**
- Produces: `void churn_set_slice_ms(uint32_t ms);` — overrides how often `churn_tick` re-evaluates which devices occupy the 4 physical BLE adv slots (normally `CHURN_SLICE_MS` = 1000 ms). Floored at 50 ms to prevent a runaway loop.
- Consumes: nothing new.

**Why this exists:** `CHURN_HW_INSTANCES` (4) concurrent BLE adv slots only get re-evaluated once per `CHURN_SLICE_MS`, cycling round-robin through the population. This — not per-device `life_ms` — is the actual bottleneck on how fast new identities reach the air. Shortening device lifetimes alone (Task 3) without also shortening this cadence wastes CPU respawning identities that never get a turn on a physical slot before they respawn again.

- [ ] **Step 1: Add the runtime override**

In `main/churn.h`, change:

```c
void   churn_set_accel(float mult);
float  churn_accel(void);
void   churn_init(uint32_t now_ms);
```

to:

```c
void   churn_set_accel(float mult);
float  churn_accel(void);
// Override how often the 4 HW adv slots are re-evaluated (normally CHURN_SLICE_MS = 1000 ms). This,
// not per-device life_ms, is the real bottleneck on how fast NEW identities reach the air: shorter
// device lifetimes are wasted if the slot presenting them isn't revisited fast enough to show them.
// Floored at 50 ms. TURBO mode is the only caller that changes this away from the default.
void   churn_set_slice_ms(uint32_t ms);
void   churn_init(uint32_t now_ms);
```

- [ ] **Step 2: Implement it and use it in the tick**

In `main/churn.c`, change:

```c
static uint32_t s_apply_gen;                         // bumped whenever the on-air set changes

void    churn_set_apply(churn_apply_fn fn) { s_apply = fn; }
void    churn_set_paused(bool paused) { s_paused = paused; }
bool    churn_paused(void) { return s_paused; }
uint32_t churn_apply_gen(void) { return s_apply_gen; }
```

to:

```c
static uint32_t s_apply_gen;                         // bumped whenever the on-air set changes
static uint32_t s_slice_ms = CHURN_SLICE_MS;          // presentation cadence; churn_set_slice_ms overrides

void    churn_set_apply(churn_apply_fn fn) { s_apply = fn; }
void    churn_set_paused(bool paused) { s_paused = paused; }
bool    churn_paused(void) { return s_paused; }
uint32_t churn_apply_gen(void) { return s_apply_gen; }
void    churn_set_slice_ms(uint32_t ms) { s_slice_ms = ms < 50u ? 50u : ms; }
```

Then change the slice check inside `churn_tick`:

```c
    if (now_ms - s_last_slice_ms < CHURN_SLICE_MS) return;
```

to:

```c
    if (now_ms - s_last_slice_ms < s_slice_ms) return;
```

- [ ] **Step 3: Build to confirm it compiles**

```powershell
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Fleet -Do build
```

Expected: `BUILD: Project build complete.`

- [ ] **Step 4: Commit**

```bash
git add main/churn.h main/churn.c
git commit -m "feat(turbo): churn_set_slice_ms -- runtime override for the BLE presentation cadence

The 4 physical adv slots only re-evaluate the population once per
CHURN_SLICE_MS (1s), round-robin. This is the actual bottleneck on how
fast new identities reach the air, not per-device life_ms -- shortening
lifetimes without also shortening this wastes CPU respawning devices
that never get a turn on a slot. No behavior change yet; TURBO mode
(a later task) is the only caller that will use a non-default value."
```

---

### Task 3: BLE turbo churn — `ble_devices_set_turbo`

**Files:**
- Modify: `main/ble_devices.h`
- Modify: `main/ble_devices.c`
- Modify: `tools/decoy_audit/synth_dump.c`
- Create: `tools/decoy_audit/tests/test_turbo_ble.py`

**Interfaces:**
- Produces: `void ble_devices_set_turbo(bool on);` — when on, every freshly spawned device (initial spawn, growth, or respawn-on-expiry) gets a short fixed lifetime instead of the normal role/atype-based bands, overriding `accel` entirely.
- Consumes: nothing new — pure addition to the existing `dev_spawn` path.

- [ ] **Step 1: Write the failing host test**

Create `tools/decoy_audit/tests/test_turbo_ble.py`:

```python
import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def devices(seed=1, n=8, ticks=20, tick_ms=1000, turbo=False):
    args = [EXE, "--devices", str(seed), str(n), str(ticks), str(tick_ms)]
    if turbo:
        args.append("turbo")
    out = subprocess.check_output(args, text=True)
    return [ln.split() for ln in out.splitlines() if ln.startswith("D ")]
    # columns: [0]="D" [1]=t [2]=slot [3]=addr_hex [4]=atype [5]=role [6]=event [7]=company [8]=itvl


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class TurboBleChurn(unittest.TestCase):
    def test_turbo_respawns_far_more_than_normal(self):
        # 20 x 1s ticks is far shorter than any normal lifetime band (shortest is the 2 min
        # transient minimum), so normal mode should show only the initial births in this window.
        normal = devices(seed=3, n=8, ticks=20, tick_ms=1000, turbo=False)
        turbo = devices(seed=3, n=8, ticks=20, tick_ms=1000, turbo=True)
        normal_born = [r for r in normal if r[6] == "born"]
        turbo_born = [r for r in turbo if r[6] == "born"]
        self.assertGreater(len(turbo_born), len(normal_born),
                           f"turbo should respawn far more: normal={len(normal_born)} turbo={len(turbo_born)}")

    def test_turbo_life_is_short(self):
        # Every "born" event past t=0 on a given slot is a respawn. Gaps between successive births
        # on the same slot must sit inside the turbo band (2-5s) with slack for tick granularity.
        rows = devices(seed=5, n=4, ticks=15, tick_ms=1000, turbo=True)
        by_slot = {}
        for r in rows:
            if r[6] != "born":
                continue
            slot = int(r[2]); t = int(r[1])
            by_slot.setdefault(slot, []).append(t)
        gaps = [b - a for times in by_slot.values() for a, b in zip(times, times[1:])]
        self.assertTrue(gaps, "no respawns observed in turbo mode")
        for g in gaps:
            self.assertLessEqual(g, 6000, f"turbo respawn gap too slow: {g} ms")

    def test_turbo_still_varies_atype_and_company(self):
        # Turbo only shortens life_ms; the atype/company/payload draw is untouched, so respawns
        # must still show more than one distinct atype and more than one distinct company over a
        # long-enough run -- a wiring bug that collapsed everything to one shape would fail this.
        rows = devices(seed=7, n=8, ticks=30, tick_ms=1000, turbo=True)
        atypes = {r[4] for r in rows if r[6] == "born"}
        companies = {r[7] for r in rows if r[6] == "born"}
        self.assertGreater(len(atypes), 1, f"turbo collapsed to one atype: {atypes}")
        self.assertGreater(len(companies), 1, f"turbo collapsed to one company: {companies}")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Add the `turbo` flag to `synth_dump`'s `--devices` mode**

In `tools/decoy_audit/synth_dump.c`, find the `--devices` block:

```c
    if (argc > 1 && strcmp(argv[1], "--devices") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      ndev   = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 16;
        int      ticks  = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 4000;
        unsigned tickms = argc > 5 ? (unsigned)strtoul(argv[5], 0, 10) : 1000;
        srand(seed);
        roster_init();                                  // build the behaviour library (host: template fallback)
        uint32_t t = 0;
        ble_devices_init(ndev, t);
```

Change it to:

```c
    if (argc > 1 && strcmp(argv[1], "--devices") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      ndev   = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 16;
        int      ticks  = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 4000;
        unsigned tickms = argc > 5 ? (unsigned)strtoul(argv[5], 0, 10) : 1000;
        int      turbo  = argc > 6 && strcmp(argv[6], "turbo") == 0;
        srand(seed);
        roster_init();                                  // build the behaviour library (host: template fallback)
        ble_devices_set_turbo(turbo != 0);
        uint32_t t = 0;
        ble_devices_init(ndev, t);
```

(Everything after `ble_devices_init(ndev, t);` in this block is unchanged.)

- [ ] **Step 3: Rebuild and confirm the test fails (function doesn't exist yet)**

```powershell
& "$env:USERPROFILE\Downloads\simulacra\tools\decoy_audit\run.ps1" -Rebuild
```

Expected: a C compile error — `ble_devices_set_turbo` undeclared.

- [ ] **Step 4: Implement `ble_devices_set_turbo` in `ble_devices.h`**

In `main/ble_devices.h`, change:

```c
// Churn acceleration: lifetimes are divided by `mult` (clamped to [1,8]). Applies to devices born
// later AND rescales the remaining life of live unbound devices, so a change takes effect now.
// Idempotent — safe to call every tick with a slowly-decaying value.
void  ble_devices_set_accel(float mult, uint32_t now_ms);
float ble_devices_accel(void);
```

to:

```c
// Churn acceleration: lifetimes are divided by `mult` (clamped to [1,8]). Applies to devices born
// later AND rescales the remaining life of live unbound devices, so a change takes effect now.
// Idempotent — safe to call every tick with a slowly-decaying value.
void  ble_devices_set_accel(float mult, uint32_t now_ms);
float ble_devices_accel(void);
// TURBO mode: every freshly spawned device (init/grow/respawn-on-expiry) gets a short fixed
// lifetime instead of the normal role/atype-based bands, overriding accel entirely. Only life_ms
// changes -- atype/role/payload are still drawn normally, so identity diversity is unaffected.
// Idempotent; off by default.
void  ble_devices_set_turbo(bool on);
```

- [ ] **Step 5: Implement it in `ble_devices.c`**

In `main/ble_devices.c`, change:

```c
#define ACCEL_MIN 1.0f
#define ACCEL_MAX 8.0f
```

to:

```c
#define ACCEL_MIN 1.0f
#define ACCEL_MAX 8.0f
// TURBO respawn band -- placeholder pending the on-hardware tuning pass
// (docs/superpowers/specs/2026-08-12-turbo-flood-mode-design.md, Open question).
#define TURBO_LIFE_MIN_MS 2000u   // 2 s
#define TURBO_LIFE_MAX_MS 5000u   // 5 s

static bool s_turbo = false;
void ble_devices_set_turbo(bool on) { s_turbo = on; }
```

Then in `dev_spawn`, change:

```c
    if (s_accel > 1.0f) {                           // accelerated churn: shorter lives, same shape
        uint32_t l = (uint32_t)((float)d->life_ms / s_accel);
        d->life_ms = l < 1000u ? 1000u : l;         // never below a second (would thrash the radios)
    }
```

to:

```c
    if (s_turbo) {                                  // TURBO: ignore role/atype bands AND accel
        d->life_ms = rnd_range(TURBO_LIFE_MIN_MS, TURBO_LIFE_MAX_MS);
    } else if (s_accel > 1.0f) {                    // accelerated churn: shorter lives, same shape
        uint32_t l = (uint32_t)((float)d->life_ms / s_accel);
        d->life_ms = l < 1000u ? 1000u : l;         // never below a second (would thrash the radios)
    }
```

- [ ] **Step 6: Rebuild and run the test**

```powershell
& "$env:USERPROFILE\Downloads\simulacra\tools\decoy_audit\run.ps1" -Rebuild
python -m unittest discover -s "$env:USERPROFILE\Downloads\simulacra\tools\decoy_audit\tests" -k TurboBleChurn -v
```

Expected: `Ran 3 tests ... OK`

- [ ] **Step 7: Run the full decoy_audit suite to confirm no regression**

```powershell
python -m unittest discover -s "$env:USERPROFILE\Downloads\simulacra\tools\decoy_audit\tests"
```

Expected: all tests pass (99+ tests, matching the count before this task plus the 3 new ones).

- [ ] **Step 8: Commit**

```bash
git add main/ble_devices.h main/ble_devices.c tools/decoy_audit/synth_dump.c tools/decoy_audit/tests/test_turbo_ble.py
git commit -m "feat(turbo): ble_devices_set_turbo -- short fixed respawn band

Overrides the role/atype lifetime bands (and accel) with a short fixed
band (2-5s, provisional) so the BLE crowd respawns with fresh addresses
and payloads as fast as the presentation cadence (churn_set_slice_ms,
previous commit) allows. atype/role/payload draw is untouched, so
identity diversity is unaffected -- only how often it changes."
```

---

### Task 4: Wi-Fi turbo churn — `probe_agents_set_turbo`

**Files:**
- Modify: `main/probe_agents.h`
- Modify: `main/probe_agents.c`
- Modify: `tools/probe_audit/probe_dump.c`
- Create: `tools/probe_audit/tests/test_turbo_wifi.py`

**Interfaces:**
- Produces: `void probe_agents_set_turbo(bool on);` — when on, freshly spawned agents are forced `DUTY_ACTIVE` with the interval floored to `ACTIVE_MIN_MS`, and MAC rotation (both at spawn and via `probe_agents_rotate_tick`) uses a much shorter band than the normal 8–15 min persona band.
- Consumes: nothing new.

- [ ] **Step 1: Write the failing host test**

Create `tools/probe_audit/tests/test_turbo_wifi.py`:

```python
import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def turborot(seed=1, ticks=20, tickms=1000):
    out = subprocess.check_output([EXE, "--turborot", str(seed), str(ticks), str(tickms)], text=True)
    return [(int(t), m) for t, m in (ln.split() for ln in out.splitlines())]


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class TurboWifiRotation(unittest.TestCase):
    def test_mac_rotates_multiple_times_in_20s(self):
        # The normal (non-turbo) persona MAC rotation band is 8-15 MINUTES; in a 20s window it
        # would never rotate at all. Turbo's 3-8s band must show several rotations in the same window.
        rows = turborot(seed=2, ticks=20, tickms=1000)
        self.assertGreaterEqual(len(rows), 3, f"expected several MAC changes in 20s, got {rows}")

    def test_gaps_sit_in_the_turbo_band(self):
        rows = turborot(seed=4, ticks=25, tickms=1000)
        times = [t for t, _ in rows]
        for a, b in zip(times, times[1:]):
            self.assertGreaterEqual(b - a, 3000 - 1000, f"rotated too fast: {b - a} ms")
            self.assertLessEqual(b - a, 8000 + 1000, f"rotated too slow: {b - a} ms")

    def test_every_mac_is_unique(self):
        rows = turborot(seed=6, ticks=25, tickms=1000)
        macs = [m for _, m in rows]
        self.assertEqual(len(macs), len(set(macs)), "a turbo rotation reused a MAC")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Add the `--turborot` mode to `probe_dump.c`**

In `tools/probe_audit/probe_dump.c`, right after the `--agentrot` block closes (before the `--coexistrot` comment block), insert:

```c
    // TURBO mode: unbound agents (no persona binding), MAC rotation only, on the fast turbo band
    // instead of the 8-15 min persona band. Guard against the mode ever silently reverting to the
    // slow band.
    if (argc > 1 && strcmp(argv[1], "--turborot") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      ticks  = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 20;
        unsigned tickms = argc > 4 ? (unsigned)strtoul(argv[4], 0, 10) : 1000;
        srand(seed);
        probe_agents_set_turbo(1);
        probe_agents_set_target(1, 0);
        char last[13] = "";
        uint32_t t = 0;
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            probe_agents_rotate_tick(t);
            const probe_agent_t *a = probe_agents_at(0);
            char hex[13]; for (int b = 0; b < 6; b++) sprintf(hex + b * 2, "%02x", a->mac[b]);
            if (strcmp(hex, last) != 0) { printf("%u %s\n", (unsigned)t, hex); strcpy(last, hex); }
        }
        return 0;
    }
```

- [ ] **Step 3: Rebuild and confirm the test fails**

```powershell
& "$env:USERPROFILE\Downloads\simulacra\tools\probe_audit\run.ps1" -Rebuild
```

Expected: a C compile error — `probe_agents_set_turbo` undeclared.

- [ ] **Step 4: Implement `probe_agents_set_turbo` in `probe_agents.h`**

In `main/probe_agents.h`, change:

```c
// Move `current` toward `target` by at most `step` (magnitude), never overshooting. Pure: the
// glide's step arithmetic, isolated from the jitter clock so it is directly unit-testable.
int probe_glide_next(int current, int target, int step);
```

to:

```c
// Move `current` toward `target` by at most `step` (magnitude), never overshooting. Pure: the
// glide's step arithmetic, isolated from the jitter clock so it is directly unit-testable.
int probe_glide_next(int current, int target, int step);

// TURBO mode: freshly spawned agents are forced DUTY_ACTIVE with the scan interval floored to
// ACTIVE_MIN_MS, and MAC rotation (agent_spawn's initial schedule AND probe_agents_rotate_tick)
// uses a much shorter band than the normal 8-15 min persona band. Idempotent; off by default.
void probe_agents_set_turbo(bool on);
```

- [ ] **Step 5: Implement it in `probe_agents.c`**

In `main/probe_agents.c`, change:

```c
#define PERSONA_MAC_ROT_MIN_MS 480000u   // 8 min  (Wi-Fi MAC intra-life rotation, fast-realistic)
#define PERSONA_MAC_ROT_MAX_MS 900000u   // 15 min
```

to:

```c
#define PERSONA_MAC_ROT_MIN_MS 480000u   // 8 min  (Wi-Fi MAC intra-life rotation, fast-realistic)
#define PERSONA_MAC_ROT_MAX_MS 900000u   // 15 min
// TURBO MAC rotation band -- placeholder pending the on-hardware tuning pass
// (docs/superpowers/specs/2026-08-12-turbo-flood-mode-design.md, Open question).
#define TURBO_MAC_ROT_MIN_MS 3000u    // 3 s
#define TURBO_MAC_ROT_MAX_MS 8000u    // 8 s
```

Then change:

```c
static uint32_t rnd_range(uint32_t lo, uint32_t hi) { return lo + (esp_random() % (hi - lo + 1u)); }
static uint32_t persona_mac_rotate_base(void) { return rnd_range(PERSONA_MAC_ROT_MIN_MS, PERSONA_MAC_ROT_MAX_MS); }
```

to:

```c
static uint32_t rnd_range(uint32_t lo, uint32_t hi) { return lo + (esp_random() % (hi - lo + 1u)); }
static bool     s_turbo = false;
void probe_agents_set_turbo(bool on) { s_turbo = on; }
// The MAC rotation interval, on whichever band is active. Renamed from persona_mac_rotate_base:
// it now serves both the persona band (unchanged) and the turbo band, not personas exclusively.
static uint32_t mac_rotate_base(void)
{
    return s_turbo ? rnd_range(TURBO_MAC_ROT_MIN_MS, TURBO_MAC_ROT_MAX_MS)
                   : rnd_range(PERSONA_MAC_ROT_MIN_MS, PERSONA_MAC_ROT_MAX_MS);
}
```

Then in `agent_spawn`, change:

```c
static void agent_spawn(probe_agent_t *a, uint32_t now_ms)
{
    probe_random_mac(a->mac);
    a->arch    = probe_pick_archetype();
    a->seq     = (uint16_t)(esp_random() & 0x0FFFu);             // fresh random 12-bit base
    a->duty    = (esp_random() % 3u == 0u) ? DUTY_ACTIVE : DUTY_IDLE;  // ~33% active
    a->born_ms = now_ms;
    a->life_ms = rnd_range(LIFE_MIN_MS, LIFE_MAX_MS);
    a->alive   = true;
    uint32_t base = (a->duty == DUTY_ACTIVE) ? rnd_range(ACTIVE_MIN_MS, ACTIVE_MAX_MS)
                                             : rnd_range(IDLE_MIN_MS, IDLE_MAX_MS);
    a->next_scan_ms = now_ms + (esp_random() % base);            // random phase-in (not all due at once)
    a->next_mac_rotate_ms = now_ms + persona_mac_rotate_base();
    assign_ssids(a);
}
```

to:

```c
static void agent_spawn(probe_agent_t *a, uint32_t now_ms)
{
    probe_random_mac(a->mac);
    a->arch    = probe_pick_archetype();
    a->seq     = (uint16_t)(esp_random() & 0x0FFFu);             // fresh random 12-bit base
    // TURBO: always active, no idle 67% -- maximum burst frequency.
    a->duty    = s_turbo ? DUTY_ACTIVE : ((esp_random() % 3u == 0u) ? DUTY_ACTIVE : DUTY_IDLE);
    a->born_ms = now_ms;
    a->life_ms = rnd_range(LIFE_MIN_MS, LIFE_MAX_MS);
    a->alive   = true;
    // TURBO: floored to the fastest existing band rather than randomized within it.
    uint32_t base = s_turbo ? ACTIVE_MIN_MS
                  : (a->duty == DUTY_ACTIVE) ? rnd_range(ACTIVE_MIN_MS, ACTIVE_MAX_MS)
                                             : rnd_range(IDLE_MIN_MS, IDLE_MAX_MS);
    a->next_scan_ms = now_ms + (esp_random() % base);            // random phase-in (not all due at once)
    a->next_mac_rotate_ms = now_ms + mac_rotate_base();
    assign_ssids(a);
}
```

Then in `probe_agents_rotate_tick`, change:

```c
            a->next_mac_rotate_ms = now_ms + persona_mac_rotate_base();
```

to:

```c
            a->next_mac_rotate_ms = now_ms + mac_rotate_base();
```

- [ ] **Step 6: Rebuild and run the test**

```powershell
& "$env:USERPROFILE\Downloads\simulacra\tools\probe_audit\run.ps1" -Rebuild
python -m unittest discover -s "$env:USERPROFILE\Downloads\simulacra\tools\probe_audit\tests" -k TurboWifiRotation -v
```

Expected: `Ran 3 tests ... OK`

- [ ] **Step 7: Run the full probe_audit suite to confirm no regression**

```powershell
python -m unittest discover -s "$env:USERPROFILE\Downloads\simulacra\tools\probe_audit\tests"
```

Expected: all tests pass (89+ tests, matching the count before this task plus the 3 new ones).

- [ ] **Step 8: Commit**

```bash
git add main/probe_agents.h main/probe_agents.c tools/probe_audit/probe_dump.c tools/probe_audit/tests/test_turbo_wifi.py
git commit -m "feat(turbo): probe_agents_set_turbo -- forced-active duty + fast MAC rotation

Freshly spawned agents are forced DUTY_ACTIVE with the scan interval
floored, and MAC rotation moves from the 8-15 min persona band to a
3-8s turbo band (provisional). persona_mac_rotate_base renamed to
mac_rotate_base since it now serves both bands."
```

---

### Task 5: `coexist_set_turbo` — the tick-level override

**Files:**
- Modify: `main/coexist.h`
- Modify: `main/coexist.c`
- Modify: `main/settings.c`

**Interfaces:**
- Produces: `void coexist_set_turbo(bool on);` — forces `ble_devices_count()` to `BLE_DEVICES_MAX` and `probe_agents_count()` to `PROBE_AGENTS_MAX`, releases any bound personas, and switches both radios' churn intervals, all while active. Skips reprofile/glide/census-recalc so nothing claws the population back down while turbo is on.
- Consumes: `ble_devices_set_turbo` (Task 3), `probe_agents_set_turbo` (Task 4), `churn_set_slice_ms` (Task 2), `sim_settings_t.turbo` (Task 1).

**No host test for this task.** `coexist.c` has no host-testable path anywhere in this codebase (it is saturated with `esp_wifi`/`esp_timer` calls) — every existing change to it in this project has been verified by build success plus the on-target self-test, and this task follows that same, already-established precedent. Task 6 adds the on-target self-test that exercises this function for real.

- [ ] **Step 1: Declare `coexist_set_turbo`**

In `main/coexist.h`, change:

```c
void coexist_request_preset(uint8_t preset_id);
```

to:

```c
void coexist_request_preset(uint8_t preset_id);

// TURBO override: force both radios to their hardware ceiling and switch to fast churn intervals,
// bypassing the fleet-share floor/ceiling and room-density population-match entirely. Releases any
// bound personas (turbo doesn't use them). Idempotent. Call with false to hand control back to the
// normal reprofile/glide path on the next tick.
void coexist_set_turbo(bool on);
```

- [ ] **Step 2: Implement it and wire the tick-loop skip logic**

In `main/coexist.c`, find the static state block:

```c
static bool     s_wifi_ok;
static bool     s_wifi_allowed = true;    // webui: false defers Wi-Fi (STA) so the config AP can own it
static uint32_t s_wifi_ctr;
static uint32_t s_accel_until_ms;         // 0 = not accelerating
static int      s_listen_ch = -1;         // espnow: >=0 -> park Wi-Fi on this channel between bursts to listen
```

Change it to:

```c
static bool     s_wifi_ok;
static bool     s_wifi_allowed = true;    // webui: false defers Wi-Fi (STA) so the config AP can own it
static uint32_t s_wifi_ctr;
static uint32_t s_accel_until_ms;         // 0 = not accelerating
static int      s_listen_ch = -1;         // espnow: >=0 -> park Wi-Fi on this channel between bursts to listen
static bool     s_turbo;                  // TURBO preset active: every radio floods at hardware max
#define COEX_TURBO_SLICE_MS 250u          // BLE presentation cadence while turbo (vs CHURN_SLICE_MS=1000)
```

Then, right after the `coexist_drain_requests` function definition, add:

```c
void coexist_set_turbo(bool on)
{
    if (on == s_turbo) return;                        // idempotent
    s_turbo = on;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    ble_devices_set_turbo(on);
    probe_agents_set_turbo(on);
    churn_set_slice_ms(on ? COEX_TURBO_SLICE_MS : CHURN_SLICE_MS);
    if (on) {
        // Bypasses the fleet-share ceiling directly: every board floods at its OWN hardware max,
        // independent of room density or how many peers are heard. No persona coupling either --
        // personas exist to defeat single-radio-ghost filtering (an indistinguishability
        // mechanism), which is not the goal here, so release any currently bound.
        ble_devices_set_count(BLE_DEVICES_MAX, now);
        probe_agents_set_target(PROBE_AGENTS_MAX, now);
        phantom_set_count(0, now);
        ESP_LOGW(TAG, "TURBO: flooding at max (ble=%d wifi=%d)", BLE_DEVICES_MAX, PROBE_AGENTS_MAX);
    } else {
        ESP_LOGW(TAG, "TURBO: off, resuming normal population-match");
    }
}
```

- [ ] **Step 3: Skip the census-triggered bounds recalc while turbo is active**

Find, in `coexist_task`:

```c
        {   // A node joining or leaving changes everyone's share; resize now rather than waiting
            // for the next re-profile (up to 10 min on Ward).
            static int s_last_k = -1;
            int k = fleet_pop_size();
            if (k != s_last_k) {
                if (s_last_k >= 0) ESP_LOGW(TAG, "fleet census %d -> %d nodes; resizing crowd", s_last_k, k);
                s_last_k = k;
                sim_settings_recalc_bounds();
            }
        }
```

Change the last line inside the `if` to:

```c
        {   // A node joining or leaving changes everyone's share; resize now rather than waiting
            // for the next re-profile (up to 10 min on Ward).
            static int s_last_k = -1;
            int k = fleet_pop_size();
            if (k != s_last_k) {
                if (s_last_k >= 0) ESP_LOGW(TAG, "fleet census %d -> %d nodes; resizing crowd", s_last_k, k);
                s_last_k = k;
                if (!s_turbo) sim_settings_recalc_bounds();   // turbo's bounds aren't K-relative
            }
        }
```

- [ ] **Step 4: Fire Wi-Fi on every tick while turbo, and skip the persona/glide sub-block**

Find:

```c
        if (d.fire_wifi && s_wifi_ok && s_wifi_allowed && !observe_window_active()) {
            probe_agents_glide_tick(now);                             // ramp applied pop toward target
            const uint8_t *ch24; size_t n24 = probe_channels_24(&ch24);
            // The glide moves the Wi-Fi agent count to match room density; the persona registry
            // must follow it. Personas beyond the agent count would advertise a phone on BLE that
            // never probes on Wi-Fi, and agents beyond the persona count would have no BLE twin and
            // no lifecycle here (probe_agents_lifecycle is SIMULACRA_PROBE-only), so they would
            // never age out. Keeping the counts equal preserves the one-device-two-radios invariant.
            // Personas may never fill the whole BLE crowd: a crowd that is 100% phone-shaped
            // personas is a monoculture (every device company 0x0000, no beacons, no tags), which
            // is a stronger tell than any single device. Cap them at half the population and pull
            // the Wi-Fi agent set down to match, preserving the one-device-two-radios invariant.
            int crowd = ble_devices_count();
            int cap   = crowd / 2;  if (cap < 1) cap = 1;
            if (probe_agents_count() > cap) probe_agents_set_target(cap, now);
            phantom_set_count(probe_agents_count(), now);
            phantom_sync_wifi(now);                                   // agents track persona lives
            probe_agents_rotate_tick(now);        // intra-life MAC rotation (8-15 min): without this
                                                  // a persona holds ONE Wi-Fi MAC for its whole life
                                                  // while its BLE RPA rotates — the mismatch is the
                                                  // tell. probe_agents_lifecycle is standalone-only.
            if (n24) probe_inject_burst(ch24[hop24++ % n24]);        // 2.4 GHz (coex-arbitrated)
            if (p->use_5g && (++s_wifi_ctr % COEX_5G_EVERY == 0)) coexist_5g_excursion();
        }
```

Change it to:

```c
        // s_turbo ORs into the gate: while turbo is active, fire on EVERY tick (COEX_TICK_MS = 250
        // ms) rather than waiting for the persona's normal wifi_period_ms (2-7s). This is the real
        // Wi-Fi throughput lever, the same way churn_set_slice_ms is the real BLE lever.
        if ((d.fire_wifi || s_turbo) && s_wifi_ok && s_wifi_allowed && !observe_window_active()) {
            const uint8_t *ch24; size_t n24 = probe_channels_24(&ch24);
            if (!s_turbo) {
                probe_agents_glide_tick(now);                         // ramp applied pop toward target
                // The glide moves the Wi-Fi agent count to match room density; the persona registry
                // must follow it. Personas beyond the agent count would advertise a phone on BLE
                // that never probes on Wi-Fi, and agents beyond the persona count would have no BLE
                // twin and no lifecycle here (probe_agents_lifecycle is SIMULACRA_PROBE-only), so
                // they would never age out. Keeping the counts equal preserves the
                // one-device-two-radios invariant. Personas may never fill the whole BLE crowd: a
                // crowd that is 100% phone-shaped personas is a monoculture (every device company
                // 0x0000, no beacons, no tags), which is a stronger tell than any single device.
                // Cap them at half the population and pull the Wi-Fi agent set down to match.
                // TURBO skips all of this -- it doesn't use personas at all (see coexist_set_turbo).
                int crowd = ble_devices_count();
                int cap   = crowd / 2;  if (cap < 1) cap = 1;
                if (probe_agents_count() > cap) probe_agents_set_target(cap, now);
                phantom_set_count(probe_agents_count(), now);
                phantom_sync_wifi(now);                               // agents track persona lives
            }
            probe_agents_rotate_tick(now);        // intra-life MAC rotation: without this a persona
                                                  // holds ONE Wi-Fi MAC for its whole life while its
                                                  // BLE RPA rotates — the mismatch is the tell.
                                                  // probe_agents_lifecycle is standalone-only.
            if (n24) probe_inject_burst(ch24[hop24++ % n24]);        // 2.4 GHz (coex-arbitrated)
            if (p->use_5g && (++s_wifi_ctr % COEX_5G_EVERY == 0)) coexist_5g_excursion();
        }
```

- [ ] **Step 5: Skip BLE reprofile while turbo is active**

Find:

```c
        if (d.fire_reprofile) coexist_reprofile_start();
```

Change it to:

```c
        if (d.fire_reprofile && !s_turbo) coexist_reprofile_start();   // turbo owns its own population
```

- [ ] **Step 6: Wire `sim_settings_apply` to call `coexist_set_turbo`**

In `main/settings.c`, add the include:

```c
#include "settings.h"
#include "churn.h"
#include "probe.h"        // probe_desired_ble_floor(): this board's designed crowd size
#include "ble_devices.h"  // BLE_DEVICES_MAX
#include "fleet_pop.h"    // fleet_pop_share(): this node's share when K nodes split the crowd
#include "nvs.h"
#include <string.h>
```

to:

```c
#include "settings.h"
#include "churn.h"
#include "coexist.h"      // coexist_set_turbo(): the TURBO override lives at the coexist tick level
#include "probe.h"        // probe_desired_ble_floor(): this board's designed crowd size
#include "ble_devices.h"  // BLE_DEVICES_MAX
#include "fleet_pop.h"    // fleet_pop_share(): this node's share when K nodes split the crowd
#include "nvs.h"
#include <string.h>
```

Then change:

```c
void sim_settings_apply(const sim_settings_t *s)
{
    churn_set_active_target(s->active_target);
    churn_set_paused(s->paused);
    churn_set_accel(s->accel);
    s_cur = *s;
}
```

to:

```c
void sim_settings_apply(const sim_settings_t *s)
{
    churn_set_active_target(s->active_target);
    churn_set_paused(s->paused);
    churn_set_accel(s->accel);
    coexist_set_turbo(s->turbo);
    s_cur = *s;
}
```

- [ ] **Step 7: Build all three targets to confirm it compiles and links**

```powershell
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Fleet -Do build
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c6 -Fleet -Do build
```

Expected: `BUILD: Project build complete.` for both.

- [ ] **Step 8: Run the full host suite to confirm nothing outside coexist.c regressed**

```powershell
$r = "$env:USERPROFILE\Downloads\simulacra"
foreach ($d in @("tools\probe_audit\tests","tools\decoy_audit\tests","tools\pcap_learn\tests","tools\radar_audit\tests","tools\tests","web")) {
  python -m unittest discover -s (Join-Path $r $d)
}
```

Expected: `OK` for every suite.

- [ ] **Step 9: Commit**

```bash
git add main/coexist.h main/coexist.c main/settings.c
git commit -m "feat(turbo): coexist_set_turbo -- the tick-level override

Forces both radios to their hardware ceiling, releases bound personas,
and switches churn intervals via the previous two tasks' setters.
Reprofile, glide, and the census-triggered bounds recalc are all
skipped while active -- each of them would otherwise claw the forced
population back down toward a room-density or fleet-share estimate,
undoing the whole point of the mode. sim_settings_apply now calls this
as its fourth setter, same pattern as churn_set_active_target/paused/accel."
```

---

### Task 6: On-target self-test — the first full end-to-end exercise

**Files:**
- Modify: `main/churn_selftest.c`

**Interfaces:**
- Consumes: everything from Tasks 1–5, exercised together for the first time.
- Produces: nothing new — assertions only.

- [ ] **Step 1: Add the `phantom.h` include**

In `main/churn_selftest.c`, change:

```c
#include "probe.h"
#include "drift.h"
```

to:

```c
#include "probe.h"
#include "phantom.h"
#include "drift.h"
```

- [ ] **Step 2: Add the TURBO block to `test_settings_apply`**

Find, near the end of `test_settings_apply`:

```c
    sim_settings_apply_preset(SIM_PRESET_MAX);
    ST_CHECK(churn_active_target() == ce, "MAX actually refills the crowd to the ceiling");
    ST_CHECK(churn_active_target() >= stealth_n, "MAX is never smaller than STEALTH");
    ST_CHECK(churn_accel() > 2.0f, "MAX actually accelerates turnover");
```

Add immediately after it:

```c
    // TURBO bypasses the fleet-share ceiling/floor entirely: the board's OWN hardware max, not the
    // K-shared value `ce` above. Also: no persona coupling (turbo releases any bound personas), and
    // the display must correctly infer TURBO rather than reporting CUSTOM.
    phantom_init(2, 0);          // bind a couple of personas so releasing them is actually observable
    phantom_sync_ble(0);
    ST_CHECK(phantom_count() == 2, "personas bound before turbo (sanity)");
    sim_settings_apply_preset(SIM_PRESET_TURBO);
    ST_CHECK(churn_active_target() == BLE_DEVICES_MAX, "TURBO fills BLE to the hardware max, not the fleet ceiling");
    ST_CHECK(probe_agents_count() == PROBE_AGENTS_MAX, "TURBO fills Wi-Fi to the hardware max");
    ST_CHECK(phantom_count() == 0, "TURBO releases any bound personas");
    ST_CHECK(sim_settings_current_preset() == SIM_PRESET_TURBO, "display correctly infers TURBO");

    sim_settings_apply_preset(SIM_PRESET_NORMAL);
    ST_CHECK(sim_settings_current_preset() != SIM_PRESET_TURBO, "leaving TURBO actually turns it off");
    ST_CHECK(churn_active_target() <= ce, "population returns to the fleet-shared ceiling after TURBO");
```

- [ ] **Step 3: Confirm the existing "every preset keeps room for personas" loop still passes for TURBO**

No code change needed — locate this existing loop in `test_settings_apply` and confirm by inspection that it will pass once TURBO exists:

```c
    // The property the floor exists for: NO preset can shrink the crowd below the persona budget.
    for (sim_preset_t p = SIM_PRESET_PAUSE; p < SIM_PRESET_COUNT; p++) {
        sim_settings_apply_preset(p);
        ST_CHECK(churn_active_target() >= fl, "every preset keeps room for the designed personas");
    }
```

`SIM_PRESET_COUNT` now includes TURBO automatically (Task 1 bumped it), and TURBO's `churn_active_target()` is `BLE_DEVICES_MAX`, which is always `>= fl` by construction (`fl` is clamped to at most `BLE_DEVICES_MAX` in `sim_settings_floor`). This loop needs no edit; it is listed here so the implementer doesn't miss verifying it.

- [ ] **Step 4: Build and flash the self-test firmware**

```powershell
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Do buildflash -Port COM12
```

(Substitute whichever port is currently free for a self-test flash; this does not need to be a specific board.)

- [ ] **Step 5: Read the self-test result and confirm PASS**

```powershell
python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port COM12 --reset yes --seconds 25 --grep "SELFTEST|FAIL"
```

Expected: `SELFTEST: PASS (N/N)` with `fails=0`, and no `FAIL:` lines. If any TURBO-specific assertion fails, read the printed message and check the corresponding code from Tasks 1–5 rather than editing the test to match broken behavior.

- [ ] **Step 6: Reflash the normal fleet build so the board isn't left on self-test firmware**

```powershell
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Fleet -Do buildflash -Port COM12
```

- [ ] **Step 7: Commit**

```bash
git add main/churn_selftest.c
git commit -m "test(turbo): on-target self-test for the full settings->coexist->radios chain

First real functional exercise of coexist_set_turbo (no host harness
exists for coexist.c). Confirms both radios hit their hardware ceiling
independent of the fleet-share ceiling, personas are released, the
display correctly infers TURBO, and leaving it actually turns it off.

Verified PASS on hardware (COM12): SELFTEST: PASS, fails=0."
```

---

### Task 7: CYD CONTROL — the TURBO button

**Files:**
- Modify: `components/simulacra_radar/radar_ui.h`
- Modify: `components/simulacra_radar/radar_render.c`
- Modify: `components/simulacra_radar/radar_render.h`
- Modify: `tools/radar_audit/render_dump.c`
- Modify: `cyd/main/cyd_main.c`
- Create: `tools/radar_audit/tests/test_turbo_control.py`

**Interfaces:**
- Produces: a 6th preset slot on the CONTROL page (`RADAR_CTRL_PRESET_COUNT` = 6), a `turbo_armed` field on `radar_ctrl_info_t`, and two-tap arm/confirm behavior on the SEND button specifically when the pending preset is TURBO.
- Consumes: `SIM_PRESET_TURBO`'s numeric value (5) from Task 1 — referenced here as a local named constant, not a shared header include, matching how this file already treats every other preset (by array position, not by symbol) since `components/simulacra_radar` does not depend on `main/settings.h`.

- [ ] **Step 1: Bump the preset count**

In `components/simulacra_radar/radar_ui.h`, change:

```c
#define RADAR_CTRL_PRESET_COUNT 5
```

to:

```c
#define RADAR_CTRL_PRESET_COUNT 6
```

(No other change needed in this file — `radar_ctrl_select_next` in `radar_ui.c` and the prev-cycle loop in `cyd_main.c` already use this constant rather than a hardcoded 5.)

- [ ] **Step 2: Add `turbo_armed` to the CONTROL info struct**

In `components/simulacra_radar/radar_render.h`, change:

```c
typedef struct { uint8_t sel_preset; bool send_flash; uint8_t live_preset; bool clear_armed; } radar_ctrl_info_t;   // CONTROL page state
```

to:

```c
typedef struct { uint8_t sel_preset; bool send_flash; uint8_t live_preset; bool clear_armed; bool turbo_armed; } radar_ctrl_info_t;   // CONTROL page state
```

- [ ] **Step 3: Write the failing host test**

Create `tools/radar_audit/tests/test_turbo_control.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")


def control(sel=2, live=255, flash=0, clear_armed=0, turbo_armed=0):
    args = [EXE, "--control", sel, live, flash, clear_armed, turbo_armed]
    out = subprocess.check_output([str(x) for x in args], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class TurboControl(unittest.TestCase):
    def test_turbo_is_the_sixth_preset(self):
        # preset id 5 (SIM_PRESET_TURBO) must render as "TURBO" in the pending box, cycling around
        # from PAUSE(0)..MAX(4) rather than wrapping back to PAUSE at 5.
        texts = control(sel=5, live=0xFF)
        self.assertTrue(any("TURBO" in t for t in texts), f"drew: {texts}")

    def test_live_turbo_shows_in_the_live_slot(self):
        texts = control(sel=2, live=5)
        self.assertTrue(any("TURBO" in t for t in texts), f"live TURBO not shown; drew: {texts}")

    def test_send_shows_confirm_when_turbo_pending_and_armed(self):
        texts = control(sel=5, live=0xFF, turbo_armed=1)
        self.assertTrue(any("CONFIRM" in t for t in texts), f"drew: {texts}")

    def test_send_shows_plain_send_when_turbo_pending_but_not_armed(self):
        texts = control(sel=5, live=0xFF, turbo_armed=0)
        self.assertIn("SEND", texts, f"drew: {texts}")
        self.assertFalse(any("CONFIRM" in t for t in texts), f"should not be armed yet; drew: {texts}")

    def test_non_turbo_presets_unaffected_by_turbo_armed(self):
        # turbo_armed must only change rendering when TURBO is actually the pending preset.
        texts = control(sel=2, live=0xFF, turbo_armed=1)
        self.assertIn("SEND", texts, f"drew: {texts}")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 4: Rebuild and confirm the test fails (mode doesn't accept the new arg yet)**

```powershell
& "$env:USERPROFILE\Downloads\simulacra\tools\radar_audit\run.ps1"
```

Expected: the new tests fail (either a crash from too few args being read as 0/garbage, or `TURBO`/`CONFIRM` text simply not found — the render code doesn't know about a 6th preset or `turbo_armed` yet).

- [ ] **Step 5: Fix the hardcoded `5`s in `radar_render.c`**

In `components/simulacra_radar/radar_render.c`, change:

```c
static const char *CTRL_LABELS[5] = { "PAUSE", "STEALTH", "NORMAL", "DENSE", "MAX" };
static const char *PRESET_DESC[5] = { "freeze on-air", "min crowd", "balanced", "big crowd", "max crowd" };
static const char *ctrl_preset_name(uint8_t p){
    if (p < 5)     return CTRL_LABELS[p];
    if (p == 5)    return "CUSTOM";
    if (p == 0xFE) return "MIXED";
    return "-";                       // 0xFF none
}
```

to:

```c
// SIM_PRESET_TURBO; keep this in sync with the numeric order of sim_preset_t in main/settings.h --
// this component does not (and should not) depend on main/, so the two stay in sync by convention
// and by CTRL_LABELS' array position, same as every other preset here.
#define CTRL_TURBO_PRESET 5
static const char *CTRL_LABELS[RADAR_CTRL_PRESET_COUNT] =
    { "PAUSE", "STEALTH", "NORMAL", "DENSE", "MAX", "TURBO" };
static const char *PRESET_DESC[RADAR_CTRL_PRESET_COUNT] =
    { "freeze on-air", "min crowd", "balanced", "big crowd", "max crowd", "flood the zone" };
static const char *ctrl_preset_name(uint8_t p){
    if (p < RADAR_CTRL_PRESET_COUNT)      return CTRL_LABELS[p];
    if (p == RADAR_CTRL_PRESET_COUNT)     return "CUSTOM";
    if (p == 0xFE)                        return "MIXED";
    return "-";                       // 0xFF none
}
```

- [ ] **Step 6: Fix the remaining hardcoded `5`s and add the confirm-vs-send logic**

In `components/simulacra_radar/radar_render.c`, change:

```c
    char box[16]; snprintf(box, sizeof box, "[ %s ]", CTRL_LABELS[sel % 5]);
    radar_gfx_text(g, 70, 120, box, COL_FG);
    radar_gfx_text(g, 8, 152, PRESET_DESC[sel % 5], COL_DIM);
    // SEND / SENT / ACTIVE
    bool active = c && (c->live_preset == c->sel_preset) && (c->live_preset <= 4);
    radar_gfx_fill_rect(g, 60, 205, 120, 34, COL_RING);      // SEND button
    const char *slabel = (c && c->send_flash) ? "SENT" : active ? "ACTIVE" : "SEND";
    uint16_t slc       = (c && c->send_flash) ? COL_OK  : active ? COL_DIM  : COL_FG;
    radar_gfx_text(g, 96, 216, slabel, slc);
```

to:

```c
    char box[16]; snprintf(box, sizeof box, "[ %s ]", CTRL_LABELS[sel % RADAR_CTRL_PRESET_COUNT]);
    radar_gfx_text(g, 70, 120, box, COL_FG);
    radar_gfx_text(g, 8, 152, PRESET_DESC[sel % RADAR_CTRL_PRESET_COUNT], COL_DIM);
    // SEND / SENT / ACTIVE / CONFIRM (TURBO pending + armed needs a second tap, like CLEAR THREATS)
    bool active      = c && (c->live_preset == c->sel_preset) && (c->live_preset < RADAR_CTRL_PRESET_COUNT);
    bool turbo_ask   = c && (sel % RADAR_CTRL_PRESET_COUNT == CTRL_TURBO_PRESET) && c->turbo_armed;
    radar_gfx_fill_rect(g, 60, 205, 120, 34, turbo_ask ? COL_WARN : COL_RING);   // SEND button
    const char *slabel = turbo_ask ? "CONFIRM?" : (c && c->send_flash) ? "SENT" : active ? "ACTIVE" : "SEND";
    uint16_t slc       = turbo_ask ? COL_FG : (c && c->send_flash) ? COL_OK  : active ? COL_DIM  : COL_FG;
    radar_gfx_text(g, 96, 216, slabel, slc);
```

- [ ] **Step 7: Add the `turbo_armed` positional argument to `render_dump.c`**

In `tools/radar_audit/render_dump.c`, change:

```c
    if (argc > 1 && strcmp(argv[1], "--control") == 0) {
        int a = 2;
        int sel   = argc > a ? atoi(argv[a]) : 2; a++;
        int live  = argc > a ? atoi(argv[a]) : 0xFF; a++;
        int flash = argc > a ? atoi(argv[a]) : 0; a++;
        int carm  = argc > a ? atoi(argv[a]) : 0; a++;
        radar_wire_status_t st; memset(&st, 0, sizeof st);
        radar_ctrl_info_t ctrl; memset(&ctrl, 0, sizeof ctrl);
        ctrl.sel_preset = (uint8_t)sel; ctrl.live_preset = (uint8_t)live; ctrl.send_flash = flash != 0;
        ctrl.clear_armed = carm != 0;
        static uint16_t cband[240 * 320];
        radar_render_view(RADAR_VIEW_CONTROL, &st, 0, 0, -1, -1, 0, &ctrl, NULL, NULL, 0,
                          cband, 320, 240, 320, flush_noop, 0);
        return 0;
    }
```

to:

```c
    if (argc > 1 && strcmp(argv[1], "--control") == 0) {
        int a = 2;
        int sel   = argc > a ? atoi(argv[a]) : 2; a++;
        int live  = argc > a ? atoi(argv[a]) : 0xFF; a++;
        int flash = argc > a ? atoi(argv[a]) : 0; a++;
        int carm  = argc > a ? atoi(argv[a]) : 0; a++;
        int tarm  = argc > a ? atoi(argv[a]) : 0; a++;
        radar_wire_status_t st; memset(&st, 0, sizeof st);
        radar_ctrl_info_t ctrl; memset(&ctrl, 0, sizeof ctrl);
        ctrl.sel_preset = (uint8_t)sel; ctrl.live_preset = (uint8_t)live; ctrl.send_flash = flash != 0;
        ctrl.clear_armed = carm != 0; ctrl.turbo_armed = tarm != 0;
        static uint16_t cband[240 * 320];
        radar_render_view(RADAR_VIEW_CONTROL, &st, 0, 0, -1, -1, 0, &ctrl, NULL, NULL, 0,
                          cband, 320, 240, 320, flush_noop, 0);
        return 0;
    }
```

- [ ] **Step 8: Rebuild and run the new tests**

```powershell
& "$env:USERPROFILE\Downloads\simulacra\tools\radar_audit\run.ps1"
```

Expected: all tests pass, including the 5 new `TurboControl` tests. (`run.ps1` rebuilds unconditionally and always runs the full suite, so this single command covers both.)

- [ ] **Step 9: Wire the CYD tap handler**

In `cyd/main/cyd_main.c`, find the `static uint32_t s_clear_arm_ms;` declaration and add a sibling right after it:

```c
static uint32_t s_clear_arm_ms;   // CONTROL: CLEAR THREATS armed-at (0 = disarmed); 3s confirm window
```

becomes:

```c
static uint32_t s_clear_arm_ms;   // CONTROL: CLEAR THREATS armed-at (0 = disarmed); 3s confirm window
static uint32_t s_turbo_arm_ms;   // CONTROL: TURBO SEND armed-at (0 = disarmed); 3s confirm window
#define CFG_PRESET_TURBO 5        // SIM_PRESET_TURBO; keep numeric value in sync with main/settings.h
```

Then find the CONTROL tap-handling block:

```c
                if (ty < 40) {                           // top strip = BACK to HOME (drawn "< BACK")
                    s_clear_arm_ms = 0;
                    radar_ui_on_input(&ui, now);
                } else if (ty >= 246) {                  // CLEAR THREATS band (2-tap arm/confirm)
                    if (s_clear_arm_ms && (uint32_t)(now - s_clear_arm_ms) < 3000) {
                        send_config(CONFIG_CLEAR_THREATS);
                        radar_ctrl_mark_sent(&ui, now);
                        s_clear_arm_ms = 0;
                    } else {
                        s_clear_arm_ms = now;            // arm
                    }
                } else if (ty > 200 && tx > 60 && tx < 180) {   // SEND button
                    s_clear_arm_ms = 0;
                    send_config(ui.sel_preset);
                    radar_ctrl_mark_sent(&ui, now);
                } else if (tx < 80) {                    // left zone: prev == cycle-around
                    s_clear_arm_ms = 0;
                    for (int i = 0; i < RADAR_CTRL_PRESET_COUNT - 1; i++) radar_ctrl_select_next(&ui);
                } else if (tx > 160) {                   // right zone: next
                    s_clear_arm_ms = 0;
                    radar_ctrl_select_next(&ui);
                } else {                                 // center (preset label) = stay put
                    radar_ui_note_input(&ui, now);
```

Change it to:

```c
                if (ty < 40) {                           // top strip = BACK to HOME (drawn "< BACK")
                    s_clear_arm_ms = 0; s_turbo_arm_ms = 0;
                    radar_ui_on_input(&ui, now);
                } else if (ty >= 246) {                  // CLEAR THREATS band (2-tap arm/confirm)
                    s_turbo_arm_ms = 0;
                    if (s_clear_arm_ms && (uint32_t)(now - s_clear_arm_ms) < 3000) {
                        send_config(CONFIG_CLEAR_THREATS);
                        radar_ctrl_mark_sent(&ui, now);
                        s_clear_arm_ms = 0;
                    } else {
                        s_clear_arm_ms = now;            // arm
                    }
                } else if (ty > 200 && tx > 60 && tx < 180) {   // SEND button
                    s_clear_arm_ms = 0;
                    if (ui.sel_preset == CFG_PRESET_TURBO) {   // 2-tap confirm: max RF output, fleet-wide
                        if (s_turbo_arm_ms && (uint32_t)(now - s_turbo_arm_ms) < 3000) {
                            send_config(ui.sel_preset);
                            radar_ctrl_mark_sent(&ui, now);
                            s_turbo_arm_ms = 0;
                        } else {
                            s_turbo_arm_ms = now;        // arm
                        }
                    } else {
                        s_turbo_arm_ms = 0;
                        send_config(ui.sel_preset);
                        radar_ctrl_mark_sent(&ui, now);
                    }
                } else if (tx < 80) {                    // left zone: prev == cycle-around
                    s_clear_arm_ms = 0; s_turbo_arm_ms = 0;
                    for (int i = 0; i < RADAR_CTRL_PRESET_COUNT - 1; i++) radar_ctrl_select_next(&ui);
                } else if (tx > 160) {                   // right zone: next
                    s_clear_arm_ms = 0; s_turbo_arm_ms = 0;
                    radar_ctrl_select_next(&ui);
                } else {                                 // center (preset label) = stay put
                    radar_ui_note_input(&ui, now);
```

- [ ] **Step 10: Pass `turbo_armed` into the rendered `radar_ctrl_info_t`**

Find:

```c
            radar_ctrl_info_t ctrl = { .sel_preset = ui.sel_preset,
                .send_flash = (ui.send_flash_ms && (now - ui.send_flash_ms) < RADAR_CTRL_FLASH_MS),
                .live_preset = agg.preset,
                .clear_armed = (s_clear_arm_ms && (uint32_t)(now - s_clear_arm_ms) < 3000) };
```

Change it to:

```c
            radar_ctrl_info_t ctrl = { .sel_preset = ui.sel_preset,
                .send_flash = (ui.send_flash_ms && (now - ui.send_flash_ms) < RADAR_CTRL_FLASH_MS),
                .live_preset = agg.preset,
                .clear_armed = (s_clear_arm_ms && (uint32_t)(now - s_clear_arm_ms) < 3000),
                .turbo_armed = (s_turbo_arm_ms && (uint32_t)(now - s_turbo_arm_ms) < 3000) };
```

- [ ] **Step 11: Build the CYD target**

```powershell
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target cyd -Fleet -Do build
```

Expected: `BUILD: Project build complete.`

- [ ] **Step 12: Commit**

```bash
git add components/simulacra_radar/radar_ui.h components/simulacra_radar/radar_render.h components/simulacra_radar/radar_render.c tools/radar_audit/render_dump.c tools/radar_audit/tests/test_turbo_control.py cyd/main/cyd_main.c
git commit -m "feat(turbo): CYD CONTROL page -- TURBO as the 6th preset, 2-tap confirm on SEND

RADAR_CTRL_PRESET_COUNT 5->6; fixes the hardcoded-5 spots in
radar_render.c this was already flagged as the known conflict point
from the flock-flood branch's own 6th preset. SEND requires arm/confirm
(reusing the exact CLEAR THREATS pattern) only when TURBO is the
pending preset -- every other preset still fires on one tap."
```

---

### Task 8: Fleet build, flash, and on-hardware tuning pass

**Files:** none (verification and constant-tuning only; any constant changes land as edits to files already modified in Tasks 3/4/5, listed below if touched).

This task cannot be scripted end-to-end — it requires watching real serial output on real hardware and reacting to it, the same way the existing 5 GHz excursion pacing (`COEX_5G_PER_EXCURSION`, `main/coexist.c`) was originally tuned by observing `ESP_ERR_NO_MEM`/257 errors and backing off. Follow these steps in order; do not skip the observation steps even if nothing looks obviously wrong.

- [ ] **Step 1: Build and flash all three targets**

```powershell
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Fleet -Do buildflash -Port COM12
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Fleet -Do flash -Port COM16
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c6 -Fleet -Do buildflash -Port COM13
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target cyd -Fleet -Do buildflash -Port COM20
```

(Enumerate actual ports first — `Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match '\(COM\d+\)' }` — they drift between sessions; do not assume the values above are current.)

- [ ] **Step 2: Trigger TURBO from the CYD and watch one decoy's serial output**

On the CYD: CONTROL → cycle to TURBO → tap SEND (arms) → tap SEND again within 3s (fires). Then:

```powershell
python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port COM12 --reset no --seconds 30 --grep "TURBO|decoy alive|burst ch|ESP_ERR|rc=-1|rc=257|Guru|panic|abort|Backtrace"
```

Watch specifically for:
- `TURBO: flooding at max (ble=32 wifi=16)` confirming activation.
- Any `ESP_ERR_NO_MEM`, `rc=257`, or nonzero `tx_rc=`/`set_ch_rc=` values in the `burst ch=...` lines — these indicate the Wi-Fi TX queue or channel-set calls are being pushed harder than the hardware sustains.
- Any crash/panic/backtrace — a hard stop, not a tuning signal; if this happens, the constants from Tasks 3–5 are too aggressive and must be raised before proceeding.

- [ ] **Step 3: If TX errors appeared, back off the relevant constant and re-verify**

- If BLE-side errors: raise `TURBO_LIFE_MIN_MS`/`TURBO_LIFE_MAX_MS` in `main/ble_devices.c`, or raise `COEX_TURBO_SLICE_MS` in `main/coexist.c` (the slice interval is more likely the actual cause than device life, per the mechanism explained in Task 2 — try that first).
- If Wi-Fi-side errors: the fire-every-tick change in Task 5 is the aggressive lever; if `esp_wifi_80211_tx` return codes turn nonzero under sustained turbo, add a minimum spacing between bursts specifically for turbo (a new `s_turbo_last_burst_ms` guard in `coexist.c`, analogous to the existing `vTaskDelay(pdMS_TO_TICKS(3))` drain between 5 GHz channels) rather than reverting to the persona's slow `wifi_period_ms`.
- After any change, rebuild+reflash that board and repeat Step 2 until a clean 30 s window shows no errors.

- [ ] **Step 4: Confirm the fleet-wide effect from the CYD's own status stream**

```powershell
python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port COM20 --reset no --seconds 30 --grep "status rx"
```

Expected: every reporting node shows `decoys=` at its hardware max (32 for the C5s, 24 for the C6) while turbo is active on all of them, with no dropped-frame warnings (`rx: dropped`).

- [ ] **Step 5: Confirm TURBO cleanly reverts**

On the CYD: CONTROL → cycle to NORMAL → tap SEND (single tap, no confirm needed for a non-TURBO preset). Re-run the Step 2 read for ~10s and confirm `TURBO: off, resuming normal population-match` appears and `decoy alive active=...` returns to a value consistent with the fleet-shared ceiling (not the hardware max), matching the session's established population-match behavior.

- [ ] **Step 6: Run the full host suite one final time**

```powershell
$r = "$env:USERPROFILE\Downloads\simulacra"
foreach ($d in @("tools\probe_audit\tests","tools\decoy_audit\tests","tools\pcap_learn\tests","tools\radar_audit\tests","tools\tests","web")) {
  python -m unittest discover -s (Join-Path $r $d)
}
```

Expected: `OK` for every suite (unaffected by anything in this task unless Step 3 changed a constant that also has host coverage from Tasks 3/4 — if so, confirm those specific tests still pass with the tuned values).

- [ ] **Step 7: Update the spec's Open Question with the final chosen values**

In `docs/superpowers/specs/2026-08-12-turbo-flood-mode-design.md`, replace the "Open question" section's content with the actual constants landed on and a one-line note of what, if anything, had to be backed off from the plan's starting values and why.

- [ ] **Step 8: Commit**

```bash
git add docs/superpowers/specs/2026-08-12-turbo-flood-mode-design.md
# plus any constant-tuning edits from Step 3, if made
git commit -m "feat(turbo): on-hardware tuning pass -- final churn/probe intervals

Verified on the full fleet (c5 x2, c6, cyd): TURBO activates fleet-wide
from CONTROL, all radios hit hardware max with no TX errors over a
sustained window, and cleanly reverts to fleet-shared population-match
on preset change. Spec's Open Question updated with final constants."
```

---

## Self-Review

**Spec coverage:** every section of `2026-08-12-turbo-flood-mode-design.md` maps to a task — architecture/orthogonal override → Tasks 1, 5; BLE behavior → Task 3; Wi-Fi behavior → Task 4; identifier uniqueness → explicitly a no-op, noted in the spec and unchanged by this plan; wire plumbing/activation → Tasks 1, 7; exit path → Task 5 (skip logic) + Task 6 (test) + Task 8 (Step 5 verification); testing → Tasks 3, 4, 6, 7 each carry their own; open question (tuning) → Task 8.

**Placeholder scan:** the two constant pairs (`TURBO_LIFE_MIN/MAX_MS`, `TURBO_MAC_ROT_MIN/MAX_MS`) and `COEX_TURBO_SLICE_MS` are real starting numbers with an explicit comment pointing at Task 8, not "TBD" — this matches the spec's own deliberate deferral and is not a plan-writing placeholder.

**Type/signature consistency, checked across tasks:**
- `sim_settings_t.turbo` (Task 1) is read by `sim_settings_apply` (Task 5) and `sim_settings_match_preset` (Task 1) — same field name throughout.
- `ble_devices_set_turbo(bool)` (Task 3) — called from `coexist_set_turbo` (Task 5) with a plain `bool`, no now_ms param, matching the header declared in Task 3.
- `probe_agents_set_turbo(bool)` (Task 4) — same pattern, called from Task 5.
- `churn_set_slice_ms(uint32_t)` (Task 2) — called from Task 5 with either `COEX_TURBO_SLICE_MS` or `CHURN_SLICE_MS`, both `uint32_t`-compatible literals.
- `coexist_set_turbo(bool)` (Task 5) — called from `sim_settings_apply` (Task 5, same task, same file diff) and from the CYD only indirectly (via the wire preset, never called directly by cyd/ code, which is correct — cyd/ has no build dependency on main/).
- `radar_ctrl_info_t.turbo_armed` (Task 7) — set in `cyd_main.c`, read in `radar_render.c`'s `draw_control`, and passed positionally in `render_dump.c`'s test harness — all three updated together in Task 7.
- `CTRL_TURBO_PRESET` (radar_render.c) and `CFG_PRESET_TURBO` (cyd_main.c) are two independently-named local constants both equal to `5`, deliberately not unified into a shared header — this matches the existing precedent in this codebase (CTRL_LABELS array position already encodes preset identity without a shared enum) and is called out explicitly in Task 7's Interfaces block rather than left implicit.

**Scope check:** eight tasks, each single-file-cluster and independently testable (six with host tests, one on-target self-test, one hardware-only). This is one coherent feature, not a multi-subsystem spec that needed decomposing at brainstorming time.
