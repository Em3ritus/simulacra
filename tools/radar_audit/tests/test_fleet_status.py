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


@unittest.skipUnless(os.path.exists(EXE), "fleet_dump not built")
class FutureStamp(unittest.TestCase):
    """A record stamped slightly in the FUTURE must read as fresh, not ancient.

    The Vigil samples `now` once per UI frame but stamps node records later in the same frame (the
    ESP-NOW drain runs mid-loop), so last_ms legitimately exceeds now by a few ms. Unsigned
    subtraction turned that into ~49 days: every node that had just reported read STALE, the fleet
    aggregate emptied to 0 devices / 0 threats for one frame every second, and the display flipped
    RADAR -> HOME -> RADAR continuously."""

    def test_aggregate_keeps_nodes_stamped_in_the_future(self):
        # two nodes reported, then the observer clock is 5 ms BEHIND their stamps
        out = run("up 0 32 up 1 24 back 5 agg")
        self.assertEqual(out[-1], "dev=56 tc=0", f"future-stamped nodes dropped out: {out[-1]}")

    def test_node_stamped_in_the_future_is_alive(self):
        out = run("up 0 32 back 5 at0")
        self.assertIn("alive=1", out[-1], f"future-stamped node read as dead: {out[-1]}")

    def test_genuinely_stale_nodes_still_drop_out(self):
        """The guard must not turn into 'everything is always alive'."""
        out = run("up 0 32 wait agg")
        self.assertEqual(out[-1], "dev=0 tc=0", f"stale node still counted: {out[-1]}")


@unittest.skipUnless(os.path.exists(EXE), "fleet_dump not built")
class Prune(unittest.TestCase):
    """Long-gone nodes must be retired, or their SILENT cards occupy HOME's three card slots and
    push a LIVE node off the display -- a board that looks dropped while it is still meshing.
    Decoys re-randomise their MAC on every boot, so each reboot leaves one of these behind."""

    def test_prune_drops_long_silent_nodes(self):
        # node 0 goes quiet, node 1 reports 70 s later; prune at 60 s retires only node 0
        out = run("up 0 32 adv 70000 up 1 24 prune 60000 count")
        self.assertEqual(out[-1], "1", "a node silent past the prune age should be retired")

    def test_prune_keeps_merely_stale_nodes(self):
        """A node that is SILENT but recent must stay: the card is informative, not noise."""
        out = run("up 0 32 up 1 24 adv 20000 prune 60000 count")
        self.assertEqual(out[-1], "2", "a briefly quiet node must still show as SILENT")

    def test_pruned_slot_frees_room_for_a_live_node(self):
        out = run("up 0 32 adv 70000 up 1 24 up 2 8 up 3 8 prune 60000 count")
        self.assertEqual(out[-1], "3", "pruning must free the dead slot for live nodes")
