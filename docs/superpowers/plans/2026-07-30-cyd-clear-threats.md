# CYD Clear-Threats Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A CYD CONTROL-page button (2-tap arm/confirm) that broadcasts a signed command telling every decoy to wipe its persisted threat table.

**Architecture:** Reuse the existing Ed25519-signed `RADAR_TYPE_CONFIG` path with a reserved `preset_id` sentinel `CONFIG_CLEAR_THREATS = 0xFF`. Decoy branches on it → `detect_clear_threats()`. CYD adds a CLEAR button with arm/confirm on CONTROL. No wire-version bump.

**Tech Stack:** C (ESP-IDF firmware; MSVC `cl` host tests), Python `unittest` (`render_dump`).

## Global Constraints

- No wire-version bump; reuse `send_config`/`config_wire` signed path. Sentinel `CONFIG_CLEAR_THREATS = 0xFF`.
- The CLEAR path lives under the same `SIMULACRA_CONFIG_CTRL` guard as the existing SEND path.
- Arm window = 3000 ms (mirrors the fleet REVOKE modal). Any other CONTROL action disarms.
- `draw_control` must not draw past `y=318`.

## File Structure

- `components/simulacra_radar/config_wire.h` — `CONFIG_CLEAR_THREATS` sentinel.
- `main/detect.h` — declare `detect_clear_threats`.
- `main/esp_now_link.c` — config-handler branch.
- `components/simulacra_radar/radar_render.h` — `radar_ctrl_info_t.clear_armed`.
- `components/simulacra_radar/radar_render.c` — `draw_control` CLEAR button.
- `tools/radar_audit/render_dump.c` — `--control` `clear_armed` arg.
- `tools/radar_audit/tests/test_control.py` — CLEAR assertions.
- `cyd/main/cyd_main.c` — `s_clear_arm_ms`, CONTROL touch, `ctrl.clear_armed`.
- `main/churn_selftest.c` — sentinel round-trip + clear check.

---

### Task 1: CONTROL CLEAR button render + sentinel constant

Adds the sentinel, the `clear_armed` field, and the CLEAR button (host-tested). No decoy/CYD wiring yet.

**Files:**
- Modify: `components/simulacra_radar/config_wire.h`, `radar_render.h`, `radar_render.c`, `tools/radar_audit/render_dump.c`
- Test: `tools/radar_audit/tests/test_control.py`

**Interfaces:**
- Produces: `CONFIG_CLEAR_THREATS` (0xFF); `radar_ctrl_info_t.clear_armed`; `draw_control` CLEAR button.

- [ ] **Step 1: Write the failing tests**

Append to `tools/radar_audit/tests/test_control.py` (and update the `control()` helper to pass `clear_armed`):

Replace the existing `control()` helper with:

```python
def control(sel=2, live=255, flash=0, clear_armed=0):
    args = [EXE, "--control", sel, live, flash, clear_armed]
    out = subprocess.check_output([str(x) for x in args], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]
```

Add a new test class at the end (before `if __name__`):

