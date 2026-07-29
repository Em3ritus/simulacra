# CYD Threat Detail Card (THREAT drill-in) — Design

**Date:** 2026-07-29
**Status:** Approved (brainstorm), pending spec review
**Sub-project:** B of the CYD dashboard build-out (deep operator console). Order: A → **B** → C → D.
A (per-node NODE console) shipped (merge `cafda9f`). This spec covers **B only**. C = INFO system console + legend; D = wire-v2 telemetry + live CONTROL (decoy-side).

## Goal

Surface the per-threat fields already transmitted in `radar_wire_status_t.threats[]` but never rendered: **`confidence`, `vendor`, `epochs`, `first_epoch`, `last_epoch`**. Do it via a drill-in **THREAT detail card** reached from the FOLLOWERS view, plus one in-place win (confidence on surveillance rows).

## Motivation

Today `draw_detail` (FOLLOWERS) shows per threat only: an escalation dot, the name (class name or hash), recurrence (`Np Ns` or `new`), and `best_rssi`. The SURVEILLANCE section shows name + rssi. The wire struct also carries `confidence` (0–100, the matched signature's confidence), `vendor` (BLE company id, e.g. `0x004C` Apple/AirTag, `0x09C8` XUNTONG/Flock), `epochs` (persistence count), and `first_epoch`/`last_epoch` (sighting span) — all invisible. For a deep operator console these are exactly the fields that let you judge a detection (is this a strong Flock match? how long has this follower persisted? what company id?).

All data is already on the wire and already aggregated (`fleet_status_aggregate` copies these fields into the fleet threat). This is a **pure CYD-render feature**: no decoy change, no wire bump, no fleet reflash.

## Non-goals (YAGNI)

- No separate SURVEILLANCE view (the detail card handles surveillance threats too; the FOLLOWERS SURVEILLANCE section stays).
- No row-precise hit-testing (would need a hit-map plumbed through the banded renderer). Selection is body-tap + prev/next paging.
- No new decoy-reported data (that is D).
- No two-line list rows.

## Architecture

### Navigation model

New view **`RADAR_VIEW_THREAT`**, inserted before `RADAR_VIEW_COUNT` in `radar_ui.h` (after `RADAR_VIEW_NODE`).

| Surface | Today | After B |
|---|---|---|
| FOLLOWERS (`RADAR_VIEW_DETAIL`) tap | any tap → HOME (generic `else` in cyd_main) | header strip (`ty<26`) → HOME; **body tap with ≥1 threat** → `RADAR_VIEW_THREAT` at threat 0; body tap with 0 threats → HOME |
| THREAT header strip (`ty<26`) | — | BACK to `RADAR_VIEW_DETAIL` |
| THREAT left third (`tx<80`) / right third (`tx>160`) | — | prev / next threat (wraps) |
| THREAT center | — | refresh (`radar_ui_note_input`) |

### Selection stability (mirrors A's `s_sel_node`)

- cyd_main gains `static uint32_t s_sel_threat;` — the **hash** of the threat being inspected (hashes are stable across frames; array indices are not).
- Each frame, in the render block where `agg` is available, cyd_main records the current threat set into `static uint32_t s_threat_hashes[RADAR_MAX_THREATS]; static int s_threat_n;` (from `agg.threats[0..threat_count-1]`), and resolves `s_sel_threat` → `sel_threat` (index into `agg.threats[]`, or `-1` if the hash is gone).
- FOLLOWERS body-tap sets `s_sel_threat = s_threat_hashes[0]` (previous frame's set — 1-frame stale, ~100 ms, fine) and selects THREAT. It guards on `s_threat_n > 0`.
- THREAT prev/next page over `s_threat_hashes[0..s_threat_n-1]`: find the current hash's index, move ±1 with wrap, set `s_sel_threat` to the neighbor's hash.
- If `s_sel_threat` is absent from `agg.threats[]` (detection cleared), `sel_threat == -1` → `draw_threat` renders a `THREAT GONE` placeholder (BACK still works).

Paging order is the raw `agg.threats[]` array order; the `n/N` indicator is `sel_threat+1 / threat_count`.

## Rendering interface change

`radar_render_view(...)` gains **one** parameter, `int sel_threat`, immediately after `sel_node`:

```c
void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                       const radar_node_view_t *nodes, int node_count, int sel_node, int sel_threat,
                       const radar_lib_info_t *lib, const radar_ctrl_info_t *ctrl,
                       const exposure_t *expo, uint16_t sweep_deg,
                       uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx);
```

- `sel_threat` = index into `st->threats[]` for `RADAR_VIEW_THREAT`; `-1` = none (every other view ignores it).
- Ripples to: cyd_main's 4 `radar_render_view` call sites, and `render_dump` (2 existing calls + the new `--threat` mode). Same shape as A's `sel_node` addition.
- Dispatch adds `else if (view == RADAR_VIEW_THREAT) draw_threat(&g, st, sel_threat);`.

## THREAT card content (`draw_threat`)

One threat from `st->threats[sel]`, using the existing `draw_header`/`row_section`/`row_kv` primitives.

- **Placeholder:** `sel < 0 || sel >= st->threat_count` → `draw_header(g, "THREAT")` + `radar_gfx_text(g, 60, 150, "THREAT GONE", COL_ASH)`, return.
- **Header:** `draw_header` titled `THREAT n/N` where `n = sel+1`, `N = st->threat_count`.
- **Subline** (y=32): name + category, colored by escalation. Name = `sig_class_name(class_id)` when `kind == DETECT_KIND_KNOWN`, else `%08lx` of `hash`.
- **CLASSIFICATION** (section y=50):
  - `kind` → `"known"` if `kind==DETECT_KIND_KNOWN` else `"behavioral"`
  - `class` → `sig_class_name(class_id)` if known, else `"-"`
  - `category` → `cat_name(category)` (see helper)
  - `confidence` → `"%u%%"` if known, else `"-"`
  - `vendor` → `"0x%04X"` if `vendor != 0 && vendor != 0xFFFF`, else `"-"`
- **SIGHTING** (section y=150):
  - `rssi` → `"%ddB"` (`best_rssi`)
  - `escalation` → verdict string from `threat_escalation_level(sessions_seen, places_seen)`: `NEW` / `RECURRING` / `PERSISTENT`
  - `sessions` → `"%u"` (`sessions_seen`)
  - `places` → `"%u"` (`places_seen`)
  - `epochs` → `"%u"` (`epochs`)
  - `span` → `"e%u..e%u"` (`first_epoch`, `last_epoch`)

Row y-positions (16px rows): CLASSIFICATION rows 68/84/100/116/132; SIGHTING rows 168/184/200/216/232/248. Ends at y≈248 (< 318). All within budget.

**Helper:** `cat_name(uint8_t c)` returns `"TRACKER"` / `"CAMERA"` / `"BODYCAM"` / `"UNKNOWN"` for `SIG_CAT_TRACKER` / `SIG_CAT_CAMERA` / `SIG_CAT_BODYCAM` / (default), added to `radar_render.c`.

## In-place win: confidence on surveillance rows

In `draw_detail`'s SURVEILLANCE loop, add confidence between the name and the right-aligned rssi:

```c
radar_gfx_text(g,20,y,sig_class_name(st->threats[i].class_id),COL_HUNTER);
char cf[8]; snprintf(cf,sizeof cf,"%u%%",(unsigned)st->threats[i].confidence);
radar_gfx_text(g,120,y,cf,COL_ASH);
char r[12]; snprintf(r,sizeof r,"%ddB",(int)st->threats[i].best_rssi);
radar_gfx_text(g,224-(int)strlen(r)*8,y,r,COL_ASH);
```

Follower rows are unchanged (their extra depth lives in the card).

## cyd_main integration points

- **Touch:** add a `RADAR_VIEW_DETAIL` branch (body-tap → THREAT) and a `RADAR_VIEW_THREAT` branch (BACK / prev / next / refresh) to the edge dispatch chain, before the generic `else`.
- **Render block:** record `s_threat_hashes[]`/`s_threat_n` from `agg`, resolve `sel_threat`, and pass `sel_threat` to all four `radar_render_view` calls (alongside the existing `sel_idx`).
- **Freshness overlay:** THREAT is **not** added to the skip list — it is fleet threat data, so when the fleet goes stale the `NO DECOY` band correctly covers it (same as DETAIL today).
- **Idle-return:** unchanged (THREAT is a normal view).

## Error handling / edge cases

- **Zero threats:** FOLLOWERS body-tap is a no-op → HOME; THREAT is unreachable with an empty list.
- **Selected threat clears while viewing:** `sel_threat == -1` → `THREAT GONE` placeholder, BACK works.
- **Single threat:** prev/next wrap to self (no-op).
- **`sel_threat` out of range:** treated as placeholder.
- **Behavioral follower (not known):** `class`/`confidence`/`vendor` show `-`; the rest render.
- **Provision vs non-provision builds:** THREAT is build-flag-independent.

## Testing

Host-only via `tools/radar_audit/render_dump` (MSVC `cl`):

1. **New `--threat` mode** that builds a `radar_wire_status_t` with N threats, sets threat `sel`'s fields (kind, class_id, category, confidence, vendor, best_rssi, epochs, first_epoch, last_epoch, sessions_seen, places_seen), and calls `radar_render_view(RADAR_VIEW_THREAT, ..., sel_threat=sel, ...)`.
2. Assert for a **known camera** (Flock, category CAMERA): header `THREAT 1/…`, `CAMERA`, `Flock`, confidence `%`, vendor `0x09C8`, `epochs`, span `e..`.
3. Assert for a **behavioral follower**: `behavioral`, `class`/`confidence`/`vendor` show `-`.
4. Assert the **`THREAT GONE`** placeholder for `sel = -1`.
5. **`draw_detail` test:** a surveillance threat's row now includes a `%` confidence token.
6. Compile-verify firmware for **cyd** (renders THREAT) and **c5** (component still builds).

## Files touched

- `components/simulacra_radar/radar_ui.h` — add `RADAR_VIEW_THREAT`.
- `components/simulacra_radar/radar_render.h` — `radar_render_view` signature (+`int sel_threat`).
- `components/simulacra_radar/radar_render.c` — `draw_threat`, `cat_name`, dispatch, `sel_threat` plumb; surveillance-row confidence in `draw_detail`.
- `cyd/main/cyd_main.c` — `s_sel_threat`/`s_threat_hashes[]`/`s_threat_n`, FOLLOWERS + THREAT touch branches, record/resolve `sel_threat`, pass it at the 4 call sites.
- `tools/radar_audit/render_dump.c` — `--threat` mode + update the existing calls/init for the new param.
- `tools/radar_audit/tests/test_node_view.py` (or a new `test_threat_detail.py`) — THREAT + surveillance-confidence assertions.

## Rollout

Pure CYD firmware change. Flash the CYD only; decoys untouched. No wire bump — new CYD interoperates with the existing fleet.
