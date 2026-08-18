# NODES List Page + Fleet-Cap Raise Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the CYD's HOME fleet-strip (hard-capped at 3 reachable nodes, silently stranding a
4th tracked node with no way to open its detail page), replace it with a NODES list page reachable
from HOME's icon grid, and raise the fleet-tracking ceiling from 4 to 8.

**Architecture:** Three code changes layered in dependency order: (1) raise `FLEET_STATUS_MAX`, (2)
add a new `RADAR_VIEW_NODES` render view (pure, host-testable, additive — doesn't touch HOME yet),
(3) remove the HOME strip, grow the icon grid to 8 tiles, and fix the actual reachability bug (a
hard-coded 3-slot shortlist array that both the strip's tap-mapping and the NODE detail page's
prev/next cycling depend on). A final task updates the published wiki and verifies on real hardware.

**Tech Stack:** C (ESP-IDF firmware, shared `components/simulacra_radar` render library), Python
`unittest` + a small MSVC/POSIX-buildable C host harness (`tools/radar_audit`) that compiles the
real firmware source on the host for fast, hardware-free testing.

## Global Constraints

- `FLEET_STATUS_MAX` (`cyd/main/fleet_status.h`) goes from `4` to `8`, matching `FLEET_NODE_CAP`
  (`main/fleet.h`, already `8`) so the decoy-side self-exclusion table and the CYD's tracking
  ceiling agree.
- NODES list page shows every tracked node on one screen — **no pagination**. Row layout is fixed:
  start `y=36`, row height `24px` (8 rows → `y=36..228`, comfortably inside the page body before the
  footer divider at `y=298`). This exact geometry is a contract between Task 2 (renders it) and
  Task 3 (must compute the same row a tap landed in) — do not let them drift.
- HOME's icon grid moves up to start at `y=32` (right after the top bar) and grows from 7 to 8
  tiles in the same 2-col × 4-row layout, each row `66px` tall (`(296-32)/4 = 66`) instead of the
  old `48px` (the freed strip space distributed evenly across the 4 rows). New tile: **NODES**, in
  the previously-empty 8th grid slot (row 3, col 1 — next to EXPOSURE).
- `cyd_main.c`'s touch-dispatch logic (where the actual reachability bug lives) is **not** compiled
  by any host test harness — `tools/radar_audit`'s Makefile only builds `radar_ui.c`,
  `fleet_status.c`, `radar_render.c`, and `exposure.c`. Task 3's dispatch changes are verified by
  careful code correctness (exact line-level diffs given in this plan) plus the final on-device
  flash test in Task 4 — there is no host unit test for touch dispatch anywhere in this project
  today, so this isn't a gap specific to this feature.
- No HOME fleet-summary line (explicitly rejected in the approved design) — HOME becomes top bar +
  8-tile grid only.

---

### Task 1: Raise the fleet-tracking cap to 8

**Files:**
- Modify: `cyd/main/fleet_status.h:9`
- Modify: `tools/radar_audit/tests/test_fleet_status.py`

**Interfaces:**
- Produces: `FLEET_STATUS_MAX == 8` (consumed implicitly by every other task — `radar_node_view_t
  nv[FLEET_STATUS_MAX]` in `cyd_main.c:1127`, `fleet_status_t.nodes[FLEET_STATUS_MAX]`, and the new
  `s_node_ids[FLEET_STATUS_MAX]` array Task 3 introduces all size off this one constant, no other
  code changes needed for the cap itself).

- [ ] **Step 1: Write the failing test**

Add to `tools/radar_audit/tests/test_fleet_status.py`, inside the existing `class FS(unittest.TestCase):`
block (after `test_upsert_counts_distinct_nodes`):

```python
    def test_cap_holds_at_eight_nodes(self):
        # push 9 distinct node ids; FLEET_STATUS_MAX=8 means the table holds only the first 8 it
        # ever sees (fleet_status_upsert drops a genuinely-new id once full -- see the "table full:
        # drop" comment in fleet_status.c). This is a regression test for the cap value itself, not
        # for eviction policy (that LRU-recycling of ids lives in cyd_main.c's node_id_for(), which
        # pre-empts fleet_status.c ever seeing more than FLEET_STATUS_MAX distinct ids in practice --
        # not host-testable, see the plan's Global Constraints).
        cmds = " ".join(f"up {i} {i+1}" for i in range(9))  # ids 0..8, active_devices i+1
        self.assertEqual(run(f"{cmds} count"), ["8"])
```

- [ ] **Step 2: Run test to verify it fails**

Build `fleet_dump` directly (it only needs `fleet_status.c`, not the full `run.ps1`/MSVC toolchain
dance the render-related tools need):

```powershell
cd tools\radar_audit
gcc -O2 -Wall -Wno-unused-parameter -I..\..\cyd\main -I..\..\components\simulacra_radar fleet_dump.c ..\..\cyd\main\fleet_status.c -o fleet_dump.exe
python -m unittest tests.test_fleet_status -v
```

(On Linux/CI, `make fleet_dump` from `tools/radar_audit/` does the same thing via the Makefile.)

Expected: `test_cap_holds_at_eight_nodes` FAILS — with `FLEET_STATUS_MAX` still `4`, `count` reads
`4`, not `8`.

- [ ] **Step 3: Bump the constant**

In `cyd/main/fleet_status.h`, change:
```c
#define FLEET_STATUS_MAX       4
```
to:
```c
#define FLEET_STATUS_MAX       8
```

- [ ] **Step 4: Rebuild and run test to verify it passes**

```powershell
cd tools\radar_audit
gcc -O2 -Wall -Wno-unused-parameter -I..\..\cyd\main -I..\..\components\simulacra_radar fleet_dump.c ..\..\cyd\main\fleet_status.c -o fleet_dump.exe
python -m unittest tests.test_fleet_status -v
```

Expected: all tests PASS, including the new one.

- [ ] **Step 5: Fix a comment made stale by this change**

`tools/radar_audit/tests/test_fleet_status.py`'s `class Prune` docstring currently reads:

```python
class Prune(unittest.TestCase):
    """Long-gone nodes must be retired, or their SILENT cards occupy HOME's three card slots and
    push a LIVE node off the display -- a board that looks dropped while it is still meshing.
    Decoys re-randomise their MAC on every boot, so each reboot leaves one of these behind."""
```

"HOME's three card slots" describes the strip this whole feature removes (Task 3). Change it to:

```python
class Prune(unittest.TestCase):
    """Long-gone nodes must be retired, or their SILENT records occupy fleet-tracking slots and
    push a LIVE node out of the table -- a board that looks dropped while it is still meshing.
    Decoys re-randomise their MAC on every boot, so each reboot leaves one of these behind."""
```

- [ ] **Step 6: Commit**

```bash
git add cyd/main/fleet_status.h tools/radar_audit/tests/test_fleet_status.py
git commit -m "feat(cyd): raise FLEET_STATUS_MAX 4 -> 8

Matches FLEET_NODE_CAP (main/fleet.h), which is already 8 -- the
decoy-side self-exclusion table and the CYD's tracking ceiling now
agree. Also fixes a test-comment reference to the fleet strip this
project's next task removes."
```

---

### Task 2: NODES list render view (additive, host-testable)

**Files:**
- Modify: `components/simulacra_radar/radar_ui.h`
- Modify: `components/simulacra_radar/radar_render.c`
- Modify: `tools/radar_audit/render_dump.c`
- Test: `tools/radar_audit/tests/test_render_nodeslist.py` (new file)

**Interfaces:**
- Consumes: `radar_node_view_t` (existing type, `radar_render.h:33` — `{ uint8_t id; const
  radar_wire_status_t *st; bool alive; uint32_t age_s; }`), `radar_render_view()` (existing
  function, `radar_render.h:39-43` — signature unchanged by this task).
- Produces: `RADAR_VIEW_NODES` (new enum value in `radar_view_t`, `radar_ui.h`), a new internal
  `static void draw_nodes_list(radar_gfx_t *g, const radar_node_view_t *nodes, int node_count)` in
  `radar_render.c` (not exported — only reachable via `radar_render_view(RADAR_VIEW_NODES, ...)`).
  Row geometry contract for Task 3: **row `i` occupies `y = 36 + i*24` to `y = 36 + i*24 + 24`**.

- [ ] **Step 1: Add the new view enum value**

In `components/simulacra_radar/radar_ui.h`, change:
```c
typedef enum { RADAR_VIEW_HOME = 0, RADAR_VIEW_RADAR, RADAR_VIEW_DETAIL, RADAR_VIEW_STATS,
               RADAR_VIEW_LIBRARY, RADAR_VIEW_CONTROL, RADAR_VIEW_INFO, RADAR_VIEW_EXPOSURE,
               RADAR_VIEW_NODE, RADAR_VIEW_THREAT,
               RADAR_VIEW_COUNT } radar_view_t;
```
to:
```c
typedef enum { RADAR_VIEW_HOME = 0, RADAR_VIEW_RADAR, RADAR_VIEW_DETAIL, RADAR_VIEW_STATS,
               RADAR_VIEW_LIBRARY, RADAR_VIEW_CONTROL, RADAR_VIEW_INFO, RADAR_VIEW_EXPOSURE,
               RADAR_VIEW_NODES, RADAR_VIEW_NODE, RADAR_VIEW_THREAT,
               RADAR_VIEW_COUNT } radar_view_t;
```
(`RADAR_VIEW_NODES` — the list — sits next to `RADAR_VIEW_NODE` — the per-node detail page — on
purpose; deliberately distinct names, list vs. detail. This shifts `RADAR_VIEW_NODE`'s and
`RADAR_VIEW_THREAT`'s ordinal values by one; confirmed safe — nothing in this codebase dispatches to
either by a hardcoded numeric literal, both are always reached by name or via the `--node`/`--threat`
render_dump flags.)

- [ ] **Step 2: Add the render_dump test harness support for the new view (test infrastructure first)**

In `tools/radar_audit/render_dump.c`, add a new `--nodeslist` mode. Insert this block right after the
existing `--node` block (after its closing `}` around line 92, before the `--threat` block):

```c
    if (argc > 1 && strcmp(argv[1], "--nodeslist") == 0) {
        int a = 2;
        int count = argc > a ? atoi(argv[a]) : 0; a++;
        if (count > 8) count = 8;
        static radar_wire_status_t sts[8];
        static radar_node_view_t lv[8];
        memset(sts, 0, sizeof sts);
        for (int i = 0; i < count; i++) {
            int id     = argc > a ? atoi(argv[a]) : i; a++;
            int alive  = argc > a ? atoi(argv[a]) : 1; a++;
            int active = argc > a ? atoi(argv[a]) : 0; a++;
            int batmv  = argc > a ? atoi(argv[a]) : 0; a++;
            sts[i].active_devices = (uint16_t)active;
            sts[i].battery_mv = (uint16_t)batmv;
            sts[i].battery_pct = 0xFF;
            lv[i].id = (uint8_t)id; lv[i].st = &sts[i]; lv[i].alive = alive != 0; lv[i].age_s = 0;
        }
        static uint16_t lband[240 * 320];
        radar_render_view(RADAR_VIEW_NODES, NULL, lv, count, -1, -1, 0, 0, NULL, NULL, 0,
                          lband, 320, 240, 320, flush_noop, 0);
        return 0;
    }
```

Also update the usage comment at the top of the file (line 7) to mention the new mode:
```c
//   view: 0 HOME 1 RADAR 2 DETAIL 3 STATS 4 LIBRARY 5 CONTROL 6 INFO 8 NODE (via --node) 9 THREAT (via --threat)
```
becomes:
```c
//   view: 0 HOME 1 RADAR 2 DETAIL 3 STATS 4 LIBRARY 5 CONTROL 6 INFO
//   NODES via --nodeslist <count> [id alive active_devices battery_mv]...
//   NODE via --node, THREAT via --threat
```

At this point the file won't compile yet — `draw_nodes_list` and `RADAR_VIEW_NODES`'s dispatch
branch don't exist. That's expected; the next steps add them.

- [ ] **Step 3: Write the failing test**

Create `tools/radar_audit/tests/test_render_nodeslist.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")


def run(args):
    return subprocess.check_output([EXE] + args, text=True).splitlines()


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class NodesList(unittest.TestCase):
    def test_eight_nodes_renders_eight_id_labels(self):
        args = ["--nodeslist", "8"]
        for i in range(8):
            args += [str(i), "1", str(10 + i), "0"]   # id=i alive=1 active=10+i battery=0(USB)
        out = run(args)
        for i in range(8):
            self.assertTrue(any(f"N{i}" in line for line in out), f"missing row for node {i}: {out}")

    def test_dead_node_shows_silent(self):
        out = run(["--nodeslist", "1", "3", "0", "0", "0"])   # id=3 alive=0
        self.assertTrue(any("SILENT" in line for line in out))

    def test_alive_node_shows_channel(self):
        out = run(["--nodeslist", "1", "0", "1", "5", "0"])   # id=0 alive=1, no low-batt/degraded flags
        self.assertTrue(any("CHANNEL" in line for line in out))

    def test_zero_nodes_shows_empty_state(self):
        out = run(["--nodeslist", "0"])
        self.assertTrue(any("no nodes" in line.lower() for line in out))

    def test_battery_percent_rendered_when_present(self):
        out = run(["--nodeslist", "1", "0", "1", "5", "3700"])   # battery_mv=3700, battery_pct left 0xFF -> voltage form
        self.assertTrue(any("3.70V" in line or "3.7" in line for line in out))
```

- [ ] **Step 4: Run test to verify it fails to build**

```powershell
cd tools\radar_audit
.\run.ps1 -Rebuild
```

Expected: build FAILS — `RADAR_VIEW_NODES` dispatch and `draw_nodes_list` don't exist yet in
`radar_render.c` (the enum value itself exists from Step 1, but nothing renders it).

- [ ] **Step 5: Add a shared node-health helper, then `draw_nodes_list` using it**

`draw_node` already derives a node's health word/color inline (`radar_render.c:379-383`), and the
old strip block (removed in Task 3) had its own copy of the exact same logic. Rather than adding a
third copy for the new list view, factor it into one small helper first.

In `components/simulacra_radar/radar_render.c`, add this helper above `draw_node`:

```c
static void node_health(const radar_node_view_t *nv, uint16_t *color, const char **word){
    bool alive = nv->alive;
    bool low_batt = alive && (nv->st->flags & 0x08);
    bool degraded = alive && (nv->st->flags & 0x04);
    *color = !alive ? COL_ASH : (low_batt || degraded) ? COL_WARD : COL_CHANNEL;
    *word  = !alive ? "SILENT" : low_batt ? "LOW BATT" : degraded ? "DEGRADED" : "CHANNEL";
}
```

Update `draw_node` to use it — replace its existing inline derivation:
```c
    bool alive = nv->alive;
    bool low_batt = alive && (st->flags & 0x08);
    bool degraded = alive && (st->flags & 0x04);
    uint16_t sc = !alive ? COL_ASH : (low_batt || degraded) ? COL_WARD : COL_CHANNEL;
    const char *health = !alive ? "SILENT" : low_batt ? "LOW BATT" : degraded ? "DEGRADED" : "CHANNEL";
    radar_gfx_text(g, 8, 32, health, sc);
```
with:
```c
    bool alive = nv->alive;
    uint16_t sc; const char *health;
    node_health(nv, &sc, &health);
    radar_gfx_text(g, 8, 32, health, sc);
```
(`alive` is still needed a few lines below for the `if (!alive) { ...seen Ns ago... }` branch, so
keep that one line.)

Then add `draw_nodes_list` itself, placed after `draw_node` (conceptually related — the list that
leads into that detail page):

```c
static void draw_nodes_list(radar_gfx_t *g, const radar_node_view_t *nodes, int node_count){
    draw_header(g, "NODES");
    if (node_count <= 0) {
        radar_gfx_text(g, 16, 40, "no nodes reporting", COL_ASH);
        return;
    }
    for (int i = 0; i < node_count && i < 8; i++) {
        int y = 36 + i * 24;
        const radar_node_view_t *nv = &nodes[i];
        uint16_t sc; const char *health;
        node_health(nv, &sc, &health);
        radar_gfx_fill_rect(g, 8, y + 2, 6, 6, sc);
        char id[8]; snprintf(id, sizeof id, "N%u", (unsigned)nv->id);
        radar_gfx_text(g, 18, y, id, COL_BONE);
        radar_gfx_text(g, 42, y, health, sc);
        if (nv->alive) {
            const radar_wire_status_t *st = nv->st;
            char cnt[8]; snprintf(cnt, sizeof cnt, "%u", (unsigned)st->active_devices);
            radar_gfx_text(g, 150 - (int)strlen(cnt) * 8, y, cnt, COL_BONE);
            if (st->battery_mv) {
                char b[16];
                if (st->battery_pct != 0xFF)
                    snprintf(b, sizeof b, "%u%%", (unsigned)st->battery_pct);
                else
                    snprintf(b, sizeof b, "%u.%02uV", (unsigned)(st->battery_mv / 1000),
                             (unsigned)((st->battery_mv % 1000) / 10));
                bool low_batt = (st->flags & 0x08) != 0;   // recomputed here: node_health() doesn't expose it separately
                radar_gfx_text(g, 224 - (int)strlen(b) * 8, y, b, low_batt ? COL_WARD : COL_ASH);
            }
        }
    }
}
```

- [ ] **Step 6: Wire the dispatch branch**

In `radar_render_view` (`radar_render.c:457-475`), add one more `else if` branch. Change:
```c
        else if(view==RADAR_VIEW_NODE) draw_node(&g,nodes,node_count,sel_node);
```
to:
```c
        else if(view==RADAR_VIEW_NODES) draw_nodes_list(&g,nodes,node_count);
        else if(view==RADAR_VIEW_NODE) draw_node(&g,nodes,node_count,sel_node);
```

- [ ] **Step 7: Rebuild and run tests to verify they pass**

```powershell
cd tools\radar_audit
.\run.ps1 -Rebuild
```

Expected: build succeeds, all tests pass (existing suite + the 5 new `NodesList` tests). Confirm the
full count in the output tail (should read strictly more tests than before Task 2, all `OK`).

- [ ] **Step 8: Commit**

```bash
git add components/simulacra_radar/radar_ui.h components/simulacra_radar/radar_render.c \
        tools/radar_audit/render_dump.c tools/radar_audit/tests/test_render_nodeslist.py
git commit -m "feat(cyd): add RADAR_VIEW_NODES list render view

Additive only -- not wired into HOME's grid yet (next task). Renders
one row per tracked node (id, health, live decoy count, battery),
matching the at-a-glance fields the fleet strip showed. Factored a
node_health() helper out of the near-duplicate logic in draw_node
while touching this code, rather than adding a third copy."
```

---

### Task 3: Remove the HOME strip, wire NODES into the grid, fix the reachability bug

**Files:**
- Modify: `components/simulacra_radar/radar_render.c` (`draw_home`)
- Modify: `cyd/main/cyd_main.c`

**Interfaces:**
- Consumes: `RADAR_VIEW_NODES` (Task 2), row geometry contract from Task 2 (`y = 36 + i*24`,
  8-row max).
- Produces: nothing further downstream — this is the last code task; Task 4 is docs + hardware
  verification only.

- [ ] **Step 1: Remove the fleet-strip render block from `draw_home`**

In `components/simulacra_radar/radar_render.c`, `draw_home` currently has this signature and strip
block (`radar_render.c:223-263`):
```c
static void draw_home(radar_gfx_t *g, const radar_wire_status_t *st, const radar_node_view_t *nodes, int nc){
    radar_gfx_clear(g, COL_VOID);
    radar_gfx_fill_rect(g, 0, 0, 240, 26, COL_CRYPT);
    radar_gfx_hline(g, 0, 239, 26, COL_EDGE);
    radar_sigil_draw(g, SIGIL_CIRCLE, 12, 13, 7, COL_ARCANE);
    radar_gfx_text(g, 26, 9, "SIMULACRA", COL_BONE);
    radar_posture_t p = radar_posture(st);
    const char *pl = posture_label(p);
    int px = 232 - (int)strlen(pl) * 8;
    radar_gfx_text(g, px, 9, pl, posture_color(p));
    radar_gfx_text(g, px - 8 - 6 * 8, 9, "STATUS", COL_ASH);
    int nsurv=0;
    for(uint8_t i=0;i<st->threat_count;i++) if(is_surveil_cat(st->threats[i].category)) nsurv++;
    if(nsurv>0){ char sb[16]; snprintf(sb,sizeof sb,"!%d",nsurv); radar_gfx_text(g, 100, 9, sb, COL_HUNTER); }
    int cols = nc < 1 ? 0 : (nc > 3 ? 3 : nc);
    for(int i=0;i<cols;i++){
        int x=i*80, y=30;
        radar_gfx_fill_rect(g, x+2, y, 76, 70, COL_CRYPT);
        bool alive = nodes[i].alive;
        bool low_batt = alive && (nodes[i].st->flags & 0x08);
        bool degraded = alive && (nodes[i].st->flags & 0x04);
        uint16_t sc = !alive ? COL_ASH : (low_batt || degraded) ? COL_WARD : COL_CHANNEL;
        const char *health = !alive ? "SILENT" : low_batt ? "LOW BATT" : degraded ? "DEGRADED" : "CHANNEL";
        char b[12]; snprintf(b,sizeof b,"N%u",(unsigned)nodes[i].id); radar_gfx_text(g, x+8, y+6, b, COL_BONE);
        radar_gfx_fill_rect(g, x+68, y+8, 4, 4, sc);
        snprintf(b,sizeof b,"%u",(unsigned)(alive?nodes[i].st->active_devices:0)); radar_gfx_text(g, x+8, y+24, b, COL_BONE);
        if (alive && nodes[i].st->battery_mv) {
            uint16_t mv = nodes[i].st->battery_mv; uint8_t pc = nodes[i].st->battery_pct;
            if (pc != 0xFF) snprintf(b,sizeof b,"%u%% %u.%01uV",(unsigned)pc,(unsigned)(mv/1000),(unsigned)((mv%1000)/100));
            else            snprintf(b,sizeof b,"%u.%02uV",(unsigned)(mv/1000),(unsigned)((mv%1000)/10));
            radar_gfx_text(g, x+8, y+40, b, low_batt ? COL_WARD : COL_ASH);
        }
        radar_gfx_text(g, x+8, y+54, health, sc);
    }
    static const sigil_id_t sig[7]={SIGIL_CIRCLE,SIGIL_HUNTER,SIGIL_LIVING,SIGIL_RITE,SIGIL_WARD,SIGIL_GRIMOIRE,SIGIL_CIRCLE};
    static const char *lbl[7]={"RADAR","FOLLOWERS","DECOYS","CONTROL","LIBRARY","INFO","EXPOSURE"};
    for(int i=0;i<7;i++){
        int cx=(i%2)*120, cy=104+(i/2)*48;
        radar_gfx_fill_rect(g, cx+1, cy+1, 118, 46, COL_CRYPT);
        radar_sigil_draw(g, sig[i], cx+18, cy+23, 10, COL_ARCANE);
        radar_gfx_text(g, cx+36, cy+19, lbl[i], COL_BONE);
    }
    radar_gfx_hline(g, 0, 239, 298, COL_EDGE);
    radar_gfx_text(g, 6, 304, "TAP AN ICON TO OPEN", COL_ASH);
}
```

Replace the whole function with:
```c
static void draw_home(radar_gfx_t *g, const radar_wire_status_t *st){
    radar_gfx_clear(g, COL_VOID);
    radar_gfx_fill_rect(g, 0, 0, 240, 26, COL_CRYPT);
    radar_gfx_hline(g, 0, 239, 26, COL_EDGE);
    radar_sigil_draw(g, SIGIL_CIRCLE, 12, 13, 7, COL_ARCANE);
    radar_gfx_text(g, 26, 9, "SIMULACRA", COL_BONE);
    radar_posture_t p = radar_posture(st);
    const char *pl = posture_label(p);
    int px = 232 - (int)strlen(pl) * 8;
    radar_gfx_text(g, px, 9, pl, posture_color(p));
    radar_gfx_text(g, px - 8 - 6 * 8, 9, "STATUS", COL_ASH);
    int nsurv=0;
    for(uint8_t i=0;i<st->threat_count;i++) if(is_surveil_cat(st->threats[i].category)) nsurv++;
    if(nsurv>0){ char sb[16]; snprintf(sb,sizeof sb,"!%d",nsurv); radar_gfx_text(g, 100, 9, sb, COL_HUNTER); }
    static const sigil_id_t sig[8]={SIGIL_CIRCLE,SIGIL_HUNTER,SIGIL_LIVING,SIGIL_RITE,SIGIL_WARD,SIGIL_GRIMOIRE,SIGIL_CIRCLE,SIGIL_LIVING};
    static const char *lbl[8]={"RADAR","FOLLOWERS","DECOYS","CONTROL","LIBRARY","INFO","EXPOSURE","NODES"};
    for(int i=0;i<8;i++){                                          // 4 rows @ 66px, grid reclaims the old strip's space
        int cx=(i%2)*120, cy=32+(i/2)*66;
        radar_gfx_fill_rect(g, cx+1, cy+1, 118, 64, COL_CRYPT);
        radar_sigil_draw(g, sig[i], cx+18, cy+29, 10, COL_ARCANE);
        radar_gfx_text(g, cx+36, cy+25, lbl[i], COL_BONE);
    }
    radar_gfx_hline(g, 0, 239, 298, COL_EDGE);
    radar_gfx_text(g, 6, 304, "TAP AN ICON TO OPEN", COL_ASH);
}
```

- [ ] **Step 2: Update `draw_home`'s call site**

In `radar_render_view` (`radar_render.c`), change:
```c
        if(view==RADAR_VIEW_HOME) draw_home(&g,st,nodes,node_count);
```
to:
```c
        if(view==RADAR_VIEW_HOME) draw_home(&g,st);
```

- [ ] **Step 3: Update the render_dump HOME test invocation**

`render_dump.c`'s generic bottom-of-main path (used for the bare-numeric HOME/RADAR/DETAIL/STATS/
LIBRARY/CONTROL/INFO views) still calls `radar_render_view` with a `nodes`/`node_count` pair that
`draw_home` no longer reads for view 0 — that's fine, those args are simply ignored for HOME now
and still used correctly for the other views in that same generic path. No change needed here; this
step exists only to confirm you checked it (verify by re-reading `render_dump.c`'s final `main`
block around line 206-211 — the call already passes `nodes, 1` unconditionally, which still
compiles and still serves DETAIL/STATS/etc. correctly).

