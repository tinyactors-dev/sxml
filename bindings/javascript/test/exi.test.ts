// Tests of EXI in the JavaScript package under Node.js (ctest
// javascript/exi, wasm preset; packet 8.15, D333): Processor.encodeExi and
// Processor.parseExi against the golden streams of tests/exi, which
// EXIficient wrote (tests/exi/README.md). SXML_WASM_PATH names the module.
//
// For a sample of each schema-less set of tests/exi/manifest.tsv:
// - encodeExi of the parsed input is the golden stream byte for byte
//   (compression: parseExi of it is deep-equal to the golden decoding,
//   since DEFLATE output differs between implementations);
// - parseExi of the golden stream is deep-equal to the golden decoding.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { env } from 'node:process';
import { describe, test } from 'node:test';
import { pathToFileURL } from 'node:url';

import { load, Processor, SxmlError, type ExiOptions, type Value } from '../src/index.ts';

const wasmPath = env.SXML_WASM_PATH;
if (wasmPath === undefined) {
  throw new Error('set SXML_WASM_PATH to the sxml.wasm to test');
}
const sxml = await load({ wasm: pathToFileURL(wasmPath) });
const root = new URL('../../../', import.meta.url);
const sample = 4;
const sets = [
  'bit-packed', 'bit-packed-full', 'byte-alignment-full', 'pre-compression', 'pre-compression-full',
  'pre-compression-blocks', 'compression', 'compression-full', 'compression-blocks', 'string-limits',
  'string-limits-zero', 'fragment', 'header-options', 'canonical', 'canonical-byte-alignment',
  'canonical-compression', 'self-contained', 'self-contained-byte-alignment',
];
/** The cases EXIficient decodes wrongly (tests/exi/oracle-deviations.txt). */
const deviations = new Set(
  readFileSync(new URL('tests/exi/oracle-deviations.txt', root), 'utf8')
    .split('\n')
    .filter((line) => line.trim() !== '' && !line.startsWith('#'))
    .map((line) => line.split(/\s/)[0]),
);

/** The ExiOptions of an option specification of the manifest
 *  (tests/exi/README.md, "Options"), and whether it preserves comments,
 *  processing instructions and prefixes, for deep-equal. */
function optionsOf(specification: string): ExiOptions {
  const options: ExiOptions = {};
  for (const part of specification.split(';').filter((item) => item !== '')) {
    const [key, value = ''] = part.split('=');
    switch (key) {
      case 'alignment':
        options.alignment = value as ExiOptions['alignment'];
        break;
      case 'preserve':
        options.preserve = value.split(',') as ExiOptions['preserve'];
        break;
      case 'fragment':
        options.fragment = true;
        break;
      case 'strict':
        options.strict = true;
        break;
      case 'block-size':
        options.blockSize = Number(value);
        break;
      case 'value-max-length':
        options.valueMaxLength = Number(value);
        break;
      case 'value-partition-capacity':
        options.valuePartitionCapacity = Number(value);
        break;
      case 'canonical':
        options.canonical = true;
        break;
      case 'self-contained':
        options.selfContained = true;
        options.selfContainedElements = value.split(',').map((name) => (name.startsWith('{') ? `Q${name}` : name));
        break;
      case 'header':
        options.includeCookie = value.split(',').includes('cookie');
        options.includeOptions = value.split(',').includes('options');
        break;
      default:
        throw new Error(`unknown option ${key}`);
    }
  }
  return options;
}

/** Whether two nodes are deep-equal, comparing what the options keep. */
function deepEqual(processor: Processor, first: Value, second: Value, options: ExiOptions): boolean {
  const kept = (item: string) => (options.preserve ?? []).includes(item as never) ? 'true()' : 'false()';
  const expression = processor.compile(
    `deep-equal($a, $b, {'comments': ${kept('comments')}, 'processing-instructions': ${kept('pis')}, ` +
      `'namespace-prefixes': ${kept('prefixes')}})`,
    { variables: ['a', 'b'] },
  );
  const result = expression.evaluate({ variables: { a: first, b: second } }).toString();
  expression.dispose();
  return result === 'true()';
}

/** Runs body with a fresh processor, then disposes it and checks that
 *  nothing it allocated is left. */
