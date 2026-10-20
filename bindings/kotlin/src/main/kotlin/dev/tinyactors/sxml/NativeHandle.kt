package dev.tinyactors.sxml

/**
 * A handle a processor made, held by the object the caller has (a value,
 * an expression, a query, a stylesheet or a schema). It is released once:
 * by close(), by closing its processor, or by the cleaner after the owner
 * is unreachable (D238). Every use holds the processor's lock (D35).
 */
internal class NativeHandle(owner: Any, val processor: Processor, kind: Int, handle: Long, private val what: String) {
    private var handle = handle
    private val registration: NativeCleaner.Registration

    init {
        // The action refers to the state, the kind and the handle, not to
        // the owner, which it must not keep reachable.
        val state = processor.state
        val releasedKind = kind
        val released = handle
        state.track(releasedKind, released)
        registration = NativeCleaner.register(owner) { synchronized(state) { state.releaseHandle(releasedKind, released) } }
    }

    /** The handle, checked usable; the caller holds the processor's lock. */
    fun address(): Long {
        check(handle != 0L && !processor.state.isClosed) { "sxml: this $what has been closed" }
        return handle
    }

    /** Releases the handle. Idempotent. */
    fun close() {
        synchronized(processor.state) { handle = 0 }
        registration.clean()
    }
}
