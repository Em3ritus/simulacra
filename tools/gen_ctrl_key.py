#!/usr/bin/env python3
"""Manage the Ed25519 control signing keypair for the provisioned fleet regime.

It lives in two firmware headers: cyd/main/sim_ctrl_sk.h (secret, gitignored) and
components/simulacra_radar/sim_ctrl_key.h (public). The Vigil signs enrollment OFFERs and CONFIG
commands with the secret; decoys verify with the public key. Regenerate before any real deployment,
because the committed keys are placeholders.

  (default)          generate a fresh keypair and write both headers
  --from-backup F    adopt an existing fleet's key instead (browser backup, or a bare hex seed)
  --export-backup F  write the current key out in the browser's backup format

WHY IMPORT AND EXPORT EXIST
    A fleet flashed from the web page is keyed by the BROWSER, so its key exists only in that
    browser profile, its backup file, and the boards. The source tree still holds a different key.
    Building from source and flashing the Vigil then silently re-keys it: every enrolled decoy stops
    verifying its signatures, the roster still lists them, and the console goes quiet with nothing on
    screen to say why. --from-backup is what stops that, by making the source tree agree with the
    fleet you already have. --export-backup goes the other way, so a source-keyed fleet can be
    extended with a board flashed from the browser."""
import argparse, hashlib, json, os, re, subprocess, sys

BACKUP_KIND = "simulacra-fleet-key"     # must match web/keygen.js

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


def pub_from_seed(seed):
    """Derive the 32-byte public key from a 32-byte seed, same two backends as gen_keypair()."""
    if len(seed) != 32:
        sys.exit("error: seed must be 32 bytes (64 hex chars), got %d" % len(seed))
    try:
        from nacl.signing import SigningKey
        return bytes(SigningKey(seed).verify_key)
    except ImportError:
        pass
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        from cryptography.hazmat.primitives import serialization as s
        k = Ed25519PrivateKey.from_private_bytes(seed)
        return k.public_key().public_bytes(s.Encoding.Raw, s.PublicFormat.Raw)
    except ImportError:
        sys.exit("error: need PyNaCl or the cryptography package -> pip install pynacl")


def fingerprint(pub):
    """The short id web/keygen.js shows, so one fleet reads the same in the browser, in this tool,
    and in a backup file: first 8 bytes of SHA-256 over the public key, in 4-character groups."""
    d = hashlib.sha256(pub).hexdigest()[:16]
    return "-".join(d[i:i + 4] for i in range(0, 16, 4))


def seed_from_backup(text):
    """Accept a web/keygen.js backup file, or a bare 32-byte hex seed pasted in."""
    t = text.strip()
    if t.startswith("{"):
        o = json.loads(t)
        if o.get("kind") != BACKUP_KIND:
            sys.exit("error: that JSON is not a Simulacra fleet key backup (kind=%r)" % o.get("kind"))
        if not o.get("seed_hex"):
            sys.exit("error: backup has no seed_hex")
        seed = bytes.fromhex(o["seed_hex"])
        # If the file carries the public half too, it has to agree. A mismatch means the file was
        # edited or truncated, and importing it would key a fleet nobody can control.
        if o.get("public_key_hex") and bytes.fromhex(o["public_key_hex"]) != pub_from_seed(seed):
            sys.exit("error: backup is inconsistent -- its public key does not match its seed")
        return seed
    try:
        raw = bytes.fromhex(t.replace(" ", "").replace("\n", "").replace("\r", ""))
    except ValueError:
        sys.exit("error: not a backup file and not a hex seed")
    if len(raw) == 32:
        return raw
    if len(raw) == 64:
        # The firmware's 64-byte TweetNaCl secret, seed||pub. This is the shape you get from
        # `patch_keyblock.py --which ctrl_sk` or by reading the block straight off a board, so
        # accepting it saves an error at exactly the moment someone is recovering a live fleet.
        seed, pub = raw[:32], raw[32:]
        if pub_from_seed(seed) != pub:
            sys.exit("error: 64-byte key is not a valid seed||pub pair -- its halves disagree")
        return seed
    sys.exit("error: expected a 32-byte seed or a 64-byte seed||pub secret, got %d bytes" % len(raw))


