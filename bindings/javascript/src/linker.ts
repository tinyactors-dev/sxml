// The module linker (docs/GLOSSARY.md, `module_linker`; D293): links a
// tree-sitter language file, a WebAssembly side module as
// `tree-sitter build --wasm` writes it (`tree-sitter-NAME.wasm`), into the
// running sxml.wasm, so that sxml's tree-sitter runtime calls its lexer
// and its external scanner like those of a grammar compiled in.
//
// The side module's ABI is the WebAssembly tool conventions' "Dynamic
// linking" (DynamicLinking.md): a `dylink.0` custom section gives the
// memory and table sizes it needs; it imports env.memory,
// env.__indirect_function_table, env.__stack_pointer, env.__memory_base
// and env.__table_base, and places its data at __memory_base and its
// functions at __table_base; it may export __wasm_apply_data_relocs and
// __wasm_call_ctors, which run once. tree-sitter's own loader
// (lib/src/wasm_store.c, ts_wasm_store_load_language) reads the same
// files the same way, and its external scanners may import the C
// library functions of lib/src/wasm-stdlib/imports.txt, plus abort and
// __assert_fail. sxml.wasm exports its memory, its growable function
// table, its stack pointer and those functions
// (bindings/javascript/CMakeLists.txt), so the side module shares all of
// them: one memory, one heap, one table, one stack.
//
// Memory for the side module's data comes from sxml.wasm's own malloc,
// so it never overlaps sxml's heap; its table slots are added at the end
// of the table. Neither is ever released: a WebAssembly instance cannot
// be unloaded while sxml's table holds its functions.

/** What the linker needs of the loaded sxml.wasm. */
export interface LinkTarget {
  memory: WebAssembly.Memory;
  table: WebAssembly.Table;
  stackPointer: WebAssembly.Global;
  /** The C library functions sxml.wasm exports for external scanners. */
  functions: ReadonlyMap<string, Function>;
  /** Reserves zeroed memory with sxml.wasm's malloc; never released. */
  reserve(byteCount: number, alignment: number): number;
}

/** A linked language module. */
export interface LinkedLanguage {
  /** NAME of the module's tree_sitter_NAME function. */
  functionName: string;
  /** What tree_sitter_NAME() returned: the address of its TSLanguage. */
  languageAddress: number;
}

/** The contents of a dylink.0 section that the linker uses. */
interface DylinkInfo {
  memorySize: number;
  memoryAlignment: number;
  tableSize: number;
  tableAlignment: number;
  neededLibraries: string[];
}

// dylink.0 subsection types (DynamicLinking.md, "The dylink.0 Section").
const dylink_memory_info = 1;
const dylink_needed = 2;

function refused(reason: string): Error {
  return new Error(`sxml: the module is not a tree-sitter language file sxml can link: ${reason}`);
}

/** Reads the module's dylink.0 section. */
function readDylink(module: WebAssembly.Module): DylinkInfo {
  const sections = WebAssembly.Module.customSections(module, 'dylink.0');
  const section = sections[0];
  if (section === undefined) {
    throw refused('it has no dylink.0 section (tree-sitter build --wasm writes one)');
  }
  const bytes = new Uint8Array(section);
  let offset = 0;
  const readByte = (): number => {
    const byte = bytes[offset];
    if (byte === undefined) {
      throw refused('its dylink.0 section is cut short');
    }
    offset += 1;
    return byte;
  };
  const readUnsigned = (): number => {
    let value = 0;
    let shift = 0;
    for (;;) {
      const byte = readByte();
      value += (byte & 0x7f) * 2 ** shift;
      if ((byte & 0x80) === 0) {
        return value;
      }
      shift += 7;
      if (shift > 28) {
        throw refused('its dylink.0 section has a number past 32 bits');
      }
    }
  };
  const decoder = new TextDecoder();
  const info: DylinkInfo = {
    memorySize: 0,
    memoryAlignment: 0,
    tableSize: 0,
    tableAlignment: 0,
    neededLibraries: [],
  };
  while (offset < bytes.length) {
    const type = readByte();
    const size = readUnsigned();
    const end = offset + size;
    if (end > bytes.length) {
      throw refused('its dylink.0 section is cut short');
    }
    if (type === dylink_memory_info) {
      info.memorySize = readUnsigned();
      info.memoryAlignment = readUnsigned();
      info.tableSize = readUnsigned();
      info.tableAlignment = readUnsigned();
    } else if (type === dylink_needed) {
      const count = readUnsigned();
      for (let index = 0; index < count; index += 1) {
        const nameSize = readUnsigned();
        info.neededLibraries.push(decoder.decode(bytes.subarray(offset, offset + nameSize)));
        offset += nameSize;
      }
    }
    // Export and import information (flags of weak or TLS symbols) do
    // not concern a language module.
    offset = end;
  }
  if (info.memoryAlignment > 16 || info.tableAlignment > 16) {
    throw refused('its dylink.0 section asks for an alignment past 2^16');
  }
  return info;
}

