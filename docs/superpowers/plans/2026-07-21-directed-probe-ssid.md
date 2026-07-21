# Directed-Probe SSID Realism Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make ~62% of decoy Wi-Fi probe personas probe for named SSIDs drawn from a fixed curated pool of ubiquitous PUBLIC network names, so the crowd's device-level wildcard fraction drops from 1.0 toward the measured real-crowd ~0.36 — while the wildcard code path stays byte-identical to today.

**Architecture:** Four units. (1) A new pure `main/ssid_pool.{h,c}` holds a compiled-in weighted list of generic public SSIDs with pure accessors. (2) `probe_build_request` gains an optional SSID and composes the frame in two pieces (byte-identical when the SSID is NULL). (3) `probe_agent_t` gains a per-life assigned SSID set (pool indices), drawn once at each identity birth, plus a pure per-burst selector. (4) The live injector (`probe.c`) resolves each due agent's per-burst SSID and passes it through. All logic is host-testable via `tools/probe_audit` (probe_dump + Python); the firmware compile is verified on `build_c5`.

**Tech Stack:** C99 firmware (ESP-IDF, target esp32c5); MSVC `cl` host build of `probe_dump.exe` via `host_stubs` + `/FIportab.h`; Python 3.12 stdlib `unittest`.

## Global Constraints

- **Safety invariant — never observe/learn:** the SSID pool is a fixed compiled-in list only. `ssid_pool.c`/`ssid_pool.h` must NOT include or reference any of `observe.h`, `learn.h`, `sig_store.h`, `wifi_observe.h`, or any capture/learn source. Enforced by a structural test.
- **Safety invariant — per-persona independent draw:** each agent draws its own SSID subset independently in `agent_spawn` and `probe_agent_sync`. No node-wide shared list; no node-to-node uniqueness enforcement. Popular pool entries recurring across agents is correct.
- **Assignment is per-life, not per-rotation:** the intra-life MAC rotation branch in `probe_agents_lifecycle` must NOT touch `ssid_n`/`ssid_idx`. Only birth sites (`agent_spawn`, `probe_agent_sync`) assign.
- **Wildcard path unchanged:** `probe_build_request(..., ssid=NULL, ssid_len=0, ...)` must produce byte-for-byte the same frame as today. Pinned by the existing `fixtures/*.hex` byte-exact test.
- **Calibration knob:** `SSID_ASSIGN_PCT = 62` (percent of personas that get a named set). Device-level wildcard fraction the audit measures is driven directly by this (an assigned agent = "probes a name" = non-wildcard, matching how `kismet_behavior.py` classifies the real reference: a device is wildcard iff it never probed a named SSID). Target measured decoy wildcard fraction: 0.28–0.50 (real anchor ~0.36).
- **Pool bound:** `SSID_POOL_MAX_LEN = 32` (802.11 SSID element max). Every pool entry is non-empty and ≤ 32 bytes.
- **Design refinement vs. spec (deliberate):** the spec floated tying the *per-burst* wildcard probability to the measured axis. This plan instead measures the axis at the **device/identity level** (assignment fraction) exactly as the kismet reference does, and treats the per-burst interleave (`SSID_BURST_NAMED_PCT = 60`) purely as an on-air realism knob. This makes `SSID_ASSIGN_PCT` robustly control the headline and removes the spec's flagged short-window misclassification fragility. Same success criterion (device-level wildcard fraction in the target band).
- **Commit trailers required** on every commit:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
  ```
- **No PII in tracked files:** never hardcode the OS username, a real person's name, or a real personal MAC. Commit author identity is the repo default (Em3ritus).
- **Host test invariant:** the full `tools/probe_audit` suite must stay green (existing count + new tests). `PowerShell` `>` writes UTF-16/BOM — never use it to produce files a C reader parses; not relevant here (no generated data files), but keep to `python -m unittest` for tests.

---

### Task 1: SSID pool module

**Files:**
- Create: `main/ssid_pool.h`
- Create: `main/ssid_pool.c`
- Modify: `main/CMakeLists.txt:2` (add `ssid_pool.c` to SRCS)
- Modify: `tools/probe_audit/Makefile:3` (add `$(ROOT)/main/ssid_pool.c` to SRC)
- Modify: `tools/probe_audit/run.ps1:23` (add `..\..\main\ssid_pool.c` to the `cl` source list)
- Modify: `tools/probe_audit/probe_dump.c` (add `--ssidpool` mode + `#include "ssid_pool.h"`)
- Test: `tools/probe_audit/tests/test_ssid_pool.py` (create)

**Interfaces:**
- Consumes: `esp_random()` (from `esp_random.h`; host stub already provides it).
- Produces:
  - `#define SSID_POOL_MAX_LEN 32`
  - `int ssid_pool_count(void);` — number of entries (> 0).
  - `const char *ssid_pool_at(int i, uint8_t *len_out);` — NUL-terminated name + its byte length via `*len_out`; returns `NULL` and leaves `*len_out` untouched if `i` out of range.
  - `int ssid_pool_pick_weighted(void);` — weighted random index in `[0, ssid_pool_count())`.

