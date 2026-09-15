// Rewrite the magic-tagged key blocks inside a built ESP-IDF image, in the browser, before it is
// flashed.
//
// WHY
//   Anything compiled into a published binary is public. These images are downloadable from the
//   Pages site, so a key baked into one is a key everybody has. Generating the keypair here and
//   rewriting it into the image at flash time is the only way a browser-flashed fleet ends up
//   holding something that was never published.
//
// THE THREE THINGS THAT BREAK A PATCHED IMAGE
//   An ESP-IDF app image carries two integrity values over its own contents, and a patched image
//   that ignores either is rejected by the bootloader with nothing on the console: the boot log
//   stops after the partition table and the app never starts. Both were found that way on hardware.
//
//     1. A 1-byte XOR checksum over every segment's DATA bytes, seeded 0xEF, written as the last
//        byte of the 16-byte block following the final segment.
//     2. A 32-byte SHA256 over the app image before it, appended at the end, present when byte 23
//        of the app header is 1.
//
//   The third is about WHERE those live. What this file downloads is a MERGED flash image from
//   esptool merge_bin: padding, then the bootloader, then the partition table, then the app. Byte 0
//   is not the app's header, and on ESP32 it is not even 0xE9. Walking segments from byte 0 parses
//   the bootloader, writes the checksum into the middle of the file, and hashes the wrong range,
//   producing an image that flashes perfectly and never boots. So everything here works on the app
//   image located THROUGH THE PARTITION TABLE.
//
//   tools/patch_keyblock.py is the reference implementation and produces byte-identical output.
//   Keep them in step; web/test_keypatch.py checks that the constants have not drifted apart.

export const MAGICS = {
  // decoy: Ed25519 public key. Verifies the Vigil's enrollment offers and CONFIG commands.
  ctrl_pk: { magic: "SIMULACRA:CTRLPK", size: 32 },
  // Vigil: Ed25519 secret, TweetNaCl seed||pub 64-byte format.
  ctrl_sk: { magic: "SIMULACRA:CTRLSK", size: 64 },
};

const PART_TABLE_OFF = 0x8000; // fixed by ESP-IDF
const PART_TYPE_APP = 0;

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

/**
 * Exact byte length of the app image beginning at `start`, from its own segment table.
 * A merged image carries the app with other data after it, so the end of the file is NOT the end
 * of the app and the appended SHA256 is not the file's last 32 bytes.
 */
function appLen(d, start) {
  if (start + 24 > d.length || d[start] !== 0xe9) {
    throw new Error(`no ESP app image header at 0x${start.toString(16)}`);
  }
  const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
  const nseg = d[start + 1];
  let off = start + 24;
  for (let s = 0; s < nseg; s++) {
    if (off + 8 > d.length) throw new Error("segment table runs past end of file");
    off += 8 + dv.getUint32(off + 4, true);
  }
  if (off > d.length) throw new Error("segment data runs past end of file");
  const cksOff = off + (15 - ((off - start) % 16));
  let n = cksOff + 1 - start;
  if (d[start + 23] === 1) n += 32;
  if (start + n > d.length) throw new Error("app image runs past end of file");
  return n;
}

/**
 * `{start, length}` of the app image inside `d`. Accepts a raw app image (0xE9 at byte 0) or a
 * merged flash image, which is what the Pages site serves. For the merged form the app is found by
 * reading the partition table rather than by assuming an offset, because the app partition's
 * offset comes from the project's partition CSV and is not the same everywhere.
 *
 * THE PARTITION TABLE IS TRIED FIRST, and that order is load-bearing. 0xE9 at byte 0 does NOT mean
 * "raw app image": the BOOTLOADER has the same magic, and its flash offset is per-chip -- 0x1000 on
 * ESP32, 0x2000 on C5, but 0x0 on C6 and H2. So on a merged C6 image byte 0 is the bootloader's
 * header, and trusting it would checksum and hash the bootloader while leaving the app untouched.
 * A raw app image has no partition table to find, so it falls through correctly.
 */
export function findAppImage(d) {
  const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
  if (d.length >= PART_TABLE_OFF + 32 && d[PART_TABLE_OFF] === 0xaa && d[PART_TABLE_OFF + 1] === 0x50) {
    for (let i = 0; i < 0x1000; i += 32) {
      const e = PART_TABLE_OFF + i;
      if (e + 32 > d.length || d[e] !== 0xaa || d[e + 1] !== 0x50) break; // end of table / MD5 marker
      if (d[e + 2] === PART_TYPE_APP) {
        const off = dv.getUint32(e + 4, true);
        if (off > 0 && off < d.length && d[off] === 0xe9) {
          try {
            return { start: off, length: appLen(d, off) };
          } catch {
            /* malformed entry; keep looking */
          }
        }
      }
    }
  }

  if (d[0] === 0xe9) return { start: 0, length: appLen(d, 0) };
  throw new Error(
    `no app image: byte 0 is not 0xE9 and no partition table at 0x${PART_TABLE_OFF.toString(16)} ` +
      `lists an app partition holding one`
  );
}

/** Recompute the app image's segment checksum and appended SHA256. Mutates and returns `d`. */
export async function repairImage(d) {
  const { start, length } = findAppImage(d);
  const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
  const nseg = d[start + 1];
  const hashAppended = d[start + 23] === 1;

  let off = start + 24;
  let xor = 0xef;
  for (let s = 0; s < nseg; s++) {
    const len = dv.getUint32(off + 4, true);
    off += 8;
    for (let i = 0; i < len; i++) xor ^= d[off + i];
    off += len;
  }

  // Counted from the start of the APP IMAGE, not the start of the file.
  const cksOff = off + (15 - ((off - start) % 16));
  if (cksOff >= d.length) throw new Error("checksum offset past end of image");
  d[cksOff] = xor;

  if (hashAppended) {
    const end = start + length;
    const h = new Uint8Array(await crypto.subtle.digest("SHA-256", d.subarray(start, end - 32)));
    d.set(h, end - 32);
  }
  return d;
}

/** True if the embedded app image's own checksum and hash agree with its contents. */
export async function verifyImage(d) {
  let start, length;
  try {
    ({ start, length } = findAppImage(d));
  } catch {
    return false;
  }
  const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
  const nseg = d[start + 1];
  let off = start + 24,
    xor = 0xef;
  for (let s = 0; s < nseg; s++) {
    const len = dv.getUint32(off + 4, true);
    off += 8;
    for (let i = 0; i < len; i++) xor ^= d[off + i];
    off += len;
  }
  const cksOff = off + (15 - ((off - start) % 16));
  if (d[cksOff] !== xor) return false;
  if (d[start + 23] === 1) {
    const end = start + length;
    const want = new Uint8Array(await crypto.subtle.digest("SHA-256", d.subarray(start, end - 32)));
    for (let i = 0; i < 32; i++) if (want[i] !== d[end - 32 + i]) return false;
  }
  return true;
}

/**
 * Replace every copy of one key block inside the app image, then repair the image.
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
  const { start, length } = findAppImage(d);
  const hits = findAll(d, spec.magic).filter(
    (h) => h >= start && h + spec.magic.length + spec.size <= start + length
  );
  if (!hits.length) {
    throw new Error(
      `key block ${spec.magic} not found in the app image. This firmware predates browser key ` +
        `generation, so it cannot be personalised.`
    );
  }
  for (const h of hits) d.set(key, h + spec.magic.length);
  await repairImage(d);
  if (!(await verifyImage(d))) throw new Error("patched image failed its own integrity check");
  return { image: d, count: hits.length };
}
