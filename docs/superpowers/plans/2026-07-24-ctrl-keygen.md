# Control-Key Generator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (or subagent-driven-development) to implement this plan. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `tools/gen_ctrl_key.py` — one command that generates a fresh Ed25519 control keypair into the two committed header formats and protects the secret from accidental commit.

**Architecture:** A single self-contained Python script + a host unittest. Crypto via PyNaCl (primary) or `cryptography` (fallback). Writes `SIMULACRA_CTRL_SK[64]` (seed‖pub) and `SIMULACRA_CTRL_PK[32]` in the exact committed C-array layout.

**Tech Stack:** Python 3.12 stdlib + PyNaCl/cryptography; Ed25519 (NaCl `crypto_sign` compatible).

## Global Constraints

- **Exact header formats:** `cyd/main/sim_ctrl_sk.h` → `static const uint8_t SIMULACRA_CTRL_SK[64]`; `components/simulacra_radar/sim_ctrl_key.h` → `static const uint8_t SIMULACRA_CTRL_PK[32]`. `#pragma once` + `#include <stdint.h>`, 16 bytes/line, lowercase `0x%02x`, comma-separated, no trailing comma after the final byte.
- **`sk = seed(32) ‖ pub(32)`, `pk = pub(32)`, and `pk == sk[32:64]`** (asserted).
- **PyNaCl primary, `cryptography` fallback**, clear `pip install pynacl` error if neither.
- **Default `--skip-worktree`** on `cyd/main/sim_ctrl_sk.h` when writing to the real repo root (opt out with `--no-skip`); never fail if git is absent/file untracked — print a note.
- **Commit trailers** on every commit:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
  ```
- **No PII in tracked files.** Never use PowerShell `>` for file writes (UTF-16/BOM).

---

### Task 1: The generator + test

**Files:**
- Create: `tools/gen_ctrl_key.py`
- Create: `tools/tests/test_gen_ctrl_key.py`

**Interfaces:**
- Produces: CLI `python tools/gen_ctrl_key.py [--out-dir DIR] [--no-skip]`. Writes
  `<out-dir>/cyd/main/sim_ctrl_sk.h` and `<out-dir>/components/simulacra_radar/sim_ctrl_key.h`
  (creating parent dirs). Default out-dir = repo root.

- [ ] **Step 1: Write the failing test `tools/tests/test_gen_ctrl_key.py`**

```python
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


def parse_array(path):
    text = open(path).read()
    return bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", text))


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
            self.assertIn("SIMULACRA_CTRL_SK[64]", open(os.path.join(d, SK)).read())
            self.assertIn("SIMULACRA_CTRL_PK[32]", open(os.path.join(d, PK)).read())

    def test_two_runs_differ(self):
        with tempfile.TemporaryDirectory() as d1, tempfile.TemporaryDirectory() as d2:
            run(d1); run(d2)
            self.assertNotEqual(parse_array(os.path.join(d1, SK)),
                                parse_array(os.path.join(d2, SK)), "keys must be fresh each run")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it, expect failure**

Run: `"C:/Program Files/Python312/python.exe" -m unittest tools.tests.test_gen_ctrl_key -v`
Expected: FAIL/ERROR — `gen_ctrl_key.py` doesn't exist (subprocess returns non-zero / file missing).

- [ ] **Step 3: Write `tools/gen_ctrl_key.py`**

