package dev.tinyactors.sxml

import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/*
 * The XQuery, XSLT and XSD half of the parity list (D238): the cases of
 * the JavaScript package's test/full.test.ts, with the same names, inputs
 * and expected text. Every case ends with its processor's live allocation
 * count 0. libsxml_kotlin.so links all three (D238, D272).
 *
 * Golden file: bindings/javascript/test/full.test.ts.
 */

/** Runs body with a fresh processor that asks resolver, then closes it
 *  and checks for leaks. */
fun withResolver(resolver: Resolver, body: (Processor) -> Unit) {
    val processor = Processor(resolver = resolver)
    body(processor)
    closeChecked(processor)
}

/** Asserts that body throws an SxmlException whose message contains
 *  text. */
fun assertThrowsMatching(text: String, body: () -> Unit) {
    val error = assertFailsWith<SxmlException> { body() }
    assertTrue(error.message!!.contains(text), "expected '$text' in '${error.message}'")
}

const val IDENTITY = """<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xsl:output method="xml" omit-xml-declaration="yes"/>
  <xsl:param name="greeting" select="'Hello'"/>
  <xsl:template match="/">
    <out><xsl:value-of select="${'$'}greeting, //name"/></out>
  </xsl:template>
  <xsl:template match="name" mode="upper">
    <name><xsl:value-of select="upper-case(.)"/></name>
  </xsl:template>
  <xsl:template name="xsl:initial-template">
    <initial/>
  </xsl:template>
  <xsl:template name="named">
    <xsl:param name="n" as="xs:integer"/>
    <named n="{${'$'}n}"/>
  </xsl:template>
  <xsl:function name="f:double" xmlns:f="urn:f" as="xs:integer" visibility="public">
    <xsl:param name="x" as="xs:integer"/>
    <xsl:sequence select="${'$'}x * 2"/>
  </xsl:function>
</xsl:stylesheet>"""

const val PURCHASE_ORDER = """<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
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
</xs:schema>"""

