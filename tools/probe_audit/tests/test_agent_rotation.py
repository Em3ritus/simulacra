import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def agentrot(seed=1, ticks=35, tickms=60000):
    out = subprocess.check_output([EXE, "--agentrot", str(seed), str(ticks), str(tickms)], text=True)
    return [(int(t), m, int(g)) for t, m, g in (ln.split() for ln in out.splitlines())]


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class AgentRotation(unittest.TestCase):
    def test_bound_mac_rotates_intra_life(self):
        rows = agentrot()                                 # 35 min < 40 min life -> no reincarnation
        macs = [m for _, m, _ in rows]
        self.assertGreaterEqual(len(rows), 2)             # initial MAC + >=1 rotation
        self.assertEqual(len(macs), len(set(macs)))       # each rotation a fresh unique MAC
        self.assertEqual({g for _, _, g in rows}, {1})    # intra-life: generation unchanged

    def test_spacing_in_band(self):
        times = [t for t, _, _ in agentrot()]
        for a, b in zip(times, times[1:]):
            self.assertTrue(480000 <= b - a < 960000, f"gap {b-a} ms outside 8-15min (+1 tick)")


def coexistrot(seed=1, nph=4, ticks=60, tickms=60000):
    """Rows of (t_ms, agent, mac, generation) from the coexist-equivalent call sequence."""
    out = subprocess.check_output(
        [EXE, "--coexistrot", str(seed), str(nph), str(ticks), str(tickms)], text=True)
    return [(int(t), int(i), m, int(g)) for t, i, m, g in (ln.split() for ln in out.splitlines())]


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class CoexistRotation(unittest.TestCase):
    """BUG-1 guard. probe_agents_lifecycle (which AgentRotation drives) is only called under
    SIMULACRA_PROBE; the shipped combined build ticks phantom_sync_wifi + rotate_tick. These
    assertions fail if rotation is ever detached from the coexist path again."""

    def test_macs_rotate_on_the_coexist_path(self):
        rows = coexistrot()
        for agent in {i for _, i, _, _ in rows}:
            seen = [(t, m, g) for t, i, m, g in rows if i == agent]
            self.assertGreaterEqual(len(seen), 2, f"agent {agent} never changed MAC in 60 min")
            # at least one change must be intra-life (same generation), not just a rebirth
            intra = [b for a, b in zip(seen, seen[1:]) if b[2] == a[2]]
            self.assertTrue(intra, f"agent {agent} only changed MAC at persona rebirth")

    def test_intra_life_spacing_in_band(self):
        rows = coexistrot()
        for agent in {i for _, i, _, _ in rows}:
            seen = [(t, m, g) for t, i, m, g in rows if i == agent]
            for a, b in zip(seen, seen[1:]):
                if b[2] != a[2]:
                    continue                       # persona rebirth: lifetime, not the rotation band
                self.assertTrue(480000 <= b[0] - a[0] < 960000,
                                f"agent {agent} gap {b[0]-a[0]} ms outside 8-15min (+1 tick)")

    def test_every_mac_is_unique(self):
        macs = [m for _, _, m, _ in coexistrot()]
        self.assertEqual(len(macs), len(set(macs)), "a rotation reused a MAC")

    def test_coexist_task_actually_calls_rotate_tick(self):
        """The behavioural tests above pass whether or not the shipped build ever calls rotation —
        that is precisely how BUG-1 hid. Assert the call exists on the coexist path itself."""
        coexist = os.path.join(os.path.dirname(os.path.dirname(TOOL)), "main", "coexist.c")
        with open(coexist, encoding="utf-8") as f:
            src = f.read()
        self.assertIn("probe_agents_rotate_tick(", src,
                      "coexist.c must drive Wi-Fi MAC rotation; probe_agents_lifecycle is "
                      "SIMULACRA_PROBE-only, so without this call personas hold one MAC for life")


if __name__ == "__main__":
    unittest.main()
