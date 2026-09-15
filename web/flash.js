// Turn the published images into personalised ones and hand them to esp-web-tools.
//
// THE CONSTRAINT THAT SHAPES THIS FILE
//   esp-web-tools reads a manifest, connects to the board, identifies the chip, and only then
//   downloads the parts for the matching build. There is no hook between "chip identified" and
//   "parts fetched", so there is nowhere to patch an image on demand. Every image therefore has to
//   be personalised UP FRONT, before the user is asked to pick a serial port, and handed over as a
//   manifest whose part paths are blob: URLs pointing at the patched bytes.
//
//   That is why this page has a Prepare step instead of flashing straight away. It also makes the
//   work visible, which for a tool whose whole claim is "your key never leaves this machine" is
//   worth more than hiding it behind a spinner.
//
// WHICH KEY GOES WHERE
//   Decoys get the 32-byte public half and can only verify. The Vigil gets the 64-byte secret and
//   is the only thing that can sign an enrollment offer or a command. The mapping lives in
//   manifest.json next to the paths rather than as a chipFamily switch in here, so adding a board
//   is one manifest entry and not an edit in two files.

import { patchKey } from "./keypatch.js";

/** Fetch one image, patch it for its role, and hand back a blob URL for the patched bytes. */
async function prepareBuild(build, manifestURL, kp, onProgress) {
  const out = { ...build, parts: [] };
  for (const part of build.parts) {
    const url = new URL(part.path, manifestURL).toString();
    onProgress(`downloading ${part.path}`);
    const resp = await fetch(url, { cache: "no-store" });
    if (!resp.ok) throw new Error(`could not download ${part.path}: HTTP ${resp.status}`);
    const bytes = new Uint8Array(await resp.arrayBuffer());

    let blob;
    if (build.simulacra_key) {
      onProgress(`keying ${part.path}`);
      const key = build.simulacra_key === "ctrl_sk" ? kp.secretKey : kp.publicKey;
      const { image, count } = await patchKey(bytes, build.simulacra_key, key);
      if (!count) throw new Error(`no key block found in ${part.path}`);
      blob = new Blob([image], { type: "application/octet-stream" });
      out.simulacra_patched = count;
    } else {
      // No declared key role: pass the image through untouched rather than guessing at one.
      blob = new Blob([bytes], { type: "application/octet-stream" });
    }
    out.parts.push({ ...part, path: URL.createObjectURL(blob) });
  }
  return out;
}

/**
 * Download every build, patch each with `kp`, and return a blob: URL for a manifest that points at
 * the patched images. Hand that to esp-web-tools in place of the static manifest.
 *
 * All three are prepared even though only one gets flashed, because which one is needed is not
 * known until the chip has been read, and by then it is too late to fetch anything.
 */
export async function prepareManifest(manifestPath, kp, onProgress = () => {}) {
  const manifestURL = new URL(manifestPath, location.href).toString();
  const resp = await fetch(manifestURL, { cache: "no-store" });
  if (!resp.ok) throw new Error(`could not read the manifest: HTTP ${resp.status}`);
  const manifest = await resp.json();

  const builds = [];
  for (const b of manifest.builds) {
    builds.push(await prepareBuild(b, manifestURL, kp, onProgress));
  }
  onProgress("ready");

  const patched = { ...manifest, builds };
  const blob = new Blob([JSON.stringify(patched)], { type: "application/json" });
  // Part paths are absolute blob: URLs, so esp-web-tools resolving them against this blob: manifest
  // URL leaves them untouched.
  return { url: URL.createObjectURL(blob), manifest: patched };
}

/** Free the object URLs from a previous prepare, so re-preparing does not leak whole firmware
 *  images into the tab for as long as it stays open. */
export function releaseManifest(prepared) {
  if (!prepared) return;
  URL.revokeObjectURL(prepared.url);
  for (const b of prepared.manifest.builds) {
    for (const p of b.parts) {
      if (p.path.startsWith("blob:")) URL.revokeObjectURL(p.path);
    }
  }
}