- [ ] **Step 1: Write `main/ssid_pool.h`**

```c
#pragma once
#include <stdint.h>

// A fixed, compiled-in pool of GENERIC PUBLIC SSIDs (ubiquitous open/carrier/retail hotspot names
// that appear in a very large number of real devices' preferred-network lists everywhere). A decoy
// probing one is indistinguishable from the real background and reveals nothing about THIS user.
//
// SAFETY: this pool is the ONLY source of directed-probe SSIDs. It is NEVER populated from observed
// or learned traffic -- probing a locally-real SSID would announce the user's actual associations.
// This header/impl must not include any observe/learn/capture source (structurally tested).
#define SSID_POOL_MAX_LEN 32   // 802.11 SSID element maximum

int         ssid_pool_count(void);
const char *ssid_pool_at(int i, uint8_t *len_out);   // NUL-terminated name + byte length; NULL if OOR
int         ssid_pool_pick_weighted(void);           // weighted random index (uses esp_random)
```

- [ ] **Step 2: Write `main/ssid_pool.c`**

```c
#include "ssid_pool.h"
#include "esp_random.h"
#include <string.h>

// Curated generic PUBLIC network names + draw weights (popular ones recur across personas = realistic
// overlap). All are well-documented open/carrier/retail hotspot SSIDs found in countless PNLs; none
// implies a specific private/home network. Contents are data -- freely editable without redesign.
static const struct { const char *name; uint8_t weight; } POOL[] = {
    { "xfinitywifi",       30 },
    { "attwifi",           22 },
    { "XFINITY",           12 },
    { "Google Starbucks",  10 },
    { "eduroam",            8 },
    { "GuestWiFi",          8 },
    { "Guest",              7 },
    { "SpectrumWiFi",       6 },
    { "optimumwifi",        6 },
    { "Boingo Hotspot",     4 },
};
#define POOL_N ((int)(sizeof(POOL) / sizeof(POOL[0])))

int ssid_pool_count(void) { return POOL_N; }

const char *ssid_pool_at(int i, uint8_t *len_out)
{
    if (i < 0 || i >= POOL_N) return 0;
    if (len_out) *len_out = (uint8_t)strlen(POOL[i].name);
    return POOL[i].name;
}

int ssid_pool_pick_weighted(void)
{
    uint32_t total = 0;
    for (int i = 0; i < POOL_N; i++) total += POOL[i].weight;
    if (!total) return 0;
    uint32_t r = esp_random() % total;
    for (int i = 0; i < POOL_N; i++) {
        if (r < POOL[i].weight) return i;
        r -= POOL[i].weight;
    }
    return 0;
}
```

- [ ] **Step 3: Wire the source into all three build lists**

`main/CMakeLists.txt` line 2 — add `"ssid_pool.c"` to the `SRCS` list (e.g. right after `"probe_agents.c"`):
```
    SRCS "simulacra_main.c" ... "probe_agents.c" "ssid_pool.c" "sniff.c" ...
```

`tools/probe_audit/Makefile` line 3 — append to `SRC`:
```
SRC := probe_dump.c $(ROOT)/main/probe_frame.c $(ROOT)/main/probe_agents.c $(ROOT)/main/ssid_pool.c $(ROOT)/main/uniq_id.c $(ROOT)/main/phantom.c $(ROOT)/main/wifi_density.c
```

`tools/probe_audit/run.ps1` line 23 — add `..\..\main\ssid_pool.c` to the `cl` invocation:
```
           probe_dump.c ..\..\main\probe_frame.c ..\..\main\probe_agents.c ..\..\main\ssid_pool.c ..\..\main\uniq_id.c ..\..\main\phantom.c host_stubs\ble_devices_stub.c ..\..\main\wifi_density.c /Fe:probe_dump.exe | Out-Null
```

- [ ] **Step 4: Add the `--ssidpool` dump mode to `probe_dump.c`**

Add `#include "ssid_pool.h"` near the other includes (after `#include "wifi_density.h"`). Add this mode block near the other `--`-prefixed modes (e.g. right before the `--pick` block at line 203):

```c
    if (argc > 1 && strcmp(argv[1], "--ssidpool") == 0) {
        if (argc > 2) {                                  // weighted-pick histogram: --ssidpool <seed> <n>
            srand((unsigned)strtoul(argv[2], 0, 10));
            int n = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 10000;
            for (int i = 0; i < n; i++) printf("%d\n", ssid_pool_pick_weighted());
            return 0;
        }
        printf("%d\n", ssid_pool_count());               // no args: count, then "<len> <name>" per entry
        for (int i = 0; i < ssid_pool_count(); i++) {
            uint8_t L = 0; const char *s = ssid_pool_at(i, &L);
            printf("%d %s\n", (int)L, s);
        }
        return 0;
    }
```

