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


if __name__ == "__main__":
    unittest.main()
