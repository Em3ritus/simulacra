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
        self.assertIn('manifest="manifest.json"', html)
        self.assertIn("esp-web-tools@10", html)


if __name__ == "__main__":
    unittest.main()
