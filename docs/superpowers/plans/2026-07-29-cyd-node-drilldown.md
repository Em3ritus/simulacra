# CYD Per-Node Drill-Down (NODE console) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `RADAR_VIEW_NODE` view so tapping a HOME node card drills into a per-node telemetry console (that node's decoy/power/system/detection status).

**Architecture:** Pure CYD-render feature. The per-node data is already in `s_fleet` (the `fleet_status` table). Add one renderer (`draw_node`) fed by a selected index into the existing `radar_node_view_t nodes[]` array, extend `radar_render_view` with an `int sel_node` param, and wire HOME-card taps + in-page prev/next in `cyd_main.c`. No decoy firmware change, no wire-protocol bump, no fleet reflash.

**Tech Stack:** C (ESP-IDF for firmware; MSVC `cl` for host render tests), Python `unittest` host harness (`tools/radar_audit/render_dump`).

## Global Constraints

- CYD-render-only. Do **not** modify `radar_wire.h` wire struct, wire version, or any decoy-side code. (Wire changes are sub-project D.)
- Reuse existing render primitives: `draw_header`, `row_section`, `row_kv`, `fmt_uptime`, `is_surveil_cat` (all already in `radar_render.c`).
- Battery value strings must match `draw_home`'s formatting exactly: pct present → `"%u%% %u.%01uV"`; voltage-only → `"%u.%02uV"`; `battery_mv==0` → `"USB"`.
- Health decode must match `draw_home`: flag bit3 (`0x08`) = LOW BATT, bit2 (`0x04`) = DEGRADED, `!alive` = SILENT, else CHANNEL; battery wins over degraded.
- `draw_node` must not draw past `y=318` (240×320 panel, 8px font).
- `radar_render_view` `sel_node` is an index into `nodes[]`; `-1` = none (every non-NODE view ignores it).
- Node id label is `N<id>` (matches HOME).

---

## File Structure

- `components/simulacra_radar/radar_ui.h` — add `RADAR_VIEW_NODE` enum value (before `RADAR_VIEW_COUNT`).
- `components/simulacra_radar/radar_render.h` — add `uint32_t age_s` to `radar_node_view_t`; add `int sel_node` to `radar_render_view` signature.
- `components/simulacra_radar/radar_render.c` — new `draw_node`; dispatch + `sel_node` plumb.
- `tools/radar_audit/render_dump.c` — new `--node` harness mode; update the 2 existing `radar_render_view` calls + the `nodes[1]` initializer for the new param/field.
- `tools/radar_audit/tests/test_node_view.py` — new host tests.
- `cyd/main/fleet_status.h` / `fleet_status.c` — add `fleet_status_age_ms` accessor.
- `cyd/main/cyd_main.c` — `s_sel_node`/`s_home_ids[]`, card-tap routing, NODE in-page nav, freshness-overlay skip, `age_s` population, `sel_idx` resolution, pass `sel_idx` to the 4 `radar_render_view` calls.

---

### Task 1: NODE view scaffolding (enum, signature, struct field, header + NODE-GONE placeholder)

Delivers a reachable-from-harness NODE view that renders a titled header for a valid node and a `NODE GONE` placeholder for `sel_node < 0`. No body yet.

**Files:**
- Modify: `components/simulacra_radar/radar_ui.h`
- Modify: `components/simulacra_radar/radar_render.h`
- Modify: `components/simulacra_radar/radar_render.c`
- Modify: `tools/radar_audit/render_dump.c`
- Modify: `cyd/main/cyd_main.c` (mechanical: add the new arg to the 4 call sites)
- Test: `tools/radar_audit/tests/test_node_view.py`

**Interfaces:**
- Produces: `RADAR_VIEW_NODE` (enum); `radar_node_view_t` now `{ uint8_t id; const radar_wire_status_t *st; bool alive; uint32_t age_s; }`; `radar_render_view(..., int node_count, int sel_node, const radar_lib_info_t *lib, ...)`; `static void draw_node(radar_gfx_t *g, const radar_node_view_t *nodes, int node_count, int sel)`.

