// Tests of the JavaScript package under Node.js (ctest javascript/package,
// wasm preset). SXML_WASM_PATH names the sxml.wasm to load; ctest sets it
// to the module just built.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { env } from 'node:process';
import { describe, test } from 'node:test';
import { pathToFileURL } from 'node:url';

import { load, Processor, SxmlError, Value } from '../src/index.ts';

const wasmPath = env.SXML_WASM_PATH;
if (wasmPath === undefined) {
  throw new Error('set SXML_WASM_PATH to the sxml.wasm to test');
}
const sxml = await load({ wasm: pathToFileURL(wasmPath) });

/** Disposes processor and checks that nothing it allocated is left. */
function disposeChecked(processor: Processor): void {
  processor.dispose();
  assert.equal(processor.liveAllocationCount(), 0, 'leaked allocations');
}

/** Runs body with a fresh processor, then disposes it and checks for
 *  leaks. */
function withProcessor(body: (processor: Processor) => void): void {
  const processor = sxml.createProcessor();
  body(processor);
  disposeChecked(processor);
}

describe('the module', () => {
  test('reports the release version, that of the package', () => {
    const packageJson = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8'));
    assert.equal(sxml.version, '0.2.0');
    assert.equal(packageJson.version, sxml.version);
  });
});

// The optional features the module under test must have (D189):
// SXML_WASM_FEATURES lists them, comma-separated; none for sxml.wasm.
const expectedFeatures = (env.SXML_WASM_FEATURES ?? '').split(',').filter((name) => name !== '');

describe('optional features', () => {
  test('the module lists the features it links', () => {
    assert.deepEqual([...sxml.features], expectedFeatures);
  });

  test('a processor takes a list of the module\'s features', () => {
    for (const features of [[], expectedFeatures, undefined]) {
      const processor = sxml.createProcessor({ features });
      const result = processor.evaluate('1 + 1');
      assert.equal(result.toString(), '2');
      result.dispose();
      disposeChecked(processor);
    }
  });

  test('a feature the module does not have is refused', () => {
    assert.throws(() => sxml.createProcessor({ features: ['no-such-feature'] }), /no optional feature/);
  });
});

describe('XPath', () => {
  test('evaluates without a context', () => {
    withProcessor((processor) => {
      const result = processor.evaluate('1 to 3');
      assert.equal(result.toString(), '1\n2\n3');
      result.dispose();
    });
  });

  test('evaluates against a parsed document', () => {
    withProcessor((processor) => {
      const book = processor.parse('<book><title>XPath 4.0</title><title>XQuery</title></book>');
      const titles = processor.evaluate('//title/string()', { context: book });
      assert.equal(titles.toString(), '"XPath 4.0"\n"XQuery"');
      const count = processor.evaluate('count(//title)', { context: book });
      assert.equal(count.toString(), '2');
      for (const value of [count, titles, book]) {
        value.dispose();
      }
    });
  });

  test('binds variables from values and JSON-like data', () => {
    withProcessor((processor) => {
      const document = processor.parse('<a><b/><b/></a>');
      const result = processor.evaluate(
        'count($document//b) + $number, $text, $list?2, $record?name, empty($nothing)',
        {
          variables: {
            document,
            number: 40,
            text: 'it\'s "quoted"',
            list: [1, 'two'],
            record: { name: 'sxml' },
            nothing: null,
          },
        },
      );
      assert.equal(result.toString(), '4.2e1\n"it\'s ""quoted"""\n"two"\n"sxml"\ntrue()');
      result.dispose();
      document.dispose();
    });
  });

  test('takes variables in Q{uri}local form and namespaces', () => {
    withProcessor((processor) => {
      const document = processor.parse('<p:root xmlns:p="urn:p"><p:item>1</p:item></p:root>');
      const result = processor.evaluate('$Q{urn:v}offset + number(/q:root/q:item)', {
        context: document,
        namespaces: { q: 'urn:p' },
        variables: { 'Q{urn:v}offset': 1 },
      });
      assert.equal(result.toString(), '2.0e0');
      result.dispose();
      document.dispose();
    });
  });

  test('compiles once and evaluates many times', () => {
    withProcessor((processor) => {
      const expression = processor.compile('$n * 2', { variables: ['n'] });
      assert.deepEqual(expression.variables, ['n']);
      for (const number of [1, 2, 3]) {
        const result = expression.evaluate({ variables: { n: number } });
        assert.equal(result.toString(), `${number * 2}.0e0`);
        result.dispose();
      }
      expression.dispose();
    });
  });

  test('a variable without a value raises XPDY0002', () => {
    withProcessor((processor) => {
      const expression = processor.compile('$n', { variables: ['n'] });
      assert.throws(() => expression.evaluate(), (error: unknown) => {
        assert.ok(error instanceof SxmlError);
        assert.equal(error.code, 'err:XPDY0002');
        return true;
      });
      expression.dispose();
    });
  });

  test('reports static errors with code and location', () => {
    withProcessor((processor) => {
      assert.throws(() => processor.evaluate('1 +'), (error: unknown) => {
        assert.ok(error instanceof SxmlError);
        assert.equal(error.status, 'STATIC_ERROR');
        assert.equal(error.code, 'err:XPST0003');
        assert.equal(error.codeNamespaceUri, 'http://www.w3.org/2005/xqt-errors');
        assert.equal(error.lineNumber, 1);
        return true;
      });
    });
  });

  test('reports dynamic errors', () => {
    withProcessor((processor) => {
      assert.throws(() => processor.evaluate('1 idiv 0'), { code: 'err:FOAR0001' });
      assert.throws(() => processor.evaluate('error(xs:QName("err:XYZ"), "custom")'), {
        code: 'err:XYZ',
        description: 'custom',
      });
    });
  });
});

