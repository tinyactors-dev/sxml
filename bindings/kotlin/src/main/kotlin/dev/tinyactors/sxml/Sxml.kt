package dev.tinyactors.sxml

/**
 * The library: `libsxml_kotlin.so`, the sxml library with every optional
 * feature but EXPath File, the host shim and the JNI glue (D238). It
 * loads with the first use of the binding (System.loadLibrary, or
 * System.load of the system property `sxml.library.path`).
 */
object Sxml {
    /** The library version, such as "0.2.0". */
    val version: String

    /** The optional features the library has, by name, in the order of
     *  sxml_all_optional_features() (D189). */
    val features: List<String>

    init {
        val request = LibraryRequest()
        Native.library(request)
        version = request.version?.decodeToString() ?: ""
        features = request.features?.map { it.decodeToString() } ?: emptyList()
    }

    /** The bit mask of the named features for the host shim. */
    internal fun featureMask(names: Collection<String>): Int {
        var mask = 0
        for (name in names) {
            val index = features.indexOf(name)
            require(index >= 0) { "sxml: this library has no optional feature '$name'" }
            mask = mask or (1 shl index)
        }
        return mask
    }

    /** The DUCET shipped beside the library, read once. */
    internal val bundledCollationData: ByteArray? by lazy {
        Sxml::class.java.getResourceAsStream("allkeys.txt")?.use { it.readBytes() }
    }
}
