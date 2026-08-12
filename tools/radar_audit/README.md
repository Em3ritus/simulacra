# radar_audit — host verification for the Vigil console

Host-compiles the firmware's own console logic — the CYD's UI state machine, render pipeline, fleet
aggregation, and exposure-check — so its behaviour is verified against the exact source that runs on
the display, not a reimplementation.

## What it checks

Four small dumper harnesses, each linking one real production file and driving it from Python
`unittest`:

- **`ui_dump`** (`components/simulacra_radar/radar_ui.c`) — the view state machine: default/idle
  view, input-driven navigation, auto-wake to HUNTERS on a new follower.
- **`fleet_dump`** (`cyd/main/fleet_status.c`) — fleet aggregation: per-node upsert/stale/alive
  tracking, cross-node threat union (closest RSSI wins), preset agreement/MIXED detection, and
  pruning long-silent nodes so a dead node's card can't push a live one off the display.
- **`render_dump`** (`components/simulacra_radar/radar_render.c` + `exposure.c`) — every screen the
  console draws: radar/HOME, node and threat detail, the INFO system/fleet console, the CONTROL
  preset picker (including TURBO's and CLEAR THREATS' two-tap arm/confirm rendering), and the
  exposure-check view.
- **`expo_dump`** (`components/simulacra_radar/exposure.c`) — the leaked-SSID exposure check in
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
  positional CLI args (including `turbo_armed`) — see `test_turbo_control.py` / `test_control.py` for
  the current argument order.
- This suite covers the console's *logic* (state, aggregation, what text/flags a screen would draw),
  not pixel output — there is no display or touch hardware on the host.
