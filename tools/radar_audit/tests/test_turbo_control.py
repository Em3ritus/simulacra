import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")


def control(sel=2, live=255, flash=0, clear_armed=0, turbo_armed=0):
    args = [EXE, "--control", sel, live, flash, clear_armed, turbo_armed]
    out = subprocess.check_output([str(x) for x in args], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class TurboControl(unittest.TestCase):
    def test_turbo_is_the_sixth_preset(self):
        # preset id 5 (SIM_PRESET_TURBO) must render as "TURBO" in the pending box, cycling around
        # from PAUSE(0)..MAX(4) rather than wrapping back to PAUSE at 5.
        texts = control(sel=5, live=0xFF)
        self.assertTrue(any("TURBO" in t for t in texts), f"drew: {texts}")

    def test_live_turbo_shows_in_the_live_slot(self):
        texts = control(sel=2, live=5)
        self.assertTrue(any("TURBO" in t for t in texts), f"live TURBO not shown; drew: {texts}")

    def test_send_shows_confirm_when_turbo_pending_and_armed(self):
        texts = control(sel=5, live=0xFF, turbo_armed=1)
        self.assertTrue(any("CONFIRM" in t for t in texts), f"drew: {texts}")

    def test_send_shows_plain_send_when_turbo_pending_but_not_armed(self):
        texts = control(sel=5, live=0xFF, turbo_armed=0)
        self.assertIn("SEND", texts, f"drew: {texts}")
        self.assertFalse(any("CONFIRM" in t for t in texts), f"should not be armed yet; drew: {texts}")

    def test_non_turbo_presets_unaffected_by_turbo_armed(self):
        # turbo_armed must only change rendering when TURBO is actually the pending preset.
        texts = control(sel=2, live=0xFF, turbo_armed=1)
        self.assertIn("SEND", texts, f"drew: {texts}")


if __name__ == "__main__":
    unittest.main()
