import os, re, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(os.path.dirname(HERE), "gen_ctrl_key.py")
PY = sys.executable
SK = "cyd/main/sim_ctrl_sk.h"
PK = "components/simulacra_radar/sim_ctrl_key.h"


def run(out):
    r = subprocess.run([PY, SCRIPT, "--out-dir", out, "--no-skip"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r


def read(path):
    with open(path) as f:
        return f.read()


def parse_array(path):
    return bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", read(path)))


def ed25519_verify(sk64, pk32, msg=b"simulacra-ctrl-test"):
    """Sign msg with sk64, verify under pk32; return True iff valid."""
    try:
        from nacl.signing import SigningKey, VerifyKey
        sig = SigningKey(sk64[:32]).sign(msg).signature
        VerifyKey(pk32).verify(msg, sig)     # raises on failure
        return True
    except ImportError:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PrivateKey, Ed25519PublicKey)
        sig = Ed25519PrivateKey.from_private_bytes(sk64[:32]).sign(msg)
        Ed25519PublicKey.from_public_bytes(pk32).verify(sig, msg)   # raises on failure
        return True


class GenCtrlKey(unittest.TestCase):
    def test_generates_valid_matching_pair(self):
        with tempfile.TemporaryDirectory() as d:
            run(d)
            sk = parse_array(os.path.join(d, SK))
            pk = parse_array(os.path.join(d, PK))
            self.assertEqual(len(sk), 64, "SK must be 64 bytes")
            self.assertEqual(len(pk), 32, "PK must be 32 bytes")
            self.assertEqual(pk, sk[32:64], "PK must equal SK[32:64]")
            self.assertTrue(ed25519_verify(sk, pk), "sign/verify round-trip must pass")

    def test_headers_keep_expected_declarations(self):
        with tempfile.TemporaryDirectory() as d:
            run(d)
            self.assertIn("SIMULACRA_CTRL_SK[64]", read(os.path.join(d, SK)))
            self.assertIn("SIMULACRA_CTRL_PK[32]", read(os.path.join(d, PK)))

    def test_two_runs_differ(self):
        with tempfile.TemporaryDirectory() as d1, tempfile.TemporaryDirectory() as d2:
            run(d1); run(d2)
            self.assertNotEqual(parse_array(os.path.join(d1, SK)),
                                parse_array(os.path.join(d2, SK)), "keys must be fresh each run")


REPO = os.path.dirname(os.path.dirname(HERE))


def git(*args):
    return subprocess.run(["git", *args], cwd=REPO, capture_output=True, text=True)


@unittest.skipUnless(os.path.isdir(os.path.join(REPO, ".git")), "not a git checkout")
class SecretStaysLocal(unittest.TestCase):
    """The signing secret must never become committable again: whoever holds it can sign CONFIG
    commands (pause / clear-threats) for every node trusting the matching public key."""

    def test_secret_is_gitignored(self):
        self.assertEqual(git("check-ignore", "-q", SK).returncode, 0,
                         f"{SK} is not gitignored")

    def test_secret_is_not_tracked(self):
        self.assertNotEqual(git("ls-files", "--error-unmatch", SK).returncode, 0,
                            f"{SK} is TRACKED — `git rm --cached {SK}`")

    def test_example_template_is_tracked(self):
        self.assertEqual(git("ls-files", "--error-unmatch", SK + ".example").returncode, 0,
                         "the bring-up template must stay tracked or a fresh clone cannot build")

    def test_generator_refuses_to_write_into_the_repo_if_tracked(self):
        """Guard the guard: the refusal must key off git tracking, not a hardcoded path check."""
        with open(os.path.join(os.path.dirname(HERE), "gen_ctrl_key.py")) as f:
            src = f.read()
        self.assertIn("ls-files", src, "refuse_if_tracked must consult git")
        self.assertIn("refuse_if_tracked(a.out_dir)", src, "the refusal must run before writing")


if __name__ == "__main__":
    unittest.main()
