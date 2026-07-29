# D3: detect Flock via the `test_flck` probe-request SSID

**Date:** 2026-07-26
**Status:** design (approved, pre-plan)
**Area:** `main/surveil_oui.{c,h}` (new SSID matcher), `main/wifi_observe.c` (probe SSID parse)

## Goal

Detect Flock cameras by the distinctive Wi-Fi network name **`test_flck`** they probe for. Flock
Falcon/Sparrow production firmware ships a saved dev network `test_flck` (CVE-2025-59409); when a unit's
Wi-Fi interface is up it emits **directed probe requests** carrying `test_flck` in the SSID element and
auto-connects. Matching that SSID is a passively-observable, near-zero-false-positive Flock signal.

Item **D3** of the FlockDecoy-inspired list (D2 probe-request OUI vector shipped; D1 BLE `TN` deferred;
F1 flood realism to follow).

## Verified observability (the make-or-break check)

Per the GainSec Falcon/Sparrow writeup, `test_flck` is a **saved network the camera probes for and
auto-connects to** — NOT one it broadcasts. So it is observable **passively as a directed probe
request** (SSID element in a probe request), which is exactly the frame type `wifi_observe` already
receives in promiscuous mode. No opsec cost, no active scanning.

**Honest caveats (documented, not hidden):**
- Only fires when a Flock unit has its Wi-Fi interface up and is actively probing for the saved network
  → **intermittent / low-prevalence**, and per the CVE not every unit has the saved network. It is a
  **high-confidence, low-frequency** signal: when a `test_flck` probe appears it is near-certainly Flock
  (no ordinary device probes for that string), but it may rarely appear.
- Channel-opportunistic like all our passive Wi-Fi detection (only heard on the current channel).
- No real positive capture yet; validation is synthetic.

## Architecture

Reuse the surveillance pipeline; add one new matcher and one parse site.

### 1. SSID matcher — `main/surveil_oui.{c,h}`

Add a small SSID watchlist next to the OUI table, plus:

```c
// Match an SSID (exact, case-sensitive) against the surveillance-SSID watchlist. Returns true (and
// fills class_id/category) on a hit; false otherwise. Pure. `ssid` is NOT NUL-terminated; `len` is the
// SSID element length.
bool surveil_ssid_match(const uint8_t *ssid, uint8_t len, uint8_t *class_id, uint8_t *category);
```

Seed: `{ "test_flck" (len 9) -> SIG_CLASS_FLOCK / SIG_CAT_CAMERA }`. The module keeps its name
(`surveil_oui`) for continuity — it is the "surveillance signature" unit; a rename is out of scope.

### 2. Probe-request SSID parse — `main/wifi_observe.c`

In `rx_cb`'s probe-request branch, **after** the existing D2 source-OUI check and **before** the
randomized-only density filter, parse the SSID element and check it:

```c
    // D3: a directed probe for a known surveillance SSID (e.g. Flock's saved dev network "test_flck")
    // is a camera phoning home; the source MAC may be randomized (so the OUI check above misses it).
    if (p->rx_ctrl.sig_len >= 26 && f[24] == 0x00) {          // first IE is the SSID element (id 0)
        uint8_t slen = f[25];
        if (slen > 0 && p->rx_ctrl.sig_len >= (uint32_t)(26 + slen)) {
            uint8_t scls, scat;
            if (surveil_ssid_match(f + 26, slen, &scls, &scat)) {
                if (!fleet_mac_excluded(sa, now))
                    surveil_note(surveil_hash(sa), p->rx_ctrl.rssi, scls, scat);  // Law 1: hash, MAC dropped
                return;
            }
        }
    }
```

The device identity remains its **source MAC** (`sa`, hashed via `surveil_hash`); the SSID is only the
detection trigger. A hit flows through the identical downstream (`surveil_note` → coexist drain →
`detect_note_known(…, 85, …)` → threat table → wire → CYD SURVEILLANCE, labeled `Flock`).

### Ordering in the probe branch

1. D2: source-OUI match (real-OUI camera) → note + return.
2. D3: SSID match (camera probing `test_flck`, possibly with a randomized MAC) → note + return.
3. Density path (randomized-only phone proxy) — unchanged.

## Properties preserved

- **Law 1:** SSID compared in place; the source MAC is hashed, never stored or forwarded.
- **No false positives:** `test_flck` is a literal Flock dev string; no ordinary device probes for it.
- **Density unaffected:** an SSID hit returns before `wifi_obs_note`; a normal named probe (any other
  SSID) falls through to the unchanged density path.
- **Own fleet excluded:** `fleet_mac_excluded` guard retained (our probe agents never emit `test_flck`).

## Testing

- **Host — `surveil_ssid_match` (new):** `test_flck` → `(FLOCK, CAMERA)`; a different SSID (e.g.
  `attwifi`) and an empty SSID → no match. Via a `probe_dump --surveilssid <ascii>` harness mode +
  tests in `tools/probe_audit/tests/test_surveil.py`, mirroring the `--surveiloui` pattern.
- **Firmware:** compile-verify c5 + c6 (`wifi_observe.c` is decoy-only).
- **On-air:** a real positive needs a capture near a Flock unit actively probing — deferred.

## Out of scope

- Beacon SSID matching (Flock does not broadcast `test_flck`).
- Substring/wildcard SSID matching (exact match only; YAGNI).
- Additional surveillance SSIDs beyond `test_flck` (extensible later; the table is one entry now).
- D1 (BLE `TN`) and F1 (flood realism); any change to `detect.c`, the wire, or the density estimator.
