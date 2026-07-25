# Flock/Raven BLE Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect Flock Safety / Raven surveillance gear over BLE (manufacturer company id `0x09C8`) and surface it on the CYD as distinct "surveillance presence," reusing the existing tracker-signature framework.

**Architecture:** Add one `SIG_CLASS_FLOCK` signature to `sig_seed.c` — because `detect_note_known` confirms a known-class hit on first sighting and the status wire already carries `category`/`class_id`/`best_rssi`, this lights up the whole decoy→ESP-NOW→CYD path with no `detect.c`/wire changes. The only new work is the CYD render treatment (a SURVEILLANCE section split out of the follower list + a HOME indicator) and host validation.

**Tech Stack:** C (ESP-IDF firmware + shared `simulacra_radar` component, host-compiled via MSVC `cl`), Python `unittest` host tests driving the `sig_scan` and `render_dump` harnesses.

## Global Constraints

- BLE signature = manufacturer company id **`0x09C8`** (2504, "XUNTONG"), matched company-id-only (`pat_len=0`), `category=SIG_CAT_CAMERA`, `class_id=SIG_CLASS_FLOCK`, `confidence=60`.
- Label the class exactly **`Flock`** (one class covers the Flock/Raven ecosystem; no Flock-vs-Raven split).
- Bump `SIG_SEED_VERSION` to **2** (seed content changed).
- **Do NOT modify** `detect.c`, `coexist.c`, or the `radar_wire_status_t` format — the existing path already carries everything needed.
- **Posture must not change:** `radar_posture` must not be made to flip to HUNTED on a camera. A camera gets the separate SURVEIL indicator only.
- Commit identity is the repo-local `Em3ritus` noreply. Every commit carries the trailers:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.

---

### Task 1: The Flock signature + host validation

**Files:**
- Modify: `components/simulacra_radar/threat_sig.h` (add `SIG_CLASS_FLOCK` to `sig_class_t`)
- Modify: `components/simulacra_radar/sig_class_name.h` (add the `"Flock"` case)
- Modify: `components/simulacra_radar/sig_seed.c` (add the seed entry; bump version)
- Modify: `tools/pcap_learn/sig_scan.c` (extend `CLASS_NAME[]`; update the header string)
- Test: `tools/pcap_learn/tests/test_flock.py` (new file)

**Interfaces:**
- Produces: `SIG_CLASS_FLOCK` (enum value 3, before `SIG_CLASS_COUNT`), `sig_class_name(SIG_CLASS_FLOCK) == "Flock"`, and a seed `threat_sig_t{ company_id=0x09C8, category=SIG_CAT_CAMERA, class_id=SIG_CLASS_FLOCK }`. Consumed by Task 2's render.

- [ ] **Step 1: Write the failing host test**

Create `tools/pcap_learn/tests/test_flock.py`:

```python
import os, subprocess, tempfile, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "sig_scan.exe" if os.name == "nt" else "sig_scan")


def advert(company, mfg):
    # sig_scan requires an "ad" field to count the line; the matcher itself reads company + mfg.
    return (f'{{"ts":1.0,"company":{company},"svc":0,"atype":"public",'
            f'"addr":"aabbccddeeff","mfg":"{mfg}","svcd":"","ad":"0201060aff{mfg}"}}')


def scan(lines):
    with tempfile.NamedTemporaryFile("w", suffix=".ndjson", delete=False, newline="\n") as f:
        f.write("\n".join(lines) + "\n"); path = f.name
    try:
        return subprocess.check_output([EXE, path], text=True)
    finally:
        os.unlink(path)


@unittest.skipUnless(os.path.exists(EXE), "sig_scan not built")
class Flock(unittest.TestCase):
    def test_flock_mfg_id_matches(self):
        # company 0x09C8 = 2504 (mfg little-endian starts c809) -> one Flock hit
        out = scan([advert(2504, "c809aabbccdd")])
        self.assertRegex(out, r"Flock\s*:\s*1")

    def test_selectivity_neighbor_id_no_match(self):
        # a neighboring company id must NOT match Flock
        out = scan([advert(2503, "c709aabbccdd")])
        self.assertRegex(out, r"Flock\s*:\s*0")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Build sig_scan and run the test to verify it fails**

Run (from repo root):
```
powershell -NoProfile -Command "cd tools/pcap_learn; cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\components\simulacra_radar /I..\..\main sig_scan.c ..\..\components\simulacra_radar\sig_match.c ..\..\components\simulacra_radar\sig_seed.c /Fe:sig_scan.exe"
python -m unittest discover -s tools/pcap_learn/tests -v
```
Expected: `sig_scan.exe` builds from the current sources and runs, but prints no "Flock" line (the signature doesn't exist yet) → the two `Flock` tests FAIL on the regex. Pre-existing tests still pass.

- [ ] **Step 3: Add `SIG_CLASS_FLOCK` to the class enum**

In `components/simulacra_radar/threat_sig.h`, change the `sig_class_t` line:

```c
typedef enum { SIG_CLASS_AIRTAG = 0, SIG_CLASS_SMARTTAG, SIG_CLASS_TILE, SIG_CLASS_FLOCK, SIG_CLASS_COUNT } sig_class_t;
```

- [ ] **Step 4: Add the class name**

In `components/simulacra_radar/sig_class_name.h`, add the case before `default`:

```c
        case SIG_CLASS_FLOCK:    return "Flock";
