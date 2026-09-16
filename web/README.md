# Simulacra Web-Flasher

Flash a Simulacra fleet from your browser - no toolchain. Open `index.html` (served over http/https)
in **desktop Chrome or Edge**, plug in a board, click **Connect & Flash**. The chip is auto-detected
and the correct role is installed:

| Board | Chip | Role |
|---|---|---|
| Waveshare ESP32-C5-WIFI6-KIT - **suggested** | ESP32-C5 | Ward decoy |
| SparkFun Thing Plus ESP32-C6 | ESP32-C6 | Shade decoy |
| CYD - ESP32-2432S028 (2.8" ESP32 display) | ESP32 (classic) | Vigil controller |

**The Waveshare ESP32-C5-WIFI6-KIT is the suggested decoy board** - dual-band, and it takes and
charges its own battery. That is the specific board the project is developed and tested on; any
ESP32-C5 runs the same firmware, but battery sensing is board-specific, so a different C5 may need
its own wiring. ESP32-C6 remains fully supported for anyone who wants the lower-power/
everyday-carry variant.
A minimal fleet is **one decoy + one CYD**. Flash each board in turn.

## Regime: provisioned, keyed in your browser

This flasher installs the **provisioned** build, with the fleet's signing keypair generated in the
browser and written into the images on their way to the board.

The problem it solves is that anything compiled into a published binary is public. These images are
downloadable from the Pages site, so a key baked into one is a key everybody has, and regenerating it
per release just ships a new secret in the new download. Confirmed rather than assumed: the control
secret was findable at a fixed offset in a built CYD image.

So the flasher works like this:

1. **Generate a fleet key.** An Ed25519 keypair from the browser's CSPRNG. The seed is kept in
   `localStorage` so every board you flash from that browser joins the same fleet, and there is a
   backup file to download.
2. **Prepare images.** All three firmwares are downloaded and the keypair is written into them here,
   in the page. The Vigil gets the 64-byte secret, decoys get the 32-byte public half. What CI
   compiled in is a placeholder that no board ever runs.
3. **Connect & Flash.** esp-web-tools reads the chip and installs the matching patched image.

Everything downstream is then per-install for free: the Vigil mints a random ESP-NOW transport key on
first boot and hands it to decoys during the authenticated enrollment, so that key never exists in
any binary either.

**Keep the backup.** The key lives only in that browser profile. Lose it and adding a fourth board
later means re-keying and reflashing every board you already did.

`python tools/gen_ctrl_key.py` does the same job for a from-source build, and produces the same
format, so a fleet keyed either way is interchangeable.

### Rebuilding from source after a browser flash

A browser-keyed fleet's key lives in three places: that browser profile, the backup file, and the
boards. The source tree still holds a **different** key, so building and flashing the Vigil from
source silently re-keys it. Every enrolled decoy stops verifying its signatures, the roster still
lists them, and nothing on screen says why.

Adopt the fleet's key into the tree first, then build as normal:

```sh
python tools/gen_ctrl_key.py --from-backup simulacra-fleet-key-<fingerprint>.json
```

It prints the fingerprint it adopted. Check that against the one the flasher shows before you flash
anything. Going the other way, to add a browser-flashed board to a fleet you keyed from source:

```sh
python tools/gen_ctrl_key.py --export-backup fleet.json    # then import it in the page
```

Both directions produce the same file format, and `--from-backup` also accepts a bare 32-byte hex
seed or the 64-byte `seed||pub` secret you get from reading the block off a board.

### What this does not give you

- **No flash encryption.** Anyone who takes a board and reads its flash recovers that fleet's
  transport key. The Vigil can revoke a board, which re-keys the fleet and re-enrolls the rest.
  Treat a decoy as something you could lose.
- **Pairing is deliberate, not automatic.** After flashing, open **CONTROL** on the Vigil and tap
  **PAIR NEW NODE** for a 30-second window, then accept each board by matching the fingerprint it
  prints on its serial console. Boards never join a fleet on their own.

## Caveats

- **Chrome / Edge desktop only** - Web Serial isn't in Firefox or Safari.
- **Prescriptive BOM** - a plain non-CYD ESP32 board would receive CYD firmware. Use the boards above.
- **Flash every board from the same browser**, or import the backup first. Two different keys means
  two fleets that ignore each other.
- **The images must carry the key blocks.** `patchKey` refuses an image whose magic is missing rather
  than flashing a published key, so a firmware built from a tree without `sim_ctrl_key.h` /
  `sim_ctrl_sk.h` in block form will be rejected at the Prepare step.

## How the patching works (maintainer)

`web/keypatch.js` rewrites the key and repairs the image. `tools/patch_keyblock.py` is the reference
implementation and produces byte-identical output; `web/test_keypatch.py` runs the JS under node and
diffs the bytes, because two implementations of a byte-exact format drift quietly.

Three things will brick a patched image, all found on hardware:

1. a 1-byte XOR checksum over every segment's data, seeded `0xEF`, at the last byte of the 16-byte
   block after the final segment
2. a 32-byte SHA256 over the app image, appended, when header byte 23 is 1
3. the app not starting at byte 0. The published images are **merged** (padding, bootloader,
   partition table, app), so the app is located through the partition table at `0x8000`. Byte 0
   being `0xE9` proves nothing: the bootloader shares that magic and sits at offset 0 on C6 and H2.

Get any of them wrong and the image flashes cleanly, the boot log stops after the partition table,
and the board never starts.

## Build the binaries (maintainer)

Run from the repo root in a shell that can reach the `build-flash-read` skill:

```
web\build_flasher.ps1
```

This builds the three firmwares (correct IDF version per chip) and writes merged, single-file images
to `web/firmware/*.bin` (gitignored). Then serve locally to test:

```
web\build_flasher.ps1 -Serve      # http://localhost:8000
```

## Publish (maintainer, when ready)

Automated by `.github/workflows/flasher.yml` - it builds the three provisioned firmwares in CI,
merges each, and deploys `web/` + a fresh `firmware/` to GitHub Pages, so **no binaries ever live in
git**. Note that the deploy step copies `web/*.js`: the page is ES modules, and a missing one means
the key step 404s and the flasher quietly loses the thing that makes it safe.

1. Push the repo (after a PII scan).
2. Repo **Settings → Pages → Source = "GitHub Actions"** (one-time).
3. Push a change under `main/`, `cyd/`, `components/`, or `web/` (or hit **Run workflow**) → the Action
   builds + deploys → the flasher is live at `https://<owner>.github.io/<repo>/`.

The workflow is a first-pass sketch (three builds across two IDF versions, ESP32-C5 is new) - expect to
tune it on the first CI run. See the caveats commented at the top of the workflow file.
