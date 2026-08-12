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
  <img src="https://img.shields.io/badge/license-pending-8a8a8a?style=flat-square" alt="License pending">
  <img src="https://img.shields.io/badge/platform-ESP32--C5%20·%20C6%20·%20CYD-9d4edd?style=flat-square" alt="Platform">
  <img src="https://img.shields.io/badge/ESP--IDF-5.4%20·%205.5-1f9e6a?style=flat-square" alt="ESP-IDF">
  <img src="https://img.shields.io/badge/radios-BLE%20·%20Wi--Fi-14a06a?style=flat-square" alt="Radios">
</p>

**A modular, multi-node ESP32 anti-tracking system.** Simulacra continuously fabricates a
churning crowd of plausible-but-fake wireless devices around you — drowning your real devices in
noise so that passive trackers, ALPR add-ons, and co-travel correlators can't reliably pick your
signal out of the crowd — while passively watching for the trackers that follow you.

It is built from cooperating nodes, each playing to a different board's strengths, coordinated
over an encrypted ESP-NOW link.

> **Try it in your browser — no toolchain.** Plug in a board and flash a starter fleet at
> **[em3ritus.github.io/simulacra](https://em3ritus.github.io/simulacra/)** (desktop Chrome/Edge).

---

## ⚠️ Legal & responsible use

Simulacra is a research and personal-privacy tool. It transmits synthetic BLE/Wi-Fi advertising
traffic and observes the RF around you. **You are responsible for complying with the radio
regulations and laws in your jurisdiction.** Do not use it to harass, impersonate a specific
person or device, interfere with networks or emergency services, or evade lawful process. Use it
on hardware you own, in ways that are legal where you are. No warranty; see the license.

**TURBO is not a DoS mode.** It's still in-spec, non-connectable BLE advertising and standard
802.11 probe requests at legal power — no deauth, no jamming, no malformed frames. A handful of
ESP32 boards cannot meaningfully deny service to nearby real networks or clients; what it does is
raise the volume of traffic someone has to process. The same rules above apply to it.

---

## How it works

- **A churning synthetic crowd.** Instead of hiding a device, Simulacra hides it *in a crowd* —
  generating a rotating population of realistic fake devices (random-static MACs, plausible
  vendor/format shapes, realistic advertising cadence) that constantly turns over.
- **Structure, never identity.** The self-learning engine harvests the *shape* of real nearby
  device adverts (vendor/format/AD structure) and turns them into new decoy archetypes — but it
  strips all identifying content. A learned template is "a device *of this kind*", never "*this
  device*". A hard "Law-3" gate refuses to ever learn or emit forbidden identity subtypes
  (e.g. Apple continuity / Fast Pair pairing beacons).
- **Passive detection.** While it churns, it watches for followers — devices that persist with you
  — and matches adverts against a signature database of known trackers (AirTag / SmartTag / Tile)
  and surveillance gear.
- **Coordinated, not cloned.** Nodes share a learned library and exclude each other from their own
  models over an authenticated ESP-NOW link, so the fleet behaves like one diverse crowd rather
  than several identical decoys.

## Architecture — the nodes

| Node | Board | Role |
|------|-------|------|
| **Ward** | ESP32-C5 (dual-band Wi-Fi 6) | Fixed / vehicle decoy — dense, dual-band crowd generation |
| **Shade** | ESP32-C6 | Mobile / everyday-carry decoy — lean, 2.4 GHz, low-profile |
| **Vigil** | ESP32 + "Cheap Yellow Display" | Controller & librarian — radar/status screen, touch control, encrypted SD library, fleet key custody |

Roles are selected at build time so one firmware tree serves every board.

## Features

- Rotating BLE decoy crowd with realistic random-static MACs, vendor/format shapes, and advertising
  cadence — including **persistent devices with per-type address rotation** (RPAs/NRPAs rotate on
  realistic schedules, static beacons hold) and a **death/rebirth lifecycle**, so the population
  turns over like a real crowd instead of a fixed set of decoys.
- **Wi-Fi PAN cover:** independent, archetype-faithful probe-request agents (iPhone / Galaxy /
  Pixel / generic Android). Each fake phone carries its **own 802.11 sequence counter**, so the
  real device can't be fingerprinted out of the probe traffic by its sequence/timing constellation.
  The agent population **matches the ambient device density** (divided across the live fleet so the
  crowd never over-populates an empty room), and a realistic majority **probe named public networks**
  — drawn from a fixed pool of ubiquitous open SSIDs (xfinitywifi, attwifi, eduroam …), **never an
  observed or local one** — so the fake phones blend with the real phones probing the same hotspots.
- On-device **self-learning** of ambient device *shapes* into new decoy archetypes (structure-only,
  Law-3 gated), synced across the fleet and persisted to an AES-GCM-sealed SD library on Vigil — real
  encryption, but currently keyed from a published, non-secret placeholder constant in the baked-key
  regime, and (a gap being tracked, not yet fixed) from the same placeholder even in the provisioned
  regime as of this writing. Impact is bounded — the library holds structure-only skeletons, no
  bystander identities — but don't treat the SD card itself as sensitive yet.
- **Passive follower detection** and **tracker/surveillance fingerprint** matching.
- **Signed fleet control:** Vigil pushes Ed25519-signed behaviour presets (PAUSE / STEALTH /
  NORMAL / DENSE / MAX / **TURBO**) to every decoy over ESP-NOW — and the console shows which preset
  the fleet is **actually running** (live vs. pending), plus a signed one-tap **clear-threats**
  command that wipes every decoy's stale detection history from the panel. **TURBO** is a field-use
  flood mode, not a realism mode: every board independently maxes its own BLE and Wi-Fi churn — no
  room-density matching — to raise the processing cost of whoever's watching. Manual-only, two-tap
  confirm on the console, sticky until changed.
- **On-air fleet enrollment (ECDH):** decoys ship with no shared transport key and enroll on-air via
  a mutually-authenticated 3-message handshake, so **a captured decoy can't forge commands or
  impersonate the Vigil to other decoys.** It does end up holding the same shared fleet transport key
  as every other member, though — see Security model below for what that key protects and doesn't.
- **Vigil console:** an at-a-glance **protection posture** — one honest word for your current state
  (`CLOAKED` / `EXPOSED` when there's no crowd to hide in / `HUNTED` when a follower is confirmed /
  `DARK`) — plus a live radar/threat display, grouped status pages, a per-node fleet roster, and
  enroll/revoke control for fleet members. Tap in for depth: a **per-node telemetry console**, a
  **per-threat detail card** (device class, confidence, vendor, persistence), a two-page **system
  console + colour legend**, and signed fleet control — all from the touch panel, no laptop.
- **Fleet health at a glance:** decoys report TX self-health and battery state over the link, so
  Vigil surfaces a `DEGRADED` or `LOW BATT` node on its roster before it goes quiet in the field.

## Security model

- **Asymmetric by design.** The controller (Vigil) holds the private signing key; decoys hold only
  the public key. A captured decoy can verify commands but cannot forge them or control the fleet.
- **Never trust the wire.** Every synced/seeded template is re-gated (budget + Law-3 + hash recompute)
  on receipt, so a leaked key or spoofed node still cannot inject a forbidden identity.
- **The fleet transport key is shared and currently unencrypted at rest.** Every enrolled decoy holds
  the same symmetric key that encrypts status/threat/learn-sync traffic on the mesh — this is what
  lets nodes talk to each other and the Vigil at all, and it's a different key from the CONTROL
  signing key above. No flash/NVS encryption is configured on any board, so a physically recovered
  decoy's key is recoverable with `esptool read_flash`. **Revoke a lost or captured board immediately**
  (fleet roster → REVOKE) — this rotates the whole fleet onto a new key and re-enrolls the survivors,
  cutting the compromised board out. Until you do, treat a missing decoy as a live risk to the mesh's
  confidentiality, not just a lost board.
- **Structure-only learning.** Real-world captures are stripped to skeletons; no bystander identities
  or names are stored or emitted.
- **Measured, not assumed.** A host-side audit suite compiles the *real* generator and scores how
  separable the decoys are from a real crowd across every axis an adversary could use — address type,
  advertising interval, vendor mix, AD structure, presence/lifespan, and RSSI — turning "are the
  decoys convincing?" into a single regression-gate number instead of a hope.

## Hardware

- **ESP32-C5** (Ward) and **ESP32-C6** (Shade) decoys.
- **ESP32** "Cheap Yellow Display" (ILI9341 + XPT2046 touch + microSD) for Vigil.

## Build & flash

### Flash from your browser — no toolchain (starter fleet)

The fastest way to try Simulacra:

### **→ [em3ritus.github.io/simulacra](https://em3ritus.github.io/simulacra/)**

Open it in desktop **Chrome or Edge**, plug in a board, click **Connect & Flash** — the
**browser web-flasher** (ESP Web Tools / Web Serial) auto-detects the chip and installs the right
role (C5 → Ward, C6 → Shade, ESP32 → CYD), no ESP-IDF and no command line. It installs the
**baked starter** regime (shared public key), so it's for trying Simulacra out, not a private
deployment. Source and self-host notes: [`web/`](web/).

### Build from source (full / provisioned regime)

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) — v5.5 for the C5 decoy,
v5.4 for the C6 decoy and the Vigil (classic ESP32). With the IDF environment active:

