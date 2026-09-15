#!/usr/bin/env python3
"""Replace a magic-tagged key block inside a built ESP-IDF image, then repair the image.

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
      2. A 32-byte SHA256 over the app image before it, appended at the end, present when byte 23
         of the header is 1.

    Repair both, in that order, or the board boots the bootloader and stops.

    THE THIRD ONE, found 2026-09-15: the images the web flasher actually downloads are MERGED
    flash images from "esptool merge_bin", not raw app images. A merged image is padding, then the
    bootloader, then the partition table, then the app, so byte 0 is not the app's header and on
    ESP32 it is not even 0xE9. Walking segments from byte 0 parses the BOOTLOADER, writes the
    checksum byte into the middle of the file, and hashes the wrong range. The result flashes
    perfectly and never boots. Everything below therefore operates on the app image located
    THROUGH THE PARTITION TABLE, not on the file as a whole.
"""
import argparse
import hashlib
import struct
import sys

MAGICS = {
    "ctrl_pk": (b"SIMULACRA:CTRLPK", 32),   # decoy: Ed25519 public key, verifies the Vigil
    "ctrl_sk": (b"SIMULACRA:CTRLSK", 64),   # Vigil: Ed25519 secret (seed||pub), TweetNaCl format
}

PART_TABLE_OFF = 0x8000      # fixed by ESP-IDF
PART_MAGIC = b"\xaa\x50"
PART_TYPE_APP = 0


def app_len(img, start):
    """Exact byte length of the app image beginning at start, from its own segment table.

    Needed because a merged image carries the app with other data after it, so the end of the file
    is NOT the end of the app and the appended SHA256 is not the file's last 32 bytes.
    """
    if start + 24 > len(img) or img[start] != 0xE9:
        raise ValueError("no ESP app image header at %#x" % start)
    nseg = img[start + 1]
    off = start + 24
    for _ in range(nseg):
        if off + 8 > len(img):
            raise ValueError("segment table runs past end of file")
        _addr, ln = struct.unpack("<II", img[off:off + 8])
        off += 8 + ln
    if off > len(img):
        raise ValueError("segment data runs past end of file")
    cks_off = off + (15 - ((off - start) % 16))
    n = cks_off + 1 - start
    if img[start + 23] == 1:
        n += 32
    if start + n > len(img):
        raise ValueError("app image runs past end of file")
    return n


def find_app_image(img):
    """(offset, length) of the app image inside img.

    Accepts a raw app image (0xE9 at byte 0) or a merged flash image, which is what CI publishes
    and what the browser downloads. For the merged form the app is found by reading the partition
    table rather than by assuming an offset, because the app partition's offset comes from the
    project's partition CSV and is not the same everywhere.

    THE PARTITION TABLE IS TRIED FIRST, and that order is load-bearing. 0xE9 at byte 0 does NOT
    mean "raw app image": the BOOTLOADER has the same magic, and its flash offset is per-chip --
    0x1000 on ESP32, 0x2000 on C5, but 0x0 on C6 and H2. So on a merged C6 image byte 0 is the
    bootloader's header, and trusting it would checksum and hash the bootloader while leaving the
    app untouched. A raw app image has no partition table to find, so it falls through correctly.
    """
    if len(img) >= PART_TABLE_OFF + 32 and img[PART_TABLE_OFF:PART_TABLE_OFF + 2] == PART_MAGIC:
        for i in range(0, 0x1000, 32):
            e = img[PART_TABLE_OFF + i:PART_TABLE_OFF + i + 32]
            if len(e) < 32 or e[:2] != PART_MAGIC:
                break                   # end of table, or the trailing MD5 marker
            if e[2] == PART_TYPE_APP:
                off = struct.unpack("<I", e[4:8])[0]
                if 0 < off < len(img) and img[off] == 0xE9:
                    try:
                        return off, app_len(img, off)
                    except ValueError:
                        pass            # malformed entry; keep looking

    if img[0] == 0xE9:
        return 0, app_len(img, 0)
    raise ValueError("no app image: byte 0 is not 0xE9 and no partition table at %#x lists an app "
                     "partition holding one" % PART_TABLE_OFF)


def find_block(img, magic):
    """Byte offsets of every occurrence of magic. A header-defined block can land in more than one
    translation unit, so every copy has to be rewritten or the stale one is the one that gets used."""
    out, i = [], img.find(magic)
    while i >= 0:
        out.append(i)
        i = img.find(magic, i + 1)
    return out


def repair(img):
    """Recompute the app image's segment checksum and appended SHA256. Returns a new bytearray of
    the same total length; bytes outside the app image are untouched."""
    d = bytearray(img)
    start, ln = find_app_image(d)
    nseg = d[start + 1]
    hash_appended = d[start + 23] == 1

    off = start + 24
    xor = 0xEF
    for _ in range(nseg):
        _addr, seglen = struct.unpack("<II", d[off:off + 8])
        off += 8
        for b in d[off:off + seglen]:
            xor ^= b
        off += seglen

    # The checksum is the last byte of the 16-byte block following the final segment, counted from
    # the START OF THE APP IMAGE rather than the start of the file.
    cks_off = off + (15 - ((off - start) % 16))
    if cks_off >= len(d):
        raise ValueError("checksum offset past end of image")
    d[cks_off] = xor

    if hash_appended:
        end = start + ln
        d[end - 32:end] = hashlib.sha256(bytes(d[start:end - 32])).digest()
    return d


def verify(img):
    """True if the embedded app image's own checksum and hash agree with its contents."""
    d = bytes(img)
    try:
        start, ln = find_app_image(d)
    except ValueError:
        return False
    nseg = d[start + 1]
    off, xor = start + 24, 0xEF
    for _ in range(nseg):
        _a, seglen = struct.unpack("<II", d[off:off + 8])
        off += 8
        for b in d[off:off + seglen]:
            xor ^= b
        off += seglen
    cks_off = off + (15 - ((off - start) % 16))
    if d[cks_off] != xor:
        return False
    if d[start + 23] == 1:
        end = start + ln
        if hashlib.sha256(d[start:end - 32]).digest() != d[end - 32:end]:
            return False
    return True


def patch(img, which, newkey):
    magic, size = MAGICS[which]
    if len(newkey) != size:
        raise ValueError("%s needs %d bytes, got %d" % (which, size, len(newkey)))
    start, ln = find_app_image(img)
    hits = [h for h in find_block(img, magic)
            if h >= start and h + len(magic) + size <= start + ln]
    if not hits:
        raise ValueError("magic %r not found inside the app image: was it built from a tree with "
                         "the key-block headers?" % magic.decode())
    d = bytearray(img)
    for h in hits:
        d[h + len(magic):h + len(magic) + size] = newkey
    return repair(d), len(hits)


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
        try:
            start, ln = find_app_image(img)
            print("app image: %#x .. %#x (%d bytes)" % (start, start + ln, ln))
        except ValueError as e:
            print("app image: NOT FOUND (%s)" % e)
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
