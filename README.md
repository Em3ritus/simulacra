```
root@simulacra:~# ./summon --crowd --bury-signal
  ██████  ██▓ ███▄ ▄███▓ █    ██  ██▓    ▄▄▄       ▄████▄   ██▀███   ▄▄▄
▒██    ▒ ▓██▒▓██▒▀█▀ ██▒ ██  ▓██▒▓██▒   ▒████▄    ▒██▀ ▀█  ▓██ ▒ ██▒▒████▄
░ ▓██▄   ▒██▒▓██    ▓██░▓██  ▒██░▒██░   ▒██  ▀█▄  ▒▓█    ▄ ▓██ ░▄█ ▒▒██  ▀█▄
  ▒   ██▒░██░▒██    ▒██ ▓▓█  ░██░▒██░   ░██▄▄▄▄██ ▒▓▓▄ ▄██▒▒██▀▀█▄  ░██▄▄▄▄██
▒██████▒▒░██░▒██▒   ░██▒▒▒█████▓ ░██████▒▓█   ▓██▒▒ ▓███▀ ░░██▓ ▒██▒ ▓█   ▓██▒
▒ ▒▓▒ ▒ ░░▓  ░ ▒░   ░  ░░▒▓▒ ▒ ▒ ░ ▒░▓  ░▒▒   ▓▒█░░ ░▒ ▒  ░░ ▒▓ ░▒▓░ ▒▒   ▓▒█░
░ ░▒  ░ ░ ▒ ░░  ░      ░░░▒░ ░ ░ ░ ░ ▒  ░ ▒   ▒▒ ░  ░  ▒     ░▒ ░ ▒░  ▒   ▒▒ ░
░  ░  ░   ▒ ░░      ░    ░░░ ░ ░   ░ ░    ░   ▒   ░          ░░   ░   ░   ▒
      ░   ░         ░      ░         ░  ░     ░  ░░ ░         ░           ░  ░
  summon the crowd · bury the signal              [ ble · wifi · esp-now ]
```

<p align="center">
  <img src="https://img.shields.io/badge/license-GPL--3.0-1f9e6a?style=flat-square" alt="License: GPL-3.0">
  <img src="https://img.shields.io/badge/platform-ESP32--C5%20·%20C6%20·%20CYD-9d4edd?style=flat-square" alt="Platform">
  <img src="https://img.shields.io/badge/ESP--IDF-5.4%20·%205.5-1f9e6a?style=flat-square" alt="ESP-IDF">
  <img src="https://img.shields.io/badge/radios-BLE%20·%20Wi--Fi-14a06a?style=flat-square" alt="Radios">
</p>

**A modular, multi-node ESP32 anti-tracking system.** Simulacra continuously fabricates a
churning crowd of plausible-but-fake wireless devices around you - drowning your real devices in
noise so that passive trackers, ALPR add-ons, and co-travel correlators can't reliably pick your
signal out of the crowd - while passively watching for the trackers that follow you.

It is built from cooperating nodes, each playing to a different board's strengths, coordinated
over an encrypted ESP-NOW link.