- [ ] **Step 4: Run the render_dump host tests to catch any HOME-related regression**

```powershell
cd tools\radar_audit
.\run.ps1 -Rebuild
```

Expected: build succeeds (draw_home's signature change is self-contained), all tests still pass. If
any existing test asserted specific strip-related "TXT" output for HOME (grep
`tools/radar_audit/tests/*.py` for `"N0"` or similar in a HOME-view context to check), update or
remove that assertion — the strip's text output no longer exists. Search first with:
```bash
grep -rn "render_dump.*\bexe.*0\b\|HOME" tools/radar_audit/tests/*.py
```
If nothing references HOME's old strip output specifically, no test changes are needed here.

- [ ] **Step 5: Rename and un-truncate the id-shortlist array (the actual bug fix)**

In `cyd/main/cyd_main.c`, change (around line 241-243):
```c
static uint8_t s_sel_node;        // NODE view: id of the node being inspected
static uint8_t s_home_ids[3];     // ids of the (<=3) node cards HOME last rendered, left->right
static int     s_home_n;          // how many of s_home_ids are valid
```
to:
```c
static uint8_t s_sel_node;        // NODE view: id of the node being inspected
static uint8_t s_node_ids[FLEET_STATUS_MAX];  // ids of the (<=FLEET_STATUS_MAX) tracked nodes, nv[] order
static int     s_node_n;                       // how many of s_node_ids are valid
```

- [ ] **Step 6: Fix the population site (this line is what actually caused the bug)**

Around `cyd_main.c:1151-1153`, change:
```c
            // Record the strip ids HOME will draw (<=3), for the next frame's card-tap mapping.
            s_home_n = nvc > 3 ? 3 : nvc;
            for (int i = 0; i < s_home_n; i++) s_home_ids[i] = nv[i].id;
```
to:
```c
            // Record every currently-tracked node id, for the NODES list and NODE-cycling taps.
            s_node_n = nvc;
            for (int i = 0; i < s_node_n; i++) s_node_ids[i] = nv[i].id;
```

- [ ] **Step 7: Remove the HOME fleet-strip touch-dispatch branch and grow the grid mapping**

Around `cyd_main.c:942-960`, change:
```c
        if (edge && !modal_open) {
            if (ui.view == RADAR_VIEW_HOME) {
                // HOME sigil grid -> jump to that view. Geometry mirrors draw_home: 2 cols split at
                // x=120, 3 rows of 64px starting at y=104 (CIRCLE/HUNTERS / LIVING/RITES / WARDS/GRIMOIRE).
                // The fleet strip (y 30..100) taps into the live radar. Topbar / gaps just keep awake.
                static const radar_view_t GRID[7] = {          // 4 rows @ 48px (see draw_home)
                    RADAR_VIEW_RADAR,   RADAR_VIEW_DETAIL,     // CIRCLE   HUNTERS
                    RADAR_VIEW_STATS,   RADAR_VIEW_CONTROL,    // LIVING   RITES
                    RADAR_VIEW_LIBRARY, RADAR_VIEW_INFO,       // WARDS    GRIMOIRE
                    RADAR_VIEW_EXPOSURE };                     // EXPOSURE (row 3, col 0)
                radar_view_t v = RADAR_VIEW_COUNT;             // sentinel: no target
                if (ty >= 104 && ty < 296) { int idx=((ty-104)/48)*2+(tx>=120?1:0); if (idx<7) v=GRID[idx]; }
                else if (ty >= 30 && ty < 100) {
                    int card = tx / 80;
                    if (card < s_home_n) { s_sel_node = s_home_ids[card]; v = RADAR_VIEW_NODE; }
                    else                 v = RADAR_VIEW_RADAR;   // empty strip area -> aggregate radar
                }
                if (v != RADAR_VIEW_COUNT) { radar_ui_select_view(&ui, v, now); send_request(); last_req = now; }
                else                       radar_ui_note_input(&ui, now);
            } else if (ui.view == RADAR_VIEW_CONTROL) {
```
to:
```c
        if (edge && !modal_open) {
            if (ui.view == RADAR_VIEW_HOME) {
                // HOME sigil grid -> jump to that view. Geometry mirrors draw_home: 2 cols split at
                // x=120, 4 rows of 66px starting at y=32 (grid reclaims the old fleet-strip's space).
                static const radar_view_t GRID[8] = {          // 4 rows @ 66px (see draw_home)
                    RADAR_VIEW_RADAR,   RADAR_VIEW_DETAIL,     // CIRCLE   HUNTERS
                    RADAR_VIEW_STATS,   RADAR_VIEW_CONTROL,    // LIVING   RITES
                    RADAR_VIEW_LIBRARY, RADAR_VIEW_INFO,       // WARDS    GRIMOIRE
                    RADAR_VIEW_EXPOSURE, RADAR_VIEW_NODES };   // EXPOSURE NODES
                radar_view_t v = RADAR_VIEW_COUNT;             // sentinel: no target
                if (ty >= 32 && ty < 296) { int idx=((ty-32)/66)*2+(tx>=120?1:0); if (idx<8) v=GRID[idx]; }
                if (v != RADAR_VIEW_COUNT) { radar_ui_select_view(&ui, v, now); send_request(); last_req = now; }
                else                       radar_ui_note_input(&ui, now);
            } else if (ui.view == RADAR_VIEW_NODES) {
                if (ty < 26) { radar_ui_on_input(&ui, now); }             // "< BACK" strip -> HOME
                else {
                    int idx = (ty - 36) / 24;                             // matches draw_nodes_list's row geometry
                    if (idx >= 0 && idx < s_node_n) {
                        s_sel_node = s_node_ids[idx];
                        radar_ui_select_view(&ui, RADAR_VIEW_NODE, now); send_request(); last_req = now;
                    } else {
                        radar_ui_note_input(&ui, now);
                    }
                }
            } else if (ui.view == RADAR_VIEW_CONTROL) {
```

- [ ] **Step 8: Fix the NODE detail page's prev/next cycling to use the full (untruncated) id list**

Around `cyd_main.c:1013-1023`, change:
```c
            } else if (ui.view == RADAR_VIEW_NODE) {
                if (ty < 26) { radar_ui_on_input(&ui, now); }             // "< BACK" strip -> HOME
                else {
                    if (s_home_n > 0) {
                        int cur = 0;
                        for (int i = 0; i < s_home_n; i++) if (s_home_ids[i] == s_sel_node) { cur = i; break; }
                        if (tx < 80)        s_sel_node = s_home_ids[(cur - 1 + s_home_n) % s_home_n];
                        else if (tx > 160)  s_sel_node = s_home_ids[(cur + 1) % s_home_n];
                    }
                    radar_ui_note_input(&ui, now); send_request(); last_req = now;
                }
```
to:
```c
            } else if (ui.view == RADAR_VIEW_NODE) {
                if (ty < 26) { radar_ui_on_input(&ui, now); }             // "< BACK" strip -> HOME
                else {
                    if (s_node_n > 0) {
                        int cur = 0;
                        for (int i = 0; i < s_node_n; i++) if (s_node_ids[i] == s_sel_node) { cur = i; break; }
                        if (tx < 80)        s_sel_node = s_node_ids[(cur - 1 + s_node_n) % s_node_n];
                        else if (tx > 160)  s_sel_node = s_node_ids[(cur + 1) % s_node_n];
                    }
                    radar_ui_note_input(&ui, now); send_request(); last_req = now;
                }
```

This is the actual fix for the reachability bug: `s_node_n` now holds up to `FLEET_STATUS_MAX` (8)
ids instead of being hard-capped at 3, so cycling from any node reaches every tracked node.

- [ ] **Step 9: Search for any other reference to the old names**

```bash
grep -n "s_home_ids\|s_home_n" cyd/main/cyd_main.c
```

Expected: no output — every reference was covered by Steps 5-8. If anything remains, it was missed;
update it the same way (rename `s_home_ids` → `s_node_ids`, `s_home_n` → `s_node_n`).

- [ ] **Step 10: Confirm `FLEET_STATUS_MAX` is visible at the array declaration site**

```bash
grep -n "#include.*fleet_status" cyd/main/cyd_main.c
```

Expected: at least one hit near the top of the file — `fleet_status.h` (which defines
`FLEET_STATUS_MAX`) must already be included, since the file already uses `fleet_status_t` and
friends throughout. If for some reason it's missing, add `#include "fleet_status.h"` near the other
local includes.

- [ ] **Step 11: Build the CYD firmware to confirm it compiles**

```powershell
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target cyd -Do build
```

Expected: `Project build complete.` No errors. (Don't flash yet — Task 4 does the full on-device
verification after the docs are updated too, so there's one clean hardware-verification pass at the
end rather than two.)

- [ ] **Step 12: Commit**

```bash
git add components/simulacra_radar/radar_render.c cyd/main/cyd_main.c
git commit -m "feat(cyd): remove HOME fleet strip, add NODES tile, fix node reachability bug

The actual bug: s_home_ids was a hard-coded 3-slot array, unrelated to
FLEET_STATUS_MAX, that both the strip's tap-mapping and the NODE
detail page's prev/next cycling depended on -- so even before this
change's cap raise, a 4th tracked node was permanently unreachable in
the UI (tracked and counted, but its detail page could never open).

Renamed to s_node_ids[FLEET_STATUS_MAX] and dropped the '> 3 ? 3'
truncation entirely. HOME's icon grid grows to 8 tiles (adds NODES,
filling the previously-empty 8th slot) and reclaims the freed strip
space, going from 4 rows @ 48px to 4 rows @ 66px starting right after
the top bar."
```

---

### Task 4: Docs + on-device verification

**Files:**
- Modify: `private/CYD-CONSOLE-WIKI.md` (local source, gitignored)
- Modify (via the wiki git repo, not this one): `CYD-Console-Guide.md`, `CYD-Console-Reference.md`

**Interfaces:**
- Consumes: everything from Tasks 1-3 (final integration + verification pass).
- Produces: nothing — terminal task.

- [ ] **Step 1: Update the wiki source**

In `private/CYD-CONSOLE-WIKI.md`:
- In the `## HOME` section, remove the "Fleet strip" bullet (the one describing "Up to three small
  cards... If there are more live nodes than there's room to show, tapping the empty part of the
  strip opens the aggregate RADAR view instead") and update the icon-grid table from 7 rows to 8,
  adding a `NODES` row: `| NODES | The full list of tracked decoy nodes, tap any to open its detail page |`.
- Add a new `## NODES` section (place it before `## NODE *(per-node telemetry)*`, matching the
  list-then-detail ordering used elsewhere in this doc):
  ```markdown
  ## NODES

  The full list of every decoy node the console is currently tracking (up to 8). Each row shows a
  node's id, health word, live decoy count, and battery (if present) — the same at-a-glance fields
  HOME's fleet strip used to show, just as a scrollable-free list instead of cards, so every tracked
  node is reachable, not just the first few. Tap any row to open that node's full
  [NODE](#node-per-node-telemetry) detail page.
  ```
- In the `## NODE *(per-node telemetry)*` section, update "Left/right zones step to the previous/
  next known node." — no wording change needed, it's already accurate; just confirm it now truly
  means *every* tracked node, not a truncated subset (it does, after Task 3).
- Update the "Sources" footer list at the bottom if it enumerates specific source files — no new
  files were created in `cyd/main/` for this feature, but `components/simulacra_radar/radar_render.c`
  and `radar_ui.h` are already listed there, so likely no change needed; just glance at it.

- [ ] **Step 2: Publish the wiki update**

Follow the same process used for prior wiki updates this project (clone `simulacra.wiki.git` to a
scratch dir, edit `CYD-Console-Guide.md` and `CYD-Console-Reference.md` to match, commit as
`Em3ritus` with no `Claude-Session` trailer, push). This repeats an established pattern from earlier
in this project's history — no new process to invent here.

- [ ] **Step 3: Enumerate current COM ports (they drift between sessions)**

```powershell
Get-PnpDevice -Class Ports -PresentOnly | Select-Object FriendlyName, Status | Format-Table -AutoSize
```

Confirm the CYD's port (expect CH340, historically COM20 but verify fresh).

- [ ] **Step 4: Build and flash the CYD**

```powershell
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target cyd -Port <COM_FROM_STEP_3> -Do all -ReadSeconds 10 -Grep 'cyd|render|touch'
```

Expected: clean build, successful flash, no crash/error in the serial log.

- [ ] **Step 5: On-device verification checklist**

With the fleet (3 decoys) already running from prior sessions, walk through by hand:
- [ ] HOME shows 8 tiles (no fleet strip), including a NODES tile in the grid.
- [ ] Tapping NODES opens a list showing all 3 currently-live decoys, each with a plausible
  health/count/battery row.
- [ ] Tapping a row in NODES opens that node's full detail page (CROWD/POWER/SYSTEM/DETECTIONS).
- [ ] From a NODE detail page, left/right taps cycle through all 3 known nodes (not just some).
- [ ] BACK from NODES returns to HOME; BACK from a NODE detail page returns to HOME (unchanged
  behavior, confirmed still working).
- [ ] Every other HOME grid tile (RADAR/FOLLOWERS/DECOYS/CONTROL/LIBRARY/INFO/EXPOSURE) still opens
  its correct page — a full regression check on the touch-dispatch changes in Task 3, since none of
  that logic is host-testable.

Note: this project's fleet currently has only 3 physical boards, so the specific "4th node was
unreachable, now it isn't" scenario this whole feature fixes can't be directly demonstrated on real
hardware — Task 1's host test is what actually proves the cap raise, and Task 3's code-level line
diffs (not a test) are what prove the reachability fix. This checklist is a regression/sanity pass
on everything else, not a re-proof of the bug fix itself.

`private/CYD-CONSOLE-WIKI.md` is gitignored — Step 1's edit has nothing to commit in this repo. The
actual publication is Step 2, against the separate wiki repo.
