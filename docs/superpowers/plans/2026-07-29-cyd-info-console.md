# CYD INFO System Console + Legend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the 3-line INFO page into a two-page view — page 0 a SYSTEM/FLEET console, page 1 a color/posture LEGEND — toggled by body-tap.

**Architecture:** Pure CYD-render. Pass the existing `lib` snapshot to `draw_info` and add a small `radar_sys_info_t` (node count, sig-DB ver/count, link age, build tag, page) filled by cyd_main. `draw_info` renders page 0 or page 1 off `sys->page`; cyd_main toggles a `s_info_page` static on INFO body-tap. No decoy/wire change.

**Tech Stack:** C (ESP-IDF firmware; MSVC `cl` host tests), Python `unittest` harness (`tools/radar_audit/render_dump`).

## Global Constraints

- CYD-render-only. Do **not** modify `radar_wire.h`, the wire version, or any decoy-side code.
- Reuse existing primitives/helpers: `draw_header`, `row_section`, `row_kv`, `fmt_uptime`, `posture_color`, `escalation_color` (all already in `radar_render.c`).
- Legend colors MUST come from `posture_color()` / `escalation_color()` and the `COL_*` health constants so the legend can't drift from the live UI.
- `sys` is the new optional pointer (NULL except INFO), placed **immediately after `expo`** in `radar_render_view`.
- `draw_info` must render page 0 gracefully when `sys == NULL` (host positional path) — keep a `SYSTEM` section with a `firmware` row and humanized uptime so the existing `test_radar_render.py` INFO assertions still pass.
- `draw_info` must not draw past `y=318`.

## File Structure

- `components/simulacra_radar/radar_render.h` — add `radar_sys_info_t`; add `const radar_sys_info_t *sys` to `radar_render_view`.
- `components/simulacra_radar/radar_render.c` — rewrite `draw_info(g, st, lib, sys)` (page 0 + page 1); dispatch + `sys` plumb.
- `tools/radar_audit/render_dump.c` — `--info` mode; update the 4 existing `radar_render_view` calls for the new param.
- `tools/radar_audit/tests/test_info_console.py` — page 0 / page 1 / never-link tests.
- `cyd/main/cyd_main.c` — `s_info_page`, INFO touch toggle, build `radar_sys_info_t`, freshness-overlay skip, build tag, pass `sys` at 4 call sites.

---

### Task 1: Plumbing + INFO page 0 (SYSTEM console)

Adds the struct + `sys` param, rewrites `draw_info` to render page 0 (page 1 is a header-only stub this task), the `--info` harness mode, and updates all call sites. Existing INFO tests keep passing.

**Files:**
- Modify: `components/simulacra_radar/radar_render.h`, `radar_render.c`
- Modify: `tools/radar_audit/render_dump.c`
- Modify: `cyd/main/cyd_main.c` (mechanical: add NULL `sys` to the 4 calls)
- Test: `tools/radar_audit/tests/test_info_console.py`

**Interfaces:**
- Produces: `radar_sys_info_t`; `radar_render_view(..., const exposure_t *expo, const radar_sys_info_t *sys, uint16_t sweep, ...)`; `static void draw_info(radar_gfx_t *g, const radar_wire_status_t *st, const radar_lib_info_t *lib, const radar_sys_info_t *sys)`.

- [ ] **Step 1: Write the failing test**

