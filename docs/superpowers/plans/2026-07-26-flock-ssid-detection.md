# D3: Flock `test_flck` SSID Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect Flock cameras by the distinctive `test_flck` network name they probe for, by adding an SSID matcher and parsing the probe-request SSID element — reusing the existing surveillance pipeline.

**Architecture:** A new pure `surveil_ssid_match` (small SSID watchlist) lives beside the OUI matcher in `main/surveil_oui.c`. `wifi_observe.c`'s probe-request branch parses the SSID element and, on a watchlist hit, calls `surveil_note` — same downstream as the OUI hits.

**Tech Stack:** C (ESP-IDF firmware + host-compiled matcher), Python `unittest` via the `probe_dump` harness.

## Global Constraints

- Exact, case-sensitive SSID match only; watchlist seed: `{ "test_flck" (len 9) -> SIG_CLASS_FLOCK / SIG_CAT_CAMERA }`. Enum values: `SIG_CLASS_FLOCK = 3`, `SIG_CAT_CAMERA = 1`.
- The SSID check goes in the probe-request branch, **after** the D2 source-OUI check and **before** the randomized-only density filter.
- Reuse `surveil_note` / `surveil_hash` (device identity = source MAC, hashed — Law 1). No new module, no wire change, no `detect.c` change. The density path stays unchanged.
- Bounds-check every SSID-element read against `p->rx_ctrl.sig_len`.
- Commit identity is the repo-local `Em3ritus` noreply. Every commit carries the trailers:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.

---

### Task 1: The SSID matcher + host test

**Files:**
- Modify: `main/surveil_oui.h` (declare `surveil_ssid_match`)
- Modify: `main/surveil_oui.c` (SSID watchlist + matcher; add `<string.h>`)
- Modify: `tools/probe_audit/probe_dump.c` (add `--surveilssid` mode)
- Test: `tools/probe_audit/tests/test_surveil.py` (add `SurveilSsid`)

**Interfaces:**
- Produces: `bool surveil_ssid_match(const uint8_t *ssid, uint8_t len, uint8_t *class_id, uint8_t *category);`
  Consumed by Task 2.
- Produces (harness): `probe_dump --surveilssid <ascii_ssid>` prints `<matched 0/1> <class_id> <category>`.

- [ ] **Step 1: Write the failing test**

Append to `tools/probe_audit/tests/test_surveil.py`:

```python
def ssid_match(s):
    out = subprocess.check_output([EXE, "--surveilssid", s], text=True).split()
    return int(out[0]), int(out[1]), int(out[2])   # matched(0/1), class_id, category


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class SurveilSsid(unittest.TestCase):
    def test_test_flck_matches_flock_camera(self):
        # SIG_CLASS_FLOCK = 3, SIG_CAT_CAMERA = 1
        self.assertEqual(ssid_match("test_flck"), (1, 3, 1))

    def test_other_ssid_does_not_match(self):
        self.assertEqual(ssid_match("attwifi")[0], 0)

    def test_length_prefix_does_not_match(self):
        # exact-length match: an 8-char prefix of the 9-char name must NOT match
        self.assertEqual(ssid_match("test_flc")[0], 0)
```

- [ ] **Step 2: Build and run to verify it fails**

Run: `powershell -NoProfile -File tools/probe_audit/run.ps1 -Rebuild`
Expected: build succeeds but `--surveilssid` is unknown → the `SurveilSsid` tests FAIL; the rest stay green.

- [ ] **Step 3: Declare the matcher**

In `main/surveil_oui.h`, after the `surveil_oui_match` declaration, add:

```c
// Match an SSID (exact, case-sensitive) against the surveillance-SSID watchlist. Returns true (and
// fills class_id/category) on a hit; false otherwise. Pure. `ssid` is NOT NUL-terminated; `len` is the
// SSID element length.
bool surveil_ssid_match(const uint8_t *ssid, uint8_t len, uint8_t *class_id, uint8_t *category);
```

- [ ] **Step 4: Implement the matcher**

In `main/surveil_oui.c`, add `#include <string.h>` under the existing `#include <stddef.h>`, then add
after `surveil_oui_match`:

```c
typedef struct { const char *ssid; uint8_t len; uint8_t class_id; uint8_t category; } surveil_ssid_t;

// Surveillance network names probed for by known gear. test_flck: Flock Falcon/Sparrow saved dev
// network (CVE-2025-59409) — units probe for it when Wi-Fi is up. Exact, case-sensitive.
static const surveil_ssid_t SSID_WATCH[] = {
    { "test_flck", 9, SIG_CLASS_FLOCK, SIG_CAT_CAMERA },
};
#define SSID_WATCH_N (sizeof SSID_WATCH / sizeof SSID_WATCH[0])

bool surveil_ssid_match(const uint8_t *ssid, uint8_t len, uint8_t *class_id, uint8_t *category)
{
    for (size_t i = 0; i < SSID_WATCH_N; i++) {
        if (len == SSID_WATCH[i].len && memcmp(ssid, SSID_WATCH[i].ssid, len) == 0) {
            if (class_id) *class_id = SSID_WATCH[i].class_id;
            if (category) *category = SSID_WATCH[i].category;
            return true;
        }
    }
    return false;
}
```

- [ ] **Step 5: Add the `--surveilssid` harness mode**

In `tools/probe_audit/probe_dump.c`, add this block immediately after the existing `--surveiloui` block:

```c
    if (argc > 1 && strcmp(argv[1], "--surveilssid") == 0) {   // --surveilssid <ascii_ssid>
        const char *s = argc > 2 ? argv[2] : "";
        uint8_t cls = 255, cat = 255;
        int m = surveil_ssid_match((const uint8_t *)s, (uint8_t)strlen(s), &cls, &cat) ? 1 : 0;
        printf("%d %d %d\n", m, (int)cls, (int)cat);
        return 0;
    }
```

- [ ] **Step 6: Rebuild and run to verify it passes**

Run: `powershell -NoProfile -File tools/probe_audit/run.ps1 -Rebuild`
Expected: `SurveilSsid` PASSES (`test_flck`→(1,3,1); `attwifi`/`test_flc`→0); the rest of probe_audit still green.

- [ ] **Step 7: Commit**

```bash
git add main/surveil_oui.h main/surveil_oui.c tools/probe_audit/probe_dump.c tools/probe_audit/tests/test_surveil.py
git commit -m "$(cat <<'EOF'
feat(surveil): surveillance-SSID matcher (test_flck -> Flock)

Adds surveil_ssid_match + a small SSID watchlist seeded with Flock's saved
dev network "test_flck" (CVE-2025-59409) -> SIG_CLASS_FLOCK / SIG_CAT_CAMERA.
Pure, exact case-sensitive match. Host-tested via probe_dump --surveilssid.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

### Task 2: Parse the probe SSID in wifi_observe + compile-verify

**Files:**
- Modify: `main/wifi_observe.c` (the probe-request branch of `rx_cb`)

**Interfaces:**
- Consumes: `surveil_ssid_match`, `surveil_hash`, `surveil_note` (Task 1 + existing); `fleet_mac_excluded`. `surveil_oui.h` is already included in `wifi_observe.c`.

- [ ] **Step 1: Insert the SSID parse + check**

In `main/wifi_observe.c`, the probe-request branch currently ends with the D2 OUI check followed by the
density path:

```c
    uint8_t pcls, pcat;
    if (surveil_oui_match(sa, &pcls, &pcat)) {
        if (!fleet_mac_excluded(sa, now))
            surveil_note(surveil_hash(sa), p->rx_ctrl.rssi, pcls, pcat);  // Law 1: hash, MAC dropped
        return;
    }
    if (!(sa[0] & 0x02)) return;              // randomized (locally-administered) only = real-phone proxy
```

Insert the D3 SSID block between the OUI `if (...) { ... return; }` and the `if (!(sa[0] & 0x02))` line:

```c
    // D3: a directed probe for a known surveillance SSID (e.g. Flock's saved "test_flck") is a camera
    // phoning home; the source MAC may be randomized, so the OUI check above misses it.
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

- [ ] **Step 2: Compile-verify the Shade decoy (C6)**

Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c6 -Do build`
Expected: `BUILD: Project build complete.`

- [ ] **Step 3: Compile-verify the Ward decoy (C5)**

Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Do build`
Expected: `BUILD: Project build complete.`

- [ ] **Step 4: Commit**

```bash
git add main/wifi_observe.c
git commit -m "$(cat <<'EOF'
feat(surveil): detect Flock via the test_flck probe SSID (D3)

wifi_observe now parses the probe-request SSID element and, on a
surveillance-SSID watchlist hit (Flock's "test_flck"), records a surveil hit
keyed on the source MAC (hashed, Law 1) -- catching a Flock unit even when
it probes with a randomized MAC (so the OUI check misses it). Bounds-checked
against sig_len; density path unchanged. Compile-verified c5/c6.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

## Notes for the implementer

- **On-air true positive** needs a capture of a Flock unit actively probing for `test_flck` — deferred
  (intermittent + no hardware this session). The synthetic matcher test is the gate for now.
- **Do not** touch the density estimator, the beacon branch, `detect.c`, or the wire. The SSID hit reuses
  the exact `surveil_note` path the OUI hits already use.