- [ ] **Step 1: Write the failing test**

Create `tools/radar_audit/tests/test_node_view.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")


def node(sel=0, id=2, alive=1, age=0, active=8, target=8, roster=16,
         rpa=3, nrpa=2, static=3, pop=10, batt_mv=0, batt_pct=255,
         epoch=5, probes=100, flags=0, uptime=3600, threats=0, ncam=0):
    """Render RADAR_VIEW_NODE and return the list of text strings drawn."""
    args = [EXE, "--node", sel, id, alive, age, active, target, roster,
            rpa, nrpa, static, pop, batt_mv, batt_pct, epoch, probes,
            flags, uptime, threats, ncam]
    out = subprocess.check_output([str(x) for x in args], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class NodeScaffold(unittest.TestCase):
    def test_header_shows_node_id(self):
        texts = node(id=2)
        self.assertIn("NODE N2", texts, f"no node header; drew: {texts}")

    def test_sel_negative_shows_gone_placeholder(self):
        texts = node(sel=-1)
        self.assertIn("NODE GONE", texts, f"no gone placeholder; drew: {texts}")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: build FAILS (no `--node` mode / `RADAR_VIEW_NODE` undefined) or tests FAIL — the harness doesn't render NODE yet.

- [ ] **Step 3: Add the enum value**

In `components/simulacra_radar/radar_ui.h`, add `RADAR_VIEW_NODE` before `RADAR_VIEW_COUNT`:

```c
typedef enum { RADAR_VIEW_HOME = 0, RADAR_VIEW_RADAR, RADAR_VIEW_DETAIL, RADAR_VIEW_STATS,
               RADAR_VIEW_LIBRARY, RADAR_VIEW_CONTROL, RADAR_VIEW_INFO, RADAR_VIEW_EXPOSURE,
               RADAR_VIEW_NODE,
               RADAR_VIEW_COUNT } radar_view_t;
```

- [ ] **Step 4: Extend the struct + signature in the header**

In `components/simulacra_radar/radar_render.h`, add `age_s` to the node view and `sel_node` to the signature:

```c
// Per-node fleet view for the HOME strip: id + a pointer to that node's last status + liveness + age.
typedef struct { uint8_t id; const radar_wire_status_t *st; bool alive; uint32_t age_s; } radar_node_view_t;
```

```c
void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                       const radar_node_view_t *nodes, int node_count, int sel_node,
                       const radar_lib_info_t *lib, const radar_ctrl_info_t *ctrl,
                       const exposure_t *expo, uint16_t sweep_deg,
                       uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx);
