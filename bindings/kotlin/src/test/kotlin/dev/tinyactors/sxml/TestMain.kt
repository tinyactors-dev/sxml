package dev.tinyactors.sxml

import kotlin.system.exitProcess

/*
 * The test runner of ctest kotlin/jvm (D238): kotlin.test assertions and
 * named cases, without JUnit, so that ctest needs no Gradle and no
 * network. A suite is a list of cases with the names of the JavaScript
 * package's tests (parity by name); an Android instrumented test can run
 * the same lists (packet 6.6).
 */

/** A named test case. */
class TestCase(val name: String, val body: () -> Unit)

/** A group of cases, as a describe() block of the JavaScript tests. */
class TestSuite(val name: String) {
    val cases = ArrayList<TestCase>()

    fun test(name: String, body: () -> Unit) {
        cases.add(TestCase(name, body))
    }
}

fun describe(name: String, body: TestSuite.() -> Unit): TestSuite = TestSuite(name).apply(body)

/** Every suite of the binding's tests. */
val allSuites: List<TestSuite> get() = packageSuites + fullSuites + bindingSuites

/** Runs the suites (or those whose "suite > case" name contains the
 *  first argument) and exits 1 when a case failed. */
fun main(arguments: Array<String>) {
    val filter = arguments.firstOrNull() ?: ""
    var passed = 0
    val failed = ArrayList<String>()
    for (suite in allSuites) {
        for (case in suite.cases) {
            val name = "${suite.name} > ${case.name}"
            if (!name.contains(filter)) {
                continue
            }
            try {
                case.body()
                passed += 1
                println("ok - $name")
            } catch (failure: Throwable) {
                failed.add(name)
                println("not ok - $name")
                failure.printStackTrace(System.out)
            }
        }
    }
    println("# ${passed + failed.size} tests, $passed passed, ${failed.size} failed")
    for (name in failed) {
        println("# failed: $name")
    }
    exitProcess(if (failed.isEmpty()) 0 else 1)
}