Create `tools/radar_audit/tests/test_info_console.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")

UINT32_MAX = 4294967295


def info(page=0, nodes=3, sigver=2, sigcount=12, linkage=4, libcount=40, libcap=128,
         cardmb=8, sdok=1, decoys=88, target=96, pop=44, uptime=47143):
    """Render RADAR_VIEW_INFO (2-page) and return the text strings drawn."""
    args = [EXE, "--info", page, nodes, sigver, sigcount, linkage, libcount, libcap,
            cardmb, sdok, decoys, target, pop, uptime]
    out = subprocess.check_output([str(x) for x in args], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class InfoSystemPage(unittest.TestCase):
    def test_sections_present(self):
        texts = info(page=0)
        for s in ("FLEET", "SIGNATURES", "STORAGE", "LINK", "SYSTEM"):
            self.assertIn(s, texts, f"missing section {s}; drew: {texts}")

    def test_values(self):
        texts = info(page=0, nodes=3, sigver=2, sigcount=12, cardmb=8, linkage=4)
        i = texts.index("nodes"); self.assertEqual(texts[i + 1], "3", f"drew: {texts}")
        self.assertIn("v2 (12)", texts, f"sig db wrong; drew: {texts}")
        self.assertIn("OK 8MB", texts, f"card wrong; drew: {texts}")
        self.assertIn("4s ago", texts, f"link wrong; drew: {texts}")
        self.assertIn("cydtest", texts, f"firmware tag missing; drew: {texts}")
        self.assertIn("TAP: LEGEND", texts, f"footer hint missing; drew: {texts}")

    def test_link_never(self):
        texts = info(page=0, linkage=UINT32_MAX)
        self.assertIn("never", texts, f"drew: {texts}")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: build FAILS (no `--info` mode / `radar_sys_info_t` undefined) or `InfoSystemPage` FAILS.

- [ ] **Step 3: Add `radar_sys_info_t` + extend the signature in the header**

In `components/simulacra_radar/radar_render.h`, add the struct near `radar_ctrl_info_t`:

```c
typedef struct {                 // CYD system/fleet snapshot for the INFO page
    uint8_t  node_count;         // meshing nodes
    uint16_t sig_ver;            // signature-DB version
    uint16_t sig_count;          // signatures loaded
    uint32_t link_age_s;         // seconds since last status; UINT32_MAX = never
    const char *build;           // firmware/build tag, e.g. "cyd v2 flood"
    uint8_t  page;               // INFO view: 0 = system console, 1 = legend
} radar_sys_info_t;
```

Add `#include <stdint.h>` is already present via existing includes. Update the signature:

```c
void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                       const radar_node_view_t *nodes, int node_count, int sel_node, int sel_threat,
                       const radar_lib_info_t *lib, const radar_ctrl_info_t *ctrl,
                       const exposure_t *expo, const radar_sys_info_t *sys, uint16_t sweep_deg,
                       uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx);
```

- [ ] **Step 4: Rewrite `draw_info` (page 0 full + page 1 stub) and update dispatch in `radar_render.c`**

Replace the existing `draw_info` with:

```c
static void draw_info(radar_gfx_t *g, const radar_wire_status_t *st,
                      const radar_lib_info_t *lib, const radar_sys_info_t *sys){
    if (sys && sys->page == 1) {          // page 1 = legend (filled in Task 2)
        draw_header(g, "LEGEND");
        return;
    }
    draw_header(g, "INFO");
    char v[24];
    row_section(g, 34, "FLEET");
    snprintf(v,sizeof v,"%u",(unsigned)(sys ? sys->node_count : 0)); row_kv(g,52,"nodes",v);
    snprintf(v,sizeof v,"%u",(unsigned)st->active_devices);          row_kv(g,68,"decoys",v);
    snprintf(v,sizeof v,"%u",(unsigned)st->active_target);           row_kv(g,84,"target",v);
    snprintf(v,sizeof v,"%u",(unsigned)st->pop_ewma);                row_kv(g,100,"real crowd",v);
    row_section(g, 118, "SIGNATURES");
    if (sys) { snprintf(v,sizeof v,"v%u (%u)",(unsigned)sys->sig_ver,(unsigned)sys->sig_count); row_kv(g,136,"sig db",v); }
    else     row_kv(g,136,"sig db","-");
    if (lib) { snprintf(v,sizeof v,"%u/%u",(unsigned)lib->lib_count,(unsigned)lib->lib_cap); row_kv(g,152,"shapes",v); }
    else     row_kv(g,152,"shapes","-");
    row_section(g, 170, "STORAGE");
    if (lib && lib->sd_ok) { snprintf(v,sizeof v,"OK %luMB",(unsigned long)lib->card_mb); row_kv(g,188,"card",v); }
    else                   row_kv(g,188,"card", lib ? "ABSENT" : "-");
    row_section(g, 206, "LINK");
    if (sys && sys->link_age_s != UINT32_MAX) { snprintf(v,sizeof v,"%lus ago",(unsigned long)sys->link_age_s); row_kv(g,224,"last status",v); }
    else                                       row_kv(g,224,"last status","never");
    row_section(g, 242, "SYSTEM");
    fmt_uptime(v,sizeof v,st->uptime_s);          row_kv(g,260,"uptime",v);
    row_kv(g,276,"firmware", (sys && sys->build) ? sys->build : "cyd");
    radar_gfx_text(g, 8, 298, "TAP: LEGEND", COL_ASH);
}
```

