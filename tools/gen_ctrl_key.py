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


def refuse_if_tracked(out_dir):
    """Never write a real secret into a path git is tracking — one `git commit -a` would publish
    fleet control authority. sim_ctrl_sk.h is gitignored; if it is tracked again, stop."""
    if os.path.abspath(out_dir) != REPO:
        return
    r = subprocess.run(["git", "ls-files", "--error-unmatch", SK_REL],
                       cwd=REPO, capture_output=True, text=True)
    if r.returncode == 0:
        sys.exit(f"error: {SK_REL} is TRACKED by git — writing a real secret there would publish it.\n"
                 f"  fix: git rm --cached {SK_REL}   (it is already listed in .gitignore)")


def main():
    ap = argparse.ArgumentParser(description="Generate the Ed25519 control signing keypair.")
    ap.add_argument("--out-dir", default=REPO, help="repo root to write into (default: this repo)")
    ap.add_argument("--no-skip", action="store_true",
                    help="do NOT git-skip-worktree the secret (default protects it from commit)")
    a = ap.parse_args()

    refuse_if_tracked(a.out_dir)
    seed, pub = gen_keypair()
    sk64 = seed + pub
    assert len(sk64) == 64 and len(pub) == 32 and pub == sk64[32:64], "malformed keypair"

    sk_path = os.path.join(a.out_dir, SK_REL)
    pk_path = os.path.join(a.out_dir, PK_REL)
    write(sk_path, SK_TMPL.format(body=c_array(sk64)))
    write(pk_path, PK_TMPL.format(body=c_array(pub)))
    print(f"wrote {sk_path}")
    print(f"wrote {pk_path}")

    # The secret is gitignored (and refuse_if_tracked() proved it is not tracked), so it cannot
    # be committed by accident. Confirm that out loud rather than relying on the operator's memory.
    if not a.no_skip and os.path.abspath(a.out_dir) == REPO:
        r = subprocess.run(["git", "check-ignore", "-q", SK_REL], cwd=REPO, capture_output=True)
        print(f"  git: {SK_REL} is gitignored -- your secret stays local" if r.returncode == 0
              else f"  WARNING: {SK_REL} is NOT gitignored -- add it to .gitignore before committing")

    print("\nNEXT: rebuild + reflash EVERY board (decoys bake the new public key, the CYD the new "
          "secret) and re-enroll the fleet, or the CYD's signatures won't verify.")


if __name__ == "__main__":
    main()