function withProcessor(body: (processor: Processor) => void): void {
  const processor = sxml.createProcessor();
  body(processor);
  processor.dispose();
  assert.equal(processor.liveAllocationCount(), 0, 'leaked allocations');
}

const rows = readFileSync(new URL('tests/exi/manifest.tsv', root), 'utf8')
  .split('\n')
  .filter((line) => line !== '' && !line.startsWith('#'))
  .map((line) => line.split('\t'));

for (const name of sets) {
  describe(`the golden set ${name}`, () => {
    for (const [set = '', id = '', input = '', specification = '', oracle = '', decoded = ''] of rows
      .filter((row) => row[0] === name && row[4] === 'ok' && !deviations.has(`${row[0]}/${row[1]}`))
      .slice(0, sample)) {
      void set;
      void oracle;
      const options = optionsOf(specification);
      const golden = new Uint8Array(readFileSync(new URL(`tests/exi/golden/${name}/${id}.exi`, root)));
      test(`${id}: encodeExi`, () => {
        withProcessor((processor) => {
          const document = processor.parse(readFileSync(new URL(input, root)));
          const stream = processor.encodeExi(document, options);
          assert.ok(stream instanceof Uint8Array);
          if (options.alignment === 'compression') {
            const expected = processor.parse(readFileSync(new URL(decoded, root)));
            assert.ok(deepEqual(processor, processor.parseExi(stream, options), expected, options));
          } else {
            assert.deepEqual(stream, golden);
          }
        });
      });
      test(`${id}: parseExi`, () => {
        withProcessor((processor) => {
          const expected = processor.parse(readFileSync(new URL(decoded, root)));
          assert.ok(deepEqual(processor, processor.parseExi(golden, options), expected, options));
        });
      });
    }
  });
}

describe('EXI in the package', () => {
  test('the default options are bit-packed with default fidelity', () => {
    withProcessor((processor) => {
      const document = processor.parse('<order id="A-1"><line sku="KB-104">2</line><!-- note --></order>');
      const stream = processor.encodeExi(document);
      const back = processor.parseExi(stream);
      assert.equal(back.serialize({ method: 'xml', omitXmlDeclaration: true }),
        '<order id="A-1"><line sku="KB-104">2</line></order>');
    });
  });

  test('the header can carry the cookie, which parseExi recognizes', () => {
    withProcessor((processor) => {
      const document = processor.parse('<order id="A-1"><!-- kept --></order>');
      const withCookie = processor.encodeExi(document, { includeCookie: true });
      assert.deepEqual([...withCookie.subarray(0, 4)], [0x24, 0x45, 0x58, 0x49]);
      assert.equal(processor.parseExi(withCookie).serialize({ method: 'xml', omitXmlDeclaration: true }),
        '<order id="A-1"/>');
    });
  });

  test('exi:serialize and exi:parse are bound to the prefix exi', () => {
    withProcessor((processor) => {
      const result = processor.evaluate(`exi:parse(exi:serialize(parse-xml('<a b="1">c</a>'), {}), {})/a/@b/string()`);
      assert.equal(result.toString(), '"1"');
    });
  });

  test('a stream that is not EXI is refused with MALFORMED_INPUT', () => {
    withProcessor((processor) => {
      assert.throws(() => processor.parseExi(new TextEncoder().encode('<order/>')), (error: unknown) => {
        assert.ok(error instanceof SxmlError);
        assert.equal(error.status, 'MALFORMED_INPUT');
        return true;
      });
    });
  });

  test('options are checked: an unknown alignment and a negative block size throw', () => {
    withProcessor((processor) => {
      const document = processor.parse('<order/>');
      assert.throws(() => processor.encodeExi(document, { alignment: 'sideways' as never }), TypeError);
      assert.throws(() => processor.encodeExi(document, { blockSize: -1 }), RangeError);
    });
  });

  test('encodeExi takes a document or an element node, nothing else', () => {
    withProcessor((processor) => {
      const element = processor.evaluate(`parse-xml('<order id="A-1"/>')/*`);
      const back = processor.parseExi(processor.encodeExi(element));
      assert.equal(back.serialize({ method: 'xml', omitXmlDeclaration: true }), '<order id="A-1"/>');
      assert.throws(() => processor.encodeExi(processor.evaluate('1, 2')), SxmlError);
    });
  });
});
