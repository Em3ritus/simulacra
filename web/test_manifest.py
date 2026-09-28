import json, os, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
EXPECTED = {"ESP32-C5": "firmware/decoy-c5.bin",
            "ESP32-C6": "firmware/decoy-c6.bin",
            "ESP32":    "firmware/cyd.bin"}


class Manifest(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(HERE, "manifest.json")) as f:
            self.m = json.load(f)

    def test_has_name_and_version(self):
        self.assertTrue(self.m.get("name"))
        self.assertTrue(self.m.get("version"))

    def test_three_expected_chip_families(self):
        fams = {b["chipFamily"] for b in self.m["builds"]}
        self.assertEqual(fams, set(EXPECTED), f"chipFamily set wrong: {fams}")

    def test_each_build_single_part_at_zero(self):
        for b in self.m["builds"]:
            self.assertEqual(len(b["parts"]), 1, f"{b['chipFamily']} must be one merged part")
            self.assertEqual(b["parts"][0]["offset"], 0, f"{b['chipFamily']} part offset must be 0")
            self.assertEqual(b["parts"][0]["path"], EXPECTED[b["chipFamily"]],
                             f"{b['chipFamily']} path wrong")

    def test_bins_exist_and_nonempty_when_built(self):
        for b in self.m["builds"]:
            p = os.path.join(HERE, b["parts"][0]["path"])
            if not os.path.exists(p):
                self.skipTest("firmware not built yet (run build_flasher.ps1)")
            self.assertGreater(os.path.getsize(p), 0, f"{p} is empty")

    def test_page_references_manifest(self):
        with open(os.path.join(HERE, "index.html")) as f:
            html = f.read()
        self.assertIn('prepareManifest("manifest.json"', html,
                      "the page must feed manifest.json through the key-patching step")

    def test_flasher_library_is_the_vendored_build(self):
        """The released esp-web-tools cannot identify ESP32-C5 rev v1.2, so the page must load the
        vendored build made against esptool-js 0.7.0 (issue #13). Going back to a CDN copy, or
        vendoring a build without the fix, silently breaks every current C5 again."""
        with open(os.path.join(HERE, "index.html")) as f:
            html = f.read()
        self.assertIn('src="vendor/esp-web-tools/install-button.js"', html)
        self.assertNotIn("unpkg.com/esp-web-tools", html, "flasher library must not come from a CDN")
        vend = os.path.join(HERE, "vendor", "esp-web-tools")
        self.assertTrue(os.path.exists(os.path.join(vend, "install-button.js")))
        self.assertTrue(os.path.exists(os.path.join(vend, "LICENSE")), "Apache-2.0 needs its LICENSE")
        bundle = "".join(open(os.path.join(vend, n), encoding="utf-8").read()
                         for n in os.listdir(vend) if n.endswith(".js"))
        self.assertIn("820080751", bundle, "vendored bundle lacks the C5 v1.2 chip magic 0x30e1706f")

    def test_install_button_has_no_static_manifest(self):
        """Fail safe. The button's only manifest is the blob the Prepare step builds, so a skipped
        or failed key step cannot fall back to flashing the published images. Those carry a
        placeholder secret whose public half does not match the decoys', so an unpatched fleet has
        a control plane that silently does nothing."""
        with open(os.path.join(HERE, "index.html")) as f:
            html = f.read()
        self.assertNotIn('manifest="manifest.json"', html,
                         "install button must not have a static manifest to fall back to")

    def test_key_modules_are_wired_up(self):
        """The safety property of this page is that the key is made locally and patched in before
        flashing. If these stop being imported the page still flashes and silently stops being
        safe, which is the failure worth a test."""
        with open(os.path.join(HERE, "index.html")) as f:
            html = f.read()
        for mod in ("./keygen.js", "./flash.js"):
            self.assertIn(mod, html, f"{mod} is no longer loaded by the page")
        for name in ("keygen.js", "flash.js", "keypatch.js"):
            self.assertTrue(os.path.exists(os.path.join(HERE, name)), f"{name} is missing")

    def test_every_build_declares_which_key_it_takes(self):
        """flash.js patches by this field. A build without one is passed through unpatched, which
        would ship a board keyed to a published placeholder."""
        for b in self.m["builds"]:
            self.assertIn(b.get("simulacra_key"), ("ctrl_pk", "ctrl_sk"),
                          f"{b['chipFamily']} declares no usable simulacra_key")
        roles = {b["chipFamily"]: b["simulacra_key"] for b in self.m["builds"]}
        self.assertEqual(roles["ESP32"], "ctrl_sk", "the CYD is the Vigil and holds the secret")
        self.assertEqual(roles["ESP32-C5"], "ctrl_pk", "decoys verify only, never sign")
        self.assertEqual(roles["ESP32-C6"], "ctrl_pk", "decoys verify only, never sign")


if __name__ == "__main__":
    unittest.main()
