import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")


def control(sel=2, live=255, flash=0, clear_armed=0):
    args = [EXE, "--control", sel, live, flash, clear_armed]
    out = subprocess.check_output([str(x) for x in args], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class ControlLivePending(unittest.TestCase):
    def test_live_and_pending_labels(self):
        texts = control(sel=2, live=4)
        self.assertTrue(any("LIVE" in t for t in texts), f"drew: {texts}")
        self.assertTrue(any("PENDING" in t for t in texts), f"drew: {texts}")

    def test_live_name_and_pending_box(self):
        texts = control(sel=2, live=4)          # live MAX, pending NORMAL
        self.assertTrue(any("MAX" in t for t in texts), f"live name; drew: {texts}")
        self.assertTrue(any("NORMAL" in t for t in texts), f"pending box; drew: {texts}")
        self.assertIn("balanced", texts, f"selected-preset desc; drew: {texts}")

    def test_send_when_pending_differs(self):
        self.assertIn("SEND", control(sel=2, live=4), "should read SEND when live!=pending")

    def test_active_when_live_equals_pending(self):
        self.assertIn("ACTIVE", control(sel=4, live=4), "should read ACTIVE when live==pending")

    def test_mixed_live(self):
        self.assertTrue(any("MIXED" in t for t in control(sel=2, live=254)),
                        "0xFE should render MIXED")

    def test_none_live(self):
        texts = control(sel=2, live=255)
        self.assertNotIn("ACTIVE", texts, f"none must not be ACTIVE; drew: {texts}")


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class ControlClearThreats(unittest.TestCase):
    def test_clear_button_present(self):
        self.assertTrue(any("CLEAR THREATS" in t for t in control()),
                        "CLEAR THREATS button should render")

    def test_clear_confirm_when_armed(self):
        texts = control(clear_armed=1)
        self.assertTrue(any("CONFIRM CLEAR?" in t for t in texts),
                        "armed CLEAR should read CONFIRM CLEAR?")
        self.assertFalse(any("CLEAR THREATS" == t for t in texts),
                         "armed CLEAR should not also show the un-armed label")


if __name__ == "__main__":
    unittest.main()
