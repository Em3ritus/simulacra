# Per-Node Population Glide (Adaptation-Speed Option D) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make each decoy node ramp its Wi-Fi fake-phone count toward a new target by ±1 on an independent jittered cadence (a "glide") instead of snapping in one batch, so the aggregate fake-crowd size changes like organic device churn rather than a synchronized controller step.

**Architecture:** Fold the glide into `main/probe_agents.c`, which already owns the applied count (`s_n`) and `probe_agents_set_target`, and is already host-tested by `tools/probe_audit`. A pure helper (`probe_glide_next`) does the step arithmetic; file-static state plus two entry points (`probe_agents_glide_set_target`, `probe_agents_glide_tick`) own the desired target and the jittered clock. `main/coexist.c` changes at exactly two call sites: the reprofile branch records the target via the glide instead of snapping, and the Wi-Fi burst branch advances the glide.

**Tech Stack:** C (ESP-IDF firmware, host-compiled via MSVC `cl` for tests), Python `unittest` host tests driving a `probe_dump.exe` harness.

## Global Constraints

- Glide granularity is one agent at a time: `GLIDE_STEP = 1` (exact).
- Jitter interval bounds: `GLIDE_MIN_MS = 30000`, `GLIDE_MAX_MS = 60000` (exact; per-node draw via `esp_random`).
- Boot is instant: the **first** `probe_agents_glide_set_target` after init applies immediately (no ramp); only later changes glide.
- Law 4 (population-match) is preserved: the glide only smooths *application*; it never alters the computed target. Do NOT add noise to or otherwise decorrelate the target itself.
- Jitter must be per-node independent (drawn from `esp_random`, already `rand()`-backed on host) — no node-id, MAC derivation, or inter-node coordination.
- No new source files and no build-file edits: `probe_agents.c` and `probe_dump.c` are already in the `tools/probe_audit` compile list (`run.ps1`, `Makefile`).
- Commit identity is the repo-local `Em3ritus` noreply. Every commit carries the trailers:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.

---

### Task 1: Pure glide-step helper `probe_glide_next`

**Files:**
- Modify: `main/probe_agents.h` (add one declaration)
- Modify: `main/probe_agents.c` (add the pure function)
- Modify: `tools/probe_audit/probe_dump.c` (add a `--glidenext` harness mode)
- Test: `tools/probe_audit/tests/test_glide.py` (new file)

**Interfaces:**
- Produces: `int probe_glide_next(int current, int target, int step);` — returns `current` moved toward `target` by at most `abs(step)`, never overshooting. Pure, deterministic, no RNG/clock.
- Produces (harness): `probe_dump --glidenext <current> <target> <step>` prints one line: the `probe_glide_next` result.

- [ ] **Step 1: Write the failing test**

Create `tools/probe_audit/tests/test_glide.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def glidenext(current, target, step):
    out = subprocess.check_output([EXE, "--glidenext", str(current), str(target), str(step)], text=True)
    return int(out.strip())


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class GlideNext(unittest.TestCase):
    def test_steps_up_by_step(self):
        self.assertEqual(glidenext(4, 8, 1), 5)

    def test_steps_down_by_step(self):
        self.assertEqual(glidenext(8, 4, 1), 7)

    def test_never_overshoots_up(self):
        self.assertEqual(glidenext(7, 8, 5), 8)

    def test_never_overshoots_down(self):
        self.assertEqual(glidenext(8, 7, 5), 7)

    def test_noop_when_at_target(self):
        self.assertEqual(glidenext(6, 6, 1), 6)

    def test_negative_step_is_treated_as_magnitude(self):
        self.assertEqual(glidenext(4, 8, -1), 5)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `powershell -NoProfile -File tools/probe_audit/run.ps1 -Rebuild`
Expected: build FAILS (`probe_glide_next` undefined / `--glidenext` unknown) — the exe won't rebuild, so `test_glide.py` is either skipped or errors. Confirm the failure is the missing symbol, not an unrelated break.

- [ ] **Step 3: Add the declaration to the header**

In `main/probe_agents.h`, after the `probe_agent_pick_ssid` declaration (end of file), add:

```c
// Move `current` toward `target` by at most `step` (magnitude), never overshooting. Pure: the
// glide's step arithmetic, isolated from the jitter clock so it is directly unit-testable.
int probe_glide_next(int current, int target, int step);
```

- [ ] **Step 4: Implement the pure helper**

In `main/probe_agents.c`, add at the end of the file:

```c
int probe_glide_next(int current, int target, int step)
{
    if (step < 0) step = -step;
    if (current < target) { int d = target - current; return current + (d < step ? d : step); }
    if (current > target) { int d = current - target; return current - (d < step ? d : step); }
    return current;
}
```

- [ ] **Step 5: Add the `--glidenext` harness mode**

In `tools/probe_audit/probe_dump.c`, add this block inside `main`, immediately before the
`if (argc > 1 && strcmp(argv[1], "--pick") == 0)` block:

```c
    if (argc > 1 && strcmp(argv[1], "--glidenext") == 0) {   // pure step: --glidenext <cur> <target> <step>
        int cur  = argc > 2 ? (int)strtol(argv[2], 0, 10) : 0;
        int tgt  = argc > 3 ? (int)strtol(argv[3], 0, 10) : 0;
        int step = argc > 4 ? (int)strtol(argv[4], 0, 10) : 1;
        printf("%d\n", probe_glide_next(cur, tgt, step));
        return 0;
    }
