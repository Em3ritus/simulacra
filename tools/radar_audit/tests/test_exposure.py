import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "expo_dump.exe" if os.name == "nt" else "expo_dump")


def run(script):
    """Feed a scripted event stream; return the final result line as a dict.
    Commands (one per line): start <ms> | probe <fp_hex> <ms> [ssid] | tick <ms>."""
    out = subprocess.run([EXE], input=script, capture_output=True, text=True).stdout.strip().splitlines()
    last = out[-1]
    d = dict(tok.split("=", 1) for tok in last.split()[1:])
    d["ssids"] = [s for s in d.get("ssids", "").split(",") if s]
    return d


@unittest.skipUnless(os.path.exists(EXE), "expo_dump not built")
class Exposure(unittest.TestCase):
    def test_toggle_burst_device_wins(self):
        # dev 0xAA probes steadily; dev 0xBB (the phone) is quiet in baseline then bursts after toggle.
        s = "start 0\n"
        s += "".join(f"probe 0xAA {t}\n" for t in range(0, 4000, 500))       # baseline: AA active
        s += "probe 0xBB 100\n"                                              # BB barely seen in baseline
        s += "tick 4001\n"                                                   # -> WATCH
        s += "".join(f"probe 0xBB {t} HomeWiFi\n" for t in range(4100, 5100, 100))  # BB bursts, names a net
        s += "probe 0xAA 4600\n"                                             # AA trickles (no spike)
        s += "tick 10002\n"                                                  # -> RESULT
        d = run(s)
        self.assertEqual(d["state"], "RESULT")
        self.assertEqual(d["ambiguous"], "0")
        self.assertEqual(d["winner_fp"].lower(), "0xbb")
        self.assertIn("HomeWiFi", d["ssids"])

    def test_busiest_baseline_does_not_win_without_spike(self):
        s = "start 0\n" + "".join(f"probe 0xAA {t}\n" for t in range(0, 4000, 200))  # AA very busy baseline
        s += "tick 4001\n" + "probe 0xAA 5000\n" + "tick 10002\n"                     # AA no watch spike
        self.assertEqual(run(s)["ambiguous"], "1", "no post-toggle spike -> ambiguous")

    def test_wildcard_phone_identified_with_empty_ssids(self):
        s = "start 0\nprobe 0xBB 100\ntick 4001\n"
        s += "".join(f"probe 0xBB {t}\n" for t in range(4100, 5100, 100))            # burst, no SSID
        s += "tick 10002\n"
        d = run(s)
        self.assertEqual(d["winner_fp"].lower(), "0xbb")
        self.assertEqual(d["ssids"], [])

    def test_ssids_deduped(self):
        s = "start 0\nprobe 0xBB 100\ntick 4001\n"
        s += "".join(f"probe 0xBB {t} CoffeeShop\n" for t in range(4100, 4600, 100))  # same SSID repeated
        s += "tick 10002\n"
        self.assertEqual(run(s)["ssids"], ["CoffeeShop"])


if __name__ == "__main__":
    unittest.main()
