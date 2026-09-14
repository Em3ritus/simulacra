"""The image patcher must repair BOTH integrity values, or the board silently will not boot.

These run against a synthetic ESP app image rather than a real firmware build, so they work in CI
where no build artifacts exist. The format details they encode were all learned the hard way on
hardware: a patched image that leaves either value stale boots the bootloader, prints the partition
table, and then stops with nothing on the console.
"""
import hashlib
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import patch_keyblock as P  # noqa: E402


def make_image(payload, hash_appended=True):
    """Build a minimal but structurally real ESP app image around `payload`."""
    seg = bytes(payload)
    hdr = bytearray(24)
    hdr[0] = 0xE9          # magic
    hdr[1] = 1             # one segment
    hdr[23] = 1 if hash_appended else 0
    body = bytearray(hdr)
    body += struct.pack("<II", 0x40080000, len(seg))
    body += seg
    pad = 15 - (len(body) % 16)
    body += b"\x00" * pad
    xor = 0xEF
    for b in seg:
        xor ^= b
    body.append(xor)
    if hash_appended:
        body += hashlib.sha256(bytes(body)).digest()
    return bytes(body)


class PatcherRepairsIntegrity(unittest.TestCase):
    def setUp(self):
        magic, size = P.MAGICS["ctrl_pk"]
        self.payload = b"\xaa" * 64 + magic + bytes(size) + b"\xbb" * 64
        self.img = make_image(self.payload)

    def test_reference_image_is_valid(self):
        self.assertTrue(P.verify(self.img), "test fixture is not a valid image")

    def test_patch_repairs_both_values(self):
        out, n = P.patch(self.img, "ctrl_pk", bytes(range(32)))
        self.assertEqual(n, 1)
        self.assertTrue(P.verify(out),
                        "patched image fails its own integrity check: the bootloader would refuse "
                        "to start it, with nothing printed to explain why")

    def test_patch_actually_lands(self):
        key = bytes(range(0x40, 0x60))
        out, _ = P.patch(self.img, "ctrl_pk", key)
        magic = P.MAGICS["ctrl_pk"][0]
        h = bytes(out).find(magic)
        self.assertEqual(bytes(out)[h + len(magic):h + len(magic) + 32], key)

    def test_size_is_preserved(self):
        out, _ = P.patch(self.img, "ctrl_pk", bytes(32))
        self.assertEqual(len(out), len(self.img),
                         "flash offsets assume the image size never changes")

    def test_stale_checksum_is_detected(self):
        """Proves verify() would actually catch the failure mode, rather than always saying OK."""
        d = bytearray(self.img)
        h = bytes(d).find(P.MAGICS["ctrl_pk"][0])
        d[h + 16:h + 48] = bytes(range(32))      # patch WITHOUT repairing
        self.assertFalse(P.verify(bytes(d)))

    def test_every_copy_is_patched(self):
        """A header-defined block can land in several translation units. A stale copy left behind
        may be the one that actually gets used."""
        magic, size = P.MAGICS["ctrl_pk"]
        payload = (magic + bytes(size)) * 3
        img = make_image(payload)
        out, n = P.patch(img, "ctrl_pk", b"\x5a" * 32)
        self.assertEqual(n, 3)
        self.assertEqual(bytes(out).count(b"\x5a" * 32), 3)
        self.assertTrue(P.verify(out))

    def test_missing_magic_is_an_error(self):
        img = make_image(b"\x00" * 128)
        with self.assertRaises(ValueError):
            P.patch(img, "ctrl_pk", bytes(32))

    def test_wrong_key_size_is_an_error(self):
        with self.assertRaises(ValueError):
            P.patch(self.img, "ctrl_pk", bytes(16))

    def test_image_without_appended_hash(self):
        magic, size = P.MAGICS["ctrl_pk"]
        img = make_image(magic + bytes(size), hash_appended=False)
        out, _ = P.patch(img, "ctrl_pk", bytes(range(32)))
        self.assertTrue(P.verify(out))


class JsAndPythonStayInStep(unittest.TestCase):
    """web/keypatch.js is the code that actually runs for users. It must not drift from the
    reference, so pin the constants both sides depend on."""

    def test_magics_match_the_js(self):
        js = open(os.path.join(HERE, "keypatch.js"), encoding="utf-8").read()
        for name, (magic, size) in P.MAGICS.items():
            self.assertIn('magic: "%s"' % magic.decode(), js,
                          "%s magic differs between patch_keyblock.py and keypatch.js" % name)
            self.assertIn("size: %d" % size, js)

    def test_js_repairs_both_values(self):
        js = open(os.path.join(HERE, "keypatch.js"), encoding="utf-8").read()
        self.assertIn("0xef", js, "JS lost the segment-checksum seed")
        self.assertIn("SHA-256", js, "JS lost the appended-hash repair")


if __name__ == "__main__":
    unittest.main()