```

- [ ] **Step 6: Rebuild and run the test to verify it passes**

Run: `powershell -NoProfile -File tools/probe_audit/run.ps1 -Rebuild`
Expected: build succeeds; `GlideNext` tests PASS; the rest of the probe_audit suite still green.

- [ ] **Step 7: Commit**

```bash
git add main/probe_agents.h main/probe_agents.c tools/probe_audit/probe_dump.c tools/probe_audit/tests/test_glide.py
git commit -m "$(cat <<'EOF'
feat(probe): pure glide-step helper probe_glide_next

The isolated step arithmetic for the per-node population glide: move the
applied decoy count toward the target by at most `step` without
overshooting. Pure and directly unit-tested via a --glidenext harness mode.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

### Task 2: Glide state + entry points + integration harness

**Files:**
- Modify: `main/probe_agents.h` (two declarations)
- Modify: `main/probe_agents.c` (glide constants, file-static state, reset on init, two functions)
- Modify: `tools/probe_audit/probe_dump.c` (add a stdin-driven `--glide` harness mode)
- Test: `tools/probe_audit/tests/test_glide.py` (add integration tests)

**Interfaces:**
- Consumes: `probe_glide_next` (Task 1); existing `probe_agents_init`, `probe_agents_set_target`, `probe_agents_count`; existing file-static `static uint32_t rnd_range(uint32_t lo, uint32_t hi)` in `probe_agents.c`.
- Produces:
  - `void probe_agents_glide_set_target(int target, uint32_t now_ms);` — records the desired target; the **first** call after init applies it immediately (boot-instant) and arms the glide clock; later calls only record it.
  - `void probe_agents_glide_tick(uint32_t now_ms);` — if the jittered interval has elapsed and the applied count differs from the desired target, step it by `GLIDE_STEP` toward the target and re-arm the clock. Self-gating; safe to call every Wi-Fi burst.
  - `probe_agents_init` now also resets the glide state (so the next `glide_set_target` is treated as boot).
- Produces (harness): `probe_dump --glide <seed>` reads stdin lines and prints the applied count after each:
  - `init <n0>` → `probe_agents_init(n0, 0)`; prints count.
  - `target <now_ms> <tgt>` → `probe_agents_glide_set_target(tgt, now_ms)`; prints count.
  - `tick <now_ms>` → `probe_agents_glide_tick(now_ms)`; prints count.

- [ ] **Step 1: Write the failing integration tests**

Append to `tools/probe_audit/tests/test_glide.py` (before the `if __name__` line):

