package dev.tinyactors.sxml

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
                    // The task file's example also serializes the result with
                    // the JSON method, which a sequence of two items cannot
                    // take (Serialization 4.0, 10: SERE0023).
                    val error = assertSxmlException { result.serialize(mapOf("method" to "json")) }
                    assertEquals("err:SERE0023", error.code)
                }
            }
            assertEquals(0, processor.liveAllocationCount())
        }

        test("text crosses as UTF-8: U+0000 and supplementary characters are kept") {
            withProcessor { processor ->
                val result = processor.evaluate("string-length(\$text), string-to-codepoints(\$text)[1]", variables = mapOf("text" to "😀x"))
                assertEquals("2\n128512", result.toString())
                result.close()
                // Modified UTF-8 would write U+0000 as C0 80, which is not
                // UTF-8; the binding passes the bytes of encodeToByteArray.
                val zero = processor.evaluate("string-to-codepoints(\$text)", variables = mapOf("text" to "a\u0000b"))
                assertEquals("97\n0\n98", zero.toString())
                zero.close()
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

private fun dropProcessors() {
    for (index in 0 until 20) {
        val processor = Processor()
        processor.parse("<a>$index</a>")
        processor.compile("$index")
    }
}
