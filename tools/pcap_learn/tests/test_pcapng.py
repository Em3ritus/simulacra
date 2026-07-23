import os, struct, subprocess, sys, tempfile, shutil, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
PARSE = os.path.join(TOOL, "parse_pcap.py")
PY = sys.executable


def _find_editcap():
    p = shutil.which("editcap")
    if p:
        return p
    for c in (r"C:\Program Files\Wireshark\editcap.exe",
              r"C:\Program Files (x86)\Wireshark\editcap.exe",
              "/usr/bin/editcap"):
        if os.path.exists(c):
            return c
    return None


def make_dlt256_pcap(path):
    """One synthetic DLT256 advert record: ADV_IND carrying an Apple (0x004C) mfg AD."""
    gh = struct.pack("<IHHIIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, 256)   # classic pcap hdr, DLT256
    aa = bytes.fromhex("d6be898e")                                     # advertising access address
    phdr = b"\x00\x00\x00\x00" + aa + b"\x00\x00"                      # 10-byte PHDR (ref AA at off 4)
    adva = bytes.fromhex("010203040506")
    ad = bytes.fromhex("020106" "03ff4c00")                            # flags + Apple mfg (company 0x004C)
    pdu = bytes([0x00, 6 + len(ad)]) + adva + ad                       # ADV_IND: h0=0, len, AdvA, AD
    data = phdr + aa + pdu
    rh = struct.pack("<IIII", 1000, 0, len(data), len(data))
    with open(path, "wb") as f:
        f.write(gh + rh + data)


def parse(path):
    out = subprocess.run([PY, PARSE, path], capture_output=True, text=True)
    return [l for l in out.stdout.splitlines() if l.strip()], out.stderr


class Pcapng(unittest.TestCase):
    def test_classic_pcap_parses_baseline(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "t.pcap"); make_dlt256_pcap(p)
            lines, err = parse(p)
            self.assertEqual(len(lines), 1, err)
            self.assertIn('"company":76', lines[0])      # 0x4C == 76

    @unittest.skipUnless(_find_editcap(), "editcap (Wireshark) not available")
    def test_pcapng_matches_classic(self):
        editcap = _find_editcap()
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "t.pcap"); make_dlt256_pcap(p)
            png = os.path.join(d, "t.pcapng")
            r = subprocess.run([editcap, "-F", "pcapng", p, png], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            with open(png, "rb") as fh:
                self.assertEqual(fh.read(4), b"\x0a\x0d\x0d\x0a", "not a pcapng")
            classic_lines, _ = parse(p)
            png_lines, err = parse(png)
            self.assertEqual(png_lines, classic_lines, err)   # transparent: same adverts either way
            self.assertEqual(len(png_lines), 1)


if __name__ == "__main__":
    unittest.main()