```

- [ ] **Step 5: Add `draw_node` (header + placeholder only) and dispatch in `radar_render.c`**

Add `draw_node` immediately after `draw_info` (so `draw_header` is already defined):

```c
static void draw_node(radar_gfx_t *g, const radar_node_view_t *nodes, int node_count, int sel){
    if (sel < 0 || sel >= node_count) {
        draw_header(g, "NODE");
        radar_gfx_text(g, 72, 150, "NODE GONE", COL_ASH);
        return;
    }
    char title[12]; snprintf(title, sizeof title, "NODE N%u", (unsigned)nodes[sel].id);
    draw_header(g, title);
}
```

Update `radar_render_view`'s signature line and add the dispatch arm:

```c
void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                       const radar_node_view_t *nodes, int node_count, int sel_node,
                       const radar_lib_info_t *lib, const radar_ctrl_info_t *ctrl,
                       const exposure_t *expo, uint16_t sweep, uint16_t *band, int band_h, int w, int h,
                       radar_flush_fn flush, void *ctx){
    for(int y0=0;y0<h;y0+=band_h){ radar_gfx_t g={ .buf=band, .w=w, .y0=y0, .h=band_h };
        radar_gfx_clear(&g,COL_BG);
        if(view==RADAR_VIEW_HOME) draw_home(&g,st,nodes,node_count);
        else if(view==RADAR_VIEW_DETAIL) draw_detail(&g,st);
        else if(view==RADAR_VIEW_STATS) draw_stats(&g,st);
        else if(view==RADAR_VIEW_LIBRARY) draw_library(&g,lib);
        else if(view==RADAR_VIEW_CONTROL) draw_control(&g,ctrl);
        else if(view==RADAR_VIEW_INFO) draw_info(&g,st);
        else if(view==RADAR_VIEW_EXPOSURE) draw_exposure(&g,expo);
        else if(view==RADAR_VIEW_NODE) draw_node(&g,nodes,node_count,sel_node);
        else draw_radar(&g,st,sweep);
        flush(y0, band_h, band, ctx); }
}
```

- [ ] **Step 6: Add the `--node` harness mode + fix existing calls in `render_dump.c`**

Add this block at the top of `main`, right after the `--expo` block (before `int view = ...`):

```c
    if (argc > 1 && strcmp(argv[1], "--node") == 0) {
        int a = 2;
        int sel   = argc > a ? atoi(argv[a]) : 0; a++;
        int id    = argc > a ? atoi(argv[a]) : 0; a++;
        int alive = argc > a ? atoi(argv[a]) : 1; a++;
        unsigned age = argc > a ? (unsigned)strtoul(argv[a], 0, 10) : 0; a++;
        radar_wire_status_t st; memset(&st, 0, sizeof st);
        if (argc > a) st.active_devices = (uint16_t)atoi(argv[a]); a++;
        if (argc > a) st.active_target  = (uint8_t)atoi(argv[a]);  a++;
        if (argc > a) st.roster_size    = (uint16_t)atoi(argv[a]); a++;
        if (argc > a) st.form_restless  = (uint8_t)atoi(argv[a]);  a++;
        if (argc > a) st.form_wandering = (uint8_t)atoi(argv[a]);  a++;
        if (argc > a) st.form_bound     = (uint8_t)atoi(argv[a]);  a++;
        if (argc > a) st.pop_ewma       = (uint16_t)atoi(argv[a]); a++;
        if (argc > a) st.battery_mv     = (uint16_t)atoi(argv[a]); a++;
        if (argc > a) st.battery_pct    = (uint8_t)atoi(argv[a]); else st.battery_pct = 0xFF; a++;
        if (argc > a) st.epoch          = (uint16_t)atoi(argv[a]); a++;
        if (argc > a) st.probes_sent    = (uint32_t)strtoul(argv[a], 0, 10); a++;
        if (argc > a) st.flags          = (uint8_t)atoi(argv[a]);  a++;
        if (argc > a) st.uptime_s       = (uint32_t)strtoul(argv[a], 0, 10); a++;
        int threats = argc > a ? atoi(argv[a]) : 0; a++;
        int ncam    = argc > a ? atoi(argv[a]) : 0; a++;
        st.threat_count = (uint8_t)threats;
        for (int i = 0; i < threats && i < RADAR_MAX_THREATS; i++) st.threats[i].best_rssi = -55;
        for (int i = 0; i < ncam && i < threats && i < RADAR_MAX_THREATS; i++) {
            st.threats[i].kind = DETECT_KIND_KNOWN; st.threats[i].category = SIG_CAT_CAMERA;
            st.threats[i].class_id = SIG_CLASS_FLOCK;
        }
        static uint16_t nband[240 * 320];
        radar_node_view_t nodes[1] = { { (uint8_t)id, &st, alive != 0, age } };
        radar_render_view(RADAR_VIEW_NODE, &st, nodes, 1, sel, 0, 0, NULL, 0,
                          nband, 320, 240, 320, flush_noop, 0);
        return 0;
    }
```

Update the existing `--expo` render call (add `-1` for `sel_node` after `node_count`):

```c
        radar_render_view(RADAR_VIEW_EXPOSURE, 0, 0, 0, -1, 0, 0, &e, 0, eband, 320, 240, 320, flush_noop, 0);
