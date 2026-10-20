package dev.tinyactors.sxml

/*
 * Host data as JSON text for fn:parse-json (D138: one rule for every
 * binding). Numbers, booleans, null (the empty sequence), lists, arrays
 * and maps with string keys are JSON-shaped; anything else is
 * IllegalArgumentException.
 */

/** Writes data as JSON text. */
internal fun jsonText(data: Any?): String {
    val text = StringBuilder()
    writeJson(text, data)
    return text.toString()
}

private fun writeJson(text: StringBuilder, data: Any?) {
    when (data) {
        null -> text.append("null")
        is Boolean -> text.append(data)
        is Double -> writeNumber(text, data.isFinite(), data)
        is Float -> writeNumber(text, data.isFinite(), data)
        is Number -> text.append(data.toString())
        is String -> writeString(text, data)
        is Map<*, *> -> {
            text.append('{')
            var isFirst = true
            for ((key, member) in data) {
                require(key is String) { "sxml: a map key must be a string, not ${key?.javaClass?.name}" }
                if (!isFirst) {
                    text.append(',')
                }
                isFirst = false
                writeString(text, key)
                text.append(':')
                writeJson(text, member)
            }
            text.append('}')
        }
        is List<*> -> writeArray(text, data)
        is Array<*> -> writeArray(text, data.asList())
        else -> throw IllegalArgumentException("sxml: cannot convert ${data.javaClass.name} to a value")
    }
}

private fun writeNumber(text: StringBuilder, isFinite: Boolean, number: Number) {
    require(isFinite) { "sxml: JSON has no number $number" }
    text.append(number.toString())
}

private fun writeArray(text: StringBuilder, members: List<*>) {
    text.append('[')
    for ((index, member) in members.withIndex()) {
        if (index > 0) {
            text.append(',')
        }
        writeJson(text, member)
    }
    text.append(']')
}

/** A JSON string (RFC 8259, 7): quotes, backslashes and controls
 *  escaped. */
private fun writeString(text: StringBuilder, string: String) {
    text.append('"')
    for (character in string) {
        when {
            character == '"' -> text.append("\\\"")
            character == '\\' -> text.append("\\\\")
            character < ' ' -> text.append("\\u").append(character.code.toString(16).padStart(4, '0'))
            else -> text.append(character)
        }
    }
    text.append('"')
}

/** Splits an NCName or Q{uri}local into namespace URI and local name. */
internal fun splitName(name: String): Pair<String, String> {
    val match = Regex("^Q\\{([^{}]*)\\}(.+)$").find(name) ?: return "" to name
    return match.groupValues[1] to match.groupValues[2]
}
