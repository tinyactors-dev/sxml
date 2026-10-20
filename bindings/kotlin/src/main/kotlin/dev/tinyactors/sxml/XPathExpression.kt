package dev.tinyactors.sxml

/** A compiled XPath 4.0 expression (include/sxml/xpath.h), evaluated many
 *  times. [Processor.compile] makes it. */
class XPathExpression internal constructor(
    val processor: Processor,
    handle: Long,
    variables: List<String>,
) : AutoCloseable {
    /** The external variables, in the order compile was given them. */
    val variables: List<String> = variables.toList()
    private val native = NativeHandle(this, processor, HandleKind.EXPRESSION, handle, "expression")

    /**
     * Evaluates the expression; the result is a new value.
     *
     * @param context the context value, an input as [Processor.value]
     *   takes it; null for an absent focus.
     * @param variables values by variable name; a declared variable left
     *   out is unbound (XPDY0002), and null is the empty sequence.
     */
    fun evaluate(context: Any? = null, variables: Map<String, Any?> = emptyMap()): Value =
        synchronized(processor.state) {
            processor.state.enter()
            val address = native.address()
            val made = ArrayList<Value>()
            try {
                val contextHandle = if (context == null) 0 else processor.input(context, made).address()
                val values = LongArray(this.variables.size) { index ->
                    val name = this.variables[index]
                    if (variables.containsKey(name)) processor.input(variables[name], made).address() else 0
                }
                processor.evaluateHandle(address, contextHandle, values)
            } finally {
                for (value in made) {
                    value.close()
                }
            }
        }

    /** Releases the expression. Idempotent. */
    override fun close() = native.close()
}
