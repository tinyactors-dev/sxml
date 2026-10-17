// The WASI (preview 1) imports of sxml.wasm, for Node.js and browsers alike.
//
// sxml does no I/O of its own (D10): the library never prints and reads
// resources only through the resolver, which the package implements. What
// wasi-libc still imports is the clock (fn:current-dateTime, through
// sxml_system_clock) and the stdio functions it links but sxml never calls
// with a real descriptor. So instead of node:wasi or a browser polyfill, this
// is the whole system interface: the clocks are real, everything else
// answers EBADF or ENOSYS. Any import the module names that is not here gets
// the ENOSYS stub, so a newer wasi-libc cannot break loading.

const errno_success = 0;
const errno_bad_file_descriptor = 8;
const errno_invalid = 28;
const errno_not_supported = 52;

const clock_realtime = 0;
const clock_monotonic = 1;

/** Builds the wasi_snapshot_preview1 import object for module, writing into
 *  the memory that memory() returns once the instance exists. */
export function wasiImports(
  module: WebAssembly.Module,
  memory: () => WebAssembly.Memory,
): WebAssembly.ModuleImports {
  const implemented: WebAssembly.ModuleImports = {
    clock_time_get(clockId: number, _precision: bigint, timeAddress: number): number {
      let nanoseconds: bigint;
      if (clockId === clock_realtime) {
        nanoseconds = BigInt(Date.now()) * 1_000_000n;
      } else if (clockId === clock_monotonic) {
        nanoseconds = BigInt(Math.round(performance.now() * 1_000_000));
      } else {
        return errno_invalid;
      }
      new DataView(memory().buffer).setBigUint64(timeAddress, nanoseconds, true);
      return errno_success;
    },
    fd_write(): number {
      return errno_bad_file_descriptor;
    },
    fd_close(): number {
      return errno_bad_file_descriptor;
    },
    fd_seek(): number {
      return errno_bad_file_descriptor;
    },
    proc_exit(exitCode: number): never {
      throw new Error(`sxml.wasm called proc_exit(${exitCode})`);
    },
  };
  const imports: WebAssembly.ModuleImports = {};
  for (const entry of WebAssembly.Module.imports(module)) {
    if (entry.module === 'wasi_snapshot_preview1') {
      imports[entry.name] = implemented[entry.name] ?? (() => errno_not_supported);
    }
  }
  return imports;
}
