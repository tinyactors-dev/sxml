// Tests of streamed output and input in the JavaScript package (D294):
// output sinks, the adapters, chunk sources, memory bounds on large
// outputs, and the step limit (D295; ctest javascript/streams, wasm
// preset). SXML_WASM_PATH names the sxml.wasm to load.
import assert from 'node:assert/strict';
import { env } from 'node:process';
import { Writable } from 'node:stream';
import { describe, test } from 'node:test';
import { pathToFileURL } from 'node:url';

import {
  load,
  nodeStreamSink,
  outputReadableStream,
  Processor,
  type ProcessorOptions,
  type PutDocument,
  textSink,
  writableStreamSink,
} from '../src/index.ts';

const wasmPath = env.SXML_WASM_PATH;
if (wasmPath === undefined) {
  throw new Error('set SXML_WASM_PATH to the sxml.wasm to test');
}
const wasmUrl = pathToFileURL(wasmPath);
const sxml = await load({ wasm: wasmUrl });

function withProcessor(body: (processor: Processor) => void, options: ProcessorOptions = {}): void {
  const processor = sxml.createProcessor(options);
  body(processor);
  processor.dispose();
  assert.equal(processor.liveAllocationCount(), 0, 'leaked allocations');
}

/** A sink that keeps its chunks. */
function collector(): { chunks: Uint8Array[]; sink: (chunk: Uint8Array) => void; text(): string } {
  const chunks: Uint8Array[] = [];
  return {
    chunks,
    sink: (chunk) => chunks.push(chunk),
    text: () => Buffer.concat(chunks).toString('utf8'),
  };
}

const document = '<doc>héllo wörld € 😀 <b n="1">text</b></doc>';

describe('output sinks', () => {
  test('serializeTo writes what serialize returns, a chunk at a time', () => {
    withProcessor(
      (processor) => {
        const value = processor.parse(document);
        const expected = value.serialize({ method: 'xml', omitXmlDeclaration: true });
        const output = collector();
        value.serializeTo(output.sink, { method: 'xml', omitXmlDeclaration: true });
        assert.equal(output.text(), expected);
        assert.ok(output.chunks.length > 1);
        assert.ok(output.chunks.every((chunk) => chunk.byteLength <= 7));
        // The adaptive method without options.
        const adaptive = collector();
        processor.serializeTo(value, adaptive.sink);
        assert.equal(adaptive.text(), value.toString());
        value.dispose();
      },
      { outputChunkByteCount: 7 },
    );
  });

  test('each chunk is a copy the sink may keep', () => {
    withProcessor(
      (processor) => {
        const value = processor.evaluate('string-join(1 to 2000, ",")');
        const output = collector();
        value.serializeTo(output.sink);
        assert.equal(output.text(), value.toString());
        assert.ok(output.chunks.every((chunk) => chunk.buffer !== sxml.exports.memory.buffer));
        value.dispose();
      },
      { outputChunkByteCount: 256 },
    );
  });

  test('characters split across chunks decode whole in a text sink', () => {
    for (const chunkByteCount of [1, 2, 3, 4]) {
      withProcessor(
        (processor) => {
          const value = processor.evaluate('"é€😀" || "a€"');
          const raw = collector();
          value.serializeTo(raw.sink, { method: 'text' });
          // Some chunk ends inside a character: decoding chunks one by
          // one does not give the text.
          assert.ok(raw.chunks.some((chunk) => {
            try {
              new TextDecoder('utf-8', { fatal: true }).decode(chunk);
              return false;
            } catch {
              return true;
            }
          }));
          const pieces: string[] = [];
          value.serializeTo(textSink((text) => pieces.push(text)), { method: 'text' });
          assert.equal(pieces.join(''), 'é€😀a€');
          value.dispose();
        },
        { outputChunkByteCount: chunkByteCount },
      );
    }
  });

  test('an XQuery writes its result with its output declarations', () => {
    withProcessor(
      (processor) => {
        const query = processor.compileQuery(
          'declare option output:method "xml"; declare option output:indent "yes"; <a>{(1 to 50) ! <b>{.}</b>}</a>',
        );
        const expected = query.run();
        const output = collector();
        query.runTo(output.sink);
        assert.equal(output.text(), expected);
        const value = query.evaluate();
        const again = collector();
        query.serializeTo(value, again.sink);
        assert.equal(again.text(), expected);
        value.dispose();
        query.dispose();
      },
      { outputChunkByteCount: 16 },
    );
  });

  test('a document fn:put stores is written to a sink', () => {
    withProcessor((processor) => {
      const query = processor.compileQuery('put(<a>€</a>, "file:///out/a.xml")');
      let put: PutDocument | undefined;
      query.evaluate({ updates: { putDocument: (document) => (put = document) } }).dispose();
      assert.ok(put !== undefined);
      const output = collector();
      put.serializeTo(output.sink);
      assert.equal(output.text(), put.serialize());
      put.node.dispose();
      put.parameters?.dispose();
      query.dispose();
    });
  });

  test('an object sink is closed after the output, and aborted on failure', () => {
    withProcessor((processor) => {
      const events: string[] = [];
      const value = processor.evaluate('map { "a": 1 }');
      value.serializeTo({ write: () => events.push('write'), close: () => events.push('close') }, { method: 'json' });
      assert.deepEqual(events.slice(-1), ['close']);
      events.length = 0;
      let reason: unknown;
      assert.throws(
        () =>
          value.serializeTo(
            { write: () => events.push('write'), abort: (error) => (reason = error) },
            { method: 'xml' },
          ),
        /SENR0001/,
      );
      assert.match(String(reason), /SENR0001/);
      value.dispose();
    });
  });

  test('an exception a sink throws stops the call and is rethrown', () => {
    withProcessor(
      (processor) => {
        const value = processor.evaluate('string-join(1 to 1000)');
        const failure = new Error('disk full');
        let count = 0;
        assert.throws(
          () =>
            value.serializeTo(() => {
              count += 1;
              if (count === 2) {
                throw failure;
              }
            }),
          (error) => error === failure,
        );
        assert.equal(count, 2);
        // The processor works after it.
        assert.equal(value.toString().length > 0, true);
        value.dispose();
      },
      { outputChunkByteCount: 64 },
    );
  });

  test('a sink must not use its processor', () => {
    withProcessor((processor) => {
      const value = processor.evaluate('"x"');
      assert.throws(() => value.serializeTo(() => processor.evaluate('1')), /must not use its processor/);
      value.dispose();
    });
  });
});

