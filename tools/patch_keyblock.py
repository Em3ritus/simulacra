#!/usr/bin/env python3
"""Replace a magic-tagged key block inside a built ESP-IDF app image, then repair the image.

This is the reference implementation of what the web flasher does in JavaScript. Keeping a Python
copy means the behaviour can be tested on the bench without a browser, and the two can be diffed
when one of them misbehaves.

WHY THIS EXISTS
    Anything compiled into a published binary is public. The web-flasher images are downloadable, so
    a key baked into one is a key everybody has. Generating the keys in the browser and rewriting
    them into the image at flash time is the only way a browser-flashed fleet ends up holding
    something that was never published.

WHAT BIT US, AND WHY THIS FILE IS LONGER THAN YOU EXPECT
    An ESP-IDF app image is not a flat blob you can edit in place. It carries TWO integrity values
    over its own contents, and a patched image that ignores either one is rejected by the bootloader
    with no error on the console: the boot log simply stops after the partition table and the app
    never starts. Both were found that way on hardware, in that order.

      1. A 1-byte XOR checksum over every segment's DATA bytes, seeded 0xEF, written as the last
         byte of the 16-byte block after the final segment.
      2. A 32-byte SHA256 over the whole image before it, appended at the end, present when byte 23
         of the header is 1.

    Repair both, in that order, or the board boots the bootloader and stops.
"""
import argparse
import hashlib
import struct
import sys

MAGICS = {
    "ctrl_pk": (b"SIMULACRA:CTRLPK", 32),   # decoy: Ed25519 public key, verifies the Vigil
    "ctrl_sk": (b"SIMULACRA:CTRLSK", 64),   # Vigil: Ed25519 secret (seed||pub), TweetNaCl format
}


def find_block(img, magic):
    """Byte offsets of every occurrence of magic. A header-defined block can land in more than one
    translation unit, so every copy has to be rewritten or the stale one is the one that gets used."""
    out, i = [], img.find(magic)
    while i >= 0:
        out.append(i)
        i = img.find(magic, i + 1)
    return out


def repair(img):
    """Recompute the segment checksum and the appended SHA256. Returns a new bytearray."""
    d = bytearray(img)
    if d[0] != 0xE9:
        raise ValueError("not an ESP app image (magic 0x%02X, expected 0xE9)" % d[0])
    nseg = d[1]
    hash_appended = d[23] == 1

    # Walk the segment table to find where the data ends.
    off = 24
    xor = 0xEF
    for _ in range(nseg):
        _addr, ln = struct.unpack("<II", d[off:off + 8])
        off += 8
        for b in d[off:off + ln]:
            xor ^= b
        off += ln

    # The checksum is the last byte of the 16-byte block that follows the final segment.
    cks_off = off + (15 - (off % 16))
    if cks_off >= len(d):
        raise ValueError("checksum offset past end of image")
    d[cks_off] = xor

    if hash_appended:
        d[-32:] = hashlib.sha256(bytes(d[:-32])).digest()
    return d


def patch(img, which, newkey):
    magic, size = MAGICS[which]
    if len(newkey) != size:
        raise ValueError("%s needs %d bytes, got %d" % (which, size, len(newkey)))
    hits = find_block(img, magic)
    if not hits:
        raise ValueError("magic %r not found: was the image built from a tree with the key-block "
                         "headers?" % magic.decode())
    d = bytearray(img)
    for h in hits:
        d[h + len(magic):h + len(magic) + size] = newkey
    return repair(d), len(hits)


def verify(img):
    """True if the image's own checksum and hash agree with its contents."""
    d = bytes(img)
    if d[0] != 0xE9:
        return False
    nseg = d[1]
    off, xor = 24, 0xEF
    for _ in range(nseg):
        _a, ln = struct.unpack("<II", d[off:off + 8])
        off += 8
        for b in d[off:off + ln]:
            xor ^= b
        off += ln
    cks_off = off + (15 - (off % 16))
    if d[cks_off] != xor:
        return False
    if d[23] == 1 and hashlib.sha256(d[:-32]).digest() != d[-32:]:
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--which", choices=sorted(MAGICS), required=True)
    ap.add_argument("--key-hex", help="replacement key as hex; omit to only verify the image")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()

    img = open(a.image, "rb").read()
    if not a.key_hex:
        print("integrity: %s" % ("OK" if verify(img) else "BROKEN"))
        for name, (magic, size) in MAGICS.items():
            hits = find_block(img, magic)
            if hits:
                print("  %-8s %d block(s) at %s" % (name, len(hits), ", ".join(hex(h) for h in hits)))
        return 0

    out, n = patch(img, a.which, bytes.fromhex(a.key_hex))
    if not verify(out):
        sys.exit("patched image failed its own integrity check; refusing to write")
    if len(out) != len(img):
        sys.exit("patched image changed size; refusing to write")
    open(a.out or a.image, "wb").write(bytes(out))
    print("patched %d block(s), integrity repaired, %d bytes" % (n, len(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
