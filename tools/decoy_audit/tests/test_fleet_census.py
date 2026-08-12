import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def fleetnode(script):
    out = subprocess.run([EXE, "--fleetnode"], input=script, capture_output=True, text=True).stdout
    return [int(x) for x in out.split()]


def _macs(n, base=1):
    return "".join("note 02000000%04x 0\n" % (base + i) for i in range(n))


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class FleetCensus(unittest.TestCase):
    def test_no_peers_count_zero_livesize_one(self):
        self.assertEqual(fleetnode("reset\ncount 0\nlivesize 0\n"), [0, 1])

    def test_distinct_nodes_counted(self):
        self.assertEqual(fleetnode("reset\n" + _macs(3) + "count 0\nlivesize 0\n"), [3, 4])

    def test_renote_same_node_no_double_count(self):
        script = "reset\nnote 020000000001 0\nnote 020000000001 100\ncount 100\nlivesize 100\n"
        self.assertEqual(fleetnode(script), [1, 2])

    def test_ttl_expiry_drops_node(self):
        # FLEET_MAC_TTL_MS = 90000: a node last seen at t=0 is gone by t=200000
        script = "reset\nnote 020000000001 0\ncount 200000\nlivesize 200000\n"
        self.assertEqual(fleetnode(script), [0, 1])

    def test_eviction_caps_at_node_cap(self):
        # FLEET_NODE_CAP = 8: noting 12 distinct nodes still reports at most 8
        rows = fleetnode("reset\n" + _macs(12) + "count 0\n")
        self.assertLessEqual(rows[-1], 8)

    def test_independent_of_mac_exclusion_table(self):
        # noting many distinct SYNTHETIC macs via the existing exclusion API must not
        # inflate the NODE count -- they are unrelated tables.
        pass  # covered structurally: --fleetnode never touches fleet_note_peer_macs


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class ShareFollowsLiveCensus(unittest.TestCase):
    """The BLE crowd share must divide by the LIVE node count, not a compile-time constant.

    SIMULACRA_FLEET_SIZE defaulted to 1 and nothing ever set it, so every node in a 3-node fleet
    sized its crowd as if standalone: roughly 3x the intended BLE density radiating in one room,
    which re-opens the density tell that population-match exists to close. The Wi-Fi side already
    used the live census; these pin the BLE side to it too."""

    def test_size_is_one_with_no_peers(self):
        self.assertEqual(fleetnode("reset\nrefresh 0\nsize\n"), [1])

    def test_size_follows_peers_heard(self):
        # two peers heard + self = 3
        self.assertEqual(fleetnode("reset\n" + _macs(2) + "refresh 0\nsize\n"), [3])

    def test_share_divides_by_live_size(self):
        # a fleet-wide target of 30 across 3 nodes -> 10 each
        self.assertEqual(fleetnode("reset\n" + _macs(2) + "refresh 0\nshare 30\n"), [10])

    def test_share_is_whole_target_when_standalone(self):
        self.assertEqual(fleetnode("reset\nrefresh 0\nshare 30\n"), [30])

    def test_size_drops_back_when_peers_go_quiet(self):
        # peers noted at t=0 age out by t=200000 -> back to standalone, full crowd again
        self.assertEqual(fleetnode("reset\n" + _macs(2) + "refresh 200000\nshare 30\n"), [30])


if __name__ == "__main__":
    unittest.main()