```sh
# Decoy (Ward / Shade) — from the repo root
idf.py set-target esp32c5          # or esp32c6 for Shade
idf.py -DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1 build
idf.py -p <PORT> flash monitor

# Vigil controller — from ./cyd
cd cyd
idf.py set-target esp32
idf.py -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1 build
idf.py -p <PORT> flash monitor
```

Build-time gates (`-D…=1`) select each node's role and optional subsystems (fleet control,
enrollment, self-test). See `docs/` for design specs and per-feature notes.

**Fleet-key regime must match across the whole fleet.** With `-DSIMULACRA_FLEET_PROVISION=1`
(shown above) the Vigil mints a random fleet key at first boot and grants it to decoys through
enrollment — so decoys need the flag too, or they fall back to the baked compile-time key, never
receive the grant, and stay invisible to the controller even while healthy. To run the simpler
baked-key regime instead, omit `-DSIMULACRA_FLEET_PROVISION=1` from **every** node (all then share
the key in `components/simulacra_radar/radar_key.h`). Changing any `-D…` flag needs a clean build
(`rm -rf build sdkconfig`) so the old define doesn't linger.

**Rotate the CONTROL signing key before real use.** `-DSIMULACRA_CONFIG_CTRL=1` (shown above) is a
separate keypair from the fleet-transport key: whoever holds it can sign presets and CLEAR THREATS
commands for every node trusting the matching public key. The committed placeholder bytes
(`cyd/main/sim_ctrl_sk.h.example`) are **public knowledge** — they're in this repo's git history —
so a fleet left on them has no real control-plane authentication. Regenerate before deploying
anything you care about:

```sh
python tools/gen_ctrl_key.py       # rewrites the secret + public key headers
```

then rebuild and reflash **every** board together (decoys bake the new public key, the Vigil the new
secret) — a half-rotated fleet stops verifying.

## Repository layout

```
main/                    decoy firmware (churn, persistent devices, probes, self-learning, detection, ESP-NOW)
cyd/                     Vigil controller firmware (display, touch, SD librarian, fleet authority)
components/simulacra_radar/  shared code (wire formats, learning, signatures, rendering)
components/tweetnacl/     vendored TweetNaCl (Ed25519 / X25519)
tools/pcap_learn/         replay a BLE capture through the real learn/detect pipeline
tools/decoy_audit/        score how separable the BLE decoys are from a real crowd
tools/probe_audit/        verify Wi-Fi probe frames are archetype-faithful and Law-3 safe
tools/radar_audit/        verify the Vigil console's render/control/fleet-status logic on the host
tools/seq_gate/           post-flash check that each fake phone's 802.11 sequence stays independent
web/                      browser web-flasher (ESP Web Tools) — flash a starter fleet with no toolchain
docs/                     design specs, implementation plans, hardware notes, and the roadmap
```

## Offline & bench tools

Simulacra's host tools compile the **real firmware code** (not reimplementations), so behaviour is
verified against the same source that runs on-device:

- **`tools/pcap_learn/`** — replay a BLE capture (`.pcap` or `.pcapng`) through the actual
  self-learning pipeline (validate structure-only learning, emit a seed library) and the tracker
  matcher with dwell/co-travel analysis.
- **`tools/decoy_audit/`** — compile the real BLE generator on the host and score how separable the
  synthetic crowd is from a real capture, as a ranked scorecard plus a single regression-gate number.
- **`tools/probe_audit/`** — byte-exact verification that the Wi-Fi probe frames match real-phone
  archetypes, and that directed-SSID probes only ever name generic **public** networks from a fixed
  compiled-in pool — never one sourced from observed or local traffic.