```

Update the `nodes[1]` initializer (new 4th field) and the final render call:

```c
    radar_node_view_t nodes[1] = { { 0, &st, true, 0 } };

    static uint16_t band[240 * 320];
    // One full-height band: each draw_* runs once, so text is emitted a single time.
    radar_render_view((radar_view_t)view, &st, nodes, 1, -1, &lib, &ctrl, NULL, 0,
                      band, 320, 240, 320, flush_noop, NULL);
```

Also update the header comment listing views to include `8 NODE`.

- [ ] **Step 7: Update the 4 `radar_render_view` call sites in `cyd_main.c` (mechanical)**

In `cyd/main/cyd_main.c`, every `radar_render_view(ui.view, &agg, nv, nvc, &lib, ...)` call gains `-1` after `nvc` (there are 4: two in the `SIMULACRA_FLEET_PROVISION` block, two in the `#else` block). Example:

```c
                        radar_render_view(ui.view, &agg, nv, nvc, -1, &lib, &ctrl, (ui.view==RADAR_VIEW_EXPOSURE?&s_expo:NULL), sweep, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
```

Apply the identical `nvc, -1, &lib` edit to all four. (Behavior unchanged this task — NODE is not yet reachable; Task 4 replaces `-1` with the resolved index.)

- [ ] **Step 8: Build + run tests to verify they pass**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: build succeeds; `NodeScaffold.test_header_shows_node_id` and `test_sel_negative_shows_gone_placeholder` PASS; all pre-existing tests still PASS.

- [ ] **Step 9: Commit**

```bash
git add components/simulacra_radar/radar_ui.h components/simulacra_radar/radar_render.h components/simulacra_radar/radar_render.c tools/radar_audit/render_dump.c tools/radar_audit/tests/test_node_view.py cyd/main/cyd_main.c
git commit -m "feat(cyd-node): RADAR_VIEW_NODE scaffold + sel_node plumb + header/placeholder"
```

---

### Task 2: `draw_node` body — subline, CROWD, POWER, SYSTEM, DETECTIONS

Fills the live-node console for an alive node.

**Files:**
- Modify: `components/simulacra_radar/radar_render.c`
- Test: `tools/radar_audit/tests/test_node_view.py`

**Interfaces:**
- Consumes: `draw_header`, `row_section`, `row_kv`, `fmt_uptime`, `is_surveil_cat` (existing statics in `radar_render.c`); `radar_node_view_t` with `age_s`.
- Produces: full `draw_node` output (sections `CROWD`/`POWER`/`SYSTEM`/`DETECTIONS`).

- [ ] **Step 1: Write the failing tests**

Append to `tools/radar_audit/tests/test_node_view.py`:

```python
@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class NodeBody(unittest.TestCase):
    def test_sections_present(self):
        texts = node()
        for s in ("CROWD", "POWER", "SYSTEM", "DETECTIONS"):
            self.assertIn(s, texts, f"missing section {s}; drew: {texts}")
        for label in ("decoys", "target", "roster", "rpa/nrpa/static",
                      "real crowd", "battery", "epoch", "probes", "churn", "uptime"):
            self.assertIn(label, texts, f"missing row {label}; drew: {texts}")

    def test_health_channel_when_alive(self):
        self.assertIn("CHANNEL", node(alive=1, flags=0))

    def test_battery_usb(self):
        self.assertIn("USB", node(batt_mv=0))

    def test_battery_pct_format(self):
        self.assertIn("83% 3.9V", node(batt_mv=3900, batt_pct=83))

    def test_battery_voltage_only(self):
        self.assertIn("3.90V", node(batt_mv=3900, batt_pct=255))

    def test_churn_paused(self):
        self.assertIn("PAUSED", node(flags=1))

    def test_detections_partition(self):
        # 3 threats, 1 surveillance -> followers 2, surveillance 1
        texts = node(threats=3, ncam=1)
        i = texts.index("followers"); self.assertEqual(texts[i + 1], "2", f"drew: {texts}")
        j = texts.index("surveillance"); self.assertEqual(texts[j + 1], "1", f"drew: {texts}")
```