- [ ] **Step 5: Write the failing test `tools/probe_audit/tests/test_ssid_pool.py`**

```python
import os, re, subprocess, unittest
from collections import Counter

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(TOOL))
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")
POOL_MAX_LEN = 32


def pool_entries():
    out = subprocess.check_output([EXE, "--ssidpool"], text=True).splitlines()
    count = int(out[0])
    entries = []
    for ln in out[1:count + 1]:
        length, name = ln.split(" ", 1)
        entries.append((int(length), name))
    return count, entries


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class SsidPool(unittest.TestCase):
    def test_pool_nonempty(self):
        count, entries = pool_entries()
        self.assertGreater(count, 0)
        self.assertEqual(count, len(entries))

    def test_entries_valid_length(self):
        _, entries = pool_entries()
        for length, name in entries:
            self.assertEqual(length, len(name), f"{name!r} length field mismatch")
            self.assertGreater(length, 0, f"{name!r} empty")
            self.assertLessEqual(length, POOL_MAX_LEN, f"{name!r} exceeds SSID_POOL_MAX_LEN")

    def test_weighted_pick_in_range_and_favors_heavy(self):
        count, _ = pool_entries()
        out = subprocess.check_output([EXE, "--ssidpool", "7", "20000"], text=True).split()
        idx = [int(x) for x in out]
        self.assertTrue(all(0 <= i < count for i in idx), "pick out of range")
        c = Counter(idx)
        self.assertGreater(c[0], c[count - 1], "heaviest-weight entry not favored over lightest")

    def test_source_has_no_observe_or_learn_dependency(self):
        # SAFETY invariant: the pool must never pull from observed/learned traffic.
        forbidden = ("observe.h", "learn.h", "sig_store.h", "wifi_observe.h")
        for fn in ("ssid_pool.c", "ssid_pool.h"):
            text = open(os.path.join(ROOT, "main", fn)).read()
            includes = re.findall(r'#\s*include\s+"([^"]+)"', text)
            for f in forbidden:
                self.assertNotIn(f, includes, f"{fn} must not include {f}")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 6: Rebuild and run — verify tests fail then pass**

Run (from a Developer PowerShell for VS):
```
tools\probe_audit\run.ps1 -Rebuild
```
Expected: `probe_dump.exe` compiles with `ssid_pool.c`; `test_ssid_pool.py` all pass; the rest of the suite stays green. (Before Steps 1-4 exist the module wouldn't link — this step confirms the build wiring and the four pool tests together.)

- [ ] **Step 7: Commit**

```bash
git add main/ssid_pool.h main/ssid_pool.c main/CMakeLists.txt \
        tools/probe_audit/Makefile tools/probe_audit/run.ps1 \
        tools/probe_audit/probe_dump.c tools/probe_audit/tests/test_ssid_pool.py
git commit -m "feat(probe): compiled-in public-SSID pool + host dump/tests

Fixed curated pool of generic PUBLIC network names with weighted pick.
SAFETY: never sourced from observe/learn (structurally tested)."
```

---

### Task 2: Frame builder optional SSID

**Files:**
- Modify: `main/probe_frame.h:31-32` (signature of `probe_build_request`)
- Modify: `main/probe_frame.c:119-140` (`probe_build_request` body)
- Modify: `main/probe.c:119` (caller passes `NULL, 0` for now)
- Modify: `tools/probe_audit/probe_dump.c:210-222` (default dump path: optional SSID arg)
- Test: `tools/probe_audit/tests/test_probe_frame.py` (extend)

**Interfaces:**
- Consumes: nothing new (uses literal `32` for the SSID max; no `ssid_pool.h` include needed here).
- Produces: new signature
  ```c
  int probe_build_request(const uint8_t mac[6], uint8_t ch, probe_arch_t arch, bool band5,
                          const char *ssid, uint8_t ssid_len, uint8_t *out, size_t *out_len);
  ```
  `ssid==NULL` (or `ssid_len==0`) → wildcard, byte-identical to the old output. Return codes: `0` ok, `1` bad arch, `2` band absent, `3` frame overflow, `4` ssid_len > 32.

- [ ] **Step 1: Update the byte-exact test to call through the new dump arg (still wildcard) and add a directed case**

In `tools/probe_audit/tests/test_probe_frame.py`, replace `build_arch` and add a directed helper + tests:

```python
def build_arch(idx, ch, b5, ssid=None):
    args = [EXE, str(idx), str(ch), str(b5)]
    if ssid is not None:
        args.append(ssid)
    out = subprocess.check_output(args, text=True).strip()
    return bytes.fromhex(out)
