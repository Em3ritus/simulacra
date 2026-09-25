# radar_audit - host verification for the Vigil console

Host-compiles the firmware's own console logic - the CYD's UI state machine, render pipeline, fleet
aggregation, and exposure-check - so its behaviour is verified against the exact source that runs on
the display, not a reimplementation.

## What it checks

Four small dumper harnesses, each linking one real production file and driving it from Python
`unittest` (plus `vigil_sim`, a presentation tool rather than a check -- see below):

- **`ui_dump`** (`components/simulacra_radar/radar_ui.c`) - the view state machine: default/idle
  view, input-driven navigation, auto-wake to HUNTERS on a new follower.
- **`fleet_dump`** (`cyd/main/fleet_status.c`) - fleet aggregation: per-node upsert/stale/alive
  tracking, cross-node threat union (closest RSSI wins), preset agreement/MIXED detection, and
  pruning long-silent nodes so a dead node's card can't push a live one off the display.
- **`render_dump`** (`components/simulacra_radar/radar_render.c` + `exposure.c`) - every screen the
  console draws: radar/HOME, node and threat detail, the INFO system/fleet console, the CONTROL
  preset picker (including TURBO's and CLEAR THREATS' two-tap arm/confirm rendering), and the
  exposure-check view.
- **`expo_dump`** (`components/simulacra_radar/exposure.c`) - the leaked-SSID exposure check in
  isolation.

## Run

```
pwsh run.ps1        # build all four *_dump.exe (MSVC) + run the tests
```

Non-Windows: `make && python -m unittest discover -s tests -v`.

## Notes

- `render_dump.c` links a `host_stubs/` shim (`portab.h`) supplying the ESP-IDF bits
  `radar_render.c` expects, so the real rendering source compiles unmodified on the host.
- The CONTROL harness's `--control` mode takes the render function's `radar_ctrl_info_t` fields as
  positional CLI args (including `turbo_armed`) - see `test_turbo_control.py` / `test_control.py` for
  the current argument order.
- The **test suite** covers the console's *logic* (state, aggregation, what text/flags a screen
  would draw), not pixel output: `render_dump.c` stubs every gfx primitive to a no-op and captures
  only text. `vigil_sim` is the exception -- it links the real rasteriser instead (see below).

## vigil_sim - the Vigil console on the desktop

`pwsh run.ps1 -Sim` builds `vigil_sim.exe`: the CYD console in a resizable Win32 window, for showing
the interface without hardware (demos, talks, screenshots).

**It is a presentation tool, not a check.** Nothing in `tests/` exercises it, and it must never be
treated as evidence about firmware behaviour. What makes it honest is the split:

| layer | source |
|---|---|
| fleet telemetry | **`sim_fleet.c` - THE ONLY FICTION.** Stands in for the radio. |
| aggregation | `cyd/main/fleet_status.c` - real (upsert, stale, prune, threat union, MIXED) |
| view state machine | `components/simulacra_radar/radar_ui.c` - real |
| drawing | `radar_render.c` + `radar_gfx.c` + `radar_sigil.c` + `radar_geom.c` - real |
| panel | Win32 `StretchDIBits`, nearest-neighbour |

So every pixel is drawn by the firmware; only the antenna is simulated. The numbers `sim_fleet.c`
emits are *plausible, not measured* - never quote them as results.

Modes: bare for the interactive window, `--story` for a hands-free scripted sequence (also a frame
source if a recording is wanted), `--shot` to render every view to `shot_*.ppm` headlessly and exit.

Keys: `H R N D T I C B S V` pick views - arrows select within a list - `TAB`/`ENTER` drive the
CONTROL page through the real `radar_ui_*` calls - `J` join, `K` drop, `G` degrade, `A`/`F` add a
known/follower threat, `E` escalate, `X` clear, `P` pause (DARK), `O` toggle ambient crowd
(EXPOSED) - `SPACE` freeze, `+`/`-` scale, `F1` help, `ESC` quit.

Keyboard-driven rather than click-driven on purpose: the CYD's touch hit-testing lives in
`cyd/main/cyd_main.c`, which does not host-compile, and reimplementing it here would create a second
implementation free to drift from the real one.

**Windows-only** (Win32 window), so it is deliberately absent from the `Makefile`'s `all` target -
see the note there. `run.ps1` with no arguments is unchanged and still just builds the four dumpers
and runs the tests.

## vigil_web - the same console in a browser

`pwsh build_web.ps1` compiles the same core to WebAssembly (emscripten; see that script's header
for the one-time emsdk setup). Output is `vigil_web.js` + `vigil_web.wasm` beside `vigil_web.html`.
**Serve all three from one directory** - opening the HTML off the filesystem fails, because the
browser blocks the wasm fetch under `file://`. Whole payload is ~65 KB.

Same honesty split as `vigil_sim`: `sim_fleet.c` is the only fiction. Extra behaviour wired up here:

- **Tap targets**, mapped in JS from the *rendered* geometry (`draw_home`'s `((y-32)/66)*2 +
  (x>=120?1:0)`, `draw_control`'s button rects). Not shared with `cyd_main.c`, whose hit-testing is
  bound to ESP-NOW sends, enrollment calls and device-local statics that cannot compile to wasm.
  A zone a few pixels off is cosmetic here; on the board it would matter.
- **INFO legend** - tapping the body flips `radar_sys_info_t.page`, mirroring
  `cyd_main.c`'s `s_info_page ^= 1`.
- **EXPOSURE** - drives the real `exposure.c` state machine (BASELINE 4 s -> WATCH 6 s -> RESULT)
  from a synthetic probe stream: flat-rate ambient devices, plus one that bursts during WATCH with
  named SSIDs. The firmware picks the winner itself; nothing tells it which device is the phone.
  **The SSIDs are invented**, in the router-default style `ssid_pool.c` uses. None came from a
  capture, and the page says so.

Panel geometry, the one thing worth getting right: the Vigil renders **240 wide x 320 tall,
portrait**. `render_dump.c` passes `(band, 320, 240, 320)` = `band_h=320, w=240, h=320`. Passing it
the other way round clips roughly a third off the bottom of every view (CONTROL loses all three of
its buttons) and leaves an unpainted strip down the right-hand side.