Note: `row_kv` emits the label then the value as two consecutive `TXT` draws, so `texts[i+1]` is the value for `texts[i]`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: `NodeBody` tests FAIL (only the header renders; no sections/values).

- [ ] **Step 3: Implement the full `draw_node` body**

Replace `draw_node` in `components/simulacra_radar/radar_render.c` with:

```c
static void draw_node(radar_gfx_t *g, const radar_node_view_t *nodes, int node_count, int sel){
    if (sel < 0 || sel >= node_count) {
        draw_header(g, "NODE");
        radar_gfx_text(g, 72, 150, "NODE GONE", COL_ASH);
        return;
    }
    const radar_node_view_t *nv = &nodes[sel];
    const radar_wire_status_t *st = nv->st;
    char title[12]; snprintf(title, sizeof title, "NODE N%u", (unsigned)nv->id);
    draw_header(g, title);
    // subline: health word (+ liveness age when silent)
    bool alive = nv->alive;
    bool low_batt = alive && (st->flags & 0x08);
    bool degraded = alive && (st->flags & 0x04);
    uint16_t sc = !alive ? COL_ASH : (low_batt || degraded) ? COL_WARD : COL_CHANNEL;
    const char *health = !alive ? "SILENT" : low_batt ? "LOW BATT" : degraded ? "DEGRADED" : "CHANNEL";
    radar_gfx_text(g, 8, 32, health, sc);
    if (!alive) { char ag[20]; snprintf(ag, sizeof ag, "seen %us ago", (unsigned)nv->age_s);
                  radar_gfx_text(g, 104, 32, ag, COL_ASH); }
    char v[24];
    // CROWD
    row_section(g, 50, "CROWD");
    snprintf(v,sizeof v,"%u",(unsigned)st->active_devices); row_kv(g,68,"decoys",v);
    snprintf(v,sizeof v,"%u",(unsigned)st->active_target);  row_kv(g,84,"target",v);
    snprintf(v,sizeof v,"%u",(unsigned)st->roster_size);    row_kv(g,100,"roster",v);
    snprintf(v,sizeof v,"%u / %u / %u",(unsigned)st->form_restless,(unsigned)st->form_wandering,(unsigned)st->form_bound);
    row_kv(g,116,"rpa/nrpa/static",v);
    snprintf(v,sizeof v,"%u",(unsigned)st->pop_ewma);       row_kv(g,132,"real crowd",v);
    // POWER
    row_section(g, 150, "POWER");
    if (st->battery_mv == 0) row_kv(g,168,"battery","USB");
    else if (st->battery_pct != 0xFF) {
        snprintf(v,sizeof v,"%u%% %u.%01uV",(unsigned)st->battery_pct,
                 (unsigned)(st->battery_mv/1000),(unsigned)((st->battery_mv%1000)/100));
        row_kv(g,168,"battery",v);
    } else {
        snprintf(v,sizeof v,"%u.%02uV",(unsigned)(st->battery_mv/1000),(unsigned)((st->battery_mv%1000)/10));
        row_kv(g,168,"battery",v);
    }
    // SYSTEM
    row_section(g, 188, "SYSTEM");
    snprintf(v,sizeof v,"%u",(unsigned)st->epoch);              row_kv(g,206,"epoch",v);
    snprintf(v,sizeof v,"%lu",(unsigned long)st->probes_sent);  row_kv(g,222,"probes",v);
    row_kv(g,238,"churn",(st->flags&0x1)?"PAUSED":"running");
    fmt_uptime(v,sizeof v,st->uptime_s);                        row_kv(g,254,"uptime",v);
    // DETECTIONS (this node's own counts; the list lives on the aggregate FOLLOWERS view)
    int nf=0, ns=0;
    for(uint8_t i=0;i<st->threat_count;i++){ if(is_surveil_cat(st->threats[i].category)) ns++; else nf++; }
    row_section(g, 272, "DETECTIONS");
    snprintf(v,sizeof v,"%d",nf); row_kv(g,288,"followers",v);
    snprintf(v,sizeof v,"%d",ns); row_kv(g,304,"surveillance",v);
}
```