```

- [ ] **Step 5: Add the seed signature + bump the version**

In `components/simulacra_radar/sig_seed.c`, change `#define SIG_SEED_VERSION 1` to `2`, and add this entry to the `SEED[]` array after the Tile entry (before the closing `};`):

```c
    // Flock Safety / Raven surveillance gear: BLE mfg company id 0x09C8 (XUNTONG module vendor,
    // the signature the flockyou/DeFlock ecosystem uses). Company-id-only match (pat_len=0);
    // moderate confidence -- 0x09C8 is the module vendor, not Flock-specific (documented ceiling).
    { .sig_id=4, .category=SIG_CAT_CAMERA, .class_id=SIG_CLASS_FLOCK,
      .company_id=0x09C8, .svc_uuid16=0x0000, .addr_type_mask=0,
      .match_src=SIG_SRC_MFG_DATA, .pat_off=0, .pat_len=0,
      .pattern={0}, .mask={0}, .confidence=60 },
```

- [ ] **Step 6: Extend `sig_scan.c`'s class table + header string**

In `tools/pcap_learn/sig_scan.c`, change the `CLASS_NAME` array (line ~17):

```c
static const char *CLASS_NAME[] = { "AirTag", "SmartTag", "Tile", "Flock" };
```

and the signature-DB header print (the line containing `AirTag / SmartTag / Tile`):

```c
    printf("signature DB      : %zu sigs (v%u): AirTag / SmartTag / Tile / Flock\n", ndb, sig_seed_version());
```

- [ ] **Step 7: Rebuild sig_scan and run the test to verify it passes**

Run:
```
powershell -NoProfile -Command "cd tools/pcap_learn; cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\components\simulacra_radar /I..\..\main sig_scan.c ..\..\components\simulacra_radar\sig_match.c ..\..\components\simulacra_radar\sig_seed.c /Fe:sig_scan.exe"
python -m unittest discover -s tools/pcap_learn/tests -v
```
Expected: build succeeds; `Flock` tests PASS; the pre-existing `pcap_learn` tests still pass. If any pre-existing test hard-codes "3 sigs" or "v1", update it to the new count/version (the seed now has 4 sigs at v2).

- [ ] **Step 8: Commit**

```bash
git add components/simulacra_radar/threat_sig.h components/simulacra_radar/sig_class_name.h components/simulacra_radar/sig_seed.c tools/pcap_learn/sig_scan.c tools/pcap_learn/tests/test_flock.py
git commit -m "$(cat <<'EOF'
feat(sig): Flock/Raven BLE signature (mfg company 0x09C8)

Adds SIG_CLASS_FLOCK under SIG_CAT_CAMERA, matched company-id-only on
0x09C8 (the signature the flockyou/DeFlock ecosystem uses for Flock cameras
and Raven detectors). Bumps the seed version to 2. sig_scan now reports a
Flock line. Host-validated: synthetic 0x09C8 advert matches; a neighboring
company id does not (selectivity).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

### Task 2: CYD surveillance-presence render

**Files:**
- Modify: `components/simulacra_radar/radar_render.c` (`draw_detail` split; `draw_home` indicator)
- Modify: `tools/radar_audit/render_dump.c` (add a camera-threat arg so tests can inject one)
- Test: `tools/radar_audit/tests/test_radar_render.py` (add a surveillance-render test class)

**Interfaces:**
- Consumes: `SIG_CLASS_FLOCK`, `sig_class_name` (Task 1); existing `SIG_CAT_CAMERA`, `DETECT_KIND_KNOWN`, `COL_HUNTER`, `threat_escalation_level`.
- Produces (harness): `render_dump <view> <restless> <wandering> <bound> <active> <roster> <target> <threat_count> <pop> <esc> <flags> <uptime> <ncam>` — the new final positional arg `ncam` marks the first `ncam` threats as camera-category (`SIG_CAT_CAMERA`/`SIG_CLASS_FLOCK`/KNOWN, rssi -55); the rest stay behavioral followers.

- [ ] **Step 1: Write the failing render tests**

Append to `tools/radar_audit/tests/test_radar_render.py` (before the `if __name__` line if present, else at end):

```python
DETAIL = 2