> **Try it in your browser - no toolchain.** Generate a fleet key, plug in a board and flash at
> **[em3ritus.github.io/simulacra](https://em3ritus.github.io/simulacra/)** (desktop Chrome/Edge).

**Jump to:** [Legal](#️-legal--responsible-use) · [How it works](#how-it-works) ·
[Architecture](#architecture--the-nodes) · [Features](#features) ·
[Security model](#security-model) · [Hardware](#hardware) · [Build & flash](#build--flash) ·
[Repo layout](#repository-layout) · [Offline tools](#offline--bench-tools) ·
[Recent updates](#recent-updates) · [Contributing](#contributing) · [Credits](#credits) ·
[License](#license) · **[Wiki](https://github.com/Em3ritus/simulacra/wiki)**

---

## ⚠️ Legal & responsible use

Simulacra is a research and personal-privacy tool. It transmits synthetic BLE/Wi-Fi advertising
traffic and observes the RF around you. **You are responsible for complying with the radio
regulations and laws in your jurisdiction.** Do not use it to harass, impersonate a specific
person or device, interfere with networks or emergency services, or evade lawful process. Use it
on hardware you own, in ways that are legal where you are. No warranty; see the license.

**TURBO is not a DoS mode.** It's still in-spec, non-connectable BLE advertising and standard
802.11 probe requests at legal power - no deauth, no jamming, no malformed frames. A handful of
ESP32 boards cannot meaningfully deny service to nearby real networks or clients; what it does is
raise the volume of traffic someone has to process. The same rules above apply to it.

---

## How it works

- **A churning synthetic crowd.** Instead of hiding a device, Simulacra hides it *in a crowd* -
  generating a rotating population of realistic fake devices (random-static MACs, plausible
  vendor/format shapes, realistic advertising cadence) that constantly turns over.
- **Structure, never identity.** The self-learning engine harvests the *shape* of real nearby
  device adverts (vendor/format/AD structure) and turns them into new decoy archetypes - but it
  strips all identifying content. A learned template is "a device *of this kind*", never "*this
  device*". A hard "Law-3" gate refuses to ever learn or emit forbidden identity subtypes
  (e.g. Apple continuity / Fast Pair pairing beacons).
- **Passive detection.** While it churns, it watches for followers - devices that persist with you
  - and matches adverts against a signature database of known trackers (AirTag / SmartTag / Tile)
  and surveillance gear.
- **Coordinated, not cloned.** Nodes share a learned library and exclude each other from their own
  models over an authenticated ESP-NOW link, so the fleet behaves like one diverse crowd rather
  than several identical decoys.

## What it doesn't do

These follow directly from the design rather than being gaps waiting to be filled, so they're worth
stating plainly.

- **It can't hide a device that already has a persistent identifier.** Everything Simulacra emits is
  gone inside 15 minutes and never returns, so in a fixed location long-term observation separates
  decoys from real devices with a single filter: *any identifier seen on more than one day*. That
  drops the whole fleet and leaves the real persistent set untouched. Simulacra defeats discovery,
  census, and correlation. It does not defeat a targeted lookup of an identifier someone already
  holds, and nothing external to the device can - that's a device-side fix (client MAC
  randomisation, suppressing probe requests once associated, radios off on fixed infrastructure that
  doesn't need them). In a static deployment the honest value is that an observer can't tell how many
  people are there or what changed week to week.
- **`presence_duration` is the weakest axis on the audit scorecard, deliberately.** It's the measured
  cost of the 15-minute ceiling. Closing it by re-adding long-lived identities is the one fix that
  must not be applied, and `tools/decoy_audit/tests/test_addr_onair_cap.py` exists to fail if someone
  tries.
- **One Vigil per fleet, for now.** Authorisation is key-based rather than identity-based, so any
  Vigil holding the control key is already authorised. What blocks it is the per-decoy replay floor:
  it's a single salt-independent counter, and two Vigils spending their own counter blocks would
  silently fight over it. Fixable (make the floor per-Vigil, keyed by signing identity, the same
  shape the telemetry path already uses) but it's a wire-version bump and not yet started.
- **Decoys are non-connectable by design.** They don't beacon fake APs or accept associations.

## Architecture - the nodes

| Node | Board | Role |
|------|-------|------|
| **Ward** | ESP32-C5 (dual-band Wi-Fi 6) | Fixed / vehicle decoy - dense, dual-band crowd generation |
| **Shade** | ESP32-C6 | Mobile / everyday-carry decoy - lean, 2.4 GHz, low-profile |
| **Vigil** | ESP32 + "Cheap Yellow Display" | Controller & librarian - radar/status screen, touch control, encrypted SD library, fleet key custody |

Roles are selected at build time so one firmware tree serves every board. **ESP32-C5 (Ward) is the
suggested board to start with** - dual-band, and it takes and charges its own battery - so a
minimal build is one C5 + one Vigil. ESP32-C6 (Shade) remains fully supported for anyone who wants
the lower-power/everyday-carry variant.

## Features

- Rotating BLE decoy crowd with realistic random-static MACs, vendor/format shapes, and advertising
  cadence, on a **death/rebirth lifecycle** so the population turns over like a real crowd instead of
  a fixed set of decoys. Every identifier is capped at **15 minutes on air** (`ADDR_MAX_ONAIR_MS`),
  matching real-phone RPA rotation: RPAs/NRPAs rotate on realistic schedules, and static addresses
  honour the same ceiling by dying and being reborn as wholly new devices rather than rotating - an
  address whose top two bits declare "I am static" must not rotate, or it contradicts itself on air.
- **Wi-Fi PAN cover:** independent probe-request agents built from **capture-derived IE structures**.
  Five ship, including two 2.4 GHz-only ones modelling devices with no 5 GHz radio - a class an
  invented archetype table never produces. Each fake phone carries its **own 802.11 sequence
  counter**, so the real device can't be fingerprinted out of the probe traffic by its
  sequence/timing constellation. The agent population **matches the ambient device density**, and a
  realistic **minority (~21%, measured)** probe named public networks - drawn from a fixed pool of
  ubiquitous open SSIDs (xfinitywifi, attwifi, eduroam …), **never an observed or local one** - so
  the fake phones blend with the real phones probing the same hotspots. A persona's saved-network set
  is **redrawn on every MAC rotation**, so it can't be used to link one MAC to the next.
- On-device **self-learning** of ambient device *shapes* into new decoy archetypes (structure-only,
  Law-3 gated), synced across the fleet and persisted to an AES-GCM-sealed SD library on Vigil, keyed
  from the CONTROL secret in the provisioned regime (`-DSIMULACRA_FLEET_PROVISION=1` - rotates
  automatically with `tools/gen_ctrl_key.py`). The baked-key demo regime still keys it from a
  published, non-secret placeholder constant - an accepted tradeoff for that regime, matching its
  documented "shared key, not private" posture elsewhere.
- **Passive follower detection** and **tracker/surveillance fingerprint** matching.
- **Signed fleet control:** Vigil pushes Ed25519-signed behaviour presets to every decoy over
  ESP-NOW, in two modes. **AUTO** sizes each board's crowd from the ambient device density it
  measures, so the fleet matches the room it's actually in; an operator-set cap bounds how far it
  can scale. **MANUAL** (LOW / MED / HIGH) pins a fixed fraction of each board's capacity and
  ignores the room. The console shows which preset the fleet is **actually running** (live vs.
  pending), plus a signed one-tap **clear-threats** command that wipes every decoy's stale detection
  history from the panel. **TURBO** is a field-use flood mode, not a realism mode: every board
  independently maxes its own BLE and Wi-Fi churn - no room-density matching - to raise the
  processing cost of whoever's watching. Manual-only, two-tap confirm, sticky until changed.
- **Boards are additive.** Each decoy sizes its own crowd independently, so adding a board adds
  cover rather than redistributing it, while AUTO's density matching keeps the *fleet* honest in a
  sparse room. Every board also carries its own set of hardware advertising slots, which is the real
  limit on how fast fresh identities reach the air.
- **On-air fleet enrollment (ECDH):** decoys ship with no shared transport key and enroll on-air via
  a mutually-authenticated 3-message handshake, so **a captured decoy can't forge commands or
  impersonate the Vigil to other decoys.** It does end up holding the same shared fleet transport key
  as every other member, though - see Security model below for what that key protects and doesn't.
- **Vigil console:** an at-a-glance **protection posture** - one honest word for your current state
  (`CLOAKED` / `EXPOSED` when there's no crowd to hide in / `HUNTED` when a follower is confirmed /
  `DARK`) - plus a live radar/threat display, grouped status pages, a per-node fleet roster, and
  enroll/revoke control for fleet members. Tap in for depth: a **per-node telemetry console**, a
  **per-threat detail card** (device class, confidence, vendor, persistence), a two-page **system
  console + colour legend**, and signed fleet control - all from the touch panel, no laptop.
- **Fleet health at a glance:** decoys report TX self-health and battery state over the link, so
  Vigil surfaces a `DEGRADED` or `LOW BATT` node on its roster before it goes quiet in the field.

## Security model

- **Asymmetric by design.** The controller (Vigil) holds the private signing key; decoys hold only
  the public key. A captured decoy can verify commands but cannot forge them or control the fleet.
- **A published binary cannot hold a secret, so the keys are made where you are.** The web-flasher
  images are downloadable, so anything compiled into them is public: verified by finding the control
  secret at a fixed offset in a built CYD image. Generating a fresh keypair per release would not
  help, because the new secret ships in the new download. So the flasher **generates the fleet's
  Ed25519 keypair in your browser** and rewrites it into the images before flashing; what CI compiles
  in is a placeholder no board ever runs. Everything downstream is then per-install for free, because
  the Vigil mints a random ESP-NOW transport key on first boot and hands it to decoys over the
  authenticated enrollment, so that key exists in no binary either. `tools/gen_ctrl_key.py` does the
  same job for a from-source build and produces the same format.
- **Never trust the wire.** Every synced/seeded template is re-gated (budget + Law-3 + hash recompute)
  on receipt, so a leaked key or spoofed node still cannot inject a forbidden identity.
- **The fleet transport key is shared and currently unencrypted at rest.** Every enrolled decoy holds
  the same symmetric key that encrypts status/threat/learn-sync traffic on the mesh - this is what
  lets nodes talk to each other and the Vigil at all, and it's a different key from the CONTROL
  signing key above. No flash/NVS encryption is configured on any board, so a physically recovered
  decoy's key is recoverable with `esptool read_flash`. **Revoke a lost or captured board immediately**
  (fleet roster → REVOKE) - this rotates the whole fleet onto a new key and re-enrolls the survivors,
  cutting the compromised board out. Until you do, treat a missing decoy as a live risk to the mesh's
  confidentiality, not just a lost board.
- **Structure-only learning.** Real-world captures are stripped to skeletons; no bystander identities
  or names are stored or emitted.
- **Measured, not assumed.** A host-side audit suite compiles the *real* generator and scores how
  separable the decoys are from a real crowd across every axis an adversary could use - address type,
  advertising interval, vendor mix, AD structure, presence/lifespan, and RSSI - turning "are the
  decoys convincing?" into a single regression-gate number instead of a hope.

## Hardware

- **ESP32-C5** (Ward) - the suggested/primary decoy board. Dual-band Wi-Fi 6, and it takes and
  charges its own LiPo battery. Developed and tested against the **Waveshare ESP32-C5-WIFI6-KIT**,
  which is the board the battery wiring and pin choices assume. Other ESP32-C5 boards run the same
  firmware, but battery sensing is board-specific - the Waveshare kit has no fuel gauge, so it uses
  an ADC divider on GPIO6 rather than the I2C gauge some boards carry.
- **ESP32-C6** (Shade) - fully supported alternative decoy, leaner and lower-power for
  everyday-carry.
- **ESP32** "Cheap Yellow Display" (ILI9341 + XPT2046 touch + microSD) for Vigil.

Specific boards and part numbers: see [`web/README.md`](web/README.md)'s board table. For what
every screen and setting on the Vigil console does once it's flashed, see the
**[project wiki](https://github.com/Em3ritus/simulacra/wiki)**.

## Build & flash

### Flash from your browser - no toolchain

The fastest way to build a fleet:

### **→ [em3ritus.github.io/simulacra](https://em3ritus.github.io/simulacra/)**

Open it in desktop **Chrome or Edge**. Generate a fleet key, click **Prepare images**, then plug in
each board and **Connect & Flash** - the **browser web-flasher** (ESP Web Tools / Web Serial)
auto-detects the chip and installs the right role (C5 → Ward, C6 → Shade, ESP32 → CYD), no ESP-IDF
and no command line. Source and self-host notes: [`web/`](web/).

**The keys are generated in your browser, not shipped in the download.** Anything compiled into a
publicly downloadable binary is public, so the page makes an Ed25519 keypair locally and rewrites it
into the images before they are flashed. The Vigil then mints a random ESP-NOW transport key on
first boot and hands it to decoys during enrollment, so nothing that matters ever existed in a
published file. Flash every board from the same browser, or import your backup first, or you will
end up with two fleets that ignore each other.

**Save the key backup.** It lives only in that browser profile. Without it, adding a board later
means re-keying and reflashing every board you already did.

This is not full device security: there is no flash encryption, so anyone who takes a board can
recover that fleet's transport key from it. The Vigil can revoke a board, which re-keys the fleet and
re-enrolls the rest.

### Build from source (full / provisioned regime)

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) - v5.5 for the C5 decoy,
v5.4 for the C6 decoy and the Vigil (classic ESP32). With the IDF environment active:

```sh
# Decoy (Ward / Shade) - from the repo root
idf.py set-target esp32c5          # or esp32c6 for Shade
idf.py -DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1 build
idf.py -p <PORT> flash monitor

# Vigil controller - from ./cyd
cd cyd
idf.py set-target esp32
idf.py -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1 build
idf.py -p <PORT> flash monitor
```

Build-time gates (`-D…=1`) select each node's role and optional subsystems (fleet control,
enrollment, self-test). See `docs/` for design specs and per-feature notes.

**Fleet-key regime must match across the whole fleet.** With `-DSIMULACRA_FLEET_PROVISION=1`
(shown above) the Vigil mints a random fleet key at first boot and grants it to decoys through
enrollment - so decoys need the flag too, or they fall back to the baked compile-time key, never
receive the grant, and stay invisible to the controller even while healthy. To run the simpler
baked-key regime instead, omit `-DSIMULACRA_FLEET_PROVISION=1` from **every** node (all then share
the key in `components/simulacra_radar/radar_key.h`). Changing any `-D…` flag needs a clean build
(`rm -rf build sdkconfig`) so the old define doesn't linger.

**Rotate the CONTROL signing key before real use.** `-DSIMULACRA_CONFIG_CTRL=1` (shown above) is a
separate keypair from the fleet-transport key: whoever holds it can sign presets and CLEAR THREATS
commands for every node trusting the matching public key. The committed placeholder bytes
(`cyd/main/sim_ctrl_sk.h.example`) are **public knowledge** - they're in this repo's git history -
so a fleet left on them has no real control-plane authentication. Regenerate before deploying
anything you care about:

```sh
python tools/gen_ctrl_key.py       # rewrites the secret + public key headers
```

then rebuild and reflash **every** board together (decoys bake the new public key, the Vigil the new
secret) - a half-rotated fleet stops verifying.

**If your fleet was flashed from the browser, adopt its key first.** That fleet's key exists only in
the browser profile that made it, its backup file, and the boards; this tree holds a different one.
Building from source and flashing the Vigil silently re-keys it, and every enrolled decoy stops
verifying while the roster still lists them - a fleet that looks fine and answers nothing.

```sh
python tools/gen_ctrl_key.py --from-backup fleet-key.json   # adopt; prints the fingerprint
python tools/gen_ctrl_key.py --export-backup fleet.json     # or go the other way
```

Check the fingerprint it prints against the one the flasher shows before flashing anything.
`--from-backup` also takes a bare 32-byte hex seed, or the 64-byte `seed||pub` secret read off a
board, so a fleet can be recovered from hardware if the backup is lost.

## Repository layout

```
main/                    decoy firmware (churn, lifecycle, probes, self-learning, detection, ESP-NOW)
cyd/                     Vigil controller firmware (display, touch, SD librarian, fleet authority)
components/simulacra_radar/  shared code (wire formats, learning, signatures, rendering)
components/tweetnacl/     vendored TweetNaCl (Ed25519 / X25519)
tools/pcap_learn/         replay a BLE capture through the real learn/detect pipeline
tools/decoy_audit/        score how separable the BLE decoys are from a real crowd
tools/probe_audit/        verify Wi-Fi probe frames are archetype-faithful and Law-3 safe
tools/radar_audit/        verify the Vigil console's render/control/fleet-status logic on the host
tools/seq_gate/           post-flash check that each fake phone's 802.11 sequence stays independent
web/                      browser web-flasher - keys a fleet locally, then flashes it, no toolchain
docs/                     design specs, implementation plans, and the roadmap
```

## Offline & bench tools

Simulacra's host tools compile the **real firmware code** (not reimplementations), so behaviour is
verified against the same source that runs on-device:

- **`tools/pcap_learn/`** - replay a BLE capture (`.pcap` or `.pcapng`) through the actual
  self-learning pipeline (validate structure-only learning, emit a seed library) and the tracker
  matcher with dwell/co-travel analysis.
- **`tools/decoy_audit/`** - compile the real BLE generator on the host and score how separable the
  synthetic crowd is from a real capture, as a ranked scorecard plus a single regression-gate number.
- **`tools/probe_audit/`** - byte-exact verification that the Wi-Fi probe frames match the
  capture-derived IE structures, and that directed-SSID probes only ever name generic **public**
  networks from a fixed compiled-in pool - never one sourced from observed or local traffic.
- **`tools/radar_audit/`** - compiles the Vigil console's own render/control/fleet-status code on the
  host, so every screen (radar, node/threat detail, INFO console, CONTROL presets) and the fleet
  aggregation logic (stale-node pruning, threat dedup, live-vs-pending preset) are verified against
  the exact source that runs on the CYD.
- **`tools/seq_gate/`** - a two-board post-flash gate confirming each fake phone keeps its own
  802.11 sequence counter after an IDF/toolchain bump.

Each tool has its own README with build and run steps.

## Recent updates

Newest first - full history in [`CHANGELOG.md`](CHANGELOG.md). Forward-looking milestones live in
[`docs/ROADMAP.md`](docs/ROADMAP.md).

- **The web flasher makes a fleet nobody else can read.** A published binary cannot hold a secret,
  so a key baked into a downloadable image protects nothing. The fleet's Ed25519 keypair is now
  generated **in your browser** and written into the images before flashing; the Vigil then mints a
  random ESP-NOW transport key on first boot and hands it to decoys over the authenticated
  enrollment, so nothing that matters was ever published. Fixing it surfaced a third way to brick a
  patched image: the published images are *merged*, so the app does not start at byte 0, and walking
  segments from there parses the bootloader and hashes the wrong range. Verified on hardware.
- **Enrollment is a button, not a hidden gesture.** Pairing used to hang off a 1.5 s press-and-hold
  anywhere on the screen, which did one of three different things depending on state you could not
  see. It is a labelled button on CONTROL now, and its text is the state: `PAIR NEW NODE`,
  `PAIRING 24s`, `ROTATE FLEET KEY?`, or `ACCEPT NODE` with the joining board's full fingerprint.
- **No persistent identifiers, anywhere.** A slice of the crowd used to hold one static address for
  4-12 h so the fleet would reproduce the long presence tail real environments have. That inverted
  the point - a decoy holding one address for hours, carried by the operator, is a *better* tracking
  handle than the phone it covers. A 15-minute ceiling now applies across both radios, matching real
  phone RPA rotation, and static devices honour it by dying and being reborn as wholly new devices.
  Wi-Fi saved-network sets are redrawn on every MAC rotation too - a set that outlives a rotation is
  the standard way MAC randomisation gets defeated in the field.
- **AD structure and Wi-Fi probe shape are learned, not hardcoded.** Both were fitted to single
  captures and did not survive a change of environment; a census of 877 real probing devices found
  none of the shipped Wi-Fi IE layouts present even once. Structure now tracks the room the way
  intervals and vendor mix already did. Cross-validated AD-structure separability
  **[0.153-0.925] -> [0.088-0.381]**.
- **Decoys never emit a tracker signature.** Three paths could make a decoy match this project's own
  tracker detector, meaning nearby phones would warn their owners that an unknown tracker was
  travelling with them. All closed, behind a fail-closed gate.
- **The ESP-NOW link went quiet.** Measured from outside, ~99 vendor action frames/min against an
  ambient median of zero, most of it the Vigil rebroadcasting its whole library every 20 s.
  Now delta-based and jittered: **66.9/min -> 4.2/min**.
- **Project wiki.** A full [CYD console guide + reference](https://github.com/Em3ritus/simulacra/wiki)
  (every screen, setting, preset, and status word explained) and a project-wide glossary, published
  and linked from the README.
- **TURBO flood mode.** A sixth console preset for field use, not realism: trigger it fleet-wide
  (two-tap confirm) and every board independently maxes its own BLE + Wi-Fi churn - no room-density
  matching, no persona coupling - burning through as much identifier space as the hardware sustains.
  Hardware-verified at both full 3-node-fleet and single-standalone-board (K=1) scale with zero radio
  TX errors.
- **Vigil console, fully fleshed out.** The touch dashboard became a real operator console: a
  **per-node telemetry page**, a **per-threat detail card**, a two-page **INFO** system/fleet console
  with a colour/posture **legend**, **live-vs-pending preset** state (flags a `MIXED` fleet), and a
  signed, two-tap **CLEAR THREATS** control.
- **Browser web-flasher - [live](https://em3ritus.github.io/simulacra/).** Build a whole fleet from
  a web page - ESP Web Tools over Web Serial, auto-detecting the board and installing the right role.
  The fleet's signing keypair is generated in the browser and written into the images before
  flashing, so the published download holds only a placeholder. A CI action builds the three
  firmwares and deploys the flasher to GitHub Pages on every firmware change, so no binaries ever
  live in git.

## Contributing

**Issues welcome** - bug reports, questions, hardware requests. **PRs aren't being merged yet** - see
[`CONTRIBUTING.md`](CONTRIBUTING.md) for why and what to expect.

## Credits

Originally forked from and built on [**0xXyc/splinter**](https://github.com/0xXyc/splinter) - the
project that started the idea. Simulacra extends it into a multi-node, self-learning, fleet-managed
system.

## License

**[GNU GPL v3.0](LICENSE).** Simulacra is derived from [0xXyc/splinter](https://github.com/0xXyc/splinter),
which carries no license file of its own - its author has confirmed directly that splinter was
written for others to build on. The current codebase is almost entirely original work; the small
remainder still tracing back to those early files is covered by that same permission.
