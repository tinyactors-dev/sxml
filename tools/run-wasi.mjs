// Runs a WASI (preview 1) module under Node.js: node run-wasi.mjs module.wasm [arguments...]
// Used as CMAKE_CROSSCOMPILING_EMULATOR so ctest can run WebAssembly test executables.
// The exit code of the module becomes the exit code of this process.
import { readFile } from 'node:fs/promises';
import { argv, cwd, env, exit } from 'node:process';
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
const wasi = new WASI({
  version: 'preview1',
  args: argv.slice(2),
  env,
  preopens: { '.': cwd() },
  returnOnExit: true,
});
const module = await WebAssembly.compile(await readFile(modulePath));
const instance = await WebAssembly.instantiate(module, wasi.getImportObject());
exit(wasi.start(instance));
