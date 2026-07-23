import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")

# radar_view_t: HOME=0 RADAR=1 DETAIL=2 STATS=3 LIBRARY=4 CONTROL=5 INFO=6
STATS = 3


def render(view, *args):
    """Return the list of text strings drawn by a view (the part after 'TXT x y ')."""
    out = subprocess.check_output([EXE, str(view), *[str(a) for a in args]], text=True)
    texts = []
    for ln in out.splitlines():
        if ln.startswith("TXT "):
            texts.append(ln.split(" ", 3)[3])   # drop 'TXT', x, y
    return texts


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class StatsFormBreakdown(unittest.TestCase):
    def test_stats_shows_form_breakdown_values(self):
        # restless=5 wandering=3 bound=8 -> the DECOYS/STATS page must surface the RPA/NRPA/static split.
        texts = render(STATS, 5, 3, 8)
        joined = " | ".join(texts)
        self.assertTrue(any("rpa" in t.lower() for t in texts),
                        f"no form-breakdown row found; drew: {joined}")
        row = next(t for t in texts if "rpa" in t.lower())
        self.assertIn("5", row, f"restless count missing: {row!r}")
        self.assertIn("3", row, f"wandering count missing: {row!r}")
        self.assertIn("8", row, f"bound count missing: {row!r}")

    def test_stats_still_shows_existing_rows(self):
        # regression: adding the form row must not drop the decoy/pop rows.
        texts = render(STATS, 5, 3, 8, 12, 16, 10)
        joined = " ".join(texts).lower()
        self.assertIn("decoy", joined, "STATS lost its title/decoy row")

    def test_data_views_share_back_and_title_header(self):
        # STATS/DETAIL/LIBRARY/INFO get the shared themed header: a "< BACK" affordance + page title,
        # matching HOME/CONTROL. (threat_count=0 so DETAIL renders its header path.)
        for view, title in [(2, "FOLLOWERS"), (3, "DECOYS"), (4, "LIBRARY"), (6, "INFO")]:
            texts = render(view, 1, 1, 1, 4, 8, 4, 0)
            joined = " | ".join(texts)
            self.assertIn("< BACK", texts, f"view {view} missing BACK affordance; drew: {joined}")
            self.assertTrue(any(title in t for t in texts),
                            f"view {view} missing title {title!r}; drew: {joined}")

    def test_all_views_render_without_crash(self):
        # smoke: every view renders (harness exits 0) for a representative populated status.
        for v in range(0, 7):
            subprocess.check_call([EXE, str(v), "5", "3", "8", "12", "16", "10", "2"],
                                  stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main()
