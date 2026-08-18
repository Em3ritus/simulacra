import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def match(machex):
    out = subprocess.check_output([EXE, "--surveiloui", machex], text=True).split()
    return int(out[0]), int(out[1]), int(out[2])   # matched(0/1), class_id, category


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class Surveil(unittest.TestCase):
    def test_flock_oui_matches_camera(self):
        m, cls, cat = match("b41e52aabbcc")
        # SIG_CLASS_FLOCK = 3, SIG_CAT_CAMERA = 1
        self.assertEqual((m, cls, cat), (1, 3, 1))

    def test_axon_oui_matches_bodycam(self):
        m, cls, cat = match("0025dfaabbcc")
        # SIG_CLASS_AXON = 4, SIG_CAT_BODYCAM = 2
        self.assertEqual((m, cls, cat), (1, 4, 2))

    def test_espressif_does_not_match(self):
        self.assertEqual(match("a4cf12aabbcc")[0], 0)   # generic module vendor -> no false positive

    def test_liteon_does_not_match(self):
        self.assertEqual(match("70c94eaabbcc")[0], 0)

    def test_random_does_not_match(self):
        self.assertEqual(match("123456789abc")[0], 0)


def ssid_match(s):
    out = subprocess.check_output([EXE, "--surveilssid", s], text=True).split()
    return int(out[0]), int(out[1]), int(out[2])   # matched(0/1), class_id, category


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class SurveilSsid(unittest.TestCase):
    def test_test_flck_matches_flock_camera(self):
        # SIG_CLASS_FLOCK = 3, SIG_CAT_CAMERA = 1
        self.assertEqual(ssid_match("test_flck"), (1, 3, 1))

    def test_other_ssid_does_not_match(self):
        self.assertEqual(ssid_match("attwifi")[0], 0)

    def test_length_prefix_does_not_match(self):
        # exact-length match: an 8-char prefix of the 9-char name must NOT match
        self.assertEqual(ssid_match("test_flc")[0], 0)

    def test_flock_hotspot_prefix_matches(self):
        # real observed instance from production firmware (GainSec / BirdShot DEF CON 34 research),
        # CVE-2025-47818 -- SIG_CLASS_FLOCK = 3, SIG_CAT_CAMERA = 1
        self.assertEqual(ssid_match("Flock-230503"), (1, 3, 1))

    def test_flock_prefix_matches_bare_dash(self):
        self.assertEqual(ssid_match("Flock-"), (1, 3, 1))

    def test_flock_prefix_requires_the_dash(self):
        # "Flock" alone, with nothing after it, is not the hotspot's naming convention
        self.assertEqual(ssid_match("Flock")[0], 0)

    def test_flock_prefix_is_case_sensitive(self):
        self.assertEqual(ssid_match("flock-230503")[0], 0)


def name_match(s):
    out = subprocess.check_output([EXE, "--surveilname", s], text=True).split()
    return int(out[0]), int(out[1]), int(out[2])   # matched(0/1), class_id, category


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class SurveilName(unittest.TestCase):
    def test_flock_substring_matches_camera(self):
        # Raven's own BLE advertised local name contains "flock" (GainSec / BirdShot DEF CON 34
        # research, raven_ble.py's discovery filter, confirmed against real hardware).
        # SIG_CLASS_FLOCK = 3, SIG_CAT_CAMERA = 1
        self.assertEqual(name_match("Flock Raven-1A2B"), (1, 3, 1))

    def test_matches_is_case_insensitive(self):
        self.assertEqual(name_match("FLOCK-DEVICE")[0], 1)
        self.assertEqual(name_match("myflockthing")[0], 1)

    def test_other_name_does_not_match(self):
        self.assertEqual(name_match("Galaxy Buds")[0], 0)

    def test_empty_name_does_not_match(self):
        self.assertEqual(name_match("")[0], 0)