`UINT32_MAX` needs `<stdint.h>` (already included transitively). Update the dispatch call in `radar_render_view` (and add `sys` to the signature line):

```c
void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                       const radar_node_view_t *nodes, int node_count, int sel_node, int sel_threat,
                       const radar_lib_info_t *lib, const radar_ctrl_info_t *ctrl,
                       const exposure_t *expo, const radar_sys_info_t *sys, uint16_t sweep, uint16_t *band, int band_h, int w, int h,
                       radar_flush_fn flush, void *ctx){
    for(int y0=0;y0<h;y0+=band_h){ radar_gfx_t g={ .buf=band, .w=w, .y0=y0, .h=band_h };
        radar_gfx_clear(&g,COL_BG);
        if(view==RADAR_VIEW_HOME) draw_home(&g,st,nodes,node_count);
        else if(view==RADAR_VIEW_DETAIL) draw_detail(&g,st);
        else if(view==RADAR_VIEW_STATS) draw_stats(&g,st);
        else if(view==RADAR_VIEW_LIBRARY) draw_library(&g,lib);
        else if(view==RADAR_VIEW_CONTROL) draw_control(&g,ctrl);
        else if(view==RADAR_VIEW_INFO) draw_info(&g,st,lib,sys);
        else if(view==RADAR_VIEW_EXPOSURE) draw_exposure(&g,expo);
        else if(view==RADAR_VIEW_NODE) draw_node(&g,nodes,node_count,sel_node);
        else if(view==RADAR_VIEW_THREAT) draw_threat(&g,st,sel_threat);
        else draw_radar(&g,st,sweep);
        flush(y0, band_h, band, ctx); }
}
```

- [ ] **Step 5: Add the `--info` harness mode + fix existing calls in `render_dump.c`**

Add this block right after the `--threat` block (before `int view = ...`):

```c
    if (argc > 1 && strcmp(argv[1], "--info") == 0) {
        int a = 2;
        int page   = argc > a ? atoi(argv[a]) : 0; a++;
        int nodes  = argc > a ? atoi(argv[a]) : 0; a++;
        int sigver = argc > a ? atoi(argv[a]) : 0; a++;
        int sigcnt = argc > a ? atoi(argv[a]) : 0; a++;
        unsigned long linkage = argc > a ? strtoul(argv[a], 0, 10) : 0; a++;
        int libcount = argc > a ? atoi(argv[a]) : 0; a++;
        int libcap   = argc > a ? atoi(argv[a]) : 0; a++;
        int cardmb   = argc > a ? atoi(argv[a]) : 0; a++;
        int sdok     = argc > a ? atoi(argv[a]) : 0; a++;
        int decoys   = argc > a ? atoi(argv[a]) : 0; a++;
        int target   = argc > a ? atoi(argv[a]) : 0; a++;
        int pop      = argc > a ? atoi(argv[a]) : 0; a++;
        unsigned long uptime = argc > a ? strtoul(argv[a], 0, 10) : 0; a++;
        radar_wire_status_t st; memset(&st, 0, sizeof st);
        st.active_devices = (uint16_t)decoys; st.active_target = (uint8_t)target;
        st.pop_ewma = (uint16_t)pop; st.uptime_s = (uint32_t)uptime;
        radar_lib_info_t lib; memset(&lib, 0, sizeof lib);
        lib.sd_ok = sdok != 0; lib.card_mb = (uint32_t)cardmb;
        lib.lib_count = (uint16_t)libcount; lib.lib_cap = (uint16_t)libcap;
        radar_sys_info_t sys; memset(&sys, 0, sizeof sys);
        sys.node_count = (uint8_t)nodes; sys.sig_ver = (uint16_t)sigver;
        sys.sig_count = (uint16_t)sigcnt; sys.link_age_s = (uint32_t)linkage;
        sys.build = "cydtest"; sys.page = (uint8_t)page;
        static uint16_t iband[240 * 320];
        radar_render_view(RADAR_VIEW_INFO, &st, 0, 0, -1, -1, &lib, 0, 0, &sys, 0,
                          iband, 320, 240, 320, flush_noop, 0);
        return 0;
    }
```

