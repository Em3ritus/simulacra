# D2: catch surveillance gear via its Wi-Fi probe requests

**Date:** 2026-07-26
**Status:** design (approved, pre-plan)
**Area:** `main/wifi_observe.c` (the promiscuous RX callback)

## Goal

Extend the shipped Wi-Fi surveillance detector to also match the curated vendor-OUI watchlist against
the **source MAC of probe requests**, not just beacon BSSIDs. Flock cameras (and similar gear) spray
Wi-Fi probe requests "phoning home" using their **real, globally-administered** OUI — which the current
probe path drops. Catching those is a passive, zero-opsec-cost detection vector.

This is item **D2** of the FlockDecoy-inspired improvement list (D1 BLE `TN`-serial deferred pending a
passive-vs-active scan decision; D3 `test_flck` SSID and F1 flood realism to follow).

## Why probe requests

- Flock ALPR cameras periodically emit Wi-Fi **probe requests** trying to reach their backhaul
  (verified: `0xXyc/flock-you-wifi-recon` detects Flock exactly this way).
- Probe requests are **received passively** in promiscuous mode — we transmit nothing, so unlike the
  BLE active-scan problem (D1), this vector costs us no opsec. This is why D2 is sequenced before D1.
- A camera's probe carries a **real vendor OUI** (globally-administered). Today `wifi_observe.c`'s probe
  path keeps only **randomized** (locally-administered) sources for the real-phone density estimate and
  drops real-OUI ones (`if (!(sa[0] & 0x02)) return;`) — i.e. it drops the very frames a camera emits.

## Change

Reuse the existing surveillance pipeline entirely. In `rx_cb`, in the probe-request branch
(`f[0] == 0x40`), add a surveil-OUI check on the source MAC **before** the randomized-only density
filter:

```c
    if (f[0] != 0x40) return;                 // probe request
    const uint8_t *sa = f + 10;               // source MAC
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // A probe request from a surveillance-vendor OUI (a real, globally-administered MAC) is a camera
    // phoning home. Check BEFORE the randomized-only density filter, which would drop a real-OUI source.
    uint8_t pcls, pcat;
    if (surveil_oui_match(sa, &pcls, &pcat)) {
        if (!fleet_mac_excluded(sa, now)) surveil_note(surveil_hash(sa), p->rx_ctrl.rssi, pcls, pcat);
        return;
    }
    if (!(sa[0] & 0x02)) return;              // randomized only = real-phone proxy (density path unchanged)
    if (fleet_mac_excluded(sa, now)) return;
    wifi_obs_note(sa, now);
```

(The single change vs today is the moved-up `now` computation and the inserted surveil-OUI block; the
density path below it is byte-for-byte unchanged.)

A probe-request surveil hit flows through the identical downstream as a beacon hit: `surveil_note` →
`coexist_task` drains the ring → `detect_note_known(hash, rssi, class, category, 85, epoch)` → threat
table → wire → CYD SURVEILLANCE section. No new module, no wire change, no `detect.c` change.

## Properties preserved

- **Law 1 (hash-and-drop):** the OUI is matched on the raw MAC, then the MAC is hashed via
  `surveil_hash`; the full MAC never crosses the RX→coexist thread boundary or gets stored.
- **No false positives:** the watchlist holds only vendor-*owned* IEEE blocks (`B4:1E:52` Flock →
  `CAMERA`, `00:25:DF` Axon → `BODYCAM`). No phone uses those OUIs, so real phones never trip it.
- **Density estimate unaffected:** a real-OUI camera probe was already excluded from the phone-density
  count (it fails the randomized-only test); the surveil check returns early and never reaches
  `wifi_obs_note`, so the count is unchanged.
- **Own fleet excluded:** `fleet_mac_excluded` still guards (our fleet uses randomized MACs and would
  never match a vendor OUI anyway, but the guard stays for safety).

## Testing

- **Host:** no new test — `surveil_oui_match` is already covered (`tools/probe_audit/tests/test_surveil.py`:
  Flock→CAMERA, Axon→BODYCAM, Espressif/Liteon/random reject). D2 only calls the already-tested matcher
  from a second frame type; the probe branch itself is `esp_wifi` RX glue and is not host-compilable
  (same as the beacon branch).
- **Firmware:** compile-verify c5 + c6 (`wifi_observe.c` is decoy-only; not in the CYD build).
- **On-air:** a real-world true positive needs a Wi-Fi capture near actual Flock/Axon hardware —
  deferred (no hardware access this session, no real positive sample yet).

## Out of scope

- D1 (BLE `TN`-serial match) — deferred; it needs a passive-vs-active scan decision.
- D3 (`test_flck` SSID matching) — needs an SSID-parse path; separate item.
- Any change to the density estimator, the beacon path, `detect.c`, or the wire.
- Adding more OUIs to the watchlist (the matcher/table is unchanged here).