- [ ] **Step 4: Build + run tests to verify they pass**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: all `NodeBody` + `NodeScaffold` tests PASS.

- [ ] **Step 5: Commit**

```bash
git add components/simulacra_radar/radar_render.c tools/radar_audit/tests/test_node_view.py
git commit -m "feat(cyd-node): draw_node body (CROWD/POWER/SYSTEM/DETECTIONS + battery formats)"
```

---

### Task 3: SILENT-node subline (liveness age)

Verifies the not-alive path: the subline reads `SILENT` and `seen Ns ago`, and the body still renders last-status values.

**Files:**
- Test: `tools/radar_audit/tests/test_node_view.py`

**Interfaces:**
- Consumes: the `draw_node` `!alive` branch (already implemented in Task 2).

- [ ] **Step 1: Write the failing test**

Append to `tools/radar_audit/tests/test_node_view.py`:

```python
@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class NodeSilent(unittest.TestCase):
    def test_silent_subline_shows_age(self):
        texts = node(alive=0, age=45, active=7)
        self.assertIn("SILENT", texts, f"drew: {texts}")
        self.assertIn("seen 45s ago", texts, f"drew: {texts}")

    def test_silent_still_shows_last_values(self):
        # a silent node keeps rendering its last decoy count, not a blank page
        texts = node(alive=0, age=45, active=7)
        i = texts.index("decoys"); self.assertEqual(texts[i + 1], "7", f"drew: {texts}")
```

- [ ] **Step 2: Run tests to verify their status**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: PASS (the `!alive` branch is already implemented in Task 2). This task is a verification gate; if either assertion fails, fix `draw_node`'s subline branch until they pass.

- [ ] **Step 3: Commit**

```bash
git add tools/radar_audit/tests/test_node_view.py
git commit -m "test(cyd-node): SILENT-node subline liveness-age assertions"
```

---

### Task 4: cyd_main integration (routing, in-page nav, live resolution) + fleet age accessor

Makes NODE reachable: HOME card tap opens it, prev/next sweeps the fleet, the page stays live and shows liveness age.

**Files:**
- Modify: `cyd/main/fleet_status.h`
- Modify: `cyd/main/fleet_status.c`
- Modify: `cyd/main/cyd_main.c`

**Interfaces:**
- Consumes: `RADAR_VIEW_NODE`; `radar_node_view_t.age_s`; `radar_render_view(..., sel_node, ...)`.
- Produces: `uint32_t fleet_status_age_ms(const fleet_status_t *f, int i, uint32_t now_ms)`.

- [ ] **Step 1: Add the fleet age accessor (declaration)**

In `cyd/main/fleet_status.h`, after `fleet_status_at`:

```c
// Milliseconds since node i (i-th used slot, same indexing as fleet_status_at) last reported. 0 if absent.
uint32_t fleet_status_age_ms(const fleet_status_t *f, int i, uint32_t now_ms);
```

- [ ] **Step 2: Implement the accessor**

In `cyd/main/fleet_status.c`, after `fleet_status_at`:

```c
uint32_t fleet_status_age_ms(const fleet_status_t *f, int i, uint32_t now_ms)
{
    int seen = 0;
    for (int k = 0; k < FLEET_STATUS_MAX; k++) {
        if (!f->nodes[k].used) continue;
        if (seen++ != i) continue;
        return (uint32_t)(now_ms - f->nodes[k].last_ms);
    }
    return 0;
}
```

- [ ] **Step 3: Add cyd_main NODE state**

In `cyd/main/cyd_main.c`, near the other radar-link statics (after the `s_node_n` registry, ~line 149), add:

```c
static uint8_t s_sel_node;        // NODE view: id of the node being inspected
static uint8_t s_home_ids[3];     // ids of the (<=3) node cards HOME last rendered, left->right
static int     s_home_n;          // how many of s_home_ids are valid
```

