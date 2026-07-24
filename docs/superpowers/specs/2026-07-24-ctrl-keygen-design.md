# Control-Key Generator (`tools/gen_ctrl_key.py`) — Design

**Date:** 2026-07-24
**Status:** Approved (design).

## Goal

One command that generates a fresh Ed25519 **control signing keypair** and installs it into the two
header files the firmware builds from — so a real provisioned deployment's "regenerate the placeholder
signing key" step is a single command, not a manual crypto chore. The public-key header already names
this tool (`// regenerate with tools/gen_ctrl_key.py before real use`); it was planned but never built.

## Background

The provisioned fleet regime authenticates the Vigil with an Ed25519 identity: it signs enrollment
OFFERs and every CONFIG command; decoys verify with the matching public key. The whole handshake's
integrity rests on that secret being genuinely secret. Today both halves are committed **placeholders**
(marked `CHANGE ME` / regenerate). The firmware uses TweetNaCl `crypto_sign_keypair(pk, sk)` →
`pk[32]`, `sk[64]` where `sk = seed‖pub`.

- `cyd/main/sim_ctrl_sk.h` — `static const uint8_t SIMULACRA_CTRL_SK[64]` (secret, seed‖pub)
- `components/simulacra_radar/sim_ctrl_key.h` — `static const uint8_t SIMULACRA_CTRL_PK[32]` (public)

## What it does

`tools/gen_ctrl_key.py` (no arguments needed):

1. Generate a fresh Ed25519 keypair. Crypto backend: **PyNaCl** primary (same NaCl family as the
   firmware's TweetNaCl → byte-identical `seed‖pub`), fall back to the `cryptography` package; if
   neither is importable, exit with a clear `pip install pynacl` message.
2. Compute `sk64 = seed(32) ‖ pub(32)` and `pk32 = pub(32)`. **Assert `pk32 == sk64[32:64]`** (the
   pair can never be mismatched).
3. Overwrite the two headers in the **exact committed format** (same `#pragma once`, include,
   `static const uint8_t NAME[N] = { … };` layout, 16 bytes per line, lowercase `0x` hex), with
   comments changed from "placeholder" to a generated-key notice: the secret header warns
   *"generated secret — keep local, never commit"*, the public header notes *"generated public key —
   safe to share; must match cyd/main/sim_ctrl_sk.h"*.
4. **Protect the secret by default:** run `git update-index --skip-worktree cyd/main/sim_ctrl_sk.h`
   so git ignores local changes to that tracked file — the real secret stays on the machine and can't
   be accidentally staged/committed, while the public repo keeps its placeholder so out-of-box builds
   still work. A `--no-skip` flag opts out. If git isn't available or the file isn't tracked, skip
   this step with a printed note (don't fail).
5. Print a completion notice: the new key means **rebuild + reflash every board** (decoys bake the new
   public key; the CYD bakes the new secret) and **re-enroll** the fleet, or the CYD's signatures
   won't verify.

## Architecture

Single self-contained script, `tools/gen_ctrl_key.py`. Pure standard library except the crypto
backend (PyNaCl / cryptography, already present on the dev host). One small helper formats a byte
string as the project's C-array body. Repo paths resolved relative to the script location
(`tools/..`), so it works from any CWD.

## Testing

`tools/tests/test_gen_ctrl_key.py` (Python `unittest`):

- Run the generator into a **temporary repo layout** (temp dir with `cyd/main/` and
  `components/simulacra_radar/` + a throwaway `git init`, or `--out-dir` pointed at the temp tree — see
  Open questions), then:
  - Parse both generated headers; assert `SIMULACRA_CTRL_SK` is 64 bytes and `SIMULACRA_CTRL_PK` is 32.
  - Assert `PK == SK[32:64]`.
  - **Sign/verify round-trip:** using the parsed SK, sign a test message and verify it under the parsed
    PK — proves a valid, matching Ed25519 pair (not just well-formed arrays).
  - Assert the files keep the required `static const uint8_t SIMULACRA_CTRL_SK[64]` /
    `SIMULACRA_CTRL_PK[32]` declarations (drop-in compatibility).
- Assert two successive runs produce **different** keys (fresh randomness).

## Out of scope

- Build wiring for a gitignored-secret-with-committed-placeholder fallback (`__has_include`) — the
  `--skip-worktree` approach covers the accidental-commit risk without touching the build. Revisit only
  if a cleaner secret-management story is wanted later.
- Key rotation/epoch bookkeeping, HSM/key-storage integration, non-Ed25519 keys.
- Regenerating the *fleet transport key* (that's minted on-device at first boot; unrelated).

## Open questions

- **Test isolation:** give the generator an optional `--out-dir` (default = repo root) so the test can
  target a temp tree without touching the real headers or running git on the real repo. Chosen: yes,
  add `--out-dir` (keeps the test hermetic; default preserves the zero-arg UX). The `--skip-worktree`
  git step only runs when writing to the real repo root (skipped for a custom `--out-dir`).
