// The size budget of the WebAssembly modules (D93, D138; ctest
// javascript/size_budget): node size-budget.mjs MODULE STRIPPED GZIPPED
// [MODULE STRIPPED GZIPPED]...
//
// For each module: its size stripped (without custom sections: names,
// debug information, producers; what llvm-strip leaves) and that stripped
// module compressed by gzip -9 must not pass the two limits, in bytes.
// Prints the sizes, so the test log records the growth D93 asks to log.
//
// The growth guard (D225): when the linker wrote a map next to a module
// (MODULE with .map for .wasm, -Wl,-Map in CMakeLists.txt) and
// size-baseline/NAME.txt exists beside this script, the objects that grew
// most since that baseline are printed: the five largest growths always,
// twenty when the module is over budget, so the commit that grew it and
// the part that did are named at once. Refresh a baseline after an audit
// or a budget raise, in the same change:
//   node size-budget.mjs --write-baseline MAP BASELINE
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { basename, dirname, join } from 'node:path';
import { argv, exit } from 'node:process';
import { fileURLToPath } from 'node:url';
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

/**
 * The bytes of code and data each input object gives a module, from the
 * map wasm-ld writes: a Map from an object's name (libsxml.a(xml), or
 * libsxml.a(xml) [data]) to its byte count.
 */
function objectSizes(mapText) {
  const sizes = new Map();
  let section = '';
  for (const line of mapText.split('\n')) {
    const fields = line.trim().split(/\s+/);
    if (fields.length === 4 && /^[A-Z]+$/.test(fields[3])) {
      section = fields[3];
      continue;
    }
    if (fields.length < 4 || (section !== 'CODE' && section !== 'DATA')) {
      continue;
    }
    const input = fields[3];
    const end = input.indexOf(':(');
    if (end < 0) {
      continue;
    }
    const name =
      input
        .slice(0, end)
        .replace(/.*\/CMakeFiles\/[^/]+\.dir\//, '')
        .replace(/^.*\/(lib[^/]+\.a)\(/, '$1(')
        .replace(/^\/.*\//, '')
        .replace(/\.c\.obj|\.obj/g, '') + (section === 'DATA' ? ' [data]' : '');
    sizes.set(name, (sizes.get(name) ?? 0) + Number.parseInt(fields[2], 16));
  }
  return sizes;
}

function readBaseline(path) {
  const sizes = new Map();
  for (const line of readFileSync(path, 'utf8').split('\n')) {
    const match = /^(\d+) (.+)$/.exec(line);
    if (match) {
      sizes.set(match[2], Number(match[1]));
    }
  }
  return sizes;
}

/** Prints the count largest growths of the map's objects over the baseline. */
function printGrowth(mapPath, baselinePath, count) {
  const current = objectSizes(readFileSync(mapPath, 'utf8'));
  const baseline = readBaseline(baselinePath);
  const growth = [];
  for (const name of new Set([...current.keys(), ...baseline.keys()])) {
    const change = (current.get(name) ?? 0) - (baseline.get(name) ?? 0);
    if (change !== 0) {
      growth.push([change, name]);
    }
  }
  growth.sort((first, second) => second[0] - first[0]);
  const total = growth.reduce((sum, [change]) => sum + change, 0);
  console.log(`  code and data since ${basename(baselinePath)}: ${total >= 0 ? '+' : ''}${total} bytes`);
  for (const [change, name] of growth.slice(0, count)) {
    if (change > 0) {
      console.log(`    +${change} ${name}`);
    }
  }
}

if (argv[2] === '--write-baseline') {
  const sizes = [...objectSizes(readFileSync(argv[3], 'utf8'))].sort((first, second) =>
    first[0] < second[0] ? -1 : 1,
  );
  writeFileSync(argv[4], sizes.map(([name, size]) => `${size} ${name}\n`).join(''));
  exit(0);
}

const baselineDirectory = join(dirname(fileURLToPath(import.meta.url)), 'size-baseline');
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
  const mapPath = path.replace(/\.wasm$/, '.map');
  const baselinePath = join(baselineDirectory, `${basename(path, '.wasm')}.txt`);
  if (mapPath !== path && existsSync(mapPath) && existsSync(baselinePath)) {
    printGrowth(mapPath, baselinePath, within ? 5 : 20);
  }
  failed ||= !within;
}
exit(failed ? 1 : 0);
