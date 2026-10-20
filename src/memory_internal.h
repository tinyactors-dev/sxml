#pragma once

#include <stddef.h>

#include "sxml/memory.h"
#include "sxml/status.h"

/*
 * Internal helpers of the memory substrate (src/memory.c). Internal to
 * sxml.
 */

/* An array to allocate: element_count elements of element_size bytes
   each, aligned for the element. */
typedef struct sxml_array_allocation_request {
    size_t element_count;
    size_t element_size;
    size_t element_alignment;
} sxml_array_allocation_request;

/*
 * Allocates the array request describes from allocator, every byte zero,
 * and writes its address to *memory (release it with
 * sxml_allocator_release on the same allocator); an array of no elements
 * is NULL, and nothing is allocated. The one helper for what many modules
 * did with a private copy (D169): the size is checked (count times size,
 * sxml_size_multiply), then allocated.
 *
 * Returns the failures of sxml_size_multiply and sxml_allocator_allocate;
 * *memory is NULL on failure.
 */
SXML_NODISCARD sxml_status sxml_allocator_allocate_array(sxml_allocator allocator,
                                                         sxml_array_allocation_request request,
                                                         void **memory);

/* A growable array (glossary `growable_array`) to make room in: the
   caller's fields, which sxml_allocator_reserve_array updates. */
typedef struct sxml_array_reservation_request {
    /* The array's memory, NULL while it has none; replaced when it grows. */
    void **elements;
    /* Its capacity in elements; raised when it grows. */
    size_t *capacity;
    /* The elements in use, which keep their values when the array moves. */
    size_t element_count;
    /* The elements the array must hold. */
    size_t required_count;
    size_t element_size;
    size_t element_alignment;
    /* The capacity of the first allocation; 0 takes 8. */
    size_t minimum_capacity;
} sxml_array_reservation_request;

/*
 * Makes room for request->required_count elements: when the capacity is
 * smaller, allocates an array of at least that many (the capacity
 * doubled from request->minimum_capacity on), every byte zero, copies the
 * elements in use and releases the old memory. The one helper for the
 * arrays that grow by doubling (D237's list). The array is unchanged on
 * failure.
 *
 * Returns the failures of sxml_allocator_allocate_array, and
 * SXML_STATUS_ARITHMETIC_OVERFLOW for a capacity that does not fit.
 */
SXML_NODISCARD sxml_status
sxml_allocator_reserve_array(sxml_allocator allocator,
                             const sxml_array_reservation_request *request);
