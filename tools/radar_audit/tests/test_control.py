import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")


def control(sel=2, live=255, flash=0, clear_armed=0, turbo_armed=0,
            pair_state=None, pair_secs=30, rotate_armed=0):
    args = [EXE, "--control", sel, live, flash, clear_armed, turbo_armed]
    # pair_state=None means "don't pass it", which is how a non-provisioned build renders: no
    # PAIR button at all. Passing any value sets pair_shown.
    if pair_state is not None:
        args += [pair_state, pair_secs, rotate_armed]
    out = subprocess.check_output([str(x) for x in args], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class ControlLivePending(unittest.TestCase):
    def test_live_and_pending_labels(self):
        texts = control(sel=2, live=4)
        self.assertTrue(any("LIVE" in t for t in texts), f"drew: {texts}")
        self.assertTrue(any("PENDING" in t for t in texts), f"drew: {texts}")

    def test_live_name_and_pending_box(self):
        # Preset names changed 2026-08-24 with the AUTO/MANUAL split (ordinals 1-4 went
        # STEALTH/NORMAL/DENSE/MAX -> AUTO/LOW/MED/HIGH). sel=2 is LOW, live=4 is HIGH.
        texts = control(sel=2, live=4)          # live HIGH, pending LOW
        self.assertTrue(any("HIGH" in t for t in texts), f"live name; drew: {texts}")
        self.assertTrue(any("LOW" in t for t in texts), f"pending box; drew: {texts}")
        self.assertIn("quarter crowd", texts, f"selected-preset desc; drew: {texts}")

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
class ControlPresetLabels(unittest.TestCase):
    """CTRL_LABELS is indexed by sim_preset_t, so its ORDER is a wire contract
    (config_wire.h, CONFIG_WIRE_VER 2). A reorder here silently remaps every preset a
    Vigil sends to the fleet. See main/settings.h's enum comment."""

    EXPECTED = ["PAUSE", "AUTO", "LOW", "MED", "HIGH", "TURBO"]

    def test_each_ordinal_renders_its_label(self):
        for ordinal, label in enumerate(self.EXPECTED):
            texts = control(sel=ordinal, live=255)
            self.assertTrue(any(f"[ {label} ]" == t for t in texts),
                            f"ordinal {ordinal} should render {label}; drew: {texts}")

    def test_retired_presets_are_gone(self):
        for ordinal in range(len(self.EXPECTED)):
            texts = control(sel=ordinal, live=255)
            for gone in ("STEALTH", "NORMAL", "DENSE"):
                self.assertFalse(any(gone in t for t in texts),
                                 f"retired preset {gone} still rendered at {ordinal}: {texts}")


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


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class ControlPairButton(unittest.TestCase):
    """The PAIR button replaced a >=1.5s press-and-hold. The point of the change is that the
    state is legible, so these assert the label actually tracks the state rather than that a
    button exists somewhere."""

    def test_absent_without_provisioning(self):
        texts = control()
        self.assertFalse(any("PAIR" in t for t in texts),
                         f"a non-provisioned build must draw no PAIR button; drew: {texts}")

    def test_idle_offers_to_open_a_window(self):
        self.assertIn("PAIR NEW NODE", control(pair_state=0))

    def test_open_counts_down(self):
        texts = control(pair_state=1, pair_secs=25)
        self.assertIn("PAIRING 25s", texts, f"drew: {texts}")
        self.assertFalse(any("PAIR NEW NODE" == t for t in texts),
                         "an open window must not still offer to open one")

    def test_rotate_needs_a_confirm(self):
        # Rotation re-keys the whole fleet and drops anything that does not re-enroll, so it is
        # never one tap. Armed, the button must SAY that is what the next tap does.
        texts = control(pair_state=1, pair_secs=25, rotate_armed=1)
        self.assertIn("ROTATE FLEET KEY?", texts, f"drew: {texts}")
        self.assertFalse(any("PAIRING" in t for t in texts),
                         "armed rotate must not also show the countdown label")

    def test_pending_shows_the_whole_fingerprint(self):
        # The accept is TOFU: the operator reads this against the decoy's serial print. A
        # truncated fingerprint would make that comparison weaker than it looks.
        texts = control(pair_state=2)
        self.assertIn("ACCEPT NODE", texts, f"drew: {texts}")
        self.assertIn("b882-fbfe-9628-13d3", texts,
                      f"full fingerprint must be on screen; drew: {texts}")

    def test_pair_does_not_displace_clear_threats(self):
        # The two buttons are 4px apart and CLEAR's tap band used to run to the bottom of the
        # screen. Both must still render.
        texts = control(pair_state=0)
        self.assertTrue(any("CLEAR THREATS" in t for t in texts), f"drew: {texts}")
        self.assertIn("PAIR NEW NODE", texts, f"drew: {texts}")


if __name__ == "__main__":
    unittest.main()
