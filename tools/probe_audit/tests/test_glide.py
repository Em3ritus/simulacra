import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def glidenext(current, target, step):
    out = subprocess.check_output([EXE, "--glidenext", str(current), str(target), str(step)], text=True)
    return int(out.strip())


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class GlideNext(unittest.TestCase):
    def test_steps_up_by_step(self):
        self.assertEqual(glidenext(4, 8, 1), 5)

    def test_steps_down_by_step(self):
        self.assertEqual(glidenext(8, 4, 1), 7)

    def test_never_overshoots_up(self):
        self.assertEqual(glidenext(7, 8, 5), 8)

    def test_never_overshoots_down(self):
        self.assertEqual(glidenext(8, 7, 5), 7)

    def test_noop_when_at_target(self):
        self.assertEqual(glidenext(6, 6, 1), 6)

    def test_negative_step_is_treated_as_magnitude(self):
        self.assertEqual(glidenext(4, 8, -1), 5)


if __name__ == "__main__":
    unittest.main()
