package dev.tinyactors.sxml

/**
 * The status of a failed call: the names of `sxml_status`
 * (include/sxml/status.h), in its order, without the `SXML_STATUS_` prefix.
 */
enum class Status {
    OK,
    INVALID_ARGUMENT,
    INSUFFICIENT_MEMORY,
    MISALIGNED_MEMORY,
    CAPACITY_EXCEEDED,
    LIMIT_EXCEEDED,
    OUT_OF_MEMORY,
    ARITHMETIC_OVERFLOW,
    INDEX_OUT_OF_RANGE,
    UNEXPECTED_END_OF_INPUT,
    MALFORMED_INPUT,
    INVALID_UTF8,
    UNSUPPORTED_FEATURE,
    INPUT_OUTPUT_FAILED,
    STATIC_ERROR,
    DYNAMIC_ERROR,
    TYPE_ERROR,
    ;

    internal companion object {
        /** The status with the number `sxml_status` gives it. */
        fun of(number: Int): Status = entries.getOrElse(number) { INVALID_ARGUMENT }
    }
}
