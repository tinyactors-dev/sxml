/*
 * Sorting an array in place (src/sort_internal.h; packet 3.15, D161): the
 * heapsort that the XQuery prolog, the attributes of a direct element
 * constructor, the order of global initializers and XSLT's character maps
 * had each written for their own element type.
 *
 * Golden file: src/hash.c (a small internal module of plain functions).
 */
#include "sort_internal.h"

#include <stdint.h>
#include <string.h>

/* Bytes swapped at a time. */
#define SXML_SORT_SWAP_CHUNK_BYTE_COUNT ((size_t)64)

/* The element at position. */
static uint8_t *element_at(const sxml_sort_request *request, size_t position) {
    return (uint8_t *)request->elements + (position * request->element_size);
}

static bool element_is_before(const sxml_sort_request *request, size_t first,
                              size_t second) {
    return request->is_before(request->state,
                              (sxml_sort_pair){.first = element_at(request, first),
                                               .second = element_at(request, second)});
}

static void swap_elements(const sxml_sort_request *request, size_t first, size_t second) {
    uint8_t *first_bytes = element_at(request, first);
    uint8_t *second_bytes = element_at(request, second);
    uint8_t chunk[SXML_SORT_SWAP_CHUNK_BYTE_COUNT];
    size_t offset = 0;
    while (offset < request->element_size) {
        size_t chunk_byte_count = request->element_size - offset;
        if (chunk_byte_count > sizeof(chunk)) {
            chunk_byte_count = sizeof(chunk);
        }
        memcpy(chunk, first_bytes + offset, chunk_byte_count);
        memcpy(first_bytes + offset, second_bytes + offset, chunk_byte_count);
        memcpy(second_bytes + offset, chunk, chunk_byte_count);
        offset += chunk_byte_count;
    }
}

/* The bounds of a heap: its root and how many elements it has. */
typedef struct heap_bounds {
    size_t root;
    size_t element_count;
} heap_bounds;

/* Moves the element at heap.root down the heap of the first
   heap.element_count elements. */
static void sift_down(const sxml_sort_request *request, heap_bounds heap) {
    size_t parent = heap.root;
    for (;;) {
        size_t largest = parent;
        const size_t left = (2 * parent) + 1;
        const size_t right = left + 1;
        if (left < heap.element_count && element_is_before(request, largest, left)) {
            largest = left;
        }
        if (right < heap.element_count && element_is_before(request, largest, right)) {
            largest = right;
        }
        if (largest == parent) {
            return;
        }
        swap_elements(request, parent, largest);
        parent = largest;
    }
}

void sxml_sort_in_place(const sxml_sort_request *request) {
    const size_t count = request->element_count;
    if (count < 2 || request->element_size == 0) {
        return;
    }
    for (size_t heap_root = count / 2; heap_root > 0; heap_root -= 1) {
        sift_down(request, (heap_bounds){.root = heap_root - 1, .element_count = count});
    }
    for (size_t heap_end = count; heap_end > 1; heap_end -= 1) {
        swap_elements(request, 0, heap_end - 1);
        sift_down(request, (heap_bounds){.root = 0, .element_count = heap_end - 1});
    }
}
