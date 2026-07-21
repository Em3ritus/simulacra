import os, re, subprocess, unittest
from collections import Counter

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(TOOL))
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")
POOL_MAX_LEN = 32


def pool_entries():
    out = subprocess.check_output([EXE, "--ssidpool"], text=True).splitlines()
    count = int(out[0])
    entries = []
    for ln in out[1:count + 1]:
        length, name = ln.split(" ", 1)
        entries.append((int(length), name))
    return count, entries


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class SsidPool(unittest.TestCase):
    def test_pool_nonempty(self):
        count, entries = pool_entries()
        self.assertGreater(count, 0)
        self.assertEqual(count, len(entries))

    def test_entries_valid_length(self):
        _, entries = pool_entries()
        for length, name in entries:
            self.assertEqual(length, len(name), f"{name!r} length field mismatch")
            self.assertGreater(length, 0, f"{name!r} empty")
            self.assertLessEqual(length, POOL_MAX_LEN, f"{name!r} exceeds SSID_POOL_MAX_LEN")

    def test_weighted_pick_in_range_and_favors_heavy(self):
        count, _ = pool_entries()
        out = subprocess.check_output([EXE, "--ssidpool", "7", "20000"], text=True).split()
        idx = [int(x) for x in out]
        self.assertTrue(all(0 <= i < count for i in idx), "pick out of range")
        c = Counter(idx)
        self.assertGreater(c[0], c[count - 1], "heaviest-weight entry not favored over lightest")

    def test_source_has_no_observe_or_learn_dependency(self):
        # SAFETY invariant: the pool must never pull from observed/learned traffic.
        forbidden = ("observe.h", "learn.h", "sig_store.h", "wifi_observe.h")
        for fn in ("ssid_pool.c", "ssid_pool.h"):
            with open(os.path.join(ROOT, "main", fn)) as fh:
                text = fh.read()
            includes = re.findall(r'#\s*include\s+"([^"]+)"', text)
            for f in forbidden:
                self.assertNotIn(f, includes, f"{fn} must not include {f}")


if __name__ == "__main__":
    unittest.main()
