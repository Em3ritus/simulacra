// Generate, store and back up the fleet's Ed25519 control keypair, in the browser.
//
// WHY THIS IS THE WHOLE POINT
//   The images on this page are downloadable by anyone, so any key compiled into them is public by
//   definition. The fix is not a better-hidden baked key, it is to have no baked key worth having:
//   the keypair is made HERE, on the machine doing the flashing, and written into the image on its
//   way to the board. Nothing that matters is ever published.
//
//   Everything else follows from that one key. The Vigil signs enrollment offers with the secret
//   half and decoys verify with the public half, so only your controller can enroll your boards.
//   The ESP-NOW transport key is then minted at random by the Vigil on first boot and handed over
//   during that authenticated enrollment, which means it never exists in any binary either.
//
// WHAT IS STORED, AND WHERE
//   Only the 32-byte seed, hex, in localStorage under one key. The public half and the 64-byte
//   TweetNaCl secret are derived from it on demand, so there is exactly one secret to look after.
//   localStorage is per-origin and per-browser: clearing site data loses it, and so does flashing
//   your third board from a different machine. Hence the backup file, which is not optional if you
//   ever intend to add a board.
//
// FORMAT
//   tools/gen_ctrl_key.py builds the firmware's secret as seed||pub, which is exactly TweetNaCl's
//   64-byte secretKey. nacl.sign.keyPair.fromSeed(seed) produces the same bytes, so a fleet keyed
//   from this page and one keyed from the Python tool are interchangeable.

const STORAGE_KEY = "simulacra.ctrl.seed.v1";
const BACKUP_KIND = "simulacra-fleet-key";

/** tweetnacl is loaded as a classic script and lands on window. Fail loudly rather than silently
 *  generating something weak if it did not load. */
function nacl() {
  const n = globalThis.nacl;
  if (!n || !n.sign || !n.sign.keyPair) {
    throw new Error("tweetnacl did not load, so no key can be generated. Check your connection " +
                    "and reload; flashing without it would bake a published key into your boards.");
  }
  return n;
}

export const toHex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");

export function fromHex(s) {
  const t = String(s).trim().replace(/\s+/g, "").toLowerCase();
  if (!/^[0-9a-f]*$/.test(t) || t.length % 2) throw new Error("not valid hex");
  const out = new Uint8Array(t.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(t.substr(i * 2, 2), 16);
  return out;
}

/** Derive the full keypair from a 32-byte seed. */
export function fromSeed(seed) {
  if (seed.length !== 32) throw new Error(`seed must be 32 bytes, got ${seed.length}`);
  const kp = nacl().sign.keyPair.fromSeed(seed);
  // Cheap self-check: the firmware assumes secretKey is literally seed||pub. If a future tweetnacl
  // ever changed that, every board would flash fine and no signature would verify.
  if (kp.secretKey.length !== 64 || toHex(kp.secretKey.slice(0, 32)) !== toHex(seed) ||
      toHex(kp.secretKey.slice(32)) !== toHex(kp.publicKey)) {
    throw new Error("tweetnacl produced a secret key that is not seed||pub; refusing to use it");
  }
  return { seed, publicKey: kp.publicKey, secretKey: kp.secretKey };
}

/** A fresh keypair from the browser's CSPRNG. Not stored; call save() to keep it. */
export function generate() {
  const seed = new Uint8Array(32);
  crypto.getRandomValues(seed);
  return fromSeed(seed);
}

export function load() {
  let hex;
  try {
    hex = localStorage.getItem(STORAGE_KEY);
  } catch {
    return null;                     // private window, or site data blocked
  }
  if (!hex) return null;
  try {
    return fromSeed(fromHex(hex));
  } catch {
    return null;                     // corrupt entry: treat as absent rather than wedging the page
  }
}

export function save(kp) {
  localStorage.setItem(STORAGE_KEY, toHex(kp.seed));
}

export function forget() {
  try {
    localStorage.removeItem(STORAGE_KEY);
  } catch {
    /* nothing stored anyway */
  }
}

/**
 * Short identifier for a public key, so the same fleet key is recognisable across the page, the
 * backup file, and two different boards. It is a display aid, not a security check.
 */
export async function fingerprint(publicKey) {
  const d = new Uint8Array(await crypto.subtle.digest("SHA-256", publicKey));
  return toHex(d.slice(0, 8)).replace(/(.{4})(?=.)/g, "$1-");
}

/** The backup file's contents. The seed is the secret; everything else is derivable and is written
 *  only so a human opening the file can tell what it is and which fleet it belongs to. */
export async function backupJSON(kp) {
  return JSON.stringify(
    {
      kind: BACKUP_KIND,
      version: 1,
      created: new Date().toISOString(),
      fingerprint: await fingerprint(kp.publicKey),
      seed_hex: toHex(kp.seed),
      public_key_hex: toHex(kp.publicKey),
      note:
        "This is the signing key for one Simulacra fleet. Anyone holding seed_hex can enroll and " +
        "command your boards, so keep it as you would an SSH private key. You need it to add a " +
        "board later: without it, adding one means re-keying and reflashing the whole fleet.",
    },
    null,
    2
  );
}

/** Accept a backup file, or a bare 32-byte hex seed pasted in. */
export function importBackup(text) {
  const t = String(text).trim();
  if (t.startsWith("{")) {
    const o = JSON.parse(t);
    if (o.kind !== BACKUP_KIND) throw new Error("that JSON is not a Simulacra fleet key backup");
    if (!o.seed_hex) throw new Error("backup has no seed_hex");
    const kp = fromSeed(fromHex(o.seed_hex));
    // If the file also carries the public half, make sure it agrees. A mismatch means the file was
    // edited or truncated, and importing it would key a fleet nobody can control.
    if (o.public_key_hex && toHex(kp.publicKey) !== String(o.public_key_hex).toLowerCase()) {
      throw new Error("backup is inconsistent: its public key does not match its seed");
    }
    return kp;
  }
  return fromSeed(fromHex(t));
}
