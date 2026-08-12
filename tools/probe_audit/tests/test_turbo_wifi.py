import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def turborot(seed=1, ticks=20, tickms=1000):
    out = subprocess.check_output([EXE, "--turborot", str(seed), str(ticks), str(tickms)], text=True)
    return [(int(t), m) for t, m in (ln.split() for ln in out.splitlines())]


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


if __name__ == "__main__":
    unittest.main()
