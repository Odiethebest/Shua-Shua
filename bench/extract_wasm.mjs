// bench/extract_wasm.mjs — pull the embedded wasm out of a SINGLE_FILE build.
//
// scripts/build-wasm.sh passes -sSINGLE_FILE=1, which inlines the .wasm into the
// .js. Emscripten 6 embeds it as a raw latin1 string literal (not a base64 data
// URI), so we locate the wasm magic "\0asm\x01\0\0\0" inside a string literal,
// walk out to that literal's quotes, and decode it.
//
// Usage: node bench/extract_wasm.mjs <shuashua.js> <out.wasm>

import { readFileSync, writeFileSync } from "node:fs";

const [, , inPath, outPath] = process.argv;
if (!inPath || !outPath) {
  console.error("usage: node extract_wasm.mjs <shuashua.js> <out.wasm>");
  process.exit(2);
}

const src = readFileSync(inPath, "utf8");
// The wasm preamble: NUL 'a' 's' 'm' followed by the u32 version 1. Built from
// escapes rather than inlined literally — with raw control bytes in it, git
// classifies this source file as binary.
const magic = "\u0000asm\u0001\u0000\u0000\u0000";
const at = src.indexOf(magic);
if (at < 0) {
  console.error("wasm magic not found — is this a -sSINGLE_FILE build?");
  process.exit(1);
}

// Walk back one char to the literal's opening quote.
const quote = src[at - 1];
if (quote !== "'" && quote !== '"') {
  console.error(`unexpected char before wasm magic: ${JSON.stringify(quote)}`);
  process.exit(1);
}

// Scan forward to the matching close quote, honouring backslash escapes.
let i = at;
for (;;) {
  const c = src[i];
  if (c === quote) break;
  i += c === "\\" ? 2 : 1;
}

const literal = src.slice(at - 1, i + 1);
const decoded = (0, eval)(literal); // a string literal and nothing else
const buf = Buffer.from(decoded, "latin1");
writeFileSync(outPath, buf);

const okMagic = buf.subarray(0, 4).toString("hex") === "0061736d";
console.log(`extracted ${buf.length} bytes -> ${outPath}`);
console.log(`  magic ${okMagic ? "ok" : "BAD"}, wasm version ${buf.readUInt32LE(4)}`);
process.exit(okMagic ? 0 : 1);
