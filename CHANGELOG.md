# Changelog

Newest first. Forward-looking milestones live in [`docs/ROADMAP.md`](docs/ROADMAP.md). The README's
own "Recent updates" section keeps only the latest few entries — this is the full history.

- **Project wiki.** A full [CYD console guide + reference](https://github.com/Em3ritus/simulacra/wiki)
  (every screen, setting, preset, and status word explained) and a project-wide glossary, published
  and linked from the README.
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
