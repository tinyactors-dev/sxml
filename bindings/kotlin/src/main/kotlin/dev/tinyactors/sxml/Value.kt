package dev.tinyactors.sxml

/**
 * An XDM value: a sequence of items in its processor (a parsed document,
 * an evaluation's result). Yours to close; one that is never closed is
 * released after it is unreachable, and closing its processor releases it
 * too.
 */
class Value internal constructor(val processor: Processor, handle: Long) : AutoCloseable {
    private var handle = handle
    private val registration: NativeCleaner.Registration

    init {
        // The action refers to the state and the handle, not to this.
        val state = processor.state
        val released = handle
        state.track(HandleKind.VALUE, released)
        registration = NativeCleaner.register(this) { synchronized(state) { state.releaseHandle(HandleKind.VALUE, released) } }
    }

    /** The handle, checked usable; the caller holds the processor's lock. */
    internal fun address(): Long {
        check(handle != 0L && !processor.state.isClosed) { "sxml: this value has been closed" }
        return handle
    }

    /** Serializes the value as fn:serialize does with [options] (method
     *  xml unless they name another; keys may be camelCase). */
    fun serialize(options: Map<String, Any?> = emptyMap()): String = processor.serialize(this, options)

    /** The value by the adaptive method: items one per line, strings
     *  quoted, nodes as XML. */
    override fun toString(): String = processor.serialize(this)

    /** Releases the value. Idempotent. */
    override fun close() {
        synchronized(processor.state) { handle = 0 }
        registration.clean()
    }
}
