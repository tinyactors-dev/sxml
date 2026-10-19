// Runs a WASI (preview 1) module under Node.js: node run-wasi.mjs module.wasm [arguments...]
// Used as CMAKE_CROSSCOMPILING_EMULATOR so ctest can run WebAssembly test executables.
// The exit code of the module becomes the exit code of this process.
//
// Visible to the module (WASI preopens, D93): the working directory as ".",
// and the source tree (the parent of this script's directory) under its own
// absolute path, so that tests which find the vendored suites from __FILE__
// (tests/xml_conformance_test.c: <root>/tests/../third_party/xmlconf) open
// them as they do natively. The whole tree is preopened, not only
// third_party/, because wasi-libc matches preopens by path prefix and does
// not resolve the ".." first. The host's timezone database,
// /usr/share/zoneinfo, is preopened too when it exists, as the sxml command
// reads its zones there (D209).
import { existsSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { argv, cwd, env, exit } from 'node:process';
import { fileURLToPath } from 'node:url';
import { setFlagsFromString } from 'node:v8';
import { WASI } from 'node:wasi';

// Node 22's WASI functions are V8 fast API callbacks, and fd_write allocates
// through uvwasi, which can start a garbage collection inside the fast call.
// The collector then crashes walking the optimized wasm frame (SIGSEGV in
// InnerPointerToCodeCache::GetCacheEntry), whenever a collection happens to
// fall there: about one run in eight of tests that grow memory a lot. Calling
// the WASI functions the ordinary way avoids it (D65). Set before any wasm
// import wrapper is compiled.
setFlagsFromString('--no-turbo-fast-api-calls');

const modulePath = argv[2];
const sourceRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const zoneinfo = '/usr/share/zoneinfo';
const preopens = { '.': cwd(), [sourceRoot]: sourceRoot };
if (existsSync(zoneinfo)) {
  preopens[zoneinfo] = zoneinfo;
}
const wasi = new WASI({
  version: 'preview1',
  args: argv.slice(2),
  env,
  preopens,
  returnOnExit: true,
});
const module = await WebAssembly.compile(await readFile(modulePath));
const instance = await WebAssembly.instantiate(module, wasi.getImportObject());
exit(wasi.start(instance));
