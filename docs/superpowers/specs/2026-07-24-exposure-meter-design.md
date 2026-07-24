# Exposure Meter (interactive Wi-Fi, CYD view) — Design

**Date:** 2026-07-24
**Status:** Approved (design). Adoption lever #3 — the "watch your own phone shout" demo.

## Goal

A new CYD view that shows a user, live and interactively, how much their **own phone leaks** over Wi-Fi. You hold the CYD, tap to scan, toggle your phone's Wi-Fi off→on, and the CYD catches your phone's probe burst and reveals what it just announced to the room — chiefly the **named networks** in its preferred-network list. It's the visceral "I didn't know it did that" hook that motivates running Simulacra; it audits *your* exposure rather than protecting you, so it's a demo/awareness surface, not a protection feature.

## Scope

- **v1 = Wi-Fi only.** The CYD already brings Wi-Fi up for ESP-NOW; adding promiscuous probe-sniffing is incremental. Named SSIDs in probes are the most visceral leak. **BLE device-type reveal is out of scope for v1** (needs NimBLE on the CYD — a bigger lift; a documented phase 2).
- **Ephemeral by construction.** Nothing about the user's devices is persisted — the session lives in RAM and is discarded on exit (honors the no-lost-device-liability razor).

## Interaction model — modal & exclusive

Entering the exposure view **pauses the CYD's fleet duties**: Wi-Fi flips to promiscuous and hops channels 1/6/11 to catch probe requests; the ESP-NOW request/broadcast loop is suspended for the duration. Exiting restores channel 1 and resumes ESP-NOW. This is a deliberate on-demand demo (the user is looking at the screen, not the radar), so we accept the exclusive mode and avoid all promiscuous-vs-link coexistence complexity.

## The interactive flow (state machine)

1. **IDLE** — the view shows *"TAP TO SCAN THE AIR."*
2. On tap → **BASELINE** (~4 s): sniff probe requests, recording per-**fingerprint** probe counts and any named SSIDs. Establishes the ambient set of probing devices.
3. → **PROMPT** — screen: *"Now toggle your phone's Wi-Fi OFF and ON."* and immediately opens the watch window.
4. **WATCH** (~6 s): a phone re-enabling Wi-Fi fires a fresh **burst** of active-scan probes. Track each fingerprint's probe count in this window.
5. → **RESULT**: pick the fingerprint with the largest **activity spike** relative to its baseline rate = the user's phone. Show its exposure: the named SSIDs it leaked, probe count, and a loudness read. If no clear spike (ambiguous / phone wildcards only), say so and offer to re-run.

The whole thing is **elicitation, not tracking** — the toggle *causes* the tell; no persistent identifier is used or stored.

## Architecture (4 small units)

1. **`components/simulacra_radar/exposure.{h,c}`** — the pure **exposure-session state machine** (no ESP deps → host-testable). Interface:
   ```c
   #define EXPO_MAX_DEVICES 24          // distinct fingerprints tracked
   #define EXPO_MAX_SSIDS   8           // named SSIDs kept for the winner
   typedef enum { EXPO_IDLE, EXPO_BASELINE, EXPO_WATCH, EXPO_RESULT } expo_state_t;
   typedef struct { expo_state_t state; /* opaque internals */ } exposure_t;

   void expo_reset(exposure_t*);                                  // -> IDLE
   void expo_start(exposure_t*, uint32_t now_ms);                 // IDLE -> BASELINE
   // feed one sniffed probe: fingerprint (structure hash, MAC/SSID-independent), optional named SSID
   // (NULL/empty = wildcard), timestamp. Ignored unless BASELINE/WATCH.
   void expo_probe(exposure_t*, uint32_t fp, const char *ssid, uint8_t ssid_len, uint32_t now_ms);
   void expo_tick(exposure_t*, uint32_t now_ms);                  // advances BASELINE->WATCH->RESULT on timers
   // RESULT accessors:
   bool        expo_have_result(const exposure_t*);
   uint32_t    expo_winner_fp(const exposure_t*);
   int         expo_winner_probe_count(const exposure_t*);
   int         expo_winner_ssids(const exposure_t*, const char **out, int max);  // # named SSIDs, fills out[]
   bool        expo_ambiguous(const exposure_t*);                 // no clear spike -> true
   ```
   Timings are `#define`d (`EXPO_BASELINE_MS=4000`, `EXPO_WATCH_MS=6000`). "Activity spike" = post-toggle probe count minus the baseline-rate expectation for that fingerprint; the max positive spike wins, subject to a minimum-margin threshold (else `ambiguous`).

