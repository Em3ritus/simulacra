# Simulacra Web-Flasher

Flash a Simulacra fleet from your browser — no toolchain. Open `index.html` (served over http/https)
in **desktop Chrome or Edge**, plug in a board, click **Connect & Flash**. The chip is auto-detected
and the correct role is installed:

| Board | Chip | Role |
|---|---|---|
| ESP32-C5 dev board (e.g. ESP32-C5-DevKitC-1) | ESP32-C5 | Ward decoy |
| SparkFun Thing Plus ESP32-C6 | ESP32-C6 | Shade decoy |
| CYD — ESP32-2432S028 (2.8" ESP32 display) | ESP32 (classic) | Vigil controller |

A minimal fleet is **one decoy + one CYD**. Flash each board in turn.

## Regime

This flasher installs the **baked starter** build: every fleet shares the compile-time key in
`components/simulacra_radar/radar_key.h`. That key is public, so baked is for *trying it out*, not a
private deployment. For real use, build the **provisioned** regime from source (unique per-fleet key +
enrollment) — see the main project README.

## Caveats

- **Chrome / Edge desktop only** — Web Serial isn't in Firefox or Safari.
- **Prescriptive BOM** — a plain non-CYD ESP32 board would receive CYD firmware. Use the boards above.

## Build the binaries (maintainer)

Run from the repo root in a shell that can reach the `build-flash-read` skill:

```
web\build_flasher.ps1
```

This builds the three **baked** firmwares (correct IDF version per chip) and writes merged, single-file
images to `web/firmware/*.bin` (gitignored). Then serve locally to test:

```
web\build_flasher.ps1 -Serve      # http://localhost:8000
```

## Publish (maintainer, when ready)

Automated by `.github/workflows/flasher.yml` — it builds the three baked firmwares in CI, merges each,
and deploys `web/` + a fresh `firmware/` to GitHub Pages, so **no binaries ever live in git**.

1. Push the repo (after a PII scan).
2. Repo **Settings → Pages → Source = "GitHub Actions"** (one-time).
3. Push a change under `main/`, `cyd/`, `components/`, or `web/` (or hit **Run workflow**) → the Action
   builds + deploys → the flasher is live at `https://<owner>.github.io/<repo>/`.

The workflow is a first-pass sketch (three builds across two IDF versions, ESP32-C5 is new) — expect to
tune it on the first CI run. See the caveats commented at the top of the workflow file.
