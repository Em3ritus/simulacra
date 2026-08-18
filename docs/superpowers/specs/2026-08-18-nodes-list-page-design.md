# NODES list page + fleet-cap raise — design spec

**Goal:** Remove the CYD's HOME fleet-strip (which is not just cosmetically limited but hard-capped
at 3 nodes with no way to reach a 4th), replace it with a dedicated NODES list page reachable from
HOME's icon grid that reaches every currently-tracked node, and raise the fleet-tracking ceiling from
4 to 8 to give real headroom.

## Background: the bug this fixes

`FLEET_STATUS_MAX` (`cyd/main/fleet_status.h:9`) is `4` — the CYD can track up to 4 decoy nodes'
status simultaneously (`fleet_status_t.nodes[FLEET_STATUS_MAX]`, with LRU eviction of the
least-recently-heard node once full — see `node_id_for()` in `cyd_main.c:166-181`).

`radar_node_view_t nv[FLEET_STATUS_MAX]` (`cyd_main.c:1127`) is already correctly sized to the full
cap — every tracked node's live status *is* available every frame.

But `s_home_ids` (`cyd_main.c:242`) is declared `static uint8_t s_home_ids[3]` — a **separate,
hard-coded 3-slot array**, unrelated to `FLEET_STATUS_MAX`. It's populated every frame at
`cyd_main.c:1151-1153`:
```c
s_home_n = nvc > 3 ? 3 : nvc;
for (int i = 0; i < s_home_n; i++) s_home_ids[i] = nv[i].id;
```
This is the only path that maps a tapped HOME-strip card to a node id (`cyd_main.c:956`), *and* the
only path the NODE detail page's left/right cycling walks (`cyd_main.c:1013-1023`). So even though
`FLEET_STATUS_MAX=4` nodes can be tracked, **only 3 are ever reachable in the UI** — the 4th is
tracked (counted in aggregates) but its NODE detail page can never be opened. This is a real,
present bug, not a hypothetical one introduced by raising the cap.

## Changes

### 1. Constants

- `cyd/main/fleet_status.h:9`: `#define FLEET_STATUS_MAX 4` → `8`.
- `main/fleet.h:21` (`FLEET_NODE_CAP 8`, decoy-side self-exclusion table): unchanged — already 8,
  matches the new cap so a decoy never false-positives on a fleetmate.
- No other array needs a manual resize: `nv[FLEET_STATUS_MAX]`, `fleet_status_t.nodes[...]`, and
  every downstream consumer already size off the constant.

### 2. Kill the 3-slot shortlist, replace with a full one

