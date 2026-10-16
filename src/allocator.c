/*
 * The default allocator and the counting allocator (include/sxml/memory.h).
 *
 * This is the only file in sxml that calls the C library heap
 * (docs/C_GUIDELINES.md, "Ownership and lifetimes"; enforced by the ast-grep
 * rule allocation-outside-allocator). Part of the golden module with
 * src/memory.c.
 */
#include "sxml/memory.h"

#include <stdalign.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <malloc.h>
#endif

/* ------------------------------------------------------- Default allocator */

/*
 * Heap memory for request, or NULL.
 *
 * On Windows, memory from _aligned_malloc must be released with
 * _aligned_free, and release does not know the alignment it was allocated
 * with, so every allocation uses _aligned_malloc. Elsewhere, free releases
 * both malloc and aligned_alloc memory, so plain malloc serves the common
 * case and aligned_alloc only the over-aligned one.
 */
static void *sxml_heap_allocate(sxml_allocation_request request) {
#if defined(_WIN32)
    return _aligned_malloc(request.byte_count, request.alignment);
#else
    if (request.alignment <= alignof(max_align_t)) {
        return malloc(request.byte_count);
    }
    /* Some C libraries still require the size to be a multiple of the
       alignment, as C11 did before DR 460 relaxed it in C17. */
    size_t aligned_byte_count = 0;
    if (sxml_size_align_up(request.byte_count, request.alignment,
                           &aligned_byte_count) != SXML_STATUS_OK) {
        return NULL;
    }
    return aligned_alloc(request.alignment, aligned_byte_count);
#endif
}

static void sxml_heap_release(void *memory) {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    free(memory);
#endif
}

static sxml_status sxml_default_allocate_operation(
    void *state, sxml_allocation_request request, void **memory) {
    (void)state;
    void *allocation = sxml_heap_allocate(request);
    if (allocation == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    *memory = allocation;
    return SXML_STATUS_OK;
}

static void sxml_default_release_operation(void *state, void *memory) {
    (void)state;
    sxml_heap_release(memory);
}

static const sxml_allocator_operations sxml_default_allocator_operations = {
    .allocate = sxml_default_allocate_operation,
    .release = sxml_default_release_operation,
};

sxml_allocator sxml_default_allocator(void) {
    return (sxml_allocator){
        .state = NULL,
        .operations = &sxml_default_allocator_operations,
    };
}

/* ------------------------------------------------------ Counting allocator */

struct sxml_counting_allocator {
    sxml_allocator backing_allocator;
    uint64_t failing_allocation_number;
    sxml_allocation_counts counts;
    /* The allocator that sxml_counting_allocator_create allocated this
       object with; the null allocator after sxml_counting_allocator_initialize. */
    sxml_allocator owning_allocator;
};

sxml_status sxml_counting_allocator_memory_requirements(
    const sxml_counting_allocator_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (sxml_allocator_is_null(configuration->backing_allocator)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){
        .byte_count = sizeof(struct sxml_counting_allocator),
        .alignment = alignof(struct sxml_counting_allocator),
    };
    return SXML_STATUS_OK;
}

sxml_status sxml_counting_allocator_initialize(
    sxml_memory_view memory,
    const sxml_counting_allocator_configuration *configuration,
    sxml_counting_allocator **counting_allocator) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_counting_allocator_memory_requirements(
        configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* memory_view_check_requirements has checked size and alignment. */
    void *placement = memory.bytes;
    sxml_counting_allocator *new_counting_allocator = placement;
    *new_counting_allocator = (sxml_counting_allocator){
        .backing_allocator = configuration->backing_allocator,
        .failing_allocation_number = configuration->failing_allocation_number,
        .counts =
            {
                .attempted_allocation_count = 0,
                .failed_allocation_count = 0,
                .release_count = 0,
                .live_allocation_count = 0,
                .allocated_byte_count = 0,
            },
        .owning_allocator = {.state = NULL, .operations = NULL},
    };
    *counting_allocator = new_counting_allocator;
    return SXML_STATUS_OK;
}

void sxml_counting_allocator_finalize(
    sxml_counting_allocator *counting_allocator) {
    /* The counting allocator acquires nothing of its own: the memory it
       hands out belongs to the backing allocator. */
    (void)counting_allocator;
}

sxml_status sxml_counting_allocator_create(
    sxml_allocator allocator,
    const sxml_counting_allocator_configuration *configuration,
    sxml_counting_allocator **counting_allocator) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_counting_allocator_memory_requirements(
        configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(allocator,
                                     (sxml_allocation_request){
                                         .byte_count = requirements.byte_count,
                                         .alignment = requirements.alignment,
                                     },
                                     &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_counting_allocator *new_counting_allocator = NULL;
    status = sxml_counting_allocator_initialize(
        (sxml_memory_view){
            .bytes = memory,
            .byte_count = requirements.byte_count,
        },
        configuration, &new_counting_allocator);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_counting_allocator->owning_allocator = allocator;
    *counting_allocator = new_counting_allocator;
    return SXML_STATUS_OK;
}

void sxml_counting_allocator_destroy(
    sxml_counting_allocator *counting_allocator) {
    if (counting_allocator == NULL) {
        return;
    }
    sxml_allocator owning_allocator = counting_allocator->owning_allocator;
    sxml_counting_allocator_finalize(counting_allocator);
    sxml_allocator_release(owning_allocator, counting_allocator);
}

static sxml_status sxml_counting_allocate_operation(
    void *state, sxml_allocation_request request, void **memory) {
    sxml_counting_allocator *counting_allocator = state;
    sxml_allocation_counts *counts = &counting_allocator->counts;
    counts->attempted_allocation_count += 1;
    if (counts->attempted_allocation_count ==
        counting_allocator->failing_allocation_number) {
        counts->failed_allocation_count += 1;
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    sxml_status status = sxml_allocator_allocate(
        counting_allocator->backing_allocator, request, memory);
    if (status != SXML_STATUS_OK) {
        counts->failed_allocation_count += 1;
        return status;
    }
    counts->live_allocation_count += 1;
    counts->allocated_byte_count += request.byte_count;
    return SXML_STATUS_OK;
}

static void sxml_counting_release_operation(void *state, void *memory) {
    sxml_counting_allocator *counting_allocator = state;
    /* A release without a matching allocation wraps live_allocation_count
       around to a huge value, which a test's check for 0 then reports. */
    counting_allocator->counts.release_count += 1;
    counting_allocator->counts.live_allocation_count -= 1;
    sxml_allocator_release(counting_allocator->backing_allocator, memory);
}

static const sxml_allocator_operations sxml_counting_allocator_operations = {
    .allocate = sxml_counting_allocate_operation,
    .release = sxml_counting_release_operation,
};

sxml_allocator sxml_counting_allocator_as_allocator(
    sxml_counting_allocator *counting_allocator) {
    return (sxml_allocator){
        .state = counting_allocator,
        .operations = &sxml_counting_allocator_operations,
    };
}

sxml_allocation_counts sxml_counting_allocator_read_counts(
    const sxml_counting_allocator *counting_allocator) {
    return counting_allocator->counts;
}