```python
def run_glide(seed, script_lines):
    """Feed newline-joined commands to `--glide` on stdin; return the list of printed counts (ints)."""
    p = subprocess.run([EXE, "--glide", str(seed)], input="\n".join(script_lines) + "\n",
                       text=True, capture_output=True)
    return [int(x) for x in p.stdout.split()]


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class GlideSession(unittest.TestCase):
    def test_boot_first_target_is_instant(self):
        # init at 4, first target 8 -> applied jumps to 8 immediately (no ramp on boot)
        counts = run_glide(1, ["init 4", "target 1000 8"])
        self.assertEqual(counts, [4, 8])

    def test_later_change_ramps_by_one_per_tick(self):
        # boot at 8 (target 8 == current, no move), then raise to 12 and advance the clock well
        # past the max interval (60000ms) each tick -> a clean +1 staircase to 12, then plateau.
        script = ["init 8", "target 1000 8", "target 100000 12"]
        t = 100000
        for _ in range(6):
            t += 60001
            script.append(f"tick {t}")
        counts = run_glide(1, script)
        # counts: [8 (init), 8 (boot target), 8 (record 12), then ticks]
        tail = counts[3:]
        self.assertEqual(tail, [9, 10, 11, 12, 12, 12])

    def test_shrink_ramps_down_by_one(self):
        script = ["init 8", "target 1000 8", "target 100000 5"]
        t = 100000
        for _ in range(4):
            t += 60001
            script.append(f"tick {t}")
        counts = run_glide(1, script)
        self.assertEqual(counts[3:], [7, 6, 5, 5])

    def test_no_step_before_min_interval(self):
        # after a step, a tick <30000ms (GLIDE_MIN_MS) later must NOT advance again
        script = ["init 8", "target 1000 8", "target 100000 12",
                  "tick 100000",     # first step -> 9, arms next step 30000..60000ms out
                  "tick 129999"]     # 29999ms later: below the min interval -> no advance
        counts = run_glide(1, script)
        self.assertEqual(counts[-2:], [9, 9])

    def test_step_by_max_interval(self):
        # after a step, a tick 60000ms (GLIDE_MAX_MS) later MUST advance (interval <= 60000)
        script = ["init 8", "target 1000 8", "target 100000 12",
                  "tick 100000",     # first step -> 9, arms next step at 100000 + [30000,60000]
                  "tick 160000"]     # 60000ms later: at/above the max interval -> advance to 10
        counts = run_glide(1, script)
        self.assertEqual(counts[-1], 10)

    def test_converges_and_holds_at_target(self):
        script = ["init 2", "target 1000 2", "target 100000 6"]
        t = 100000
        for _ in range(10):
            t += 60001
            script.append(f"tick {t}")
        counts = run_glide(1, script)
        self.assertEqual(counts[-1], 6)              # reached target
        self.assertTrue(all(c <= 6 for c in counts)) # never overshot

    def test_solo_k1_still_glides(self):
        # K=1 (standalone) is just a single node; the glide still ramps (not fleet-only)
        script = ["init 3", "target 1000 3", "target 100000 7", "tick 160001", "tick 220002"]
        counts = run_glide(1, script)
        self.assertEqual(counts[-2:], [4, 5])
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `powershell -NoProfile -File tools/probe_audit/run.ps1`
Expected: build succeeds (Task 1 code present) but `--glide` is an unknown mode, so it prints nothing → the new `GlideSession` tests FAIL on the empty/short count lists. `GlideNext` still passes.

- [ ] **Step 3: Add glide constants and state to `probe_agents.c`**

In `main/probe_agents.c`, after the existing `#define SSID_BURST_NAMED_PCT 60` line, add:

```c
#define GLIDE_STEP    1        // move the applied population one agent at a time (device-faithful)
#define GLIDE_MIN_MS  30000u   // per-node jittered step interval: lower bound (~30 s)
#define GLIDE_MAX_MS  60000u   // upper bound (~60 s); each step re-draws independently via esp_random
```

Then, after the existing `static int s_n;` line, add:

```c
static int      s_glide_target;       // desired applied count the glide is ramping toward
static bool     s_glide_armed;        // false until the first glide_set_target (boot-instant gate)
static uint32_t s_next_glide_ms;      // earliest time the next +/-1 step may apply
```

- [ ] **Step 4: Reset the glide state in `probe_agents_init`**

In `main/probe_agents.c`, in `probe_agents_init`, add `s_glide_armed = false;` right after `s_n = n;`:

```c
void probe_agents_init(int n, uint32_t now_ms)
{
    if (n > PROBE_AGENTS_MAX) n = PROBE_AGENTS_MAX;
    if (n < 1) n = 1;
    s_n = n;
    s_glide_armed = false;                         // next glide_set_target is treated as boot (instant)
    for (int i = 0; i < s_n; i++) agent_spawn(&s_agents[i], now_ms);
}
```