Update the four existing `radar_render_view` calls to add `sys` (NULL, immediately after the `expo` arg):
- `--expo`: `radar_render_view(RADAR_VIEW_EXPOSURE, 0, 0, 0, -1, -1, 0, 0, &e, NULL, 0, eband, 320, 240, 320, flush_noop, 0);`
- `--node`: `radar_render_view(RADAR_VIEW_NODE, &st, nodes, 1, sel, -1, 0, 0, NULL, NULL, 0, nband, 320, 240, 320, flush_noop, 0);`
- `--threat`: `radar_render_view(RADAR_VIEW_THREAT, &st, 0, 0, -1, sel, 0, 0, NULL, NULL, 0, tband, 320, 240, 320, flush_noop, 0);`
- final: `radar_render_view((radar_view_t)view, &st, nodes, 1, -1, -1, &lib, &ctrl, NULL, NULL, 0, band, 320, 240, 320, flush_noop, NULL);`

Also update the header comment to note `--info <page> ...`.

- [ ] **Step 6: Update the 4 `radar_render_view` call sites in `cyd_main.c` (mechanical)**

Each call is `radar_render_view(ui.view, &agg, nv, nvc, sel_idx, sel_threat, &lib, &ctrl, (ui.view==RADAR_VIEW_EXPOSURE?&s_expo:NULL), sweep, ...)`. Insert `NULL` after the `(...&s_expo:NULL)` expo arg (4 occurrences):

```c
                        radar_render_view(ui.view, &agg, nv, nvc, sel_idx, sel_threat, &lib, &ctrl, (ui.view==RADAR_VIEW_EXPOSURE?&s_expo:NULL), NULL, sweep, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
```

(Behavior unchanged — INFO on device renders page 0 with `sys=NULL` until Task 3 wires the real `sys`.)

- [ ] **Step 7: Build + run tests to verify they pass**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: build succeeds; `InfoSystemPage` PASS; the pre-existing INFO assertions in `test_radar_render.py` (INFO header, `SYSTEM`, `firmware`, `13h 5m`) still PASS; all other suites PASS.

- [ ] **Step 8: Commit**

```bash
git add components/simulacra_radar/radar_render.h components/simulacra_radar/radar_render.c tools/radar_audit/render_dump.c tools/radar_audit/tests/test_info_console.py cyd/main/cyd_main.c
git commit -m "feat(cyd-info): radar_sys_info_t + INFO page 0 system console + sys plumb"
```

---

### Task 2: INFO page 1 (LEGEND)

**Files:**
- Modify: `components/simulacra_radar/radar_render.c`
- Test: `tools/radar_audit/tests/test_info_console.py`

**Interfaces:**
- Consumes: `posture_color`, `escalation_color`, `RADAR_POSTURE_*`, `ESCALATION_*`, `COL_*` (existing in `radar_render.c`).

- [ ] **Step 1: Write the failing tests**

