# Changelog

Newest first. Forward-looking milestones live in [`docs/ROADMAP.md`](docs/ROADMAP.md). The README's
own "Recent updates" section keeps only the latest few entries - this is the full history.

- **The web flasher can finally make a fleet nobody else can read.** It could not before, because
  anything compiled into a published binary is public: the images are downloadable, so a key baked
  into one is a key everybody has, and regenerating it per release just ships a new secret in the
  new download. The previous answer was to compile no control plane at all, which removed the
  forgeable command path but left the mesh encrypted under a published transport key. That is
  encryption anybody can undo. The keypair is now generated **in the browser** and written into the
  images before flashing, and everything downstream follows from it: the Vigil mints a random
  ESP-NOW transport key on first boot and hands it to decoys over the authenticated enrollment, so
  that key exists in no binary either. CI builds the provisioned regime with a placeholder no board
  ever runs. Fixing it surfaced a third way to brick a patched image, on top of the segment checksum
  and appended SHA256 already known: the published images are **merged** (padding, bootloader,
  partition table, app), so the app does not start at byte 0, and walking segments from there parses
  the bootloader and hashes the wrong range - an image that flashes perfectly and never boots. Both
  patchers now locate the app through the partition table, and consult it *before* the byte-0
  shortcut, because the bootloader shares the app's 0xE9 magic and sits at offset 0 on C6 and H2.
  Verified on hardware: a browser-keyed merged image boots, loads its app from 0x10000, and reads
  back with the generated key at the expected offset.
- **Enrollment is a button, not a hidden gesture.** Pairing hung off a 1.5 s press-and-hold anywhere
  on the screen, which was invisible, fired from any view, and did one of three different things
  depending on state the operator could not see. It is now a labelled button at the bottom of
  CONTROL whose text *is* the state: `PAIR NEW NODE`, `PAIRING 24s`, `ROTATE FLEET KEY?`, or
  `ACCEPT NODE` with the joining board's full fingerprint. Rotation re-keys every node and drops
  anything that does not re-enroll, so it takes the same two-tap confirm as CLEAR THREATS rather
  than hiding behind a gesture, and the fingerprint is shown whole because the accept is TOFU and a
  truncated one would weaken the only check it rests on.
