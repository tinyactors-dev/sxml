// Tests of XQuery, XSLT and XSD validation in the JavaScript package under
// Node.js (ctest javascript/languages and javascript/languages_full, wasm
// preset). SXML_WASM_PATH names the module to load: on sxml-full.wasm
// every test runs; on sxml.wasm, the core module, which links none of the
// three (D189), only the test that they are refused.
import assert from 'node:assert/strict';
import { env } from 'node:process';
import { describe, test } from 'node:test';
import { pathToFileURL } from 'node:url';

import {
  load,
  Processor,
  SchemaError,
  SxmlError,
  Value,
  type Message,
  type PutDocument,
  type UpdatedTree,
} from '../src/index.ts';

const wasmPath = env.SXML_WASM_PATH;
if (wasmPath === undefined) {
  throw new Error('set SXML_WASM_PATH to the sxml.wasm or sxml-full.wasm to test');
}
const sxml = await load({ wasm: pathToFileURL(wasmPath) });
const isFull = sxml.features.length > 0;

/** Runs body with a fresh processor, then disposes it and checks that
 *  nothing it allocated is left. */
function withProcessor(body: (processor: Processor) => void, options = {}): void {
  const processor = sxml.createProcessor(options);
  body(processor);
  processor.dispose();
  assert.equal(processor.liveAllocationCount(), 0, 'leaked allocations');
}

describe('the core module', { skip: isFull }, () => {
  test('refuses XQuery, XSLT and XSD: they are in sxml-full.wasm', () => {
    withProcessor((processor) => {
      assert.throws(() => processor.compileQuery('1'), /XQuery needs the full module/);
      assert.throws(() => processor.compileStylesheet('<x/>'), /XSLT needs the full module/);
      assert.throws(() => processor.compileSchema('<x/>'), /XSD validation needs the full module/);
    });
  });
});