```

Add these test methods to `class ProbeFrame`:

```python
    def test_directed_ssid_element_present(self):
        f = build_arch(0, 6, 0, "xfinitywifi")           # iphone 2.4, named
        d = ies(f)
        self.assertIn(0x00, d, "SSID element present")
        self.assertEqual(d[0x00], b"xfinitywifi", "directed SSID bytes emitted")
        self.assertEqual(d[0x03], bytes([6]), "DS channel still patched with a directed SSID")
        self.assertLessEqual(len(f), 256)

    def test_directed_body_matches_wildcard_after_ssid(self):
        # Everything after the SSID element must be identical to the wildcard frame's body-after-SSID.
        wild = build_arch(0, 6, 0)
        named = build_arch(0, 6, 0, "attwifi")
        # wildcard SSID element is 2 bytes (0x00,0x00) at body offset 0; directed is 2+len(name).
        wild_after  = wild[24 + 2:]
        named_after = named[24 + 2 + len("attwifi"):]
        self.assertEqual(named_after, wild_after, "IE body after SSID diverged")
```

- [ ] **Step 2: Run the new tests to verify they fail**

Run:
```
"C:/Program Files/Python312/python.exe" -m unittest tools.probe_audit.tests.test_probe_frame -v
```
Expected: FAIL — the current `probe_dump` default path ignores a 5th arg and the current builder has no SSID support, so `test_directed_ssid_element_present` sees a wildcard (empty) SSID. (`test_matches_fixture` must still PASS — do not break it.)

- [ ] **Step 3: Change the header signature `main/probe_frame.h`**

Replace lines 27-32 (the doc comment + declaration) with:

```c
// Build a probe request for source `mac` on `ch`, using archetype `arch`'s per-band IE set. band5
// selects the 5 GHz tail. `ssid`/`ssid_len` give an optional directed SSID: pass NULL/0 for the
// wildcard broadcast probe (output then byte-identical to the historical wildcard-only builder).
// Writes the 802.11 frame to out (<= PROBE_FRAME_MAX) and its length. Returns 0 on success; 1 bad
// arch, 2 arch lacks that band, 3 frame overflow, 4 ssid_len exceeds the 802.11 SSID max (32).
int    probe_build_request(const uint8_t mac[6], uint8_t ch, probe_arch_t arch, bool band5,
                           const char *ssid, uint8_t ssid_len, uint8_t *out, size_t *out_len);
```

- [ ] **Step 4: Rewrite the builder body `main/probe_frame.c`**

Replace the whole `probe_build_request` function (lines 119-140) with:

```c
int probe_build_request(const uint8_t mac[6], uint8_t ch, probe_arch_t arch, bool band5,
                        const char *ssid, uint8_t ssid_len, uint8_t *out, size_t *out_len)
{
    const probe_archetype_t *a = probe_archetype(arch);
    if (!a) return 1;
    const uint8_t *tail = band5 ? a->tail5 : a->tail24;
    uint16_t tlen       = band5 ? a->tail5_len : a->tail24_len;
    if (!tail || tlen == 0) return 2;                 // archetype lacks this band
    if (ssid == 0) ssid_len = 0;                      // NULL -> wildcard
    if (ssid_len > 32) return 4;                      // 802.11 SSID element max
    // body = SSID element (2 + ssid_len) + the archetype tail AFTER its placeholder SSID (tlen - 2)
    if (24u + (uint32_t)tlen + (uint32_t)ssid_len > PROBE_FRAME_MAX) return 3;

    uint8_t *p = out;
    *p++ = 0x40; *p++ = 0x00;                          // frame control: mgmt/probe-req
    *p++ = 0x00; *p++ = 0x00;                          // duration
    memset(p, 0xff, 6); p += 6;                        // DA broadcast
    memcpy(p, mac, 6); p += 6;                         // SA = our randomized MAC
    memset(p, 0xff, 6); p += 6;                        // BSSID broadcast
    *p++ = 0x00; *p++ = 0x00;                          // seq control (driver overwrites)
    *p++ = 0x00; *p++ = ssid_len;                      // SSID element: wildcard (0) or directed
    if (ssid_len) { memcpy(p, ssid, ssid_len); p += ssid_len; }
    memcpy(p, tail + 2, (size_t)(tlen - 2)); p += (tlen - 2);   // tail after its placeholder SSID
    patch_ds_channel(out + 24, (uint16_t)(2u + ssid_len + (tlen - 2)), ch);
    *out_len = (size_t)(p - out);
    return 0;
}
```

- [ ] **Step 5: Update the live caller `main/probe.c`**

At line 119, change the call to pass wildcard explicitly (Task 4 upgrades this to the per-burst SSID):

```c
        if (probe_build_request(due[i]->mac, channel, due[i]->arch, band5, NULL, 0, f, &n) != 0)
