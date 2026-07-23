# Dashboard: Protection Posture — Design

**Date:** 2026-07-23
**Status:** Self-approved (autonomous session; user asleep, explicit "keep working, we'll fine-tune"). Visual/label choices are FLAGGED for user review.
**Lever:** #1 of the adoption set ("make the invisible protection legible"). The Pwnagotchi/Flipper lesson: the screen is the adoption driver.

## Goal

Give the CYD an honest, at-a-glance **protection posture** — one word + colour that answers "am I actually protected right now?" It must tell the truth when you are NOT protected (empty RF space, or a confirmed follower present), never falsely reassure. This is the function core of the "see your protection" dashboard; visual polish comes after user review.

## Why this, first

Everything the fleet does (project a crowd, detect hunters) is currently shown as scattered counts. A posture fuses them into the single question a user actually has. It's pure function over data that already flows (fleet-aggregate `radar_wire_status_t`): `active_devices` (shades), `pop_ewma` (the ambient living), `threats[]` + escalation (hunters), `flags` bit0 (paused).

## The four postures (honest by construction)

```
HUNTED  (red / COL_HUNTER)  a confirmed RECURRING or PERSISTENT follower is present.
                            Only confirmed recurrence -- a NEW/one-session blip does NOT
                            cry wolf (that's why the escalation model exists). NEW threats
                            still show in the FOLLOWERS view.
EXPOSED (amber / COL_WARD)  decoys running, but ~no ambient crowd to blend into. This is
                            the honest empty-space truth: with nothing real around you, the
                            decoys can't hide you (co-location ceiling). Say so.
CLOAKED (green / COL_CHANNEL) decoys active AND a real ambient crowd exists to blend into.
DARK    (dim / COL_ASH)     decoys paused (flags bit0) or not emitting (active_devices==0).
```

Priority order when multiple apply: **HUNTED > DARK > EXPOSED > CLOAKED**. A confirmed hunter is the top fact regardless of crowd; a paused fleet is DARK regardless of crowd.

## Decision logic (pure, testable)

```c
radar_posture_t radar_posture(const radar_wire_status_t *st):
    for each threat: if escalation != NEW -> HUNTED         // confirmed follower wins
    if (flags & PAUSED) or active_devices == 0 -> DARK
    if pop_ewma <= POSTURE_MIN_CROWD (=2) -> EXPOSED        // no crowd to hide in
    else -> CLOAKED
```

`POSTURE_MIN_CROWD = 2`: at or below ~2 ambient devices there is effectively no crowd (matches the WIFI_OBS_FLOOR reasoning and the empty-space discussion). **[FLAG for user: threshold value.]**

## Rendering (first pass — FLAGGED for user tuning)

Surface it where it's always seen: the **HOME top bar**, right-aligned, one word coloured by severity — beside the existing "SIMULACRA" wordmark. Reuses `draw_header`/HOME bar styling (crypt fill already there). `draw_home` gains the aggregate `st` (already available in `radar_render_view`; HOME currently ignores it). No new view, no layout upheaval — a v1 the user can then promote/restyle (e.g. a full-width banner, an icon, animation) once seen on-panel. **[FLAG: placement + whether it deserves a bigger treatment.]**

## Architecture

- `radar_posture_t` enum + `radar_posture(const radar_wire_status_t*)` live in `radar_render.{h,c}` (tightly coupled to status rendering; keeps the host render harness able to test it end-to-end with no new build wiring). Pure, no I/O.
- `draw_home` renders the posture word in the top bar.
- Testing rides the existing `tools/radar_audit/render_dump.c` text-capture harness (extended to set `pop_ewma` and threat escalation), asserting HOME shows the right posture word for representative statuses. This is the same harness built this week — no new infra.

## Testing

Host (radar_audit, text-capture):
- recurring/persistent threat present -> HOME renders "HUNTED".
- decoys active + pop_ewma high, no confirmed threat -> "CLOAKED".
- decoys active + pop_ewma 0 -> "EXPOSED".
- flags paused (or active_devices 0) -> "DARK".
- NEW-only threat (sessions/places 1/1) does NOT read HUNTED (no false alarm).
- priority: paused + high crowd -> DARK; confirmed threat + paused -> HUNTED.

Firmware: CYD **and** decoy (shared component) compile-verified.

## Out of scope (this spec)

- Any restyle beyond the one-word posture (full "crowd swarm" visualization, animation, the exposure meter, tracker-detection headline work) — those are later levers / later passes.
- Changing the detection/escalation engine — posture only *reads* it.

## Open questions for the user (morning)

1. `POSTURE_MIN_CROWD` threshold (2?) and whether pop_ewma is the right "crowd" signal vs total_obs.
2. Placement/prominence — top-bar word (this v1) vs a full-width banner vs an icon/sigil.
3. Should a NEW (unconfirmed) potential follower get its own low-key posture (e.g. "WATCHED") or stay only in FOLLOWERS? (v1: stays in FOLLOWERS, to avoid crying wolf.)
