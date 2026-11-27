/*
 * Deflate (RFC 1951; src/deflate_internal.h; D333). Packet 8.7 writes it.
 */
#include "deflate_internal.h"

sxml_status sxml_deflate(const sxml_deflate_request *request, sxml_memory_view *output) {
    (void)output;
    if (request == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}