```

- [ ] **Step 6: Update the host dump default path `tools/probe_audit/probe_dump.c`**

Replace the default (trailing) block starting at line 210 (`probe_arch_t a = ...`) through its `probe_build_request` call with:

```c
    probe_arch_t a = (argc > 1) ? (probe_arch_t)strtoul(argv[1], 0, 10) : ARCH_IPHONE;
    unsigned ch    = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 10) : 6;
    bool band5     = (argc > 3) ? (strtoul(argv[3], 0, 10) != 0) : false;
    const char *ssid = (argc > 4) ? argv[4] : 0;                 // optional directed SSID
    uint8_t ssid_len = ssid ? (uint8_t)strlen(ssid) : 0;

    uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
    if (probe_build_request(mac, (uint8_t)ch, a, band5, ssid, ssid_len, f, &n)) {
        fprintf(stderr, "build failed (arch=%u band5=%d)\n", a, band5);
        return 2;
    }
```

- [ ] **Step 7: Rebuild and run — verify all frame tests pass (fixtures unchanged)**

Run:
```
tools\probe_audit\run.ps1 -Rebuild
```
Expected: `test_probe_frame.py` all pass, INCLUDING `test_matches_fixture` (wildcard path byte-identical) and the two new directed tests. Full suite green.

- [ ] **Step 8: Commit**

```bash
git add main/probe_frame.h main/probe_frame.c main/probe.c \
        tools/probe_audit/probe_dump.c tools/probe_audit/tests/test_probe_frame.py
git commit -m "feat(probe): optional directed SSID in probe_build_request

Two-piece compose (SSID element + tail-after-placeholder). Wildcard path
(ssid=NULL) byte-identical to before; pinned by the fixture test."
```

---

### Task 3: Per-persona SSID assignment + per-burst selector

**Files:**
- Modify: `main/probe_agents.h:14-25` (struct fields) and add the selector declaration
- Modify: `main/probe_agents.c` (assign at birth sites; add selector; constants)
- Modify: `tools/probe_audit/probe_dump.c` (A-record wildcard field + `--ssidburst`, `--ssidstable` modes)
- Test: `tools/probe_audit/tests/test_probe_agents.py` (retune one test) and add `tools/probe_audit/tests/test_ssid_assign.py` (create)

**Interfaces:**
- Consumes: `ssid_pool_pick_weighted()`, `ssid_pool_at()`, `SSID_POOL_MAX_LEN` (from `ssid_pool.h`); `esp_random()`.
- Produces:
  - `probe_agent_t` gains `uint8_t ssid_n;` (0 = wildcard-only for life) and `uint8_t ssid_idx[AGENT_SSID_MAX];` with `#define AGENT_SSID_MAX 3`.
  - `const char *probe_agent_pick_ssid(const probe_agent_t *a, uint8_t *len_out);` — returns a pool string (sets `*len_out`) for a NAMED burst, or `NULL`/`*len_out=0` for a wildcard burst. Unassigned agents (`ssid_n==0`) always return `NULL`.

- [ ] **Step 1: Add struct fields + selector declaration to `main/probe_agents.h`**

Add the include and constant near the top (after line 4 `#include "probe_frame.h"`):
```c
#include "ssid_pool.h"
#define AGENT_SSID_MAX 3        // a real phone's active saved-network set is small
```

Add two fields to `probe_agent_t` (after the `persona_gen` field, before the closing brace at line 25):
```c
    uint8_t      ssid_n;                 // # assigned named SSIDs; 0 = wildcard-only for this life
    uint8_t      ssid_idx[AGENT_SSID_MAX];  // indices into ssid_pool (assigned once per life)
```

Add the selector declaration at the end of the file (after `probe_agent_sync`):
```c
// Choose this burst's SSID for agent a: a pool string (sets *len_out) to probe a NAMED network, or
// NULL (*len_out=0) for a wildcard burst. Agents with ssid_n==0 always return NULL. Pure; uses
// esp_random for the per-burst wildcard-vs-named roll. Does not mutate the agent.
const char *probe_agent_pick_ssid(const probe_agent_t *a, uint8_t *len_out);
```

- [ ] **Step 2: Add constants + assignment helper + selector to `main/probe_agents.c`**

Add constants near the existing `#define`s (after line 12):
```c
#define SSID_ASSIGN_PCT      62   // % of personas that get a named-SSID set (rest wildcard for life)
#define SSID_BURST_NAMED_PCT 60   // for an assigned persona, % of bursts that name a network (on-air realism)
```

Add the include at the top (after `#include "esp_random.h"`):
```c
#include "ssid_pool.h"
```