describe('transformations', () => {
  const stylesheet = `<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
    <xsl:output omit-xml-declaration="yes"/>
    <xsl:template name="xsl:initial-template">
      <main>
        <xsl:for-each select="1 to 3">
          <xsl:result-document href="part{.}.xml" omit-xml-declaration="yes"><part n="{.}">€</part></xsl:result-document>
        </xsl:for-each>
        <xsl:if test="$fail">
          <xsl:result-document href="bad.xml" method="xml" build-tree="no"><xsl:sequence select="map { 'a': 1 }"/></xsl:result-document>
        </xsl:if>
      </main>
    </xsl:template>
    <xsl:param name="fail" select="false()"/>
  </xsl:stylesheet>`;

  test('the principal result and the result documents go to their sinks', () => {
    withProcessor(
      (processor) => {
        const compiled = processor.compileStylesheet(stylesheet, { baseUri: 'file:///out/style.xsl' });
        const principal = collector();
        const documents = new Map<string, ReturnType<typeof collector>>();
        const closed: string[] = [];
        const result = compiled.transform({
          baseOutputUri: 'file:///out/main.xml',
          output: principal.sink,
          resultDocumentOutput: (uri) => {
            const output = collector();
            documents.set(uri, output);
            return { write: output.sink, close: () => closed.push(uri) };
          },
        });
        assert.equal(result.output, undefined);
        assert.equal(principal.text(), '<main/>');
        assert.deepEqual([...documents.keys()], ['file:///out/part1.xml', 'file:///out/part2.xml', 'file:///out/part3.xml']);
        assert.equal(documents.get('file:///out/part2.xml')?.text(), '<part n="2">€</part>');
        assert.deepEqual(closed, [...documents.keys()]);
        assert.deepEqual(result.resultDocuments, []);
        compiled.dispose();
      },
      { outputChunkByteCount: 4 },
    );
  });

  test('result documents without a sink are gathered as before', () => {
    withProcessor((processor) => {
      const compiled = processor.compileStylesheet(stylesheet);
      const principal = collector();
      const result = compiled.transform({
        baseOutputUri: 'file:///out/main.xml',
        output: principal.sink,
        resultDocumentOutput: (uri) => (uri.endsWith('part2.xml') ? () => undefined : undefined),
      });
      assert.deepEqual(
        result.resultDocuments.map((document) => [document.uri, document.output]),
        [
          ['file:///out/part1.xml', '<part n="1">€</part>'],
          ['file:///out/part3.xml', '<part n="3">€</part>'],
        ],
      );
      // Without a principal sink, the principal result is the output.
      const other = compiled.transform({ baseOutputUri: 'file:///out/main.xml', resultDocumentOutput: () => undefined });
      assert.equal(other.output, '<main/>');
      assert.equal(other.resultDocuments.length, 3);
      compiled.dispose();
    });
  });

  test('a failed transformation aborts the open document and the principal sink', () => {
    withProcessor((processor) => {
      const compiled = processor.compileStylesheet(stylesheet);
      const aborted: string[] = [];
      const closed: string[] = [];
      assert.throws(
        () =>
          compiled.transform({
            baseOutputUri: 'file:///out/main.xml',
            parameters: { fail: true },
            output: { write: () => undefined, abort: () => aborted.push('principal') },
            resultDocumentOutput: (uri) => ({
              write: () => undefined,
              close: () => closed.push(uri),
              abort: () => aborted.push(uri),
            }),
          }),
        /SENR0001/,
      );
      assert.equal(closed.length, 3);
      assert.deepEqual(aborted, ['file:///out/bad.xml', 'principal']);
      compiled.dispose();
    });
  });
});

