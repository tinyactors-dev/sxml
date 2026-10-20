package dev.tinyactors.sxml

import java.lang.ref.PhantomReference
import java.lang.ref.ReferenceQueue
import java.util.Collections
import java.util.IdentityHashMap
import java.util.concurrent.atomic.AtomicBoolean

/*
 * The JNI side of the binding (docs/GLOSSARY.md, "jni_glue" and
 * "native_request"; D238): the entry points of src/main/c/sxml_kotlin_jni.c
 * and the request objects they take. Every entry point takes one request,
 * reads its inputs and writes back what the call made (a handle, the
 * error, an output) into the same object. Text crosses as UTF-8 byte
 * arrays, never as jstring: JNI's modified UTF-8 differs from UTF-8 for
 * U+0000 and supplementary characters.
 *
 * The glue looks the fields up by these names when a processor is made
 * (sxml_kotlin_field_table), so a field renamed here must be renamed
 * there. All fields are @JvmField, so that JNI sees them as fields.
 */

/** The failure a call writes back: sxml_host_error (bindings/host). */
internal class NativeError {
    @JvmField var status: Int = 0
    @JvmField var codeNamespaceUri: ByteArray? = null
    @JvmField var codeLocalName: ByteArray? = null
    @JvmField var codeNamespacePrefix: ByteArray? = null
    @JvmField var description: ByteArray? = null
    @JvmField var sourceName: ByteArray? = null
    @JvmField var lineNumber: Int = 0
    @JvmField var columnNumber: Int = 0

    fun toException(): SxmlException {
        val namespaceUri = text(codeNamespaceUri)
        val localName = text(codeLocalName)
        val statusValue = Status.of(status)
        return SxmlException(
            status = statusValue,
            code = SxmlException.codeOf(namespaceUri, localName, text(codeNamespacePrefix)),
            codeNamespaceUri = namespaceUri,
            codeLocalName = localName,
            description = if (statusValue == Status.OUT_OF_MEMORY && description == null) "Out of memory" else text(description),
            sourceName = text(sourceName),
            lineNumber = lineNumber,
            columnNumber = columnNumber,
        )
    }

    private fun text(bytes: ByteArray?): String = bytes?.decodeToString() ?: ""
}

/** What every request has: the glue's processor, the handle the call
 *  made (0 when it failed), and its failure. */
internal open class NativeRequest(
    @JvmField var processor: Long = 0,
) {
    @JvmField var handle: Long = 0
    @JvmField val error = NativeError()
}

/** The library: its version and optional features (no processor). */
internal class LibraryRequest : NativeRequest() {
    @JvmField var version: ByteArray? = null
    @JvmField var features: Array<ByteArray>? = null
}

/** processorCreate, processorDestroy and liveAllocationCount. */
internal class ProcessorRequest(processor: Long = 0) : NativeRequest(processor) {
    @JvmField var failingAllocationNumber: Int = 0
    @JvmField var featureMask: Int = 0
    /** The state whose resolve method the glue calls. */
    @JvmField var state: ProcessorState? = null
    /** Written: the live (or leaked) allocation count. */
    @JvmField var allocationCount: Int = 0
}

/** parse (text the document, uri its URI) and string (text). */
internal class TextRequest(processor: Long, @JvmField val text: ByteArray, @JvmField val uri: ByteArray) :
    NativeRequest(processor)

/** compile. namespaces: prefix and URI, in pairs; variables: namespace
 *  URI and local name, in pairs. */
internal class CompileRequest(
    processor: Long,
    @JvmField val expressionText: ByteArray,
    @JvmField val baseUri: ByteArray,
    @JvmField val namespaces: Array<ByteArray>,
    @JvmField val variables: Array<ByteArray>,
) : NativeRequest(processor)

/** evaluate: value handles, 0 for an absent context or an unbound
 *  variable. */
internal class EvaluateRequest(
    processor: Long,
    @JvmField val expression: Long,
    @JvmField val context: Long,
    @JvmField val variableValues: LongArray,
) : NativeRequest(processor)

/** serialize: options 0 for the adaptive method. */
internal class SerializeRequest(processor: Long, @JvmField val value: Long, @JvmField val options: Long) :
    NativeRequest(processor) {
    @JvmField var output: ByteArray? = null
}

/** release: a handle of a kind (HandleKind). */
internal class ReleaseRequest(processor: Long, @JvmField val kind: Int, @JvmField val target: Long) :
    NativeRequest(processor)

/** The kinds of handle a processor makes (sxml_kotlin_handle_kind). */
internal object HandleKind {
    const val VALUE = 0
    const val EXPRESSION = 1
}

/** The JNI entry points (sxml_kotlin_jni.c). */
internal object Native {
    init {
        val path = System.getProperty("sxml.library.path")
        if (path != null) {
            System.load(path)
        } else {
            System.loadLibrary("sxml_kotlin")
        }
    }

    @JvmStatic external fun library(request: LibraryRequest)

    @JvmStatic external fun processorCreate(request: ProcessorRequest)

    @JvmStatic external fun processorDestroy(request: ProcessorRequest)

    @JvmStatic external fun liveAllocationCount(request: ProcessorRequest)

    @JvmStatic external fun parse(request: TextRequest)

    @JvmStatic external fun string(request: TextRequest)

    @JvmStatic external fun compile(request: CompileRequest)

    @JvmStatic external fun evaluate(request: EvaluateRequest)

    @JvmStatic external fun serialize(request: SerializeRequest)

    @JvmStatic external fun release(request: ReleaseRequest)
}

/**
 * The backstop for objects that are never closed, as the JavaScript
 * package's FinalizationRegistry (D238). It is what java.lang.ref.Cleaner
 * does, written out over a PhantomReference and a daemon thread, because
 * Android has Cleaner only from API 33 and the binding's minSdk is 24
 * (D247). An action runs once: on [Registration.clean] (close) or after
 * its owner became unreachable.
 */
internal object NativeCleaner {
    private val queue = ReferenceQueue<Any>()
    private val registrations: MutableSet<Registration> = Collections.synchronizedSet(
        Collections.newSetFromMap(IdentityHashMap()),
    )

    class Registration(owner: Any, private val action: Runnable) : PhantomReference<Any>(owner, queue) {
        private val isDone = AtomicBoolean(false)

        fun clean() {
            if (isDone.compareAndSet(false, true)) {
                registrations.remove(this)
                clear()
                action.run()
            }
        }
    }

    init {
        val thread = Thread({
            while (true) {
                try {
                    (queue.remove() as Registration).clean()
                } catch (_: Throwable) {
                    // An action that fails must not stop the others.
                }
            }
        }, "sxml-cleaner")
        thread.isDaemon = true
        thread.start()
    }

    /** Runs action once, at the latest after owner is unreachable. The
     *  action must not refer to owner. */
    fun register(owner: Any, action: Runnable): Registration {
        val registration = Registration(owner, action)
        registrations.add(registration)
        return registration
    }
}
