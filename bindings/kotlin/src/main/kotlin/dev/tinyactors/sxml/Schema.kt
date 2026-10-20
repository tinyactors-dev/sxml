package dev.tinyactors.sxml

/** A schema document to compile: its text, or a URI the resolver is asked
 *  for ([ResourceKind.SCHEMA_DOCUMENT]). */
sealed class SchemaDocument {
    /** A document's text (UTF-8, or bytes in the encoding it declares),
     *  with its URI, against which its includes and imports resolve. */
    class Text(val bytes: ByteArray, val uri: String? = null) : SchemaDocument() {
        constructor(text: String, uri: String? = null) : this(text.encodeToByteArray(), uri)
    }

    /** A document by URI reference, resolved against the base URI. */
    class Resolved(val uri: String) : SchemaDocument()
}

/** How the validation root is assessed (XSD 1.1 Part 1, 5.2): [STRICT]
 *  needs a global declaration for it; [LAX] does not. In the order of
 *  sxml_validation_mode. */
enum class ValidationMode {
    STRICT,
    LAX,
}

/** The validity of the validation root (Part 1, 3.3.5.1), in the order of
 *  sxml_validity. */
enum class Validity {
    VALID,
    INVALID,
    NOT_KNOWN,
    ;

    internal companion object {
        fun of(number: Int): Validity = entries.getOrElse(number) { INVALID }
    }
}

/** One error of a compilation or validation (XSD 1.1 Part 1, appendix C).
 *
 * @property code the constraint name, such as `cvc-complex-type.2.4`.
 * @property sourceName the schema document's URI, or the instance's. */
data class SchemaReport(
    val code: String,
    val description: String,
    val sourceName: String,
    val lineNumber: Int,
    val columnNumber: Int,
)

/** What a validation found. Closing it closes [document]. */
class ValidationResult internal constructor(
    val validity: Validity,
    /** Every error found, in order; empty when valid. */
    val reports: List<SchemaReport>,
    /** Unless invalid: the validated copy, its elements and attributes
     *  annotated with their types (XDM 4.0, 3.3). */
    val document: Value?,
) : AutoCloseable {
    override fun close() {
        document?.close()
    }
}

/** A schema that does not compile: every error, with the first as the
 *  exception's own code, description and location. */
class SchemaException internal constructor(error: SxmlException, val reports: List<SchemaReport>) :
    SxmlException(
        error.status,
        error.code,
        error.codeNamespaceUri,
        error.codeLocalName,
        error.description,
        error.sourceName,
        error.lineNumber,
        error.columnNumber,
    )

/**
 * A compiled XSD 1.1 schema (include/sxml/schema.h), validating many
 * documents. [Processor.compileSchema] makes it.
 *
 * Golden file: the Schema class of bindings/javascript/src/index.ts.
 */
class Schema internal constructor(val processor: Processor, handle: Long) : AutoCloseable {
    private val native = NativeHandle(this, processor, HandleKind.SCHEMA, handle, "schema")

    /**
     * Validates a document or element node of this processor (XSD 1.1
     * Part 1, 5.2). An invalid node is not an exception: see the result's
     * validity and reports.
     *
     * @param type validate the root against this type of the schema
     *   instead, by NCName or Q{uri}local.
     */
    fun validate(node: Value, mode: ValidationMode = ValidationMode.STRICT, type: String? = null): ValidationResult =
        validateInput(node, null, ValidationOptions(mode, type, null))

    /** Validates XML text, parsed and validated in one pass so that the
     *  reports have its lines and columns; [uri] is its document URI. Text
     *  that is not well-formed throws, as [Processor.parse] does. */
    fun validate(
        text: String,
        mode: ValidationMode = ValidationMode.STRICT,
        type: String? = null,
        uri: String? = null,
    ): ValidationResult = validateInput(null, text.encodeToByteArray(), ValidationOptions(mode, type, uri))

    /** Validates an XML document in the encoding it declares, as the text
     *  overload does. */
    fun validate(
        bytes: ByteArray,
        mode: ValidationMode = ValidationMode.STRICT,
        type: String? = null,
        uri: String? = null,
    ): ValidationResult = validateInput(null, bytes, ValidationOptions(mode, type, uri))

    /** Releases the schema. Idempotent. */
    override fun close() = native.close()

    private class ValidationOptions(val mode: ValidationMode, val type: String?, val uri: String?)

    private fun validateInput(node: Value?, text: ByteArray?, options: ValidationOptions): ValidationResult =
        synchronized(processor.state) {
            val state = processor.state
            state.enter()
            val address = native.address()
            if (node != null) {
                processor.checkOwn(node)
            }
            val (namespaceUri, localName) = if (options.type == null) "" to "" else splitName(options.type)
            val request = ValidateRequest(
                state.processor,
                address,
                node?.address() ?: 0,
                text,
                (options.uri ?: "").encodeToByteArray(),
                options.mode.ordinal,
                arrayOf(namespaceUri.encodeToByteArray(), localName.encodeToByteArray()),
            )
            Native.schemaValidate(request)
            val failure = if (request.handle == 0L && state.hasFailed(request)) state.failure(request) else null
            val reports = schemaReports(processor.adoptSideResults(request.sideResults))
            if (failure != null) {
                throw failure
            }
            ValidationResult(Validity.of(request.validity), reports, processor.adoptOptional(request.handle))
        }
}

/** The SCHEMA_REPORT side results as reports. */
internal fun schemaReports(results: List<SideResult>): List<SchemaReport> =
    results.filter { it.kind == SideResult.SCHEMA_REPORT }.map { result ->
        SchemaReport(result.code, result.text, result.uri, result.lineNumber, result.columnNumber)
    }
