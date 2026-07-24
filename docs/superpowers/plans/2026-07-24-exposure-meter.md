# Exposure Meter (interactive Wi-Fi, CYD) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A modal CYD view that identifies the user's phone by its Wi-Fi-toggle probe burst and reveals the named networks it leaks.

**Architecture:** A pure, host-tested exposure state machine (`exposure.c`) groups sniffed probes by an opaque fingerprint and picks the device that *spikes* after the toggle; a CYD promiscuous sniffer computes the fingerprint + SSID from raw probe frames and feeds it; a new `RADAR_VIEW_EXPOSURE` renders the guided flow; entering/leaving switches the CYD radio between promiscuous-hop and the ESP-NOW fleet link.

**Tech Stack:** C99 (ESP-IDF, CYD = classic esp32 / IDF 5.4); MSVC `cl` host tests via `tools/radar_audit`; Python 3.12 unittest.

## Global Constraints

- **v1 = Wi-Fi only**, **ephemeral** (no persistence of anything about the user's devices).
- **Modal exclusive mode:** entering EXPOSURE suspends the ESP-NOW request/broadcast loop and goes promiscuous+hop {1,6,11}; exiting restores channel 1 + ESP-NOW.
- **Identification is by post-toggle spike, not raw volume** (the busiest baseline device must not win unless it spikes).
- **Fingerprint is computed in the CYD glue** (MAC- and SSID-independent) and passed to `exposure.c` as a `uint32_t`; the state machine never parses frames.
- **Timings:** `EXPO_BASELINE_MS=4000`, `EXPO_WATCH_MS=6000`, min burst margin `EXPO_MIN_SPIKE=3`.
- **Caps:** `EXPO_MAX_DEVICES=24`, `EXPO_MAX_SSIDS=8`, SSID ≤ 32 bytes.
- **Commit trailers** on every commit:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
  ```
- **No PII in tracked files.** Never write files with PowerShell `>` (UTF-16/BOM); use the Write tool or `Set-Content -Encoding ascii`.

---

### Task 1: Pure exposure state machine + host tests

**Files:**
- Create: `components/simulacra_radar/exposure.h`
- Create: `components/simulacra_radar/exposure.c`
- Create: `tools/radar_audit/expo_dump.c`
- Modify: `tools/radar_audit/run.ps1` (build `expo_dump.exe`)
- Test: `tools/radar_audit/tests/test_exposure.py`

**Interfaces:**
- Produces: the `exposure_t` API below; the render (Task 2) reads results via the accessors; the CYD glue (Task 3) drives it with `expo_start`/`expo_probe`/`expo_tick`.

- [ ] **Step 1: Write `components/simulacra_radar/exposure.h`**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define EXPO_BASELINE_MS 4000u
#define EXPO_WATCH_MS    6000u
#define EXPO_MIN_SPIKE   3        // min extra probes over baseline expectation to count as "your" burst
#define EXPO_MAX_DEVICES 24
#define EXPO_MAX_SSIDS   8
#define EXPO_SSID_MAX    32

typedef enum { EXPO_IDLE, EXPO_BASELINE, EXPO_WATCH, EXPO_RESULT } expo_state_t;

typedef struct {
    uint32_t fp;
    uint16_t base_n, watch_n;                       // probe counts per phase
    uint8_t  nssid;
    char     ssid[EXPO_MAX_SSIDS][EXPO_SSID_MAX + 1];
} expo_dev_t;

typedef struct {
    expo_state_t state;
    uint32_t     t_phase;                           // ms at entry of the current timed phase
    int          ndev;
    expo_dev_t   dev[EXPO_MAX_DEVICES];
    int          winner;                            // index into dev[], -1 if none/ambiguous
} exposure_t;

void expo_reset(exposure_t *e);                     // -> IDLE, cleared
void expo_start(exposure_t *e, uint32_t now_ms);    // IDLE/RESULT -> BASELINE
// Feed one sniffed probe. ssid NULL/len 0 = wildcard. Ignored unless BASELINE or WATCH.
void expo_probe(exposure_t *e, uint32_t fp, const char *ssid, uint8_t ssid_len, uint32_t now_ms);
void expo_tick(exposure_t *e, uint32_t now_ms);     // advances BASELINE->WATCH->RESULT on the timers

bool     expo_have_result(const exposure_t *e);     // state == RESULT
bool     expo_ambiguous(const exposure_t *e);       // RESULT but no clear spike (winner < 0)
uint32_t expo_winner_fp(const exposure_t *e);       // 0 if none
int      expo_winner_probes(const exposure_t *e);   // winner watch_n, 0 if none
int      expo_winner_ssids(const exposure_t *e, const char **out, int max);  // # named SSIDs, fills out[]
```

- [ ] **Step 2: Write the failing test `tools/radar_audit/tests/test_exposure.py`**

```python
import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "expo_dump.exe" if os.name == "nt" else "expo_dump")


def run(script):
    """Feed a scripted event stream; return the final result line dict.
    Script commands (one per line): start <ms> | probe <fp> <ms> [ssid] | tick <ms>.
    Output (last line): RESULT state=<s> winner_fp=<hex> probes=<n> ambiguous=<0/1> ssids=a,b,c"""
    out = subprocess.run([EXE], input=script, capture_output=True, text=True).stdout.strip().splitlines()
    last = out[-1]
    d = dict(tok.split("=", 1) for tok in last.split()[1:])
    d["ssids"] = [s for s in d.get("ssids", "").split(",") if s]
    return d


@unittest.skipUnless(os.path.exists(EXE), "expo_dump not built")
class Exposure(unittest.TestCase):
    def test_toggle_burst_device_wins(self):
        # dev 0xAA probes steadily; dev 0xBB (the phone) is quiet in baseline then bursts after toggle.
        s = "start 0\n"
        s += "".join(f"probe 0xAA {t}\n" for t in range(0, 4000, 500))       # baseline: AA active
        s += "probe 0xBB 100\n"                                              # BB barely seen in baseline
        s += "tick 4001\n"                                                   # -> WATCH
        s += "".join(f"probe 0xBB {t} HomeWiFi\n" for t in range(4100, 5100, 100))  # BB bursts, names a net
        s += "probe 0xAA 4600\n"                                             # AA trickles (no spike)
        s += "tick 10002\n"                                                  # -> RESULT
        d = run(s)
        self.assertEqual(d["state"], "RESULT")
        self.assertEqual(d["ambiguous"], "0")
        self.assertEqual(d["winner_fp"].lower(), "0xbb")
        self.assertIn("HomeWiFi", d["ssids"])

    def test_busiest_baseline_does_not_win_without_spike(self):
        s = "start 0\n" + "".join(f"probe 0xAA {t}\n" for t in range(0, 4000, 200))  # AA very busy baseline
        s += "tick 4001\n" + "probe 0xAA 5000\n" + "tick 10002\n"                     # AA no watch spike
        self.assertEqual(run(s)["ambiguous"], "1", "no post-toggle spike -> ambiguous")

    def test_wildcard_phone_identified_with_empty_ssids(self):
        s = "start 0\nprobe 0xBB 100\ntick 4001\n"
        s += "".join(f"probe 0xBB {t}\n" for t in range(4100, 5100, 100))            # burst, no SSID
        s += "tick 10002\n"
        d = run(s)
        self.assertEqual(d["winner_fp"].lower(), "0xbb")
        self.assertEqual(d["ssids"], [])

    def test_ssids_deduped(self):
        s = "start 0\nprobe 0xBB 100\ntick 4001\n"
        s += "".join(f"probe 0xBB {t} CoffeeShop\n" for t in range(4100, 4600, 100))  # same SSID repeated
        s += "tick 10002\n"
        self.assertEqual(run(s)["ssids"], ["CoffeeShop"])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 3: Run the test, expect failure**

Run: `"C:/Program Files/Python312/python.exe" -m unittest discover -s tools/radar_audit/tests -p test_exposure.py -v`
Expected: skipped/failed — `expo_dump.exe` not built yet.

- [ ] **Step 4: Write `components/simulacra_radar/exposure.c`**

```c
#include "exposure.h"
#include <string.h>

static expo_dev_t *find_or_add(exposure_t *e, uint32_t fp)
{
    for (int i = 0; i < e->ndev; i++) if (e->dev[i].fp == fp) return &e->dev[i];
    if (e->ndev >= EXPO_MAX_DEVICES) return 0;
    expo_dev_t *d = &e->dev[e->ndev++];
    memset(d, 0, sizeof *d);
    d->fp = fp;
    return d;
}

void expo_reset(exposure_t *e) { memset(e, 0, sizeof *e); e->state = EXPO_IDLE; e->winner = -1; }

void expo_start(exposure_t *e, uint32_t now_ms)
{
    memset(e, 0, sizeof *e);
    e->state = EXPO_BASELINE; e->t_phase = now_ms; e->winner = -1;
}

void expo_probe(exposure_t *e, uint32_t fp, const char *ssid, uint8_t ssid_len, uint32_t now_ms)
{
    (void)now_ms;
    if (e->state != EXPO_BASELINE && e->state != EXPO_WATCH) return;
    expo_dev_t *d = find_or_add(e, fp);
    if (!d) return;
    if (e->state == EXPO_BASELINE) { if (d->base_n  < 0xFFFF) d->base_n++;  }
    else                           { if (d->watch_n < 0xFFFF) d->watch_n++; }
    if (ssid && ssid_len && ssid_len <= EXPO_SSID_MAX && d->nssid < EXPO_MAX_SSIDS) {
        for (int i = 0; i < d->nssid; i++)                       // dedup
            if (strncmp(d->ssid[i], ssid, ssid_len) == 0 && d->ssid[i][ssid_len] == 0) return;
        memcpy(d->ssid[d->nssid], ssid, ssid_len);
        d->ssid[d->nssid][ssid_len] = 0;
        d->nssid++;
    }
}

static void resolve(exposure_t *e)
{
    // spike = watch_n - baseline expectation (baseline rate scaled to the watch window).
    // ratio = WATCH_MS / BASELINE_MS.  Pick the max positive spike >= EXPO_MIN_SPIKE.
    int best = -1, best_spike = 0;
    for (int i = 0; i < e->ndev; i++) {
        int expected = (int)((long)e->dev[i].base_n * EXPO_WATCH_MS / EXPO_BASELINE_MS);
        int spike = (int)e->dev[i].watch_n - expected;
        if (spike > best_spike) { best_spike = spike; best = i; }
    }
    e->winner = (best >= 0 && best_spike >= EXPO_MIN_SPIKE) ? best : -1;
}

void expo_tick(exposure_t *e, uint32_t now_ms)
{
    if (e->state == EXPO_BASELINE && (now_ms - e->t_phase) >= EXPO_BASELINE_MS) {
        e->state = EXPO_WATCH; e->t_phase = now_ms;
    } else if (e->state == EXPO_WATCH && (now_ms - e->t_phase) >= EXPO_WATCH_MS) {
        resolve(e); e->state = EXPO_RESULT;
    }
}

bool     expo_have_result(const exposure_t *e) { return e->state == EXPO_RESULT; }
bool     expo_ambiguous(const exposure_t *e)   { return e->state == EXPO_RESULT && e->winner < 0; }
uint32_t expo_winner_fp(const exposure_t *e)   { return e->winner >= 0 ? e->dev[e->winner].fp : 0; }
int      expo_winner_probes(const exposure_t *e){ return e->winner >= 0 ? e->dev[e->winner].watch_n : 0; }

int expo_winner_ssids(const exposure_t *e, const char **out, int max)
{
    if (e->winner < 0) return 0;
    const expo_dev_t *d = &e->dev[e->winner];
    int n = d->nssid < max ? d->nssid : max;
    for (int i = 0; i < n; i++) out[i] = d->ssid[i];
    return n;
}
```

- [ ] **Step 5: Write the host harness `tools/radar_audit/expo_dump.c`**

```c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "exposure.h"

// Reads a scripted event stream on stdin and prints the state after each command, then a RESULT line.
//   start <ms> | probe <fp_hex> <ms> [ssid] | tick <ms>
int main(void)
{
    exposure_t e; expo_reset(&e);
    char line[128], cmd[16], ssid[64];
    unsigned long ms; unsigned fp;
    while (fgets(line, sizeof line, stdin)) {
        if (sscanf(line, "%15s", cmd) != 1) continue;
        if      (!strcmp(cmd, "start") && sscanf(line, "%*s %lu", &ms) == 1) expo_start(&e, (uint32_t)ms);
        else if (!strcmp(cmd, "tick")  && sscanf(line, "%*s %lu", &ms) == 1) expo_tick(&e, (uint32_t)ms);
        else if (!strcmp(cmd, "probe")) {
            int got = sscanf(line, "%*s %x %lu %63s", &fp, &ms, ssid);
            if (got >= 2) expo_probe(&e, (uint32_t)fp, got == 3 ? ssid : 0,
                                     got == 3 ? (uint8_t)strlen(ssid) : 0, (uint32_t)ms);
        }
    }
    const char *S[] = { "IDLE", "BASELINE", "WATCH", "RESULT" };
    const char *sl[EXPO_MAX_SSIDS]; int ns = expo_winner_ssids(&e, sl, EXPO_MAX_SSIDS);
    printf("RESULT state=%s winner_fp=0x%02x probes=%d ambiguous=%d ssids=",
           S[e.state], expo_winner_fp(&e), expo_winner_probes(&e), expo_ambiguous(&e) ? 1 : 0);
    for (int i = 0; i < ns; i++) printf("%s%s", i ? "," : "", sl[i]);
    printf("\n");
    return 0;
}
```

- [ ] **Step 6: Add `expo_dump.exe` to `tools/radar_audit/run.ps1`**

After the `render_dump.exe` build line, add:
```powershell
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /I $rad `
   (Join-Path $tool "expo_dump.c") (Join-Path $rad "exposure.c") /Fe:(Join-Path $tool "expo_dump.exe") | Out-Null
```

- [ ] **Step 7: Build + run the tests, expect pass**

Run: `tools\radar_audit\run.ps1 -Rebuild` (or just `run.ps1`)
Expected: `test_exposure.py` all pass (toggle-burst wins, busiest-baseline-without-spike is ambiguous, wildcard identified with empty ssids, ssids deduped); the rest of the radar_audit suite stays green.

- [ ] **Step 8: Commit**

```bash
git add components/simulacra_radar/exposure.h components/simulacra_radar/exposure.c \
        tools/radar_audit/expo_dump.c tools/radar_audit/run.ps1 tools/radar_audit/tests/test_exposure.py
git commit -m "feat(exposure): pure exposure-session state machine + host tests

Groups sniffed probes by opaque fingerprint; identifies the device that spikes
after the Wi-Fi toggle (not raw volume) and collects its leaked SSIDs."
```

Also add `exposure.c` to the firmware build: **Modify `components/simulacra_radar/CMakeLists.txt`** SRCS to include `exposure.c` (so both decoy and CYD builds compile it). Commit that with the above.

---

### Task 2: EXPOSURE view render + HOME grid reflow (6→7)

**Files:**
- Modify: `components/simulacra_radar/radar_ui.h` (add `RADAR_VIEW_EXPOSURE` before `RADAR_VIEW_COUNT`)
- Modify: `components/simulacra_radar/radar_render.c` (add `draw_exposure`; reflow the HOME sigil grid to 7 tiles; dispatch)
- Modify: `tools/radar_audit/render_dump.c` (let the harness render EXPOSURE with a synthetic result)
- Test: `tools/radar_audit/tests/test_radar_render.py` (EXPOSURE renders each step)

**Interfaces:**
- Consumes: `exposure_t` + accessors (Task 1).
- Produces: `RADAR_VIEW_EXPOSURE`; `radar_render_view` gains an `exposure` pointer parameter for that view (NULL for other displays), mirroring how `lib`/`ctrl` are passed.

- [ ] **Step 1: Add the view enum value**

In `components/simulacra_radar/radar_ui.h`, change the enum to insert `RADAR_VIEW_EXPOSURE` before `RADAR_VIEW_COUNT`:
```c
typedef enum { RADAR_VIEW_HOME = 0, RADAR_VIEW_RADAR, RADAR_VIEW_DETAIL, RADAR_VIEW_STATS,
               RADAR_VIEW_LIBRARY, RADAR_VIEW_CONTROL, RADAR_VIEW_INFO, RADAR_VIEW_EXPOSURE,
               RADAR_VIEW_COUNT } radar_view_t;
```

- [ ] **Step 2: Write the failing render test**

Add to `tools/radar_audit/tests/test_radar_render.py` (EXPOSURE view id = 7). Add a helper + tests:
```python
def exposure(step, fp=0xbb, probes=5, ssids="HomeWiFi,CoffeeShop", ambiguous=0):
    # render_dump --expo <step 0=idle 1=baseline 2=watch 3=result> <fp> <probes> <ambiguous> <ssids csv>
    out = subprocess.check_output([EXE, "--expo", str(step), hex(fp), str(probes),
                                   str(ambiguous), ssids], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class ExposureView(unittest.TestCase):
    def test_idle_prompts_scan(self):
        self.assertTrue(any("SCAN" in t.upper() for t in exposure(0)))

    def test_watch_prompts_toggle(self):
        self.assertTrue(any("TOGGLE" in t.upper() or "WI-FI" in t.upper() for t in exposure(2)))

    def test_result_shows_leaked_ssids(self):
        t = exposure(3, ssids="HomeWiFi,CoffeeShop")
        self.assertTrue(any("HomeWiFi" in x for x in t))
        self.assertTrue(any("CoffeeShop" in x for x in t))

    def test_ambiguous_result_says_so(self):
        self.assertTrue(any("AGAIN" in x.upper() or "NO " in x.upper() for x in exposure(3, ambiguous=1)))
```

- [ ] **Step 3: Run it, expect failure**

Run: `"C:/Program Files/Python312/python.exe" -m unittest tools.radar_audit.tests.test_radar_render.ExposureView -v`
Expected: FAIL — no `--expo` mode / no `draw_exposure` yet.

- [ ] **Step 4: Write `draw_exposure` and reflow the grid in `radar_render.c`**

Add `#include "exposure.h"` near the top. Add the renderer (uses the shared `draw_header` / `row_kv` primitives):
```c
static void draw_exposure(radar_gfx_t *g, const exposure_t *e){
    draw_header(g, "EXPOSURE");
    if(!e || e->state == EXPO_IDLE){
        radar_gfx_text(g, 24, 120, "TAP TO SCAN THE AIR", COL_BONE);
        radar_gfx_text(g, 24, 150, "see what your phone leaks", COL_ASH);
        return;
    }
    if(e->state == EXPO_BASELINE){
        radar_gfx_text(g, 24, 130, "listening...", COL_ARCANE);
        return;
    }
    if(e->state == EXPO_WATCH){
        radar_gfx_text(g, 16, 120, "TOGGLE YOUR PHONE'S", COL_BONE);
        radar_gfx_text(g, 16, 144, "WI-FI OFF, THEN ON", COL_BONE);
        radar_gfx_text(g, 16, 176, "watching for the burst", COL_ASH);
        return;
    }
    // RESULT
    if(expo_ambiguous(e)){
        radar_gfx_text(g, 24, 120, "no clear signal", COL_WARD);
        radar_gfx_text(g, 24, 150, "TAP TO TRY AGAIN", COL_BONE);
        return;
    }
    char l[40]; snprintf(l,sizeof l,"your phone: %d probes", expo_winner_probes(e));
    radar_gfx_text(g, 12, 36, l, COL_HUNTER);
    const char *ss[EXPO_MAX_SSIDS]; int n = expo_winner_ssids(e, ss, EXPO_MAX_SSIDS);
    if(n == 0){
        radar_gfx_text(g, 12, 70, "named no networks (good)", COL_CHANNEL);
        radar_gfx_text(g, 12, 94, "but still announced itself", COL_ASH);
    } else {
        radar_gfx_text(g, 12, 66, "it announced it knows:", COL_ASH);
        int y = 90;
        for(int i=0;i<n && y<300;i++){ radar_gfx_text(g, 20, y, ss[i], COL_BONE); y+=18; }
    }
}
```
Reflow the HOME grid from 6 to 7 tiles (48 px rows so 4 rows fit). Replace the `sig[6]`/`lbl[6]` arrays and the loop:
```c
    static const sigil_id_t sig[7]={SIGIL_CIRCLE,SIGIL_HUNTER,SIGIL_LIVING,SIGIL_RITE,SIGIL_WARD,SIGIL_GRIMOIRE,SIGIL_CIRCLE};
    static const char *lbl[7]={"RADAR","FOLLOWERS","DECOYS","CONTROL","LIBRARY","INFO","EXPOSURE"};
    for(int i=0;i<7;i++){
        int cx=(i%2)*120, cy=104+(i/2)*48;
        radar_gfx_fill_rect(g, cx+1, cy+1, 118, 46, COL_CRYPT);
        radar_sigil_draw(g, sig[i], cx+18, cy+23, 10, COL_ARCANE);
        radar_gfx_text(g, cx+36, cy+19, lbl[i], COL_BONE);
    }
```
(EXPOSURE reuses `SIGIL_CIRCLE` for v1 — a dedicated sigil is a later polish.) Add the dispatch: `radar_render_view` gains a `const exposure_t *expo` param; in the band loop add `else if(view==RADAR_VIEW_EXPOSURE) draw_exposure(&g, expo);`. Update `radar_render.h`'s signature and **every caller** (`cyd_main.c` render sites pass the live `exposure_t*` for EXPOSURE, else NULL; `render_dump.c` passes its synthetic one).

- [ ] **Step 5: Add `--expo` mode to `render_dump.c`**

Build a synthetic `exposure_t` from argv and render it. Near the other modes:
```c
    if (argc > 1 && strcmp(argv[1], "--expo") == 0) {
        int step = argc>2?atoi(argv[2]):0;
        exposure_t e; expo_reset(&e);
        // drive to the requested state deterministically
        if (step >= 1) { expo_start(&e, 0); }
        if (step >= 2) { expo_tick(&e, EXPO_BASELINE_MS + 1); }         // -> WATCH
        if (step >= 3) {
            unsigned fp = argc>3?(unsigned)strtoul(argv[3],0,16):0xbb;
            int probes = argc>4?atoi(argv[4]):5;
            int ambig  = argc>5?atoi(argv[5]):0;
            if (!ambig) for (int i=0;i<probes;i++) expo_probe(&e, fp, 0,0, EXPO_BASELINE_MS+100);
            if (!ambig && argc>6 && argv[6][0]) {                       // csv ssids
                char buf[256]; strncpy(buf,argv[6],sizeof buf-1); buf[sizeof buf-1]=0;
                for (char *t=strtok(buf,","); t; t=strtok(0,",")) expo_probe(&e, fp, t,(uint8_t)strlen(t), EXPO_BASELINE_MS+100);
            }
            expo_tick(&e, EXPO_BASELINE_MS + EXPO_WATCH_MS + 2);        // -> RESULT
        }
        static uint16_t band[240*320];
        radar_render_view(RADAR_VIEW_EXPOSURE, /*st*/0, 0,0, 0,0, /*expo*/&e, 0, band, 320,240,320, flush_noop, 0);
        return 0;
    }
```
Note: pass the harness's synthetic `exposure_t*` in the new `expo` slot of `radar_render_view`; other `render_dump` modes pass NULL there. Include `exposure.h`, and compile `exposure.c` into `render_dump.exe` (add it to the render_dump `cl` line in run.ps1).

- [ ] **Step 6: Build + run — EXPOSURE render tests pass, all prior render tests still pass**

Run: `tools\radar_audit\run.ps1 -Rebuild`
Expected: `ExposureView` tests pass; the existing render/posture/data-page tests stay green (the added `radar_render_view` param is NULL for them).

- [ ] **Step 7: Commit**

```bash
git add components/simulacra_radar/radar_ui.h components/simulacra_radar/radar_render.c \
        components/simulacra_radar/radar_render.h tools/radar_audit/render_dump.c \
        tools/radar_audit/tests/test_radar_render.py
git commit -m "feat(cyd): EXPOSURE view render + 7-tile HOME grid

Guided exposure UI (scan -> toggle prompt -> reveal leaked SSIDs / loudness);
HOME sigil grid reflowed to 4 rows for the 7th tile. radar_render_view gains an
exposure pointer (NULL for other displays)."
```

---

### Task 3: CYD promiscuous sniffer, fingerprint, modal radio switch, wiring

**Files:**
- Create: `cyd/main/expo_sniff.h`, `cyd/main/expo_sniff.c`
- Modify: `cyd/main/CMakeLists.txt` (add `expo_sniff.c`)
- Modify: `cyd/main/cyd_main.c` (own an `exposure_t`; enter/exit exposure mode; touch drives tap-to-scan; feed `expo_tick`; pass `&expo` to the EXPOSURE render)

**Interfaces:**
- Consumes: `exposure.h` API; the promiscuous pattern from `main/wifi_observe.c` (`WIFI_PKT_MGMT`, `f[0]==0x40` probe-req, `esp_wifi_set_promiscuous_filter/_rx_cb/(true)`).
- Produces:
  ```c
  void expo_sniff_start(exposure_t *e, uint32_t now_ms);   // promiscuous+hop on, expo_start(e)
  void expo_sniff_tick(uint32_t now_ms);                   // hop channels; call from the CYD loop
  void expo_sniff_stop(void);                              // promiscuous off, channel 1
  ```

- [ ] **Step 1: Write `cyd/main/expo_sniff.h`**

```c
#pragma once
#include <stdint.h>
#include "exposure.h"
void expo_sniff_start(exposure_t *e, uint32_t now_ms);
void expo_sniff_tick(uint32_t now_ms);
void expo_sniff_stop(void);
```

- [ ] **Step 2: Write `cyd/main/expo_sniff.c`**

```c
#include "expo_sniff.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "expo";
static exposure_t *s_e;
static const uint8_t CH[] = { 1, 6, 11 };
static int s_hop;
static uint32_t s_last_hop;

// FNV-1a over the probe's IE STRUCTURE: (id,len) pairs, excluding SSID (id 0) and DS param (id 3),
// and never the addresses -- so a device's probes group across MAC + SSID rotation.
static uint32_t fingerprint(const uint8_t *ies, int len)
{
    uint32_t h = 2166136261u;
    int i = 0;
    while (i + 2 <= len) {
        uint8_t id = ies[i], ln = ies[i + 1];
        if (id != 0x00 && id != 0x03) { h ^= id; h *= 16777619u; h ^= ln; h *= 16777619u; }
        i += 2 + ln;
    }
    return h;
}

static const uint8_t *ssid_of(const uint8_t *ies, int len, uint8_t *out_len)
{
    int i = 0;
    while (i + 2 <= len) {
        uint8_t id = ies[i], ln = ies[i + 1];
        if (id == 0x00) { *out_len = ln; return ln ? &ies[i + 2] : 0; }
        i += 2 + ln;
    }
    *out_len = 0; return 0;
}

static void rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT || !s_e) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *f = p->payload;
    if (f[0] != 0x40) return;                 // probe request
    int flen = p->rx_ctrl.sig_len;
    const uint8_t *ies = f + 24;              // after the 24-byte MAC header
    int ielen = flen - 24 - 4;                // minus FCS
    if (ielen < 2) return;
    uint32_t fp = fingerprint(ies, ielen);
    uint8_t sl; const uint8_t *ss = ssid_of(ies, ielen, &sl);
    char tmp[33]; if (ss && sl <= 32) { memcpy(tmp, ss, sl); tmp[sl] = 0; }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    expo_probe(s_e, fp, (ss && sl) ? tmp : 0, sl, now);
}

void expo_sniff_start(exposure_t *e, uint32_t now_ms)
{
    s_e = e; s_hop = 0; s_last_hop = now_ms;
    expo_start(e, now_ms);
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(rx_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(CH[0], WIFI_SECOND_CHAN_NONE);
    ESP_LOGW(TAG, "exposure sniff up");
}

void expo_sniff_tick(uint32_t now_ms)
{
    if (now_ms - s_last_hop >= 250) {         // hop ~4x/s to cover 1/6/11
        s_last_hop = now_ms;
        s_hop = (s_hop + 1) % (int)(sizeof CH);
        esp_wifi_set_channel(CH[s_hop], WIFI_SECOND_CHAN_NONE);
    }
}

void expo_sniff_stop(void)
{
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);   // back to the ESP-NOW channel
    s_e = 0;
    ESP_LOGW(TAG, "exposure sniff down");
}
```

- [ ] **Step 3: Wire into `cyd_main.c`**

Add `#include "expo_sniff.h"` and a file-scope `static exposure_t s_expo;` (call `expo_reset(&s_expo)` in setup). In the touch/input handler:
- Add `RADAR_VIEW_EXPOSURE` to the HOME `GRID[7]` (grow the array; map the 7th tile → `RADAR_VIEW_EXPOSURE`; update the hit-test rows to the 48 px pitch used in Task 2).
- On **entering** EXPOSURE (select-view): pause ESP-NOW requests (`s_paused_espnow = true;` guard the periodic `send_request()`), then `expo_sniff_start(&s_expo, now)`.
- On **leaving** EXPOSURE (any nav away / idle-home): `expo_sniff_stop()`, `expo_reset(&s_expo)`, resume ESP-NOW.
- While in EXPOSURE: each loop iteration call `expo_sniff_tick(now)` and `expo_tick(&s_expo, now)`; a tap while `state==EXPO_IDLE` or `EXPO_RESULT` (re-run) calls `expo_sniff_start(&s_expo, now)`.
- Pass `&s_expo` as the new `exposure` arg to `radar_render_view` when `ui.view==RADAR_VIEW_EXPOSURE`, else NULL.

(Exact edits are localized to the input `if (ui.view == RADAR_VIEW_HOME)` grid block, the per-view branch, and the render call — follow the existing CONTROL-view pattern for enter/leave bookkeeping.)

- [ ] **Step 4: Add `expo_sniff.c` to the CYD build**

`cyd/main/CMakeLists.txt` — add `"expo_sniff.c"` to `SRCS`.

- [ ] **Step 5: Compile-verify the CYD**

Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target cyd -Do build`
Expected: `Project build complete.` (exposure.c compiles into the CYD via the shared component; expo_sniff.c compiles; the render dispatch links). Fix any signature mismatches from the new `radar_render_view` parameter.

- [ ] **Step 6: Commit**

```bash
git add cyd/main/expo_sniff.h cyd/main/expo_sniff.c cyd/main/CMakeLists.txt cyd/main/cyd_main.c
git commit -m "feat(cyd): exposure promiscuous sniffer + modal wiring

Promiscuous probe-req sniff with a MAC/SSID-independent IE fingerprint, feeding
the exposure state machine; entering EXPOSURE suspends ESP-NOW and hops 1/6/11,
exiting restores channel 1 + the fleet link. CYD compile-verified."
```

---

### Task 4: On-bench interactive validation (with the user)

**Files:** none (validation).

- [ ] **Step 1: Flash the CYD**

`& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target cyd -Fleet -Port <COM> -Do all -ReadSeconds 6 -Grep "panel up|exposure"`
Expected: boots (`panel up`), fleet link normal on HOME.

- [ ] **Step 2: Run the exposure flow (user + a phone)**

On the CYD: tap the **EXPOSURE** tile → "TAP TO SCAN" → tap → wait for "TOGGLE YOUR WI-FI" → on a phone, turn Wi-Fi off then on → within the watch window the view resolves. Expected: it identifies the phone (a probe count) and shows either its leaked SSID names or "named no networks (good)". Re-run if it reads ambiguous.

- [ ] **Step 3: Confirm the fleet recovers on exit**

Tap back to HOME. Expected: `expo_sniff_stop` restores channel 1; within a couple seconds the CYD is back on the fleet link (HOME posture/roster live again). Read serial to confirm `exposure sniff down` + resumed `status rx`.

- [ ] **Step 4: Record the outcome** in `private/PROJECT-MAP.md` §11 (what phone, whether identified, SSIDs revealed or wildcard-only). No commit (private/ gitignored).

---

## Post-plan: finishing

After Task 4, use **superpowers:finishing-a-development-branch**: verify the full `tools/radar_audit` suite green, then present options. Work is on `main` (local) unless a branch was used; keep the standing push stance (PII-scan then push on the user's go). Note BLE exposure is the documented phase-2 follow-up.

## Self-review notes

- Spec coverage: exposure state machine (T1), CYD sniffer + fingerprint + modal switch (T3), EXPOSURE render + grid reflow (T2), on-bench flow (T4). All spec §Architecture units mapped.
- Placeholder scan: full code for exposure.{h,c}, expo_dump, expo_sniff.{h,c}, draw_exposure, render_dump --expo; the cyd_main wiring is described as localized edits against the existing CONTROL-view pattern (glue, not new logic).
- Consistency: `exposure_t`, `expo_start/probe/tick`, `expo_winner_ssids`, `RADAR_VIEW_EXPOSURE`, the new `radar_render_view` exposure param, and the `--expo` harness all agree across tasks; timings/caps come from the header `#define`s.
