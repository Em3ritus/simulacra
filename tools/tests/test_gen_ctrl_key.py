import hashlib, json, os, re, subprocess, sys, tempfile, unittest

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
        """The names call sites use must still resolve, and the sizes must be right.

        The keys moved into magic-prefixed structs so the web flasher can find and replace them in a
        built image, with a macro preserving the original symbol name. So the plain
        `SIMULACRA_CTRL_SK[64]` declaration is gone, but what actually matters is unchanged: the
        symbol exists and the array is the right length.
        """
        with tempfile.TemporaryDirectory() as d:
            run(d)
            sk_h, pk_h = read(os.path.join(d, SK)), read(os.path.join(d, PK))
            self.assertIn("#define SIMULACRA_CTRL_SK", sk_h)
            self.assertIn("#define SIMULACRA_CTRL_PK", pk_h)
            self.assertIn("key[64]", sk_h)
            self.assertIn("key[32]", pk_h)

    def test_headers_carry_the_locator_magic(self):
        """Without the magic, the web flasher cannot find the key in a built image and every
        browser-flashed fleet falls back to a published key."""
        with tempfile.TemporaryDirectory() as d:
            run(d)
            self.assertIn("'S','I','M','U','L','A','C','R','A',':','C','T','R','L','S','K'",
                          read(os.path.join(d, SK)))
            self.assertIn("'S','I','M','U','L','A','C','R','A',':','C','T','R','L','P','K'",
                          read(os.path.join(d, PK)))

    def test_layout_stays_patchable(self):
        """packed + used are load-bearing: padding between magic and key would break the fixed
        offset the patcher relies on, and without `used` the linker can discard the block when only
        .key is referenced."""
        with tempfile.TemporaryDirectory() as d:
            run(d)
            for f in (SK, PK):
                h = read(os.path.join(d, f))
                self.assertIn("__attribute__((packed))", h)
                self.assertIn("__attribute__((used))", h)

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
                            f"{SK} is TRACKED - `git rm --cached {SK}`")

    def test_example_template_is_tracked(self):
        self.assertEqual(git("ls-files", "--error-unmatch", SK + ".example").returncode, 0,
                         "the bring-up template must stay tracked or a fresh clone cannot build")

    def test_generator_refuses_to_write_into_the_repo_if_tracked(self):
        """Guard the guard: the refusal must key off git tracking, not a hardcoded path check."""
        with open(os.path.join(os.path.dirname(HERE), "gen_ctrl_key.py")) as f:
            src = f.read()
        self.assertIn("ls-files", src, "refuse_if_tracked must consult git")
        self.assertIn("refuse_if_tracked(a.out_dir)", src, "the refusal must run before writing")


def tool(out, *extra, stdin=None):
    return subprocess.run([PY, SCRIPT, "--out-dir", out, *extra],
                          input=stdin, capture_output=True, text=True)


class AdoptAnExistingFleetKey(unittest.TestCase):
    """A fleet flashed from the web page is keyed by the BROWSER, so the source tree holds a
    different key. Building from source and flashing the Vigil then silently re-keys it: every
    enrolled decoy stops verifying its signatures, the roster still lists them, and nothing on
    screen says why. --from-backup is what prevents that, so these check it reproduces the SAME
    key rather than quietly minting a new one."""

    def test_import_reproduces_the_same_keypair(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            run(a)
            sk = parse_array(os.path.join(a, SK))[:64]
            # Feed the 64-byte seed||pub secret, the shape you get reading the block off a board.
            r = tool(b, "--no-skip", "--from-backup", "-", stdin=sk.hex())
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(parse_array(os.path.join(b, SK))[:64], sk,
                             "imported secret differs from the one supplied")
            self.assertEqual(parse_array(os.path.join(b, PK))[:32], sk[32:],
                             "imported public key is not that secret's own public half")

    def test_import_accepts_a_bare_32_byte_seed(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            run(a)
            sk = parse_array(os.path.join(a, SK))[:64]
            r = tool(b, "--no-skip", "--from-backup", "-", stdin=sk[:32].hex())
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(parse_array(os.path.join(b, SK))[:64], sk)

    def test_export_import_round_trips_byte_for_byte(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            run(a)
            bak = os.path.join(a, "backup.json")
            self.assertEqual(tool(a, "--export-backup", bak).returncode, 0)
            self.assertEqual(tool(b, "--no-skip", "--from-backup", bak).returncode, 0)
            for rel in (SK, PK):
                self.assertEqual(read(os.path.join(a, rel)), read(os.path.join(b, rel)),
                                 f"{rel} differs after an export/import round trip")

    def test_exported_backup_matches_the_browsers_format(self):
        """The browser has to read this file and vice versa, so its shape is a contract."""
        with tempfile.TemporaryDirectory() as a:
            run(a)
            bak = os.path.join(a, "backup.json")
            self.assertEqual(tool(a, "--export-backup", bak).returncode, 0)
            d = json.loads(read(bak))
            keygen = read(os.path.join(os.path.dirname(os.path.dirname(HERE)), "web", "keygen.js"))
            self.assertEqual(d["kind"], "simulacra-fleet-key")
            self.assertIn('BACKUP_KIND = "%s"' % d["kind"], keygen,
                          "backup kind drifted apart from web/keygen.js")
            for field in ("seed_hex", "public_key_hex", "fingerprint", "version"):
                self.assertIn(field, d, f"backup is missing {field}")
                self.assertIn(field, keygen, f"web/keygen.js no longer writes {field}")

    def test_fingerprint_matches_the_browsers_algorithm(self):
        """One fleet must read the same in the browser, in this tool and in the backup file, or the
        comparison the operator is told to make is meaningless."""
        with tempfile.TemporaryDirectory() as a:
            run(a)
            pk = parse_array(os.path.join(a, PK))[:32]
            h = hashlib.sha256(pk).hexdigest()[:16]
            want = "-".join(h[i:i + 4] for i in range(0, 16, 4))
            bak = os.path.join(a, "backup.json")
            self.assertEqual(tool(a, "--export-backup", bak).returncode, 0)
            self.assertEqual(json.loads(read(bak))["fingerprint"], want)

    def test_inconsistent_backup_is_refused(self):
        """An edited or truncated backup would key a fleet nobody can control. Fail, do not guess."""
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            run(a)
            bak = os.path.join(a, "backup.json")
            self.assertEqual(tool(a, "--export-backup", bak).returncode, 0)
            d = json.loads(read(bak))
            d["public_key_hex"] = "00" * 32                  # halves now disagree
            with open(bak, "w") as f:
                json.dump(d, f)
            r = tool(b, "--no-skip", "--from-backup", bak)
            self.assertNotEqual(r.returncode, 0, "an inconsistent backup must be refused")
            self.assertIn("inconsistent", (r.stderr + r.stdout).lower())

    def test_mismatched_64_byte_secret_is_refused(self):
        with tempfile.TemporaryDirectory() as b:
            r = tool(b, "--no-skip", "--from-backup", "-", stdin="ab" * 64)
            self.assertNotEqual(r.returncode, 0, "seed||pub whose halves disagree must be refused")

    def test_export_never_creates_a_key(self):
        """Export is read-only. If it generated one it would re-key the fleet it was asked about."""
        with tempfile.TemporaryDirectory() as b:
            r = tool(b, "--export-backup", "-")
            self.assertNotEqual(r.returncode, 0, "export with no key present must fail")
            self.assertFalse(os.path.exists(os.path.join(b, SK)),
                             "export must never create a secret")


if __name__ == "__main__":
    unittest.main()
