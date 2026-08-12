import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def turborot(seed=1, ticks=20, tickms=1000):
    out = subprocess.check_output([EXE, "--turborot", str(seed), str(ticks), str(tickms)], text=True)
    return [(int(t), m) for t, m in (ln.split() for ln in out.splitlines())]


def agents_lateturbo(seed=1, n=8, pre_ticks=5, post_ticks=20, tickms=1000):
    args = [EXE, "--agents-lateturbo", str(seed), str(n), str(pre_ticks), str(post_ticks), str(tickms)]
    out = subprocess.check_output(args, text=True)
    duty = {}     # slot -> "active"/"idle", snapshotted right after the switch
    rotations = {}  # slot -> [t, t, ...] rotation timestamps observed after the switch
    for ln in out.splitlines():
        parts = ln.split()
        if parts[0] == "A":
            duty[int(parts[2])] = parts[3]
        elif parts[0] == "R":
            rotations.setdefault(int(parts[2]), []).append(int(parts[1]))
    return duty, rotations


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class TurboWifiRotation(unittest.TestCase):
    def test_mac_rotates_multiple_times_in_20s(self):
        # The normal (non-turbo) persona MAC rotation band is 8-15 MINUTES; in a 20s window it
        # would never rotate at all. Turbo's 3-8s band must show several rotations in the same window.
        rows = turborot(seed=2, ticks=20, tickms=1000)
        self.assertGreaterEqual(len(rows), 3, f"expected several MAC changes in 20s, got {rows}")

    def test_gaps_sit_in_the_turbo_band(self):
        rows = turborot(seed=4, ticks=25, tickms=1000)
        times = [t for t, _ in rows]
        for a, b in zip(times, times[1:]):
            self.assertGreaterEqual(b - a, 3000 - 1000, f"rotated too fast: {b - a} ms")
            self.assertLessEqual(b - a, 8000 + 1000, f"rotated too slow: {b - a} ms")

    def test_every_mac_is_unique(self):
        rows = turborot(seed=6, ticks=25, tickms=1000)
        macs = [m for _, m in rows]
        self.assertEqual(len(macs), len(set(macs)), "a turbo rotation reused a MAC")

    def test_preexisting_agent_goes_active_and_rotates_on_late_turbo(self):
        # I-1 regression guard: init normally (turbo off), let some agents land DUTY_IDLE with a
        # slow 8-15 min MAC-rotation deadline, THEN switch turbo on mid-run. Every pre-existing
        # agent must show DUTY_ACTIVE immediately at the switch (not only fresh spawns), and every
        # agent must rotate its MAC within the turbo band (~3-8s) of the switch, not wait out its
        # old multi-minute deadline.
        duty, rotations = agents_lateturbo(seed=11, n=8, pre_ticks=5, post_ticks=20, tickms=1000)
        self.assertEqual(len(duty), 8, f"expected a duty snapshot for all 8 agents: {duty}")
        self.assertTrue(all(v == "active" for v in duty.values()),
                         f"turbo-on must force every pre-existing agent to DUTY_ACTIVE: {duty}")
        switch_t = 5000   # pre_ticks(5) * tickms(1000)
        self.assertEqual(len(rotations), 8,
                          f"expected all 8 pre-existing agents to rotate after turbo switched on, got {rotations}")
        for slot, times in rotations.items():
            first = times[0]
            self.assertLessEqual(first, switch_t + 8000 + 1000,
                                  f"agent {slot} took too long to rotate after turbo-on: first={first}")


if __name__ == "__main__":
    unittest.main()
