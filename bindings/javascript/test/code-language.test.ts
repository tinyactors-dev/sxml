// Tests of tree-sitter's WebAssembly language files in the JavaScript
// package (ctest javascript/code_languages, wasm preset; D293).
// SXML_WASM_PATH names the sxml.wasm to load; SXML_CODE_LANGUAGE_DIRECTORY
// the directory where the build wrote the language modules: vendored
// grammars compiled as `tree-sitter build --wasm` compiles them (JSON
// without an external scanner; Python, whose scanner calls malloc,
// calloc, realloc and free; CSS, whose scanner uses the stack pointer and
// iswspace). Other modules are made here, byte by byte.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { env } from 'node:process';
import { describe, test } from 'node:test';
import { pathToFileURL } from 'node:url';

import { load, Processor, SxmlError } from '../src/index.ts';
import type { CodeLanguage } from '../src/index.ts';

const wasmPath = env.SXML_WASM_PATH;
const languageDirectory = env.SXML_CODE_LANGUAGE_DIRECTORY;
if (wasmPath === undefined || languageDirectory === undefined) {
  throw new Error('set SXML_WASM_PATH and SXML_CODE_LANGUAGE_DIRECTORY');
}
const sxml = await load({ wasm: pathToFileURL(wasmPath) });

function languageFile(name: string): Uint8Array {
  return readFileSync(`${languageDirectory}/${name}`);
}

function vendoredQuery(language: string, query: string): string {
  return readFileSync(
    new URL(`../../../third_party/tree-sitter-grammars/${language}/queries/${query}.scm`, import.meta.url),
    'utf8',
  );
}

/** The result of expression on a fresh processor with languages,
 *  serialized as XML; the processor is disposed and checked for leaks. */
function run(languages: CodeLanguage[], expression: string, code = ''): string {
  const processor: Processor = sxml.createProcessor({ codeLanguages: languages });
  const result = processor.evaluate(expression, { variables: { code } });
  const text = result.serialize({ method: 'xml' });
  result.dispose();
  processor.dispose();
  assert.equal(processor.liveAllocationCount(), 0, 'leaked allocations');
  return text;
}

// ------------------------------------------------- Hand-made side modules

function leb128(value: number): number[] {
  const bytes: number[] = [];
  let rest = value;
  do {
    let byte = rest & 0x7f;
    rest >>>= 7;
    if (rest !== 0) {
      byte |= 0x80;
    }
    bytes.push(byte);
  } while (rest !== 0);
  return bytes;
}

function wasmName(text: string): number[] {
  const bytes = [...new TextEncoder().encode(text)];
  return [...leb128(bytes.length), ...bytes];
}

function section(id: number, body: number[]): number[] {
  return [id, ...leb128(body.length), ...body];
}

/** A module with a dylink.0 section (tool conventions, "Dynamic linking":
 *  WASM_DYLINK_MEM_INFO 1, WASM_DYLINK_NEEDED 2) and, optionally, needed
 *  libraries and one imported function env.NAME of type () -> (). */
function sideModule(options: { needed?: string[]; importedFunction?: string; dylink?: boolean }): Uint8Array {
  const bytes = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];
  if (options.dylink !== false) {
    const dylink = [...wasmName('dylink.0'), 1, ...leb128(4), 0, 0, 0, 0];
    const needed = options.needed ?? [];
    if (needed.length > 0) {
      const body = [...leb128(needed.length), ...needed.flatMap(wasmName)];
      dylink.push(2, ...leb128(body.length), ...body);
    }
    bytes.push(...section(0, dylink));
  }
  if (options.importedFunction !== undefined) {
    bytes.push(...section(1, [1, 0x60, 0, 0]));
    bytes.push(...section(2, [1, ...wasmName('env'), ...wasmName(options.importedFunction), 0x00, 0]));
  }
  return new Uint8Array(bytes);
}

/** A side module whose tree_sitter_old_abi() returns the address of a
 *  TSLanguage of ABI 12, which sxml does not read: its first field is
 *  the ABI version (tree_sitter/parser.h). Its data is placed at
 *  __memory_base. */
function oldAbiModule(): Uint8Array {
  const i32 = 0x7f;
  const globalGetMemoryBase = [0x23, 0];
  return new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    ...section(0, [...wasmName('dylink.0'), 1, ...leb128(4), 64, 2, 0, 0]),
    ...section(1, [1, 0x60, 0, 1, i32]),
    ...section(2, [
      2,
      ...wasmName('env'), ...wasmName('memory'), 0x02, 0x00, 0,
      ...wasmName('env'), ...wasmName('__memory_base'), 0x03, i32, 0,
    ]),
    ...section(3, [1, 0]),
    ...section(7, [1, ...wasmName('tree_sitter_old_abi'), 0x00, 0]),
    ...section(10, [1, ...leb128(4), 0, ...globalGetMemoryBase, 0x0b]),
    ...section(11, [1, 0, ...globalGetMemoryBase, 0x0b, 4, 12, 0, 0, 0]),
  ]);
}

// ------------------------------------------------------------------ Tests

