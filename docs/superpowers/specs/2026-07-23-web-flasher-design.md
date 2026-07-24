# Browser Web-Flasher (Baked Starter) — Design

**Date:** 2026-07-23
**Status:** Approved (design). Lever #4 of the adoption set — the "conversion multiplier" that turns interest into a built fleet.

## Goal

Let a newcomer flash a working Simulacra fleet **from a web page in Chrome/Edge — no ESP-IDF, no esptool, no command line.** Plug in a board, click **Connect & Flash**, done. The moment they've flashed ≥1 decoy + the CYD, the baked fleet links up with zero enrollment.

## Decisions (locked with user)

- **Regime: baked starter.** The flasher serves baked binaries (shared compile-time key from `components/simulacra_radar/radar_key.h`). Boards link instantly, no enrollment handshake (which the web-flasher can't perform anyway). Baked = a *starter/demo* regime, not private — the key is already public in the repo. Provisioned (unique per-fleet key) stays a documented upgrade path for real use, built by advanced users with the toolchain.
- **Scope: build + validate locally; publish on the user's timeline.** Nothing is pushed or made public by this work. Success = an end-to-end flash of a real board through the browser on localhost.
- **Technical gate: CLEARED.** ESP Web Tools / esptool-js supports ESP32-C5, ESP32-C6, and classic ESP32, and **auto-detects the connected chip** to pick the right firmware from the manifest. One button handles all three roles.

## Chip → role mapping (auto-detected)

| Connected chip | Firmware flashed | Role |
|---|---|---|
| ESP32-C5 | baked decoy (`build_c5`) | Ward decoy |
| ESP32-C6 | baked decoy (c6) | Shade decoy |
| ESP32 (classic) | baked CYD (`cyd/`) | Vigil controller |

The mapping is 1:1 (each chip family = exactly one role in this fleet), so auto-detect always picks correctly for the prescribed BOM.

## Components

1. **`web/index.html`** — static page. Loads the esp-web-tools web component from the unpkg CDN
   (`https://unpkg.com/esp-web-tools@10/dist/web/install-button.js`, module script — the standard
   integration) and renders `<esp-web-install-button manifest="manifest.json">`. Copy explains: what
   Simulacra is (one-line + link to repo), what boards you need and that you flash each one, and the
   honest caveats (Chrome/Edge desktop only; baked = starter regime, not private; the anti-tracking
   purpose). No build tooling, no framework — plain HTML/CSS.
2. **`web/manifest.json`** — ESP Web Tools manifest. `name`, `version`, and a `builds` array with
   three entries, each `chipFamily` = `"ESP32-C5"` / `"ESP32-C6"` / `"ESP32"` and `parts` = a single
   merged image at `offset: 0` (relative path `firmware/<variant>.bin`). No Improv (decoys/CYD need
   no Wi-Fi provisioning).
3. **`web/firmware/*.bin`** — three merged baked images (`decoy-c5.bin`, `decoy-c6.bin`, `cyd.bin`).
   **Gitignored** (multi-MB build artifacts); produced by the build script. Merged via
   `esptool merge_bin` from each target's `flash_args` so bootloader/partition/app offsets are baked
   into one file flashable at offset 0 (handles the per-chip bootloader offset difference:
   esp32 @ 0x1000, RISC-V @ 0x0).
4. **`web/build_flasher.ps1`** — orchestrates: for each of c5 / c6 / cyd, build **baked** firmware
   (reusing the build-flash-read flow so the correct IDF version per target is used — c5→5.5, c6→5.4,
   cyd→5.4/esp32; baked = `-DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1`, i.e. the `-Fleet` flag set,
   NO `-DSIMULACRA_FLEET_PROVISION`), then `esptool merge_bin` → `web/firmware/<variant>.bin`. A
   `-Serve` switch runs `python -m http.server` from `web/` for local testing. Force-clean per target
   to avoid the mixed-regime cache trap (a stale `FLEET_PROVISION=1`).
5. **`web/README.md`** — the BOM (exact buyable boards: an ESP32-C5 dev board, SparkFun Thing Plus
   ESP32-C6, the CYD ESP32-2432S028), a numbered flashing guide, and caveats (Chrome/Edge only;
   starter-vs-provisioned; prescriptive BOM because a non-CYD esp32 board would wrongly receive CYD
   firmware); plus the **publish checklist**: push repo → deploy `web/` **including a freshly-built
   `firmware/`** to GitHub Pages (the manifest uses relative `firmware/*.bin` paths, so the bins must
   ship with the page — via a build-and-deploy GitHub Action, or by including the built bins in the
   Pages deployment). A versioned GitHub Release with the same bins is optional (nice for direct
   downloads) but not required by the manifest.
6. **`web/test_manifest.py`** — host test: the manifest is valid JSON; exactly the three expected
   `chipFamily` values are present; each build has a single part at offset 0; and (when the bins have
   been built) each referenced `firmware/*.bin` exists and is non-empty. Skips the file-existence
   assertions gracefully if firmware hasn't been built yet.

## Data flow (newcomer)

1. Open the page in Chrome/Edge → the esp-web-tools button appears.
2. Plug in a board, click **Connect & Flash** → the component reads the chip family over Web Serial,
   matches it to a `builds` entry in the manifest, downloads that merged bin, and flashes it.
3. Repeat per board. Baked key → decoys + CYD find each other on boot, no enrollment.

## Local validation flow (this scope's deliverable)

`web/build_flasher.ps1` (build all three + merge) → `web/build_flasher.ps1 -Serve` (localhost) →
open `http://localhost:8000` in Chrome → plug in a real board (e.g. a C5 or the CYD) → flash it
through the browser → confirm it boots the flashed firmware over serial. That is the end-to-end proof.

## Testing

- **Host:** `web/test_manifest.py` — JSON validity, three correct chipFamily entries, single-part-at-0
  structure, bins-exist-when-built.
- **End-to-end (manual, on-bench):** the local validation flow above — a real browser flash of a real
  board. This is the acceptance test for the feature.

## Out of scope (v1)

- Publishing (push / GitHub release / Pages enablement) — deferred, user-triggered; documented as a
  checklist in the README.
- CI-built release binaries — a later nicety; v1 builds locally.
- Provisioned-regime flashing, Improv Wi-Fi, an in-page fleet dashboard/monitor.
- Firefox/Safari support — impossible (no Web Serial); documented, not worked around.

## Open questions

None blocking. The BOM's exact ESP32-C5 dev-board model is whatever the user currently uses (filled
in from the bench during implementation; the README lists it concretely, no placeholder).
