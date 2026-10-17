// The size budget of the WebAssembly modules (D93, D138; ctest
// javascript/size_budget): node size-budget.mjs MODULE STRIPPED GZIPPED
// [MODULE STRIPPED GZIPPED]...
//
// For each module: its size stripped (without custom sections: names,
// debug information, producers; what llvm-strip leaves) and that stripped
// module compressed by gzip -9 must not pass the two limits, in bytes.
// Prints the sizes, so the test log records the growth D93 asks to log.
import { readFileSync } from 'node:fs';
import { basename } from 'node:path';
import { argv, exit } from 'node:process';
import { gzipSync } from 'node:zlib';

/** The module without its custom sections (section id 0). */
function strip(bytes) {
  const headerByteCount = 8;
  const kept = [bytes.subarray(0, headerByteCount)];
  let offset = headerByteCount;
  while (offset < bytes.length) {
    const start = offset;
    const id = bytes[offset];
    offset += 1;
    let size = 0;
    let shift = 0;
    let byte;
    do {
      byte = bytes[offset];
      offset += 1;
      size += (byte & 0x7f) * 2 ** shift;
      shift += 7;
    } while (byte & 0x80);
    offset += size;
    if (id !== 0) {
      kept.push(bytes.subarray(start, offset));
    }
  }
  return Buffer.concat(kept);
}

let failed = false;
for (let index = 2; index + 2 < argv.length + 1; index += 3) {
  const path = argv[index];
  const strippedLimit = Number(argv[index + 1]);
  const gzippedLimit = Number(argv[index + 2]);
  const stripped = strip(readFileSync(path));
  const gzippedByteCount = gzipSync(stripped, { level: 9 }).length;
  const within = stripped.length <= strippedLimit && gzippedByteCount <= gzippedLimit;
  console.log(
    `${basename(path)}: ${stripped.length} bytes stripped (limit ${strippedLimit}), ` +
      `${gzippedByteCount} gzipped (limit ${gzippedLimit})${within ? '' : ': OVER BUDGET'}`,
  );
  failed ||= !within;
}
exit(failed ? 1 : 0);
