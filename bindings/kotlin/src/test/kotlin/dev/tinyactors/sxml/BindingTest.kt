package dev.tinyactors.sxml

import java.io.File
import java.util.concurrent.CyclicBarrier
import java.util.concurrent.atomic.AtomicReference
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

/*
 * The Kotlin binding's own rules (D238): close and the cleaner, threads,
 * input conversion and the example of the task file. Every case ends with
 * its processors' live allocation counts 0.
 */

/** Runs body on count threads at once and rethrows the first failure. */
private fun onThreads(count: Int, body: (Int) -> Unit) {
    val barrier = CyclicBarrier(count)
    val failure = AtomicReference<Throwable?>(null)
    val threads = (0 until count).map { threadIndex ->
        Thread {
            try {
                barrier.await()
                body(threadIndex)
            } catch (exception: Throwable) {
                failure.compareAndSet(null, exception)
            }
        }
    }
    threads.forEach { it.start() }
    threads.forEach { it.join() }
    failure.get()?.let { throw it }
}

val bindingSuites = listOf(
    describe("the Kotlin binding") {
        test("runs the example of the task file") {
            val processor = Processor()
            processor.use {
                val book = processor.parse("<book><title>XPath</title><year>2026</year></book>")
                val expression = processor.compile("//title/string(), xs:integer(//year) + \$n", variables = listOf("n"))
                expression.evaluate(context = book, variables = mapOf("n" to 1)).use { result ->
                    assertEquals("\"XPath\"\n2.027e3", result.toString())
                    assertEquals("XPath 2027", result.serialize(mapOf("method" to "text")))
                    // The JSON method takes one item only (Serialization 4.0,
                    // 10: SERE0023), so the example uses the text method.
                    val error = assertSxmlException { result.serialize(mapOf("method" to "json")) }
                    assertEquals("err:SERE0023", error.code)
                }
            }
            assertEquals(0, processor.liveAllocationCount())
        }

        test("text crosses as UTF-8: supplementary characters are kept, U+0000 is refused") {
            withProcessor { processor ->
                val result = processor.evaluate("string-length(\$text), string-to-codepoints(\$text)[1]", variables = mapOf("text" to "😀x"))
                assertEquals("2\n128512", result.toString())
                result.close()
                // Modified UTF-8 would write U+0000 as C0 80, which is not
                // UTF-8; the binding passes the bytes of encodeToByteArray,
                // and the host shim refuses U+0000, which is no XML 1.1
                // character (XML 1.1, production [2] Char).
                for (text in listOf("a\u0000b", "\uFFFF")) {
                    val refused = assertSxmlException { processor.evaluate(".", context = text) }
                    assertEquals(Status.INVALID_ARGUMENT, refused.status)
                }
                val control = processor.evaluate("string-to-codepoints(\$text)", variables = mapOf("text" to "a\u0001b"))
                assertEquals("97\n1\n98", control.toString())
                control.close()
                val json = processor.value(mapOf("text" to "😀"))
                val length = processor.evaluate("string-length(\$map?text)", variables = mapOf("map" to json))
                assertEquals("1", length.toString())
                length.close()
                json.close()
            }
        }

        test("converts host data by the fn:parse-json rule") {
            withProcessor { processor ->
                val value = processor.value(mapOf("list" to arrayOf<Any?>(1.5, true, null), "nested" to mapOf("a" to listOf<Any>())))
                assertEquals("{\"list\":[1.5,true,null],\"nested\":{\"a\":[]}}", value.serialize(mapOf("method" to "json")))
                value.close()
                assertFailsWith<IllegalArgumentException> { processor.value(Any()) }
                assertFailsWith<IllegalArgumentException> { processor.value(mapOf(1 to 2)) }
                assertFailsWith<IllegalArgumentException> { processor.value(Double.NaN) }
            }
        }
    },

    describe("resource data") {
        // The data kinds a feature asks the resolver for (D209, D213):
        // TZif files of the IANA Time Zone Database and CLDR files, here
        // from the vendored copies, as a host serves them from its own.
        test("serves time zones and locale data through the resolver") {
            val requests = ArrayList<ResourceKind>()
            withResolver({ request ->
                requests.add(request.kind)
                val file = when (request.kind) {
                    ResourceKind.TIMEZONE_DATA ->
                        File(sourceDirectory, "third_party/tzdata/zoneinfo/" + request.absoluteUri.removePrefix("urn:sxml:timezone-data:"))
                    ResourceKind.LOCALE_DATA ->
                        File(sourceDirectory, "third_party/cldr/common/" + request.absoluteUri.removePrefix("urn:sxml:locale-data:"))
                    else -> null
                }
                if (file != null && file.isFile) Resource.Bytes(file.readBytes()) else null
            }) { processor ->
                processor.evaluate(
                    "civil-timezone(xs:dateTime(\"2026-07-01T12:00:00Z\"), \"Europe/Paris\")," +
                        " format-date(xs:date(\"2026-10-20\"), \"[MNn]\", \"de\", (), ())",
                ).use { result ->
                    assertEquals("xs:dayTimeDuration(\"PT2H\")\n\"Oktober\"", result.toString())
                }
                assertTrue(ResourceKind.TIMEZONE_DATA in requests, requests.toString())
                assertTrue(ResourceKind.LOCALE_DATA in requests, requests.toString())
            }
        }

        // CollationData.Bundled reads dev/tinyactors/sxml/allkeys.txt from
        // the class path, which the build copies from the vendored DUCET.
        test("the bundled DUCET is a class path resource") {
            withProcessor { processor ->
                processor.evaluate("compare('Chap2', 'Chap10', 'http://www.w3.org/2013/collation/UCA?numeric=yes;fallback=no')")
                    .use { result -> assertEquals("-1", result.toString()) }
            }
        }
    },

    describe("close and the cleaner") {
        test("close is idempotent, and use {} closes") {
            val processor = Processor()
            val value = processor.parse("<a/>")
            value.close()
            value.close()
            processor.compile("1").use { expression -> expression.evaluate().use { assertEquals("1", it.toString()) } }
            processor.close()
            processor.close()
            assertEquals(0, processor.liveAllocationCount())
        }

        test("an object used after it is closed is IllegalStateException") {
            val processor = Processor()
            val value = processor.parse("<a/>")
            val expression = processor.compile(".")
            value.close()
            expression.close()
            assertFailsWith<IllegalStateException> { value.toString() }
            assertFailsWith<IllegalStateException> { expression.evaluate() }
            assertFailsWith<IllegalStateException> { processor.evaluate(".", context = value) }
            closeChecked(processor)
            assertFailsWith<IllegalStateException> { processor.evaluate("1") }
        }

        test("expressions that are never closed are collected") {
            val processor = Processor()
            processor.compile("1").close()
            val baseline = processor.liveAllocationCount()
            compileGarbage(processor)
            var attempt = 0
            while (attempt < 50 && processor.liveAllocationCount() > baseline) {
                System.gc()
                Thread.sleep(20)
                attempt += 1
            }
            assertEquals(baseline, processor.liveAllocationCount())
            closeChecked(processor)
        }

        test("queries, stylesheets and schemas that are never closed are collected") {
            val processor = Processor()
            processor.compileQuery("1").close()
            processor.compileStylesheet(IDENTITY).close()
            processor.compileSchema(PURCHASE_ORDER).close()
            val baseline = processor.liveAllocationCount()
            compileFullGarbage(processor)
            var attempt = 0
            while (attempt < 50 && processor.liveAllocationCount() > baseline) {
                System.gc()
                Thread.sleep(20)
                attempt += 1
            }
            assertEquals(baseline, processor.liveAllocationCount())
            closeChecked(processor)
        }

        test("results, updated trees and result documents are closed with use {}") {
            withProcessor { processor ->
                // The first round fills the processor's name pool and
                // caches; the second must then leave the count as it was.
                useFullObjects(processor)
                val baseline = processor.liveAllocationCount()
                useFullObjects(processor)
                assertEquals(baseline, processor.liveAllocationCount())
            }
        }

        test("a processor that is never closed is collected with its values") {
            // Only the count of a processor that is still open can be read,
            // so this checks that the cleaner's work leaves the library
            // usable: the processors collected here destroy their values
            // first, which ASan would report if done in the wrong order.
            dropProcessors()
            for (attempt in 0 until 10) {
                System.gc()
                Thread.sleep(20)
            }
            withProcessor { processor ->
                processor.evaluate("1").use { assertEquals("1", it.toString()) }
            }
        }
    },

    describe("threads") {
        test("two threads that share a processor take turns") {
            val processor = Processor()
            val expression = processor.compile("xs:integer(\$n) * 2", variables = listOf("n"))
            onThreads(2) { threadIndex ->
                for (iteration in 0 until 500) {
                    val number = threadIndex * 1000 + iteration
                    expression.evaluate(variables = mapOf("n" to number)).use { result ->
                        assertEquals("${number * 2}", result.toString())
                    }
                    processor.parse("<n>$number</n>").use { document ->
                        processor.evaluate("number(/n)", context = document).use { result ->
                            assertEquals("$number", result.serialize(mapOf("method" to "text")))
                        }
                    }
                }
            }
            expression.close()
            closeChecked(processor)
        }

        test("separate processors run in parallel") {
            onThreads(4) { threadIndex ->
                withProcessor { processor ->
                    for (iteration in 0 until 200) {
                        processor.evaluate("sum(1 to xs:integer(\$n))", variables = mapOf("n" to threadIndex + iteration)).use { result ->
                            val count = threadIndex + iteration
                            assertEquals("${count * (count + 1) / 2}", result.toString())
                        }
                    }
                }
            }
        }

        test("the cleaner takes the processor's lock") {
            val processor = Processor()
            onThreads(2) { threadIndex ->
                for (iteration in 0 until 200) {
                    if (threadIndex == 0) {
                        processor.parse("<a>$iteration</a>")
                        if (iteration % 50 == 0) {
                            System.gc()
                        }
                    } else {
                        processor.evaluate("count(1 to $iteration)").use { result ->
                            assertEquals("$iteration", result.toString())
                        }
                    }
                }
            }
            closeChecked(processor)
            assertTrue(processor.liveAllocationCount() == 0)
        }
    },
)

