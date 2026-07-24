# Browser Web-Flasher (Baked Starter) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A browser page that flashes a Simulacra fleet over Web Serial — plug in any board, auto-detect the chip, click Flash — validated end-to-end on the bench, nothing published.

**Architecture:** A tiny static site under `web/` (page + ESP Web Tools manifest) whose manifest lists three chip-family builds pointing at merged **baked** binaries. A PowerShell script builds the three baked firmwares and merges each into one flashable image, then can serve the site on localhost for a real browser flash.

**Tech Stack:** ESP Web Tools v10 (unpkg CDN, Web Serial), esptool `merge_bin`, plain HTML/CSS, Python 3.12 (manifest test + local server), the existing `build-flash-read` skill for per-target IDF builds.

## Global Constraints

- **Baked regime only:** builds use `-DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1` (the skill's `-Fleet` switch) and **must NOT** define `SIMULACRA_FLEET_PROVISION`. Force-clean per target to avoid a stale `FLEET_PROVISION=1` in the CMake cache.
- **Exact chipFamily strings** (ESP Web Tools): `"ESP32-C5"`, `"ESP32-C6"`, `"ESP32"`.
- **Chip → binary:** ESP32-C5 → `firmware/decoy-c5.bin`; ESP32-C6 → `firmware/decoy-c6.bin`; ESP32 → `firmware/cyd.bin`. Relative paths (bins ship next to the page).
- **Merged image flashed at offset 0** (merge_bin bakes the per-chip offsets: C5 bootloader @ 0x2000, CYD @ 0x1000, app @ 0x10000, partition @ 0x8000 — all read from each build's `flash_args`).
- **Per-target IDF:** c5 → IDF 5.5 (`$env:USERPROFILE\esp\v5.5\esp-idf`); c6 → IDF 5.4 (`$env:USERPROFILE\esp\v5.4\esp-idf`); cyd → IDF 5.4. The `build-flash-read` skill handles this for the build; the merge step re-exports the matching version.
- **esp-web-tools:** `https://unpkg.com/esp-web-tools@10/dist/web/install-button.js` (module script).
- **No publish:** nothing pushed; no GitHub release/Pages. `web/firmware/` is gitignored.
- **Commit trailers** on every commit:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
  ```
- **No PII in tracked files** (no OS username, real name, real MAC). PowerShell `>` writes UTF-16/BOM — write files with the Write tool or `Set-Content -Encoding ascii`, never `>`.

---

### Task 1: Static flasher site + manifest + validation test + README

**Files:**
- Create: `web/index.html`
- Create: `web/manifest.json`
- Create: `web/.gitignore`
- Create: `web/README.md`
- Create: `web/test_manifest.py`

**Interfaces:**
- Produces: `web/manifest.json` with a `builds` array of three `{chipFamily, parts:[{path, offset:0}]}` entries; the page references it via `<esp-web-install-button manifest="manifest.json">`. The build script (Task 2) writes bins to the `path`s.

- [ ] **Step 1: Write the failing test `web/test_manifest.py`**

```python
import json, os, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
EXPECTED = {"ESP32-C5": "firmware/decoy-c5.bin",
            "ESP32-C6": "firmware/decoy-c6.bin",
            "ESP32":    "firmware/cyd.bin"}


class Manifest(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(HERE, "manifest.json")) as f:
            self.m = json.load(f)

    def test_has_name_and_version(self):
        self.assertTrue(self.m.get("name"))
        self.assertTrue(self.m.get("version"))

    def test_three_expected_chip_families(self):
        fams = {b["chipFamily"] for b in self.m["builds"]}
        self.assertEqual(fams, set(EXPECTED), f"chipFamily set wrong: {fams}")

    def test_each_build_single_part_at_zero(self):
        for b in self.m["builds"]:
            self.assertEqual(len(b["parts"]), 1, f"{b['chipFamily']} must be one merged part")
            self.assertEqual(b["parts"][0]["offset"], 0, f"{b['chipFamily']} part offset must be 0")
            self.assertEqual(b["parts"][0]["path"], EXPECTED[b["chipFamily"]],
                             f"{b['chipFamily']} path wrong")

    def test_bins_exist_and_nonempty_when_built(self):
        for b in self.m["builds"]:
            p = os.path.join(HERE, b["parts"][0]["path"])
            if not os.path.exists(p):
                self.skipTest("firmware not built yet (run build_flasher.ps1)")
            self.assertGreater(os.path.getsize(p), 0, f"{p} is empty")

    def test_page_references_manifest(self):
        with open(os.path.join(HERE, "index.html")) as f:
            html = f.read()
        self.assertIn('manifest="manifest.json"', html)
        self.assertIn("esp-web-tools@10", html)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it, expect failure**

Run: `"C:/Program Files/Python312/python.exe" -m unittest web.test_manifest -v`
Expected: FAIL/ERROR (manifest.json and index.html do not exist yet).

- [ ] **Step 3: Write `web/manifest.json`**

```json
{
  "name": "Simulacra",
  "version": "0.1.0-baked",
  "new_install_prompt_erase": true,
  "builds": [
    { "chipFamily": "ESP32-C5", "parts": [ { "path": "firmware/decoy-c5.bin", "offset": 0 } ] },
    { "chipFamily": "ESP32-C6", "parts": [ { "path": "firmware/decoy-c6.bin", "offset": 0 } ] },
    { "chipFamily": "ESP32",    "parts": [ { "path": "firmware/cyd.bin",       "offset": 0 } ] }
  ]
}
```

- [ ] **Step 4: Write `web/index.html`**

```html
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Flash Simulacra</title>
<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js"></script>
<style>
  body { font: 16px/1.5 system-ui, sans-serif; max-width: 40rem; margin: 3rem auto; padding: 0 1rem;
         background: #12100f; color: #e8e2d4; }
  h1 { color: #b48ead; }
  a { color: #8fbcbb; }
  .card { background: #1c1a18; border: 1px solid #302c28; border-radius: 8px; padding: 1rem 1.25rem; }
  .caveat { color: #b0a999; font-size: 0.9rem; }
  esp-web-install-button { display: inline-block; margin: 1rem 0; }
  button[slot=activate] { font-size: 1rem; padding: 0.6rem 1.2rem; border-radius: 6px;
         border: 1px solid #b48ead; background: #2a2430; color: #e8e2d4; cursor: pointer; }
</style>
</head>
<body>
  <h1>Flash Simulacra</h1>
  <p>Simulacra is a multi-node anti-tracking decoy fleet: it hides your real devices in a plausible
     fake crowd and flags trackers following you.
     <a href="https://github.com/Em3ritus/simulacra">Project &amp; source &rarr;</a></p>

  <div class="card">
    <p><strong>You need:</strong> at least one decoy board (ESP32-C5 or ESP32-C6) and one CYD
       controller (ESP32-2432S028). Plug in each board and flash it — the flasher detects which
       chip it is and installs the right role automatically.</p>
    <esp-web-install-button manifest="manifest.json">
      <button slot="activate">Connect &amp; Flash</button>
    </esp-web-install-button>
  </div>

  <p class="caveat"><strong>Notes.</strong> Works in desktop <strong>Chrome or Edge</strong> only
     (Web Serial). This installs the <strong>baked starter</strong> build: all fleets share a public
     compile-time key, so it is for trying Simulacra out, not for a private deployment — build the
     provisioned regime from source for real use. Flashing a plain (non-CYD) ESP32 board would give it
     CYD firmware, so use the boards listed in the README.</p>
</body>
</html>
```

- [ ] **Step 5: Write `web/.gitignore`**

```
firmware/
```

- [ ] **Step 6: Write `web/README.md`**

````markdown
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

## Publish (maintainer, when ready — not automated)

1. Push the repo (after a PII scan).
2. Deploy `web/` **including a freshly-built `firmware/`** to GitHub Pages — the manifest uses relative
   `firmware/*.bin` paths, so the bins must ship with the page (a build-and-deploy Action, or include
   the built bins in the Pages deployment).
3. (Optional) Attach the same bins to a versioned GitHub Release for direct download.
````

- [ ] **Step 7: Run the test, expect pass (bin checks skip)**

Run: `"C:/Program Files/Python312/python.exe" -m unittest web.test_manifest -v`
Expected: PASS, with `test_bins_exist_and_nonempty_when_built` reported as **skipped** (firmware not built yet).

- [ ] **Step 8: Commit**

```bash
git add web/index.html web/manifest.json web/.gitignore web/README.md web/test_manifest.py
git commit -m "feat(web-flasher): static site + manifest + validation test

Baked-starter ESP Web Tools page (3 chip families, auto-detect), manifest,
BOM/caveats/publish README, and a manifest-structure test."
```

---

### Task 2: Build script — build baked firmware + merge to single images

**Files:**
- Create: `web/build_flasher.ps1`

**Interfaces:**
- Consumes: the `build-flash-read` skill at `$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1` (`-Target c5|c6|cyd -Fleet -Do build`); each build's `flash_args`.
- Produces: `web/firmware/decoy-c5.bin`, `web/firmware/decoy-c6.bin`, `web/firmware/cyd.bin` (merged, offset-0-flashable).

- [ ] **Step 1: Write `web/build_flasher.ps1`**

```powershell
<#
.SYNOPSIS  Build the three BAKED Simulacra firmwares and merge each into one flashable image for the
           web-flasher (web/firmware/*.bin). -Serve hosts web/ on localhost for a browser flash test.
#>
[CmdletBinding()] param([switch]$Serve)
$ErrorActionPreference = "Stop"
$web = $PSScriptRoot
$root = Resolve-Path (Join-Path $web "..")
$fw = Join-Path $web "firmware"

if ($Serve) {
    Set-Location $web
    Write-Host "serving $web at http://localhost:8000  (Chrome/Edge, plug in a board, Connect & Flash)" -ForegroundColor Cyan
    & "C:/Program Files/Python312/python.exe" -m http.server 8000
    return
}

New-Item -ItemType Directory -Force $fw | Out-Null
$skill = "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1"

# target, chip, IDF export, build dir (relative to repo root), output name
$targets = @(
    @{ t="c5";  chip="esp32c5"; idf="v5.5"; bdir="build";     out="decoy-c5.bin" },
    @{ t="c6";  chip="esp32c6"; idf="v5.4"; bdir="build";     out="decoy-c6.bin" },
    @{ t="cyd"; chip="esp32";   idf="v5.4"; bdir="cyd\build"; out="cyd.bin" }
)

foreach ($x in $targets) {
    Write-Host "=== building baked $($x.t) ===" -ForegroundColor Cyan
    & $skill -Target $x.t -Fleet -Do build
    if ($LASTEXITCODE -ne 0) { throw "build failed for $($x.t)" }

    # merge in a FRESH shell so each target's IDF export/env is isolated (avoids cross-target collision)
    $bdir = Join-Path $root $x.bdir
    $outAbs = Join-Path $fw $x.out
    $exportPs1 = "$env:USERPROFILE\esp\$($x.idf)\esp-idf\export.ps1"
    $cmd = "& '$exportPs1' *> `$null; Set-Location '$bdir'; " +
           "python -m esptool --chip $($x.chip) merge_bin -o '$outAbs' '@flash_args'"
    powershell -NoProfile -Command $cmd
    if ($LASTEXITCODE -ne 0) { throw "merge failed for $($x.t)" }
    if (-not (Test-Path $outAbs)) { throw "no output for $($x.t)" }
    Write-Host "  -> $outAbs ($([math]::Round((Get-Item $outAbs).Length/1KB)) KB)" -ForegroundColor Green
}
Write-Host "done. run  web\build_flasher.ps1 -Serve  to flash a board in the browser." -ForegroundColor Cyan
```

- [ ] **Step 2: Run the build script**

Run (from repo root, a shell that can launch the skill):
`web\build_flasher.ps1`
Expected: three `=== building baked ... ===` blocks, each ending `-> ...decoy-c5.bin (N KB)` etc.; on completion `web/firmware/decoy-c5.bin`, `decoy-c6.bin`, `cyd.bin` all exist and are non-empty. (If a chained-shell IDF env error appears — `No module named esp_idf_monitor` — re-run; each target's merge is isolated in its own `powershell -NoProfile` to prevent that.)

- [ ] **Step 3: Verify the bins via the Task-1 test (now exercised, not skipped)**

Run: `"C:/Program Files/Python312/python.exe" -m unittest web.test_manifest -v`
Expected: PASS with `test_bins_exist_and_nonempty_when_built` now **running** (not skipped) and passing.

- [ ] **Step 4: Sanity-check a merged image is a valid esptool image**

Run: `& "$env:USERPROFILE\esp\v5.5\esp-idf\export.ps1" *> $null; python -m esptool image_info web\firmware\decoy-c5.bin | Select-String "Chip|Segment|Checksum"`
Expected: prints chip = ESP32-C5 and a valid checksum line (confirms the merge produced a real bootable image, not garbage).

- [ ] **Step 5: Commit**

```bash
git add web/build_flasher.ps1
git commit -m "feat(web-flasher): build script (baked firmware -> merged bins)

Builds the three baked firmwares via build-flash-read and merges each into a
single offset-0 image in web/firmware/; -Serve hosts web/ on localhost."
```

---

### Task 3: On-bench end-to-end acceptance (manual, with the user)

**Files:** none (validation only).

**Interfaces:** Consumes the built `web/firmware/*.bin` (Task 2) and the served page.

- [ ] **Step 1: Serve the flasher**

Run: `web\build_flasher.ps1 -Serve`
Expected: `serving ... at http://localhost:8000`.

- [ ] **Step 2: Flash a real board through the browser (user + hardware)**

In desktop **Chrome or Edge**, open `http://localhost:8000`. Plug in one board (start with the CYD on
its COM port, or a C5). Click **Connect & Flash**, pick the serial port, confirm. Expected: ESP Web
Tools reports the detected chip family (e.g. "ESP32" for the CYD), erases, writes, and verifies to 100%.

- [ ] **Step 3: Confirm the board boots the flashed firmware**

Read serial to confirm the freshly-flashed board runs (reuse the skill's reader):
`& "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py"` equivalently — e.g. for the CYD:
`python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port <COM> --seconds 6 --reset yes --grep "panel up|espnow|coexist"`
Expected: the board prints its normal boot lines (CYD: `panel up: live radar loop starting`; a decoy:
`responder up` / `burst ch=`), proving the browser-flashed image is the real, working firmware.

- [ ] **Step 4: Record the result**

Update `private/PROJECT-MAP.md` §11 with the outcome (which chip was flashed via browser, that it
booted). No commit needed (private/ is gitignored). If the flash failed, capture the ESP Web Tools
error and stop — do not mark the feature done.

---

## Post-plan: finishing

After Task 3 passes, use **superpowers:finishing-a-development-branch**: verify `web/test_manifest.py`
+ the full `tools/*/tests` suites still pass, then present merge options. This work is on `main`
(local, unpushed) unless a branch was created; keep the standing stance (no push without the user).
Publishing the flasher (push → Pages/release) is explicitly a separate, user-triggered step documented
in `web/README.md`.

## Self-review notes

- Spec §Components 1–6 all covered: index.html (T1), manifest.json (T1), firmware bins (T2), build
  script (T2), README (T1), test_manifest (T1). Local-validation flow → T3.
- No placeholders: all file contents are complete; the C5 board is named concretely
  (ESP32-C5-DevKitC-1) in the README.
- Consistency: the three `chipFamily`/path pairs match across manifest, test (`EXPECTED`), and build
  script (`out` names) — `decoy-c5.bin` / `decoy-c6.bin` / `cyd.bin`.