describe('XQuery', { skip: !isFull }, () => {
  test('compiles once and evaluates many times with external variables', () => {
    withProcessor((processor) => {
      const query = processor.compileQuery(
        'declare variable $n external := 10; declare variable $who external; $who || ": " || $n * 2',
      );
      assert.equal(query.isUpdating, false);
      assert.equal(query.evaluate({ variables: { who: 'a', n: 1 } }).toString(), '"a: 2"');
      assert.equal(query.evaluate({ variables: { who: 'b' } }).toString(), '"b: 20"');
      // A name the query does not declare is ignored (XPath 4.0's dynamic
      // context rule); a required one without a value raises XPDY0002.
      assert.equal(query.evaluate({ variables: { who: 'c', other: 3 } }).toString(), '"c: 20"');
      assert.throws(() => query.evaluate(), (error: unknown) => {
        assert.ok(error instanceof SxmlError);
        assert.equal(error.code, 'err:XPDY0002');
        return true;
      });
      query.dispose();
    });
  });

  test('evaluates against a context value, with namespaces and Q{uri}local names', () => {
    withProcessor((processor) => {
      const document = processor.parse('<p:list xmlns:p="urn:p"><p:i>1</p:i><p:i>2</p:i></p:list>');
      const query = processor.compileQuery(
        'declare variable $Q{urn:v}factor external; sum(//p:i) * $Q{urn:v}factor',
        { namespaces: { p: 'urn:p' } },
      );
      const result = query.evaluate({ context: document, variables: { 'Q{urn:v}factor': 10 } });
      assert.equal(result.toString(), '3.0e1');
    });
  });

  test('serializes with the output declarations of the prolog', () => {
    withProcessor((processor) => {
      const query = processor.compileQuery(
        'declare option output:method "json"; declare option output:indent "no"; map { "a": [1, 2] }',
      );
      assert.deepEqual(JSON.parse(query.run()), { a: [1, 2] });
      const xml = processor.compileQuery('<r>{1 + 1}</r>');
      // XQuery 4.0, 5.22.1: method xml, omit-xml-declaration yes.
      assert.equal(xml.run(), '<r>2</r>');
      const value = xml.evaluate();
      assert.equal(
        value.serialize({ method: 'xml', omitXmlDeclaration: false }),
        '<?xml version="1.0" encoding="UTF-8"?><r>2</r>',
      );
    });
  });

  test('reports static errors with code, location and source', () => {
    withProcessor((processor) => {
      assert.throws(
        () => processor.compileQuery('declare variable $x := 1;\n$x +'),
        (error: unknown) => {
          assert.ok(error instanceof SxmlError);
          assert.equal(error.status, 'STATIC_ERROR');
          assert.equal(error.code, 'err:XPST0003');
          assert.equal(error.sourceName, 'query');
          assert.equal(error.lineNumber, 2);
          return true;
        },
      );
      assert.throws(() => processor.compileQuery('$undeclared'), /XPST0008/);
    });
  });

  test('reports dynamic errors', () => {
    withProcessor((processor) => {
      const query = processor.compileQuery('error(xs:QName("err:FOER0000"), "stop")');
      assert.throws(() => query.evaluate(), (error: unknown) => {
        assert.ok(error instanceof SxmlError);
        assert.equal(error.status, 'DYNAMIC_ERROR');
        assert.equal(error.code, 'err:FOER0000');
        assert.equal(error.description, 'stop');
        return true;
      });
    });
  });

  test('imports library modules through the resolver', () => {
    const requests: string[] = [];
    withProcessor(
      (processor) => {
        const query = processor.compileQuery(
          'import module namespace m = "urn:m" at "m.xq"; m:twice(21)',
          { baseUri: 'https://example.com/q/' },
        );
        assert.equal(query.evaluate().toString(), '42');
        assert.deepEqual(requests, ['xquery-module https://example.com/q/m.xq']);
      },
      {
        resolver: ({ kind, absoluteUri }: { kind: string; absoluteUri: string }) => {
          requests.push(`${kind} ${absoluteUri}`);
          return 'module namespace m = "urn:m"; declare function m:twice($x) { $x * 2 };';
        },
      },
    );
  });

  test('hands the trees an updating query changed to the update sink', () => {
    withProcessor((processor) => {
      const document = processor.parse('<a><b>1</b></a>', { uri: 'urn:doc' });
      const query = processor.compileQuery(
        'replace value of node //b with "2", insert node <c/> into /a',
      );
      assert.equal(query.isUpdating, true);
      const trees: UpdatedTree[] = [];
      const result = query.evaluate({ context: document, updates: { updatedTree: (tree) => trees.push(tree) } });
      // XQUF 3.0, 2.2.3: an updating query's value is the empty sequence.
      assert.equal(result.toString(), '');
      assert.equal(trees.length, 1);
      assert.equal(trees[0]?.original.toString(), '<a><b>1</b></a>');
      assert.equal(trees[0]?.updated.toString(), '<a><b>2</b><c/></a>');
      // Trees never change: the document is as it was.
      assert.equal(document.toString(), '<a><b>1</b></a>');
      // Without a sink the trees are dropped, and nothing leaks.
      query.evaluate({ context: document }).dispose();
    });
  });

  test('hands fn:put documents to the update sink, or raises FOUP0002 without', () => {
    withProcessor((processor) => {
      const query = processor.compileQuery(
        'put(<doc>{1 + 1}</doc>, "out.xml", map { "indent": true() })',
        { baseUri: 'file:///data/' },
      );
      const documents: PutDocument[] = [];
      query.evaluate({ updates: { putDocument: (document) => documents.push(document) } });
      assert.equal(documents.length, 1);
      assert.equal(documents[0]?.uri, 'file:///data/out.xml');
      assert.equal(documents[0]?.node.toString(), '<doc>2</doc>');
      assert.ok(documents[0]?.parameters instanceof Value);
      assert.equal(documents[0]?.serialize(), '<doc>2</doc>');
      assert.throws(() => query.evaluate(), /FOUP0002/);
    });
  });

  test('delivers fn:trace and fn:message output', () => {
    withProcessor((processor) => {
      const messages: Message[] = [];
      const query = processor.compileQuery('trace(1 + 1, "sum"), message("note")');
      query.evaluate({ messages: (message) => messages.push(message) });
      assert.deepEqual(
        messages.map(({ text, code }) => [text, code]),
        [
          ['2', 'sum'],
          ['"note"', ''],
        ],
      );
    });
  });
});