- **No persistent identifiers, anywhere.** A slice of the crowd used to hold one static address for
  4-12 h, added so the fleet would reproduce the long presence tail real environments have. That
  inverted the point: a decoy holding one address for hours, on a board carried by the operator, is
  a *better* tracking handle than the phone it covers, since phones rotate their RPA every ~15 min.
  The hole was wider than that band - STATIC addresses never rotate, so a static device's address
  was on air for its entire life, and static is 75% of the crowd. `ADDR_MAX_ONAIR_MS` (15 min) is
  now a hard ceiling across **both radios**, honoured by static devices dying and being reborn as
  wholly new devices rather than by rotating. Verified over a 6 h simulated run: 1252 completed
  spans, max 15.0 min, none over. The cost is accepted knowingly - the fleet no longer reproduces
  that presence tail, and `presence_duration` is now the worst audit axis. Wi-Fi saved-network sets
  are redrawn on every MAC rotation under the same rule, reversing a deliberate earlier decision to
  carry them across (a real phone's saved networks belong to the device, not the MAC): a set that
  outlives a rotation is a persistent identifier, and the one most commonly used to defeat MAC
  randomisation in the field. Accepted residual: aggregate SSID-to-MAC multiplicity drifts toward
  one MAC per name, which the 38-entry pool blunts but does not erase.
- **AD structure is learned, not hardcoded.** The last generation axis the model could not express.
  `pick_no_mfg_template()` had been fitted to a single 2026-07-05 capture in which flags-only
  advertisers were 52.7% of devices; the same share measures 6.7% and 0.0% elsewhere. Structure now
  samples `rf_model` exactly as intervals and vendors already do, via two histograms (no-mfg shape,
  and mfg-bearing shape). Cross-validated `ad_structure` spread **[0.153-0.925] -> [0.088-0.381]**;
  worst-case headline **0.925 -> 0.448**.
- **Never emit a tracker signature.** Decoys could match this project's own detector three ways: the
  `tile` template advertised service UUID 0xFEED verbatim; the Apple manufacturer path could roll
  Find My's 0x12 subtype by chance (3 in 768); and the learn loop could adopt a real AirTag or Tile
  in the room and clone it. Nearby phones running tracker detection would have told their owners an
  unknown tracker was travelling with them - the exact harm this project exists to oppose. All three
  closed, with a fail-closed Law-3 gate on template output that immediately caught a fourth (a
  Microsoft template, forbidden as Swift Pair). 0xFD6F (COVID Exposure Notification) and 0xFD5A
  (SmartTag) removed from the service-UUID pool.
- **Wi-Fi probe archetypes rebuilt from capture.** The shipped IE layouts were modelled from
  documentation, and a census of 877 real probing devices found **none of the eight present even
  once** - every probe the fleet emitted carried a structure existing nowhere in ambient, which
  classifies the fleet regardless of how well the source MAC is randomised. Replaced with real
  captured structures (8 of 8 now match, covering 52.2% of that crowd), including two 2.4-GHz-only
  archetypes, because devices without a 5 GHz radio exist and an invented table has none.
- **ESP-NOW link went quiet.** Measured from outside, the fleet emitted ~99 vendor action frames/min
  where 203 of 206 ambient devices emit exactly zero. The Vigil, not the decoys, was the loudest
  thing in the system: it rebroadcast its entire learned library every 20 s on a fixed period. Now a
  delta sync on a jittered cadence, with `FLEET_MACS` also delta-based and its chunks paced.
  Measured **66.9/min -> 4.2/min** total; library sync **65.9 -> 2.4**.
- **Replay-driven presence oracle closed.** Capturing a sealed frame needs no key, and the replay
  window reset on any salt change - so alternating captures from two sender boots made every decoy
  in range answer with a STATUS. Telemetry replay state is now a high-water counter per salt, so an
  additional or rebooted *sender* resumes cleanly where a global monotonic floor would have refused
  it. This covers the telemetry path only: CONTROL commands still gate on a single salt-independent
  monotonic floor per decoy, and that floor is what prevents running two Vigils today - each spends
  its own counter block, so they would silently fight over which is ahead.
- **Named-probe rate matched to measurement.** 21% of real devices ever name a network and those
  that do name almost every time, looking for exactly one; the fleet had 62% of personas naming 60%
  of the time with up to three saved networks each - the middle of a distribution that is actually
  bimodal. SSID pool widened 22 -> 38 entries, from general knowledge of router defaults rather than
  from captures: a census found 145 distinct probed SSIDs of which exactly one was probed by 8+
  independent devices, and that one was a local business. Real probed SSIDs are people's networks.
- **Additive fleet population + AUTO/MANUAL modes.** Decoys used to run `1/K` of one fleet-wide
  crowd, so adding a board redistributed cover instead of adding it. That division was introduced to
  fix a real measured problem (three boards once put 88 synthetic devices into a room holding 4-9
  real ones), but it was justified partly on spatial diversity the boards never get in practice -
  they travel together in one bag. Each board now sizes its own crowd from the ambient density it
  measures, and boards add up. The preset ladder is replaced by two orthogonal modes: **AUTO**
  tracks the room and honours an operator-set cap; **MANUAL** LOW/MED/HIGH pins a fixed fraction of
  each board's capacity. This also retires the STEALTH/NORMAL pair, which resolved to identical
  settings on the C5 and could not be told apart on the console. CONFIG wire goes to v2 (the preset
  ordinals changed meaning, so a mixed-firmware fleet now fails loudly instead of silently applying
  the wrong preset) - **flash every board together.** Hardware-verified on the 3-node fleet.
- **Project wiki.** A full [CYD console guide + reference](https://github.com/Em3ritus/simulacra/wiki)
  (every screen, setting, preset, and status word explained) and a project-wide glossary, published
  and linked from the README.
- **TURBO flood mode.** A sixth console preset for field use, not realism: trigger it fleet-wide
  (two-tap confirm) and every board independently maxes its own BLE + Wi-Fi churn - no room-density
  matching, no persona coupling - burning through as much identifier space as the hardware sustains.
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
- **Browser web-flasher - [live](https://em3ritus.github.io/simulacra/).** Flash a starter fleet
  from a web page - ESP Web Tools over Web Serial, auto-detecting the board and installing the right
  role. A CI action builds the three firmwares and deploys the flasher to GitHub Pages on every
  firmware change, so no binaries ever live in git.
- **Protection posture + dashboard cleanup.** The Vigil now leads with one honest word for your
  current state (`CLOAKED` / `EXPOSED` / `HUNTED` / `DARK`), and the data pages were reorganized into
  grouped, aligned sections.
- **Wi-Fi crowd realism.** Probe agents now match the ambient device density (divided across the live
  fleet) and a realistic majority probe **named public networks** from a fixed pool - never an
  observed SSID - closing the "everyone's a wildcard" behavioural tell.
  *(Both halves later revised: the `1/K` division was retired for additive population, and the
  named-probe share dropped from a majority to a measured ~21% minority. See the entries above.)*
- **Cross-protocol personas (M10 v1).** BLE and Wi-Fi identities are now bound into single,
  co-present synthetic devices that appear and leave together - so a correlator can't isolate your
  real dual-radio phone by filtering out single-radio "ghosts." Persona BLE identities present a
  realistic, Law-3-safe phone shape (terse flags-only / 16-bit service-UUID adverts on rotating
  RPAs), never vendor-matched accessory beacons.
- **Detectability, measured across every axis.** The host audit compiles the real generator *and*
  the real self-learning path, then scores separability from a real capture on address-type mix,
  advertising interval, vendor histogram, AD structure, and presence/lifespan - turning "are the
  decoys convincing?" into a single regression-gate number. Recent passes closed several structural
  tells (AD-structure monoculture, a bogus presence metric) and added a static-infrastructure cohort.
- **Presence & lifespan realism.** Decoys now include persistent static-infrastructure devices and
  per-type address rotation alongside the death/rebirth churn, so the population's come-and-go
  behaviour matches a real crowd instead of a fixed set.
  *(Later reversed: the persistent cohort was removed entirely under the 15-minute ceiling. It made
  a decoy a better tracking handle than the phone it covered. See "No persistent identifiers".)*
- **Vigil console (CYD).** Live radar/threat dashboard reskinned, plain-language labels, responsive
  touch control, and a per-node fleet roster surfacing TX-health and battery state.
- **Post-flash sequence gate.** A two-board bench check confirms each fake phone keeps an
  independent 802.11 sequence counter after an IDF/toolchain bump.