Append to `tools/radar_audit/tests/test_info_console.py`:

```python
@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class InfoLegendPage(unittest.TestCase):
    def test_legend_header_and_sections(self):
        texts = info(page=1)
        self.assertIn("LEGEND", texts, f"drew: {texts}")
        for s in ("POSTURE", "ESCALATION", "HEALTH"):
            self.assertIn(s, texts, f"missing section {s}; drew: {texts}")

    def test_legend_labels(self):
        texts = info(page=1)
        for w in ("CLOAKED", "EXPOSED", "DARK", "HUNTED",
                  "NEW", "RECURRING", "PERSISTENT",
                  "CHANNEL", "DEGRADED", "LOW BATT", "SILENT"):
            self.assertIn(w, texts, f"missing legend label {w}; drew: {texts}")
        self.assertIn("TAP: SYSTEM", texts, f"footer hint missing; drew: {texts}")
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: `InfoLegendPage` FAILS (page 1 is a header-only stub).

- [ ] **Step 3: Implement the legend page**

In `components/simulacra_radar/radar_render.c`, replace the page-1 stub branch in `draw_info`:

```c
    if (sys && sys->page == 1) {          // page 1 = legend
        draw_header(g, "LEGEND");
        row_section(g, 32, "POSTURE");
        radar_gfx_text(g, 16, 48, "CLOAKED", posture_color(RADAR_POSTURE_CLOAKED));
        radar_gfx_text(g, 104, 48, "hidden in crowd", COL_ASH);
        radar_gfx_text(g, 16, 62, "EXPOSED", posture_color(RADAR_POSTURE_EXPOSED));
        radar_gfx_text(g, 104, 62, "no crowd", COL_ASH);
        radar_gfx_text(g, 16, 76, "DARK", posture_color(RADAR_POSTURE_DARK));
        radar_gfx_text(g, 104, 76, "decoys paused", COL_ASH);
        radar_gfx_text(g, 16, 90, "HUNTED", posture_color(RADAR_POSTURE_HUNTED));
        radar_gfx_text(g, 104, 90, "follower here", COL_ASH);
        row_section(g, 108, "ESCALATION");
        radar_gfx_fill_rect(g, 16, 126, 6, 6, escalation_color(ESCALATION_NEW));
        radar_gfx_text(g, 30, 124, "NEW", escalation_color(ESCALATION_NEW));
        radar_gfx_text(g, 120, 124, "this session", COL_ASH);
        radar_gfx_fill_rect(g, 16, 140, 6, 6, escalation_color(ESCALATION_RECURRING));
        radar_gfx_text(g, 30, 138, "RECURRING", escalation_color(ESCALATION_RECURRING));
        radar_gfx_text(g, 120, 138, "seen again", COL_ASH);
        radar_gfx_fill_rect(g, 16, 154, 6, 6, escalation_color(ESCALATION_PERSISTENT));
        radar_gfx_text(g, 30, 152, "PERSISTENT", escalation_color(ESCALATION_PERSISTENT));
        radar_gfx_text(g, 120, 152, "follower", COL_ASH);
        row_section(g, 170, "HEALTH");
        radar_gfx_text(g, 16, 186, "CHANNEL", COL_CHANNEL);
        radar_gfx_text(g, 104, 186, "healthy", COL_ASH);
        radar_gfx_text(g, 16, 200, "DEGRADED", COL_WARD);
        radar_gfx_text(g, 104, 200, "probe wedged", COL_ASH);
        radar_gfx_text(g, 16, 214, "LOW BATT", COL_WARD);
        radar_gfx_text(g, 104, 214, "battery low", COL_ASH);
        radar_gfx_text(g, 16, 228, "SILENT", COL_ASH);
        radar_gfx_text(g, 104, 228, "not reporting", COL_ASH);
        radar_gfx_text(g, 8, 298, "TAP: SYSTEM", COL_ASH);
        return;
    }
