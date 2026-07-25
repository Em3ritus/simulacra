# Flock/Raven BLE surveillance detection

**Date:** 2026-07-25
**Status:** design (approved, pre-plan)
**Area:** tracker/surveillance detection (`components/simulacra_radar/sig_seed.c`, `threat_sig.h`,
`sig_class_name.h`; CYD render `radar_render.c`; host `tools/pcap_learn`)

## Goal

Detect Flock Safety / Raven surveillance devices over BLE and surface them on the CYD as
*surveillance presence* — the counter-surveillance capability that projects like flockyou / DeFlock /
flockback provide. This is sub-project **A (BLE)**; the Wi-Fi OUI/SSID detection path is a separate
future spec (**B**).

## Ground truth (researched 2026-07-25)

- The whole flockyou ecosystem matches Flock **and** Raven gear over BLE on **manufacturer company
  ID `0x09C8`** (2504, registered to "XUNTONG", the BLE module vendor Flock uses). Raven additionally
  advertises custom service UUIDs, but since `0x09C8` already catches both, the UUIDs are redundant
  for detection (and the current matcher only supports 16-bit service UUIDs, not Raven's 128-bit ones).
- Sources: colonelpanichacks/flock-you, virtuallyscott.github.io/flock-you, Marslauncher/flock-you-scanner.
- The Wi-Fi side (31 Flock-infra MAC OUI prefixes + SSID patterns) is **out of scope** here.

**Honest signature caveats (documented, not hidden):**
- `0x09C8` is the *module vendor*, not Flock-specific, so the match could in principle fire on another
  product using the same BLE module. This is the accepted ceiling of a company-ID signature and is
  exactly what the flockyou ecosystem uses; confidence is set moderate (60) to reflect it.
- The match cannot distinguish a Flock *camera* from a Raven *gunshot detector* (shared company ID and
  no confirmed distinguishing sub-pattern). One class, labeled `Flock`, covers the ecosystem.
- **No real positive sample exists yet.** The most recent BLE drive capture had zero `0x09C8`
  devices. Validation uses a synthetic advert (below); a true-positive confirmation needs a future
  capture near a known Flock location (deflock.me maps them).

## How it fits the existing framework

The detector already does exactly this kind of matching:

- `sig_match.c` `one_match()` filters by `company_id` before any byte pattern — so a company-ID-only
  signature (`pat_len=0`) is fully supported.
- `detect.c` `detect_note_known()` records a known-class hit into the threat table **on the first
  sighting** (returns `DETECT_CONFIRM` immediately — no multi-epoch persistence, unlike behavioral
  followers). A single drive-by Flock advert therefore lands in the threat table at once.
- `radar_wire_status_t.threats[]` already carries per-threat `category`, `class_id`, and `best_rssi`.

**Consequence:** adding the signature lights up the entire decoy → ESP-NOW status → CYD pipeline with
**no changes to `detect.c`, `coexist.c`, or the wire format**. The only new work beyond the signature
is the CYD *render* treatment.

## Components

### 1. Signature (decoy) — `components/simulacra_radar/`

- `threat_sig.h`: add `SIG_CLASS_FLOCK` to `sig_class_t` (before `SIG_CLASS_COUNT`). The
  `SIG_CAT_CAMERA` category already exists — reused, no new category.
- `sig_class_name.h`: add `case SIG_CLASS_FLOCK: return "Flock";`.
- `sig_seed.c`: add one `threat_sig_t` entry and bump `SIG_SEED_VERSION` to 2:
  ```c
  // Flock Safety / Raven surveillance gear: BLE mfg company id 0x09C8 (XUNTONG module vendor).
  // Company-id-only match (pat_len=0); moderate confidence — see design caveats.
  { .sig_id=4, .category=SIG_CAT_CAMERA, .class_id=SIG_CLASS_FLOCK,
    .company_id=0x09C8, .svc_uuid16=0x0000, .addr_type_mask=0,
    .match_src=SIG_SRC_MFG_DATA, .pat_off=0, .pat_len=0,
    .pattern={0}, .mask={0}, .confidence=60 },
  ```

### 2. Host validation — `tools/pcap_learn/`

- The `sig_scan.c` harness already reports hits by class via `sig_class_name`, so it will label Flock
  hits automatically once the seed entry exists. Add a Flock line to its per-class summary if the
  summary is class-enumerated (otherwise no change).
- New host test (`tools/pcap_learn/tests/`): a **synthetic** advert carrying mfg company `0x09C8`
  asserts `sig_match` fires with `class_id=SIG_CLASS_FLOCK`, `category=SIG_CAT_CAMERA`; a neighboring
  company ID (e.g. `0x09C7`) asserts it does **not** fire (selectivity). Re-running the real drive
  capture is expected to still report 0 Flock hits.

### 3. CYD presence readout (the "(b)" render) — `radar_render.c`

Pure render, driven off the existing `threats[]` (`category==SIG_CAT_CAMERA`); no wire/decoy change.

- **FOLLOWERS view (`draw_detail`):** split camera-category rows out of the follower list into their
  own **"SURVEILLANCE" section** (own sub-header + `Flock` label + proximity `best_rssi`), rendered
  distinctly from behavioral followers. The follower "seen / flagged" summary counts **only**
  non-camera threats, so a camera is never described as a follower.
- **HOME view (`draw_home`):** when ≥1 camera-category threat is present, draw a small surveillance
  indicator near the STATUS headline — `SURVEIL xN` (N = count of camera-category threats), in a
  distinct color. Absent when N=0.
- **Posture unchanged:** `radar_posture` must **not** be made to flip to HUNTED on a camera. HUNTED
  means "something is tracking you"; surveillance infrastructure gets the separate SURVEIL indicator.
  A camera is a single-sighting `NEW` escalation, so the existing posture logic already ignores it —
  no change required, and none should be made.

## Data flow

```
BLE advert (mfg company 0x09C8)
  -> observe.c parses adv fields
  -> coexist_on_report(): sig_match() -> sig_hit_t{class=FLOCK, cat=CAMERA}
  -> detect_note_known(hash, rssi, FLOCK, CAMERA, 60, epoch)  [CONFIRM on first sighting]
  -> threat table row {kind=KNOWN, class_id=FLOCK, category=CAMERA, best_rssi}
  -> serialized into radar_wire_status_t.threats[] over ESP-NOW
  -> CYD: draw_detail SURVEILLANCE section + draw_home SURVEIL xN indicator
```

## Testing

- **Host — `pcap_learn`:** synthetic `0x09C8` match + selectivity (new test); real drive capture still 0.
- **Host — `radar_audit` render harness:** a synthetic status with a `category==SIG_CAT_CAMERA` threat
  renders the SURVEILLANCE section + `Flock` label in the FOLLOWERS view and the `SURVEIL xN` indicator
  on HOME; a follower-only status shows neither and the follower count is unaffected; a mixed status
  keeps cameras out of the follower count.
- **Firmware:** compile-verify c6 + c5 (shared `simulacra_radar` component) and the CYD build.

## Out of scope (v1)

- Wi-Fi Flock-infra OUI/SSID detection (sub-project B — separate spec).
- GPS mapping / deflock.me upload (no GPS on the decoy).
- Raven-vs-Flock discrimination (no confirmed distinguishing sub-pattern on the shared company ID).
- Any change to `detect.c`, `coexist.c`, or the wire format (the existing path already carries
  everything needed).
