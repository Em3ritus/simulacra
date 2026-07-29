# CYD Threat Detail Card (THREAT drill-in) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `RADAR_VIEW_THREAT` detail card that surfaces the per-threat fields already on the wire but never rendered (confidence, vendor, epochs, first/last-epoch), reached by tapping the FOLLOWERS body and paged prev/next.

**Architecture:** Pure CYD-render feature, mirroring the just-shipped NODE drill-in. Threat data is already in `agg.threats[]`. Add a `draw_threat` renderer fed by an `int sel_threat` index, extend `radar_render_view`, and wire FOLLOWERS body-tap + THREAT prev/next in `cyd_main.c` using a hash-keyed selection (`s_sel_threat`) resolved each frame. No decoy/wire change.

**Tech Stack:** C (ESP-IDF firmware; MSVC `cl` host tests), Python `unittest` harness (`tools/radar_audit/render_dump`).

## Global Constraints

- CYD-render-only. Do **not** modify `radar_wire.h`, the wire version, or any decoy-side code.
- Reuse existing render primitives: `draw_header`, `row_section`, `row_kv`, `sig_class_name`, `threat_escalation_level`, `is_surveil_cat`.
- `radar_render_view` `sel_threat` is an index into `st->threats[]`; `-1` = none (only `RADAR_VIEW_THREAT` reads it). It goes immediately after `sel_node`.
- Selection is stored as a threat `hash` (`uint32_t`), resolved to an index each frame (hashes are stable, indices are not).
- Paging order = raw `agg.threats[]` order; `n/N` = `sel_threat+1 / threat_count`.
- `draw_threat` must not draw past `y=318`.
- Threat name: `sig_class_name(class_id)` when `kind == DETECT_KIND_KNOWN`, else `%08lx` of `hash` (matches `draw_detail`).

## File Structure

- `components/simulacra_radar/radar_ui.h` — add `RADAR_VIEW_THREAT` (after `RADAR_VIEW_NODE`, before `RADAR_VIEW_COUNT`).
- `components/simulacra_radar/radar_render.h` — add `int sel_threat` to `radar_render_view`.
- `components/simulacra_radar/radar_render.c` — new `cat_name` + `draw_threat`; dispatch + `sel_threat` plumb; surveillance-row confidence in `draw_detail`.
- `tools/radar_audit/render_dump.c` — new `--threat` mode; update the 3 existing `radar_render_view` calls for the new param.
- `tools/radar_audit/tests/test_threat_detail.py` — new host tests.
- `cyd/main/cyd_main.c` — `s_sel_threat`/`s_threat_hashes[]`/`s_threat_n`, FOLLOWERS + THREAT touch branches, record/resolve `sel_threat`, pass it to the 4 render calls.

---

### Task 1: THREAT scaffolding (enum, signature, harness `--threat`, header + placeholder)

Delivers a harness-reachable THREAT view: titled `THREAT n/N` header for a valid threat, `THREAT GONE` for `sel < 0`. No body yet.

**Files:**
- Modify: `components/simulacra_radar/radar_ui.h`
- Modify: `components/simulacra_radar/radar_render.h`
- Modify: `components/simulacra_radar/radar_render.c`
- Modify: `tools/radar_audit/render_dump.c`
- Modify: `cyd/main/cyd_main.c` (mechanical: add the new arg to the 4 call sites)
- Test: `tools/radar_audit/tests/test_threat_detail.py`

**Interfaces:**
- Produces: `RADAR_VIEW_THREAT` (enum); `radar_render_view(..., int sel_node, int sel_threat, const radar_lib_info_t *lib, ...)`; `static void draw_threat(radar_gfx_t *g, const radar_wire_status_t *st, int sel)`.

- [ ] **Step 1: Write the failing test**