- **`tools/radar_audit/`** — compiles the Vigil console's own render/control/fleet-status code on the
  host, so every screen (radar, node/threat detail, INFO console, CONTROL presets) and the fleet
  aggregation logic (stale-node pruning, threat dedup, live-vs-pending preset) are verified against
  the exact source that runs on the CYD.
- **`tools/seq_gate/`** — a two-board post-flash gate confirming each fake phone keeps its own
  802.11 sequence counter after an IDF/toolchain bump.

Each tool has its own README with build and run steps.

## Recent updates

Newest first. Forward-looking milestones live in [`docs/ROADMAP.md`](docs/ROADMAP.md).

- **TURBO flood mode.** A sixth console preset for field use, not realism: trigger it fleet-wide
  (two-tap confirm) and every board independently maxes its own BLE + Wi-Fi churn — no room-density
  matching, no persona coupling — burning through as much identifier space as the hardware sustains.
  The idea, and the framing that population-matching optimizes for *not being flagged* while TURBO
  optimizes for *processing cost*, came out of a DEFCON conversation. Ships in main and the public
  web-flasher build. Hardware-verified at both full 3-node-fleet and single-standalone-board (K=1)
  scale with zero radio TX errors.
- **Vigil console, fully fleshed out.** The touch dashboard became a real operator console: tap a
  node card for a **per-node telemetry page**; tap a follower for a **per-threat detail card**
  surfacing the fields the decoys already report (device class, match confidence, vendor company-id,
  epochs, first/last-seen span); a two-page **INFO** system/fleet console with a colour/posture
  **legend**; **live-vs-pending preset** state (each decoy now reports the preset it is *actually*
  running, so the console flags a `MIXED` fleet); and a signed, two-tap **CLEAR THREATS** control
  that wipes the fleet's detection history from the panel.
- **Browser web-flasher — [live](https://em3ritus.github.io/simulacra/).** Flash a starter fleet
  from a web page — ESP Web Tools over Web Serial, auto-detecting the board and installing the right
  role. A CI action builds the three firmwares and deploys the flasher to GitHub Pages on every
  firmware change, so no binaries ever live in git.
- **Protection posture + dashboard cleanup.** The Vigil now leads with one honest word for your
  current state (`CLOAKED` / `EXPOSED` / `HUNTED` / `DARK`), and the data pages were reorganized into
  grouped, aligned sections.
- **Wi-Fi crowd realism.** Probe agents now match the ambient device density (divided across the live
  fleet) and a realistic majority probe **named public networks** from a fixed pool — never an
  observed SSID — closing the "everyone's a wildcard" behavioural tell.
- **Cross-protocol personas (M10 v1).** BLE and Wi-Fi identities are now bound into single,
  co-present synthetic devices that appear and leave together — so a correlator can't isolate your
  real dual-radio phone by filtering out single-radio "ghosts." Persona BLE identities present a
  realistic, Law-3-safe phone shape (terse flags-only / 16-bit service-UUID adverts on rotating
  RPAs), never vendor-matched accessory beacons.
- **Detectability, measured across every axis.** The host audit compiles the real generator *and*
  the real self-learning path, then scores separability from a real capture on address-type mix,
  advertising interval, vendor histogram, AD structure, and presence/lifespan — turning "are the
  decoys convincing?" into a single regression-gate number. Recent passes closed several structural
  tells (AD-structure monoculture, a bogus presence metric) and added a static-infrastructure cohort.
- **Presence & lifespan realism.** Decoys now include persistent static-infrastructure devices and
  per-type address rotation alongside the death/rebirth churn, so the population's come-and-go
  behaviour matches a real crowd instead of a fixed set.
- **Vigil console (CYD).** Live radar/threat dashboard reskinned, plain-language labels, responsive
  touch control, and a per-node fleet roster surfacing TX-health and battery state.
- **Post-flash sequence gate.** A two-board bench check confirms each fake phone keeps an
  independent 802.11 sequence counter after an IDF/toolchain bump.

## Credits

Originally forked from and built on [**0xXyc/splinter**](https://github.com/0xXyc/splinter) — the
project that started the idea. Simulacra extends it into a multi-node, self-learning, fleet-managed
system.

## License

**Currently unlicensed — all rights reserved.** Simulacra is derived from
[0xXyc/splinter](https://github.com/0xXyc/splinter), which is itself unlicensed; those upstream
portions remain under their original author's terms. A permissive license is intended once the
upstream licensing is settled — until then, no redistribution or reuse rights are granted.