```

(`escalation_color` takes a `detect_escalation_t`; `ESCALATION_NEW`/`ESCALATION_RECURRING`/`ESCALATION_PERSISTENT` are the values already used by `draw_detail`/`draw_radar`. `posture_color` takes a `radar_posture_t`.)

- [ ] **Step 4: Build + run tests to verify they pass**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: `InfoLegendPage` + `InfoSystemPage` PASS; pre-existing suites still PASS.

- [ ] **Step 5: Commit**

```bash
git add components/simulacra_radar/radar_render.c tools/radar_audit/tests/test_info_console.py
git commit -m "feat(cyd-info): INFO page 1 LEGEND (posture/escalation/health, live colors)"
```

---

### Task 3: cyd_main integration (page toggle, sys build, freshness skip)

Wires INFO's two pages to the device: body-tap toggles, the real `radar_sys_info_t` is built each frame, and the freshness overlay stops covering INFO.

**Files:**
- Modify: `cyd/main/cyd_main.c`

**Interfaces:**
- Consumes: `radar_sys_info_t`; `radar_render_view(..., sys, ...)`; `fleet_status_count`, `s_sigdb_ver`, `s_sigdb_n`, `s_status_ms`.

- [ ] **Step 1: Add the build tag + INFO page state**

Near the top of `cyd/main/cyd_main.c` (after the includes, before `app_main`), add the compile-time build tag:

```c
#ifdef SIMULACRA_FLOCK_FLOOD
#define CYD_BUILD_TAG "cyd v2 flood"
#else
#define CYD_BUILD_TAG "cyd v2"
#endif
```

Near the NODE/THREAT statics, add:

```c
static uint8_t s_info_page;       // INFO view: 0 = system console, 1 = legend
```

- [ ] **Step 2: Build the `radar_sys_info_t` and pass it to the render calls**

In the render block, right after the `sel_threat` resolution added in sub-project B, build `sysinfo`:

```c
            radar_sys_info_t sysinfo = {
                .node_count = (uint8_t)fleet_status_count(&s_fleet),
                .sig_ver    = s_sigdb_ver,
                .sig_count  = (uint16_t)s_sigdb_n,
                .link_age_s = s_status_ms ? (now - s_status_ms) / 1000 : UINT32_MAX,
                .build      = CYD_BUILD_TAG,
                .page       = s_info_page,
            };
```

Replace the `NULL` sys arg (added in Task 1) with `&sysinfo` in all four `radar_render_view` calls:

```c
                        radar_render_view(ui.view, &agg, nv, nvc, sel_idx, sel_threat, &lib, &ctrl, (ui.view==RADAR_VIEW_EXPOSURE?&s_expo:NULL), &sysinfo, sweep, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
```

`UINT32_MAX` requires `<stdint.h>` (already included in cyd_main).

- [ ] **Step 3: INFO body-tap toggles the page**

In the `edge && !modal_open` touch dispatch chain, add a `RADAR_VIEW_INFO` branch before the generic `else` (INFO currently routes through it → any tap returns HOME):

```c
            } else if (ui.view == RADAR_VIEW_INFO) {
                if (ty < 26) { radar_ui_on_input(&ui, now); }   // "< BACK" strip -> HOME
                else { s_info_page ^= 1; radar_ui_note_input(&ui, now); }   // body -> flip page
            }
```

- [ ] **Step 4: Skip the freshness overlay on INFO**

In the `SIMULACRA_FLEET_PROVISION` render block's overlay chain, add an INFO arm alongside HOME/EXPOSURE/NODE:

```c
                    else if (ui.view == RADAR_VIEW_INFO)     { /* INFO shows link age in its own row */ }
```

In the `#else` block's overlay guard, exclude INFO:

```c
                    if (ui.view != RADAR_VIEW_HOME && ui.view != RADAR_VIEW_EXPOSURE && ui.view != RADAR_VIEW_NODE && ui.view != RADAR_VIEW_INFO) draw_freshness_overlay(band, now);
```

- [ ] **Step 5: Compile-verify the CYD firmware**

