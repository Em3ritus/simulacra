import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")


def node(sel=0, id=2, alive=1, age=0, active=8, target=8, roster=16,
         rpa=3, nrpa=2, static=3, pop=10, batt_mv=0, batt_pct=255,
         epoch=5, probes=100, flags=0, uptime=3600, threats=0, ncam=0):
    """Render RADAR_VIEW_NODE and return the list of text strings drawn."""
    args = [EXE, "--node", sel, id, alive, age, active, target, roster,
            rpa, nrpa, static, pop, batt_mv, batt_pct, epoch, probes,
            flags, uptime, threats, ncam]
    out = subprocess.check_output([str(x) for x in args], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class NodeScaffold(unittest.TestCase):
    def test_header_shows_node_id(self):
        texts = node(id=2)
        self.assertIn("NODE N2", texts, f"no node header; drew: {texts}")

    def test_sel_negative_shows_gone_placeholder(self):
        texts = node(sel=-1)
        self.assertIn("NODE GONE", texts, f"no gone placeholder; drew: {texts}")


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class NodeBody(unittest.TestCase):
    def test_sections_present(self):
        texts = node()
        for s in ("CROWD", "POWER", "SYSTEM", "DETECTIONS"):
            self.assertIn(s, texts, f"missing section {s}; drew: {texts}")
        for label in ("decoys", "target", "roster", "rpa/nrpa/static",
                      "real crowd", "battery", "epoch", "probes", "churn", "uptime"):
            self.assertIn(label, texts, f"missing row {label}; drew: {texts}")

    def test_health_channel_when_alive(self):
        self.assertIn("CHANNEL", node(alive=1, flags=0))

    def test_battery_usb(self):
        self.assertIn("USB", node(batt_mv=0))

    def test_battery_pct_format(self):
        self.assertIn("83% 3.9V", node(batt_mv=3900, batt_pct=83))

    def test_battery_voltage_only(self):
        self.assertIn("3.90V", node(batt_mv=3900, batt_pct=255))

    def test_churn_paused(self):
        self.assertIn("PAUSED", node(flags=1))

    def test_detections_partition(self):
        # 3 threats, 1 surveillance -> followers 2, surveillance 1
        texts = node(threats=3, ncam=1)
        i = texts.index("followers"); self.assertEqual(texts[i + 1], "2", f"drew: {texts}")
        j = texts.index("surveillance"); self.assertEqual(texts[j + 1], "1", f"drew: {texts}")


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class NodeSilent(unittest.TestCase):
    def test_silent_subline_shows_age(self):
        texts = node(alive=0, age=45, active=7)
        self.assertIn("SILENT", texts, f"drew: {texts}")
        self.assertIn("seen 45s ago", texts, f"drew: {texts}")

    def test_silent_still_shows_last_values(self):
        # a silent node keeps rendering its last decoy count, not a blank page
        texts = node(alive=0, age=45, active=7)
        i = texts.index("decoys"); self.assertEqual(texts[i + 1], "7", f"drew: {texts}")


if __name__ == "__main__":
    unittest.main()
