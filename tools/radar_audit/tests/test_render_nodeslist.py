import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")


def run(args):
    return subprocess.check_output([EXE] + args, text=True).splitlines()


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class NodesList(unittest.TestCase):
    def test_eight_nodes_renders_eight_id_labels(self):
        args = ["--nodeslist", "8"]
        for i in range(8):
            args += [str(i), "1", str(10 + i), "0"]   # id=i alive=1 active=10+i battery=0(USB)
        out = run(args)
        for i in range(8):
            self.assertTrue(any(f"N{i}" in line for line in out), f"missing row for node {i}: {out}")

    def test_dead_node_shows_silent(self):
        out = run(["--nodeslist", "1", "3", "0", "0", "0"])   # id=3 alive=0
        self.assertTrue(any("SILENT" in line for line in out))

    def test_alive_node_shows_channel(self):
        out = run(["--nodeslist", "1", "0", "1", "5", "0"])   # id=0 alive=1, no low-batt/degraded flags
        self.assertTrue(any("CHANNEL" in line for line in out))

    def test_zero_nodes_shows_empty_state(self):
        out = run(["--nodeslist", "0"])
        self.assertTrue(any("no nodes" in line.lower() for line in out))

    def test_battery_percent_rendered_when_present(self):
        out = run(["--nodeslist", "1", "0", "1", "5", "3700"])   # battery_mv=3700, battery_pct left 0xFF -> voltage form
        self.assertTrue(any("3.70V" in line or "3.7" in line for line in out))
