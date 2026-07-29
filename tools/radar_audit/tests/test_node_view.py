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


if __name__ == "__main__":
    unittest.main()