val fullSuites = listOf(
    describe("XQuery") {
        test("compiles once and evaluates many times with external variables") {
            withProcessor { processor ->
                val query = processor.compileQuery(
                    "declare variable \$n external := 10; declare variable \$who external; \$who || \": \" || \$n * 2",
                )
                assertEquals(false, query.isUpdating)
                assertEquals("\"a: 2\"", query.evaluate(variables = mapOf("who" to "a", "n" to 1)).toString())
                assertEquals("\"b: 20\"", query.evaluate(variables = mapOf("who" to "b")).toString())
                // A name the query does not declare is ignored (XPath 4.0's
                // dynamic context rule); a required one without a value
                // raises XPDY0002.
                assertEquals("\"c: 20\"", query.evaluate(variables = mapOf("who" to "c", "other" to 3)).toString())
                assertEquals("err:XPDY0002", assertSxmlException { query.evaluate() }.code)
                query.close()
            }
        }

        test("evaluates against a context value, with namespaces and Q{uri}local names") {
            withProcessor { processor ->
                val document = processor.parse("<p:list xmlns:p=\"urn:p\"><p:i>1</p:i><p:i>2</p:i></p:list>")
                val query = processor.compileQuery(
                    "declare variable \$Q{urn:v}factor external; sum(//p:i) * \$Q{urn:v}factor",
                    namespaces = mapOf("p" to "urn:p"),
                )
                val result = query.evaluate(context = document, variables = mapOf("Q{urn:v}factor" to 10))
                assertEquals("3.0e1", result.toString())
            }
        }

        test("binds bin for EXPath Binary, as the sxml command does") {
            withProcessor { processor ->
                val query = processor.compileQuery("bin:length(bin:hex(\"CAFE\"))")
                assertEquals("2", query.run())
                query.close()
            }
        }

        test("serializes with the output declarations of the prolog") {
            withProcessor { processor ->
                val query = processor.compileQuery(
                    "declare option output:method \"json\"; declare option output:indent \"no\"; map { \"a\": [1, 2] }",
                )
                assertEquals("{\"a\":[1,2]}", query.run().filterNot { it.isWhitespace() })
                val xml = processor.compileQuery("<r>{1 + 1}</r>")
                // XQuery 4.0, 5.22.1: method xml, omit-xml-declaration yes.
                assertEquals("<r>2</r>", xml.run())
                val value = xml.evaluate()
                assertEquals(
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><r>2</r>",
                    value.serialize(mapOf("method" to "xml", "omitXmlDeclaration" to false)),
                )
            }
        }

        test("reports static errors with code, location and source") {
            withProcessor { processor ->
                val error = assertSxmlException { processor.compileQuery("declare variable \$x := 1;\n\$x +") }
                assertEquals(Status.STATIC_ERROR, error.status)
                assertEquals("err:XPST0003", error.code)
                assertEquals("query", error.sourceName)
                assertEquals(2, error.lineNumber)
                assertThrowsMatching("XPST0008") { processor.compileQuery("\$undeclared") }
            }
        }

        test("reports dynamic errors") {
            withProcessor { processor ->
                val query = processor.compileQuery("error(xs:QName(\"err:FOER0000\"), \"stop\")")
                val error = assertSxmlException { query.evaluate() }
                assertEquals(Status.DYNAMIC_ERROR, error.status)
                assertEquals("err:FOER0000", error.code)
                assertEquals("stop", error.description)
            }
        }

        test("imports library modules through the resolver") {
            val requests = ArrayList<String>()
            withResolver({ request ->
                requests.add("${request.kind} ${request.absoluteUri}")
                Resource.Text("module namespace m = \"urn:m\"; declare function m:twice(\$x) { \$x * 2 };")
            }) { processor ->
                val query = processor.compileQuery(
                    "import module namespace m = \"urn:m\" at \"m.xq\"; m:twice(21)",
                    baseUri = "https://example.com/q/",
                )
                assertEquals("42", query.evaluate().toString())
                assertEquals(listOf("XQUERY_MODULE https://example.com/q/m.xq"), requests)
            }
        }

        test("hands the trees an updating query changed to the update sink") {
            withProcessor { processor ->
                val document = processor.parse("<a><b>1</b></a>", uri = "urn:doc")
                val query = processor.compileQuery("replace value of node //b with \"2\", insert node <c/> into /a")
                assertEquals(true, query.isUpdating)
                val trees = ArrayList<UpdatedTree>()
                val result = query.evaluate(context = document, updates = UpdateSink(updatedTree = { trees.add(it) }))
                // XQUF 3.0, 2.2.3: an updating query's value is the empty
                // sequence.
                assertEquals("", result.toString())
                assertEquals(1, trees.size)
                assertEquals("<a><b>1</b></a>", trees[0].original.toString())
                assertEquals("<a><b>2</b><c/></a>", trees[0].updated.toString())
                // Trees never change: the document is as it was.
                assertEquals("<a><b>1</b></a>", document.toString())
                // Without a sink the trees are dropped, and nothing leaks.
                query.evaluate(context = document).close()
            }
        }

        test("hands fn:put documents to the update sink, or raises FOUP0002 without") {
            withProcessor { processor ->
                val query = processor.compileQuery(
                    "put(<doc>{1 + 1}</doc>, \"out.xml\", map { \"indent\": true() })",
                    baseUri = "file:///data/",
                )
                val documents = ArrayList<PutDocument>()
                query.evaluate(updates = UpdateSink(putDocument = { documents.add(it) }))
                assertEquals(1, documents.size)
                assertEquals("file:///data/out.xml", documents[0].uri)
                assertEquals("<doc>2</doc>", documents[0].node.toString())
                assertNotNull(documents[0].parameters)
                assertEquals("<doc>2</doc>", documents[0].serialize())
                assertThrowsMatching("FOUP0002") { query.evaluate() }
            }
        }

        test("delivers fn:trace and fn:message output") {
            withProcessor { processor ->
                val messages = ArrayList<Message>()
                val query = processor.compileQuery("trace(1 + 1, \"sum\"), message(\"note\")")
                query.evaluate(messages = { messages.add(it) })
                assertEquals(listOf("2" to "sum", "\"note\"" to ""), messages.map { it.text to it.code })
            }
        }
    },

    describe("XSLT") {
        test("compiles once and transforms many times with stylesheet parameters") {
            withProcessor { processor ->
                val stylesheet = processor.compileStylesheet(IDENTITY)
                val source = processor.parse("<person><name>Ada</name></person>")
                val result = stylesheet.transform(source = source)
                assertEquals("<out>Hello Ada</out>", result.output)
                assertNull(result.value)
                assertEquals(emptyList(), result.resultDocuments)
                assertEquals(
                    "<out>Hi Ada</out>",
                    stylesheet.transform(source = source, parameters = mapOf("greeting" to "Hi")).output,
                )
                stylesheet.close()
            }
        }

        test("invokes an initial mode, a named template or a function") {
            withProcessor { processor ->
                val stylesheet = processor.compileStylesheet(IDENTITY)
                val source = processor.parse("<person><name>Ada</name></person>")
                assertEquals(
                    "<name>ADA</name>",
                    stylesheet.transform(source = source, initialMode = InitialMode.Named("upper")).output,
                )
                assertEquals("<initial/>", stylesheet.transform().output)
                assertEquals(
                    "<named n=\"3\"/>",
                    stylesheet.transform(initialTemplate = "named", templateParameters = mapOf("n" to processor.evaluate("3")))
                        .output,
                )
                val result = stylesheet.transform(
                    initialFunction = "Q{urn:f}double",
                    args = listOf(processor.evaluate("21")),
                    delivery = ResultDelivery.RAW,
                )
                assertNull(result.output)
                assertEquals("42", result.value?.toString())
            }
        }

        test("delivers the principal result raw, as a tree, or with other parameters") {
            withProcessor { processor ->
                val stylesheet = processor.compileStylesheet(IDENTITY)
                val source = processor.parse("<person><name>Ada</name></person>")
                val tree = stylesheet.transform(source = source, delivery = ResultDelivery.TREE).value
                assertEquals("1", processor.evaluate("count(/out)", context = tree).toString())
                val json = stylesheet.transform(
                    source = source,
                    initialFunction = "Q{urn:f}double",
                    args = listOf(processor.evaluate("4")),
                    serialization = mapOf("method" to "json"),
                )
                assertEquals("8", json.output)
                val text = stylesheet.transform(source = source, serialization = mapOf("method" to "text"))
                assertEquals("Hello Ada", text.output)
            }
        }

        test("takes static parameters at compilation") {
            withProcessor { processor ->
                val stylesheet = processor.compileStylesheet(
                    """<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
          <xsl:param name="mode" static="yes" select="'a'"/>
          <xsl:template name="xsl:initial-template">
            <xsl:value-of select="'A'" use-when="${'$'}mode = 'a'"/>
            <xsl:value-of select="'B'" use-when="${'$'}mode = 'b'"/>
          </xsl:template>
        </xsl:stylesheet>""",
                    staticParameters = mapOf("mode" to "b"),
                )
                assertEquals("B", stylesheet.transform(serialization = mapOf("method" to "text")).output)
            }
        }

        test("collects result documents, serialized or as values") {
            withProcessor { processor ->
                val stylesheet = processor.compileStylesheet(
                    """<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
          <xsl:template name="xsl:initial-template">
            <xsl:for-each select="1 to 2">
              <xsl:result-document href="part{.}.xml" omit-xml-declaration="yes"><part n="{.}"/></xsl:result-document>
            </xsl:for-each>
            <main/>
          </xsl:template>
        </xsl:stylesheet>""",
                )
                val serialized = stylesheet.transform(
                    baseOutputUri = "https://example.com/out/",
                    serialization = mapOf("method" to "xml", "omitXmlDeclaration" to true),
                )
                assertEquals("<main/>", serialized.output)
                assertEquals(
                    listOf(
                        Triple("https://example.com/out/part1.xml", "<part n=\"1\"/>", null),
                        Triple("https://example.com/out/part2.xml", "<part n=\"2\"/>", null),
                    ),
                    serialized.resultDocuments.map { Triple(it.uri, it.output, it.value) },
                )
                val trees = stylesheet.transform(baseOutputUri = "urn:out/", resultDocumentDelivery = ResultDelivery.TREE)
                assertEquals(2, trees.resultDocuments.size)
                assertNull(trees.resultDocuments[1].output)
                assertEquals("<part n=\"2\"/>", trees.resultDocuments[1].value?.toString())
            }
        }

        test("delivers xsl:message output, also when it terminates") {
            withProcessor { processor ->
                val stylesheet = processor.compileStylesheet(
                    """<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
          <xsl:template name="xsl:initial-template">
            <xsl:message>starting</xsl:message>
            <xsl:message terminate="{${'$'}stop}">stopping</xsl:message>
            <done/>
          </xsl:template>
          <xsl:param name="stop" select="'no'"/>
        </xsl:stylesheet>""",
                )
                val messages = ArrayList<Message>()
                stylesheet.transform(messages = { messages.add(it) })
                assertEquals(listOf("starting" to false, "stopping" to false), messages.map { it.text to it.isTerminating })
                messages.clear()
                assertThrowsMatching("XTMM9000") {
                    stylesheet.transform(parameters = mapOf("stop" to "yes"), messages = { messages.add(it) })
                }
                assertEquals(
                    listOf(Triple("starting", "XTMM9000", false), Triple("stopping", "XTMM9000", true)),
                    messages.map { Triple(it.text, it.code, it.isTerminating) },
                )
            }
        }

        test("reads included modules through the resolver") {
            withResolver({ request ->
                if (request.kind == ResourceKind.XSLT_MODULE && request.absoluteUri == "https://example.com/lib.xsl") {
                    Resource.Text(
                        """<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
                <xsl:template name="from-lib">included</xsl:template>
              </xsl:stylesheet>""",
                    )
                } else {
                    null
                }
            }) { processor ->
                val stylesheet = processor.compileStylesheet(
                    """<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
            <xsl:include href="lib.xsl"/>
            <xsl:template name="xsl:initial-template"><xsl:call-template name="from-lib"/></xsl:template>
          </xsl:stylesheet>""",
                    baseUri = "https://example.com/main.xsl",
                )
                assertEquals("included", stylesheet.transform(serialization = mapOf("method" to "text")).output)
            }
        }

        test("reports static and dynamic errors with codes and locations") {
            withProcessor { processor ->
                val error = assertSxmlException {
                    processor.compileStylesheet(
                        "<xsl:stylesheet version=\"3.0\" xmlns:xsl=\"http://www.w3.org/1999/XSL/Transform\">\n<xsl:bogus/>\n</xsl:stylesheet>",
                        baseUri = "urn:bad.xsl",
                    )
                }
                assertEquals(Status.STATIC_ERROR, error.status)
                assertTrue(error.code.startsWith("err:XTSE"), error.code)
                assertEquals("urn:bad.xsl", error.sourceName)
                assertEquals(2, error.lineNumber)
                val stylesheet = processor.compileStylesheet(IDENTITY)
                assertThrowsMatching("XTDE0040") { stylesheet.transform(initialTemplate = "missing") }
                assertThrowsMatching("XTDE0700") { stylesheet.transform(initialTemplate = "named") }
            }
        }
    },

    describe("XSD validation") {
        test("validates documents and returns the typed copy") {
            withProcessor { processor ->
                val schema = processor.compileSchema(SchemaDocument.Text(PURCHASE_ORDER, "urn:po.xsd"))
                val result = schema.validate("<order><item quantity=\"2\"/><item quantity=\"3\"/></order>")
                assertEquals(Validity.VALID, result.validity)
                assertEquals(emptyList(), result.reports)
                // The copy is annotated (XDM 4.0, 3.3): quantities are
                // integers.
                val total = processor.evaluate("sum(//@quantity) instance of xs:integer", context = result.document)
                assertEquals("true()", total.toString())
                schema.close()
            }
        }

        test("returns every report of an invalid document, with locations") {
            withProcessor { processor ->
                val schema = processor.compileSchema(PURCHASE_ORDER)
                val result = schema.validate("<order>\n  <item quantity=\"0\"/>\n  <other/>\n</order>", uri = "urn:order.xml")
                assertEquals(Validity.INVALID, result.validity)
                assertNull(result.document)
                assertEquals(
                    listOf(
                        Triple("cvc-datatype-valid.1.2.1", "urn:order.xml", 2),
                        Triple("cvc-complex-type.2.4", "urn:order.xml", 3),
                    ),
                    result.reports.map { Triple(it.code, it.sourceName, it.lineNumber) },
                )
            }
        }

        test("validates nodes, laxly, and against a named type") {
            withProcessor { processor ->
                val schema = processor.compileSchema(PURCHASE_ORDER)
                val document = processor.parse("<order><item quantity=\"1\"/></order>")
                assertEquals(Validity.VALID, schema.validate(document).validity)
                assertEquals(Validity.INVALID, schema.validate("<unknown/>").validity)
                assertEquals(Validity.NOT_KNOWN, schema.validate("<unknown/>", mode = ValidationMode.LAX).validity)
                assertEquals(Validity.VALID, schema.validate("<unknown><x/></unknown>", type = "loose").validity)
                assertThrowsMatching("no type of that name") { schema.validate("<x/>", type = "missing") }
                assertEquals(Status.MALFORMED_INPUT, assertSxmlException { schema.validate("<x>") }.status)
            }
        }

        test("reports every error of a schema that does not compile") {
            withProcessor { processor ->
                val error = assertFailsWith<SchemaException> {
                    processor.compileSchema(
                        SchemaDocument.Text(
                            """<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:element name="a" type="missing"/>
  <xs:element name="b" type="alsoMissing"/>
</xs:schema>""",
                            "urn:bad.xsd",
                        ),
                    )
                }
                assertEquals("src-resolve", error.code)
                assertEquals(
                    listOf(Triple("src-resolve", "urn:bad.xsd", 2), Triple("src-resolve", "urn:bad.xsd", 3)),
                    error.reports.map { Triple(it.code, it.sourceName, it.lineNumber) },
                )
                assertFailsWith<SchemaException> { processor.compileSchema("<not-a-schema/>") }
            }
        }

        test("reads included schema documents through the resolver") {
            withResolver({ request ->
                when {
                    request.kind != ResourceKind.SCHEMA_DOCUMENT -> null
                    request.absoluteUri == "https://example.com/schemas/main.xsd" -> Resource.Text(
                        "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"><xs:include schemaLocation=\"types.xsd\"/><xs:element name=\"n\" type=\"number\"/></xs:schema>",
                    )
                    request.absoluteUri == "https://example.com/schemas/types.xsd" -> Resource.Text(
                        "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"><xs:simpleType name=\"number\"><xs:restriction base=\"xs:integer\"/></xs:simpleType></xs:schema>",
                    )
                    else -> null
                }
            }) { processor ->
                val schema = processor.compileSchema(SchemaDocument.Resolved("main.xsd"), baseUri = "https://example.com/schemas/")
                assertEquals(Validity.VALID, schema.validate("<n>5</n>").validity)
                assertEquals(Validity.INVALID, schema.validate("<n>x</n>").validity)
            }
        }
    },

    describe("memory of the full module") {
        test("disposing a processor releases its queries, stylesheets and schemas") {
            val processor = Processor()
            val query = processor.compileQuery("1")
            val stylesheet = processor.compileStylesheet(IDENTITY)
            val schema = processor.compileSchema(PURCHASE_ORDER)
            processor.close()
            assertEquals(0, processor.liveAllocationCount())
            assertFailsWith<IllegalStateException> { query.evaluate() }
            assertFailsWith<IllegalStateException> { stylesheet.transform() }
            assertFailsWith<IllegalStateException> { schema.validate("<order/>") }
            query.close()
            stylesheet.close()
            schema.close()
        }

        test("every allocation failure is reported and leaks nothing") {
            // As package.test.ts does for XPath (D23): fail the first,
            // second, ... allocation until the scenario succeeds.
            var succeeded = false
            var failing = 1
            while (!succeeded) {
                val processor = try {
                    Processor(failingAllocationNumber = failing)
                } catch (error: SxmlException) {
                    failing += 1
                    continue
                }
                try {
                    val document = processor.parse("<order><item quantity=\"2\"/></order>")
                    val updates = ArrayList<UpdatedTree>()
                    processor.compileQuery("replace value of node //@quantity with 3")
                        .evaluate(context = document, updates = UpdateSink(updatedTree = { updates.add(it) }))
                    val result = processor.compileStylesheet(IDENTITY)
                        .transform(source = document, initialTemplate = "named", templateParameters = mapOf("n" to processor.evaluate("1")))
                    val validation = processor.compileSchema(PURCHASE_ORDER).validate(document)
                    assertEquals(1, updates.size)
                    assertEquals("<named n=\"1\"/>", result.output)
                    assertEquals(Validity.VALID, validation.validity)
                    succeeded = true
                } catch (error: SxmlException) {
                    assertTrue(
                        error.status == Status.OUT_OF_MEMORY || error.status == Status.LIMIT_EXCEEDED || error.code.isNotEmpty(),
                        error.message,
                    )
                }
                closeChecked(processor)
                assertTrue(failing < 1_000_000, "no end to the allocation failures")
                failing += 1
            }
        }
    },
)