def seed_from_header(path):
    """Pull the 32-byte seed back out of a generated sim_ctrl_sk.h (the key is stored as seed||pub)."""
    try:
        s = open(path, encoding="utf-8").read()
    except FileNotFoundError:
        sys.exit("error: %s does not exist, so there is no key to export" % path)
    vals = re.findall(r"0x([0-9a-fA-F]{2})", s[s.rindex("{"):])
    if len(vals) < 64:
        sys.exit("error: %s does not contain a 64-byte key block" % path)
    return bytes(int(v, 16) for v in vals[:32])


def backup_json(seed, pub):
    """Byte-for-byte the shape web/keygen.js writes, so either side can read the other's file."""
    return json.dumps({
        "kind": BACKUP_KIND,
        "version": 1,
        "created": __import__("datetime").datetime.now(
            __import__("datetime").timezone.utc).isoformat().replace("+00:00", "Z"),
        "fingerprint": fingerprint(pub),
        "seed_hex": seed.hex(),
        "public_key_hex": pub.hex(),
        "note": ("This is the signing key for one Simulacra fleet. Anyone holding seed_hex can "
                 "enroll and command your boards, so keep it as you would an SSH private key. You "
                 "need it to add a board later: without it, adding one means re-keying and "
                 "reflashing the whole fleet."),
    }, indent=2)


def c_array(b):
    """Format bytes as the project's C-array body: 16 per line, '0x%02x', no trailing comma."""
    vals = [f"0x{x:02x}" for x in b]
    rows = [vals[i:i + 16] for i in range(0, len(vals), 16)]
    return ",\n".join("    " + ", ".join(row) for row in rows)


SK_TMPL = r"""#pragma once
#include <stdint.h>
// Ed25519 SECRET key (seed||pub) for Vigil -- GENERATED. Keep local; NEVER commit to a public repo.
// TweetNaCl 64-byte secret-key format. (Re)generate with tools/gen_ctrl_key.py.
//
// Magic-prefixed for the same reason as sim_ctrl_key.h: the web flasher rewrites the 64 bytes after
// this magic with a keypair generated in the browser, so the published image carries a placeholder
// and the flashed board carries a secret that was never published.
//
// KEEP THE LAYOUT: magic immediately followed by the key, packed, no padding.
typedef struct __attribute__((packed)) {{
    unsigned char magic[16];
    unsigned char key[64];
}} sim_ctrl_sk_block_t;

__attribute__((used))
static const sim_ctrl_sk_block_t SIMULACRA_CTRL_SK_BLOCK = {{
    {{ 'S','I','M','U','L','A','C','R','A',':','C','T','R','L','S','K' }},
    {{
{body}
    }}
}};
#define SIMULACRA_CTRL_SK (SIMULACRA_CTRL_SK_BLOCK.key)
"""

PK_TMPL = r"""#pragma once
#include <stdint.h>
// Ed25519 PUBLIC key for the Vigil->decoy CONFIG link -- GENERATED (safe to share). Must match
// cyd/main/sim_ctrl_sk.h. Decoys verify with this. (Re)generate with tools/gen_ctrl_key.py.
//
// Wrapped in a magic-prefixed block so a built image can be rewritten without rebuilding: the web
// flasher generates a keypair in the browser, finds this 16-byte magic in the downloaded binary and
// replaces the 32 bytes after it. That is what lets a browser-flashed fleet hold a key that exists
// in nobody else's install and was never published. The macro keeps every call site unchanged.
//
// KEEP THE LAYOUT: magic immediately followed by the key, packed, no padding. `used` stops the
// linker discarding the block when only .key is referenced.
typedef struct __attribute__((packed)) {{
    unsigned char magic[16];
    unsigned char key[32];
}} sim_ctrl_pk_block_t;

__attribute__((used))
static const sim_ctrl_pk_block_t SIMULACRA_CTRL_PK_BLOCK = {{
    {{ 'S','I','M','U','L','A','C','R','A',':','C','T','R','L','P','K' }},
    {{
{body}
    }}
}};
#define SIMULACRA_CTRL_PK (SIMULACRA_CTRL_PK_BLOCK.key)
"""


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(text)


