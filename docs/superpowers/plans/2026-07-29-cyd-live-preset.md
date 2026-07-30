# CYD Live Preset (wire v2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decoys report the preset they are actually running; CONTROL shows live-vs-pending + descriptions; wire bumps to v2.

**Architecture:** One decoy-side change. Append `uint8_t preset` to `radar_wire_status_t` and bump `RADAR_WIRE_VER`; the decoy infers its preset from current settings and packs it; `fleet_status_aggregate` folds it to a single value / MIXED / none; `draw_control` renders LIVE vs PENDING. Cross-version frames are rejected at `open()`, so the rollout reflashes the whole fleet.

**Tech Stack:** C (ESP-IDF firmware; MSVC `cl` host tests), Python `unittest` (`render_dump`, `fleet_dump`).

## Global Constraints

- Preset id only — no other new telemetry this bump.
- Preset value encoding everywhere: `0..4` = PAUSE/STEALTH/NORMAL/DENSE/MAX (`sim_preset_t`), `5` = CUSTOM (`SIM_PRESET_COUNT`), `0xFE` = MIXED, `0xFF` = none.
- `RADAR_WIRE_VER` becomes `2`; `radar_wire_open` already rejects mismatched versions — no dual-version parsing.
- Reuse `CTRL_LABELS[5]` for preset names; add `PRESET_DESC[5] = {"freeze on-air","min crowd","balanced","big crowd","max crowd"}`.
- `draw_control` must not draw past `y=318`.
- Decoy infers preset (no new NVS state) via a pure match against resolved presets; low-ceiling ties resolve to the lowest-id matching preset.

## File Structure

- `components/simulacra_radar/radar_wire.h` — `preset` field + `RADAR_WIRE_VER` 2.
- `cyd/main/fleet_status.c` — aggregate `out->preset`.
- `tools/radar_audit/fleet_dump.c` — `upp` + `aggp` commands.
- `tools/radar_audit/tests/test_fleet_status.py` — aggregate-preset tests.
- `components/simulacra_radar/radar_render.h` — `radar_ctrl_info_t.live_preset`.
- `components/simulacra_radar/radar_render.c` — `draw_control` live/pending/desc + `ctrl_preset_name`.
- `tools/radar_audit/render_dump.c` — `--control` mode.
- `tools/radar_audit/tests/test_control.py` — CONTROL render tests.
- `main/settings.c` / `settings.h` — `sim_settings_match_preset` + `sim_settings_current_preset`.
- `main/esp_now_link.c` — set `r.preset`.
- `cyd/main/cyd_main.c` — `ctrl.live_preset = agg.preset`.
- `main/churn_selftest.c` — inference selftest.

---

### Task 1: Wire v2 field + fleet-preset aggregate

Adds the `preset` wire field, bumps the version, and folds preset across the fleet. Host-tested via `fleet_dump`.

**Files:**
- Modify: `components/simulacra_radar/radar_wire.h`, `cyd/main/fleet_status.c`, `tools/radar_audit/fleet_dump.c`
- Test: `tools/radar_audit/tests/test_fleet_status.py`

**Interfaces:**
- Produces: `radar_wire_status_t.preset`; `RADAR_WIRE_VER == 2`; `fleet_status_aggregate` sets `out->preset` (0–4 / 5 / 0xFE / 0xFF).

- [ ] **Step 1: Write the failing tests**

Append to `tools/radar_audit/tests/test_fleet_status.py`:

```python
    def test_aggregate_preset_all_agree(self):
        # two alive nodes both preset 4 (MAX) -> fleet preset 4
        self.assertEqual(run("upp 0 8 4 upp 1 8 4 aggp"), ["preset=4"])

    def test_aggregate_preset_mixed(self):
        # differing presets -> MIXED (0xFE = 254)
        self.assertEqual(run("upp 0 8 4 upp 1 8 2 aggp"), ["preset=254"])

    def test_aggregate_preset_none_when_empty(self):
        # no alive nodes -> none (0xFF = 255)
        self.assertEqual(run("aggp"), ["preset=255"])

    def test_aggregate_preset_excludes_stale(self):
        # a stale node's preset does not count; only the alive node (preset 3) remains
        self.assertEqual(run("upp 0 8 4 wait upp 1 8 3 aggp"), ["preset=3"])
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: build FAILS (`upp`/`aggp` unknown → no output / `preset=` missing) — the new `fleet_status` tests FAIL.

- [ ] **Step 3: Add the wire field + version bump**

In `components/simulacra_radar/radar_wire.h`:

```c
#define RADAR_WIRE_VER 2
```

Append `preset` to the status struct (after `battery_pct`):

```c
    uint8_t  battery_pct;                                // state-of-charge %, 0xFF = unavailable (ADC backend)
    uint8_t  preset;                                     // running preset: 0-4 sim_preset_t, 5 CUSTOM, 0xFE MIXED, 0xFF none
} radar_wire_status_t;
```

- [ ] **Step 4: Aggregate the fleet preset**

In `cyd/main/fleet_status.c` `fleet_status_aggregate`, initialize a tracker before the loop and finalize after. Add, immediately after `memset(out, 0, sizeof(*out));`:

```c
    int agg_preset = -1;   // -1 = no alive node yet
