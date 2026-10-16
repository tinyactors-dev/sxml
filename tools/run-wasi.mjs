// Runs a WASI (preview 1) module under Node.js: node run-wasi.mjs module.wasm [arguments...]
// Used as CMAKE_CROSSCOMPILING_EMULATOR so ctest can run WebAssembly test executables.
// The exit code of the module becomes the exit code of this process.
import { readFile } from 'node:fs/promises';
import { argv, cwd, env, exit } from 'node:process';
import { WASI } from 'node:wasi';

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