def detail(threats=0, ncam=0, esc=0):
    # positional: view, restless,wandering,bound, active,roster,target, threats, pop, esc, flags, uptime, ncam
    return render(DETAIL, 1, 1, 1, 8, 16, 8, threats, 10, esc, 0, 0, ncam)


def home_surv(threats=0, ncam=0):
    return render(HOME, 1, 1, 1, 8, 16, 8, threats, 10, 0, 0, 0, ncam)


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class SurveillancePresence(unittest.TestCase):
    def test_camera_shows_surveillance_section(self):
        texts = detail(threats=1, ncam=1)
        self.assertIn("SURVEILLANCE", texts, f"no surveillance section; drew: {texts}")
        self.assertIn("Flock", texts, f"no Flock label; drew: {texts}")

    def test_camera_excluded_from_follower_count(self):
        # 3 threats, 1 camera -> the follower summary counts only the 2 non-cameras
        texts = detail(threats=3, ncam=1)
        self.assertTrue(any("2 seen" in t for t in texts), f"follower count wrong; drew: {texts}")
        self.assertIn("SURVEILLANCE", texts)

    def test_followers_only_no_surveillance(self):
        texts = detail(threats=2, ncam=0)
        self.assertNotIn("SURVEILLANCE", texts, f"unexpected surveillance section; drew: {texts}")
        self.assertTrue(any("2 seen" in t for t in texts))

    def test_home_surveil_indicator_present(self):
        texts = home_surv(threats=1, ncam=1)
        self.assertIn("!1", texts, f"no HOME surveil indicator; drew: {texts}")

    def test_home_no_indicator_without_camera(self):
        texts = home_surv(threats=1, ncam=0)
        self.assertFalse(any(t.startswith("!") for t in texts), f"unexpected indicator; drew: {texts}")
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `powershell -NoProfile -File tools/radar_audit/run.ps1 -Rebuild`
Expected: `render_dump` builds but ignores the extra `ncam` arg, so no camera threats exist → the `SurveillancePresence` tests FAIL (no "SURVEILLANCE"/"Flock"/"!1"). Existing render tests still pass.

- [ ] **Step 3: Teach `render_dump` to inject a camera threat**

In `tools/radar_audit/render_dump.c`, add `#include "threat_sig.h"` near the top includes (after `#include "radar_sigil.h"`). Then, immediately after the block that sets `st.flags`/`st.uptime_s` (the `if (argc > 12) st.uptime_s = ...` line), add:

```c
    // arg 13: how many of the threats are SIG_CAT_CAMERA surveillance (Flock), the rest followers.
    int ncam = argc > 13 ? atoi(argv[13]) : 0;
    for (int i = 0; i < ncam && i < st.threat_count && i < RADAR_MAX_THREATS; i++) {
        st.threats[i].kind     = DETECT_KIND_KNOWN;
        st.threats[i].category = SIG_CAT_CAMERA;
        st.threats[i].class_id = SIG_CLASS_FLOCK;
        st.threats[i].best_rssi = -55;
    }
```

- [ ] **Step 4: Split the SURVEILLANCE section out of `draw_detail`**

In `components/simulacra_radar/radar_render.c`, replace the entire `draw_detail` function with:

```c
static void draw_detail(radar_gfx_t *g, const radar_wire_status_t *st){
    draw_header(g,"FOLLOWERS");
    // Partition threats: behavioral followers vs. SIG_CAT_CAMERA surveillance infrastructure.
    int followers=0, cameras=0, flagged=0;
    for(uint8_t i=0;i<st->threat_count;i++){
        if(st->threats[i].category==SIG_CAT_CAMERA){ cameras++; continue; }
        followers++;
        if(threat_escalation_level(st->threats[i].sessions_seen,st->threats[i].places_seen)!=ESCALATION_NEW) flagged++;
    }
    if(followers==0) radar_gfx_text(g,16,40,"none detected",COL_ASH);
    else { char s[32]; snprintf(s,sizeof s,"%u seen  %d flagged",(unsigned)followers,flagged);
           radar_gfx_text(g,8,34,s,COL_ASH); }
    radar_gfx_hline(g,8,231,50,COL_EDGE);
    int y=58;
    for(uint8_t i=0;i<st->threat_count && y<250;i++){
        if(st->threats[i].category==SIG_CAT_CAMERA) continue;              // cameras render below
        detect_escalation_t e = threat_escalation_level(st->threats[i].sessions_seen,st->threats[i].places_seen);
        uint16_t c = escalation_color(e);
        radar_gfx_fill_rect(g,8,y+2,6,6,c);
        char name[16];
        if(st->threats[i].kind==DETECT_KIND_KNOWN) snprintf(name,sizeof name,"%s",sig_class_name(st->threats[i].class_id));
        else snprintf(name,sizeof name,"%08lx",(unsigned long)st->threats[i].hash);
        radar_gfx_text(g,20,y,name,c);
        char rec[12];
        if(e==ESCALATION_NEW) snprintf(rec,sizeof rec,"new");
        else snprintf(rec,sizeof rec,"%up %us",(unsigned)st->threats[i].places_seen,(unsigned)st->threats[i].sessions_seen);
        radar_gfx_text(g,112,y,rec,COL_ASH);
        char r[12]; snprintf(r,sizeof r,"%ddB",(int)st->threats[i].best_rssi);
        radar_gfx_text(g,224-(int)strlen(r)*8,y,r,COL_ASH);
        y+=18;
    }
    // SURVEILLANCE section: fixed infrastructure (Flock/Raven) -- present, not "following".
    if(cameras>0){
        y+=6;
        radar_gfx_text(g,8,y,"SURVEILLANCE",COL_HUNTER); y+=20;
        for(uint8_t i=0;i<st->threat_count && y<310;i++){
            if(st->threats[i].category!=SIG_CAT_CAMERA) continue;
            radar_gfx_fill_rect(g,8,y+2,6,6,COL_HUNTER);
            radar_gfx_text(g,20,y,sig_class_name(st->threats[i].class_id),COL_HUNTER);
            char r[12]; snprintf(r,sizeof r,"%ddB",(int)st->threats[i].best_rssi);
            radar_gfx_text(g,224-(int)strlen(r)*8,y,r,COL_ASH);
            y+=18;
        }
    }
}
```

