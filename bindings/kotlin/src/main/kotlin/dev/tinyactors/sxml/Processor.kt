package dev.tinyactors.sxml

/**
 * A processor (include/sxml/processor.h): its name pool, types and items.
 * Values and expressions belong to the processor that made them and hold
 * it strongly. Closing it closes everything it made; a processor that is
 * never closed is released after it is unreachable (D238).
 *
 * One thread at a time uses a processor (D35): every call holds its lock,
 * so threads that share one take turns; separate processors run in
 * parallel.
 *
 * @param resolver answers fn:doc, fn:unparsed-text and the other resource
 *   requests; without one, no resource is available.
 * @param features the library's optional features the processor may use
 *   ([Sxml.features]); null for all of them. An unknown name is
 *   [IllegalArgumentException].
 * @param collationData the DUCET of the uca-collation feature (D213).
 * @param failingAllocationNumber for tests: the allocation to fail with
 *   [Status.OUT_OF_MEMORY], counted from the processor's first (D23); 0
 *   fails none.
 */
class Processor(
    resolver: Resolver? = null,
    features: Set<String>? = null,
    collationData: CollationData = CollationData.Bundled,
    failingAllocationNumber: Int = 0,
) : AutoCloseable {
    internal val state: ProcessorState
    private val registration: NativeCleaner.Registration

    init {
        val selected = features ?: Sxml.features
        val featureMask = Sxml.featureMask(selected)
        val collationBytes: () -> ByteArray? = when (collationData) {
            CollationData.Bundled -> { -> Sxml.bundledCollationData }
            CollationData.None -> { -> null }
            is CollationData.Bytes -> {
                val bytes = collationData.bytes
                { -> bytes }
            }
        }
        state = ProcessorState(resolver, collationBytes, selected)
        val request = ProcessorRequest()
        request.failingAllocationNumber = failingAllocationNumber
        request.featureMask = featureMask
        request.state = state
        Native.processorCreate(request)
        if (request.processor == 0L) {
            throw request.error.toException()
        }
        state.processor = request.processor
        val processorState = state
        registration = NativeCleaner.register(this) { synchronized(processorState) { processorState.destroy() } }
    }

    /** Parses an XML document (UTF-8 text) and returns its document node. */
    fun parse(xml: String, uri: String? = null): Value = parse(xml.encodeToByteArray(), uri)

    /** Parses an XML document in the encoding it declares (XML 1.0, 4.3.3). */
    fun parse(xml: ByteArray, uri: String? = null): Value = synchronized(state) {
        state.enter()
        val request = TextRequest(state.processor, xml, (uri ?: "").encodeToByteArray())
        Native.parse(request)
        adopt(request)
    }

    /**
     * Compiles an XPath 4.0 expression, for evaluating it many times.
     *
     * @param variables the external variables, as NCNames or Q{uri}local;
     *   each has type item()*.
     * @param namespaces prefix to URI, added to the default bindings (and
     *   to the prefixes of the processor's features, which they may rebind).
     */
    fun compile(
        expression: String,
        variables: List<String> = emptyList(),
        namespaces: Map<String, String> = emptyMap(),
        baseUri: String? = null,
    ): XPathExpression = synchronized(state) {
        state.enter()
        val handle = compileHandle(expression, variables, namespaces, baseUri)
        XPathExpression(this, handle, variables)
    }

    /** Compiles and evaluates an expression once; the variables are the
     *  keys of [variables]. */
    fun evaluate(
        expression: String,
        context: Any? = null,
        variables: Map<String, Any?> = emptyMap(),
        namespaces: Map<String, String> = emptyMap(),
        baseUri: String? = null,
    ): Value = synchronized(state) {
        compile(expression, variables.keys.toList(), namespaces, baseUri).use { compiled ->
            compiled.evaluate(context, variables)
        }
    }

    /**
     * Converts host data to a value: a [Value] of this processor as it is,
     * a String as an xs:string, and JSON-shaped data (Number, Boolean,
     * null, List, Array, Map with String keys) as fn:parse-json reads it
     * (D138). Anything else is [IllegalArgumentException].
     */
    fun value(data: Any?): Value = synchronized(state) {
        state.enter()
        if (data is Value) {
            checkOwn(data)
            return data
        }
        if (data is String) {
            return string(data)
        }
        val json = string(jsonText(data))
        try {
            if (state.jsonExpression == 0L) {
                state.jsonExpression = compileHandle("parse-json(\$json)", listOf("json"), emptyMap(), null)
            }
            return evaluateHandle(state.jsonExpression, 0, longArrayOf(json.address()))
        } finally {
            json.close()
        }
    }

    /**
     * Serializes a value: by the adaptive method when [options] is null,
     * else as fn:serialize with them (method xml unless they name another;
     * keys may be camelCase, as `omitXmlDeclaration`).
     */
    fun serialize(value: Value, options: Map<String, Any?>? = null): String = synchronized(state) {
        state.enter()
        checkOwn(value)
        if (options == null) {
            return serializeWith(value, null)
        }
        val parameters = options.mapKeys { (key, _) -> key.replace(Regex("[A-Z]")) { "-" + it.value.lowercase() } }
        value(parameters).use { optionsValue -> serializeWith(value, optionsValue) }
    }

    /** The allocations the processor holds, for leak checks in tests; after
     *  close(), those left when it was destroyed (0 unless something
     *  leaked). */
    fun liveAllocationCount(): Int = synchronized(state) {
        if (state.isClosed) {
            return state.leakedAllocationCount
        }
        val request = ProcessorRequest(state.processor)
        Native.liveAllocationCount(request)
        request.allocationCount
    }

    /** Releases the processor and every value and expression made with it.
     *  Idempotent. */
    override fun close() {
        registration.clean()
    }

    // ------------------------------------------------- For the binding

    /** Throws unless value belongs to this processor. */
    internal fun checkOwn(value: Value) {
        require(value.processor === this) { "sxml: the value belongs to another processor" }
    }

    /** An input as a value of this processor; values it made are added to
     *  made, for the caller to close. */
    internal fun input(data: Any?, made: MutableList<Value>): Value {
        if (data is Value) {
            checkOwn(data)
            return data
        }
        val value = value(data)
        made.add(value)
        return value
    }

    /** The value a call made, or its failure thrown. */
    private fun adopt(request: NativeRequest): Value {
        if (request.handle == 0L) {
            throw state.failure(request)
        }
        return Value(this, request.handle)
    }

    private fun string(text: String): Value {
        val request = TextRequest(state.processor, text.encodeToByteArray(), EMPTY)
        Native.string(request)
        return adopt(request)
    }

    internal fun compileHandle(
        expression: String,
        variables: List<String>,
        namespaces: Map<String, String>,
        baseUri: String?,
    ): Long {
        val namespaceTexts = ArrayList<ByteArray>()
        for ((prefix, uri) in state.featureNamespaces + namespaces) {
            namespaceTexts.add(prefix.encodeToByteArray())
            namespaceTexts.add(uri.encodeToByteArray())
        }
        val variableTexts = ArrayList<ByteArray>()
        for (name in variables) {
            val (namespaceUri, localName) = splitName(name)
            variableTexts.add(namespaceUri.encodeToByteArray())
            variableTexts.add(localName.encodeToByteArray())
        }
        val request = CompileRequest(
            state.processor,
            expression.encodeToByteArray(),
            (baseUri ?: "").encodeToByteArray(),
            namespaceTexts.toTypedArray(),
            variableTexts.toTypedArray(),
        )
        Native.compile(request)
        if (request.handle == 0L) {
            throw state.failure(request)
        }
        return request.handle
    }

    internal fun evaluateHandle(expression: Long, context: Long, variableValues: LongArray): Value {
        state.enter()
        val request = EvaluateRequest(state.processor, expression, context, variableValues)
        Native.evaluate(request)
        return adopt(request)
    }

    private fun serializeWith(value: Value, options: Value?): String {
        state.enter()
        val request = SerializeRequest(state.processor, value.address(), options?.address() ?: 0)
        Native.serialize(request)
        val output = request.output ?: throw state.failure(request)
        return output.decodeToString()
    }

    private companion object {
        val EMPTY = ByteArray(0)
    }
}
