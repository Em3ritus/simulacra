// Rewrite the magic-tagged key blocks inside a built ESP-IDF app image, in the browser, before it
// is flashed.
//
// WHY
//   Anything compiled into a published binary is public. These images are downloadable from the
//   Pages site, so a key baked into one is a key everybody has. Generating the keypair here and
//   rewriting it into the image at flash time is the only way a browser-flashed fleet ends up
//   holding something that was never published.
//
// THE TWO THINGS THAT BREAK A PATCHED IMAGE
//   An ESP-IDF app image carries two integrity values over its own contents, and a patched image
//   that ignores either is rejected by the bootloader with nothing on the console: the boot log
//   stops after the partition table and the app never starts. Both were found that way on hardware.
//
//     1. A 1-byte XOR checksum over every segment's DATA bytes, seeded 0xEF, written as the last
//        byte of the 16-byte block following the final segment.
//     2. A 32-byte SHA256 over the whole image before it, appended at the end, present when byte 23
//        of the header is 1.
//
//   repairImage() fixes both, in that order. tools/patch_keyblock.py is the reference implementation
//   and produces byte-identical output; keep them in step.

export const MAGICS = {
  // decoy: Ed25519 public key. Verifies the Vigil's enrolment offers and CONFIG commands.
  ctrl_pk: { magic: "SIMULACRA:CTRLPK", size: 32 },
  // Vigil: Ed25519 secret, TweetNaCl seed||pub 64-byte format.
  ctrl_sk: { magic: "SIMULACRA:CTRLSK", size: 64 },
};

const enc = new TextEncoder();

/** Byte offsets of every occurrence of `needle` in `hay`. */
export function findAll(hay, needle) {
  const n = enc.encode(needle);
  const out = [];
  outer: for (let i = 0; i + n.length <= hay.length; i++) {
    for (let j = 0; j < n.length; j++) if (hay[i + j] !== n[j]) continue outer;
    out.push(i);
  }
  return out;
}

/** Recompute the segment checksum and the appended SHA256. Mutates and returns `d`. */
export async function repairImage(d) {
  if (d[0] !== 0xe9) throw new Error(`not an ESP app image (magic 0x${d[0].toString(16)})`);
  const nseg = d[1];
  const hashAppended = d[23] === 1;
  const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);

  let off = 24;
  let xor = 0xef;
  for (let s = 0; s < nseg; s++) {
    const len = dv.getUint32(off + 4, true);
    off += 8;
    for (let i = 0; i < len; i++) xor ^= d[off + i];
    off += len;
  }

  const cksOff = off + (15 - (off % 16));
  if (cksOff >= d.length) throw new Error("checksum offset past end of image");
  d[cksOff] = xor;

  if (hashAppended) {
    const body = d.subarray(0, d.length - 32);
    const h = new Uint8Array(await crypto.subtle.digest("SHA-256", body));
    d.set(h, d.length - 32);
  }
  return d;
}

/** True if the image's own checksum and hash agree with its contents. */
export async function verifyImage(d) {
  if (d[0] !== 0xe9) return false;
  const nseg = d[1];
  const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
  let off = 24, xor = 0xef;
  for (let s = 0; s < nseg; s++) {
    const len = dv.getUint32(off + 4, true);
    off += 8;
    for (let i = 0; i < len; i++) xor ^= d[off + i];
    off += len;
  }
  const cksOff = off + (15 - (off % 16));
  if (d[cksOff] !== xor) return false;
  if (d[23] === 1) {
    const want = new Uint8Array(await crypto.subtle.digest("SHA-256", d.subarray(0, d.length - 32)));
    for (let i = 0; i < 32; i++) if (want[i] !== d[d.length - 32 + i]) return false;
  }
  return true;
}

/**
 * Replace every copy of one key block, then repair the image.
 * A header-defined block can land in more than one translation unit, so every copy is rewritten:
 * leaving a stale one risks it being the copy that actually gets used.
 * Returns { image, count }. Throws if the magic is absent, which means the image was built from a
 * tree without the key-block headers.
 */
export async function patchKey(imageBytes, which, key) {
  const spec = MAGICS[which];
  if (!spec) throw new Error(`unknown key block: ${which}`);
  if (key.length !== spec.size) {
    throw new Error(`${which} needs ${spec.size} bytes, got ${key.length}`);
  }
  const d = new Uint8Array(imageBytes); // copy; never mutate the fetched buffer
  const hits = findAll(d, spec.magic);
  if (!hits.length) {
    throw new Error(`key block ${spec.magic} not found in image. This firmware predates ` +
                    `browser key generation, so it cannot be personalised.`);
  }
  for (const h of hits) d.set(key, h + spec.magic.length);
  await repairImage(d);
  if (!(await verifyImage(d))) throw new Error("patched image failed its own integrity check");
  return { image: d, count: hits.length };
}
