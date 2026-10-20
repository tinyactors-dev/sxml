/*
 * The memory substrate: views, checked size arithmetic, memory requirements,
 * the allocator wrappers and the arena (include/sxml/memory.h).
 *
 * This is the golden module (docs/tasks/phase-0.md, task 0.6): later modules
 * imitate its layout, naming, lifecycle functions and comments. The default
 * and counting allocators live in src/allocator.c, the only file allowed to
 * call the C library heap.
 */
#include "sxml/memory.h"

#include "memory_internal.h"

#include <stdalign.h>
#include <string.h>

/* ---------------------------------------------------------------- Views */

bool sxml_byte_view_equals(sxml_byte_view first, sxml_byte_view second) {
    if (first.byte_count != second.byte_count) {
        return false;
    }
    /* memcmp with a NULL pointer is undefined even for a count of 0. */
    if (first.byte_count == 0) {
        return true;
    }
    return memcmp(first.bytes, second.bytes, first.byte_count) == 0;
}

// ast-grep-ignore: pointer-parameter-without-length
sxml_byte_view sxml_byte_view_from_c_string(const char *c_string) {
    return (sxml_byte_view){
        .bytes = (const uint8_t *)c_string,
        .byte_count = strlen(c_string),
    };
}

sxml_status sxml_memory_view_slice(sxml_memory_view memory,
                                   sxml_byte_range range,
                                   sxml_memory_view *slice) {
    /* Written so that no sum can wrap: byte_offset is checked first. */
    if (range.byte_offset > memory.byte_count ||
        range.byte_count > memory.byte_count - range.byte_offset) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    /* Pointer arithmetic on NULL is undefined, even with an offset of 0. */
    uint8_t *slice_bytes =
        range.byte_count == 0 ? NULL : memory.bytes + range.byte_offset;
    *slice = (sxml_memory_view){
        .bytes = slice_bytes,
        .byte_count = range.byte_count,
    };
    return SXML_STATUS_OK;
}

/* ---------------------------------------------- Checked size arithmetic */

/*
 * GCC and Clang provide overflow-checking builtins that compile to a single
 * instruction and a branch. The portable fallback, used by MSVC, compares
 * against SIZE_MAX before computing.
 */
#if defined(__has_builtin)
#if __has_builtin(__builtin_add_overflow) &&                                 \
    __has_builtin(__builtin_mul_overflow)
#define SXML_HAS_OVERFLOW_BUILTINS 1
#endif
#endif

sxml_status sxml_size_add(size_t left_operand, size_t right_operand,
                          size_t *sum) {
#if defined(SXML_HAS_OVERFLOW_BUILTINS)
    size_t result = 0;
    if (__builtin_add_overflow(left_operand, right_operand, &result)) {
        return SXML_STATUS_ARITHMETIC_OVERFLOW;
    }
#else
    if (right_operand > SIZE_MAX - left_operand) {
        return SXML_STATUS_ARITHMETIC_OVERFLOW;
    }
    size_t result = left_operand + right_operand;
#endif
    *sum = result;
    return SXML_STATUS_OK;
}

sxml_status sxml_size_multiply(size_t left_operand, size_t right_operand,
                               size_t *product) {
#if defined(SXML_HAS_OVERFLOW_BUILTINS)
    size_t result = 0;
    if (__builtin_mul_overflow(left_operand, right_operand, &result)) {
        return SXML_STATUS_ARITHMETIC_OVERFLOW;
    }
#else
    if (right_operand != 0 && left_operand > SIZE_MAX / right_operand) {
        return SXML_STATUS_ARITHMETIC_OVERFLOW;
    }
    size_t result = left_operand * right_operand;
#endif
    *product = result;
    return SXML_STATUS_OK;
}

bool sxml_alignment_is_valid(size_t alignment) {
    /* A power of two has exactly one bit set. */
    return alignment != 0 && (alignment & (alignment - 1)) == 0;
}

