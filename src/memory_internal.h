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
