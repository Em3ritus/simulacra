import os, subprocess, unittest
HERE=os.path.dirname(__file__); TOOL=os.path.dirname(HERE)
EXE=os.path.join(TOOL,"fleet_dump.exe" if os.name=="nt" else "fleet_dump")
def run(s): return subprocess.check_output([EXE]+s.split(),text=True).strip().splitlines()

@unittest.skipUnless(os.path.exists(EXE),"fleet_dump not built")
class FS(unittest.TestCase):
    def test_upsert_counts_distinct_nodes(self):
        # upsert node 5 (12 devices) then node 7 (8) -> count 2
        self.assertEqual(run("up 5 12 up 7 8 count"), ["2"])
    def test_upsert_same_node_updates_not_adds(self):
        self.assertEqual(run("up 5 12 up 5 16 count at0"), ["1","id=5 dev=16 alive=1"])
    def test_stale_node_reads_not_alive(self):
        # upsert node 5, advance past stale, query
        self.assertEqual(run("up 5 12 wait at0"), ["id=5 dev=12 alive=0"])
    def test_aggregate_sums_devices(self):
        # fleet-wide DECOYS = sum across alive nodes (12 + 8 + 6 = 26)
        self.assertEqual(run("up 0 12 up 1 8 up 2 6 agg"), ["dev=26 tc=0"])
    def test_aggregate_unions_threats_keeping_closest(self):
        # same follower (deadbeef) seen by two nodes -> ONE fleet threat, closest RSSI (-30 > -50)
        self.assertEqual(run("upt 0 12 deadbeef -50 upt 1 8 deadbeef -30 agg"),
                         ["dev=20 tc=1 deadbeef@-30"])
    def test_aggregate_distinct_threats_union(self):
        # two different followers across nodes -> both appear
        out = run("upt 0 12 aaaa -40 upt 1 8 bbbb -60 agg")[0]
        self.assertTrue(out.startswith("dev=20 tc=2"))
        self.assertIn("0000aaaa@-40", out); self.assertIn("0000bbbb@-60", out)
    def test_aggregate_excludes_stale(self):
        # a stale node contributes nothing to the aggregate
        self.assertEqual(run("up 0 12 wait up 1 8 agg"), ["dev=8 tc=0"])
    def test_aggregate_preset_all_agree(self):
        # two alive nodes both preset 4 (MAX) -> fleet preset 4
        self.assertEqual(run("upp 0 8 4 upp 1 8 4 aggp"), ["preset=4"])
    def test_aggregate_preset_mixed(self):
        # differing presets -> MIXED (0xFE = 254)
        self.assertEqual(run("upp 0 8 4 upp 1 8 2 aggp"), ["preset=254"])
    def test_aggregate_preset_none_when_empty(self):
        # no alive nodes -> none (0xFF = 255)
        self.assertEqual(run("aggp"), ["preset=255"])
    def test_aggregate_preset_excludes_stale(self):
        # a stale node's preset does not count; only the alive node (preset 3) remains
        self.assertEqual(run("upp 0 8 4 wait upp 1 8 3 aggp"), ["preset=3"])