```python
@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class ControlClearThreats(unittest.TestCase):
    def test_clear_button_present(self):
        self.assertTrue(any("CLEAR THREATS" in t for t in control()),
                        "CLEAR THREATS button should render")

    def test_clear_confirm_when_armed(self):
        texts = control(clear_armed=1)
        self.assertTrue(any("CONFIRM CLEAR?" in t for t in texts),
                        "armed CLEAR should read CONFIRM CLEAR?")
        self.assertFalse(any("CLEAR THREATS" == t for t in texts),
                         "armed CLEAR should not also show the un-armed label")
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: build FAILS (`--control` takes 3 args / `clear_armed` field missing) or `ControlClearThreats` FAILS.

- [ ] **Step 3: Add the sentinel constant**

In `components/simulacra_radar/config_wire.h`, after `CONFIG_SIG_LEN`:

```c
#define CONFIG_CLEAR_THREATS 0xFF    // preset_id sentinel: wipe the decoy threat table (not a preset)
```

- [ ] **Step 4: Add `clear_armed` to `radar_ctrl_info_t`**

In `components/simulacra_radar/radar_render.h`:

```c
typedef struct { uint8_t sel_preset; bool send_flash; uint8_t live_preset; bool clear_armed; } radar_ctrl_info_t;   // CONTROL page state
```

- [ ] **Step 5: Draw the CLEAR button + move SEND up**

In `components/simulacra_radar/radar_render.c` `draw_control`, replace the SEND-button block and the footer with SEND (moved up) + CLEAR + a shorter hint. Change:

```c
    // SEND / SENT / ACTIVE
    bool active = c && (c->live_preset == c->sel_preset) && (c->live_preset < RADAR_CTRL_PRESET_COUNT);
    radar_gfx_fill_rect(g, 60, 210, 120, 40, COL_RING);      // SEND button
    const char *label = (c && c->send_flash) ? "SENT" : active ? "ACTIVE" : "SEND";
    uint16_t lc       = (c && c->send_flash) ? COL_OK  : active ? COL_DIM  : COL_FG;
    radar_gfx_text(g, 96, 224, label, lc);
    radar_gfx_text(g, 30, 296, "broadcast to all decoys", COL_DIM);
}
```

to:

```c
    // SEND / SENT / ACTIVE
    bool active = c && (c->live_preset == c->sel_preset) && (c->live_preset < RADAR_CTRL_PRESET_COUNT);
    radar_gfx_fill_rect(g, 60, 205, 120, 34, COL_RING);      // SEND button
    const char *slabel = (c && c->send_flash) ? "SENT" : active ? "ACTIVE" : "SEND";
    uint16_t slc       = (c && c->send_flash) ? COL_OK  : active ? COL_DIM  : COL_FG;
    radar_gfx_text(g, 96, 216, slabel, slc);
    // CLEAR THREATS button (2-tap arm/confirm; armed = red CONFIRM)
    bool armed = c && c->clear_armed;
    radar_gfx_fill_rect(g, 40, 252, 160, 30, armed ? COL_WARN : COL_CRYPT);
    const char *clabel = armed ? "CONFIRM CLEAR?" : "CLEAR THREATS";
    int cx = 120 - (int)strlen(clabel) * 8 / 2;
    radar_gfx_text(g, cx, 261, clabel, armed ? COL_FG : COL_ASH);
}
```

- [ ] **Step 6: Add `clear_armed` to the `--control` harness mode**

In `tools/radar_audit/render_dump.c`, in the `--control` block, parse a 4th arg and set the field:

```c
        int sel   = argc > a ? atoi(argv[a]) : 2; a++;
        int live  = argc > a ? atoi(argv[a]) : 0xFF; a++;
        int flash = argc > a ? atoi(argv[a]) : 0; a++;
        int carm  = argc > a ? atoi(argv[a]) : 0; a++;
        radar_wire_status_t st; memset(&st, 0, sizeof st);
        radar_ctrl_info_t ctrl; memset(&ctrl, 0, sizeof ctrl);
        ctrl.sel_preset = (uint8_t)sel; ctrl.live_preset = (uint8_t)live; ctrl.send_flash = flash != 0;
        ctrl.clear_armed = carm != 0;
```

Update the `--control` comment line to `<sel live flash clear_armed>`.

- [ ] **Step 7: Build + run tests to verify they pass**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: `ControlClearThreats` PASS; the pre-existing `ControlLivePending` tests still PASS (they call `control()` with the new default `clear_armed=0`).

- [ ] **Step 8: Commit**

```bash
git add components/simulacra_radar/config_wire.h components/simulacra_radar/radar_render.h components/simulacra_radar/radar_render.c tools/radar_audit/render_dump.c tools/radar_audit/tests/test_control.py
git commit -m "feat(clear-threats): CONFIG_CLEAR_THREATS sentinel + CONTROL CLEAR button render"
```

---

### Task 2: Decoy handler + CYD touch wiring + selftest

Wires the decoy to clear on the sentinel, the CYD CONTROL touch to arm/confirm/send, and adds the selftest. Firmware — compile-verified.

**Files:**
- Modify: `main/detect.h`, `main/esp_now_link.c`, `cyd/main/cyd_main.c`, `main/churn_selftest.c`

**Interfaces:**
- Consumes: `CONFIG_CLEAR_THREATS`; `detect_clear_threats`; `radar_ctrl_info_t.clear_armed`; `send_config`.

- [ ] **Step 1: Declare `detect_clear_threats`**

In `main/detect.h`, near `detect_reset`:

```c
void            detect_clear_threats(void);         // wipe confirmed threats (RAM + persisted NVS blob)
```

- [ ] **Step 2: Branch the decoy config handler on the sentinel**

In `main/esp_now_link.c`, replace:

```c
        if (cmd.version != CONFIG_WIRE_VER) return;
        if (sim_settings_apply_preset((sim_preset_t)cmd.preset_id) == 0)
            ESP_LOGW(ETAG, "config: applied preset %u", (unsigned)cmd.preset_id);
        return;