Create `tools/radar_audit/tests/test_threat_detail.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")


def threat(sel=0, count=1, kind=1, cls=3, cat=1, conf=92, vendor=0x09C8,
           rssi=-55, epochs=4, first=2, last=9, sessions=3, places=3):
    """Render RADAR_VIEW_THREAT and return the list of text strings drawn.
    kind: 1=KNOWN 0=behavioral. cls: sig_class_t. cat: sig_category_t."""
    args = [EXE, "--threat", sel, count, kind, cls, cat, conf, vendor,
            rssi, epochs, first, last, sessions, places]
    out = subprocess.check_output([str(x) for x in args], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class ThreatScaffold(unittest.TestCase):
    def test_header_shows_position(self):
        texts = threat(sel=0, count=3)
        self.assertIn("THREAT 1/3", texts, f"no position header; drew: {texts}")

    def test_sel_negative_shows_gone(self):
        texts = threat(sel=-1)
        self.assertIn("THREAT GONE", texts, f"no gone placeholder; drew: {texts}")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: build FAILS (no `--threat` mode / `RADAR_VIEW_THREAT` undefined) or the two `ThreatScaffold` tests FAIL.

- [ ] **Step 3: Add the enum value**

In `components/simulacra_radar/radar_ui.h`:

```c
typedef enum { RADAR_VIEW_HOME = 0, RADAR_VIEW_RADAR, RADAR_VIEW_DETAIL, RADAR_VIEW_STATS,
               RADAR_VIEW_LIBRARY, RADAR_VIEW_CONTROL, RADAR_VIEW_INFO, RADAR_VIEW_EXPOSURE,
               RADAR_VIEW_NODE, RADAR_VIEW_THREAT,
               RADAR_VIEW_COUNT } radar_view_t;
```

- [ ] **Step 4: Extend the signature in the header**

In `components/simulacra_radar/radar_render.h`:

```c
void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                       const radar_node_view_t *nodes, int node_count, int sel_node, int sel_threat,
                       const radar_lib_info_t *lib, const radar_ctrl_info_t *ctrl,
                       const exposure_t *expo, uint16_t sweep_deg,
                       uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx);
```

- [ ] **Step 5: Add `draw_threat` (header + placeholder only) and dispatch in `radar_render.c`**

Add `draw_threat` immediately after `draw_node`:

```c
static void draw_threat(radar_gfx_t *g, const radar_wire_status_t *st, int sel){
    if (sel < 0 || sel >= st->threat_count) {
        draw_header(g, "THREAT");
        radar_gfx_text(g, 60, 150, "THREAT GONE", COL_ASH);
        return;
    }
    char title[16]; snprintf(title, sizeof title, "THREAT %d/%u", sel + 1, (unsigned)st->threat_count);
    draw_header(g, title);
}
```

Update `radar_render_view`'s signature line + dispatch:

```c
void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                       const radar_node_view_t *nodes, int node_count, int sel_node, int sel_threat,
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
        else if(view==RADAR_VIEW_THREAT) draw_threat(&g,st,sel_threat);
        else draw_radar(&g,st,sweep);
        flush(y0, band_h, band, ctx); }
}
```

- [ ] **Step 6: Add the `--threat` harness mode + fix existing calls in `render_dump.c`**

Add this block right after the `--node` block (before `int view = ...`):

```c
    if (argc > 1 && strcmp(argv[1], "--threat") == 0) {
        int a = 2;
        int sel   = argc > a ? atoi(argv[a]) : 0; a++;
        int count = argc > a ? atoi(argv[a]) : 1; a++;
        int kind  = argc > a ? atoi(argv[a]) : 1; a++;
        int cls   = argc > a ? atoi(argv[a]) : 0; a++;
        int cat   = argc > a ? atoi(argv[a]) : 0; a++;
        int conf  = argc > a ? atoi(argv[a]) : 0; a++;
        int vendor= argc > a ? (int)strtol(argv[a], 0, 0) : 0; a++;
        int rssi  = argc > a ? atoi(argv[a]) : 0; a++;
        int epochs= argc > a ? atoi(argv[a]) : 0; a++;
        int first = argc > a ? atoi(argv[a]) : 0; a++;
        int last  = argc > a ? atoi(argv[a]) : 0; a++;
        int sess  = argc > a ? atoi(argv[a]) : 0; a++;
        int places= argc > a ? atoi(argv[a]) : 0; a++;
        radar_wire_status_t st; memset(&st, 0, sizeof st);
        if (count > RADAR_MAX_THREATS) count = RADAR_MAX_THREATS;
        st.threat_count = (uint8_t)count;
        int t = (sel >= 0 && sel < count) ? sel : 0;
        st.threats[t].hash = 0xABCD1234u;
        st.threats[t].kind = (uint8_t)kind; st.threats[t].class_id = (uint8_t)cls;
        st.threats[t].category = (uint8_t)cat; st.threats[t].confidence = (uint8_t)conf;
        st.threats[t].vendor = (uint16_t)vendor; st.threats[t].best_rssi = (int8_t)rssi;
        st.threats[t].epochs = (uint8_t)epochs;
        st.threats[t].first_epoch = (uint16_t)first; st.threats[t].last_epoch = (uint16_t)last;
        st.threats[t].sessions_seen = (uint8_t)sess; st.threats[t].places_seen = (uint8_t)places;
        static uint16_t tband[240 * 320];
        radar_render_view(RADAR_VIEW_THREAT, &st, 0, 0, -1, sel, 0, 0, NULL, 0,
                          tband, 320, 240, 320, flush_noop, 0);
        return 0;
    }
