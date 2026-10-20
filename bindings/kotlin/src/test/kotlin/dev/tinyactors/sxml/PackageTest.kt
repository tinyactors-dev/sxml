package dev.tinyactors.sxml

import java.io.File
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertSame
import kotlin.test.assertTrue

/*
 * The XPath half of the parity list (D238): the cases of the JavaScript
 * package's test/package.test.ts, with the same names, inputs and expected
 * text. Every case ends with its processor's live allocation count 0.
 *
 * Golden file: bindings/javascript/test/package.test.ts.
 */

/** Closes processor and checks that nothing it allocated is left. */
fun closeChecked(processor: Processor) {
    processor.close()
    assertEquals(0, processor.liveAllocationCount(), "leaked allocations")
}

/** Runs body with a fresh processor, then closes it and checks for
 *  leaks. */
fun withProcessor(body: (Processor) -> Unit) {
    val processor = Processor()
    body(processor)
    closeChecked(processor)
}

/** Asserts that body throws an SxmlException and returns it. */
fun assertSxmlException(body: () -> Unit): SxmlException = assertFailsWith<SxmlException> { body() }

/** The optional features the library must have (D189): SXML_KOTLIN_FEATURES
 *  lists them, comma-separated. */
val expectedFeatures: List<String> =
    (System.getenv("SXML_KOTLIN_FEATURES") ?: "").split(',').filter { it.isNotEmpty() }

/** The repository, for the vendored DUCET. */
val sourceDirectory: File = File(System.getProperty("sxml.sourceDirectory") ?: ".")

