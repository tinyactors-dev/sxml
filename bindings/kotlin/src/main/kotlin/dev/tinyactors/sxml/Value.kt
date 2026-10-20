package dev.tinyactors.sxml

/**
 * An XDM value: a sequence of items in its processor (a parsed document,
 * an evaluation's result). Yours to close; one that is never closed is
 * released after it is unreachable, and closing its processor releases it
 * too.
 */
class Value internal constructor(val processor: Processor, handle: Long) : AutoCloseable {
    private val native = NativeHandle(this, processor, HandleKind.VALUE, handle, "value")

    /** The handle, checked usable; the caller holds the processor's lock. */
    internal fun address(): Long = native.address()

    /** Serializes the value as fn:serialize does with [options] (method
     *  xml unless they name another; keys may be camelCase). */
    fun serialize(options: Map<String, Any?> = emptyMap()): String = processor.serialize(this, options)

    /** The value by the adaptive method: items one per line, strings
     *  quoted, nodes as XML. */
    override fun toString(): String = processor.serialize(this)

    /** Releases the value. Idempotent. */
    override fun close() = native.close()
}