const identity = `<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="xml" omit-xml-declaration="yes"/>
  <xsl:param name="greeting" select="'Hello'"/>
  <xsl:template match="/">
    <out><xsl:value-of select="$greeting, //name"/></out>
  </xsl:template>
  <xsl:template match="name" mode="upper">
    <name><xsl:value-of select="upper-case(.)"/></name>
  </xsl:template>
  <xsl:template name="xsl:initial-template">
    <initial/>
  </xsl:template>
  <xsl:template name="named">
    <xsl:param name="n" as="xs:integer"/>
    <named n="{$n}"/>
  </xsl:template>
  <xsl:function name="f:double" xmlns:f="urn:f" as="xs:integer" visibility="public">
    <xsl:param name="x" as="xs:integer"/>
    <xsl:sequence select="$x * 2"/>
  </xsl:function>
</xsl:stylesheet>`;

describe('XSLT', { skip: !isFull }, () => {
  test('compiles once and transforms many times with stylesheet parameters', () => {
    withProcessor((processor) => {
      const stylesheet = processor.compileStylesheet(identity);
      const source = processor.parse('<person><name>Ada</name></person>');
      assert.deepEqual(stylesheet.transform({ source }), {
        output: '<out>Hello Ada</out>',
        value: undefined,
        resultDocuments: [],
      });
      assert.equal(stylesheet.transform({ source, parameters: { greeting: 'Hi' } }).output, '<out>Hi Ada</out>');
      stylesheet.dispose();
    });
  });

  test('invokes an initial mode, a named template or a function', () => {
    withProcessor((processor) => {
      const stylesheet = processor.compileStylesheet(identity);
      const source = processor.parse('<person><name>Ada</name></person>');
      assert.equal(stylesheet.transform({ source, initialMode: 'upper' }).output, '<name>ADA</name>');
      assert.equal(stylesheet.transform().output, '<initial/>');
      assert.equal(
        stylesheet.transform({ initialTemplate: 'named', templateParameters: { n: processor.evaluate('3') } })
          .output,
        '<named n="3"/>',
      );
      const result = stylesheet.transform({
        initialFunction: 'Q{urn:f}double',
        args: [processor.evaluate('21')],
        delivery: 'raw',
      });
      assert.equal(result.output, undefined);
      assert.equal(result.value?.toString(), '42');
    });
  });

  test('delivers the principal result raw, as a tree, or with other parameters', () => {
    withProcessor((processor) => {
      const stylesheet = processor.compileStylesheet(identity);
      const source = processor.parse('<person><name>Ada</name></person>');
      const tree = stylesheet.transform({ source, delivery: 'tree' }).value;
      assert.equal(processor.evaluate('count(/out)', { context: tree }).toString(), '1');
      const json = stylesheet.transform({
        source,
        initialFunction: 'Q{urn:f}double',
        args: [processor.evaluate('4')],
        serialization: { method: 'json' },
      });
      assert.equal(json.output, '8');
      const text = stylesheet.transform({ source, serialization: { method: 'text' } });
      assert.equal(text.output, 'Hello Ada');
    });
  });

  test('takes static parameters at compilation', () => {
    withProcessor((processor) => {
      const stylesheet = processor.compileStylesheet(
        `<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
          <xsl:param name="mode" static="yes" select="'a'"/>
          <xsl:template name="xsl:initial-template">
            <xsl:value-of select="'A'" use-when="$mode = 'a'"/>
            <xsl:value-of select="'B'" use-when="$mode = 'b'"/>
          </xsl:template>
        </xsl:stylesheet>`,
        { staticParameters: { mode: 'b' } },
      );
      assert.equal(stylesheet.transform({ serialization: { method: 'text' } }).output, 'B');
    });
  });

  test('collects result documents, serialized or as values', () => {
    withProcessor((processor) => {
      const stylesheet = processor.compileStylesheet(
        `<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
          <xsl:template name="xsl:initial-template">
            <xsl:for-each select="1 to 2">
              <xsl:result-document href="part{.}.xml" omit-xml-declaration="yes"><part n="{.}"/></xsl:result-document>
            </xsl:for-each>
            <main/>
          </xsl:template>
        </xsl:stylesheet>`,
      );
      const serialized = stylesheet.transform({
        baseOutputUri: 'https://example.com/out/',
        serialization: { method: 'xml', omitXmlDeclaration: true },
      });
      assert.equal(serialized.output, '<main/>');
      assert.deepEqual(serialized.resultDocuments, [
        { uri: 'https://example.com/out/part1.xml', output: '<part n="1"/>', value: undefined },
        { uri: 'https://example.com/out/part2.xml', output: '<part n="2"/>', value: undefined },
      ]);
      const trees = stylesheet.transform({ baseOutputUri: 'urn:out/', resultDocumentDelivery: 'tree' });
      assert.equal(trees.resultDocuments.length, 2);
      assert.equal(trees.resultDocuments[1]?.output, undefined);
      assert.equal(trees.resultDocuments[1]?.value?.toString(), '<part n="2"/>');
    });
  });

  test('delivers xsl:message output, also when it terminates', () => {
    withProcessor((processor) => {
      const stylesheet = processor.compileStylesheet(
        `<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
          <xsl:template name="xsl:initial-template">
            <xsl:message>starting</xsl:message>
            <xsl:message terminate="{$stop}">stopping</xsl:message>
            <done/>
          </xsl:template>
          <xsl:param name="stop" select="'no'"/>
        </xsl:stylesheet>`,
      );
      const messages: Message[] = [];
      stylesheet.transform({ messages: (message) => messages.push(message) });
      assert.deepEqual(
        messages.map(({ text, isTerminating }) => [text, isTerminating]),
        [
          ['starting', false],
          ['stopping', false],
        ],
      );
      messages.length = 0;
      assert.throws(
        () => stylesheet.transform({ parameters: { stop: 'yes' }, messages: (message) => messages.push(message) }),
        /XTMM9000/,
      );
      assert.deepEqual(
        messages.map(({ text, code, isTerminating }) => [text, code, isTerminating]),
        [
          ['starting', 'XTMM9000', false],
          ['stopping', 'XTMM9000', true],
        ],
      );
    });
  });

  test('reads included modules through the resolver', () => {
    withProcessor(
      (processor) => {
        const stylesheet = processor.compileStylesheet(
          `<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
            <xsl:include href="lib.xsl"/>
            <xsl:template name="xsl:initial-template"><xsl:call-template name="from-lib"/></xsl:template>
          </xsl:stylesheet>`,
          { baseUri: 'https://example.com/main.xsl' },
        );
        assert.equal(stylesheet.transform({ serialization: { method: 'text' } }).output, 'included');
      },
      {
        resolver: ({ kind, absoluteUri }: { kind: string; absoluteUri: string }) =>
          kind === 'xslt-module' && absoluteUri === 'https://example.com/lib.xsl'
            ? `<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
                <xsl:template name="from-lib">included</xsl:template>
              </xsl:stylesheet>`
            : undefined,
      },
    );
  });

  test('reports static and dynamic errors with codes and locations', () => {
    withProcessor((processor) => {
      assert.throws(
        () =>
          processor.compileStylesheet(
            '<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">\n<xsl:bogus/>\n</xsl:stylesheet>',
            { baseUri: 'urn:bad.xsl' },
          ),
        (error: unknown) => {
          assert.ok(error instanceof SxmlError);
          assert.equal(error.status, 'STATIC_ERROR');
          assert.match(error.code, /^err:XTSE/);
          assert.equal(error.sourceName, 'urn:bad.xsl');
          assert.equal(error.lineNumber, 2);
          return true;
        },
      );
      const stylesheet = processor.compileStylesheet(identity);
      assert.throws(() => stylesheet.transform({ initialTemplate: 'missing' }), /XTDE0040/);
      assert.throws(() => stylesheet.transform({ initialTemplate: 'named' }), /XTDE0700/);
    });
  });
});