```

with:

```c
        if (cmd.version != CONFIG_WIRE_VER) return;
        if (cmd.preset_id == CONFIG_CLEAR_THREATS) {
            detect_clear_threats();
            ESP_LOGW(ETAG, "config: CLEAR THREATS");
        } else if (sim_settings_apply_preset((sim_preset_t)cmd.preset_id) == 0) {
            ESP_LOGW(ETAG, "config: applied preset %u", (unsigned)cmd.preset_id);
        }
        return;
```

Ensure `detect.h` and `config_wire.h` are included in `esp_now_link.c` (it already uses `detect_*` and `config_wire_open_signed`, so both are present).

- [ ] **Step 3: Add the CLEAR arm state + `clear_armed` to the CYD ctrl**

In `cyd/main/cyd_main.c`, near the other CONTROL/UI statics (e.g. beside `s_info_page`), add:

```c
static uint32_t s_clear_arm_ms;   // CONTROL: CLEAR THREATS armed-at (0 = disarmed); 3s confirm window
```

Where `ctrl` is built (the `radar_ctrl_info_t ctrl = { ... .live_preset = agg.preset };` from sub-project D), add the armed flag:

```c
            radar_ctrl_info_t ctrl = { .sel_preset = ui.sel_preset,
                .send_flash = (ui.send_flash_ms && (now - ui.send_flash_ms) < RADAR_CTRL_FLASH_MS),
                .live_preset = agg.preset,
                .clear_armed = (s_clear_arm_ms && (uint32_t)(now - s_clear_arm_ms) < 3000) };
```

- [ ] **Step 4: Wire the CONTROL touch (arm/confirm + disarm)**

In the `RADAR_VIEW_CONTROL` touch handler (the `#ifdef SIMULACRA_CONFIG_CTRL` block), add the CLEAR band and disarm the other actions. Replace:

```c
                if (ty < 40) {                           // top strip = BACK to HOME (drawn "< BACK")
                    radar_ui_on_input(&ui, now);
                } else if (ty > 200 && tx > 60 && tx < 180) {   // SEND button
                    send_config(ui.sel_preset);
                    radar_ctrl_mark_sent(&ui, now);
                } else if (tx < 80) {                    // left zone: prev == cycle-around
                    for (int i = 0; i < RADAR_CTRL_PRESET_COUNT - 1; i++) radar_ctrl_select_next(&ui);
                } else if (tx > 160) {                   // right zone: next
                    radar_ctrl_select_next(&ui);
                } else {                                 // center (preset label) = stay put
                    radar_ui_note_input(&ui, now);
                }
```

with:

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
                }
```

`CONFIG_CLEAR_THREATS` comes from `config_wire.h`, already included in `cyd_main.c`.

- [ ] **Step 5: Add the on-device selftest**

In `main/churn_selftest.c` `test_config_wire()` (which already builds a signed `config_cmd_t` with `preset_id=3`), add a sentinel round-trip after the existing open check, and a clear-threats RAM check. Append inside `test_config_wire` after the existing asserts:

```c
    // Clear-threats sentinel rides the signed config path intact.
    config_cmd_t clr = { .version = CONFIG_WIRE_VER, .preset_id = CONFIG_CLEAR_THREATS };
    uint8_t cpl[CONFIG_WIRE_PAYLOAD_LEN];
    ST_CHECK(config_wire_pack_signed(cpl, sizeof cpl, &clr, nonce, sk) > 0, "pack clear sentinel");
    config_cmd_t cgot;
    ST_CHECK(config_wire_open_signed(cpl, sizeof cpl, nonce, pk, &cgot) == 0, "open clear sentinel");
    ST_CHECK(cgot.preset_id == CONFIG_CLEAR_THREATS, "clear sentinel carried through signed path");
