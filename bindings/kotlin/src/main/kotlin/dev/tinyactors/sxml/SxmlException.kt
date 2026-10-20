package dev.tinyactors.sxml

/**
 * A failure in sxml: a static, dynamic or type error of an expression (with
 * its error code), a document that is not well-formed, a limit, or out of
 * memory. The fields are those of the JavaScript package's `SxmlError`.
 *
 * @property status what failed, such as [Status.DYNAMIC_ERROR].
 * @property code the error code as prefix:local, such as `err:FOAR0001`;
 *   `Q{uri}local` without a prefix; empty when the status has none.
 * @property sourceName the module or document the location is in
 *   (`expression`, a module's URI); empty when unknown.
 */
open class SxmlException(
    val status: Status,
    val code: String,
    val codeNamespaceUri: String,
    val codeLocalName: String,
    val description: String,
    val sourceName: String,
    val lineNumber: Int,
    val columnNumber: Int,
) : Exception(messageOf(status, code, description, lineNumber, columnNumber)) {
    internal companion object {
        /** As the JavaScript package words it: `line:column: code: description`. */
        fun messageOf(status: Status, code: String, description: String, lineNumber: Int, columnNumber: Int): String {
            val where = if (lineNumber > 0) "$lineNumber:$columnNumber: " else ""
            val what = description.ifEmpty { status.name }
            return where + (if (code.isNotEmpty()) "$code: " else "") + what
        }

        /** The code as text: prefix:local, Q{uri}local or local. */
        fun codeOf(namespaceUri: String, localName: String, prefix: String): String = when {
            localName.isEmpty() -> ""
            prefix.isNotEmpty() -> "$prefix:$localName"
            namespaceUri.isNotEmpty() -> "Q{$namespaceUri}$localName"
            else -> localName
        }

        fun outOfMemory(): SxmlException =
            SxmlException(Status.OUT_OF_MEMORY, "", "", "", "Out of memory", "", 0, 0)
    }
}