private fun compileGarbage(processor: Processor) {
    for (index in 0 until 100) {
        processor.compile("$index")
    }
}

/** Makes and closes a query, a stylesheet and a schema, and the values
 *  they give. */
private fun useFullObjects(processor: Processor) {
    processor.parse("<a/>").use { document ->
        processor.compileQuery("insert node <b/> into /a").use { query ->
            query.evaluate(context = document, updates = UpdateSink(updatedTree = { tree -> tree.use { } })).close()
        }
        processor.compileStylesheet(IDENTITY).use { stylesheet ->
            stylesheet.transform(source = document, delivery = ResultDelivery.TREE).use { result ->
                assertEquals("<out>Hello</out>", result.value?.toString())
            }
        }
    }
    processor.compileSchema(PURCHASE_ORDER).use { schema ->
        schema.validate("<order><item quantity=\"1\"/></order>").use { result ->
            assertEquals(Validity.VALID, result.validity)
        }
    }
}

private fun compileFullGarbage(processor: Processor) {
    for (index in 0 until 20) {
        processor.compileQuery("$index")
        processor.compileStylesheet(IDENTITY)
        processor.compileSchema(PURCHASE_ORDER)
    }
}

private fun dropProcessors() {
    for (index in 0 until 20) {
        val processor = Processor()
        processor.parse("<a>$index</a>")
        processor.compile("$index")
    }
}