```

In the settings/detect test area (near the existing `detect_reset()` usage), add a clear-threats effect check:

```c
    detect_reset();
    detect_note_known(0xC0FFEE, -50, 3, 1, 90, 1);      // record a known (Flock) threat
    detect_clear_threats();
    ST_CHECK(detect_threat_count() == 0, "detect_clear_threats empties the table");
```

(Place this where `detect_note_known`/`detect_threat_count` are already in scope; both are used elsewhere in the selftest.)

- [ ] **Step 6: Compile-verify the c6 decoy**

Root is on c6/5.4 (from the last flash). Build it — validates the decoy handler + selftest + config_wire:

```powershell
$py='C:\Program Files\Python312'; $env:PATH="$py;$py\Scripts;$env:PATH"
$idf='$env:USERPROFILE\esp\v5.4\esp-idf'; $env:IDF_PATH=$idf; . ($idf+'\export.ps1') *>$null
idf.py build
```

Expected: `Project build complete` (esp_now_link.c + churn_selftest.c compile with `detect_clear_threats` + the sentinel).

- [ ] **Step 7: Compile-verify the CYD**

```powershell
$py='C:\Program Files\Python312'; $env:PATH="$py;$py\Scripts;$env:PATH"
$idf='$env:USERPROFILE\esp\v5.4\esp-idf'; $env:IDF_PATH=$idf; . ($idf+'\export.ps1') *>$null
idf.py -C cyd build
```

Expected: `Project build complete` (cyd_main.c CONTROL touch + draw_control build).

- [ ] **Step 8: Re-run the host suite**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: all suites PASS.

- [ ] **Step 9: Commit**

```bash
git add main/detect.h main/esp_now_link.c cyd/main/cyd_main.c main/churn_selftest.c
git commit -m "feat(clear-threats): decoy clears on sentinel; CYD CONTROL arm/confirm + send"
```

---

## Self-Review

**Spec coverage:**
- `CONFIG_CLEAR_THREATS` sentinel + reuse signed path → Task 1 Step 3, Task 2 Step 2. ✓
- Decoy `detect_clear_threats()` branch → Task 2 Steps 1–2. ✓
- `send_config(CONFIG_CLEAR_THREATS)` from CYD → Task 2 Step 4. ✓
- `clear_armed` field + CLEAR button + CONFIRM state → Task 1 Steps 4–5. ✓
- CONTROL touch arm/confirm (3 s) + disarm on other actions → Task 2 Step 4. ✓
- Testing: host render + on-device sentinel round-trip + clear effect → Tasks 1–2. ✓
- Compile-verify c6/cyd → Task 2 Steps 6–7. ✓
- Rollout reflash (operator-present) — not automated in the plan. ✓

**Placeholder scan:** none — every code step shows complete code.

**Type consistency:** `radar_ctrl_info_t.clear_armed` (bool) added Task 1 Step 4, read by `draw_control` (Task 1 Step 5), written by cyd_main (Task 2 Step 3), fed by harness (Task 1 Step 6). `CONFIG_CLEAR_THREATS`/`detect_clear_threats`/`send_config` names match their definitions. `--control` arg order (sel, live, flash, clear_armed) matches the `control()` helper. Touch band `ty>=246` for CLEAR vs SEND `ty>200 && ty<246` (SEND checked after CLEAR, so its effective band is 200–245).

**Fit check:** SEND 205–239, CLEAR 252–282 (label at 261, ends ~269) < 318. ✓

## Rollout

Full-fleet reflash from the flood branch (c5×2, c6, cyd), operator-present. No wire-version bump.
