# D2: Surveillance Probe-Request Vector Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Match the curated surveillance-vendor OUI watchlist against Wi-Fi probe-request source MACs (not just beacon BSSIDs), so a Flock/Axon device spraying real-OUI probes phoning home is detected — passively, at zero opsec cost.

**Architecture:** One added block in `wifi_observe.c`'s promiscuous `rx_cb`: in the probe-request branch, check the source MAC against `surveil_oui_match` before the randomized-only density filter (which drops real-OUI sources). A hit reuses the whole existing surveillance pipeline (`surveil_note` → coexist drain → `detect_note_known` → CYD).

**Tech Stack:** C (ESP-IDF firmware), compile-verified via the build-flash-read skill.

## Global Constraints

- The surveil check must come **before** the `if (!(sa[0] & 0x02)) return;` randomized-only density filter (real-OUI camera probes fail that filter).
- Reuse the existing `surveil_oui_match` / `surveil_hash` / `surveil_note` (no new module, no wire change, no `detect.c` change). The watchlist (Flock `B4:1E:52`, Axon `00:25:DF`) is unchanged.
- Law 1: OUI matched on the raw MAC, then hashed via `surveil_hash`; the MAC is never stored or passed on.
- The density path below the new block must stay byte-for-byte unchanged.
- Commit identity is the repo-local `Em3ritus` noreply. Every commit carries the trailers:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.

---

### Task 1: Add the probe-request surveil-OUI check + compile-verify

**Files:**
- Modify: `main/wifi_observe.c` (the probe-request tail of `rx_cb`)

**Interfaces:**
- Consumes (already present, `surveil_oui.h` is already included in `wifi_observe.c`):
  `bool surveil_oui_match(const uint8_t mac[6], uint8_t *class_id, uint8_t *category);`
  `uint32_t surveil_hash(const uint8_t mac[6]);`
  `void surveil_note(uint32_t hash, int8_t rssi, uint8_t class_id, uint8_t category);`
  and `fleet_mac_excluded`.

- [ ] **Step 1: Insert the probe-request surveil check**

In `main/wifi_observe.c`, the probe-request tail of `rx_cb` currently reads:

```c
    if (f[0] != 0x40) return;                 // frame control: probe request
    const uint8_t *sa = f + 10;               // source MAC
    if (!(sa[0] & 0x02)) return;              // randomized (locally-administered) only = real-phone proxy
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (fleet_mac_excluded(sa, now)) return;  // skip fleetmate decoys (our own are never received)
    wifi_obs_note(sa, now);                   // raw MAC hashed-and-dropped inside
```

Replace that block with (moves `now` up; inserts the surveil check before the randomized-only filter;
density path below is otherwise unchanged):

```c
    if (f[0] != 0x40) return;                 // frame control: probe request
    const uint8_t *sa = f + 10;               // source MAC
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // A probe request from a surveillance-vendor OUI (a real, globally-administered MAC) is a camera
    // phoning home. Check BEFORE the randomized-only density filter, which would drop a real-OUI source.
    uint8_t pcls, pcat;
    if (surveil_oui_match(sa, &pcls, &pcat)) {
        if (!fleet_mac_excluded(sa, now))
            surveil_note(surveil_hash(sa), p->rx_ctrl.rssi, pcls, pcat);  // Law 1: hash, MAC dropped
        return;
    }
    if (!(sa[0] & 0x02)) return;              // randomized (locally-administered) only = real-phone proxy
    if (fleet_mac_excluded(sa, now)) return;  // skip fleetmate decoys (our own are never received)
    wifi_obs_note(sa, now);                   // raw MAC hashed-and-dropped inside
```

- [ ] **Step 2: Compile-verify the Shade decoy (C6)**

Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c6 -Do build`
Expected: `BUILD: Project build complete.`

- [ ] **Step 3: Compile-verify the Ward decoy (C5)**

Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Do build`
Expected: `BUILD: Project build complete.`

- [ ] **Step 4: Regression — run the probe_audit host suite (the surveil matcher lives there)**

Run: `powershell -NoProfile -File tools/probe_audit/run.ps1`
Expected: all probe_audit tests pass (the `Surveil` matcher tests are unchanged and still green — D2 adds a caller, not new matcher logic).

- [ ] **Step 5: Commit**

```bash
git add main/wifi_observe.c
git commit -m "$(cat <<'EOF'
feat(surveil): detect surveillance gear via its Wi-Fi probe requests (D2)

wifi_observe now matches the vendor-OUI watchlist against probe-request
source MACs, before the randomized-only density filter that used to drop
real-OUI sources. A Flock/Axon device spraying real-OUI probes phoning home
is now caught -- passively, no opsec cost -- reusing the whole surveil
pipeline (surveil_note -> coexist -> detect_note_known -> CYD SURVEILLANCE).
Law-1 hash-and-drop preserved; density path unchanged. Compile-verified c5/c6.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

## Notes for the implementer

- **No host test** — `wifi_observe.c` is `esp_wifi` RX glue (not host-compilable), and the surveil
  matcher it calls is already covered by `tools/probe_audit/tests/test_surveil.py`. The compile-verifies
  plus the unchanged matcher suite are the gate.
- **On-air true positive** needs a Wi-Fi capture near real Flock/Axon hardware — deferred (no hardware
  this session, no real positive sample yet).
- **Do not** touch the density estimator, the beacon branch, `surveil_oui.c`, `detect.c`, or the wire.
