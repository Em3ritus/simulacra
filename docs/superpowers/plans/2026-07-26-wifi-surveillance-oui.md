# Wi-Fi Surveillance-Vendor OUI Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect surveillance infrastructure over Wi-Fi by matching AP beacon BSSIDs against a curated watchlist of verified vendor-owned IEEE OUI blocks (Flock `B4:1E:52`, Axon `00:25:DF`), surfacing hits on the CYD as surveillance presence via the existing detect→wire→CYD pipeline.

**Architecture:** A new `main/surveil_oui.c` owns the curated `{OUI, class, category}` table, the pure matcher, and a single-producer/consumer pending ring. `wifi_observe.c`'s promiscuous RX gains a beacon branch that matches the BSSID's OUI, hashes the MAC (Law 1), and notes a pending hit; `coexist_task` drains it into `detect_note_known` — reusing the threat table, wire, and CYD SURVEILLANCE render unchanged. The CYD render generalizes from CAMERA-only to CAMERA-or-BODYCAM.

**Tech Stack:** C (ESP-IDF firmware + shared `simulacra_radar` component, host-compiled via MSVC `cl`), Python `unittest` driving the `probe_dump` and `render_dump` harnesses.

## Global Constraints

- Watchlist contains **only verified vendor-owned OUI blocks**: `B4:1E:52` → `SIG_CLASS_FLOCK`/`SIG_CAT_CAMERA`; `00:25:DF` → `SIG_CLASS_AXON`/`SIG_CAT_BODYCAM`. Generic component OUIs (Espressif `a4:cf:12`, Liteon `70:c9:4e`, etc.) are **excluded** — they must not match.
- Detection confidence = **85** (a vendor's own block; stronger than the BLE `0x09C8` module-vendor match).
- Law 1 (hash-and-drop): the OUI is matched on the raw MAC, then the MAC is hashed; the full MAC never crosses the RX→coexist thread boundary or gets stored.
- Reuse the existing pipeline: **no changes to `detect.c` or the `radar_wire_status_t` format.**
- Own-fleet MACs must never be flagged (`fleet_mac_excluded`).
- Commit identity is the repo-local `Em3ritus` noreply. Every commit carries the trailers:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.

---

### Task 1: The curated matcher + taxonomy + host test

**Files:**
- Create: `main/surveil_oui.h`, `main/surveil_oui.c`
- Modify: `main/CMakeLists.txt` (add the source)
- Modify: `components/simulacra_radar/threat_sig.h` (add `SIG_CLASS_AXON`)
- Modify: `components/simulacra_radar/sig_class_name.h` (add `"Axon"`)
- Modify: `tools/pcap_learn/sig_scan.c` (extend `CLASS_NAME[]` + header string)
- Modify: `tools/probe_audit/probe_dump.c` (add `--surveiloui` mode)
- Modify: `tools/probe_audit/run.ps1` + `tools/probe_audit/Makefile` (compile `surveil_oui.c` + radar include)
- Test: `tools/probe_audit/tests/test_surveil.py` (new)

**Interfaces:**
- Produces: `bool surveil_oui_match(const uint8_t mac[6], uint8_t *class_id, uint8_t *category);`
  `void surveil_init(uint32_t salt);` `uint32_t surveil_hash(const uint8_t mac[6]);`
  `void surveil_note(uint32_t hash, int8_t rssi, uint8_t class_id, uint8_t category);`
  `bool surveil_next(uint32_t *hash, int8_t *rssi, uint8_t *class_id, uint8_t *category);`
  `SIG_CLASS_AXON` (enum value 4). Consumed by Tasks 2 and 3.

- [ ] **Step 1: Write the failing host test**

Create `tools/probe_audit/tests/test_surveil.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def match(machex):
    out = subprocess.check_output([EXE, "--surveiloui", machex], text=True).split()
    return int(out[0]), int(out[1]), int(out[2])   # matched(0/1), class_id, category


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class Surveil(unittest.TestCase):
    def test_flock_oui_matches_camera(self):
        m, cls, cat = match("b41e52aabbcc")
        # SIG_CLASS_FLOCK = 3, SIG_CAT_CAMERA = 1
        self.assertEqual((m, cls, cat), (1, 3, 1))

    def test_axon_oui_matches_bodycam(self):
        m, cls, cat = match("0025dfaabbcc")
        # SIG_CLASS_AXON = 4, SIG_CAT_BODYCAM = 2
        self.assertEqual((m, cls, cat), (1, 4, 2))

    def test_espressif_does_not_match(self):
        self.assertEqual(match("a4cf12aabbcc")[0], 0)   # generic module vendor -> no false positive

    def test_liteon_does_not_match(self):
        self.assertEqual(match("70c94eaabbcc")[0], 0)

    def test_random_does_not_match(self):
        self.assertEqual(match("123456789abc")[0], 0)
```

- [ ] **Step 2: Add `SIG_CLASS_AXON` to the taxonomy**

In `components/simulacra_radar/threat_sig.h`, extend `sig_class_t`:

```c
typedef enum { SIG_CLASS_AIRTAG = 0, SIG_CLASS_SMARTTAG, SIG_CLASS_TILE, SIG_CLASS_FLOCK, SIG_CLASS_AXON, SIG_CLASS_COUNT } sig_class_t;
```

In `components/simulacra_radar/sig_class_name.h`, add before `default`:

```c
        case SIG_CLASS_AXON:     return "Axon";
```

In `tools/pcap_learn/sig_scan.c`, extend `CLASS_NAME` and the DB header print:

```c
static const char *CLASS_NAME[] = { "AirTag", "SmartTag", "Tile", "Flock", "Axon" };
```
```c
    printf("signature DB      : %zu sigs (v%u): AirTag / SmartTag / Tile / Flock / Axon\n", ndb, sig_seed_version());
```

- [ ] **Step 3: Create the matcher header**

Create `main/surveil_oui.h`:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Curated Wi-Fi surveillance-vendor OUI detection. The watchlist holds ONLY verified vendor-owned
// IEEE OUI blocks (Flock, Axon), so a match is a trustworthy surveillance signal with no noise.

// Match mac[0..2] against the watchlist. Returns true (and fills class_id/category, sig_class_t /
// sig_category_t values) on a hit; false otherwise. Pure.
bool surveil_oui_match(const uint8_t mac[6], uint8_t *class_id, uint8_t *category);

// Seed the per-session hash salt (call once, e.g. surveil_init(esp_random())).
void surveil_init(uint32_t salt);
// Salted FNV-1a over the 6-byte MAC (Law 1: hash in the RX path, then drop the MAC).
uint32_t surveil_hash(const uint8_t mac[6]);

// Single-producer (Wi-Fi RX) / single-consumer (coexist) pending ring.
void surveil_note(uint32_t hash, int8_t rssi, uint8_t class_id, uint8_t category);
bool surveil_next(uint32_t *hash, int8_t *rssi, uint8_t *class_id, uint8_t *category);
```

- [ ] **Step 4: Create the matcher implementation**

Create `main/surveil_oui.c`:

```c
#include "surveil_oui.h"
#include "threat_sig.h"
#include <stddef.h>

typedef struct { uint8_t oui[3]; uint8_t class_id; uint8_t category; } surveil_entry_t;

// VERIFIED vendor-owned IEEE blocks only (see the design doc). Add entries only after confirming the
// OUI is registered to a surveillance vendor outright -- never a shared component-module vendor.
static const surveil_entry_t WATCH[] = {
    { { 0xB4, 0x1E, 0x52 }, SIG_CLASS_FLOCK, SIG_CAT_CAMERA },   // Flock Safety (ALPR cameras)
    { { 0x00, 0x25, 0xDF }, SIG_CLASS_AXON,  SIG_CAT_BODYCAM },  // Axon / Taser (bodycam + evidence)
};
#define WATCH_N (sizeof WATCH / sizeof WATCH[0])

bool surveil_oui_match(const uint8_t mac[6], uint8_t *class_id, uint8_t *category)
{
    for (size_t i = 0; i < WATCH_N; i++) {
        if (mac[0] == WATCH[i].oui[0] && mac[1] == WATCH[i].oui[1] && mac[2] == WATCH[i].oui[2]) {
            if (class_id) *class_id = WATCH[i].class_id;
            if (category) *category = WATCH[i].category;
            return true;
        }
    }
    return false;
}

#define SURVEIL_RING 8
static uint32_t s_salt;
static struct { uint32_t hash; int8_t rssi; uint8_t class_id; uint8_t category; } s_ring[SURVEIL_RING];
static volatile uint32_t s_head, s_tail;

void surveil_init(uint32_t salt) { s_salt = salt; s_head = 0; s_tail = 0; }

uint32_t surveil_hash(const uint8_t mac[6])
{
    uint32_t h = 2166136261u ^ s_salt;
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

void surveil_note(uint32_t hash, int8_t rssi, uint8_t class_id, uint8_t category)
{
    uint32_t n = (s_head + 1u) % SURVEIL_RING;
    if (n == s_tail) return;                        // ring full -> drop (hits are rare)
    s_ring[s_head].hash = hash; s_ring[s_head].rssi = rssi;
    s_ring[s_head].class_id = class_id; s_ring[s_head].category = category;
    s_head = n;
}

bool surveil_next(uint32_t *hash, int8_t *rssi, uint8_t *class_id, uint8_t *category)
{
    if (s_tail == s_head) return false;
    *hash = s_ring[s_tail].hash; *rssi = s_ring[s_tail].rssi;
    *class_id = s_ring[s_tail].class_id; *category = s_ring[s_tail].category;
    s_tail = (s_tail + 1u) % SURVEIL_RING;
    return true;
}
```

- [ ] **Step 5: Register the source in the firmware build**

In `main/CMakeLists.txt`, add `"surveil_oui.c"` to the `SRCS` list (e.g. right after `"wifi_observe.c"`).

- [ ] **Step 6: Add the `--surveiloui` harness mode**

In `tools/probe_audit/probe_dump.c`, add `#include "surveil_oui.h"` near the other includes, then add this block inside `main` before the `--pick` block:

```c
    if (argc > 1 && strcmp(argv[1], "--surveiloui") == 0) {   // --surveiloui <mac_hex_12>
        uint8_t mac[6] = {0};
        const char *h = argc > 2 ? argv[2] : "";
        for (int i = 0; i < 6 && h[2*i] && h[2*i+1]; i++) {
            char b[3] = { h[2*i], h[2*i+1], 0 }; mac[i] = (uint8_t)strtoul(b, 0, 16);
        }
        uint8_t cls = 255, cat = 255;
        int m = surveil_oui_match(mac, &cls, &cat) ? 1 : 0;
        printf("%d %d %d\n", m, (int)cls, (int)cat);
        return 0;
    }
```

- [ ] **Step 7: Add `surveil_oui.c` + the radar include to the probe_audit build**

In `tools/probe_audit/run.ps1`, the `cl` line: add `/I..\..\components\simulacra_radar` to the include flags, and add `..\..\main\surveil_oui.c` to the source list. In `tools/probe_audit/Makefile`, add `$(ROOT)/main/surveil_oui.c` to `SRC` and `-I$(ROOT)/components/simulacra_radar` to `INC`.

- [ ] **Step 8: Build and run the test to verify it passes**

Run: `powershell -NoProfile -File tools/probe_audit/run.ps1 -Rebuild`
Expected: build succeeds; `Surveil` tests PASS (Flock→(1,3,1), Axon→(1,4,2), Espressif/Liteon/random→0); the rest of the probe_audit suite still green.

- [ ] **Step 9: Commit**

```bash
git add main/surveil_oui.h main/surveil_oui.c main/CMakeLists.txt components/simulacra_radar/threat_sig.h components/simulacra_radar/sig_class_name.h tools/pcap_learn/sig_scan.c tools/probe_audit/probe_dump.c tools/probe_audit/run.ps1 tools/probe_audit/Makefile tools/probe_audit/tests/test_surveil.py
git commit -m "$(cat <<'EOF'
feat(surveil): curated Wi-Fi surveillance-vendor OUI matcher

New main/surveil_oui.c: a watchlist of VERIFIED vendor-owned IEEE OUI
blocks (Flock B4:1E:52 -> CAMERA, Axon 00:25:DF -> BODYCAM) with a pure
matcher, salted hash, and a single-producer/consumer pending ring for the
RX->coexist bridge. Adds SIG_CLASS_AXON. Host-tested: Flock/Axon match with
the right class/category; Espressif/Liteon/random OUIs never match.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

### Task 2: Wi-Fi observe → coexist bridge

**Files:**
- Modify: `main/wifi_observe.c` (beacon branch)
- Modify: `main/coexist.c` (init + drain)

**Interfaces:**
- Consumes: `surveil_oui_match`, `surveil_hash`, `surveil_note`, `surveil_init`, `surveil_next` (Task 1); existing `fleet_mac_excluded`, `detect_note_known`, `s_epoch`.

- [ ] **Step 1: Add the beacon branch to the promiscuous RX**

In `main/wifi_observe.c`, add `#include "surveil_oui.h"` near the top includes. Then in `rx_cb`, insert a beacon branch immediately after `const uint8_t *f = p->payload;` and before `if (f[0] != 0x40) return;`:

```c
    if (f[0] == 0x80) {                              // beacon: BSSID vs surveillance-vendor OUI watchlist
        const uint8_t *bssid = f + 10;               // addr2 = the AP's MAC
        uint32_t bnow = (uint32_t)(esp_timer_get_time() / 1000);
        if (fleet_mac_excluded(bssid, bnow)) return; // never flag our own fleet
        uint8_t cls, cat;
        if (surveil_oui_match(bssid, &cls, &cat))
            surveil_note(surveil_hash(bssid), p->rx_ctrl.rssi, cls, cat);  // Law 1: hash, MAC dropped
        return;
    }
    if (f[0] != 0x40) return;                        // probe request (density path below, unchanged)
```

Delete the now-duplicate `if (f[0] != 0x40) return;` that followed (the density path's own line) — keep exactly one, the one just added above.

- [ ] **Step 2: Seed the salt + drain hits in coexist**

In `main/coexist.c`, add `#include "surveil_oui.h"` with the other includes, and a define near the top defines (e.g. by `DETECT_EPOCH_DRIFT`):

```c
#define SURVEIL_CONF 85           // vendor-owned-OUI surveillance hit confidence
```

In `coexist_start`, next to `s_detect_salt = detect_load_salt();`, add:

```c
    surveil_init(esp_random());                       // per-session salt for the Wi-Fi surveillance hits
```

In `coexist_task`, right after the existing `detect_drain_pending` handling block, add:

```c
        {
            uint32_t sh; int8_t sr; uint8_t sc, sk;
            while (surveil_next(&sh, &sr, &sc, &sk)) {  // Wi-Fi surveillance hits (RX thread) -> detector
                if (detect_note_known(sh, sr, sk, sc, SURVEIL_CONF, s_epoch) == DETECT_CONFIRM)
                    ESP_LOGW(TAG, "SURVEILLANCE %s id=%04x rssi=%d", sig_class_name(sk),
                             (unsigned)(sh & 0xFFFF), sr);
            }
        }
```

(`sig_class_name` is already included via `sig_class_name.h` in coexist.c.)

- [ ] **Step 3: Compile-verify the Shade decoy (C6)**

Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c6 -Do build`
Expected: `BUILD: Project build complete.`

- [ ] **Step 4: Commit**

```bash
git add main/wifi_observe.c main/coexist.c
git commit -m "$(cat <<'EOF'
feat(surveil): bridge Wi-Fi beacon OUI hits into the detector

wifi_observe's promiscuous RX gains a beacon branch: match the BSSID's OUI
against the surveillance watchlist, skip fleetmates, hash the MAC (Law 1),
and note a pending hit. coexist drains the ring on its own thread into
detect_note_known (conf 85) -- reusing the threat table, wire, and CYD
surveillance surfacing. Compile-verified c6.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

### Task 3: CYD render generalization (CAMERA or BODYCAM)

**Files:**
- Modify: `components/simulacra_radar/radar_render.c` (generalize the surveillance category test)
- Modify: `tools/radar_audit/render_dump.c` (inject a BODYCAM/Axon threat)
- Test: `tools/radar_audit/tests/test_radar_render.py` (add a BODYCAM render test)

**Interfaces:**
- Consumes: `SIG_CLASS_AXON`, `SIG_CAT_BODYCAM` (Task 1); existing `SIG_CAT_CAMERA`, `sig_class_name`, the `draw_detail`/`draw_home` SURVEILLANCE code, and render_dump's `ncam` arg.
- Produces (harness): `render_dump <...12 args...> <ncam> <surv_kind>` — new final arg `surv_kind` (0 = Flock/CAMERA default, 1 = Axon/BODYCAM) sets the category/class of the injected camera threats.

- [ ] **Step 1: Write the failing render test**

Append to `tools/radar_audit/tests/test_radar_render.py` (near the existing `SurveillancePresence` class):

```python
@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class SurveillanceBodycam(unittest.TestCase):
    def test_bodycam_shows_surveillance_and_axon_label(self):
        # DETAIL: 1 threat, 1 camera, surv_kind=1 (Axon/BODYCAM)
        texts = render(DETAIL, 1, 1, 1, 8, 16, 8, 1, 10, 0, 0, 0, 1, 1)
        self.assertIn("SURVEILLANCE", texts, f"no surveillance section; drew: {texts}")
        self.assertIn("Axon", texts, f"no Axon label; drew: {texts}")

    def test_home_indicator_for_bodycam(self):
        texts = render(HOME, 1, 1, 1, 8, 16, 8, 1, 10, 0, 0, 0, 1, 1)
        self.assertIn("!1", texts, f"no HOME surveil indicator for bodycam; drew: {texts}")
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `powershell -NoProfile -File tools/radar_audit/run.ps1 -Rebuild`
Expected: `render_dump` builds but ignores the extra `surv_kind` arg, so the injected threat is CAMERA/Flock → the BODYCAM tests FAIL (no "Axon"). Existing tests still pass.

- [ ] **Step 3: Let render_dump inject a BODYCAM/Axon threat**

In `tools/radar_audit/render_dump.c`, replace the existing `ncam` block with:

```c
    // arg 13: how many threats are surveillance; arg 14: kind (0=Flock/CAMERA, 1=Axon/BODYCAM).
    int ncam = argc > 13 ? atoi(argv[13]) : 0;
    int surv_kind = argc > 14 ? atoi(argv[14]) : 0;
    uint8_t sv_cat = surv_kind ? SIG_CAT_BODYCAM : SIG_CAT_CAMERA;
    uint8_t sv_cls = surv_kind ? SIG_CLASS_AXON  : SIG_CLASS_FLOCK;
    for (int i = 0; i < ncam && i < st.threat_count && i < RADAR_MAX_THREATS; i++) {
        st.threats[i].kind     = DETECT_KIND_KNOWN;
        st.threats[i].category = sv_cat;
        st.threats[i].class_id = sv_cls;
        st.threats[i].best_rssi = -55;
    }
```

- [ ] **Step 4: Generalize the surveillance category test in the renderer**

In `components/simulacra_radar/radar_render.c`, add a helper above `draw_detail`:

```c
static inline int is_surveil_cat(uint8_t c){ return c == SIG_CAT_CAMERA || c == SIG_CAT_BODYCAM; }
```

Then replace the four `category == SIG_CAT_CAMERA` / `category != SIG_CAT_CAMERA` tests:
- `draw_detail` cameras counter: `if(is_surveil_cat(st->threats[i].category)){ cameras++; continue; }`
- `draw_detail` follower-loop skip: `if(is_surveil_cat(st->threats[i].category)) continue;`
- `draw_detail` surveillance-loop filter: `if(!is_surveil_cat(st->threats[i].category)) continue;`
- `draw_home` nsurv counter: `if(is_surveil_cat(st->threats[i].category)) nsurv++;`

- [ ] **Step 5: Rebuild and run the tests to verify they pass**

Run: `powershell -NoProfile -File tools/radar_audit/run.ps1 -Rebuild`
Expected: build succeeds; `SurveillanceBodycam` PASSES (SURVEILLANCE + "Axon" + HOME "!1"); the existing `SurveillancePresence` (Flock/CAMERA) tests still green.

- [ ] **Step 6: Compile-verify c6, c5, and cyd**

Run each:
```
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c6 -Do build
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Do build
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target cyd -Do build
```
Expected: `BUILD: Project build complete.` for each.

- [ ] **Step 7: Commit**

```bash
git add components/simulacra_radar/radar_render.c tools/radar_audit/render_dump.c tools/radar_audit/tests/test_radar_render.py
git commit -m "$(cat <<'EOF'
feat(cyd): surface BODYCAM surveillance alongside CAMERA

Generalize the CYD SURVEILLANCE section + HOME indicator from CAMERA-only
to CAMERA-or-BODYCAM (is_surveil_cat), so Axon (bodycam/evidence infra)
Wi-Fi hits surface as surveillance labeled "Axon", same as Flock cameras.
render_dump gains a surv_kind arg; radar_audit covers the BODYCAM path.
Compile-verified c5/c6/cyd.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

## Notes for the implementer

- **No real positive capture.** Validation is the synthetic OUI test (Task 1) + the render test (Task 3). A true positive needs a Wi-Fi capture near known Flock/Axon hardware.
- **Ring concurrency:** `surveil_note` (Wi-Fi RX thread) is the sole producer, `surveil_next` (coexist thread) the sole consumer, with `volatile` head/tail; aligned 32-bit access is atomic on the ESP32 targets and hits are rare, so no lock is needed. A dropped hit on a full ring is harmless.
- **Do not** change `detect.c`, the wire, or the probe-request density path in `wifi_observe.c` (the beacon branch is additive and returns early).