```

Update the `--expo` call and the `--node` call and the final call to add `sel_threat` (the arg after `sel_node`):
- `--expo`: `radar_render_view(RADAR_VIEW_EXPOSURE, 0, 0, 0, -1, -1, 0, 0, &e, 0, eband, 320, 240, 320, flush_noop, 0);`
- `--node`: `radar_render_view(RADAR_VIEW_NODE, &st, nodes, 1, sel, -1, 0, 0, NULL, 0, nband, 320, 240, 320, flush_noop, 0);`
- final: `radar_render_view((radar_view_t)view, &st, nodes, 1, -1, -1, &lib, &ctrl, NULL, 0, band, 320, 240, 320, flush_noop, NULL);`

Also update the header comment to note `9 THREAT (via --threat)`.

- [ ] **Step 7: Update the 4 `radar_render_view` call sites in `cyd_main.c` (mechanical)**

Each `radar_render_view(ui.view, &agg, nv, nvc, sel_idx, &lib, ...)` gains `-1` after `sel_idx` (4 occurrences):

```c
                        radar_render_view(ui.view, &agg, nv, nvc, sel_idx, -1, &lib, &ctrl, (ui.view==RADAR_VIEW_EXPOSURE?&s_expo:NULL), sweep, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
```

(Behavior unchanged this task — THREAT not yet reachable; Task 3 replaces `-1` with the resolved index.)

- [ ] **Step 8: Build + run tests to verify they pass**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: build succeeds; both `ThreatScaffold` tests PASS; all pre-existing tests still PASS.

- [ ] **Step 9: Commit**

```bash
git add components/simulacra_radar/radar_ui.h components/simulacra_radar/radar_render.h components/simulacra_radar/radar_render.c tools/radar_audit/render_dump.c tools/radar_audit/tests/test_threat_detail.py cyd/main/cyd_main.c
git commit -m "feat(cyd-threat): RADAR_VIEW_THREAT scaffold + sel_threat plumb + header/placeholder"
```

---

### Task 2: `draw_threat` body + `cat_name` + surveillance-row confidence

**Files:**
- Modify: `components/simulacra_radar/radar_render.c`
- Test: `tools/radar_audit/tests/test_threat_detail.py`

**Interfaces:**
- Consumes: `draw_header`, `row_section`, `row_kv`, `sig_class_name`, `threat_escalation_level`, `is_surveil_cat` (existing in `radar_render.c`).
- Produces: `cat_name`; full `draw_threat` (CLASSIFICATION/SIGHTING sections); confidence token in `draw_detail`'s surveillance rows.

- [ ] **Step 1: Write the failing tests**

Append to `tools/radar_audit/tests/test_threat_detail.py`:

```python
def render(view, *args):
    out = subprocess.check_output([EXE, str(view), *[str(a) for a in args]], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


DETAIL = 2  # radar_view_t: DETAIL=2


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class ThreatBody(unittest.TestCase):
    def test_known_camera_fields(self):
        # kind=1 known, cls=3 Flock, cat=1 CAMERA, conf=92, vendor=0x09C8
        texts = threat(kind=1, cls=3, cat=1, conf=92, vendor=0x09C8, epochs=4, first=2, last=9)
        for tok in ("CAMERA", "Flock", "known", "92%", "0x09C8"):
            self.assertIn(tok, texts, f"missing {tok}; drew: {texts}")
        self.assertIn("e2..e9", texts, f"missing span; drew: {texts}")
        i = texts.index("epochs"); self.assertEqual(texts[i + 1], "4", f"drew: {texts}")

    def test_behavioral_follower_dashes(self):
        # kind=0 behavioral -> class/confidence/vendor show '-'
        texts = threat(kind=0, cls=0, cat=3, conf=0, vendor=0)
        self.assertIn("behavioral", texts, f"drew: {texts}")
        i = texts.index("class"); self.assertEqual(texts[i + 1], "-", f"class not dashed; drew: {texts}")
        j = texts.index("confidence"); self.assertEqual(texts[j + 1], "-", f"conf not dashed; drew: {texts}")
        k = texts.index("vendor"); self.assertEqual(texts[k + 1], "-", f"vendor not dashed; drew: {texts}")

    def test_escalation_verdict(self):
        texts = threat(sessions=3, places=3)   # PERSISTENT (sessions>=3)
        self.assertIn("PERSISTENT", texts, f"drew: {texts}")


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class SurveillanceRowConfidence(unittest.TestCase):
    def test_surveillance_row_shows_confidence(self):
        # DETAIL positional: view, restless,wandering,bound, active,roster,target,
        #                    threats, pop, esc, flags, uptime, ncam, surv_kind
        # 1 threat, 1 camera. render_dump sets camera confidence via the threat loop? No -
        # the --threat path sets confidence; draw_detail reads threats[i].confidence which the
        # main positional path leaves 0. Assert the '%' token is present (0% is fine).
        texts = render(DETAIL, 1, 1, 1, 8, 16, 8, 1, 10, 0, 0, 0, 1)
        self.assertTrue(any(t.endswith("%") for t in texts),
                        f"no confidence token on surveillance row; drew: {texts}")
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: `ThreatBody` + `SurveillanceRowConfidence` FAIL (only the header renders; surveillance rows lack a `%` token).

- [ ] **Step 3: Add `cat_name` and the full `draw_threat` body**

In `components/simulacra_radar/radar_render.c`, add `cat_name` just above `draw_threat`:

```c
static const char *cat_name(uint8_t c){
    return c==SIG_CAT_TRACKER?"TRACKER":c==SIG_CAT_CAMERA?"CAMERA":
           c==SIG_CAT_BODYCAM?"BODYCAM":"UNKNOWN";
}
```

Replace the header-only `draw_threat` with the full body. The threat entries are an **anonymous** struct inside `radar_wire_status_t` (no named type to take a pointer of), so reference `st->threats[sel]` directly — portable on both `cl` and GCC:

```c
static void draw_threat(radar_gfx_t *g, const radar_wire_status_t *st, int sel){
    if (sel < 0 || sel >= st->threat_count) {
        draw_header(g, "THREAT");
        radar_gfx_text(g, 60, 150, "THREAT GONE", COL_ASH);
        return;
    }
    int i = sel;
    char title[16]; snprintf(title, sizeof title, "THREAT %d/%u", sel + 1, (unsigned)st->threat_count);
    draw_header(g, title);
    detect_escalation_t e = threat_escalation_level(st->threats[i].sessions_seen, st->threats[i].places_seen);
    uint16_t ec = escalation_color(e);
    bool known = (st->threats[i].kind == DETECT_KIND_KNOWN);
    char sub[32];
    if (known) snprintf(sub, sizeof sub, "%s  %s", sig_class_name(st->threats[i].class_id), cat_name(st->threats[i].category));
    else       snprintf(sub, sizeof sub, "%08lx  %s", (unsigned long)st->threats[i].hash, cat_name(st->threats[i].category));
    radar_gfx_text(g, 8, 32, sub, ec);
    char v[24];
    row_section(g, 50, "CLASSIFICATION");
    row_kv(g, 68, "kind", known ? "known" : "behavioral");
    row_kv(g, 84, "class", known ? sig_class_name(st->threats[i].class_id) : "-");
    row_kv(g, 100, "category", cat_name(st->threats[i].category));
    if (known) { snprintf(v,sizeof v,"%u%%",(unsigned)st->threats[i].confidence); row_kv(g,116,"confidence",v); }
    else       row_kv(g,116,"confidence","-");
    if (st->threats[i].vendor != 0 && st->threats[i].vendor != 0xFFFF) { snprintf(v,sizeof v,"0x%04X",(unsigned)st->threats[i].vendor); row_kv(g,132,"vendor",v); }
    else       row_kv(g,132,"vendor","-");
    row_section(g, 150, "SIGHTING");
    snprintf(v,sizeof v,"%ddB",(int)st->threats[i].best_rssi); row_kv(g,168,"rssi",v);
    row_kv(g,184,"escalation", e==ESCALATION_PERSISTENT?"PERSISTENT":e==ESCALATION_RECURRING?"RECURRING":"NEW");
    snprintf(v,sizeof v,"%u",(unsigned)st->threats[i].sessions_seen); row_kv(g,200,"sessions",v);
    snprintf(v,sizeof v,"%u",(unsigned)st->threats[i].places_seen);   row_kv(g,216,"places",v);
    snprintf(v,sizeof v,"%u",(unsigned)st->threats[i].epochs);        row_kv(g,232,"epochs",v);
    snprintf(v,sizeof v,"e%u..e%u",(unsigned)st->threats[i].first_epoch,(unsigned)st->threats[i].last_epoch); row_kv(g,248,"span",v);
}
```

`escalation_color`, `threat_escalation_level`, `ESCALATION_*`, and `detect_escalation_t` are already available in `radar_render.c` (used by `draw_detail`).

- [ ] **Step 4: Add confidence to the surveillance rows in `draw_detail`**

In `draw_detail`'s SURVEILLANCE loop, insert the confidence token between the name and rssi. Change:

```c
            radar_gfx_text(g,20,y,sig_class_name(st->threats[i].class_id),COL_HUNTER);
            char r[12]; snprintf(r,sizeof r,"%ddB",(int)st->threats[i].best_rssi);
            radar_gfx_text(g,224-(int)strlen(r)*8,y,r,COL_ASH);
```

to:

```c
            radar_gfx_text(g,20,y,sig_class_name(st->threats[i].class_id),COL_HUNTER);
            char cf[8]; snprintf(cf,sizeof cf,"%u%%",(unsigned)st->threats[i].confidence);
            radar_gfx_text(g,120,y,cf,COL_ASH);
            char r[12]; snprintf(r,sizeof r,"%ddB",(int)st->threats[i].best_rssi);
            radar_gfx_text(g,224-(int)strlen(r)*8,y,r,COL_ASH);
```

- [ ] **Step 5: Build + run tests to verify they pass**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: all `ThreatBody` + `SurveillanceRowConfidence` + `ThreatScaffold` tests PASS; pre-existing suites still PASS.

- [ ] **Step 6: Commit**

```bash
git add components/simulacra_radar/radar_render.c tools/radar_audit/tests/test_threat_detail.py
git commit -m "feat(cyd-threat): draw_threat body (classification/sighting) + surveillance-row confidence"
```

---

### Task 3: cyd_main integration (FOLLOWERS body-tap, THREAT nav, live resolution)

Makes THREAT reachable: FOLLOWERS body-tap opens it, prev/next pages, the card stays live by hash.

**Files:**
- Modify: `cyd/main/cyd_main.c`

**Interfaces:**
- Consumes: `RADAR_VIEW_THREAT`; `radar_render_view(..., sel_node, sel_threat, ...)`; `agg.threats[].hash`.

- [ ] **Step 1: Add THREAT selection state**

Near the NODE statics added in sub-project A (`s_sel_node`/`s_home_ids`/`s_home_n`), add:

```c
static uint32_t s_sel_threat;                       // THREAT view: hash of the threat being inspected
static uint32_t s_threat_hashes[RADAR_MAX_THREATS]; // hashes of the threats agg last rendered, in order
static int      s_threat_n;                          // how many of s_threat_hashes are valid
```

- [ ] **Step 2: Record the threat set + resolve `sel_threat` each frame**

In the render block, right after the NODE block records `s_home_ids[]`/`sel_idx` (both use `nv[]`/`agg`), add the threat-set record and resolution. `agg` is already computed above the render block:

```c
            // Record the current fleet threat set (by hash) for THREAT body-tap entry + paging.
            s_threat_n = agg.threat_count > RADAR_MAX_THREATS ? RADAR_MAX_THREATS : agg.threat_count;
            for (int i = 0; i < s_threat_n; i++) s_threat_hashes[i] = agg.threats[i].hash;
            int sel_threat = -1;
            if (ui.view == RADAR_VIEW_THREAT)
                for (int i = 0; i < (int)agg.threat_count; i++)
                    if (agg.threats[i].hash == s_sel_threat) { sel_threat = i; break; }
```

- [ ] **Step 3: Pass `sel_threat` to the 4 render calls**

Replace the `-1` added in Task 1 with `sel_threat` in all four `radar_render_view(ui.view, &agg, nv, nvc, sel_idx, -1, &lib, ...)` calls:

```c
                        radar_render_view(ui.view, &agg, nv, nvc, sel_idx, sel_threat, &lib, &ctrl, (ui.view==RADAR_VIEW_EXPOSURE?&s_expo:NULL), sweep, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
```

- [ ] **Step 4: FOLLOWERS body-tap → THREAT, and THREAT in-page nav**

In the `edge && !modal_open` touch dispatch chain, add a `RADAR_VIEW_DETAIL` branch and a `RADAR_VIEW_THREAT` branch before the generic `else`. The current chain routes DETAIL through the generic `else` (any tap → HOME); the new DETAIL branch overrides that. Insert after the `RADAR_VIEW_NODE` branch added in sub-project A:

```c
            } else if (ui.view == RADAR_VIEW_DETAIL) {
                if (ty < 26 || s_threat_n == 0) { radar_ui_on_input(&ui, now); }   // BACK / nothing to drill
                else {
                    s_sel_threat = s_threat_hashes[0];
                    radar_ui_select_view(&ui, RADAR_VIEW_THREAT, now); send_request(); last_req = now;
                }
            } else if (ui.view == RADAR_VIEW_THREAT) {
                if (ty < 26) { radar_ui_select_view(&ui, RADAR_VIEW_DETAIL, now); }  // "< BACK" -> FOLLOWERS
                else {
                    if (s_threat_n > 0) {
                        int cur = 0;
                        for (int i = 0; i < s_threat_n; i++) if (s_threat_hashes[i] == s_sel_threat) { cur = i; break; }
                        if (tx < 80)        s_sel_threat = s_threat_hashes[(cur - 1 + s_threat_n) % s_threat_n];
                        else if (tx > 160)  s_sel_threat = s_threat_hashes[(cur + 1) % s_threat_n];
                    }
                    radar_ui_note_input(&ui, now); send_request(); last_req = now;
                }
            }
```

Note: THREAT BACK uses `radar_ui_select_view(&ui, RADAR_VIEW_DETAIL, now)` (jump to FOLLOWERS), not `radar_ui_on_input` (which returns to HOME), so BACK from the card lands on the list it came from.

- [ ] **Step 5: Compile-verify the CYD firmware**

Run (PowerShell, IDF 5.4 for the xtensa CYD — see `private/CYD-BUILD-FLOW.md`):

```powershell
$py='C:\Program Files\Python312'; $env:PATH="$py;$py\Scripts;$env:PATH"
$idf='$env:USERPROFILE\esp\v5.4\esp-idf'; $env:IDF_PATH=$idf; . ($idf+'\export.ps1') *>$null
idf.py -C cyd build
```

Expected: `Project build complete`, `cyd/build/simulacra_cyd.bin` regenerated, exit 0.

- [ ] **Step 6: Compile-verify the c5 decoy (shared component)**

Run (PowerShell, IDF 5.5 for the RISC-V c5; root is configured for `esp32c5`):

```powershell
$py='C:\Program Files\Python312'; $env:PATH="$py;$py\Scripts;$env:PATH"
$idf='$env:USERPROFILE\esp\v5.5\esp-idf'; $env:IDF_PATH=$idf; . ($idf+'\export.ps1') *>$null
idf.py build
```

Expected: `Project build complete` (the `simulacra_radar` component compiles with the new signature; the pre-existing `threat_color` unused-function warning is unrelated).

- [ ] **Step 7: Re-run the host suite**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: all `test_threat_detail.py` classes + the pre-existing suites PASS.

- [ ] **Step 8: Commit**

```bash
git add cyd/main/cyd_main.c
git commit -m "feat(cyd-threat): FOLLOWERS body-tap -> THREAT, prev/next paging, live hash resolution"
```

---

## Self-Review

**Spec coverage:**
- `RADAR_VIEW_THREAT` enum → Task 1 Step 3. ✓
- `sel_threat` param + ripple (render_dump ×3 + cyd_main ×4) → Task 1 Steps 4/6/7, Task 3 Step 3. ✓
- Body-tap entry (≥1 threat) / header→HOME / empty→HOME → Task 3 Step 4. ✓
- THREAT nav (BACK→FOLLOWERS, prev/next wrap, refresh) → Task 3 Step 4. ✓
- Hash-keyed selection + per-frame resolution + `s_threat_hashes[]` → Task 3 Steps 1/2. ✓
- Card content: header `n/N`, subline, CLASSIFICATION (kind/class/category/confidence/vendor), SIGHTING (rssi/escalation/sessions/places/epochs/span) → Task 2 Step 3. ✓
- `-`/dash for behavioral (class/confidence/vendor) → Task 2 Step 3 + test. ✓
- `THREAT GONE` placeholder → Task 1 Step 5. ✓
- Surveillance-row confidence → Task 2 Step 4 + test. ✓
- Testing via `--threat` incl behavioral + placeholder + surveillance confidence → Tasks 1–2. ✓
- Compile-verify cyd/c5 → Task 3 Steps 5–6. ✓

**Placeholder scan:** none — every code step shows complete code.

**Type consistency:** `draw_threat(radar_gfx_t*, const radar_wire_status_t*, int)` consistent Tasks 1–2. `sel_threat` inserted after `sel_node` at every call site (render_dump: `--expo`/`--node`/`--threat`/final; cyd_main ×4). `s_sel_threat` (`uint32_t` hash) set in Task 3 Step 4, resolved Task 3 Step 2. `cat_name`/`escalation_color`/`threat_escalation_level` names match their definitions. Test token expectations (`"0x09C8"`, `"e2..e9"`, `"92%"`, `"PERSISTENT"`, `"behavioral"`) match the format strings. `row_kv` label-then-value ordering underpins `texts[i+1]` assertions.

**Fit check:** last row `span` at `y=248`, 8px glyph → ends ~256 < 318. ✓

## Rollout

Pure CYD firmware change. Flash the CYD only (`idf.py -C cyd -p <PORT> flash`); decoys untouched. No wire-version bump.