Add the assignment helper (after `persona_mac_rotate_base` at line 18):
```c
// Draw this persona's saved-network set ONCE (called only from birth sites). ~SSID_ASSIGN_PCT of
// personas get 1..AGENT_SSID_MAX distinct pool entries; the rest stay wildcard-only for their life.
static void assign_ssids(probe_agent_t *a)
{
    a->ssid_n = 0;
    if ((esp_random() % 100u) >= SSID_ASSIGN_PCT) return;         // wildcard-only persona
    int want = 1 + (int)(esp_random() % (uint32_t)AGENT_SSID_MAX);
    for (int tries = 0; tries < 16 && a->ssid_n < want; tries++) {
        int idx = ssid_pool_pick_weighted();
        int dup = 0;
        for (int j = 0; j < a->ssid_n; j++) if (a->ssid_idx[j] == (uint8_t)idx) dup = 1;
        if (!dup) a->ssid_idx[a->ssid_n++] = (uint8_t)idx;
    }
}
```

- [ ] **Step 3: Call the helper at both birth sites**

In `agent_spawn` (add before its closing brace, after line 32 `a->next_mac_rotate_ms = ...`):
```c
    assign_ssids(a);
```

In `probe_agent_sync` (add before `return 1;` at line 118, after `a->next_mac_rotate_ms = ...`):
```c
    assign_ssids(a);
```

(Do NOT add it to the MAC-rotation branch in `probe_agents_lifecycle` — assignment is per-life, not per-rotation.)

- [ ] **Step 4: Add the per-burst selector to `main/probe_agents.c`**

Add at the end of the file:
```c
const char *probe_agent_pick_ssid(const probe_agent_t *a, uint8_t *len_out)
{
    if (len_out) *len_out = 0;
    if (a->ssid_n == 0) return 0;                                // wildcard-only persona
    if ((esp_random() % 100u) >= SSID_BURST_NAMED_PCT) return 0; // this burst is wildcard
    uint8_t which = (uint8_t)(esp_random() % a->ssid_n);
    return ssid_pool_at(a->ssid_idx[which], len_out);            // one of its OWN assigned names
}
```

- [ ] **Step 5: Update `probe_dump.c` — A-record wildcard field reflects assignment**

In the `--agents` mode, the A-record's 3rd field is currently hardcoded `1`. Change BOTH occurrences (the initial-population loop ~line 119 and the reincarnation loop ~line 130) from:
```c
            printf("A %d %u 1 ", (int)a->arch, (unsigned)a->born_ms);
```
to:
```c
            printf("A %d %u %d ", (int)a->arch, (unsigned)a->born_ms, (a->ssid_n == 0) ? 1 : 0);
```

- [ ] **Step 6: Add `--ssidburst` and `--ssidstable` dump modes to `probe_dump.c`**

Add near the other modes (e.g. before the `--pick` block):
```c
    if (argc > 1 && strcmp(argv[1], "--ssidburst") == 0) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int n         = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 16;
        int bursts    = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 50;
        srand(seed);
        probe_agents_init(n, 0);
        for (int i = 0; i < probe_agents_count(); i++) {
            const probe_agent_t *a = probe_agents_at(i);
            int named = 0; uint8_t L;
            for (int b = 0; b < bursts; b++) if (probe_agent_pick_ssid(a, &L)) named++;
            printf("%d %d %d\n", i, (int)a->ssid_n, named);   // agent, assigned count, # named of `bursts`
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--ssidstable") == 0) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int n         = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 16;
        srand(seed);
        probe_agents_init(n, 0);
        for (int i = 0; i < n; i++) probe_agent_sync(i, probe_pick_archetype(), 0, 2400000u, 1); // 40min bound
        for (int phase = 0; phase < 2; phase++) {
            if (phase == 1) probe_agents_lifecycle(600000u);  // 10 min: past the 8-15min rotation floor
            for (int i = 0; i < probe_agents_count(); i++) {
                const probe_agent_t *a = probe_agents_at(i);
                printf("%c %d %d", phase ? 'A' : 'B', i, (int)a->ssid_n);
                for (int j = 0; j < a->ssid_n; j++) printf(" %d", (int)a->ssid_idx[j]);
                printf(" ");
                for (int b = 0; b < 6; b++) printf("%02x", a->mac[b]);
                printf("\n");
            }
        }
        return 0;
    }
```

- [ ] **Step 7: Retune the now-stale wildcard test in `tools/probe_audit/tests/test_probe_agents.py`**

Replace `test_wildcard_flag_is_one_today` (lines 83-85) with:
```python
    def test_wildcard_flag_reflects_assignment(self):
        # ~SSID_ASSIGN_PCT (62%) of identities get a named set -> ~38% stay wildcard (field==1).
        recs = births(3)
        wc = sum(1 for r in recs if r[2] == 1) / len(recs)
        self.assertGreater(wc, 0.28, "far too many named (assignment rate too high)")
        self.assertLess(wc, 0.50, "far too few named (assignment rate too low)")
        self.assertTrue(all(r[2] in (0, 1) for r in recs), "wildcard field not 0/1")
```

- [ ] **Step 8: Write the failing assignment test `tools/probe_audit/tests/test_ssid_assign.py`**