describe('XML parsing', () => {
  test('reports a document that is not well-formed', () => {
    withProcessor((processor) => {
      assert.throws(() => processor.parse('<a>\n<b></a>', { uri: 'bad.xml' }), (error: unknown) => {
        assert.ok(error instanceof SxmlError);
        assert.equal(error.status, 'MALFORMED_INPUT');
        assert.equal(error.lineNumber, 2);
        return true;
      });
    });
  });

  test('takes bytes in any encoding the document declares', () => {
    withProcessor((processor) => {
      const bytes = new Uint8Array([
        ...new TextEncoder().encode('<?xml version="1.0" encoding="ISO-8859-1"?><a>'),
        0xe9,
        ...new TextEncoder().encode('</a>'),
      ]);
      const document = processor.parse(bytes, { uri: 'http://example.com/a.xml' });
      const result = processor.evaluate('string(.), document-uri(.)', { context: document });
      assert.equal(result.toString(), '"é"\n"http://example.com/a.xml"');
      result.dispose();
      document.dispose();
    });
  });
});

describe('serialization', () => {
  test('writes nodes as XML by the adaptive method', () => {
    withProcessor((processor) => {
      const document = processor.parse('<a x="1"><b>text</b></a>');
      assert.equal(document.toString(), '<a x="1"><b>text</b></a>');
      document.dispose();
    });
  });

  test('takes the parameters of fn:serialize', () => {
    withProcessor((processor) => {
      const result = processor.evaluate('map { "a": [1, true()] }');
      assert.equal(result.serialize({ method: 'json' }), '{"a":[1,true]}');
      result.dispose();
      const document = processor.parse('<a><b/></a>');
      assert.equal(
        document.serialize({ method: 'xml', omitXmlDeclaration: false }),
        '<?xml version="1.0" encoding="UTF-8"?><a><b/></a>',
      );
      assert.equal(document.serialize({ method: 'text' }), '');
      document.dispose();
    });
  });

  test('reports serialization errors', () => {
    withProcessor((processor) => {
      const result = processor.evaluate('1, 2');
      assert.throws(() => result.serialize({ method: 'json' }), { code: 'err:SERE0023' });
      assert.throws(() => result.serialize({ method: 'no-such-method' }), SxmlError);
      result.dispose();
    });
  });
});

