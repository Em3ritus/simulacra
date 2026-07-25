import os, subprocess, tempfile, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "sig_scan.exe" if os.name == "nt" else "sig_scan")


def advert(company, mfg):
    # sig_scan requires an "ad" field to count the line; the matcher itself reads company + mfg.
    return (f'{{"ts":1.0,"company":{company},"svc":0,"atype":"public",'
            f'"addr":"aabbccddeeff","mfg":"{mfg}","svcd":"","ad":"0201060aff{mfg}"}}')


def scan(lines):
    with tempfile.NamedTemporaryFile("w", suffix=".ndjson", delete=False, newline="\n") as f:
        f.write("\n".join(lines) + "\n"); path = f.name
    try:
        return subprocess.check_output([EXE, path], text=True)
    finally:
        os.unlink(path)


@unittest.skipUnless(os.path.exists(EXE), "sig_scan not built")
class Flock(unittest.TestCase):
    def test_flock_mfg_id_matches(self):
        # company 0x09C8 = 2504 (mfg little-endian starts c809) -> one Flock hit
        out = scan([advert(2504, "c809aabbccdd")])
        self.assertRegex(out, r"Flock\s*:\s*1")

    def test_selectivity_neighbor_id_no_match(self):
        # a neighboring company id must NOT match Flock
        out = scan([advert(2503, "c709aabbccdd")])
        self.assertRegex(out, r"Flock\s*:\s*0")


if __name__ == "__main__":
    unittest.main()
