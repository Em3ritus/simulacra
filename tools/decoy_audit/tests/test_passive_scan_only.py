import os, re, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
MAIN = os.path.join(ROOT, "main")


def src(name):
    with open(os.path.join(MAIN, name), encoding="utf-8", errors="replace") as f:
        return f.read()


class ScanningMustStayPassive(unittest.TestCase):
    """The observer must never active-scan. This is the difference between silent and detectable.

    A passive scan transmits nothing, so no honeypot, sniffer or counter-surveillance rig can see it
    -- not by any protocol trick, it is physically impossible. An ACTIVE scan sends a SCAN_REQ to
    every scannable advertiser it hears, addressed from our own address, which turns the observer
    from a silent listener into a beacon announcing "something here is enumerating devices".

    That is not theoretical. Passive BLE detectors exist precisely to catalogue whatever
    active-scans them, and a device that active-scans continuously announces itself to every one of
    them in range. A Simulacra board doing that would be trivially collected by anyone running the
    trick, and the whole point of this project is not to hand an observer a durable handle.

    `observe.c` gets this right. There is no test pinning it, and the setting is one character from
    catastrophe, so this file is that test.
    """

    def test_scan_params_set_passive(self):
        m = re.search(r"\.passive\s*=\s*([01])", src("observe.c"))
        self.assertIsNotNone(m, "observe.c no longer sets .passive at all -- NimBLE defaults it to "
                                "0, which is ACTIVE scanning. Set it explicitly to 1.")
        self.assertEqual(m.group(1), "1",
                         "observe.c sets .passive = 0, i.e. ACTIVE scanning. Every scannable "
                         "advertiser in range now receives a SCAN_REQ from this board, which is a "
                         "transmission that identifies it. Passive scanning is undetectable; active "
                         "scanning is not.")

    def test_no_other_scan_site_appears(self):
        """A second scan site could start an active scan without tripping the check above."""
        starts = []
        for name in ("observe.c", "churn.c", "simulacra_main.c", "esp_now_link.c"):
            path = os.path.join(MAIN, name)
            if not os.path.exists(path):
                continue
            for line in src(name).splitlines():
                code = line.split("//", 1)[0]                      # comments mention it too
                if re.search(r"ble_gap_(?:ext_)?disc\s*\(", code):
                    starts.append((name, code.strip()))
        self.assertEqual(len(starts), 1,
                         "expected exactly one scan-start site (observe.c). Found %d: %s. Every "
                         "additional one needs its own passive=1, and this test only reads the "
                         "first." % (len(starts), [n for n, _ in starts]))

    def test_own_addr_type_is_not_load_bearing(self):
        """observe.c scans with BLE_OWN_ADDR_PUBLIC -- the chip's permanent factory MAC.

        While the scan is passive this never reaches the air, because a passive scanner transmits
        nothing at all. It is recorded here so nobody reads that constant as harmless: the moment
        `.passive` becomes 0, this board starts broadcasting an identifier it can never rotate, in
        a project whose entire premise is that no identifier survives. The two settings are only
        safe together.
        """
        s = src("observe.c")
        if "BLE_OWN_ADDR_PUBLIC" not in s:
            self.skipTest("observe.c no longer scans with a public own-address type")
        m = re.search(r"\.passive\s*=\s*([01])", s)
        self.assertEqual(m.group(1) if m else None, "1",
                         "observe.c pairs BLE_OWN_ADDR_PUBLIC with an active scan. That emits the "
                         "chip's permanent factory MAC in every SCAN_REQ -- an identifier that "
                         "cannot rotate and outlives every other identifier the project bounds.")


if __name__ == "__main__":
    unittest.main()