```

Inside the loop, after the `if (... >= FLEET_STATUS_STALE_MS) continue;` guard (i.e. for each ALIVE node `st`), add:

```c
        if (agg_preset == -1)               agg_preset = st->preset;   // first alive node
        else if (agg_preset != st->preset)  agg_preset = 0xFE;         // disagreement -> MIXED
```

After the loop, before the function returns:

```c
    out->preset = (agg_preset == -1) ? 0xFF : (uint8_t)agg_preset;
```

(Once `agg_preset` is `0xFE` a further differing node keeps it `0xFE`; a matching node also keeps it — `0xFE != st->preset` stays MIXED, correct.)

- [ ] **Step 5: Add `upp` + `aggp` to `fleet_dump.c`**

In `tools/radar_audit/fleet_dump.c`, add two command branches inside the arg loop (alongside `up`/`agg`):

```c
        else if(!strcmp(argv[i],"upp")){ uint8_t id=atoi(argv[++i]); radar_wire_status_t s; memset(&s,0,sizeof s);
            s.active_devices=atoi(argv[++i]); s.preset=(uint8_t)atoi(argv[++i]); fleet_status_upsert(&f,id,&s,t); }
        else if(!strcmp(argv[i],"aggp")){ radar_wire_status_t a; fleet_status_aggregate(&f,t,&a);
            printf("preset=%u\n",(unsigned)a.preset); }
```

- [ ] **Step 6: Build + run tests to verify they pass**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: build succeeds; the four new `test_aggregate_preset_*` PASS; all pre-existing tests still PASS.

- [ ] **Step 7: Commit**

```bash
git add components/simulacra_radar/radar_wire.h cyd/main/fleet_status.c tools/radar_audit/fleet_dump.c tools/radar_audit/tests/test_fleet_status.py
git commit -m "feat(cyd-preset): wire v2 preset field + fleet-preset aggregate (agree/MIXED/none)"
```

---

### Task 2: CONTROL live-vs-pending render

**Files:**
- Modify: `components/simulacra_radar/radar_render.h`, `radar_render.c`, `tools/radar_audit/render_dump.c`
- Test: `tools/radar_audit/tests/test_control.py`

**Interfaces:**
- Consumes: `radar_wire_status_t.preset` (via `radar_ctrl_info_t.live_preset`).
- Produces: `radar_ctrl_info_t.live_preset`; `draw_control` LIVE/PENDING/description; `ctrl_preset_name`.

- [ ] **Step 1: Write the failing tests**

Create `tools/radar_audit/tests/test_control.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")