describe('the resolver', () => {
  test('serves fn:doc and fn:unparsed-text', () => {
    const requests: string[] = [];
    const processor = sxml.createProcessor({
      resolver: (request) => {
        requests.push(`${request.kind} ${request.absoluteUri}`);
        if (request.absoluteUri === 'http://example.com/data.xml') {
          return '<data><value>42</value></data>';
        }
        if (request.absoluteUri === 'http://example.com/note.txt') {
          return new TextEncoder().encode('a note');
        }
        return undefined;
      },
    });
    const result = processor.evaluate(
      'doc("data.xml")//value/string(), unparsed-text("note.txt"), doc-available("missing.xml")',
      { baseUri: 'http://example.com/' },
    );
    assert.equal(result.toString(), '"42"\n"a note"\nfalse()');
    assert.deepEqual(requests, [
      'xml-document http://example.com/data.xml',
      'text http://example.com/note.txt',
      'xml-document http://example.com/missing.xml',
    ]);
    assert.throws(() => processor.evaluate('doc("http://example.com/missing.xml")'), {
      code: 'err:FODC0002',
    });
    result.dispose();
    disposeChecked(processor);
  });

  test('an exception in the resolver is rethrown', () => {
    const problem = new Error('network down');
    const processor = sxml.createProcessor({
      resolver: () => {
        throw problem;
      },
    });
    assert.throws(() => processor.evaluate('doc("http://example.com/a.xml")'), problem);
    const result = processor.evaluate('1');
    assert.equal(result.toString(), '1');
    result.dispose();
    disposeChecked(processor);
  });

  test('the resolver may not use its processor', () => {
    let processor: Processor | undefined;
    processor = sxml.createProcessor({
      resolver: () => {
        processor?.parse('<a/>');
        return '<b/>';
      },
    });
    assert.throws(() => processor?.evaluate('doc("http://example.com/a.xml")'), /must not use/);
    processor.dispose();
  });
});

describe('memory', () => {
  test('disposing a processor releases what it made', () => {
    const processor = sxml.createProcessor();
    const document = processor.parse('<a/>');
    const expression = processor.compile('.');
    processor.dispose();
    assert.throws(() => document.toString(), /disposed/);
    assert.throws(() => expression.evaluate(), /disposed/);
    assert.throws(() => processor.parse('<a/>'), /disposed/);
    document.dispose();
    expression.dispose();
    processor.dispose();
  });

  test('values of one processor are refused by another', () => {
    const first = sxml.createProcessor();
    const second = sxml.createProcessor();
    const document = first.parse('<a/>');
    assert.throws(() => second.evaluate('.', { context: document }), /another processor/);
    first.dispose();
    second.dispose();
  });

  test('every allocation failure is reported and leaks nothing', () => {
    // As the C tests do with sxml_counting_allocator (D23): fail the first,
    // second, ... allocation until the scenario succeeds.
    let succeeded = false;
    for (let failing = 1; !succeeded; failing += 1) {
      let processor: Processor;
      try {
        processor = sxml.createProcessor({ failingAllocationNumber: failing });
      } catch (error) {
        assert.ok(error instanceof SxmlError);
        assert.equal(error.status, 'OUT_OF_MEMORY');
        continue;
      }
      const made: Value[] = [];
      try {
        const document = processor.parse('<a><b>1</b></a>');
        made.push(document);
        const result = processor.evaluate('sum(//b) + $n', {
          context: document,
          variables: { n: [2] },
        });
        made.push(result);
        assert.equal(result.serialize({ method: 'text' }), '3');
        succeeded = true;
      } catch (error) {
        assert.ok(error instanceof SxmlError, String(error));
        assert.ok(
          ['OUT_OF_MEMORY', 'LIMIT_EXCEEDED'].includes(error.status) || error.code !== '',
          error.message,
        );
      }
      for (const value of made) {
        value.dispose();
      }
      disposeChecked(processor);
      assert.ok(failing < 100_000, 'no end to the allocation failures');
    }
  });

  test('values that are never disposed are collected', async () => {
    const gc = (globalThis as { gc?: () => void }).gc;
    if (gc === undefined) {
      return;
    }
    const processor = sxml.createProcessor();
    processor.parse('<a/>').dispose();
    const baseline = processor.liveAllocationCount();
    for (let index = 0; index < 100; index += 1) {
      processor.parse('<a/>');
    }
    for (let attempt = 0; attempt < 20 && processor.liveAllocationCount() > baseline; attempt += 1) {
      gc();
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    assert.equal(processor.liveAllocationCount(), baseline);
    disposeChecked(processor);
  });
});
