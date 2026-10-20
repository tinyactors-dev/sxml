#include <stddef.h>
#include <string.h>

/* Appends one byte; returns 0 when the buffer is full. */
static int buffer_append(char *bytes, size_t *byte_count, size_t capacity, char byte) {
    if (*byte_count == capacity) {
        return 0;
    }
    bytes[*byte_count] = byte;
    *byte_count += 1;
    return 1;
}

size_t buffer_copy(char *target, const char *source, size_t len) {
    memcpy(target, source, len);
    return len;
}

void buffer_clear(char *buf, size_t capacity) {
    memset(buf, 0, capacity);
}