def control(sel=2, live=255, flash=0):
    args = [EXE, "--control", sel, live, flash]
    out = subprocess.check_output([str(x) for x in args], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class ControlLivePending(unittest.TestCase):
    def test_live_and_pending_labels(self):
        texts = control(sel=2, live=4)
        self.assertTrue(any("LIVE" in t for t in texts), f"drew: {texts}")
        self.assertTrue(any("PENDING" in t for t in texts), f"drew: {texts}")

    def test_live_name_and_pending_box(self):
        texts = control(sel=2, live=4)          # live MAX, pending NORMAL
        self.assertTrue(any("MAX" in t for t in texts), f"live name; drew: {texts}")
        self.assertTrue(any("NORMAL" in t for t in texts), f"pending box; drew: {texts}")
        self.assertIn("balanced", texts, f"selected-preset desc; drew: {texts}")

    def test_send_when_pending_differs(self):
        self.assertIn("SEND", control(sel=2, live=4), "should read SEND when live!=pending")

    def test_active_when_live_equals_pending(self):
        self.assertIn("ACTIVE", control(sel=4, live=4), "should read ACTIVE when live==pending")

    def test_mixed_live(self):
        self.assertTrue(any("MIXED" in t for t in control(sel=2, live=254)),
                        "0xFE should render MIXED")

    def test_none_live(self):
        # 0xFF none -> em-dash-ish '-' marker present, and NOT ACTIVE
        texts = control(sel=2, live=255)
        self.assertNotIn("ACTIVE", texts, f"none must not be ACTIVE; drew: {texts}")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: build FAILS (`--control` unknown) or `ControlLivePending` FAILS.

- [ ] **Step 3: Add `live_preset` to `radar_ctrl_info_t`**

In `components/simulacra_radar/radar_render.h`:

```c
typedef struct { uint8_t sel_preset; bool send_flash; uint8_t live_preset; } radar_ctrl_info_t;   // CONTROL page state
```

- [ ] **Step 4: Rewrite `draw_control`**

In `components/simulacra_radar/radar_render.c`, replace `draw_control` (and keep `CTRL_LABELS`, add `PRESET_DESC` + `ctrl_preset_name` just above it):

```c
static const char *CTRL_LABELS[5] = { "PAUSE", "STEALTH", "NORMAL", "DENSE", "MAX" };
static const char *PRESET_DESC[5] = { "freeze on-air", "min crowd", "balanced", "big crowd", "max crowd" };
static const char *ctrl_preset_name(uint8_t p){
    if (p < 5)      return CTRL_LABELS[p];
    if (p == 5)     return "CUSTOM";
    if (p == 0xFE)  return "MIXED";
    return "-";                       // 0xFF none
}
static void draw_control(radar_gfx_t *g, const radar_ctrl_info_t *c){
    radar_gfx_text(g, 8, 6, "< BACK", COL_ARCANE);
    radar_gfx_text(g, 152, 6, "CONTROL", COL_ASH);
    uint8_t sel  = c ? c->sel_preset : 2;
    uint8_t live = c ? c->live_preset : 0xFF;
    // LIVE (what the fleet is actually running)
    radar_gfx_text(g, 20, 56, "LIVE", COL_ASH);
    radar_gfx_text(g, 96, 56, ctrl_preset_name(live), live == 0xFE ? COL_WARN : COL_FG);
    // PENDING (what SEND will apply)
    radar_gfx_text(g, 20, 96, "PENDING", COL_ASH);
    radar_gfx_text(g, 20, 120, "<", COL_DIM);
    radar_gfx_text(g, 200, 120, ">", COL_DIM);
    char box[16]; snprintf(box, sizeof box, "[ %s ]", CTRL_LABELS[sel % 5]);
    radar_gfx_text(g, 70, 120, box, COL_FG);
    radar_gfx_text(g, 8, 152, PRESET_DESC[sel % 5], COL_DIM);
    // SEND / SENT / ACTIVE
    bool active = c && (c->live_preset == c->sel_preset) && (c->live_preset <= 4);
    radar_gfx_fill_rect(g, 60, 210, 120, 40, COL_RING);
    const char *label = (c && c->send_flash) ? "SENT" : active ? "ACTIVE" : "SEND";
    uint16_t lc = (c && c->send_flash) ? COL_OK : active ? COL_DIM : COL_FG;
    radar_gfx_text(g, 96, 224, label, lc);
    radar_gfx_text(g, 30, 296, "broadcast to all decoys", COL_DIM);
}
```

- [ ] **Step 5: Add the `--control` harness mode in `render_dump.c`**

Add after the `--info` block (before `int view = ...`):

```c
    if (argc > 1 && strcmp(argv[1], "--control") == 0) {
        int a = 2;
        int sel   = argc > a ? atoi(argv[a]) : 2; a++;
        int live  = argc > a ? atoi(argv[a]) : 0xFF; a++;
        int flash = argc > a ? atoi(argv[a]) : 0; a++;
        radar_wire_status_t st; memset(&st, 0, sizeof st);
        radar_ctrl_info_t ctrl; memset(&ctrl, 0, sizeof ctrl);
        ctrl.sel_preset = (uint8_t)sel; ctrl.live_preset = (uint8_t)live; ctrl.send_flash = flash != 0;
        static uint16_t cband[240 * 320];
        radar_render_view(RADAR_VIEW_CONTROL, &st, 0, 0, -1, -1, 0, &ctrl, NULL, NULL, 0,
                          cband, 320, 240, 320, flush_noop, 0);
        return 0;
    }
```

Update the header comment to note `--control <sel live flash>`.

- [ ] **Step 6: Build + run tests to verify they pass**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: `ControlLivePending` all PASS; pre-existing suites still PASS.

- [ ] **Step 7: Commit**

```bash
git add components/simulacra_radar/radar_render.h components/simulacra_radar/radar_render.c tools/radar_audit/render_dump.c tools/radar_audit/tests/test_control.py
git commit -m "feat(cyd-preset): CONTROL live-vs-pending render + preset descriptions"
```

---

### Task 3: Decoy inference + report + selftest + firmware wiring

Wires the decoy to infer & pack its preset, feeds `agg.preset` to CONTROL on the CYD, adds the on-device selftest, and compile-verifies all targets.

**Files:**
- Modify: `main/settings.c`, `main/settings.h`, `main/esp_now_link.c`, `main/churn_selftest.c`, `cyd/main/cyd_main.c`

**Interfaces:**
- Consumes: `sim_settings_resolve`, `s_cur` (settings.c); `radar_wire_status_t.preset`; `radar_ctrl_info_t.live_preset`.
- Produces: `sim_settings_match_preset`, `sim_settings_current_preset`.

- [ ] **Step 1: Declare the inference API**

In `main/settings.h`, after `sim_settings_get`:

```c
// Pure: which preset (against `ceiling`) resolves to exactly *cur? SIM_PRESET_COUNT if none (CUSTOM).
sim_preset_t sim_settings_match_preset(const sim_settings_t *cur, uint8_t ceiling);
// The preset the engine is currently running (inferred from live settings). SIM_PRESET_COUNT = CUSTOM.
sim_preset_t sim_settings_current_preset(void);
```

- [ ] **Step 2: Implement the inference**

In `main/settings.c` (after `sim_settings_get`; `s_cur` is the file-static current settings):

```c
sim_preset_t sim_settings_match_preset(const sim_settings_t *cur, uint8_t ceiling)
{
    for (sim_preset_t p = SIM_PRESET_PAUSE; p < SIM_PRESET_COUNT; p++) {
        sim_settings_t r;
        if (sim_settings_resolve(p, ceiling, &r) != 0) continue;
        if (r.active_target == cur->active_target && r.paused == cur->paused &&
            r.accel == cur->accel &&
            r.dwell_min_ms == cur->dwell_min_ms && r.dwell_max_ms == cur->dwell_max_ms &&
            r.cooldown_min_ms == cur->cooldown_min_ms && r.cooldown_max_ms == cur->cooldown_max_ms)
            return p;
    }
    return SIM_PRESET_COUNT;   // CUSTOM
}
sim_preset_t sim_settings_current_preset(void)
{
    return sim_settings_match_preset(&s_cur, CHURN_ACTIVE_SET);
}
```

(If `s_cur` is not already the identifier for the file-static settings in `settings.c`, use whatever `sim_settings_get` copies from — grep `sim_settings_get` shows `*out = s_cur;`, so `s_cur` is correct.)

- [ ] **Step 3: Report the preset in the decoy status**

In `main/esp_now_link.c` `respond_once()`, immediately after `radar_wire_status_t r; espnow_status_from_webui(&r, &w);`:

```c
    r.preset = (uint8_t)sim_settings_current_preset();
```

Ensure `settings.h` is included in `esp_now_link.c` (it uses `sim_settings_*` elsewhere — confirm the include is present; add `#include "settings.h"` if missing).

- [ ] **Step 4: Feed the live preset to CONTROL on the CYD**

In `cyd/main/cyd_main.c`, where `ctrl` is built (`radar_ctrl_info_t ctrl = { .sel_preset = ui.sel_preset, .send_flash = ... };`), add the live preset from the aggregate:

```c
            radar_ctrl_info_t ctrl = { .sel_preset = ui.sel_preset,
                .send_flash = (ui.send_flash_ms && (now - ui.send_flash_ms) < RADAR_CTRL_FLASH_MS),
                .live_preset = agg.preset };
```

- [ ] **Step 5: Add the on-device inference selftest**

In `main/churn_selftest.c`, near the existing settings checks (around the `sim_settings_apply_preset` tests), add:

```c
    sim_settings_apply_preset(SIM_PRESET_MAX);
    ST_CHECK(sim_settings_current_preset() == SIM_PRESET_MAX, "current_preset reports MAX after apply");
    sim_settings_apply_preset(SIM_PRESET_STEALTH);
    ST_CHECK(sim_settings_current_preset() == SIM_PRESET_STEALTH, "current_preset reports STEALTH after apply");
    {
        sim_settings_t custom; sim_settings_get(&custom);
        custom.dwell_min_ms += 12345;                 // a value no preset resolves to
        sim_settings_set(&custom);
        ST_CHECK(sim_settings_current_preset() == SIM_PRESET_COUNT, "granular settings report CUSTOM");
        sim_settings_apply_preset(SIM_PRESET_NORMAL);  // restore
    }
```

(Place inside the settings test function that already calls `sim_settings_apply_preset`; `ST_CHECK` and the helpers are in scope there.)

- [ ] **Step 6: Compile-verify the c5 decoy**

The root workspace target/IDF must match. If the root build dir is on c6/5.4 (from a prior flash), either build c6 (5.4) or retarget to c5. To verify the decoy path that packs `r.preset`, build **c6** (IDF 5.4) which the root is currently configured for:

```powershell
$py='C:\Program Files\Python312'; $env:PATH="$py;$py\Scripts;$env:PATH"
$idf='$env:USERPROFILE\esp\v5.4\esp-idf'; $env:IDF_PATH=$idf; . ($idf+'\export.ps1') *>$null
idf.py build
```

Expected: `Project build complete` (settings.c/esp_now_link.c compile with the new symbols; `radar_wire.h` VER=2 + field build across the component).

If the root is on c5/5.5 instead, run the same with the 5.5 env (`idf.py build`); either decoy target validates the decoy-side code.

- [ ] **Step 7: Compile-verify the CYD**

```powershell
$py='C:\Program Files\Python312'; $env:PATH="$py;$py\Scripts;$env:PATH"
$idf='$env:USERPROFILE\esp\v5.4\esp-idf'; $env:IDF_PATH=$idf; . ($idf+'\export.ps1') *>$null
idf.py -C cyd build
```

Expected: `Project build complete`, `cyd/build/simulacra_cyd.bin` regenerated (fleet_status aggregate + cyd_main ctrl.live_preset + draw_control build).

- [ ] **Step 8: Re-run the host suite**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: all suites PASS (test_control, test_fleet_status incl. the new preset cases, plus A/B/C suites).

- [ ] **Step 9: Commit**

```bash
git add main/settings.c main/settings.h main/esp_now_link.c main/churn_selftest.c cyd/main/cyd_main.c
git commit -m "feat(cyd-preset): decoy infers+reports running preset; CYD feeds it to CONTROL"
```

---

## Self-Review

**Spec coverage:**
- Wire `preset` field + `RADAR_WIRE_VER` 2 → Task 1 Step 3. ✓
- Infer-don't-store (`sim_settings_match_preset`/`current_preset`) → Task 3 Steps 1–2. ✓
- Report in `respond_once` → Task 3 Step 3. ✓
- Aggregate agree/MIXED/none → Task 1 Step 4 + tests. ✓
- `radar_ctrl_info_t.live_preset` + cyd_main fill → Task 2 Step 3, Task 3 Step 4. ✓
- CONTROL LIVE/PENDING/description/ACTIVE → Task 2 Step 4 + tests. ✓
- Preset encoding (0–4/5/0xFE/0xFF) consistent → wire comment, `ctrl_preset_name`, aggregate, tests. ✓
- Back-compat via ver reject — no code needed (documented). ✓
- Testing: host CONTROL + aggregate; on-device inference selftest → Tasks 1–3. ✓
- Compile-verify c5/c6 + cyd → Task 3 Steps 6–7. ✓
- Rollout reflash — deliberate operator-present step (not in this plan's automation). ✓

**Placeholder scan:** none — every code step shows complete code.

**Type consistency:** `radar_ctrl_info_t` gains `uint8_t live_preset` (Task 2 Step 3), read by `draw_control` (Task 2 Step 4), written by cyd_main (Task 3 Step 4). `sim_settings_match_preset(const sim_settings_t*, uint8_t)` / `sim_settings_current_preset(void)` identical in `.h`/`.c`/selftest. `radar_wire_status_t.preset` written by decoy (Task 3 Step 3), aggregated (Task 1 Step 4), surfaced via `live_preset`. Encoding constants (`0xFE`/`0xFF`/`5`) match across aggregate, `ctrl_preset_name`, and tests (`preset=254`/`preset=255`, `MIXED`). `--control` arg order (sel, live, flash) matches the `control()` helper.

**Fit check:** `draw_control` last element footer y=296 (ends ~304 < 318); SEND button 210–250; description y=152. ✓

## Rollout

Wire v2 → **coordinated full-fleet reflash** (c5×2 COM12/COM16, c6 COM13, cyd COM10), all in one session, plus a flood-branch reflash for DEFCON. A v2 CYD renders un-reflashed (v1) decoys as SILENT, so partial flashing is not valid. This step is performed with the operator present (COM10 has shown port-lock behavior needing a physical replug).
