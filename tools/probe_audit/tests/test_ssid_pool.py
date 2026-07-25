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


def render(idx, seed=0x1234):
    out = subprocess.check_output([EXE, "--ssidrender", str(idx), format(seed, "x")], text=True).strip()
    parts = out.split(" ")
    return int(parts[0]), int(parts[1]), " ".join(parts[2:])   # style, length, name


def index_of(name):
    _, entries = pool_entries()
    for i, (_ln, n) in enumerate(entries):
        if n == name:
            return i
    return -1


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class SsidSuffix(unittest.TestCase):
    def test_hex2_suffix(self):
        i = index_of("spectrumsetup"); self.assertGreaterEqual(i, 0)
        _, _, name = render(i, 0x00ab)
        self.assertRegex(name, r"^spectrumsetup-[0-9a-f]{2}$")

    def test_digit_suffix(self):
        i = index_of("NETGEAR"); self.assertGreaterEqual(i, 0)
        _, _, name = render(i, 0x0005)
        self.assertRegex(name, r"^NETGEAR\d{2,3}$")

    def test_none_style_renders_bare(self):
        i = index_of("Guest"); self.assertGreaterEqual(i, 0)
        self.assertEqual(render(i, 0x1234)[2], "Guest")

    def test_render_deterministic_for_a_seed(self):
        i = index_of("setup"); self.assertGreaterEqual(i, 0)
        self.assertEqual(render(i, 0xbeef)[2], render(i, 0xbeef)[2])

    def test_all_renders_within_max_len_and_length_matches(self):
        count, _ = pool_entries()
        for i in range(count):
            _, length, name = render(i, 0xffff)
            self.assertLessEqual(length, POOL_MAX_LEN, f"entry {i} too long")
            self.assertEqual(length, len(name), f"entry {i} length mismatch")


if __name__ == "__main__":
    unittest.main()