sxml_status sxml_size_align_up(size_t byte_count, size_t alignment,
                               size_t *aligned_byte_count) {
    if (!sxml_alignment_is_valid(alignment)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    size_t padded_byte_count = 0;
    sxml_status status =
        sxml_size_add(byte_count, alignment - 1, &padded_byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Clearing the low bits rounds down to a multiple of alignment. */
    *aligned_byte_count = padded_byte_count & ~(alignment - 1);
    return SXML_STATUS_OK;
}

/* --------------------------------------------------- Memory requirements */

sxml_status sxml_memory_requirements_append(
    sxml_memory_requirements *composite, sxml_memory_requirements member,
    size_t *member_offset) {
    if (!sxml_alignment_is_valid(composite->alignment) ||
        !sxml_alignment_is_valid(member.alignment)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    size_t offset = 0;
    sxml_status status =
        sxml_size_align_up(composite->byte_count, member.alignment, &offset);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t end_offset = 0;
    status = sxml_size_add(offset, member.byte_count, &end_offset);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Both alignments are powers of two, so the larger is a multiple of the
       smaller and satisfies both. */
    size_t alignment = composite->alignment > member.alignment
                           ? composite->alignment
                           : member.alignment;
    *composite = (sxml_memory_requirements){
        .byte_count = end_offset,
        .alignment = alignment,
    };
    *member_offset = offset;
    return SXML_STATUS_OK;
}

/* True when address is a multiple of alignment, a power of two. */
static bool sxml_address_is_aligned(const void *address, size_t alignment) {
    return ((uintptr_t)address & (uintptr_t)(alignment - 1)) == 0;
}

sxml_status sxml_memory_view_check_requirements(
    sxml_memory_view memory, sxml_memory_requirements requirements) {
    if (!sxml_alignment_is_valid(requirements.alignment)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (memory.bytes == NULL && memory.byte_count != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (memory.byte_count < requirements.byte_count) {
        return SXML_STATUS_INSUFFICIENT_MEMORY;
    }
    if (!sxml_address_is_aligned(memory.bytes, requirements.alignment)) {
        return SXML_STATUS_MISALIGNED_MEMORY;
    }
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------- Allocator wrappers */

bool sxml_allocator_is_null(sxml_allocator allocator) {
    return allocator.operations == NULL;
}

static bool sxml_allocation_request_is_valid(sxml_allocation_request request) {
    return request.byte_count != 0 &&
           sxml_alignment_is_valid(request.alignment);
}

sxml_status sxml_allocator_allocate(sxml_allocator allocator,
                                    sxml_allocation_request request,
                                    void **memory) {
    /* Checking here once lets every implementation trust its arguments. */
    if (sxml_allocator_is_null(allocator) ||
        !sxml_allocation_request_is_valid(request)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    void *allocation = NULL;
    sxml_status status =
        allocator.operations->allocate(allocator.state, request, &allocation);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Host allocators are outside our control; catching a broken alignment
       here is cheaper than debugging the misaligned access it would cause. */
    if (!sxml_address_is_aligned(allocation, request.alignment)) {
        allocator.operations->release(allocator.state, allocation);
        return SXML_STATUS_MISALIGNED_MEMORY;
    }
    *memory = allocation;
    return SXML_STATUS_OK;
}

sxml_status sxml_allocator_allocate_array(sxml_allocator allocator,
                                          sxml_array_allocation_request request,
                                          void **memory) {
    *memory = NULL;
    if (request.element_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply(request.element_count, request.element_size, &byte_count);
    void *allocation = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = request.element_alignment},
            &allocation);
    }
    if (status == SXML_STATUS_OK) {
        memset(allocation, 0, byte_count);
        *memory = allocation;
    }
    return status;
}

sxml_status sxml_allocator_reserve_array(sxml_allocator allocator,
                                         const sxml_array_reservation_request *request) {
    if (request->required_count <= *request->capacity) {
        return SXML_STATUS_OK;
    }
    size_t capacity = request->minimum_capacity == 0 ? 8 : request->minimum_capacity;
    if (capacity < *request->capacity) {
        capacity = *request->capacity;
    }
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && capacity < request->required_count) {
        status = sxml_size_multiply(capacity, 2, &capacity);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            allocator,
            (sxml_array_allocation_request){.element_count = capacity,
                                            .element_size = request->element_size,
                                            .element_alignment = request->element_alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (request->element_count > 0) {
        memcpy(memory, *request->elements, request->element_count * request->element_size);
    }
    sxml_allocator_release(allocator, *request->elements);
    *request->elements = memory;
    *request->capacity = capacity;
    return SXML_STATUS_OK;
}

void sxml_allocator_release(sxml_allocator allocator, void *memory) {
    if (memory == NULL || sxml_allocator_is_null(allocator)) {
        return;
    }
    allocator.operations->release(allocator.state, memory);
}

/* ------------------------------------------------------------------- Arena */

/*
 * The header at the start of every block the arena takes from its block
 * allocator. Blocks form a singly linked list, newest first, so that reset
 * and finalize can release them all.
 */
typedef struct sxml_arena_block {
    struct sxml_arena_block *previous_block;
} sxml_arena_block;

struct sxml_arena {
    sxml_arena_configuration configuration;
    /* The allocator that sxml_arena_create allocated the arena with; the
       null allocator for an arena placed by sxml_arena_initialize. */
    sxml_allocator owning_allocator;
    /* The newest block, or NULL when the arena holds no blocks. */
    sxml_arena_block *newest_block;
    /* The unused rest of initial_memory or of the newest shared block. */
    sxml_memory_view free_memory;
    /* Sum of the byte counts of all blocks, for maximum_block_byte_count. */
    size_t block_byte_total;
};

static bool sxml_arena_configuration_is_valid(
    const sxml_arena_configuration *configuration) {
    bool can_grow = !sxml_allocator_is_null(configuration->block_allocator);
    bool initial_memory_is_valid = configuration->initial_memory.bytes != NULL ||
                                   configuration->initial_memory.byte_count == 0;
    return initial_memory_is_valid &&
           (!can_grow || configuration->block_byte_count != 0);
}

sxml_status sxml_arena_memory_requirements(
    const sxml_arena_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (!sxml_arena_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){
        .byte_count = sizeof(struct sxml_arena),
        .alignment = alignof(struct sxml_arena),
    };
    return SXML_STATUS_OK;
}

sxml_status sxml_arena_initialize(sxml_memory_view memory,
                                  const sxml_arena_configuration *configuration,
                                  sxml_arena **arena) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_arena_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* memory_view_check_requirements has checked size and alignment. */
    void *placement = memory.bytes;
    sxml_arena *new_arena = placement;
    *new_arena = (sxml_arena){
        .configuration = *configuration,
        .owning_allocator = {.state = NULL, .operations = NULL},
        .newest_block = NULL,
        .free_memory = configuration->initial_memory,
        .block_byte_total = 0,
    };
    *arena = new_arena;
    return SXML_STATUS_OK;
}

/* Gives every block back to the block allocator. */
static void sxml_arena_release_blocks(sxml_arena *arena) {
    sxml_arena_block *block = arena->newest_block;
    while (block != NULL) {
        /* Read the link before the block's memory is gone. */
        sxml_arena_block *previous_block = block->previous_block;
        sxml_allocator_release(arena->configuration.block_allocator, block);
        block = previous_block;
    }
    arena->newest_block = NULL;
    arena->block_byte_total = 0;
}

void sxml_arena_finalize(sxml_arena *arena) {
    if (arena == NULL) {
        return;
    }
    sxml_arena_release_blocks(arena);
}

sxml_status sxml_arena_create(sxml_allocator allocator,
                              const sxml_arena_configuration *configuration,
                              sxml_arena **arena) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_arena_memory_requirements(configuration, &requirements);
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
    sxml_arena *new_arena = NULL;
    status = sxml_arena_initialize(
        (sxml_memory_view){
            .bytes = memory,
            .byte_count = requirements.byte_count,
        },
        configuration, &new_arena);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_arena->owning_allocator = allocator;
    *arena = new_arena;
    return SXML_STATUS_OK;
}

void sxml_arena_destroy(sxml_arena *arena) {
    if (arena == NULL) {
        return;
    }
    /* Finalizing may not touch the arena afterwards, so keep the allocator. */
    sxml_allocator owning_allocator = arena->owning_allocator;
    sxml_arena_finalize(arena);
    sxml_allocator_release(owning_allocator, arena);
}

/*
 * Takes request from the front of free_memory (in/out) when it fits after
 * padding for alignment. Returns false, changing nothing, when it does not.
 */
static bool sxml_arena_take_from(sxml_memory_view *free_memory,
                                 sxml_allocation_request request,
                                 void **memory) {
    if (free_memory->byte_count == 0) {
        return false;
    }
    size_t misalignment = (size_t)((uintptr_t)free_memory->bytes &
                                   (uintptr_t)(request.alignment - 1));
    size_t padding_byte_count =
        misalignment == 0 ? 0 : request.alignment - misalignment;
    /* Subtracting, not adding, so that no sum can wrap. */
    if (padding_byte_count > free_memory->byte_count ||
        request.byte_count > free_memory->byte_count - padding_byte_count) {
        return false;
    }
    size_t taken_byte_count = padding_byte_count + request.byte_count;
    *memory = free_memory->bytes + padding_byte_count;
    free_memory->bytes += taken_byte_count;
    free_memory->byte_count -= taken_byte_count;
    return true;
}

/*
 * Requests a block of block_byte_count bytes, links it into the arena and
 * writes the memory after its header to *usable_memory.
 */
static sxml_status sxml_arena_add_block(sxml_arena *arena,
                                        size_t block_byte_count,
                                        sxml_memory_view *usable_memory) {
    size_t maximum = arena->configuration.maximum_block_byte_count;
    /* block_byte_total never exceeds maximum, so the subtraction is safe. */
    if (maximum != 0 && block_byte_count > maximum - arena->block_byte_total) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    void *block_memory = NULL;
    sxml_status status =
        sxml_allocator_allocate(arena->configuration.block_allocator,
                                (sxml_allocation_request){
                                    .byte_count = block_byte_count,
                                    .alignment = alignof(sxml_arena_block),
                                },
                                &block_memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_arena_block *block = block_memory;
    block->previous_block = arena->newest_block;
    arena->newest_block = block;
    arena->block_byte_total += block_byte_count;
    *usable_memory = (sxml_memory_view){
        .bytes = (uint8_t *)block_memory + sizeof(sxml_arena_block),
        .byte_count = block_byte_count - sizeof(sxml_arena_block),
    };
    return SXML_STATUS_OK;
}

/*
 * Serves request from a new block. A request that would not fit in a block
 * of the configured size gets a block of its own, and the free memory of the
 * current block stays available for later small requests.
 */
static sxml_status sxml_arena_allocate_from_new_block(
    sxml_arena *arena, sxml_allocation_request request, void **memory) {
    /* Room for the header, the worst-case padding and the request. */
    size_t required_byte_count = 0;
    sxml_status status = sxml_size_add(
        sizeof(sxml_arena_block), request.alignment - 1, &required_byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(required_byte_count, request.byte_count,
                               &required_byte_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    bool needs_own_block =
        required_byte_count > arena->configuration.block_byte_count;
    size_t block_byte_count = needs_own_block
                                  ? required_byte_count
                                  : arena->configuration.block_byte_count;
    sxml_memory_view block_memory = {.bytes = NULL, .byte_count = 0};
    status = sxml_arena_add_block(arena, block_byte_count, &block_memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!sxml_arena_take_from(&block_memory, request, memory)) {
        /* Unreachable: the block was sized for the request. The block stays
           linked and is released with the others. */
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    if (!needs_own_block) {
        arena->free_memory = block_memory;
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_arena_allocate(sxml_arena *arena,
                                sxml_allocation_request request,
                                void **memory) {
    if (!sxml_allocation_request_is_valid(request)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (sxml_arena_take_from(&arena->free_memory, request, memory)) {
        return SXML_STATUS_OK;
    }
    if (sxml_allocator_is_null(arena->configuration.block_allocator)) {
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    return sxml_arena_allocate_from_new_block(arena, request, memory);
}

void sxml_arena_reset(sxml_arena *arena) {
    sxml_arena_release_blocks(arena);
    arena->free_memory = arena->configuration.initial_memory;
}

static sxml_status sxml_arena_allocate_operation(
    void *state, sxml_allocation_request request, void **memory) {
    return sxml_arena_allocate(state, request, memory);
}

/* Arena memory goes back only on reset, so releasing one piece is a no-op. */
static void sxml_arena_release_operation(void *state, void *memory) {
    (void)state;
    (void)memory;
}

static const sxml_allocator_operations sxml_arena_allocator_operations = {
    .allocate = sxml_arena_allocate_operation,
    .release = sxml_arena_release_operation,
};

sxml_allocator sxml_arena_as_allocator(sxml_arena *arena) {
    return (sxml_allocator){
        .state = arena,
        .operations = &sxml_arena_allocator_operations,
    };
}