```python
#!/usr/bin/env python3
"""Generate a fresh Ed25519 control signing keypair for the provisioned fleet regime and write it
into the two firmware headers (cyd/main/sim_ctrl_sk.h secret, components/simulacra_radar/sim_ctrl_key.h
public). The Vigil signs enrollment OFFERs + CONFIG commands with the secret; decoys verify with the
public key. Regenerate before any real deployment -- the committed keys are placeholders."""
import argparse, os, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # tools/.. == repo root
SK_REL = os.path.join("cyd", "main", "sim_ctrl_sk.h")
PK_REL = os.path.join("components", "simulacra_radar", "sim_ctrl_key.h")


def gen_keypair():
    """Return (seed32, pub32). PyNaCl first (same NaCl family as firmware TweetNaCl), then cryptography."""
    try:
        from nacl.signing import SigningKey
        sk = SigningKey.generate()
        return bytes(sk), bytes(sk.verify_key)
    except ImportError:
        pass
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        from cryptography.hazmat.primitives import serialization as s
        k = Ed25519PrivateKey.generate()
        seed = k.private_bytes(s.Encoding.Raw, s.PrivateFormat.Raw, s.NoEncryption())
        pub = k.public_key().public_bytes(s.Encoding.Raw, s.PublicFormat.Raw)
        return seed, pub
    except ImportError:
        sys.exit("error: need PyNaCl or the cryptography package -> pip install pynacl")


def c_array(b):
    """Format bytes as the project's C-array body: 16 per line, '0x%02x', no trailing comma."""
    vals = [f"0x{x:02x}" for x in b]
    rows = [vals[i:i + 16] for i in range(0, len(vals), 16)]
    return ",\n".join("    " + ", ".join(row) for row in rows)


SK_TMPL = """#pragma once
#include <stdint.h>
// Ed25519 SECRET key (seed||pub) for Vigil -- GENERATED. Keep local; NEVER commit to a public repo.
// TweetNaCl 64-byte secret-key format. (Re)generate with tools/gen_ctrl_key.py.
static const uint8_t SIMULACRA_CTRL_SK[64] = {{
{body}
}};
"""

PK_TMPL = """#pragma once
#include <stdint.h>
// Ed25519 PUBLIC key for the Vigil->decoy CONFIG link -- GENERATED (safe to share). Must match
// cyd/main/sim_ctrl_sk.h. Decoys verify with this. (Re)generate with tools/gen_ctrl_key.py.
static const uint8_t SIMULACRA_CTRL_PK[32] = {{
{body}
}};
"""


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser(description="Generate the Ed25519 control signing keypair.")
    ap.add_argument("--out-dir", default=REPO, help="repo root to write into (default: this repo)")
    ap.add_argument("--no-skip", action="store_true",
                    help="do NOT git-skip-worktree the secret (default protects it from commit)")
    a = ap.parse_args()

    seed, pub = gen_keypair()
    sk64 = seed + pub
    assert len(sk64) == 64 and len(pub) == 32 and pub == sk64[32:64], "malformed keypair"

    sk_path = os.path.join(a.out_dir, SK_REL)
    pk_path = os.path.join(a.out_dir, PK_REL)
    write(sk_path, SK_TMPL.format(body=c_array(sk64)))
    write(pk_path, PK_TMPL.format(body=c_array(pub)))
    print(f"wrote {sk_path}")
    print(f"wrote {pk_path}")

    # Protect the secret from accidental commit when writing to the real repo.
    if not a.no_skip and os.path.abspath(a.out_dir) == REPO:
        r = subprocess.run(["git", "update-index", "--skip-worktree", SK_REL],
                           cwd=REPO, capture_output=True, text=True)
        if r.returncode == 0:
            print(f"  git: skip-worktree set on {SK_REL} -- your secret stays local")
        else:
            print(f"  note: could not skip-worktree {SK_REL} (git absent or file untracked); "
                  f"do NOT commit it")

    print("\nNEXT: rebuild + reflash EVERY board (decoys bake the new public key, the CYD the new "
          "secret) and re-enroll the fleet, or the CYD's signatures won't verify.")


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run the test, expect pass**

Run: `"C:/Program Files/Python312/python.exe" -m unittest tools.tests.test_gen_ctrl_key -v`
Expected: PASS — all three tests (valid matching pair + round-trip, declarations present, two runs differ).

- [ ] **Step 5: Smoke-test the real invocation (dry, to a temp dir — do NOT overwrite the real placeholders)**

Run: `"C:/Program Files/Python312/python.exe" tools/gen_ctrl_key.py --out-dir "$env:TEMP/keygen_smoke" --no-skip`
Expected: prints `wrote .../sim_ctrl_sk.h` and `wrote .../sim_ctrl_key.h`, then the NEXT reminder. (Confirms the CLI + reminder text; the real headers are untouched.)

- [ ] **Step 6: Commit**

```bash
git add tools/gen_ctrl_key.py tools/tests/test_gen_ctrl_key.py
git commit -m "feat(keygen): tools/gen_ctrl_key.py — generate the Ed25519 control keypair

Writes a fresh keypair into the two committed header formats (SK 64B seed||pub,
PK 32B), asserts PK==SK[32:], and git-skip-worktrees the secret by default so a
real key can't be accidentally committed. Sign/verify round-trip test."
```

---

## Self-review notes

- Spec coverage: generator (Step 3) covers backend fallback, format, assertion, skip-worktree default, reminder; test (Step 1) covers the round-trip, declarations, freshness, and `--out-dir` isolation. All spec sections mapped.
- Placeholder scan: full file contents given; no TODOs.
- Consistency: `SIMULACRA_CTRL_SK[64]` / `SIMULACRA_CTRL_PK[32]`, `SK_REL`/`PK_REL`, and the test's `SK`/`PK` paths all agree; `pub == sk64[32:64]` asserted in both script and test.
