import json, os, re, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
# Keyed by (chipFamily, panel) because chipFamily alone is no longer unique: both CYD board
# revisions are a plain ESP32 and differ only in their display controller, so esp-web-tools cannot
# tell them apart and the user picks. flash.js keeps only the build whose panel was chosen, which
# is what stops two ESP32 builds reaching esp-web-tools (it would silently take the first).
EXPECTED = {("ESP32-C5", None): "firmware/decoy-c5.bin",
            ("ESP32-C6", None): "firmware/decoy-c6.bin",
            ("ESP32", "ili9341"): "firmware/cyd.bin",
            ("ESP32", "st7789"): "firmware/cyd-st7789.bin"}

PANELS = sorted(p for (_, p) in EXPECTED if p)


def key(b):
    return (b["chipFamily"], b.get("simulacra_panel"))


class Manifest(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(HERE, "manifest.json")) as f:
            self.m = json.load(f)

    def test_has_name_and_version(self):
        self.assertTrue(self.m.get("name"))
        self.assertTrue(self.m.get("version"))

    def test_expected_builds(self):
        got = {key(b) for b in self.m["builds"]}
        self.assertEqual(got, set(EXPECTED), f"build set wrong: {got}")

    def test_each_build_single_part_at_zero(self):
        for b in self.m["builds"]:
            k = key(b)
            self.assertEqual(len(b["parts"]), 1, f"{k} must be one merged part")
            self.assertEqual(b["parts"][0]["offset"], 0, f"{k} part offset must be 0")
            self.assertEqual(b["parts"][0]["path"], EXPECTED[k], f"{k} path wrong")

    def test_only_the_cyd_builds_declare_a_panel(self):
        """A panel field on a decoy would be filtered against the user's CYD choice and could drop
        the decoy build entirely; a CYD build without one can never be selected."""
        for b in self.m["builds"]:
            if b["chipFamily"] == "ESP32":
                self.assertIn(b.get("simulacra_panel"), PANELS,
                              f"CYD build declares no known panel: {b.get('simulacra_panel')}")
            else:
                self.assertIsNone(b.get("simulacra_panel"),
                                  f"{b['chipFamily']} is not a CYD and must not declare a panel")

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
        roles = {key(b): b["simulacra_key"] for b in self.m["builds"]}
        for panel in PANELS:
            self.assertEqual(roles[("ESP32", panel)], "ctrl_sk",
                             f"the {panel} CYD is a Vigil and holds the secret")
        self.assertEqual(roles[("ESP32-C5", None)], "ctrl_pk", "decoys verify only, never sign")
        self.assertEqual(roles[("ESP32-C6", None)], "ctrl_pk", "decoys verify only, never sign")

    def test_page_offers_exactly_the_panels_the_manifest_ships(self):
        """The chooser and the manifest have to agree. A panel offered on the page with no build
        behind it prepares nothing and the flash button never appears; a build with no way to pick
        it is dead weight the user cannot reach."""
        with open(os.path.join(HERE, "index.html")) as f:
            html = f.read()
        offered = set(re.findall(r'name="cyd-panel"[^>]*\bvalue="([a-z0-9]+)"', html))
        self.assertEqual(offered, set(PANELS), f"page offers panels {offered}, manifest has {set(PANELS)}")
        self.assertIn("simulacra_panel", open(os.path.join(HERE, "flash.js")).read(),
                      "flash.js no longer filters builds by panel, so both CYD images would be "
                      "handed to esp-web-tools and it would flash whichever came first")


if __name__ == "__main__":
    unittest.main()
