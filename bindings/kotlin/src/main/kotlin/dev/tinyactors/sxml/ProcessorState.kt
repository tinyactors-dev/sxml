package dev.tinyactors.sxml

/**
 * A processor's native side and the handles made with it, kept apart from
 * the [Processor] the caller holds, so that the cleaner actions can refer
 * to it without keeping the caller's objects reachable.
 *
 * Every use holds its monitor (`synchronized(state)`, D35: one thread at a
 * time per processor), cleaner actions included. The monitor is
 * reentrant, so the resolver, which runs inside a call on the calling
 * thread, reaches [enter] and is refused there.
 */
internal class ProcessorState(
    private val resolver: Resolver?,
    private val collationData: () -> ByteArray?,
    features: Collection<String>,
) {
    /** The glue's processor (sxml_kotlin_processor); 0 once destroyed. */
    var processor: Long = 0

    /** Handles not yet released: handle to HandleKind. */
    private val live = LinkedHashMap<Long, Int>()

    /** What the resolver threw during the current call. */
    private var pendingException: Throwable? = null
    private var isInResolver = false

    /** The answer of the last resolve, read and cleared by the glue. */
    @JvmField var resolverAnswer: ByteArray? = null

    /** parse-json(${'$'}json), compiled when first needed. */
    var jsonExpression: Long = 0

    /** What processorDestroy found still allocated. */
    var leakedAllocationCount = 0

    /** The conventional prefixes of the features' namespaces, as the sxml
     *  command binds them (EXPath Binary 4.0, 1.5; EXPath File 4.0, 1.7;
     *  D235). */
    val featureNamespaces: Map<String, String> = buildMap {
        for ((feature, prefix, uri) in featurePrefixes) {
            if (feature in features) {
                put(prefix, uri)
            }
        }
    }

    val isClosed: Boolean get() = processor == 0L

    /** Checks the processor usable at the start of a call. */
    fun enter() {
        check(processor != 0L) { "sxml: this processor has been closed" }
        check(!isInResolver) { "sxml: a resolver must not use its processor" }
        pendingException = null
    }

    fun track(kind: Int, handle: Long) {
        live[handle] = kind
    }

    /** Releases a handle unless the processor released it already. */
    fun releaseHandle(kind: Int, handle: Long) {
        if (processor == 0L || live.remove(handle) == null) {
            return
        }
        Native.release(ReleaseRequest(processor, kind, handle))
    }

    /** Releases every handle, then the processor. */
    fun destroy() {
        if (processor == 0L) {
            return
        }
        for ((handle, kind) in live) {
            Native.release(ReleaseRequest(processor, kind, handle))
        }
        live.clear()
        if (jsonExpression != 0L) {
            Native.release(ReleaseRequest(processor, HandleKind.EXPRESSION, jsonExpression))
            jsonExpression = 0
        }
        val request = ProcessorRequest(processor)
        Native.processorDestroy(request)
        leakedAllocationCount = request.allocationCount
        processor = 0
    }

    /** Whether a call failed: the resolver threw, or the call wrote an
     *  error (a validation that finds a node invalid writes none). */
    fun hasFailed(request: NativeRequest): Boolean = pendingException != null || request.error.status != 0

    /** The failure of a call: what the resolver threw, else the error the
     *  call wrote. */
    fun failure(request: NativeRequest): Throwable {
        val exception = pendingException
        if (exception != null) {
            pendingException = null
            return exception
        }
        return request.error.toException()
    }

    /**
     * The resolver bridge (D238), called by the glue during a call. Asks
     * the resolver, keeps its answer in [resolverAnswer] and returns
     * SXML_STATUS_OK; keeps what it throws and returns
     * SXML_STATUS_INPUT_OUTPUT_FAILED, so that the call rethrows it and the
     * glue never sees a pending exception.
     */
    @Suppress("unused") // Called from sxml_kotlin_jni.c.
    fun resolve(kind: Int, uriReference: ByteArray, absoluteUri: ByteArray): Int {
        resolverAnswer = null
        val request = ResourceRequest(ResourceKind.of(kind), uriReference.decodeToString(), absoluteUri.decodeToString())
        var answer: ByteArray? = null
        if (resolver != null) {
            isInResolver = true
            try {
                answer = when (val resource = resolver.resolve(request)) {
                    null -> null
                    is Resource.Text -> resource.text.encodeToByteArray()
                    is Resource.Bytes -> resource.bytes
                }
            } catch (exception: Throwable) {
                pendingException = exception
                return STATUS_INPUT_OUTPUT_FAILED
            } finally {
                isInResolver = false
            }
        }
        if (answer == null && request.kind == ResourceKind.COLLATION_DATA && request.absoluteUri == DUCET_URI) {
            answer = collationData()
        }
        resolverAnswer = answer
        return STATUS_OK
    }

    companion object {
        private const val STATUS_OK = 0
        private const val STATUS_INPUT_OUTPUT_FAILED = 13

        /** The URI the uca-collation feature asks for its DUCET by. */
        private const val DUCET_URI = "http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt"

        private val featurePrefixes = listOf(
            Triple("expath-binary", "bin", "http://expath.org/ns/binary"),
            Triple("expath-file", "file", "http://expath.org/ns/file"),
        )
    }
}