2. **`cyd/main/expo_sniff.{h,c}`** (or folded into cyd_main) — CYD promiscuous Wi-Fi glue. On enter: `esp_wifi_set_promiscuous(true)` + RX callback + a channel-hop timer over {1,6,11}; on each probe-request frame, compute the structure **fingerprint** (hash over the ordered IE ids/lengths, excluding SSID + addresses) and pull the SSID element, then call `expo_probe(...)`. On exit: promiscuous off, channel 1, ESP-NOW resumed. Firmware-only (not host-tested).

3. **`RADAR_VIEW_EXPOSURE`** render in `components/simulacra_radar/radar_render.c` — a `draw_exposure(g, exposure_t*)` that renders the current step (scan prompt / toggle prompt / spinner / result reveal with the SSID list + loudness), in the established dashboard style. Added to the HOME sigil grid.

4. **Radio-mode switch + view wiring** in `cyd_main.c` — a new sigil tile → `radar_ui_select_view(EXPOSURE)`; entering starts the sniffer + `expo_start`; the touch handler drives tap-to-scan; leaving tears down. The HOME grid grows 6→7 tiles (see Open questions).

## Data flow

promiscuous RX (probe-req) → fingerprint + SSID + ts → `expo_probe()` → (BASELINE tallies / WATCH tallies) → `expo_tick()` crosses the timers → RESULT picks max-spike fingerprint → `draw_exposure` renders its leaked SSIDs + loudness.

## Testing

- **`tools/radar_audit` host test** of `exposure.c` via a small `expo_dump` harness (feed a scripted probe-event stream over a fake clock):
  - a device that bursts in the WATCH window is picked as the winner; its named SSIDs are collected and de-duped.
  - a device probing only wildcards yields a winner (identified) but an empty SSID list.
  - no post-toggle spike → `expo_ambiguous` true, no winner.
  - the busiest *baseline* device does NOT win if it doesn't spike (identification is by spike, not raw volume).
  - state transitions fire on the timers (BASELINE→WATCH→RESULT) via `expo_tick`.
- **Render smoke** for `RADAR_VIEW_EXPOSURE` via the existing render harness (each step renders in-bounds; the result step shows the SSID rows).
- **Firmware:** CYD compile-verify; then on-bench with the user — the real toggle-your-phone flow.

## Honest caveats (in the UX copy where relevant)

- Modern iOS/Android often randomize MACs *and* wildcard their probes; then the SSID list is thin or empty. The view degrades gracefully: it still **identifies** the phone by its toggle burst and shows loudness + "your phone probes silently (good) but still announces its presence/fingerprint," which is itself a teachable result.
- If several devices toggle at once the pick is ambiguous — the view says so and offers a re-run (in practice the user controls their own toggle timing).
- During exposure mode the CYD is off the fleet link (by design); the roster will show it as gone until you exit.

## Out of scope (v1)

- BLE exposure (device-type / service-UUID reveal) — phase 2, needs NimBLE on the CYD.
- Any persistence, history, or per-device profiles (razor: nothing about the user is stored).
- Auto-identification without the toggle (RSSI co-presence heuristics) — rejected earlier as tracking-adjacent; the interactive toggle is the whole point.

## Open questions

- **HOME grid 6→7 tiles.** The current 2×3 grid at 64 px rows fills y=104–294. A 7th tile needs a reflow — candidate: 4 rows at 48 px (y=104,152,200,248; ~46 px tiles), keeping all seven views. Alternatively fold INFO into another page. **Decision (for the plan): 4-row 48 px reflow**, keeping every view; revisit if it reads too cramped on-panel.
- **Fingerprint definition** — exact bytes hashed (ordered IE id+len list; include HT/ext-cap capability bytes for stability; exclude SSID, DS-channel, and addresses). Nailed down in the plan against a real probe layout; must be MAC- and SSID-independent so a device's probes group across rotations.
