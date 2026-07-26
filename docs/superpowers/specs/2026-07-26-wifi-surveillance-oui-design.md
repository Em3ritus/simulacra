# Wi-Fi surveillance-infrastructure detection (curated vendor OUIs)

**Date:** 2026-07-26
**Status:** design (approved, pre-plan)
**Area:** `main/surveil_oui.{c,h}` (new), `main/wifi_observe.c`, `main/coexist.c`,
`components/simulacra_radar/threat_sig.h` + `sig_class_name.h` + `radar_render.c`.

## Goal

Detect surveillance infrastructure over Wi-Fi by matching observed AP beacon BSSIDs against a **curated
watchlist of verified, vendor-owned IEEE OUI blocks**, and surface hits on the CYD as *surveillance
presence* — reusing the entire detect → wire → CYD pipeline the BLE Flock detector already established.
This is the Wi-Fi counterpart (sub-project B) to the shipped BLE `SIG_CLASS_FLOCK` detector, and a
public/official feature (ships in release), unlike the personal Flock-flood mode.

## Why curated vendor OUIs (the honesty decision)

The flock-you "31 Flock OUI prefixes" list is **not** a list of Flock-owned blocks — ~26 of the 31 are
**component-vendor** OUIs (Espressif `a4:cf:12`, Liteon `70:c9:4e`/`9c:2f:9d`, Silicon Labs, Raspberry
Pi) that ship in thousands of unrelated products. flock-you tolerates that noise by cross-referencing
the **WiGLE** crowd database; an embedded device has no such database, so matching those OUIs would
flag every ESP32/Liteon/RPi AP nearby **and self-flag our own fleet**. That is noise, not detection.

Therefore the watchlist contains **only OUI blocks a surveillance vendor owns outright** (verified
against the IEEE registry), so every hit is trustworthy and false-positive-free. Seed entries:

| OUI | Vendor (verified) | class_id | category |
|-----|-------------------|----------|----------|
| `B4:1E:52` | Flock Safety (ALPR cameras) | `SIG_CLASS_FLOCK` | `SIG_CAT_CAMERA` |
| `00:25:DF` | Axon Enterprise / Taser (bodycam + evidence infra) | `SIG_CLASS_AXON` | `SIG_CAT_BODYCAM` |

Confidence is high (85) — a vendor's own registered block is a stronger signal than the BLE `0x09C8`
match (a shared module vendor). The list is one small file, trivially extended as more vendor-owned
blocks are verified.

**Honest caveats (documented, not hidden):**
- Catches vendor-registered **first-party** hardware only. Module-based surveillance gear that uses a
  generic component OUI is **not** detectable here without a crowd database — deliberately out of scope
  (matching those would be noise). Narrow but zero-false-positive.
- Detection is **channel-opportunistic**: promiscuous RX only hears the current channel, and the decoy
  hops (1/6/11 + 5 GHz) for probe bursts. A camera beacons on its own AP channel, so it is caught only
  when the decoy's channel aligns — reliable over a channel-sampling drive-by, not instantaneous.
- No real positive capture exists yet; validation is synthetic (a crafted beacon). A true positive
  needs a capture near known Flock/Axon hardware.

## Architecture

The BLE Flock work already built the downstream: `detect_note_known(hash, rssi, class, category,
confidence, epoch)` lands a hit in the threat table → `radar_wire_status_t.threats[]` (which carries
`category`/`class_id`/`best_rssi`) → the CYD SURVEILLANCE render. So this feature is a **Wi-Fi-observe →
detect bridge** plus the OUI matcher; no changes to `detect.c` or the wire.

### 1. Curated matcher — `main/surveil_oui.{c,h}` (new)

- A `static const` table of `{ uint8_t oui[3]; uint8_t class_id; uint8_t category; }`, seeded with the
  two entries above.
- `bool surveil_oui_match(const uint8_t mac[6], uint8_t *class_id, uint8_t *category);` — pure: returns
  true and fills class/category if `mac[0..2]` matches a table OUI. Host-testable, no radio/timer deps.