describe('adapters', () => {
  test('a WritableStream receives the chunks and is closed', async () => {
    const received: Uint8Array[] = [];
    const stream = new WritableStream<Uint8Array>({ write: (chunk) => void received.push(chunk) });
    const sink = writableStreamSink(stream);
    withProcessor(
      (processor) => {
        const value = processor.parse(document);
        value.serializeTo(sink, { method: 'xml', omitXmlDeclaration: true });
        value.dispose();
      },
      { outputChunkByteCount: 10 },
    );
    await sink.done;
    assert.equal(Buffer.concat(received).toString('utf8'), document);
  });

  test('a ReadableStream of the output', async () => {
    const processor = sxml.createProcessor({ outputChunkByteCount: 10 });
    const value = processor.parse(document);
    const stream = outputReadableStream((sink) => value.serializeTo(sink, { method: 'xml', omitXmlDeclaration: true }));
    const text = await new Response(stream).text();
    assert.equal(text, document);
    value.dispose();
    processor.dispose();
  });

  test('a Node.js Writable receives the chunks and is ended', async () => {
    const received: Buffer[] = [];
    const writable = new Writable({
      write(chunk: Buffer, _encoding, callback): void {
        received.push(chunk);
        callback();
      },
    });
    const finished = new Promise((resolve) => writable.on('finish', resolve));
    withProcessor((processor) => {
      const value = processor.parse(document);
      value.serializeTo(nodeStreamSink(writable), { method: 'xml', omitXmlDeclaration: true });
      value.dispose();
    });
    await finished;
    assert.equal(Buffer.concat(received).toString('utf8'), document);
  });
});

/** A generator of a document of count records, a few records per chunk. */
function* records(count: number, state: { closed: boolean } = { closed: false }): Generator<Uint8Array | string> {
  try {
    yield '<records>';
    for (let start = 0; start < count; start += 500) {
      let text = '';
      for (let index = start; index < Math.min(count, start + 500); index += 1) {
        text += `<record id="${index}">é</record>`;
      }
      yield new TextEncoder().encode(text);
    }
    yield '</records>';
  } finally {
    state.closed = true;
  }
}

describe('chunk sources', () => {
  test('fn:doc reads a document a resolver gives as chunks', () => {
    const state = { closed: false };
    withProcessor(
      (processor) => {
        const value = processor.evaluate('count(doc("urn:records")//record), string(doc("urn:records")//record[last()])');
        assert.equal(value.toString(), '1000\n"é"');
        value.dispose();
      },
      { resolver: (request) => (request.absoluteUri === 'urn:records' ? records(1000, state) : undefined) },
    );
    assert.equal(state.closed, true);
  });

  test('other kinds read the chunks whole', () => {
    withProcessor(
      (processor) => {
        const value = processor.evaluate('unparsed-text("urn:text")');
        assert.equal(value.toString(), '"a€b"');
        value.dispose();
      },
      { resolver: () => ['a', new TextEncoder().encode('€'), 'b'] },
    );
  });

  test('an exception the chunk source throws is rethrown', () => {
    const failure = new Error('read failed');
    withProcessor(
      (processor) => {
        assert.throws(() => processor.evaluate('doc("urn:x")'), (error) => error === failure);
      },
      {
        resolver: function* () {
          yield '<a>';
          throw failure;
        },
      },
    );
  });

  test('a streamed principal source is read as it is parsed', () => {
    const state = { closed: false };
    withProcessor(
      (processor) => {
        const compiled = processor.compileStylesheet(`<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
          <xsl:mode streamable="yes"/>
          <xsl:output method="text"/>
          <xsl:template match="/"><xsl:value-of select="count(records/record)"/></xsl:template>
        </xsl:stylesheet>`);
        const result = compiled.transform({ streamedSource: 'urn:records' });
        assert.equal(result.output, '3000');
        compiled.dispose();
      },
      { resolver: (request) => (request.absoluteUri === 'urn:records' ? records(3000, state) : undefined) },
    );
    assert.equal(state.closed, true);
  });
});

