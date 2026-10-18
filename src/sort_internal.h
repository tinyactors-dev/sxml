#pragma once

/*
 * Sorting an array in place (packet 3.15, D161). Internal to sxml.
 *
 * sxml_sort_in_place is a heapsort: O(n log n) comparisons, no memory
 * besides the array, no recursion (D71). It is not stable: callers that
 * need ties in a given order break them in is_before (by position, say).
 * The evaluator's stable sort of positions, which may fail and allocates,
 * is sxml_evaluator_sort_indices (src/evaluator_internal.h).
 */
#include <stdbool.h>
#include <stddef.h>

/* Two elements of the array, borrowed for the call. */
typedef struct sxml_sort_pair {
    const void *first;
    const void *second;
} sxml_sort_pair;

/* Whether pair.first sorts strictly before pair.second; must be a strict
   weak order. It may not sort, so nothing recurses through it (D71). */
typedef bool (*sxml_sort_is_before)(const void *state, sxml_sort_pair pair);

/* An array to sort in place. */
typedef struct sxml_sort_request {
    /* element_count elements of element_size bytes each. */
    void *elements;
    size_t element_count;
    size_t element_size;
    sxml_sort_is_before is_before;
    /* Passed to is_before; may be NULL. */
    const void *state;
} sxml_sort_request;

/* Puts request->elements into the order of request->is_before. */
void sxml_sort_in_place(const sxml_sort_request *request);
