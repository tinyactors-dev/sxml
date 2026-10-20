package dev.tinyactors.sxml

/**
 * A processor (include/sxml/processor.h): its name pool, types and items.
 * Values, expressions, queries, stylesheets and schemas belong to the
 * processor that made them and hold it strongly. Closing it closes
 * everything it made; a processor that is never closed is released after
 * it is unreachable (D238).
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
     * Compiles an XQuery 4.0 main module, with the library modules it
     * imports ([ResourceKind.XQUERY_MODULE]), for evaluating it many times.
     *
     * @param namespaces prefix to URI, added to XQuery's predeclared
     *   prefixes (and to those of the processor's features); the prolog may
     *   rebind them.
     * @param baseUri the static base URI, against which module locations
     *   and fn:doc resolve.
     */
    fun compileQuery(
        query: String,
        namespaces: Map<String, String> = emptyMap(),
        baseUri: String? = null,
    ): XQuery = synchronized(state) {
        state.enter()
        val request = CompileRequest(
            state.processor,
            query.encodeToByteArray(),
            (baseUri ?: "").encodeToByteArray(),
            namespaceTexts(namespaces),
            emptyArray(),
        )
        Native.queryCompile(request)
        if (request.handle == 0L) {
            throw state.failure(request)
        }
        XQuery(this, request.handle, request.isUpdating)
    }

    /**
     * Compiles an XSLT 4.0 stylesheet (UTF-8 text), with the modules it
     * includes and imports ([ResourceKind.XSLT_MODULE]) and the packages it
     * uses ([ResourceKind.XSLT_PACKAGE]), for running it many times.
     *
     * @param baseUri the principal module's URI, against which xsl:include,
     *   xsl:import and fn:doc resolve.
     * @param staticParameters the static parameters (XSLT 4.0, 9.6), by
     *   NCName or Q{uri}local; inputs as [value] takes them.
     */
    fun compileStylesheet(
        stylesheet: String,
        baseUri: String? = null,
        staticParameters: Map<String, Any?> = emptyMap(),
    ): Stylesheet = compileStylesheet(stylesheet.encodeToByteArray(), baseUri, staticParameters)

    /** Compiles a stylesheet in the encoding it declares, as the text
     *  overload does. */
    fun compileStylesheet(
        stylesheet: ByteArray,
        baseUri: String? = null,
        staticParameters: Map<String, Any?> = emptyMap(),
    ): Stylesheet = synchronized(state) {
        state.enter()
        val made = ArrayList<Value>()
        try {
            val request = StylesheetCompileRequest(
                state.processor,
                stylesheet,
                (baseUri ?: "").encodeToByteArray(),
                bindings(staticParameters, made),
            )
            Native.stylesheetCompile(request)
            if (request.handle == 0L) {
                throw state.failure(request)
            }
            Stylesheet(this, request.handle)
        } finally {
            for (value in made) {
                value.close()
            }
        }
    }

    /**
     * Compiles an XSD 1.1 schema from its documents, with every document
     * they include, import, redefine or override
     * ([ResourceKind.SCHEMA_DOCUMENT]). Throws a [SchemaException] with
     * every report when the schema has errors.
     *
     * @param baseUri against which the URIs of [SchemaDocument.Resolved]
     *   resolve.
     */
    fun compileSchema(documents: List<SchemaDocument>, baseUri: String? = null): Schema = synchronized(state) {
        state.enter()
        val texts = ArrayList<ByteArray?>()
        for (document in documents) {
            when (document) {
                is SchemaDocument.Text -> {
                    texts.add(document.bytes)
                    texts.add((document.uri ?: "").encodeToByteArray())
                }
                is SchemaDocument.Resolved -> {
                    texts.add(null)
                    texts.add(document.uri.encodeToByteArray())
                }
            }
        }
        val request = SchemaCompileRequest(state.processor, texts.toTypedArray(), (baseUri ?: "").encodeToByteArray())
        Native.schemaCompile(request)
        val reports = schemaReports(adoptSideResults(request.sideResults))
        if (request.handle == 0L) {
            val failure = state.failure(request)
            throw if (failure is SxmlException) SchemaException(failure, reports) else failure
        }
        Schema(this, request.handle)
    }

    /** Compiles a schema of one document. */
    fun compileSchema(document: SchemaDocument, baseUri: String? = null): Schema =
        compileSchema(listOf(document), baseUri)

    /** Compiles a schema of one document's text, without a URI. */
    fun compileSchema(text: String, baseUri: String? = null): Schema =
        compileSchema(listOf(SchemaDocument.Text(text)), baseUri)

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
        val optionsValue = serializationOptions(options) ?: return serializeWith(value, null)
        optionsValue.use { serializeWith(value, it) }
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

    /** Releases the processor and everything made with it: values,
     *  expressions, queries, stylesheets and schemas. Idempotent. */
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

    /** options as the map fn:serialize takes, keys in kebab case; null for
     *  none. The caller holds the lock and closes the value. */
    internal fun serializationOptions(options: Map<String, Any?>?): Value? {
        if (options == null) {
            return null
        }
        val parameters = options.mapKeys { (key, _) -> key.replace(Regex("[A-Z]")) { "-" + it.value.lowercase() } }
        return value(parameters)
    }

    /** Serializes value with a map of parameters this processor made. */
    internal fun serializeWithValue(value: Value, options: Value): String = synchronized(state) {
        state.enter()
        checkOwn(value)
        checkOwn(options)
        serializeWith(value, options)
    }

    /** Names bound to inputs, as the glue takes them; values it made are
     *  added to made. */
    internal fun bindings(bindings: Map<String, Any?>, made: MutableList<Value>): NativeBindings {
        val names = ArrayList<ByteArray>()
        val values = LongArray(bindings.size)
        for ((index, entry) in bindings.entries.withIndex()) {
            val (namespaceUri, localName) = splitName(entry.key)
            names.add(namespaceUri.encodeToByteArray())
            names.add(localName.encodeToByteArray())
            values[index] = input(entry.value, made).address()
        }
        return NativeBindings(names.toTypedArray(), values)
    }

    /** The side results a call wrote, every value adopted. */
    internal fun adoptSideResults(results: NativeSideResults): List<SideResult> {
        val kinds = results.kinds ?: return emptyList()
        val texts = results.texts!!
        val values = results.values!!
        val numbers = results.numbers!!
        return kinds.indices.map { index ->
            SideResult(
                kind = kinds[index],
                uri = texts[3 * index].decodeToString(),
                text = texts[3 * index + 1].decodeToString(),
                code = texts[3 * index + 2].decodeToString(),
                first = adoptOptional(values[2 * index]),
                second = adoptOptional(values[2 * index + 1]),
                lineNumber = numbers[3 * index],
                columnNumber = numbers[3 * index + 1],
                isTerminating = numbers[3 * index + 2] != 0,
            )
        }
    }

    /** A value handle a call made, adopted; null for 0. */
    internal fun adoptOptional(handle: Long): Value? = if (handle == 0L) null else Value(this, handle)

    /** The value or failure of a call that writes side results, and the
     *  side results, adopted. */
    internal fun outcome(request: SideResultRequest): CallOutcome {
        val failure = if (request.handle == 0L) state.failure(request) else null
        val sideResults = adoptSideResults(request.sideResults)
        return CallOutcome(adoptOptional(request.handle), failure, sideResults)
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
            namespaceTexts(namespaces),
            variableTexts.toTypedArray(),
        )
        Native.compile(request)
        if (request.handle == 0L) {
            throw state.failure(request)
        }
        return request.handle
    }

    /** The namespaces of a compilation, prefix and URI in pairs: the
     *  features' prefixes, then the caller's, which may rebind them. */
    private fun namespaceTexts(namespaces: Map<String, String>): Array<ByteArray> {
        val texts = ArrayList<ByteArray>()
        for ((prefix, uri) in state.featureNamespaces + namespaces) {
            texts.add(prefix.encodeToByteArray())
            texts.add(uri.encodeToByteArray())
        }
        return texts.toTypedArray()
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