describe('tree-sitter language modules', () => {
  test('a grammar without a scanner parses with its highlights', async () => {
    const json = await sxml.loadCodeLanguage({
      wasm: languageFile('tree-sitter-json.wasm'),
      highlights: '(number) @number\n[(true) (false)] @boolean\n',
    });
    // The name of the file's tree_sitter_NAME function.
    assert.equal(json.name, 'json');
    assert.equal(json.abiVersion, 14);
    assert.equal(
      run([json], 'sxml:parse-code($code, "json")', '[1, true]'),
      '<document xmlns:sxml="urn:sxml"><array>[<number highlight="number">1</number>, ' +
        '<true highlight="boolean">true</true>]</array></document>',
    );
    // It replaces the built-in json in the processors it is given to.
    assert.equal(run([json], 'count(sxml:code-languages()[. = "json"])'), '1');
  });

  test('a name of its own, and a module given as bytes or a compiled module', async () => {
    const bytes = languageFile('tree-sitter-json.wasm');
    const named = await sxml.loadCodeLanguage({ name: 'my-json', wasm: bytes });
    const compiled = await sxml.loadCodeLanguage({
      name: 'compiled-json',
      wasm: await WebAssembly.compile(bytes),
    });
    assert.equal(named.name, 'my-json');
    assert.equal(
      run([named, compiled], 'sxml:code-languages()[ends-with(., "-json")] => string-join(",")'),
      'my-json,compiled-json',
    );
    // Without queries there are no highlights.
    assert.equal(
      run([named], 'sxml:parse-code($code, "my-json")', '[null]'),
      '<document xmlns:sxml="urn:sxml"><array>[<null>null</null>]</array></document>',
    );
  });

  // The external scanner of Python (INDENT and DEDENT tokens) allocates
  // with malloc, calloc, realloc and free, sxml.wasm's own.
  test('a grammar with an external scanner gives the built-in grammar\'s trees', async () => {
    const python = await sxml.loadCodeLanguage({
      name: 'loaded-python',
      wasm: languageFile('tree-sitter-python.wasm'),
      highlights: vendoredQuery('python', 'highlights'),
    });
    assert.equal(python.abiVersion, 15);
    const code = 'class Shape:\n    def area(self):\n        if self.big:\n            return 2\n        return 1\n\n' +
      'def main():\n    """Doc."""\n    print(f"{Shape().area()}")\n';
    const loaded = run([python], 'sxml:parse-code($code, "loaded-python")', code);
    const builtIn = run([], 'sxml:parse-code($code, "python")', code);
    assert.equal(loaded, builtIn);
    assert.equal(
      run([python], 'sxml:parse-code($code, "loaded-python")//function_definition/identifier ! string() => string-join(",")', code),
      'area,main',
    );
    assert.match(loaded, /highlight="function.method"|highlight="function"/);
  });

  // CSS's scanner keeps locals on the shadow stack (__stack_pointer) and
  // calls iswspace and iswalnum.
  test('a scanner that uses the stack pointer and the C library\'s wctype', async () => {
    const css = await sxml.loadCodeLanguage({
      name: 'loaded-css',
      wasm: languageFile('tree-sitter-css.wasm'),
      highlights: vendoredQuery('css', 'highlights'),
    });
    const code = 'a:hover, .b > c { color: #fff; margin: 0 auto !important; }\n@media (min-width: 1px) { d { e: f(1px) } }\n';
    assert.equal(
      run([css], 'sxml:parse-code($code, "loaded-css")', code),
      run([], 'sxml:parse-code($code, "css")', code),
    );
  });

  test('one language serves several processors, and the vendored queries check', async () => {
    const python = await sxml.loadCodeLanguage({
      wasm: languageFile('tree-sitter-python.wasm'),
      highlights: vendoredQuery('python', 'highlights'),
    });
    for (let round = 0; round < 3; round += 1) {
      assert.equal(run([python], 'count(sxml:parse-code("x = 1", "python")//assignment)'), '1');
    }
  });
});

describe('language modules that are refused', () => {
  const refused = (pattern: RegExp) => (error: unknown) =>
    error instanceof Error && !(error instanceof SxmlError) && pattern.test(error.message);

  test('a query that does not fit the grammar', async () => {
    await assert.rejects(
      sxml.loadCodeLanguage({ wasm: languageFile('tree-sitter-json.wasm'), highlights: '(float) @number' }),
      (error: unknown) =>
        error instanceof Error && error.message === 'sxml: the highlights query does not fit the grammar at byte 1',
    );
    await assert.rejects(
      sxml.loadCodeLanguage({ wasm: languageFile('tree-sitter-json.wasm'), injections: '(string) @' }),
      refused(/^sxml: the injections query does not fit the grammar at byte \d+$/),
    );
  });

  test('a language of an ABI sxml does not read', async () => {
    await assert.rejects(
      sxml.loadCodeLanguage({ wasm: oldAbiModule() }),
      refused(/^sxml: the grammar's ABI is 12; sxml reads 13 to 15$/),
    );
  });

  test('a name sxml:parse-code cannot take', async () => {
    await assert.rejects(
      sxml.loadCodeLanguage({ name: 'my json', wasm: languageFile('tree-sitter-json.wasm') }),
      refused(/^sxml: 'my json' is no language name/),
    );
  });

  test('a module that is not a language file', async () => {
    await assert.rejects(
      sxml.loadCodeLanguage({ wasm: sideModule({ dylink: false }) }),
      refused(/no dylink\.0 section/),
    );
    await assert.rejects(
      sxml.loadCodeLanguage({ wasm: sideModule({ needed: ['libc++.so'] }) }),
      refused(/needs other WebAssembly libraries: libc\+\+\.so/),
    );
    await assert.rejects(
      sxml.loadCodeLanguage({ wasm: sideModule({ importedFunction: 'fopen' }) }),
      refused(/imports what sxml does not provide: env\.fopen/),
    );
    await assert.rejects(
      sxml.loadCodeLanguage({ wasm: sideModule({}) }),
      refused(/exports no function tree_sitter_NAME/),
    );
  });

  test('a language of another loaded module', async () => {
    const other = await load({ wasm: pathToFileURL(wasmPath) });
    const json = await other.loadCodeLanguage({ wasm: languageFile('tree-sitter-json.wasm') });
    assert.throws(() => sxml.createProcessor({ codeLanguages: [json] }), /another sxml module/);
  });
});