def refuse_if_tracked(out_dir):
    """Never write a real secret into a path git is tracking - one `git commit -a` would publish
    fleet control authority. sim_ctrl_sk.h is gitignored; if it is tracked again, stop.

    Runs the check IN out_dir, not always REPO - a --out-dir into a different clone or a worktree
    (e.g. .worktrees/<x>, its own checkout with its own index) needs its own tracked-path check;
    the previous version of this guard only ever checked REPO and silently no-op'd for any other
    --out-dir, so it protected everywhere except the one place a real deploy is likely to run from.
    If out_dir isn't inside a git checkout at all, `git ls-files` just fails and there is nothing to
    accidentally commit - same safe outcome as "not tracked"."""
    r = subprocess.run(["git", "ls-files", "--error-unmatch", SK_REL],
                       cwd=out_dir, capture_output=True, text=True)
    if r.returncode == 0:
        sys.exit(f"error: {SK_REL} is TRACKED by git in {out_dir} - writing a real secret there "
                 f"would publish it.\n"
                 f"  fix: git rm --cached {SK_REL}   (it is already listed in .gitignore)")


def main():
    ap = argparse.ArgumentParser(
        description="Generate, import or export the Ed25519 control signing keypair.",
        epilog="Import a browser-keyed fleet before building from source, or the Vigil you flash "
               "will not be the Vigil your decoys trust.")
    ap.add_argument("--out-dir", default=REPO, help="repo root to write into (default: this repo)")
    ap.add_argument("--no-skip", action="store_true",
                    help="do NOT git-skip-worktree the secret (default protects it from commit)")
    ap.add_argument("--from-backup", metavar="FILE",
                    help="adopt this fleet key instead of generating one: a backup saved from the "
                         "web flasher, a bare 32-byte hex seed, or - for stdin")
    ap.add_argument("--export-backup", metavar="FILE",
                    help="write the CURRENT key out in the web flasher's backup format and exit "
                         "(use - for stdout); generates nothing and changes no header")
    a = ap.parse_args()

    sk_path = os.path.join(a.out_dir, SK_REL)
    pk_path = os.path.join(a.out_dir, PK_REL)

    # Export first: it is read-only, so it must never trip the write guards below.
    if a.export_backup:
        seed = seed_from_header(sk_path)
        pub = pub_from_seed(seed)
        text = backup_json(seed, pub)
        if a.export_backup == "-":
            print(text)
        else:
            write(a.export_backup, text + "\n")
            print(f"wrote {a.export_backup}")
        print(f"  fleet {fingerprint(pub)}")
        print("  This file contains the fleet SECRET. Treat it like an SSH private key.")
        return

    refuse_if_tracked(a.out_dir)

    if a.from_backup:
        text = sys.stdin.read() if a.from_backup == "-" else open(a.from_backup, encoding="utf-8").read()
        seed = seed_from_backup(text)
        pub = pub_from_seed(seed)
        adopted = True
    else:
        seed, pub = gen_keypair()
        adopted = False
    sk64 = seed + pub
    assert len(sk64) == 64 and len(pub) == 32 and pub == sk64[32:64], "malformed keypair"

    write(sk_path, SK_TMPL.format(body=c_array(sk64)))
    write(pk_path, PK_TMPL.format(body=c_array(pub)))
    print(f"wrote {sk_path}")
    print(f"wrote {pk_path}")
    print(f"  fleet {fingerprint(pub)}")

    # The secret is gitignored (and refuse_if_tracked() proved it is not tracked), so it cannot
    # be committed by accident. Confirm that out loud rather than relying on the operator's memory.
    # Checked in out_dir itself, same reasoning as refuse_if_tracked() above.
    if not a.no_skip:
        r = subprocess.run(["git", "check-ignore", "-q", SK_REL], cwd=a.out_dir, capture_output=True)
        print(f"  git: {SK_REL} is gitignored -- your secret stays local" if r.returncode == 0
              else f"  WARNING: {SK_REL} is NOT gitignored -- add it to .gitignore before committing")

    if adopted:
        print("\nADOPTED an existing fleet key. Builds from this tree now match boards already "
              "flashed with it, so you can reflash one board without re-enrolling the rest. Check "
              "the fingerprint above against the one the web flasher shows.")
    else:
        print("\nNEXT: rebuild + reflash EVERY board (decoys bake the new public key, the CYD the "
              "new secret) and re-enroll the fleet, or the CYD's signatures won't verify.")


if __name__ == "__main__":
    main()
