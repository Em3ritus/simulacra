"""The image patcher must repair BOTH integrity values, or the board silently will not boot.

These run against a synthetic ESP app image rather than a real firmware build, so they work in CI
where no build artifacts exist. The format details they encode were all learned the hard way on
hardware: a patched image that leaves either value stale boots the bootloader, prints the partition
table, and then stops with nothing on the console.
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
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


def make_merged(app, app_off=0x10000, boot_off=0x1000):
    """A merged flash image, the form CI publishes and the browser actually downloads.

    Layout: padding, bootloader, partition table at 0x8000, app at app_off. `boot_off=0` models
    C6/H2, where the bootloader sits at offset 0 and the file therefore OPENS with 0xE9 -- the
    bootloader's magic, not the app's.
    """
    boot = make_image(b"\x99" * 256)          # stand-in bootloader: same 0xE9 image format
    out = bytearray(b"\xff" * app_off)
    out[boot_off:boot_off + len(boot)] = boot
    table = bytearray()
    for typ, sub, off, size, label in (
        (1, 0x02, 0x9000, 0x6000, b"nvs"),
        (1, 0x01, 0xF000, 0x1000, b"phy_init"),
        (0, 0x00, app_off, 0x177000, b"factory"),
    ):
        table += (b"\xaa\x50" + bytes([typ, sub]) + struct.pack("<II", off, size)
                  + label.ljust(16, b"\0") + b"\0" * 4)
    out[0x8000:0x8000 + len(table)] = table
    out += app
    return bytes(out)


class MergedFlashImages(unittest.TestCase):
    """The published images are merged, not raw app images. Parsing one from byte 0 walks the
    BOOTLOADER: it writes the checksum byte into the middle of the file and hashes the wrong range,
    producing an image that flashes perfectly and never boots."""

    def setUp(self):
        magic, size = P.MAGICS["ctrl_pk"]
        self.app = make_image(b"\xaa" * 64 + magic + bytes(size) + b"\xbb" * 64)
        self.merged = make_merged(self.app)

    def test_app_is_located_via_the_partition_table(self):
        start, length = P.find_app_image(self.merged)
        self.assertEqual(start, 0x10000)
        self.assertEqual(length, len(self.app))
        self.assertEqual(self.merged[start:start + length], self.app)

    def test_merged_image_verifies_as_built(self):
        self.assertTrue(P.verify(self.merged), "fixture is not a valid merged image")

    def test_patch_repairs_the_app_not_the_file(self):
        out, n = P.patch(self.merged, "ctrl_pk", bytes(range(32)))
        self.assertEqual(n, 1)
        self.assertTrue(P.verify(out))
        self.assertEqual(len(out), len(self.merged), "flash offsets assume a fixed image size")

    def test_bytes_outside_the_app_are_untouched(self):
        """The bootloader and partition table must survive verbatim; rewriting either is how you
        get a board that enumerates and then does nothing."""
        out, _ = P.patch(self.merged, "ctrl_pk", bytes(range(32)))
        start, length = P.find_app_image(self.merged)
        self.assertEqual(bytes(out)[:start], self.merged[:start])
        self.assertEqual(bytes(out)[start + length:], self.merged[start + length:])

    def test_patching_merged_equals_patching_the_app_alone(self):
        merged_out, _ = P.patch(self.merged, "ctrl_pk", bytes(range(32)))
        app_out, _ = P.patch(self.app, "ctrl_pk", bytes(range(32)))
        start, length = P.find_app_image(self.merged)
        self.assertEqual(bytes(merged_out)[start:start + length], bytes(app_out))

    def test_bootloader_at_offset_zero_is_not_mistaken_for_the_app(self):
        """C6 and H2 put the bootloader at 0x0, so the file opens with 0xE9. Trusting byte 0 there
        checksums the bootloader and leaves the app unpatched -- the most dangerous version of this
        bug, because the naive path looks like it worked."""
        merged = make_merged(self.app, boot_off=0)
        self.assertEqual(merged[0], 0xE9, "fixture should open with the bootloader magic")
        start, _ = P.find_app_image(merged)
        self.assertEqual(start, 0x10000, "must resolve through the partition table, not byte 0")
        out, n = P.patch(merged, "ctrl_pk", bytes(range(32)))
        self.assertEqual(n, 1)
        self.assertTrue(P.verify(out))
        self.assertEqual(bytes(out)[:0x10000], merged[:0x10000], "bootloader was modified")

    def test_naive_byte_zero_patch_is_detected_as_broken(self):
        """Guards the guard: if verify() stopped locating the app properly it would rubber-stamp
        exactly the image that bricks boards."""
        d = bytearray(self.merged)
        h = bytes(d).find(P.MAGICS["ctrl_pk"][0])
        d[h + 16:h + 48] = bytes(range(32))       # patch WITHOUT repairing
        self.assertFalse(P.verify(bytes(d)))

    def test_raw_app_image_still_works(self):
        """A raw app image has no partition table, so it must fall through to byte 0."""
        self.assertEqual(P.find_app_image(self.app), (0, len(self.app)))


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

    def test_js_locates_the_app_through_the_partition_table(self):
        js = open(os.path.join(HERE, "keypatch.js"), encoding="utf-8").read()
        self.assertIn("PART_TABLE_OFF = 0x%x" % P.PART_TABLE_OFF, js,
                      "JS lost the partition-table offset, so it would patch merged images at "
                      "byte 0 and brick them")
        self.assertIn("findAppImage", js)

    def test_js_tries_the_partition_table_before_byte_zero(self):
        """Order matters: on C6/H2 the bootloader sits at offset 0 and shares the app's 0xE9 magic,
        so a byte-0 shortcut placed first silently patches the bootloader."""
        js = open(os.path.join(HERE, "keypatch.js"), encoding="utf-8").read()
        body = js[js.index("export function findAppImage"):]
        body = body[:body.index("\n}")]
        self.assertLess(body.index("PART_TABLE_OFF"), body.index("d[0] === 0xe9"),
                        "byte-0 fallback must come AFTER the partition-table lookup")


def _node():
    """Path to node, or None. The JS is what users actually run, so when node is available the
    cross-check below is the test that matters most; when it is not, the string-matching tests
    above are all that remain and they are much weaker."""
    return shutil.which("node")


@unittest.skipUnless(_node(), "node not available")
class JsMatchesPythonOnRealBytes(unittest.TestCase):
    """Run keypatch.js through node and diff its output against the reference, byte for byte.

    The other JS tests only check that both files mention the same constants. That catches a
    renamed magic and nothing else: the arithmetic can drift while every constant still matches,
    and the result is an image that flashes cleanly and never boots. Only comparing output catches
    that.
    """

    def _xcheck(self, img, which, key):
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "in.bin")
            dst = os.path.join(td, "out.bin")
            with open(src, "wb") as f:
                f.write(img)
            r = subprocess.run(
                [_node(), os.path.join(HERE, "xcheck_keypatch.mjs"), src, which, key.hex(), dst],
                capture_output=True, text=True, cwd=HERE)
            self.assertEqual(r.returncode, 0, f"node failed: {r.stderr}")
            with open(dst, "rb") as f:
                js_out = f.read()
        py_out, py_n = P.patch(img, which, key)
        self.assertEqual(int(r.stdout.strip()), py_n, "block count differs between JS and Python")
        self.assertEqual(js_out, bytes(py_out),
                         "keypatch.js and patch_keyblock.py produced DIFFERENT bytes for the same "
                         "input; one of them is writing an image that will not boot")
        return js_out

    def test_agree_on_a_raw_app_image(self):
        magic, size = P.MAGICS["ctrl_pk"]
        img = make_image(b"\xaa" * 64 + magic + bytes(size) + b"\xbb" * 64)
        self._xcheck(img, "ctrl_pk", bytes(range(32)))

    def test_agree_on_a_merged_image(self):
        """The form the flasher actually downloads."""
        magic, size = P.MAGICS["ctrl_pk"]
        app = make_image(b"\xaa" * 64 + magic + bytes(size) + b"\xbb" * 64)
        self._xcheck(make_merged(app), "ctrl_pk", bytes(range(32)))

    def test_agree_when_the_bootloader_sits_at_offset_zero(self):
        """C6/H2 layout, where byte 0 is the bootloader's 0xE9 and the naive reading is wrong."""
        magic, size = P.MAGICS["ctrl_pk"]
        app = make_image(b"\xaa" * 64 + magic + bytes(size) + b"\xbb" * 64)
        self._xcheck(make_merged(app, boot_off=0), "ctrl_pk", bytes(range(32)))

    def test_agree_on_the_64_byte_secret_and_multiple_copies(self):
        """The CYD image carries two copies of the secret block; a stale one may be the one used."""
        magic, size = P.MAGICS["ctrl_sk"]
        app = make_image((magic + bytes(size)) * 2 + b"\xcc" * 32)
        out = self._xcheck(make_merged(app), "ctrl_sk", bytes(range(64)))
        self.assertEqual(out.count(bytes(range(64))), 2, "both copies should carry the new key")


if __name__ == "__main__":
    unittest.main()