- [ ] **Step 4: Populate `age_s` and record HOME card ids + resolve `sel_idx`**

In the render block (inside `if (ui.backlight_on)`), where `nv[]` is built, set `age_s` on each entry and on the placeholder, then record the strip ids and resolve the selected index. Replace the `nv[]`-building region with:

```c
            radar_node_view_t nv[FLEET_STATUS_MAX]; int nvc = 0;
            for (int i = 0; i < fleet_status_count(&s_fleet) && nvc < FLEET_STATUS_MAX; i++) {
                uint8_t nid; const radar_wire_status_t *nst; bool nal;
                if (fleet_status_at(&s_fleet, i, &nid, &nst, &nal, now)) {
                    nv[nvc].id = nid; nv[nvc].st = nst; nv[nvc].alive = nal;
                    nv[nvc].age_s = fleet_status_age_ms(&s_fleet, i, now) / 1000;
                    nvc++;
                }
            }
            if (nvc == 0) {
                bool s_fresh = (s_status_ms != 0 && (int32_t)(now - s_status_ms) <= 15000);
                nv[0].id = 0; nv[0].st = &s_status; nv[0].alive = s_fresh;
                nv[0].age_s = s_status_ms ? (now - s_status_ms) / 1000 : 0;
                nvc = 1;
            }
            // Record the strip ids HOME will draw (<=3), for the next frame's card-tap mapping.
            s_home_n = nvc > 3 ? 3 : nvc;
            for (int i = 0; i < s_home_n; i++) s_home_ids[i] = nv[i].id;
            // Resolve the selected node -> index into nv[] (-1 if gone) for the NODE view.
            int sel_idx = -1;
            if (ui.view == RADAR_VIEW_NODE)
                for (int i = 0; i < nvc; i++) if (nv[i].id == s_sel_node) { sel_idx = i; break; }
```

- [ ] **Step 5: Pass `sel_idx` to the 4 render calls**

Replace the `-1` added in Task 1 with `sel_idx` in all four `radar_render_view(ui.view, &agg, nv, nvc, -1, &lib, ...)` calls:

```c
                        radar_render_view(ui.view, &agg, nv, nvc, sel_idx, &lib, &ctrl, (ui.view==RADAR_VIEW_EXPOSURE?&s_expo:NULL), sweep, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
```

- [ ] **Step 6: Route HOME node-card taps into NODE**

In the `if (ui.view == RADAR_VIEW_HOME)` tap handler, replace the fleet-strip branch:

```c
                else if (ty >= 30 && ty < 100) {
                    int card = tx / 80;
                    if (card < s_home_n) { s_sel_node = s_home_ids[card]; v = RADAR_VIEW_NODE; }
                    else                 v = RADAR_VIEW_RADAR;   // empty strip area -> aggregate radar
                }
```

- [ ] **Step 7: Add NODE in-page navigation**

In the `edge && !modal_open` touch dispatch chain, insert a NODE branch before the final generic `else`:

```c
            } else if (ui.view == RADAR_VIEW_NODE) {
                if (ty < 26) { radar_ui_on_input(&ui, now); }          // "< BACK" strip -> HOME
                else {
                    if (s_home_n > 0) {
                        int cur = 0;
                        for (int i = 0; i < s_home_n; i++) if (s_home_ids[i] == s_sel_node) { cur = i; break; }
                        if (tx < 80)        s_sel_node = s_home_ids[(cur - 1 + s_home_n) % s_home_n];
                        else if (tx > 160)  s_sel_node = s_home_ids[(cur + 1) % s_home_n];
                    }
                    radar_ui_note_input(&ui, now); send_request(); last_req = now;
                }
            }
```

- [ ] **Step 8: Skip the freshness overlay on NODE**

In the `SIMULACRA_FLEET_PROVISION` render block's overlay chain, add a NODE arm alongside HOME/EXPOSURE:

```c
                    else if (ui.view == RADAR_VIEW_NODE)     { /* NODE shows its own liveness */ }
```

In the `#else` block's overlay guard, exclude NODE:

```c
                    if (ui.view != RADAR_VIEW_HOME && ui.view != RADAR_VIEW_EXPOSURE && ui.view != RADAR_VIEW_NODE) draw_freshness_overlay(band, now);
```

- [ ] **Step 9: Compile-verify the CYD firmware**

Run: `idf.py -C cyd build`
Expected: build succeeds (exit 0), produces `cyd/build/simulacra_cyd.bin`. (See `private/CYD-BUILD-FLOW.md` for the esp32 target/env gotchas.)

- [ ] **Step 10: Compile-verify the decoy targets still build the component**

Build the c5 and c6 decoy firmware the normal way (raw `idf.py` per `private/FLEET-SETUP.md` §4, each target in its own shell — batching multi-target builds in one shell fails per `private/TOOLING-GOTCHAS`). Expected: both succeed; the `simulacra_radar` component (with the new `radar_render_view` signature + `radar_node_view_t.age_s`) compiles on every target even though decoys never render NODE.

- [ ] **Step 11: Re-run the host test suite**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: all `test_node_view.py` classes + the pre-existing suites PASS.

- [ ] **Step 12: Commit**

```bash
git add cyd/main/fleet_status.h cyd/main/fleet_status.c cyd/main/cyd_main.c
git commit -m "feat(cyd-node): wire HOME card-tap -> NODE, prev/next nav, live age resolution"
```

---

## Self-Review

**Spec coverage:**
- Navigation (card tap → NODE, empty slot → aggregate RADAR) → Task 4 Step 6. ✓
- `RADAR_VIEW_NODE` enum → Task 1 Step 3. ✓
- `sel_node` param + ripple to render_dump + cyd_main → Task 1 Steps 4/6/7, Task 4 Step 5. ✓
- Live re-resolution each frame → Task 4 Step 4. ✓
- Card→id mapping via `s_home_ids[]` → Task 4 Steps 4/6. ✓
- In-page nav (BACK/prev/next/refresh, wrap) → Task 4 Step 7. ✓
- NODE content: header, subline health+liveness, CROWD/POWER/SYSTEM/DETECTIONS → Task 2. ✓ (ENVIRONMENT folded: `real crowd`/`pop_ewma` kept in CROWD; `total_obs` dropped per the fit budget — it remains on the aggregate DECOYS view.)
- Battery 3 formats → Task 2 Step 3 + tests. ✓
- SILENT persistence + "seen Ns ago" → Task 2 (impl) + Task 3 (tests). ✓
- `NODE GONE` placeholder (`sel_node==-1`) → Task 1. ✓
- Freshness-overlay skip → Task 4 Step 8. ✓
- Testing via render_dump `--node` incl SILENT + placeholder → Tasks 1–3. ✓
- Compile-verify c5/c6/cyd → Task 4 Steps 9–10. ✓

**Placeholder scan:** none — every code step shows complete code.

**Type consistency:** `draw_node(radar_gfx_t*, const radar_node_view_t*, int, int)` consistent across Tasks 1–2. `radar_render_view` `sel_node` inserted after `node_count` consistently at all call sites (render_dump ×3, cyd_main ×4). `radar_node_view_t.age_s` (`uint32_t`) defined Task 1, populated Task 4, consumed Task 2. `fleet_status_age_ms` signature identical in `.h`/`.c`. Battery format strings match `draw_home` and the test expectations (`"83% 3.9V"`, `"3.90V"`). `row_kv` label-then-value ordering underpins the `texts[i+1]` value assertions.

**Fit check:** last drawn row at `y=304` (DETECTIONS `surveillance`), 8px glyph → ends ~312 < 318. ✓

## Rollout

Pure CYD firmware change. Flash the CYD only (`idf.py -C cyd -p <PORT> flash`); decoys untouched. No wire-version bump — new CYD interoperates with the existing fleet.
