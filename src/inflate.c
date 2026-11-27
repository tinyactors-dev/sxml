/*
 * Inflate (RFC 1951; src/deflate_internal.h; D333). Packet 8.7 writes it.
 */
#include "deflate_internal.h"

sxml_status sxml_inflate(const sxml_inflate_request *request, sxml_inflate_result *result) {
    (void)result;
    if (request == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}