- `cyd_main.c:242`: `static uint8_t s_home_ids[3]` → `static uint8_t s_node_ids[FLEET_STATUS_MAX]`
  (renamed — it's no longer HOME-strip-specific). Companion `s_home_n` → `s_node_n`, same rename
  reasoning.
- `cyd_main.c:1151-1153`: drop the `nvc > 3 ? 3 : nvc` truncation entirely —
  `s_node_n = nvc; for (int i = 0; i < s_node_n; i++) s_node_ids[i] = nv[i].id;`. This one change
  is what actually fixes the unreachability bug — every other consumer just needs to read the
  renamed, now-untruncated array.

### 3. HOME page

- Remove the fleet-strip render block entirely: `radar_render.c:241-263` (the `cols`/card loop) and
  its call site's `nc`/`nodes` usage for strip purposes. `draw_home`'s signature can drop the
  `nodes`/`nc` parameters once the strip is gone (the icon grid doesn't need them).
- Remove the fleet-strip touch-dispatch case: `cyd_main.c:953-957` (the `ty >= 30 && ty < 100`
  branch that maps `tx/80` to `s_home_ids[card]`).
- Icon grid (`radar_render.c:264-271`, arrays `sig[7]`/`lbl[7]`; touch-side `GRID[7]` at
  `cyd_main.c:947-951`) grows from 7 to 8 tiles, filling the currently-empty 8th grid slot (row 3,
  col 1 — next to EXPOSURE, which currently sits alone in row 3 col 0). New tile: **NODES**,
  target view `RADAR_VIEW_NODES`. Sigil TBD at implementation time (reuse an existing one that
  reads clearly — not a design-affecting choice).
- Icon grid moves up to start right after the top bar, filling the vertical space the strip used to
  occupy (per the approved "reclaim it" option — no fleet-summary line on HOME).
- Net result: HOME = top bar (wordmark + posture, unchanged) → 8-tile icon grid → footer hint
  (unchanged).

### 4. NODES page (new view)

- New enum value `RADAR_VIEW_NODES` in `radar_ui.h:8-11` (alongside the existing, singular
  `RADAR_VIEW_NODE` detail page — deliberately distinct names, list vs. detail).
- New render function `draw_nodes_list(g, nodes, node_count)` in `radar_render.c`, dispatched from
  `radar_render_view` (`radar_render.c:457-475`, the `if/else if` chain — add one more branch).
  Reuses the same top-bar/header/BACK convention as every other page (`draw_header`, matching
  `draw_node`'s `ty < 26` back-zone).
- Body: one row per tracked node, `nodes[0..node_count-1]` top to bottom, **no pagination** — up to
  8 rows fit the page body at a normal row height without needing a scroll/paginate gesture the rest
  of the console doesn't otherwise use. Each row shows the same at-a-glance fields the old strip
  card showed (node id, health word + color dot, live decoy count, battery if present) — reuse the
  health-word/color derivation already duplicated in `draw_home`'s old strip block and `draw_node`
  (`radar_render.c:247-251` / `379-383`): `!alive → SILENT/ASH`, `low_batt||degraded → WARD`, else
  `CHANNEL`. Worth factoring into one small shared static helper while touching this code, since
  this will be the 2nd–3rd near-identical copy — not a new requirement, just avoid adding a 3rd
  copy-paste.
- Empty state (`node_count == 0`): a plain "no nodes reporting" line, matching how FOLLOWERS reads
  "CLEAR" when empty rather than showing a blank body. Note: per `cyd_main.c:1136-1150`, the
  `nv`/`nvc` computation already synthesizes a single placeholder entry (`nvc = 1`) whenever nothing
  has reported yet, specifically so HOME's strip was never blank — since NODES draws from that same
  per-frame `nv`/`nvc`, `node_count` is effectively always ≥ 1 today and this empty-state branch is
  defensive, not something you should expect to actually hit. Keep it anyway (cheap, and correct if
  that fallback ever changes) but don't spend effort making it pretty.
- Touch dispatch (new `cyd_main.c` branch for `ui.view == RADAR_VIEW_NODES`): `ty < 26` → BACK to
  HOME (`radar_ui_on_input`). Otherwise, map the tap's row (`ty` bucketed by row height, same style
  as the existing card/grid tap-mapping arithmetic) to an index into `s_node_ids[0..s_node_n-1]`;
  set `s_sel_node` to that id and `radar_ui_select_view(&ui, RADAR_VIEW_NODE, now)`.

### 5. NODE detail page — fix the cycling

- `cyd_main.c:1013-1023`: replace every `s_home_ids`/`s_home_n` reference with
  `s_node_ids`/`s_node_n` (the renamed, untruncated array from change #2). No other logic in this
  block changes — the prev/next arithmetic (`cur - 1 + s_home_n) % s_home_n`, etc.) is already
  correct, it was just walking a truncated list.
- NODE's own BACK (`ty < 26`) still returns to HOME per existing convention — it does **not** need
  to return to NODES specifically; that would be a nicer UX (return to where you came from) but is
  out of scope for this change (every other detail page in this console — THREAT, for instance —
  already has an established "detail pages go back to their list page" pattern worth following
  consistently, but retrofitting NODE's BACK target is a separate, small follow-up, not bundled
  here to keep this change focused).

### 6. Testing

- `tools/radar_audit` host suite (compiles the real `radar_render.c`/`cyd_main.c`-adjacent logic on
  the host) gets new cases:
  - `draw_nodes_list` renders the right number of rows for 0, 1, and 8 tracked nodes.
  - Tapping a NODES row selects the correct node id (mirrors the existing DETAIL/THREAT row-tap
    tests, if any exist as a pattern to follow — check `tools/radar_audit/tests/` for the closest
    existing analog before writing these from scratch).
  - NODE-page cycling reaches all 8 tracked nodes, not just the first 3 (a regression test for the
    exact bug this change fixes — construct 8 tracked nodes, cycle next 7 times from node 0, assert
    every id was visited).
  - `FLEET_STATUS_MAX` cap itself: 8 nodes trackable, a 9th evicts the least-recently-heard (this
    behavior already exists and is presumably already covered for the old cap of 4 — just confirm
    it still holds at 8, don't rewrite it).
- On-device: flash the CYD, confirm HOME shows the 8-tile grid with no strip, NODES lists all
  currently-live boards (3, with this project's real hardware), each reachable and showing correct
  live data, and NODE-page prev/next cycling works across all of them. The 4th-node-unreachable bug
  itself can't be directly reproduced on real hardware with only 3 boards in hand — the host test
  above is what actually proves the fix, hardware verification here is a sanity check on the rest.

### 7. Docs

- `private/` wiki source (the CYD-CONSOLE-WIKI.md this was originally drafted from, and the
  published GitHub wiki pages `CYD-Console-Guide`/`CYD-Console-Reference`): update the HOME section
  to remove the fleet-strip description and 7-tile grid count, add a NODES page section, and note
  the NODE page's cycling now reaches every tracked node instead of "up to 3."

## Out of scope (explicitly, so it isn't silently assumed later)

- NODE page's BACK target staying HOME (not returning to NODES) — noted above, deliberately
  deferred.
- Any scroll/paginate gesture — the chosen cap (8) fits on one screen; if the cap is ever raised
  well beyond what fits a single page, pagination becomes a real design question again, but isn't
  one today.
- A HOME fleet-summary line — explicitly rejected in favor of a clean reclaimed-space grid, per the
  approved design.