describe('memory bounds', () => {
  // A module of its own, since memory never shrinks: what it grows to is
  // what these runs needed.
  test('a 100 MB output of a streamed source stays in a small module memory', async () => {
    const module = await load({ wasm: wasmUrl, collationData: false });
    const recordCount = 100_000;
    const processor = module.createProcessor({
      resolver: (request) => (request.absoluteUri === 'urn:records' ? records(recordCount) : undefined),
    });
    // Each record writes a line of 1,004 bytes: 100.4 MB from a 2.6 MB
    // source, serialized as it is made (D231).
    const compiled = processor.compileStylesheet(`<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
      <xsl:mode streamable="yes"/>
      <xsl:output omit-xml-declaration="yes"/>
      <xsl:variable name="padding" select="string-join((1 to 110) ! 'é-paddin')"/>
      <xsl:template match="/">
        <xsl:for-each select="records/record"><line><xsl:value-of select="$padding"/></line><xsl:text>&#10;</xsl:text></xsl:for-each>
      </xsl:template>
    </xsl:stylesheet>`);
    globalThis.gc?.();
    const heapBefore = process.memoryUsage().heapUsed;
    let byteCount = 0;
    let largestHeap = 0;
    const decoder = new TextDecoder();
    let lastText = '';
    compiled.transform({
      streamedSource: 'urn:records',
      output: (chunk) => {
        byteCount += chunk.byteLength;
        lastText = decoder.decode(chunk, { stream: true });
        if (byteCount % (64 * 1024 * 64) === 0) {
          largestHeap = Math.max(largestHeap, process.memoryUsage().heapUsed);
        }
      },
    });
    assert.equal(byteCount, recordCount * 1004);
    assert.ok(lastText.endsWith('paddin</line>\n'));
    const moduleMegabytes = module.exports.memory.buffer.byteLength / 1e6;
    assert.ok(moduleMegabytes < 64, `module memory ${moduleMegabytes} MB`);
    assert.ok(largestHeap - heapBefore < 32e6, `JavaScript heap grew ${(largestHeap - heapBefore) / 1e6} MB`);
    compiled.dispose();
    processor.dispose();
    assert.equal(processor.liveAllocationCount(), 0);
  });
});

describe('the step limit', () => {
  test('maximumEvaluationStepCount bounds an evaluation', () => {
    const query = 'count((1 to 1000000)[. mod 7 = 0])';
    withProcessor((processor) => {
      const value = processor.evaluate(query);
      assert.equal(value.toString(), '142857');
      value.dispose();
    });
    withProcessor(
      (processor) => {
        assert.throws(() => processor.evaluate(query), /maximum_evaluation_step_count/);
      },
      { maximumEvaluationStepCount: 10_000 },
    );
    for (const maximumEvaluationStepCount of [-1, 1.5, 2 ** 53]) {
      assert.throws(() => sxml.createProcessor({ maximumEvaluationStepCount }), RangeError);
    }
  });

  test('each element of a streamed source grants steps (D295)', () => {
    const recordCount = 3000;
    withProcessor(
      (processor) => {
        // The streamed identity takes some 50 steps an element, more than
        // the bound of 20,000; each element read grants 500.
        const compiled = processor.compileStylesheet(`<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
          <xsl:mode streamable="yes" on-no-match="shallow-copy"/>
          <xsl:output omit-xml-declaration="yes"/>
        </xsl:stylesheet>`);
        const output = collector();
        compiled.transform({ streamedSource: 'urn:records', output: output.sink });
        assert.ok(output.text().endsWith(`<record id="${recordCount - 1}">é</record></records>`));
        compiled.dispose();
      },
      {
        maximumEvaluationStepCount: 20_000,
        resolver: (request) => (request.absoluteUri === 'urn:records' ? records(recordCount) : undefined),
      },
    );
  });
});