/** The module's tree_sitter_NAME export: tree_sitter_PREFERRED when it
 *  has one, else its only one. */
function findLanguageFunction(module: WebAssembly.Module, preferred: string | undefined): string {
  const names = WebAssembly.Module.exports(module)
    .filter(({ name, kind }) => kind === 'function' && /^tree_sitter_[A-Za-z0-9_]+$/.test(name))
    .map(({ name }) => name.slice('tree_sitter_'.length));
  if (preferred !== undefined) {
    const functionName = preferred.replace(/-/g, '_');
    if (names.includes(functionName)) {
      return functionName;
    }
  }
  const [only, ...others] = names;
  if (only === undefined) {
    throw refused('it exports no function tree_sitter_NAME');
  }
  if (others.length > 0) {
    throw refused(`it exports several languages (${names.join(', ')}); name the one to load`);
  }
  return only;
}

function trap(name: string): () => never {
  return () => {
    throw new WebAssembly.RuntimeError(`sxml: the language module called ${name}`);
  };
}

/** Links module into target and returns its language. preferredName is
 *  the name the host gave the language, if any. */
export async function linkLanguageModule(
  module: WebAssembly.Module,
  target: LinkTarget,
  preferredName: string | undefined,
): Promise<LinkedLanguage> {
  const dylink = readDylink(module);
  if (dylink.neededLibraries.length > 0) {
    throw refused(`it needs other WebAssembly libraries: ${dylink.neededLibraries.join(', ')}`);
  }

  // Every import must be one sxml provides; the others are named at once.
  const unknown: string[] = [];
  const environment: Record<string, WebAssembly.ImportValue> = {};
  const baseNames: string[] = [];
  for (const { module: importModule, name, kind } of WebAssembly.Module.imports(module)) {
    let value: WebAssembly.ImportValue | undefined;
    if (importModule === 'env') {
      if (name === 'memory' && kind === 'memory') {
        value = target.memory;
      } else if (name === '__indirect_function_table' && kind === 'table') {
        value = target.table;
      } else if (name === '__stack_pointer' && kind === 'global') {
        value = target.stackPointer;
      } else if ((name === '__memory_base' || name === '__table_base') && kind === 'global') {
        // Made below, once the bases are known.
        baseNames.push(name);
        continue;
      } else if ((name === 'abort' || name === '__assert_fail') && kind === 'function') {
        value = trap(name);
      } else if (kind === 'function') {
        value = target.functions.get(name);
      }
    }
    if (value === undefined) {
      unknown.push(`${importModule}.${name}`);
    } else {
      environment[name] = value;
    }
  }
  if (unknown.length > 0) {
    throw refused(`it imports what sxml does not provide: ${unknown.join(', ')}`);
  }
  const functionName = findLanguageFunction(module, preferredName);

  // Its data in sxml's heap, zeroed (its bss is not in its data
  // segments), and its functions after the table's last slot.
  const memoryBase = target.reserve(dylink.memorySize, 2 ** dylink.memoryAlignment);
  const tableAlignment = 2 ** dylink.tableAlignment;
  const tableLength = target.table.length;
  const tableBase = Math.ceil(tableLength / tableAlignment) * tableAlignment;
  if (tableBase + dylink.tableSize > tableLength) {
    target.table.grow(tableBase + dylink.tableSize - tableLength);
  }
  for (const name of baseNames) {
    const base = name === '__memory_base' ? memoryBase : tableBase;
    environment[name] = new WebAssembly.Global({ value: 'i32', mutable: false }, base);
  }

  const instance = await WebAssembly.instantiate(module, { env: environment });
  for (const initializer of ['__wasm_apply_data_relocs', '__wasm_call_ctors', '_initialize']) {
    const run = instance.exports[initializer];
    if (typeof run === 'function') {
      run();
    }
  }
  const languageFunction = instance.exports[`tree_sitter_${functionName}`] as () => number;
  return { functionName, languageAddress: languageFunction() >>> 0 };
}
