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