- [ ] **Step 5: Implement the two glide entry points**

In `main/probe_agents.c`, add immediately after the `probe_glide_next` function from Task 1:

```c
void probe_agents_glide_set_target(int target, uint32_t now_ms)
{
    s_glide_target = target;
    if (!s_glide_armed) {                          // boot: apply the first target at once (no ramp)
        s_glide_armed = true;
        probe_agents_set_target(target, now_ms);
        s_next_glide_ms = now_ms + rnd_range(GLIDE_MIN_MS, GLIDE_MAX_MS);
    }
}

void probe_agents_glide_tick(uint32_t now_ms)
{
    if (!s_glide_armed) return;                    // nothing to glide toward yet
    if ((int32_t)(now_ms - s_next_glide_ms) < 0) return;   // still within the jittered interval
    int cur = probe_agents_count();
    if (cur == s_glide_target) return;             // already there
    probe_agents_set_target(probe_glide_next(cur, s_glide_target, GLIDE_STEP), now_ms);
    s_next_glide_ms = now_ms + rnd_range(GLIDE_MIN_MS, GLIDE_MAX_MS);   // re-arm with a fresh draw
}
```

- [ ] **Step 6: Declare the entry points in the header**

In `main/probe_agents.h`, directly under the `probe_glide_next` declaration from Task 1, add:

```c
// Record the desired applied population. The FIRST call after probe_agents_init applies immediately
// (boot-instant, no ramp); later calls only record it — probe_agents_glide_tick ramps toward it by
// GLIDE_STEP per jittered per-node interval. now_ms seeds/advances the glide clock.
void probe_agents_glide_set_target(int target, uint32_t now_ms);

// Advance the glide: if the per-node jittered interval has elapsed and the applied count differs
// from the desired target, step it one toward the target. Self-gating; call it every Wi-Fi burst.
void probe_agents_glide_tick(uint32_t now_ms);
```

- [ ] **Step 7: Add the `--glide` harness mode**

In `tools/probe_audit/probe_dump.c`, add this block immediately after the `--glidenext` block from Task 1:

```c
    if (argc > 1 && strcmp(argv[1], "--glide") == 0) {   // stdin-driven glide session (see test_glide.py)
        srand(argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1);
        char line[64], cmd[16]; unsigned a, b;
        while (fgets(line, sizeof line, stdin)) {
            if (sscanf(line, "%15s", cmd) != 1) continue;
            if (strcmp(cmd, "init") == 0 && sscanf(line, "%*s %u", &a) == 1) {
                probe_agents_init((int)a, 0);
                printf("%d\n", probe_agents_count());
            } else if (strcmp(cmd, "target") == 0 && sscanf(line, "%*s %u %u", &a, &b) == 2) {
                probe_agents_glide_set_target((int)b, a);
                printf("%d\n", probe_agents_count());
            } else if (strcmp(cmd, "tick") == 0 && sscanf(line, "%*s %u", &a) == 1) {
                probe_agents_glide_tick(a);
                printf("%d\n", probe_agents_count());
            }
        }
        return 0;
    }
```

- [ ] **Step 8: Rebuild and run the tests to verify they pass**

Run: `powershell -NoProfile -File tools/probe_audit/run.ps1 -Rebuild`
Expected: build succeeds; `GlideNext` + `GlideSession` all PASS; the rest of the probe_audit suite still green.

- [ ] **Step 9: Commit**

```bash
git add main/probe_agents.h main/probe_agents.c tools/probe_audit/probe_dump.c tools/probe_audit/tests/test_glide.py
git commit -m "$(cat <<'EOF'
feat(probe): per-node population glide (state + entry points)

Records a desired Wi-Fi decoy target and ramps the applied count toward it
by +/-1 per jittered per-node interval (GLIDE_MIN_MS..GLIDE_MAX_MS, drawn
from esp_random so nodes desync). The first target after init applies
instantly (boot). Integration-tested via a stdin-driven --glide harness:
boot-instant, +/-1 staircase, interval bounds, convergence, K=1.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

### Task 3: Wire the glide into coexist + firmware compile-verify

**Files:**
- Modify: `main/coexist.c` (two call sites in `coexist_task`)

**Interfaces:**
- Consumes: `probe_agents_glide_set_target`, `probe_agents_glide_tick` (Task 2). `probe_agents.h` is already included in `coexist.c` (line 49).

- [ ] **Step 1: Advance the glide on each Wi-Fi burst**

In `main/coexist.c`, in `coexist_task`, the Wi-Fi burst branch currently reads:

```c
        if (d.fire_wifi && s_wifi_ok) {
            const uint8_t *ch24; size_t n24 = probe_channels_24(&ch24);
            phantom_sync_wifi(now);                                   // agents track persona lives
            if (n24) probe_inject_burst(ch24[hop24++ % n24]);        // 2.4 GHz (coex-arbitrated)
            if (p->use_5g && (++s_wifi_ctr % COEX_5G_EVERY == 0)) coexist_5g_excursion();
        }