val packageSuites = listOf(
    describe("the module") {
        test("reports the release version, that of the package") {
            assertEquals("0.2.0", Sxml.version)
        }
    },

    describe("optional features") {
        test("the module lists the features it links") {
            assertEquals(expectedFeatures, Sxml.features)
        }

        test("a processor takes a list of the module's features") {
            for (features in listOf(emptySet(), expectedFeatures.toSet(), null)) {
                val processor = Processor(features = features)
                val result = processor.evaluate("1 + 1")
                assertEquals("2", result.toString())
                result.close()
                closeChecked(processor)
            }
        }

        // The conventional prefix of EXPath Binary, as the sxml command
        // binds it (D235); the caller's namespaces may rebind it.
        test("bin is bound wherever the processor has expath-binary") {
            val hasBinary = "expath-binary" in expectedFeatures
            for (features in listOf(null, emptySet<String>())) {
                val processor = Processor(features = features)
                if (hasBinary && features == null) {
                    val result = processor.evaluate("bin:hex(\"CAFE\") => bin:to-octets()")
                    assertEquals("202\n254", result.toString())
                    result.close()
                } else {
                    val error = assertSxmlException { processor.evaluate("bin:hex(\"CAFE\")") }
                    assertEquals("err:XPST0081", error.code)
                }
                val rebound = processor.evaluate(
                    "string(namespace-uri-from-QName(xs:QName(\"bin:x\")))",
                    namespaces = mapOf("bin" to "urn:b"),
                )
                assertEquals("\"urn:b\"", rebound.toString())
                rebound.close()
                closeChecked(processor)
            }
        }

        test("a feature the module does not have is refused") {
            val error = assertFailsWith<IllegalArgumentException> { Processor(features = setOf("no-such-feature")) }
            assertTrue(error.message!!.contains("no optional feature"))
        }

        // The uca-collation feature reads the DUCET the processor was
        // given (D213); the vendored copy stands in for the bundled one.
        test("UCA collations use the DUCET when the module has the feature") {
            val uca = "http://www.w3.org/2013/collation/UCA?numeric=yes;fallback=no"
            val query = "compare('Chap2', 'Chap10', '$uca')"
            val hasFeature = "uca-collation" in expectedFeatures
            val ducet = File(sourceDirectory, "third_party/unicode-collation/allkeys.txt").readBytes()
            for ((collationData, isServed) in listOf(CollationData.Bytes(ducet) to hasFeature, CollationData.None to false)) {
                val processor = Processor(collationData = collationData)
                if (isServed) {
                    val result = processor.evaluate(query)
                    assertEquals("-1", result.toString())
                    result.close()
                } else {
                    val error = assertSxmlException { processor.evaluate(query) }
                    assertEquals("err:FOCH0002", error.code)
                }
                closeChecked(processor)
            }
        }
    },

    describe("XPath") {
        test("evaluates without a context") {
            withProcessor { processor ->
                val result = processor.evaluate("1 to 3")
                assertEquals("1\n2\n3", result.toString())
                result.close()
            }
        }

        test("evaluates against a parsed document") {
            withProcessor { processor ->
                val book = processor.parse("<book><title>XPath 4.0</title><title>XQuery</title></book>")
                val titles = processor.evaluate("//title/string()", context = book)
                assertEquals("\"XPath 4.0\"\n\"XQuery\"", titles.toString())
                val count = processor.evaluate("count(//title)", context = book)
                assertEquals("2", count.toString())
                for (value in listOf(count, titles, book)) {
                    value.close()
                }
            }
        }

        test("binds variables from values and JSON-like data") {
            withProcessor { processor ->
                val document = processor.parse("<a><b/><b/></a>")
                val result = processor.evaluate(
                    "count(\$document//b) + \$number, \$text, \$list?2, \$record?name, empty(\$nothing)",
                    variables = mapOf(
                        "document" to document,
                        "number" to 40,
                        "text" to "it's \"quoted\"",
                        "list" to listOf(1, "two"),
                        "record" to mapOf("name" to "sxml"),
                        "nothing" to null,
                    ),
                )
                assertEquals("4.2e1\n\"it's \"\"quoted\"\"\"\n\"two\"\n\"sxml\"\ntrue()", result.toString())
                result.close()
                document.close()
            }
        }

        test("takes variables in Q{uri}local form and namespaces") {
            withProcessor { processor ->
                val document = processor.parse("<p:root xmlns:p=\"urn:p\"><p:item>1</p:item></p:root>")
                val result = processor.evaluate(
                    "\$Q{urn:v}offset + number(/q:root/q:item)",
                    context = document,
                    namespaces = mapOf("q" to "urn:p"),
                    variables = mapOf("Q{urn:v}offset" to 1),
                )
                assertEquals("2.0e0", result.toString())
                result.close()
                document.close()
            }
        }

        test("compiles once and evaluates many times") {
            withProcessor { processor ->
                val expression = processor.compile("\$n * 2", variables = listOf("n"))
                assertEquals(listOf("n"), expression.variables)
                for (number in listOf(1, 2, 3)) {
                    val result = expression.evaluate(variables = mapOf("n" to number))
                    assertEquals("${number * 2}.0e0", result.toString())
                    result.close()
                }
                expression.close()
            }
        }

        test("a variable without a value raises XPDY0002") {
            withProcessor { processor ->
                val expression = processor.compile("\$n", variables = listOf("n"))
                val error = assertSxmlException { expression.evaluate() }
                assertEquals("err:XPDY0002", error.code)
                expression.close()
            }
        }

        test("reports static errors with code and location") {
            withProcessor { processor ->
                val error = assertSxmlException { processor.evaluate("1 +") }
                assertEquals(Status.STATIC_ERROR, error.status)
                assertEquals("err:XPST0003", error.code)
                assertEquals("http://www.w3.org/2005/xqt-errors", error.codeNamespaceUri)
                assertEquals(1, error.lineNumber)
            }
        }

        test("reports dynamic errors") {
            withProcessor { processor ->
                assertEquals("err:FOAR0001", assertSxmlException { processor.evaluate("1 idiv 0") }.code)
                val custom = assertSxmlException { processor.evaluate("error(xs:QName(\"err:XYZ\"), \"custom\")") }
                assertEquals("err:XYZ", custom.code)
                assertEquals("custom", custom.description)
            }
        }
    },

    describe("XML parsing") {
        test("reports a document that is not well-formed") {
            withProcessor { processor ->
                val error = assertSxmlException { processor.parse("<a>\n<b></a>", uri = "bad.xml") }
                assertEquals(Status.MALFORMED_INPUT, error.status)
                assertEquals(2, error.lineNumber)
            }
        }

        test("takes bytes in any encoding the document declares") {
            withProcessor { processor ->
                val bytes = "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a>".encodeToByteArray() +
                    byteArrayOf(0xe9.toByte()) + "</a>".encodeToByteArray()
                val document = processor.parse(bytes, uri = "http://example.com/a.xml")
                val result = processor.evaluate("string(.), document-uri(.)", context = document)
                assertEquals("\"é\"\n\"http://example.com/a.xml\"", result.toString())
                result.close()
                document.close()
            }
        }
    },

    describe("serialization") {
        test("writes nodes as XML by the adaptive method") {
            withProcessor { processor ->
                val document = processor.parse("<a x=\"1\"><b>text</b></a>")
                assertEquals("<a x=\"1\"><b>text</b></a>", document.toString())
                document.close()
            }
        }

        test("takes the parameters of fn:serialize") {
            withProcessor { processor ->
                val result = processor.evaluate("map { \"a\": [1, true()] }")
                assertEquals("{\"a\":[1,true]}", result.serialize(mapOf("method" to "json")))
                result.close()
                val document = processor.parse("<a><b/></a>")
                assertEquals(
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><a><b/></a>",
                    document.serialize(mapOf("method" to "xml", "omitXmlDeclaration" to false)),
                )
                assertEquals("", document.serialize(mapOf("method" to "text")))
                document.close()
            }
        }

        test("reports serialization errors") {
            withProcessor { processor ->
                val result = processor.evaluate("1, 2")
                assertEquals("err:SERE0023", assertSxmlException { result.serialize(mapOf("method" to "json")) }.code)
                assertSxmlException { result.serialize(mapOf("method" to "no-such-method")) }
                result.close()
            }
        }
    },

    describe("the resolver") {
        test("serves fn:doc and fn:unparsed-text") {
            val requests = ArrayList<String>()
            val processor = Processor(resolver = { request ->
                requests.add("${request.kind} ${request.absoluteUri}")
                when (request.absoluteUri) {
                    "http://example.com/data.xml" -> Resource.Text("<data><value>42</value></data>")
                    "http://example.com/note.txt" -> Resource.Bytes("a note".encodeToByteArray())
                    else -> null
                }
            })
            val result = processor.evaluate(
                "doc(\"data.xml\")//value/string(), unparsed-text(\"note.txt\"), doc-available(\"missing.xml\")",
                baseUri = "http://example.com/",
            )
            assertEquals("\"42\"\n\"a note\"\nfalse()", result.toString())
            assertEquals(
                listOf(
                    "XML_DOCUMENT http://example.com/data.xml",
                    "TEXT http://example.com/note.txt",
                    "XML_DOCUMENT http://example.com/missing.xml",
                ),
                requests,
            )
            val error = assertSxmlException { processor.evaluate("doc(\"http://example.com/missing.xml\")") }
            assertEquals("err:FODC0002", error.code)
            result.close()
            closeChecked(processor)
        }

        test("an exception in the resolver is rethrown") {
            val problem = RuntimeException("network down")
            val processor = Processor(resolver = { throw problem })
            val thrown = assertFailsWith<RuntimeException> { processor.evaluate("doc(\"http://example.com/a.xml\")") }
            assertSame(problem, thrown)
            val result = processor.evaluate("1")
            assertEquals("1", result.toString())
            result.close()
            closeChecked(processor)
        }

        test("the resolver may not use its processor") {
            var processor: Processor? = null
            processor = Processor(resolver = {
                processor?.parse("<a/>")
                Resource.Text("<b/>")
            })
            val error = assertFailsWith<IllegalStateException> { processor.evaluate("doc(\"http://example.com/a.xml\")") }
            assertTrue(error.message!!.contains("must not use"))
            closeChecked(processor)
        }
    },

    describe("memory") {
        test("disposing a processor releases what it made") {
            val processor = Processor()
            val document = processor.parse("<a/>")
            val expression = processor.compile(".")
            processor.close()
            assertFailsWith<IllegalStateException> { document.toString() }
            assertFailsWith<IllegalStateException> { expression.evaluate() }
            assertFailsWith<IllegalStateException> { processor.parse("<a/>") }
            document.close()
            expression.close()
            processor.close()
            assertEquals(0, processor.liveAllocationCount())
        }

        test("values of one processor are refused by another") {
            val first = Processor()
            val second = Processor()
            val document = first.parse("<a/>")
            val error = assertFailsWith<IllegalArgumentException> { second.evaluate(".", context = document) }
            assertTrue(error.message!!.contains("another processor"))
            closeChecked(first)
            closeChecked(second)
        }

        test("every allocation failure is reported and leaks nothing") {
            // As the C tests do with sxml_counting_allocator (D23): fail the
            // first, second, ... allocation until the scenario succeeds.
            var succeeded = false
            var failing = 1
            while (!succeeded) {
                val processor = try {
                    Processor(failingAllocationNumber = failing)
                } catch (error: SxmlException) {
                    assertEquals(Status.OUT_OF_MEMORY, error.status)
                    failing += 1
                    continue
                }
                val made = ArrayList<Value>()
                try {
                    val document = processor.parse("<a><b>1</b></a>")
                    made.add(document)
                    val result = processor.evaluate("sum(//b) + \$n", context = document, variables = mapOf("n" to listOf(2)))
                    made.add(result)
                    assertEquals("3", result.serialize(mapOf("method" to "text")))
                    succeeded = true
                } catch (error: SxmlException) {
                    assertTrue(
                        error.status == Status.OUT_OF_MEMORY || error.status == Status.LIMIT_EXCEEDED || error.code.isNotEmpty(),
                        error.message,
                    )
                }
                for (value in made) {
                    value.close()
                }
                closeChecked(processor)
                assertTrue(failing < 100_000, "no end to the allocation failures")
                failing += 1
            }
        }

        test("values that are never disposed are collected") {
            val processor = Processor()
            processor.parse("<a/>").close()
            val baseline = processor.liveAllocationCount()
            makeGarbage(processor)
            var attempt = 0
            while (attempt < 50 && processor.liveAllocationCount() > baseline) {
                System.gc()
                Thread.sleep(20)
                attempt += 1
            }
            assertEquals(baseline, processor.liveAllocationCount())
            closeChecked(processor)
        }
    },
)

/** Parses documents and drops them unclosed, in a frame of its own so that
 *  none stays reachable from the caller's. */
private fun makeGarbage(processor: Processor) {
    for (index in 0 until 100) {
        processor.parse("<a/>")
    }
}