```python
import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")
AGENT_SSID_MAX = 3


def burst(seed, n=16, bursts=60):
    out = subprocess.check_output([EXE, "--ssidburst", str(seed), str(n), str(bursts)], text=True)
    return [tuple(int(x) for x in ln.split()) for ln in out.splitlines()]   # (agent, ssid_n, named)


def stable(seed, n=16):
    out = subprocess.check_output([EXE, "--ssidstable", str(seed), str(n)], text=True)
    before, after = {}, {}
    for ln in out.splitlines():
        p = ln.split()
        tag, i, ssid_n = p[0], int(p[1]), int(p[2])
        idx = tuple(int(x) for x in p[3:3 + ssid_n])
        mac = p[3 + ssid_n]
        (before if tag == "B" else after)[i] = (ssid_n, idx, mac)
    return before, after


def pool_count():
    return int(subprocess.check_output([EXE, "--ssidpool"], text=True).splitlines()[0])


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class SsidAssign(unittest.TestCase):
    def test_assignment_fraction_near_calibration(self):
        # Pool many agents across seeds; ~62% should have ssid_n>0.
        rows = [r for s in range(1, 9) for r in burst(s)]
        frac = sum(1 for _, ssid_n, _ in rows if ssid_n > 0) / len(rows)
        self.assertGreater(frac, 0.50, f"assigned fraction {frac:.2f} too low")
        self.assertLess(frac, 0.75, f"assigned fraction {frac:.2f} too high")

    def test_assigned_count_bounded(self):
        for _, ssid_n, _ in burst(2):
            self.assertGreaterEqual(ssid_n, 0)
            self.assertLessEqual(ssid_n, AGENT_SSID_MAX)

    def test_assigned_indices_valid_and_distinct(self):
        pc = pool_count()
        before, _ = stable(5)
        checked = 0
        for i, (ssid_n, idx, _mac) in before.items():
            self.assertEqual(len(idx), ssid_n, f"agent {i} idx count mismatch")
            self.assertEqual(len(set(idx)), len(idx), f"agent {i} has duplicate SSID indices")
            for x in idx:
                self.assertTrue(0 <= x < pc, f"agent {i} index {x} out of pool range")
            checked += ssid_n
        self.assertGreater(checked, 0, "no assigned indices to validate")

    def test_unassigned_never_names_assigned_sometimes(self):
        rows = burst(4, bursts=80)
        for _, ssid_n, named in rows:
            if ssid_n == 0:
                self.assertEqual(named, 0, "wildcard-only agent emitted a named probe")
        assigned = [named for _, ssid_n, named in rows if ssid_n > 0]
        self.assertTrue(assigned, "no assigned agents to check")
        self.assertTrue(any(nm > 0 for nm in assigned), "assigned agents never named over 80 bursts")

    def test_assignment_stable_across_mac_rotation(self):
        before, after = stable(3)
        rotated = 0
        for i, (bn, bidx, bmac) in before.items():
            an, aidx, amac = after[i]
            self.assertEqual((an, aidx), (bn, bidx), f"agent {i} SSID set changed on MAC rotation")
            if amac != bmac:
                rotated += 1
        self.assertGreater(rotated, 0, "no agent rotated its MAC (test exercised nothing)")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 9: Run to verify the new/retuned tests fail**

Run:
```
"C:/Program Files/Python312/python.exe" -m unittest tools.probe_audit.tests.test_ssid_assign tools.probe_audit.tests.test_probe_agents -v
```
Expected: FAIL (the `--ssidburst`/`--ssidstable` modes and struct fields don't exist until this task's code is built).

- [ ] **Step 10: Rebuild and run the full suite**

Run:
```
tools\probe_audit\run.ps1 -Rebuild
```
Expected: all green — `test_ssid_assign.py` (4), retuned `test_probe_agents.py`, and every pre-existing test.

- [ ] **Step 11: Commit**

```bash
git add main/probe_agents.h main/probe_agents.c tools/probe_audit/probe_dump.c \
        tools/probe_audit/tests/test_probe_agents.py tools/probe_audit/tests/test_ssid_assign.py
git commit -m "feat(probe): per-persona SSID assignment + per-burst selector