const purchaseOrder = `<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:element name="order">
    <xs:complexType>
      <xs:sequence>
        <xs:element name="item" maxOccurs="unbounded">
          <xs:complexType>
            <xs:attribute name="quantity" type="xs:positiveInteger" use="required"/>
          </xs:complexType>
        </xs:element>
      </xs:sequence>
    </xs:complexType>
  </xs:element>
  <xs:complexType name="loose"><xs:sequence><xs:any processContents="skip" maxOccurs="unbounded"/></xs:sequence></xs:complexType>
</xs:schema>`;

describe('XSD validation', { skip: !isFull }, () => {
  test('validates documents and returns the typed copy', () => {
    withProcessor((processor) => {
      const schema = processor.compileSchema({ text: purchaseOrder, uri: 'urn:po.xsd' });
      const result = schema.validate('<order><item quantity="2"/><item quantity="3"/></order>');
      assert.equal(result.validity, 'valid');
      assert.deepEqual(result.reports, []);
      // The copy is annotated (XDM 4.0, 3.3): quantities are integers.
      const total = processor.evaluate('sum(//@quantity) instance of xs:integer', {
        context: result.document,
      });
      assert.equal(total.toString(), 'true()');
      schema.dispose();
    });
  });

  test('returns every report of an invalid document, with locations', () => {
    withProcessor((processor) => {
      const schema = processor.compileSchema(purchaseOrder);
      const result = schema.validate('<order>\n  <item quantity="0"/>\n  <other/>\n</order>', { uri: 'urn:order.xml' });
      assert.equal(result.validity, 'invalid');
      assert.equal(result.document, undefined);
      assert.deepEqual(
        result.reports.map(({ code, sourceName, lineNumber }) => [code, sourceName, lineNumber]),
        [
          ['cvc-datatype-valid.1.2.1', 'urn:order.xml', 2],
          ['cvc-complex-type.2.4', 'urn:order.xml', 3],
        ],
      );
    });
  });

  test('validates nodes, laxly, and against a named type', () => {
    withProcessor((processor) => {
      const schema = processor.compileSchema(purchaseOrder);
      const document = processor.parse('<order><item quantity="1"/></order>');
      assert.equal(schema.validate(document).validity, 'valid');
      assert.equal(schema.validate('<unknown/>').validity, 'invalid');
      assert.equal(schema.validate('<unknown/>', { mode: 'lax' }).validity, 'not-known');
      assert.equal(schema.validate('<unknown><x/></unknown>', { type: 'loose' }).validity, 'valid');
      assert.throws(() => schema.validate('<x/>', { type: 'missing' }), /no type of that name/);
      assert.throws(() => schema.validate('<x>'), (error: unknown) => {
        assert.ok(error instanceof SxmlError);
        assert.equal(error.status, 'MALFORMED_INPUT');
        return true;
      });
    });
  });

  test('reports every error of a schema that does not compile', () => {
    withProcessor((processor) => {
      assert.throws(
        () =>
          processor.compileSchema({
            text: `<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:element name="a" type="missing"/>
  <xs:element name="b" type="alsoMissing"/>
</xs:schema>`,
            uri: 'urn:bad.xsd',
          }),
        (error: unknown) => {
          assert.ok(error instanceof SchemaError);
          assert.equal(error.code, 'src-resolve');
          assert.deepEqual(
            error.reports.map(({ code, sourceName, lineNumber }) => [code, sourceName, lineNumber]),
            [
              ['src-resolve', 'urn:bad.xsd', 2],
              ['src-resolve', 'urn:bad.xsd', 3],
            ],
          );
          return true;
        },
      );
      assert.throws(() => processor.compileSchema('<not-a-schema/>'), SchemaError);
    });
  });

  test('reads included schema documents through the resolver', () => {
    withProcessor(
      (processor) => {
        const schema = processor.compileSchema(
          { uri: 'main.xsd' },
          { baseUri: 'https://example.com/schemas/' },
        );
        assert.equal(schema.validate('<n>5</n>').validity, 'valid');
        assert.equal(schema.validate('<n>x</n>').validity, 'invalid');
      },
      {
        resolver: ({ kind, absoluteUri }: { kind: string; absoluteUri: string }) => {
          if (kind !== 'schema-document') {
            return undefined;
          }
          if (absoluteUri === 'https://example.com/schemas/main.xsd') {
            return '<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"><xs:include schemaLocation="types.xsd"/><xs:element name="n" type="number"/></xs:schema>';
          }
          if (absoluteUri === 'https://example.com/schemas/types.xsd') {
            return '<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"><xs:simpleType name="number"><xs:restriction base="xs:integer"/></xs:simpleType></xs:schema>';
          }
          return undefined;
        },
      },
    );
  });
});

