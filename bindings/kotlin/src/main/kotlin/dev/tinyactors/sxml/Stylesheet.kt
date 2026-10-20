package dev.tinyactors.sxml

/** How a result reaches you (XSLT 4.0, 2.4.6): [SERIALIZED] as a string,
 *  [RAW] as the value the stylesheet made, [TREE] as one document node.
 *  The numbers are sxml_xslt_result_delivery's. */
enum class ResultDelivery(internal val number: Int) {
    SERIALIZED(3),
    RAW(1),
    TREE(2),
}

/** The initial mode of a transformation (XSLT 4.0, 2.4.3). */
sealed class InitialMode {
    /** A mode by NCName or Q{uri}local. */
    class Named(val name: String) : InitialMode()

    /** The unnamed mode, also when the stylesheet declares another
     *  default mode. */
    data object Unnamed : InitialMode()
}

/** A secondary result: what one xsl:result-document made (XSLT 4.0, 26.2). */
class ResultDocument internal constructor(
    /** Its href resolved against the base output URI. */
    val uri: String,
    /** [ResultDelivery.SERIALIZED]: the text. */
    val output: String?,
    /** [ResultDelivery.RAW] and [ResultDelivery.TREE]: the value, yours to
     *  close. */
    val value: Value?,
)

/** What a transformation made. Closing it closes its values. */
class TransformResult internal constructor(
    /** The principal result serialized ([ResultDelivery.SERIALIZED]). */
    val output: String?,
    /** The principal result as a value ([ResultDelivery.RAW],
     *  [ResultDelivery.TREE]). */
    val value: Value?,
    /** In the order they completed. */
    val resultDocuments: List<ResultDocument>,
) : AutoCloseable {
    override fun close() {
        value?.close()
        for (document in resultDocuments) {
            document.value?.close()
        }
    }
}

/**
 * A compiled XSLT 4.0 stylesheet (include/sxml/xslt.h), run many times.
 * [Processor.compileStylesheet] makes it.
 *
 * Golden file: the Stylesheet class of bindings/javascript/src/index.ts.
 */
class Stylesheet internal constructor(val processor: Processor, handle: Long) : AutoCloseable {
    private val native = NativeHandle(this, processor, HandleKind.STYLESHEET, handle, "stylesheet")

    /**
     * Runs one transformation (XSLT 4.0, 2.3): applies templates to the
     * source, or calls [initialTemplate] or [initialFunction]. Inputs are
     * those [Processor.value] takes.
     *
     * @param source the global context item (2.4.2): its document is the
     *   default initial match selection.
     * @param initialMatchSelection the initial match selection, when it is
     *   not the source.
     * @param initialMode default: the stylesheet's default mode.
     * @param initialTemplate calls this named template instead of applying
     *   templates (2.4.4), by NCName or Q{uri}local. Without source and
     *   initialMatchSelection, xsl:initial-template is called.
     * @param initialFunction calls this stylesheet function with [args]
     *   instead (2.4.5).
     * @param parameters the stylesheet parameters.
     * @param templateParameters parameters of the initial template or of
     *   the template rules.
     * @param baseOutputUri against which relative hrefs of
     *   xsl:result-document resolve.
     * @param delivery the principal result's delivery.
     * @param resultDocumentDelivery the result documents' delivery.
     * @param serialization serialization parameters replacing the unnamed
     *   xsl:output's, as fn:serialize takes them (keys may be camelCase).
     * @param messages receives xsl:message and xsl:assert output, after
     *   the transformation (also when it throws).
     */
    fun transform(
        source: Any? = null,
        initialMatchSelection: Any? = null,
        initialMode: InitialMode? = null,
        initialTemplate: String? = null,
        initialFunction: String? = null,
        args: List<Any?> = emptyList(),
        parameters: Map<String, Any?> = emptyMap(),
        templateParameters: Map<String, Any?> = emptyMap(),
        tunnelParameters: Map<String, Any?> = emptyMap(),
        baseOutputUri: String? = null,
        delivery: ResultDelivery = ResultDelivery.SERIALIZED,
        resultDocumentDelivery: ResultDelivery = ResultDelivery.SERIALIZED,
        serialization: Map<String, Any?>? = null,
        messages: ((Message) -> Unit)? = null,
    ): TransformResult {
        val state = processor.state
        var output: String? = null
        val outcome = synchronized(state) {
            state.enter()
            val address = native.address()
            val made = ArrayList<Value>()
            try {
                fun input(data: Any?): Long = if (data == null) 0 else processor.input(data, made).address()
                val argumentHandles = LongArray(args.size) { index -> processor.input(args[index], made).address() }
                val serializationOptions = processor.serializationOptions(serialization)?.also { made.add(it) }
                val invocationKind = when {
                    initialFunction != null -> INVOCATION_CALL_FUNCTION
                    initialTemplate != null || (source == null && initialMatchSelection == null) -> INVOCATION_CALL_TEMPLATE
                    else -> INVOCATION_APPLY_TEMPLATES
                }
                val names = ArrayList<ByteArray>()
                for (name in listOf((initialMode as? InitialMode.Named)?.name, initialTemplate, initialFunction)) {
                    val (namespaceUri, localName) = if (name == null) "" to "" else splitName(name)
                    names.add(namespaceUri.encodeToByteArray())
                    names.add(localName.encodeToByteArray())
                }
                val request = TransformRequest(
                    state.processor,
                    address,
                    input(source),
                    input(initialMatchSelection),
                    processor.bindings(parameters, made),
                    processor.bindings(templateParameters, made),
                    processor.bindings(tunnelParameters, made),
                    argumentHandles,
                    (baseOutputUri ?: "").encodeToByteArray(),
                    serializationOptions?.address() ?: 0,
                    names.toTypedArray(),
                    invocationKind,
                    delivery.number,
                    resultDocumentDelivery.number,
                    initialMode == InitialMode.Unnamed,
                )
                Native.transform(request)
                output = request.output?.decodeToString()
                processor.outcome(request)
            } finally {
                for (value in made) {
                    value.close()
                }
            }
        }
        deliverMessages(outcome.sideResults, messages)
        val resultDocuments = outcome.sideResults.filter { it.kind == SideResult.RESULT_DOCUMENT }.map { side ->
            ResultDocument(side.uri, if (side.first == null) side.text else null, side.first)
        }
        val value = outcome.value
        if (value == null) {
            for (document in resultDocuments) {
                document.value?.close()
            }
            throw outcome.failure!!
        }
        if (delivery == ResultDelivery.SERIALIZED) {
            value.close()
            return TransformResult(output, null, resultDocuments)
        }
        return TransformResult(null, value, resultDocuments)
    }

    /** Releases the stylesheet. Idempotent. */
    override fun close() = native.close()

    private companion object {
        /** sxml_xslt_invocation_kind. */
        const val INVOCATION_APPLY_TEMPLATES = 0
        const val INVOCATION_CALL_TEMPLATE = 1
        const val INVOCATION_CALL_FUNCTION = 2
    }
}
