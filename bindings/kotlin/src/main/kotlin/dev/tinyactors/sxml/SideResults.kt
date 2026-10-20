package dev.tinyactors.sxml

/*
 * Side results (docs/GLOSSARY.md, "side_result"; D216): what a call
 * delivers besides its result, read from the request after the call and
 * handed to the caller's callbacks then, never during the call (D71).
 * Golden file: the side results of bindings/javascript/src/index.ts.
 */

/**
 * Trace output (XSLT 4.0, 23.1 and 23.2; F&O 4.0, fn:trace and
 * fn:message): an xsl:message or failed xsl:assert as the text method
 * writes it, fn:trace and fn:message by the adaptive method.
 *
 * @property code xsl:message: its error code's local name (XTMM9000 by
 *   default); fn:trace and fn:message: the label.
 * @property isTerminating xsl:message terminate="yes" and xsl:assert: the
 *   call then throws.
 */
data class Message(
    val text: String,
    val code: String,
    val isTerminating: Boolean,
    val sourceName: String,
    val lineNumber: Int,
    val columnNumber: Int,
)

/** A side result as the call wrote it (sxml_host_side_result), its values
 *  adopted. */
internal class SideResult(
    val kind: Int,
    val uri: String,
    val text: String,
    val code: String,
    val first: Value?,
    val second: Value?,
    val lineNumber: Int,
    val columnNumber: Int,
    val isTerminating: Boolean,
) {
    /** Closes the values nobody took. */
    fun closeValues() {
        first?.close()
        second?.close()
    }

    companion object {
        /** sxml_host_side_result_kind. */
        const val UPDATED_TREE = 0
        const val PUT_DOCUMENT = 1
        const val RESULT_DOCUMENT = 2
        const val MESSAGE = 3
        const val SCHEMA_REPORT = 4
    }
}

/** What a call that writes side results made: its value or failure. */
internal class CallOutcome(val value: Value?, val failure: Throwable?, val sideResults: List<SideResult>)

/** Hands the MESSAGE side results to receive. */
internal fun deliverMessages(results: List<SideResult>, receive: ((Message) -> Unit)?) {
    if (receive == null) {
        return
    }
    for (result in results) {
        if (result.kind == SideResult.MESSAGE) {
            receive(Message(result.text, result.code, result.isTerminating, result.uri, result.lineNumber, result.columnNumber))
        }
    }
}