describe('memory of the full module', { skip: !isFull }, () => {
  test('disposing a processor releases its queries, stylesheets and schemas', () => {
    const processor = sxml.createProcessor();
    const query = processor.compileQuery('1');
    const stylesheet = processor.compileStylesheet(identity);
    const schema = processor.compileSchema(purchaseOrder);
    processor.dispose();
    assert.equal(processor.liveAllocationCount(), 0);
    assert.throws(() => query.evaluate(), /disposed/);
    assert.throws(() => stylesheet.transform(), /disposed/);
    assert.throws(() => schema.validate('<order/>'), /disposed/);
    query.dispose();
    stylesheet.dispose();
    schema.dispose();
  });

  test('every allocation failure is reported and leaks nothing', () => {
    // As package.test.ts does for XPath (D23): fail the first, second, ...
    // allocation until the scenario succeeds.
    let succeeded = false;
    for (let failing = 1; !succeeded; failing += 1) {
      let processor: Processor;
      try {
        processor = sxml.createProcessor({ failingAllocationNumber: failing });
      } catch (error) {
        assert.ok(error instanceof SxmlError);
        continue;
      }
      try {
        const document = processor.parse('<order><item quantity="2"/></order>');
        const updates: UpdatedTree[] = [];
        processor
          .compileQuery('replace value of node //@quantity with 3')
          .evaluate({ context: document, updates: { updatedTree: (tree) => updates.push(tree) } });
        const result = processor
          .compileStylesheet(identity)
          .transform({ source: document, initialTemplate: 'named', templateParameters: { n: processor.evaluate('1') } });
        const validation = processor.compileSchema(purchaseOrder).validate(document);
        assert.equal(updates.length, 1);
        assert.equal(result.output, '<named n="1"/>');
        assert.equal(validation.validity, 'valid');
        succeeded = true;
      } catch (error) {
        assert.ok(error instanceof SxmlError, String(error));
        assert.ok(
          ['OUT_OF_MEMORY', 'LIMIT_EXCEEDED'].includes(error.status) || error.code !== '',
          error.message,
        );
      }
      processor.dispose();
      assert.equal(processor.liveAllocationCount(), 0, `leaked allocations failing allocation ${failing}`);
      assert.ok(failing < 1_000_000, 'no end to the allocation failures');
    }
  });
});
