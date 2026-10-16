/*
 * Status names and error record classification (include/sxml/status.h).
 */
#include "sxml/status.h"

sxml_byte_view sxml_status_name(sxml_status status) {
    /* A switch, not a table indexed by status, so that -Wswitch-enum
       reports a status added without a name. */
    switch (status) {
    case SXML_STATUS_OK:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_OK");
    case SXML_STATUS_INVALID_ARGUMENT:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_INVALID_ARGUMENT");
    case SXML_STATUS_INSUFFICIENT_MEMORY:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_INSUFFICIENT_MEMORY");
    case SXML_STATUS_MISALIGNED_MEMORY:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_MISALIGNED_MEMORY");
    case SXML_STATUS_CAPACITY_EXCEEDED:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_CAPACITY_EXCEEDED");
    case SXML_STATUS_LIMIT_EXCEEDED:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_LIMIT_EXCEEDED");
    case SXML_STATUS_OUT_OF_MEMORY:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_OUT_OF_MEMORY");
    case SXML_STATUS_ARITHMETIC_OVERFLOW:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_ARITHMETIC_OVERFLOW");
    case SXML_STATUS_INDEX_OUT_OF_RANGE:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_INDEX_OUT_OF_RANGE");
    case SXML_STATUS_UNEXPECTED_END_OF_INPUT:
        return SXML_BYTE_VIEW_FROM_LITERAL(
            "SXML_STATUS_UNEXPECTED_END_OF_INPUT");
    case SXML_STATUS_MALFORMED_INPUT:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_MALFORMED_INPUT");
    case SXML_STATUS_INVALID_UTF8:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_INVALID_UTF8");
    case SXML_STATUS_UNSUPPORTED_FEATURE:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_UNSUPPORTED_FEATURE");
    case SXML_STATUS_STATIC_ERROR:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_STATIC_ERROR");
    case SXML_STATUS_DYNAMIC_ERROR:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_DYNAMIC_ERROR");
    case SXML_STATUS_TYPE_ERROR:
        return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_TYPE_ERROR");
    }
    /* Reached only for a value cast from outside the enumeration. */
    return SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_UNKNOWN");
}

bool sxml_status_has_error_record(sxml_status status) {
    return status == SXML_STATUS_STATIC_ERROR ||
           status == SXML_STATUS_DYNAMIC_ERROR ||
           status == SXML_STATUS_TYPE_ERROR;
}