Run (PowerShell, IDF 5.4 — see `private/CYD-BUILD-FLOW.md`):

```powershell
$py='C:\Program Files\Python312'; $env:PATH="$py;$py\Scripts;$env:PATH"
$idf='$env:USERPROFILE\esp\v5.4\esp-idf'; $env:IDF_PATH=$idf; . ($idf+'\export.ps1') *>$null
idf.py -C cyd build
```

Expected: `Project build complete`, `cyd/build/simulacra_cyd.bin` regenerated, exit 0.

- [ ] **Step 6: Compile-verify the c5 decoy (shared component)**

Run (PowerShell, IDF 5.5; root is `esp32c5`):

```powershell
$py='C:\Program Files\Python312'; $env:PATH="$py;$py\Scripts;$env:PATH"
$idf='$env:USERPROFILE\esp\v5.5\esp-idf'; $env:IDF_PATH=$idf; . ($idf+'\export.ps1') *>$null
idf.py build
```

Expected: `Project build complete` (component compiles with the new signature; the pre-existing `threat_color` unused-function warning is unrelated).

- [ ] **Step 7: Re-run the host suite**

Run: `powershell -File tools/radar_audit/run.ps1`
Expected: all `test_info_console.py` classes + the pre-existing suites PASS.

- [ ] **Step 8: Commit**

```bash
git add cyd/main/cyd_main.c
git commit -m "feat(cyd-info): INFO page toggle + build radar_sys_info_t + freshness skip"
```

---

## Self-Review

**Spec coverage:**
- `radar_sys_info_t` + `sys` param + ripple (render_dump ×4 calls + `--info`; cyd_main ×4) → Task 1 Steps 3/5/6, Task 3 Step 2. ✓
- Pass `lib` to `draw_info` → Task 1 Step 4 dispatch. ✓
- Page 0 SYSTEM console (FLEET/SIGNATURES/STORAGE/LINK/SYSTEM + footer) → Task 1 Step 4 + tests. ✓
- `sys==NULL` / `lib==NULL` / `link never` graceful → Task 1 Step 4 + `test_link_never`. ✓
- Existing INFO tests preserved (SYSTEM/firmware/uptime on page 0) → Task 1 Step 4 keeps a SYSTEM section w/ firmware + `fmt_uptime`. ✓
- Page 1 LEGEND (POSTURE/ESCALATION/HEALTH, live colors, footer) → Task 2 + tests. ✓
- Body-tap toggle + header→HOME → Task 3 Step 3. ✓
- Build `radar_sys_info_t` from cyd state + build tag → Task 3 Steps 1/2. ✓
- Freshness-overlay skip for INFO → Task 3 Step 4. ✓
- Compile-verify cyd/c5 → Task 3 Steps 5–6. ✓

**Placeholder scan:** none — every code step shows complete code.

**Type consistency:** `draw_info(radar_gfx_t*, const radar_wire_status_t*, const radar_lib_info_t*, const radar_sys_info_t*)` consistent Tasks 1–2. `sys` inserted after `expo` at every call site (render_dump `--expo`/`--node`/`--threat`/`--info`/final; cyd_main ×4). `radar_sys_info_t` field names (`node_count`/`sig_ver`/`sig_count`/`link_age_s`/`build`/`page`) identical in the struct, the harness, and cyd_main. `posture_color(radar_posture_t)` / `escalation_color(detect_escalation_t)` args match their definitions. Test tokens (`"v2 (12)"`, `"OK 8MB"`, `"4s ago"`, `"cydtest"`, `"TAP: LEGEND"`, `"TAP: SYSTEM"`, legend labels) match the format strings.

**Fit check:** page 0 last content `firmware` at y=276, footer at y=298 (ends ~306); page 1 last content `SILENT` at y=228, footer at y=298. Both < 318. ✓

## Rollout

Pure CYD firmware change. Flash the CYD only (`idf.py -C cyd -p <PORT> flash`); decoys untouched. No wire-version bump.
