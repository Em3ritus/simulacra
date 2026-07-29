# CYD Per-Node Drill-Down (NODE console) — Design

**Date:** 2026-07-29
**Status:** Approved (brainstorm), pending spec review
**Sub-project:** A of the CYD dashboard build-out (deep operator console). Order: **A → B → C → D**.
B = threat/surveillance depth, C = INFO system console + legend, D = wire-v2 telemetry + live CONTROL (decoy-side). A/B/C are CYD-render-only; D is the single decoy-side reflash. This spec covers **A only**.

## Goal

Let the operator tap an individual node card on HOME and drill into a **per-node console** showing that node's full decoy/health/power telemetry — the data that actually differs node-to-node and is invisible today (HOME shows only decoy count + battery + a one-word health).

## Motivation

The fleet strip on HOME renders up to 3 node cards but tapping the strip jumps to the **aggregate** RADAR — there is no way to inspect a single node. All sub-views (RADAR/FOLLOWERS/DECOYS) render `agg` (every node folded into one status). For a "deep operator console" the single most valuable missing capability is per-node inspection: which node is low on battery, which is DEGRADED, what each node projects vs. its target, each node's shade-form split, uptime, epoch, probe count.

Everything needed is **already on the wire** — `radar_wire_status_t` is broadcast per-node ~1/s and cyd_main already keeps a per-node table (`s_fleet`, keyed by sender MAC → stable `node_id`). This is a pure CYD-render feature: **no decoy firmware change, no wire-protocol bump, no fleet reflash.**

## Non-goals (YAGNI)

- No new decoy-reported data (that is sub-project D).
- No per-node **threat list** — the aggregate FOLLOWERS view already lists detections; NODE shows only this node's follower/surveillance **counts**.
- No charts, sparklines, or history buffering (that is a later/visual concern).
- No changes to how nodes are keyed or aged (`fleet_status` table is reused as-is).

## Architecture

### Navigation model

| Surface | Today | After A |
|---|---|---|
| HOME fleet strip tap (`y 30–100`) | → aggregate `RADAR_VIEW_RADAR` | tap on a **live node card** → `RADAR_VIEW_NODE` for that node; tap on empty card slot → falls back to aggregate `RADAR_VIEW_RADAR` |
| Aggregate radar | RADAR sigil tile **and** strip tap | RADAR sigil tile (unchanged, nothing lost) |

- New view enum value **`RADAR_VIEW_NODE`**, inserted immediately before `RADAR_VIEW_COUNT` in `radar_ui.h`.
- cyd_main gains `static uint8_t s_sel_node;` — the **stable node id** (`N#`) currently being inspected. Set when a node card is tapped.
- **Live re-resolution:** every frame, cyd_main resolves `s_sel_node` → that node's current `node_view` (id, status pointer, alive flag) from the freshly built `nv[]`. The page therefore stays live; if the node goes SILENT it renders dimmed with "seen Ns ago" and does **not** auto-close; when it revives, values refresh.

### Card → node-id mapping

`draw_home` renders `nodes[0..cols-1]` left-to-right in 80px columns. The tap handler runs at the top of the loop, before `nv[]` is rebuilt in the render block, so it needs the **previous** frame's card ordering:

- At render time, cyd_main records the rendered strip ids into `static uint8_t s_home_ids[3]; static int s_home_n;`.
- On a strip tap, `card = tx / 80`; if `card < s_home_n`, `s_sel_node = s_home_ids[card]` and select `RADAR_VIEW_NODE`. Else (empty slot) select aggregate `RADAR_VIEW_RADAR` (preserves current behavior for taps on empty strip area).
- 1-frame staleness (~100 ms) is acceptable.

### In-page navigation (NODE view)

Zones on the 240×320 panel:
- **Top strip** (`ty < 26`) → BACK to HOME (`radar_ui_on_input`).
- **Left third** (`tx < 80`) → previous node.
- **Right third** (`tx > 160`) → next node.
- **Center** → refresh (`radar_ui_note_input`, keeps backlight/idle timers fresh).

Prev/next cycle over the node ids currently present in `s_home_ids[0..s_home_n-1]` (the same set HOME shows), wrapping. With a single node, prev/next are no-ops (wrap to self). This lets the operator sweep the fleet without returning to HOME.

## Rendering interface change

`radar_render_view(...)` gains **one** parameter:

```c
void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                       const radar_node_view_t *nodes, int node_count, int sel_node,
                       const radar_lib_info_t *lib, const radar_ctrl_info_t *ctrl,
                       const exposure_t *expo, uint16_t sweep, uint16_t *band, int band_h,
                       int w, int h, radar_flush_fn flush, void *ctx);
```

- `sel_node` = index into `nodes[]` of the node to render for `RADAR_VIEW_NODE`; **`-1` = none** (every other view ignores it).
- Ripples to: cyd_main's 3 `radar_render_view` call sites (pass the resolved index, or `-1`), and the `tools/radar_audit/render_dump` host harness.
- Dispatch adds `else if (view == RADAR_VIEW_NODE) draw_node(&g, nodes, node_count, sel_node);`.

`radar_node_view_t` already carries `{ id, const radar_wire_status_t *st, bool alive }` — sufficient for identity, liveness, and all telemetry.

