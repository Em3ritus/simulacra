# CYD INFO System Console + Legend — Design

**Date:** 2026-07-29
**Status:** Approved (brainstorm), pending spec review
**Sub-project:** C of the CYD dashboard build-out (deep operator console). Order: A → B → **C** → D.
A (NODE console, merge `cafda9f`) and B (THREAT detail, merge `dca9f22`) shipped. This spec covers **C only**. D = wire-v2 telemetry + live CONTROL (decoy-side).

## Goal

Turn the near-stub INFO page (today: `epoch`, `uptime`, `"firmware cyd v1"`) into a real two-page view: **page 0** a system/fleet console, **page 1** a color/posture LEGEND. Body-tap toggles the pages. CYD-render-only — no decoy change, no wire bump, no fleet reflash.

## Motivation

INFO is the weakest page — three lines. For a deep operator console it should answer "what is my whole system doing right now": how many nodes are meshing, total decoys vs target, the ambient crowd, the signature-DB version the fleet is running, the learned-shape library size, SD status, how fresh the link is, and which firmware/build is loaded. Separately, the dashboard uses colored words (posture HUNTED/EXPOSED/DARK/CLOAKED) and colored dots (escalation) with no key anywhere — a legend makes the whole UI self-explaining. Both fit under the existing INFO slot as two tap-toggled pages, so no HOME-grid surgery (the 7-tile grid is full).

All data is already on the CYD (aggregate status, the librarian snapshot, the signature-DB counters, the link timestamp) — this is pure render + plumbing.

## Non-goals (YAGNI)

- No new decoy-reported data (that is D).
- No new HOME tile / grid re-layout.
- No new top-level view — the legend lives as INFO's second page.
- No history/trends.

## Architecture

### Data plumbing

`draw_info` currently receives only `st` (the aggregate). Two changes:

1. Pass the existing **`lib`** (`radar_lib_info_t`, already an argument of `radar_render_view`, used by LIBRARY) through to `draw_info` for SD/card status and the shape-library count.
2. Add a small **`radar_sys_info_t`** carrying the CYD-only fields, passed as a new `sys` param on `radar_render_view` (NULL except INFO), filled by cyd_main each frame:

```c
typedef struct {
    uint8_t  node_count;    // meshing nodes (fleet_status_count)
    uint16_t sig_ver;       // signature-DB version (s_sigdb_ver)
    uint16_t sig_count;     // signatures loaded (s_sigdb_n)
    uint32_t link_age_s;    // seconds since last status from any node; UINT32_MAX = never
    const char *build;      // firmware/build tag, e.g. "cyd v2 flood"
    uint8_t  page;          // INFO view: 0 = system console, 1 = legend
} radar_sys_info_t;
```

The `sys` param follows the existing optional-pointer convention (`lib`/`ctrl`/`expo`): it is placed immediately after `expo` in the signature and is NULL on every view except INFO.

### `radar_render_view` signature change

```c
void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                       const radar_node_view_t *nodes, int node_count, int sel_node, int sel_threat,
                       const radar_lib_info_t *lib, const radar_ctrl_info_t *ctrl,
                       const exposure_t *expo, const radar_sys_info_t *sys, uint16_t sweep_deg,
                       uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx);
```

- Ripples to: cyd_main's 4 call sites, and `render_dump` (its `--expo`/`--node`/`--threat`/final calls + a new `--info` mode).
- Dispatch changes `draw_info(&g, st)` → `draw_info(&g, st, lib, sys)`.

### Navigation