- [ ] **Step 5: Add the HOME surveil indicator**

In `components/simulacra_radar/radar_render.c`, in `draw_home`, immediately after the line
`radar_gfx_text(g, px - 8 - 6 * 8, 9, "STATUS", COL_ASH);`, add:

```c
    // Surveillance-presence count (Flock/Raven, category CAMERA): a compact "!N" left of the wordmark's
    // status area when >=1 is seen. Distinct from HUNTED (a follower) -- this is fixed infra nearby.
    int nsurv=0;
    for(uint8_t i=0;i<st->threat_count;i++) if(st->threats[i].category==SIG_CAT_CAMERA) nsurv++;
    if(nsurv>0){ char sb[8]; snprintf(sb,sizeof sb,"!%d",nsurv); radar_gfx_text(g, 100, 9, sb, COL_HUNTER); }
```

- [ ] **Step 6: Rebuild and run the tests to verify they pass**

Run: `powershell -NoProfile -File tools/radar_audit/run.ps1 -Rebuild`
Expected: build succeeds; `SurveillancePresence` tests PASS; all pre-existing `radar_audit` tests still green.

- [ ] **Step 7: Commit**

```bash
git add components/simulacra_radar/radar_render.c tools/radar_audit/render_dump.c tools/radar_audit/tests/test_radar_render.py
git commit -m "$(cat <<'EOF'
feat(cyd): surface Flock/Raven as distinct surveillance presence

FOLLOWERS view splits SIG_CAT_CAMERA rows into their own SURVEILLANCE
section (Flock label + proximity) so fixed infrastructure is never
described as a follower, and the follower count excludes them. HOME gains a
compact "!N" surveil indicator when >=1 camera is present. Posture logic
unchanged (a camera never flips it to HUNTED). Host-tested via render_dump.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

### Task 3: Firmware compile-verify

**Files:** none (verification only — the signature and render both live in the shared `simulacra_radar` component compiled into all three roles).

- [ ] **Step 1: Compile-verify the Shade decoy (C6)**

Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c6 -Do build`
Expected: `BUILD: Project build complete.`

- [ ] **Step 2: Compile-verify the Ward decoy (C5)**

Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Do build`
Expected: `BUILD: Project build complete.`

- [ ] **Step 3: Compile-verify the CYD (Vigil)**

Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target cyd -Do build`
Expected: `BUILD: Project build complete.`

- [ ] **Step 4: Commit (only if any build required a fix)**

If steps 1–3 all built clean with no edits, there is nothing to commit — skip. If a build surfaced a fix, commit it:

```bash
git add -A
git commit -m "$(cat <<'EOF'
fix(flock-ble): resolve firmware compile-verify findings

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

## Notes for the implementer

- **No real positive sample.** The most recent BLE drive capture had zero `0x09C8`; validation is the synthetic advert in Task 1. A true positive needs a future capture near a mapped Flock location.
- **Do not touch** `detect.c`, `coexist.c`, or `radar_wire_status_t` — the existing known-device path already carries category/class/rssi to the CYD.
- The HOME `!N` position (x=100) sits between the "SIMULACRA" wordmark and the "STATUS" label for the common 6-glyph postures; if it looks cramped on hardware, relocating it is a pure visual tweak (the host test only asserts the `!N` text is drawn).
