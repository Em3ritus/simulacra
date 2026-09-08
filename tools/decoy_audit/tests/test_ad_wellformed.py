import os, subprocess, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.abspath(os.path.join(HERE, ".."))
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def strip(hex_ad, company="ffff"):
    """True if learn_strip would ADOPT this advert as a template."""
    out = subprocess.check_output([EXE, "--adstrip", hex_ad, company], text=True).strip()
    return out == "ADOPT"


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class RefuseStructurallyImpossibleAdverts(unittest.TestCase):
    """The learner must not adopt an advert that violates the AD spec.

    Simulacra learns company ids and AD shapes from ambient traffic, which means a badly-implemented
    real device in the room becomes a decoy template. That is fine when the device is merely unusual;
    it is not fine when the device is structurally impossible, because impossibility is exactly what
    a passive detector tests for. Cloning such a device makes every decoy wearing that template
    trivially separable from real traffic -- by a one-line check that costs an observer nothing.

    The two payloads below are REAL, observed during bench testing against a passive BLE detector.
    Both came from genuine consumer hardware, and the detector treated each as a conclusive
    indicator of synthetic traffic. It did NOT flag Simulacra -- these tests exist so that stays
    true after the learner meets one of these devices.
    """

    # A real consumer device, address withheld: its company id 0x856C sits outside the SIG's
    # assigned range, AND the
    # 128-bit-UUID element carries ASCII text ("ELBahoriA..-MIRP") where 16 binary bytes belong.
    HONEYPOT_FULL  = "02010609ff6c858b300fbd75821107454c4261686f72694103ab2d4d495250"
    HONEYPOT_MFG   = "02010609ff6c858b300fbd7582"
    HONEYPOT_UUID  = "0201061107454c4261686f72694103ab2d4d495250"

    def test_adopts_unassigned_company_id_deliberately(self):
        """The honeypot's company-id recommendation was tested and NOT adopted. This pins that.

        Its report advises rejecting company ids outside the SIG's assigned range, on the reasoning
        that no real device can hold one. Measured against 452,462 ambient adverts from this
        project's own captures, that is false: 0x4D48 ("MH"), 0x3030 ("00") and 0x4556 ("EV") are
        ordinary sightings -- real products with ASCII stuffed into the field -- and the 0x8000+
        range is populated too. Enforcing a ceiling rejected 32% of one capture.

        The detector's own control run argues the same way from the other side: all three devices
        it flagged on this rule proved to be real hardware. An unassigned company id marks
        traffic as sloppy, not synthetic, and the room is full of sloppy. Modelling only tidy
        devices would make the decoy population cleaner than its surroundings -- a tell in the
        direction that matters most here.

        If this test ever fails, someone reinstated the ceiling. Check it against real captures
        before keeping it.
        """
        self.assertTrue(
            strip(self.HONEYPOT_MFG, "856c"),
            "rejected a template for its company id alone. Unassigned ids are common in real "
            "ambient traffic; refusing them costs diversity and buys nothing.")

    def test_rejects_ascii_text_in_a_binary_uuid_field(self):
        self.assertFalse(
            strip(self.HONEYPOT_UUID),
            "adopted a template with ASCII text in a 128-bit-UUID element. That field is 16 bytes "
            "of binary by definition; readable words in it are conclusive evidence of a generator.")

    def test_rejects_the_real_honeypot_advert(self):
        # Rejected for its ASCII-filled UUID element, not for its company id -- see the test above.
        self.assertFalse(
            strip(self.HONEYPOT_FULL, "856c"),
            "adopted the exact advert a passive honeypot flagged as conclusively synthetic.")

    def test_rejects_uuid_elements_of_impossible_length(self):
        # 128-bit UUID list carrying 15 bytes: not a whole number of UUIDs, so it cannot be real.
        self.assertFalse(strip("0201061007454c4261686f72694103ab2d4d4952"),
                         "adopted a 128-bit-UUID element that is not a multiple of 16 bytes")
        # 16-bit UUID list carrying 3 bytes: same fault at the other size.
        self.assertFalse(strip("0201060403d6fd12"),
                         "adopted a 16-bit-UUID element that is not a multiple of 2 bytes")


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class StillAdoptsGenuineAdverts(unittest.TestCase):
    """The gate must reject impossible structures WITHOUT narrowing what the learner can model.

    A well-formedness check that also throws away legitimate variety would trade one tell for a
    worse one: a decoy population less diverse than the room it is hiding in. These are the shapes
    the learner must keep taking.
    """

    def test_adopts_apple_manufacturer_advert(self):
        self.assertTrue(strip("02011a0aff4c0010050318c0a8b2", "004c"), "rejected a real Apple advert")

    def test_adopts_samsung_manufacturer_advert(self):
        self.assertTrue(strip("02011a09ff7500420409a01b23", "0075"), "rejected a real Samsung advert")

    def test_adopts_well_formed_128bit_uuid(self):
        # A genuine 16-byte binary UUID must still be adopted -- the ASCII check must key on
        # printability, not on the element type.
        self.assertTrue(strip("020106110700112233445566778899aabbccddeeff"),
                        "rejected a well-formed 128-bit UUID element")

    def test_adopts_eddystone_service_data(self):
        self.assertTrue(strip("0201060303aafe1016aafe10f00263686172676570616400"),
                        "rejected a well-formed Eddystone-URL advert")

    def test_adopts_flags_only_advert(self):
        self.assertTrue(strip("020106"), "rejected a bare flags-only advert")


if __name__ == "__main__":
    unittest.main()
