// Run the browser's own patcher outside a browser, so its OUTPUT can be diffed against the Python
// reference instead of just grepping both files for matching constants.
//
// Two implementations of the same byte-exact format drift quietly: a constant can match while the
// arithmetic around it does not, and the symptom is a board that flashes and never boots. This is
// driven by test_keypatch.py's JsMatchesPythonOnRealBytes.
//
// Usage: node xcheck_keypatch.mjs <in.bin> <which> <key-hex> <out.bin>
import { readFileSync, writeFileSync } from "node:fs";
import { patchKey } from "./keypatch.js";

const [, , inPath, which, keyHex, outPath] = process.argv;
const img = new Uint8Array(readFileSync(inPath));
const key = Uint8Array.from(Buffer.from(keyHex, "hex"));
const { image, count } = await patchKey(img, which, key);
if (image.length !== img.length) throw new Error("patched image changed size");
writeFileSync(outPath, image);
process.stdout.write(String(count));