~62% of personas draw a small distinct SSID set at each identity birth
(never on MAC rotation); pure per-burst selector interleaves wildcard/named.
A-record wildcard field now reflects assignment."
```

---

### Task 4: Live wiring + calibration verification

**Files:**
- Modify: `main/probe.c:117-120` (pass the per-burst SSID into the builder)
- Test: `tools/probe_audit/tests/test_probe_agents.py` (add the decoy-side wildcard-fraction assertion) — verifies the headline axis moved

**Interfaces:**
- Consumes: `probe_agent_pick_ssid` (Task 3), the new `probe_build_request` signature (Task 2), and `decoy_profile_from_agents` (existing, `probe_behavior_scorecard.py`).
- Produces: the shipped on-air behavior (assigned agents emit named probes) and a committed assertion that the modeled decoy `wildcard_fraction` is in the target band.

- [ ] **Step 1: Write the failing decoy-side calibration test**

Add to `tools/probe_audit/tests/test_probe_agents.py` (it already imports `os, subprocess`; add `sys` import at top if not present). Append this class:

```python
class WildcardCalibration(unittest.TestCase):
    def test_decoy_wildcard_fraction_in_target_band(self):
        # The whole point of the feature: device-level wildcard fraction drops from 1.0 toward the
        # measured real anchor (~0.36). Uses the same profiler the scorecard uses.
        import importlib.util
        scorecard = os.path.join(TOOL, "probe_behavior_scorecard.py")
        spec = importlib.util.spec_from_file_location("pbs", scorecard)
        S = importlib.util.module_from_spec(spec); spec.loader.exec_module(S)
        rows = S.run_decoy_model(EXE, 1, 16, 2220, 1000)
        wf = S.decoy_profile_from_agents(rows)["wildcard_fraction"]
        self.assertGreater(wf, 0.28, f"wildcard_fraction {wf:.3f} below band (too many named)")
        self.assertLess(wf, 0.50, f"wildcard_fraction {wf:.3f} above band (too few named)")
```

- [ ] **Step 2: Run to verify it passes already (assignment landed in Task 3)**

Run:
```
"C:/Program Files/Python312/python.exe" -m unittest tools.probe_audit.tests.test_probe_agents.WildcardCalibration -v
```
Expected: PASS — the A-record wildcard field already reflects assignment from Task 3, so the modeled fraction is ~0.38. (This test guards against future regressions of the calibration constant.) If it FAILS, the assignment rate is miscalibrated — revisit `SSID_ASSIGN_PCT`.

- [ ] **Step 3: Wire the per-burst SSID into the live injector `main/probe.c`**

Replace the build call in `probe_inject_burst` (lines 117-120) with:

```c
    for (int i = 0; i < nd; i++) {
        uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
        uint8_t slen = 0;
        const char *ssid = probe_agent_pick_ssid(due[i], &slen);   // per-burst wildcard-vs-named
        if (probe_build_request(due[i]->mac, channel, due[i]->arch, band5, ssid, slen, f, &n) != 0)
            continue;                                              // archetype lacks this band (defensive)
```

- [ ] **Step 4: Firmware compile-verify (matches current root sdkconfig)**

First confirm the current target, then build (no `set-target` — the root sdkconfig is esp32c5):
```
python -c "import re;print([l for l in open('sdkconfig') if 'IDF_TARGET=' in l])"
idf.py -B build_c5 build
```
Expected: the sdkconfig line shows `CONFIG_IDF_TARGET="esp32c5"`; the build completes with no errors (probe.c, probe_frame.c, probe_agents.c, ssid_pool.c all compile and link). If the root sdkconfig is esp32c6 instead, use `idf.py -B build_c6 build` — the sources are target-independent.

- [ ] **Step 5: Run the full host suite once more**

Run:
```
tools\probe_audit\run.ps1 -Rebuild
```
Expected: entire `tools/probe_audit` suite green.

- [ ] **Step 6: Manual calibration check against the private capture (verification, not a committed test)**

The real reference capture lives in `private/` (not in-repo). Run the scorecard against the cleanest capture to confirm the headline `wildcard_fraction` axis collapsed from ~0.64 toward ~0.0:
```
"C:/Program Files/Python312/python.exe" tools/probe_audit/probe_behavior_scorecard.py <path-to-clean.kismet> --agents 16
```
Expected: the `wildcard_fraction` axis is now small (decoy ~0.38 vs real ~0.36 → |diff| ~0.02), where it was ~0.64 before. Record the printed number in the PR/handoff notes. (If no capture is available this session, note it as deferred — the committed decoy-side band test in Step 1 already guards the decoy behavior.)

- [ ] **Step 7: Commit**

```bash
git add main/probe.c tools/probe_audit/tests/test_probe_agents.py
git commit -m "feat(probe): inject per-burst directed SSIDs on the live path

Assigned personas now probe named public SSIDs on-air; decoy wildcard
fraction ~0.38 (was 1.0), matching the measured real-crowd anchor ~0.36.
Firmware compile-verified on build_c5."
```

---

## Post-plan: finishing

After all four tasks are green, use **superpowers:finishing-a-development-branch**: verify the full host suite passes, then present merge options. On merge to `main`: `--no-ff`, PII-scan the diff (no OS username / real name / real MAC in tracked files), and push (authorized this session, same discipline as prior pushes). Update `private/PROJECT-MAP.md` §11 and the `decoy-audit-tool` / `probe-antifingerprint-stance` memories noting: directed-SSID realism shipped, decoy wildcard fraction 1.0→~0.38 (real anchor ~0.36), on-air HW validation of the named probes deferred to the user's next Kismet capture.