## NODE view content (`draw_node`)

One node, rendered with the existing `draw_header` / `row_section` / `row_kv` primitives for visual consistency with STATS/INFO.

- **Header:** `draw_header` titled `NODE N<id>` (right-aligned), `< BACK` left.
- **Subline** (below the header hairline): health word + liveness — `CHANNEL` / `LOW BATT` / `DEGRADED` / `SILENT`, colored (CHANNEL=`COL_CHANNEL`, degraded/low=`COL_WARD`, silent=`COL_ASH`), followed by `seen Ns ago` when not currently fresh. Health decode matches `draw_home`: flag bit3 = LOW BATT, bit2 = DEGRADED, `!alive` = SILENT, else CHANNEL (battery wins over degraded).
- **CROWD** — `decoys` (`active_devices`), `target` (`active_target`), `roster` (`roster_size`), `rpa/nrpa/static` (`form_restless/form_wandering/form_bound`). If vertical space is tight, `real crowd` (`pop_ewma`) and `observed` (`total_obs`) fold in here rather than a separate ENVIRONMENT section.
- **POWER** — battery: `pct% V.VV V` when `battery_pct != 0xFF`, else `V.VV V` when `battery_mv != 0`, else `USB` when `battery_mv == 0`. Amber (`COL_WARD`) when the LOW BATT flag is set.
- **SYSTEM** — `epoch`, `probes` (`probes_sent`), `churn` (PAUSED when flag bit0 else running), `uptime` (via existing `fmt_uptime`).
- **DETECTIONS** — compact: `followers N` and `surveillance M`, counted from this node's `threats[]` partitioned by `is_surveil_cat(category)`. No list.

**Fit constraint:** 240×320, 8px font. Header+subline ≈ 40px, leaving 280px. Use 15–16px rows and single section-header gaps; if the full set overflows, ENVIRONMENT folds into CROWD (above) and DETECTIONS collapses to one line. Exact spacing is a plan-level detail; the renderer must not draw past `y=318`.

A SILENT/aged node renders every value from its **last** status (dimmed via the SILENT subline), never blanks the page.

## cyd_main integration points

- **Freshness overlay:** NODE joins HOME/EXPOSURE in the skip list — the node subline shows its own liveness, so the global "NO DECOY / SEARCHING" band must not paint over it.
- **Idle-return:** unchanged — `radar_ui_on_tick` returns to HOME while clear; NODE is a normal view for timer purposes.
- **Render block:** where `nv[]` is built, resolve `sel_idx = index in nv[] whose id == s_sel_node` (or `-1` if absent), record `s_home_ids[]`/`s_home_n`, and pass `sel_idx` to `radar_render_view`. If `s_sel_node` is absent from `nv[]` (node fully gone from the table), `draw_node` receives `sel_node == -1` and renders a `NODE GONE` placeholder with BACK.

## Error handling / edge cases

- **Single node:** prev/next wrap to self (no-op).
- **Selected node goes SILENT:** page persists, subline = SILENT + "seen Ns ago", values from last status.
- **Selected node fully evicted from `s_fleet`:** `sel_node == -1` → `NODE GONE` placeholder (BACK still works). (Unlikely — `fleet_status` retains entries; this is a guard.)
- **`sel_node` out of range / `nodes[]` empty:** treated as `-1` → placeholder.
- **Provision vs non-provision builds:** NODE view is build-flag-independent (no enrollment coupling); works in both `SIMULACRA_FLEET_PROVISION` and plain builds.

## Testing

Host-only, via the existing `tools/radar_audit/render_dump` harness (MSVC `cl`, framebuffer→text dump):

1. **New `--node` mode** that constructs a `radar_wire_status_t` + a `radar_node_view_t` with a known id/alive and calls `radar_render_view(RADAR_VIEW_NODE, ...)`.
2. Assert the dump contains: `NODE N<id>`, the decoded health word, the battery string (all three formats across cases: pct+V, V-only, USB), each section header, and representative values (decoys/target, shade split, epoch, uptime).
3. **SILENT case:** `alive=false` → assert the subline shows the health = SILENT path and a `seen` age string, and that the body still renders last-status values.
4. **`sel_node == -1` case:** assert the `NODE GONE` placeholder renders.

Compile-verify the firmware for **c5, c6, and cyd** (the signature change must build on every target that links `simulacra_radar`).

## Files touched

- `components/simulacra_radar/radar_ui.h` — add `RADAR_VIEW_NODE`.
- `components/simulacra_radar/radar_render.h` — `radar_render_view` signature (+`int sel_node`).
- `components/simulacra_radar/radar_render.c` — `draw_node`, dispatch, `sel_node` plumb.
- `cyd/main/cyd_main.c` — `s_sel_node`, `s_home_ids[]`, card-tap routing, NODE in-page nav, freshness-overlay skip, resolve `sel_idx` and pass it at the 3 call sites.
- `tools/radar_audit/render_dump.c` (+ its build script/Makefile) — `--node` mode.
- `tools/radar_audit/tests/` — NODE render assertions.

## Rollout

Pure CYD firmware change. Flash the CYD only; decoys untouched. No wire-version bump, so old and new CYD builds interoperate with the existing fleet.
