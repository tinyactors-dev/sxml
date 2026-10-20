package dev.tinyactors.sxml

/**
 * A compiled XQuery 4.0 main module (include/sxml/xquery.h), evaluated
 * many times. [Processor.compileQuery] makes it.
 *
 * Golden file: the XQuery class of bindings/javascript/src/index.ts.
 */
class XQuery internal constructor(
    val processor: Processor,
    handle: Long,
    /** Whether its body is an updating expression (XQUF 3.0, 2.2): its
     *  result is then empty and its changes go to the update sink. */
    val isUpdating: Boolean,
) : AutoCloseable {
    private val native = NativeHandle(this, processor, HandleKind.QUERY, handle, "query")

    /**
     * Evaluates the query; the result is a new value.
     *
     * @param context the initial context value, an input as
     *   [Processor.value] takes it; null for none (XPDY0002 when the query
     *   needs it).
     * @param variables the external variables the prolog declares (XQuery
     *   4.0, 5.16), by NCName or Q{uri}local; names it does not declare
     *   are ignored, and a declared one without a value takes its default.
     * @param updates receives the trees an updating query changed and the
     *   documents fn:put stores, after the evaluation. Without it the trees
     *   are closed at once; without its putDocument, fn:put raises
     *   FOUP0002.
     * @param messages receives fn:trace and fn:message output, after the
     *   evaluation (also when it throws).
     */
    fun evaluate(
        context: Any? = null,
        variables: Map<String, Any?> = emptyMap(),
        updates: UpdateSink? = null,
        messages: ((Message) -> Unit)? = null,
    ): Value {
        val state = processor.state
        val outcome = synchronized(state) {
            state.enter()
            val address = native.address()
            val made = ArrayList<Value>()
            try {
                val contextHandle = if (context == null) 0 else processor.input(context, made).address()
                val request = QueryEvaluateRequest(
                    state.processor,
                    address,
                    contextHandle,
                    processor.bindings(variables, made),
                    updates?.putDocument != null,
                )
                Native.queryEvaluate(request)
                processor.outcome(request)
            } finally {
                for (value in made) {
                    value.close()
                }
            }
        }
        deliverMessages(outcome.sideResults, messages)
        deliverUpdates(outcome.sideResults, updates)
        return outcome.value ?: throw outcome.failure!!
    }

    /** Serializes a value with the serialization parameters the query's
     *  output declarations give (XQuery 4.0, 5.22: method xml unless they
     *  name another). [Value.serialize] uses options instead. */
    fun serialize(value: Value): String = synchronized(processor.state) {
        val state = processor.state
        state.enter()
        processor.checkOwn(value)
        val request = QuerySerializeRequest(state.processor, native.address(), value.address())
        Native.querySerialize(request)
        val output = request.output ?: throw state.failure(request)
        output.decodeToString()
    }

    /** Evaluates the query and serializes its result as [serialize] does. */
    fun run(
        context: Any? = null,
        variables: Map<String, Any?> = emptyMap(),
        updates: UpdateSink? = null,
        messages: ((Message) -> Unit)? = null,
    ): String = evaluate(context, variables, updates, messages).use { result -> serialize(result) }

    /** Releases the query. Idempotent. */
    override fun close() = native.close()
}

/**
 * Receives what an updating query changed, after the evaluation and in the
 * order of each tree's first update; documents fn:put stores come last.
 * Without [putDocument], fn:put raises FOUP0002 (D216). What it receives
 * is its to close.
 */
class UpdateSink(
    val updatedTree: ((UpdatedTree) -> Unit)? = null,
    val putDocument: ((PutDocument) -> Unit)? = null,
)

/** A tree an updating query changed (XQuery Update Facility 3.0,
 *  upd:applyUpdates): trees never change in sxml, so the update makes a
 *  new tree; putting it where the original was is the host's. */
class UpdatedTree internal constructor(
    /** The root as the query read it: a document node you bound, or a
     *  parentless node. */
    val original: Value,
    /** The root of the new tree with every change made. */
    val updated: Value,
) : AutoCloseable {
    override fun close() {
        original.close()
        updated.close()
    }
}

/** A document fn:put stores (XQUF 3.0, 6.1): writing it at [uri] is the
 *  host's. */
class PutDocument internal constructor(
    /** fn:put's $uri resolved against the static base URI. */
    val uri: String,
    val node: Value,
    /** fn:put's $params, or null. */
    val parameters: Value?,
) : AutoCloseable {
    /** The node serialized with [parameters] (method xml without). */
    fun serialize(): String =
        if (parameters != null) node.processor.serializeWithValue(node, parameters) else node.serialize()

    override fun close() {
        node.close()
        parameters?.close()
    }
}

/** Hands UPDATED_TREE and PUT_DOCUMENT side results to the sink; closes
 *  what it does not take. */
private fun deliverUpdates(results: List<SideResult>, sink: UpdateSink?) {
    for (result in results) {
        val original = result.first
        val second = result.second
        val receiveTree = sink?.updatedTree
        val receiveDocument = sink?.putDocument
        when {
            result.kind == SideResult.UPDATED_TREE && original != null && second != null && receiveTree != null ->
                receiveTree(UpdatedTree(original, second))
            result.kind == SideResult.PUT_DOCUMENT && original != null && receiveDocument != null ->
                receiveDocument(PutDocument(result.uri, original, second))
            result.kind == SideResult.UPDATED_TREE || result.kind == SideResult.PUT_DOCUMENT -> result.closeValues()
        }
    }
}