```

Add the glide tick as the first statement inside the block:

```c
        if (d.fire_wifi && s_wifi_ok) {
            probe_agents_glide_tick(now);                             // ramp applied pop toward target
            const uint8_t *ch24; size_t n24 = probe_channels_24(&ch24);
            phantom_sync_wifi(now);                                   // agents track persona lives
            if (n24) probe_inject_burst(ch24[hop24++ % n24]);        // 2.4 GHz (coex-arbitrated)
            if (p->use_5g && (++s_wifi_ctr % COEX_5G_EVERY == 0)) coexist_5g_excursion();
        }
```

- [ ] **Step 2: Record the reprofile target through the glide instead of snapping**

In `main/coexist.c`, in `coexist_task`, the reprofile branch currently reads:

```c
        if (d.fire_reprofile) {
            coexist_reprofile(p);                                   // BLE population-match (may early-return)
            int wt      = s_wifi_obs_ok ? wifi_obs_target(now) : WIFI_OBS_FALLBACK;
            int k       = fleet_pop_live_size(now);                 // live fleet size (peers heard + self)
            int agents  = fleet_pop_share_k(wt, k);                 // this node's share of the crowd target
            probe_agents_set_target(agents, now);
```

Change only the `probe_agents_set_target` line to record the desired target via the glide:

```c
        if (d.fire_reprofile) {
            coexist_reprofile(p);                                   // BLE population-match (may early-return)
            int wt      = s_wifi_obs_ok ? wifi_obs_target(now) : WIFI_OBS_FALLBACK;
            int k       = fleet_pop_live_size(now);                 // live fleet size (peers heard + self)
            int agents  = fleet_pop_share_k(wt, k);                 // this node's share of the crowd target
            probe_agents_glide_set_target(agents, now);             // glide toward it (boot-instant first time)
```

Leave the `ESP_LOGW("wifi popmatch: ...")` line below unchanged — it logs the computed `agents` target, which is still the right value to observe (the glide ramps the applied count toward it).

- [ ] **Step 3: Compile-verify the Shade decoy (C6)**

Run: `powershell -NoProfile -Command "& \"$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1\" -Target c6 -Do build"`
Expected: `BUILD: Project build complete.`

- [ ] **Step 4: Compile-verify the Ward decoy (C5)**

Run: `powershell -NoProfile -Command "& \"$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1\" -Target c5 -Do build"`
Expected: `BUILD: Project build complete.`

- [ ] **Step 5: Commit**

```bash
git add main/coexist.c
git commit -m "$(cat <<'EOF'
feat(coexist): apply the Wi-Fi popmatch target via the population glide

The reprofile tick now records its computed per-node target through
probe_agents_glide_set_target (boot-instant on the first tick) and the
Wi-Fi burst advances the glide, so the decoy count ramps by +/-1 on each
node's independent jittered clock instead of snapping in lockstep. The
computed target is still logged. Compile-verified c5 + c6.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

## Notes for the implementer

- **No on-air validation is claimed.** Firmware compile-verify (c5 + c6) is the bar; validating the aggregate staircase over the air needs a multi-node capture over time that we do not have.
- **Do not touch** `wifi_density.c` (EWMA/cadence), `fleet_pop.*` (the census divisor), or the BLE/persona boot population. Those are explicitly out of scope.
- The glide clock uses the same `int32_t` wrap-safe comparison idiom (`(int32_t)(now - deadline) < 0`) as the rest of `probe_agents.c` (e.g. `probe_agents_due`), so a `uint32_t` millisecond wrap is handled the same way.
