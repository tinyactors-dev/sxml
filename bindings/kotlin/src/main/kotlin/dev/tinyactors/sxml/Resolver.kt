package dev.tinyactors.sxml

/**
 * What a resource request is for: `sxml_resource_kind`
 * (include/sxml/processor.h), in its order. The JavaScript package's
 * `ResourceKind` names the same kinds (`'xml-document'` is [XML_DOCUMENT]).
 */
enum class ResourceKind {
    XML_DOCUMENT,
    TEXT,
    BINARY,
    COLLECTION,
    URI_COLLECTION,
    XQUERY_MODULE,
    XSLT_MODULE,
    XSLT_PACKAGE,
    SCHEMA_DOCUMENT,
    EXTERNAL_ENTITY,
    TIMEZONE_DATA,
    COLLATION_DATA,
    LOCALE_DATA,
    FULL_TEXT_STOP_WORDS,
    FULL_TEXT_THESAURUS,
    ;

    internal companion object {
        fun of(number: Int): ResourceKind = entries.getOrElse(number) { XML_DOCUMENT }
    }
}

/** A resource sxml asks for: fn:doc, fn:unparsed-text, a module, data. */
data class ResourceRequest(
    val kind: ResourceKind,
    /** The URI reference as written, before resolution. */
    val uriReference: String,
    /** The reference resolved against the base URI. */
    val absoluteUri: String,
)

/** A resolver's answer: the resource's text (written as UTF-8) or bytes. */
sealed class Resource {
    class Text(val text: String) : Resource()

    class Bytes(val bytes: ByteArray) : Resource()
}

/**
 * Answers resource requests: a [Resource], or null when it has none.
 *
 * It runs during a call on its processor, on the calling thread, and must
 * not use that processor (D71); doing so is [IllegalStateException]. What
 * it throws stops the call, which rethrows the same exception.
 */
fun interface Resolver {
    fun resolve(request: ResourceRequest): Resource?
}

/**
 * The DUCET the uca-collation feature reads (D213), asked for as
 * [ResourceKind.COLLATION_DATA] when the resolver has no answer.
 */
sealed class CollationData {
    /** The copy shipped beside the library: the class path resource
     *  `dev/tinyactors/sxml/allkeys.txt`. Without it, UCA collations fall
     *  back as the feature says. */
    data object Bundled : CollationData()

    /** None: UCA collation URIs fall back. */
    data object None : CollationData()

    /** The DUCET's text (allkeys.txt) as bytes. */
    class Bytes(val bytes: ByteArray) : CollationData()
}
