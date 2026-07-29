import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")

UINT32_MAX = 4294967295


def info(page=0, nodes=3, sigver=2, sigcount=12, linkage=4, libcount=40, libcap=128,
         cardmb=8, sdok=1, decoys=88, target=96, pop=44, uptime=47143):
    """Render RADAR_VIEW_INFO (2-page) and return the text strings drawn."""
    args = [EXE, "--info", page, nodes, sigver, sigcount, linkage, libcount, libcap,
            cardmb, sdok, decoys, target, pop, uptime]
    out = subprocess.check_output([str(x) for x in args], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class InfoSystemPage(unittest.TestCase):
    def test_sections_present(self):
        texts = info(page=0)
        for s in ("FLEET", "SIGNATURES", "STORAGE", "LINK", "SYSTEM"):
            self.assertIn(s, texts, f"missing section {s}; drew: {texts}")

    def test_values(self):
        texts = info(page=0, nodes=3, sigver=2, sigcount=12, cardmb=8, linkage=4)
        i = texts.index("nodes"); self.assertEqual(texts[i + 1], "3", f"drew: {texts}")
        self.assertIn("v2 (12)", texts, f"sig db wrong; drew: {texts}")
        self.assertIn("OK 8MB", texts, f"card wrong; drew: {texts}")
        self.assertIn("4s ago", texts, f"link wrong; drew: {texts}")
        self.assertIn("cydtest", texts, f"firmware tag missing; drew: {texts}")
        self.assertIn("TAP: LEGEND", texts, f"footer hint missing; drew: {texts}")

    def test_link_never(self):
        texts = info(page=0, linkage=UINT32_MAX)
        self.assertIn("never", texts, f"drew: {texts}")


if __name__ == "__main__":
    unittest.main()
