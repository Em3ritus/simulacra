import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")


def threat(sel=0, count=1, kind=1, cls=3, cat=1, conf=92, vendor=0x09C8,
           rssi=-55, epochs=4, first=2, last=9, sessions=3, places=3):
    """Render RADAR_VIEW_THREAT and return the list of text strings drawn.
    kind: 1=KNOWN 0=behavioral. cls: sig_class_t. cat: sig_category_t."""
    args = [EXE, "--threat", sel, count, kind, cls, cat, conf, vendor,
            rssi, epochs, first, last, sessions, places]
    out = subprocess.check_output([str(x) for x in args], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class ThreatScaffold(unittest.TestCase):
    def test_header_shows_position(self):
        texts = threat(sel=0, count=3)
        self.assertIn("THREAT 1/3", texts, f"no position header; drew: {texts}")

    def test_sel_negative_shows_gone(self):
        texts = threat(sel=-1)
        self.assertIn("THREAT GONE", texts, f"no gone placeholder; drew: {texts}")


def render(view, *args):
    out = subprocess.check_output([EXE, str(view), *[str(a) for a in args]], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


DETAIL = 2  # radar_view_t: DETAIL=2


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class ThreatBody(unittest.TestCase):
    def test_known_camera_fields(self):
        # kind=1 known, cls=3 Flock, cat=1 CAMERA, conf=92, vendor=0x09C8
        texts = threat(kind=1, cls=3, cat=1, conf=92, vendor=0x09C8, epochs=4, first=2, last=9)
        for tok in ("CAMERA", "Flock", "known", "92%", "0x09C8"):
            self.assertIn(tok, texts, f"missing {tok}; drew: {texts}")
        self.assertIn("e2..e9", texts, f"missing span; drew: {texts}")
        i = texts.index("epochs"); self.assertEqual(texts[i + 1], "4", f"drew: {texts}")

    def test_behavioral_follower_dashes(self):
        # kind=0 behavioral -> class/confidence/vendor show '-'
        texts = threat(kind=0, cls=0, cat=3, conf=0, vendor=0)
        self.assertIn("behavioral", texts, f"drew: {texts}")
        i = texts.index("class"); self.assertEqual(texts[i + 1], "-", f"class not dashed; drew: {texts}")
        j = texts.index("confidence"); self.assertEqual(texts[j + 1], "-", f"conf not dashed; drew: {texts}")
        k = texts.index("vendor"); self.assertEqual(texts[k + 1], "-", f"vendor not dashed; drew: {texts}")

    def test_escalation_verdict(self):
        texts = threat(sessions=3, places=3)   # PERSISTENT (sessions>=3)
        self.assertIn("PERSISTENT", texts, f"drew: {texts}")


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class SurveillanceRowConfidence(unittest.TestCase):
    def test_surveillance_row_shows_confidence(self):
        # DETAIL positional: view, restless,wandering,bound, active,roster,target,
        #                    threats, pop, esc, flags, uptime, ncam
        # 1 threat, 1 camera -> the surveillance row now carries a '%' confidence token.
        texts = render(DETAIL, 1, 1, 1, 8, 16, 8, 1, 10, 0, 0, 0, 1)
        self.assertTrue(any(t.endswith("%") for t in texts),
                        f"no confidence token on surveillance row; drew: {texts}")


if __name__ == "__main__":
    unittest.main()
