import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")
ROOT = os.path.dirname(os.path.dirname(TOOL))


def personabind(seed=1, n0=8, ticks=40):
    out = subprocess.check_output(
        [EXE, "--personabind", str(seed), str(n0), str(ticks)], text=True)
    return [(int(t), int(a), int(p)) for t, a, p in (ln.split() for ln in out.splitlines())]


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class PersonaBinding(unittest.TestCase):
    """A persona is one synthetic device presenting on TWO radios. The Wi-Fi agent count follows
    room density (the glide); the persona count must follow it exactly.

    Too many personas => BLE 'phones' that never send a probe request. A phone that advertises a
    rotating RPA but has never probed for a network in 40 minutes is not a phone, and that is the
    single-radio ghost the cross-protocol persona design exists to defeat.

    Too few personas => Wi-Fi agents with no BLE twin, and with no lifecycle on the coexist path
    (probe_agents_lifecycle runs only under SIMULACRA_PROBE), so they never age out."""

    def test_counts_stay_equal_through_density_swings(self):
        rows = personabind()
        self.assertTrue(rows)
        for t, agents, personas in rows:
            self.assertEqual(agents, personas,
                             f"t={t}: {agents} agents vs {personas} personas — "
                             "one of them has an unpaired radio")

    def test_the_swing_actually_exercised_both_directions(self):
        """Guard the guard: if the glide never moved, equality above would be trivially true."""
        counts = {a for _, a, _ in personabind()}
        self.assertGreater(max(counts), min(counts) + 2,
                           f"agent count barely moved ({sorted(counts)}) — test proves nothing")

    def test_coexist_task_couples_the_two_counts(self):
        """The behavioural test drives the coupling directly, so it passes whether or not the
        shipped build calls it. Assert the call exists on the coexist path."""
        with open(os.path.join(ROOT, "main", "coexist.c"), encoding="utf-8") as f:
            src = f.read()
        self.assertIn("phantom_set_count(probe_agents_count()", src,
                      "coexist.c must resize the persona registry to match the glided agent count")


if __name__ == "__main__":
    unittest.main()
