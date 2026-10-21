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
//
// WHY A PANEL HAS TO BE CHOSEN BY HAND
//   Two CYD revisions ship under the same ESP32-2432S028 name with different display controllers
//   (ILI9341 on the classic micro-USB board, ST7789 on the USB-C one), and the firmware binds its
//   panel driver at compile time. Both are a plain ESP32 on the wire, so the chip identification
//   esp-web-tools does cannot tell them apart -- and handed two builds with the same chipFamily it
//   would quietly flash whichever came first. So the user says which board they have, and the
//   build that does not match is dropped here, before anything is downloaded or flashed.

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
 * Every surviving build is prepared even though only one gets flashed, because which one is needed
 * is not known until the chip has been read, and by then it is too late to fetch anything.
 *
 * `panel` is the CYD display controller the user picked ("ili9341" | "st7789"). A build naming a
 * different one is dropped; a build naming none (either decoy) is always kept.
 */
export async function prepareManifest(manifestPath, kp, panel, onProgress = () => {}) {
  const manifestURL = new URL(manifestPath, location.href).toString();
  const resp = await fetch(manifestURL, { cache: "no-store" });
  if (!resp.ok) throw new Error(`could not read the manifest: HTTP ${resp.status}`);
  const manifest = await resp.json();

  const wanted = manifest.builds.filter((b) => !b.simulacra_panel || b.simulacra_panel === panel);
  if (manifest.builds.some((b) => b.simulacra_panel) && !wanted.some((b) => b.simulacra_panel)) {
    throw new Error(`no firmware for a "${panel}" display, so a CYD could not be flashed`);
  }
  // Fail loudly rather than leave the choice to manifest order. If two builds for one chipFamily
  // ever survive the filter, which one gets flashed is decided by whichever came first, and for the
  // CYD that is a flash that looks fine and lights up the wrong panel driver -- the exact failure
  // this filter exists to prevent. Cheaper to catch here than on someone's bench.
  const fams = wanted.map((b) => b.chipFamily);
  if (new Set(fams).size !== fams.length) {
    throw new Error(`manifest has two builds for one chip after choosing "${panel}"`);
  }

  const builds = [];
  for (const b of wanted) {
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