INFO today falls through to the generic `else` (any tap → HOME). New (mirrors B's DETAIL branch):
- Header strip (`ty < 26`) → HOME (`radar_ui_on_input`).
- Body (`ty >= 26`) → toggle `s_info_page` (cyd_main `static uint8_t s_info_page;`, `^= 1`), keep view (`radar_ui_note_input`).

cyd_main passes `sys.page = s_info_page` each frame.

## Page 0 — SYSTEM console (`sys->page == 0`)

`draw_header(g,"INFO")`, then `row_section`/`row_kv`:
- **FLEET** — `nodes` (`sys->node_count`), `decoys` (`st->active_devices`), `target` (`st->active_target`), `real crowd` (`st->pop_ewma`)
- **SIGNATURES** — `sig db` = `"v%u (%u)"` (`sys->sig_ver`, `sys->sig_count`); `shapes` = `"%u/%u"` (`lib->lib_count`, `lib->lib_cap`)
- **STORAGE** — `card` = `"OK %luMB"` (`lib->card_mb`) if `lib->sd_ok`, else `"ABSENT"`
- **LINK** — `last status` = `"%lus ago"` (`sys->link_age_s`) or `"never"` if `UINT32_MAX`
- **SYSTEM** — `uptime` (via `fmt_uptime(st->uptime_s)`), `firmware` (`sys->build`)
- Footer hint at the bottom: `TAP: LEGEND` (dim).

Row layout (16px rows): header 0–26; FLEET hdr 34, rows 52/68/84/100; SIGNATURES hdr 118, rows 136/152; STORAGE hdr 170, row 188; LINK hdr 206, row 224; SYSTEM hdr 242, rows 260/276; footer 298. Last content ~y=284 < footer < 318.

If `lib` is NULL, STORAGE/shapes show `-` (defensive; on the CYD `lib` is always passed). If `sys` is NULL, `draw_info` renders a minimal fallback (epoch + uptime) so the host non-`--info` path stays valid.

## Page 1 — LEGEND (`sys->page == 1`)

`draw_header(g,"LEGEND")`, reusing `posture_color`/`escalation_color` (both already `static` in `radar_render.c`) and the health colors from `draw_home`:
- **POSTURE** — four rows, each the word in its own color + a short gloss:
  - `CLOAKED` (`COL_CHANNEL`) "hidden in a crowd"
  - `EXPOSED` (`COL_WARD`) "no crowd to hide in"
  - `DARK` (`COL_ASH`) "decoys paused"
  - `HUNTED` (`COL_HUNTER`) "follower confirmed"
- **ESCALATION** — three rows, a colored dot (filled rect, like `draw_detail`) + label + gloss:
  - arcane (`COL_ARCANE`) `NEW` "this session"
  - amber (`COL_WARD`) `RECURRING` "across sessions"
  - red (`COL_HUNTER`) `PERSISTENT` "confirmed follower"
- **HEALTH** — four rows, the word colored:
  - `CHANNEL` (`COL_CHANNEL`) "healthy"
  - `DEGRADED` (`COL_WARD`) "probe TX wedged"
  - `LOW BATT` (`COL_WARD`) "battery low"
  - `SILENT` (`COL_ASH`) "not reporting"
- Footer hint: `TAP: SYSTEM` (dim).

Row layout: header 0–26; POSTURE hdr 32, rows 48/62/76/90; ESCALATION hdr 108, rows 124/138/152; HEALTH hdr 170, rows 186/200/214/228; footer 298. All < 318.

The posture/escalation color values are read from the existing helpers so the legend can never drift from what the rest of the UI actually draws.

## cyd_main integration points

- **Touch:** add a `RADAR_VIEW_INFO` branch to the edge dispatch chain (header→HOME, body→`s_info_page ^= 1`) before the generic `else`.
- **Render block:** build a local `radar_sys_info_t sysinfo` from `fleet_status_count(&s_fleet)`, `s_sigdb_ver`, `s_sigdb_n`, the link age (`s_status_ms ? (now - s_status_ms)/1000 : UINT32_MAX`), the compile-time build tag, and `s_info_page`; pass `&sysinfo` to all four `radar_render_view` calls.
- **Build tag:** a compile-time string via `#ifdef` in cyd_main, e.g. `#ifdef SIMULACRA_FLOCK_FLOOD` → `"cyd v2 flood"` else `"cyd v2"`. Lets the operator confirm which build is loaded.
- **Freshness overlay:** add `RADAR_VIEW_INFO` to the freshness-overlay skip set (see the edge case below) so the "NO DECOY" top band never overpaints the console — the LINK row is the honest staleness signal.

## Error handling / edge cases

- **INFO currently gets the freshness overlay** (it is not in the HOME/EXPOSURE/NODE skip list). Since page 0 now has a LINK row showing status age, the top-band "NO DECOY" overlay would cover the INFO header when stale. Add `RADAR_VIEW_INFO` to the freshness-overlay skip set (both the provision and non-provision blocks) so the console is never overpainted — the LINK row is the honest staleness signal.
- **`sys == NULL`** (host non-`--info` render path): `draw_info` renders the minimal epoch+uptime fallback (page ignored).
- **`lib == NULL`**: STORAGE `card` and SIGNATURES `shapes` show `-`.
- **`link_age_s == UINT32_MAX`**: LINK shows `never`.
- **Page persistence:** `s_info_page` persists across INFO entries within a session; it resets to 0 on nothing (a stale legend page on re-entry is harmless). Leaving INFO does not reset it — acceptable.
- **Provision vs non-provision builds:** INFO is build-flag-independent (the build tag differs, but the view works in both).

## Testing

Host-only via `tools/radar_audit/render_dump`:

1. **New `--info <page> [node_count sig_ver sig_count link_age lib_count lib_cap card_mb sd_ok epoch uptime]` mode** that builds `st`, `lib`, and `sys` and calls `radar_render_view(RADAR_VIEW_INFO, ...)` with `sys.page = <page>`.
2. **Page 0 asserts:** `FLEET`, `SIGNATURES`, `STORAGE`, `LINK`, `SYSTEM` headers; the `nodes` value equals `node_count`; `sig db` renders `v<ver> (<count>)`; `card` shows `OK <mb>MB` when sd_ok; `last status` shows `<n>s ago`; `firmware` shows the build tag; footer `TAP: LEGEND`.
3. **Page 1 asserts:** `LEGEND` header; `POSTURE`/`ESCALATION`/`HEALTH` sections; the words `CLOAKED`, `HUNTED`, `PERSISTENT`, `SILENT` present; footer `TAP: SYSTEM`.
4. **`link_age` never:** pass `UINT32_MAX` → `LINK` shows `never`.
5. Compile-verify firmware for **cyd** (renders INFO) and **c5** (component still builds).

## Files touched

- `components/simulacra_radar/radar_render.h` — add `radar_sys_info_t`; `radar_render_view` signature (+`const radar_sys_info_t *sys`).
- `components/simulacra_radar/radar_render.c` — rewrite `draw_info(g, st, lib, sys)` with the two pages + footer hints; dispatch + `sys` plumb.
- `cyd/main/cyd_main.c` — `s_info_page`, INFO touch branch, build the `radar_sys_info_t`, add `RADAR_VIEW_INFO` to the freshness skip, pass `sys` at the 4 call sites, compile-time build tag.
- `tools/radar_audit/render_dump.c` — `--info` mode + update the existing calls for the new param.
- `tools/radar_audit/tests/test_info_console.py` — page 0 / page 1 / never-link assertions.

## Rollout

Pure CYD firmware change. Flash the CYD only; decoys untouched. No wire-version bump.