- A small pending-hit ring for the observe→coexist thread bridge:
  - `void surveil_note(uint32_t hash, int8_t rssi, uint8_t class_id, uint8_t category);` — called from
    the Wi-Fi RX callback (single producer).
  - `bool surveil_next(uint32_t *hash, int8_t *rssi, uint8_t *class_id, uint8_t *category);` — drained
    by `coexist_task` (single consumer); returns false when empty.
  - `void surveil_init(uint32_t salt);` — seeds the per-session hash salt (set once from coexist).
- Hashing (`salted FNV-1a over the 6-byte MAC`) happens **inside `surveil_note`'s caller in the RX
  path**, or a helper `uint32_t surveil_hash(const uint8_t mac[6])` here — the full MAC is dropped
  immediately after hashing (Law 1). Only the hash crosses the thread boundary and reaches `detect`.

### 2. Signature taxonomy

- `components/simulacra_radar/threat_sig.h`: add `SIG_CLASS_AXON` to `sig_class_t` (before
  `SIG_CLASS_COUNT`). `SIG_CAT_CAMERA` and `SIG_CAT_BODYCAM` already exist.
- `components/simulacra_radar/sig_class_name.h`: add `case SIG_CLASS_AXON: return "Axon";`.
- `tools/pcap_learn/sig_scan.c`: extend `CLASS_NAME[]` to include `"Axon"` (keeps the class-indexed
  array in bounds, mirroring the earlier Flock addition).

### 3. Wi-Fi observe bridge — `main/wifi_observe.c`

Add a **beacon** branch to `rx_cb`, before the existing probe-request/density path:

```
if (f[0] == 0x80) {                         // beacon
    const uint8_t *bssid = f + 10;          // addr2 = the AP's MAC
    if (fleet_mac_excluded(bssid, now)) return;      // never flag our own fleet
    uint8_t cls, cat;
    if (surveil_oui_match(bssid, &cls, &cat)) {
        uint32_t h = surveil_hash(bssid);   // Law 1: hash, then the MAC is dropped
        surveil_note(h, p->rx_ctrl.rssi, cls, cat);
    }
    return;
}
```

The existing probe-request density path (`f[0]==0x40`, randomized-source) is unchanged.

### 4. coexist bridge — `main/coexist.c`

`surveil_init(esp_random())` at detector setup; then each `coexist_task` tick, drain:

```
uint32_t h; int8_t rssi; uint8_t cls, cat;
while (surveil_next(&h, &rssi, &cls, &cat))
    detect_note_known(h, rssi, cls, cat, SURVEIL_CONF, s_epoch);   // SURVEIL_CONF = 85
```

Runs on the coexist thread (single detect writer), matching the BLE path's discipline.

### 5. CYD render — `components/simulacra_radar/radar_render.c`

Today `draw_detail`'s SURVEILLANCE split and `draw_home`'s `!N` indicator test
`category == SIG_CAT_CAMERA`. Generalize both to **`category == SIG_CAT_CAMERA || category ==
SIG_CAT_BODYCAM`** so Axon (BODYCAM) hits also surface as surveillance, labeled via `sig_class_name`
(`Flock` / `Axon`). No wire change.

## Testing

- **Host — `surveil_oui_match` (new pure test):** `B4:1E:52` → `FLOCK`/`CAMERA`; `00:25:DF` →
  `AXON`/`BODYCAM`; Espressif `a4:cf:12`, Liteon `70:c9:4e`, and a random OUI → **no match**
  (selectivity — the noise OUIs must not fire). Compiled via a small host harness.
- **Host — CYD render (`radar_audit`):** extend the surveillance-render test so a `BODYCAM`-category
  threat ALSO renders the SURVEILLANCE section + the `Axon` label + the HOME `!N` indicator; a
  follower-only status still shows neither.
- **Firmware:** compile-verify c5/c6/cyd.

## Out of scope

- SSID matching (surveillance APs are frequently hidden-SSID; OUI is the reliable signal).
- Module-OUI / generic-vendor matching (noise without a crowd database).
- Any crowd-DB (WiGLE-style) cross-reference or GPS mapping.
- Changes to `detect.c`, the wire format, or the normal decoy behavior.
