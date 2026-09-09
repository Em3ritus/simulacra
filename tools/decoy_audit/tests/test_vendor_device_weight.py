import os, subprocess, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.abspath(os.path.join(HERE, ".."))
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def weights(devices, adverts_each):
    """(chatty_vendor_count, quiet_vendor_count) after both vendors get `devices` devices."""
    out = subprocess.check_output([EXE, "--vendorweight", str(devices), str(adverts_each)], text=True)
    a, b = out.split()
    return int(a), int(b)


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class VendorHistogramIsDeviceWeighted(unittest.TestCase):
    """The vendor census must count DEVICES, not advertisements.

    The vendor histogram decides which company ids the generator emits, so its weighting decides
    what the synthetic crowd claims to be. Counting adverts answers "which vendor talks most",
    which is a different question with a very different answer.

    Measured on air overnight 2026-09-09: one chatty printer emitting 1400 advertisements became
    96.9% of the model, in a room whose five devices were 20% that vendor. GEN_MAX_VENDOR_PCT then
    clamped the generated crowd to its 40% ceiling, so the output looked plausible while the model
    underneath was wrong by a factor of five -- the guard absorbing an error it exists to backstop,
    not to correct.

    tools/decoy_audit fixed the same defect on the analysis side (capture_profile.py device-weights
    atype and the vendor histogram, having measured an 11x distortion). This pins the firmware half.
    """

    def test_chattiness_does_not_change_the_census(self):
        """Same device count, wildly different advert counts -> same learned weight."""
        for each in (1, 10, 100, 1000):
            chatty, quiet = weights(5, each)
            self.assertEqual(
                chatty, quiet,
                "with %d adverts per device the chatty vendor scored %d vs %d for the quiet one. "
                "Both have 5 devices, so both must weigh 5: the census is counting adverts."
                % (each, chatty, quiet))

    def test_census_equals_device_count(self):
        for devs in (1, 3, 12):
            chatty, quiet = weights(devs, 50)
            self.assertEqual((chatty, quiet), (devs, devs),
                             "expected %d/%d devices, model learned %d/%d"
                             % (devs, devs, chatty, quiet))

    def test_the_printer_scenario(self):
        """The exact shape that produced the finding: 1 chatty device vs 4 quiet ones."""
        # 1 device x 1400 adverts against 1 device x 1 advert. Advert weighting gives the chatty
        # vendor 99.9%; device weighting gives it 50%.
        chatty, quiet = weights(1, 1400)
        share = chatty / float(chatty + quiet)
        self.assertLess(share, 0.60,
                        "one chatty device took %.1f%% of the census against one quiet device. "
                        "Advert weighting would give it 99.9%%." % (100 * share))


if __name__ == "__main__":
    unittest.main()
