#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * The memory substrate (docs/C_GUIDELINES.md, "Object lifecycle and
 * caller-provided memory", "Interfaces", "Memory and buffers").
 *
 * - Views: functions on sxml_byte_view and sxml_memory_view (sxml/view.h).
 * - Checked size arithmetic: every size computation that could overflow goes
 *   through sxml_size_add, sxml_size_multiply or sxml_size_align_up.
 * - Memory requirements: what an object needs, and how a composite object
 *   adds up the requirements of its parts.
 * - The allocator interface, the default allocator (the C library heap), and
 *   a counting allocator that tests use to count and fail allocations.
 * - An arena: many small allocations released together.
 *
 * Conventions for every object type in sxml, shown here by the counting
 * allocator and the arena:
 * - <type>_memory_requirements(configuration, requirements) reports the size
 *   and alignment of the object for that configuration. It is deterministic.
 * - <type>_initialize(memory, configuration, object) places the object in
 *   caller memory and never allocates the object itself; <type>_finalize
 *   releases what the object acquired, never the memory it lives in.
 * - <type>_create(allocator, configuration, object) allocates and initializes;
 *   it stores the allocator so <type>_destroy needs no extra argument.
 * - finalize and destroy accept NULL and do nothing.
 * - Objects do not move: they stay at their address until finalized.
 * - On failure, outputs are left untouched (docs/DECISIONS.md, D20).
 */

/* ---------------------------------------------------------------- Views */

/*
 * True when first and second have the same byte_count and the same bytes.
 * Two empty views are equal whatever their pointers.
 *
 * Thread safety: safe to call from any thread.
 */
SXML_EXPORT bool sxml_byte_view_equals(sxml_byte_view first,
                                       sxml_byte_view second);

/*
 * A byte view over a NUL-terminated string, without the NUL.
 *
 * For external boundaries only (command lines, host bindings); inside sxml
 * text is always a byte view. c_string must not be NULL; the view borrows it
 * and is valid as long as c_string is.
 *
 * Thread safety: safe to call from any thread.
 */
// ast-grep-ignore: pointer-parameter-without-length
SXML_EXPORT sxml_byte_view sxml_byte_view_from_c_string(const char *c_string);

/* A range of bytes within a view: byte_count bytes from byte_offset. */
typedef struct sxml_byte_range {
    size_t byte_offset;
    size_t byte_count;
} sxml_byte_range;

/*
 * Writes to *slice the part of memory that range selects.
 *
 * Returns SXML_STATUS_INDEX_OUT_OF_RANGE when the range does not lie within
 * memory. The slice aliases memory.
 *
 * Thread safety: safe to call from any thread.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_memory_view_slice(
    sxml_memory_view memory, sxml_byte_range range, sxml_memory_view *slice);

/* ---------------------------------------------- Checked size arithmetic */

/*
 * Each function writes its exact result to the output and returns
 * SXML_STATUS_OK, or returns SXML_STATUS_ARITHMETIC_OVERFLOW and leaves the
 * output untouched when the result does not fit in size_t.
 *
 * Thread safety: safe to call from any thread.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_size_add(size_t left_operand,
                                                     size_t right_operand,
                                                     size_t *sum);

SXML_NODISCARD SXML_EXPORT sxml_status sxml_size_multiply(
    size_t left_operand, size_t right_operand, size_t *product);

/*
 * True when alignment is a power of two (1, 2, 4, ...), the only alignments
 * C allows (C17, section 6.2.8).
 */
SXML_EXPORT bool sxml_alignment_is_valid(size_t alignment);

/*
 * Writes to *aligned_byte_count the smallest multiple of alignment that is
 * not less than byte_count.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT when alignment is not valid and
 * SXML_STATUS_ARITHMETIC_OVERFLOW when the result does not fit in size_t.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_size_align_up(
    size_t byte_count, size_t alignment, size_t *aligned_byte_count);

/* --------------------------------------------------- Memory requirements */

/*
 * The size and alignment of memory an object needs.
 *
 * alignment is a power of two. An empty requirement, the starting point for
 * a composite object, is {.byte_count = 0, .alignment = 1}.
 */
typedef struct sxml_memory_requirements {
    size_t byte_count;
    size_t alignment;
} sxml_memory_requirements;

/*
 * Adds the requirements of one member to those of a composite object laid
 * out in one block, and writes the member's offset within the block to
 * *member_offset.
 *
 * The member is placed at the first offset at or after composite->byte_count
 * that is a multiple of member.alignment; composite grows to its end and
 * takes the larger alignment. Append every member in a fixed order, then
 * split the block with sxml_memory_view_slice at the recorded offsets:
 *
 *     sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
 *     size_t header_offset = 0;
 *     size_t table_offset = 0;
 *     status = sxml_memory_requirements_append(&requirements, header, &header_offset);
 *     ...
 *     status = sxml_memory_requirements_append(&requirements, table, &table_offset);
 *
 * composite is in/out. Returns SXML_STATUS_INVALID_ARGUMENT when either
 * alignment is not valid and SXML_STATUS_ARITHMETIC_OVERFLOW when the total
 * does not fit in size_t; on failure neither composite nor *member_offset
 * changes.
 *
 * Thread safety: safe to call from any thread.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_memory_requirements_append(
    sxml_memory_requirements *composite, sxml_memory_requirements member,
    size_t *member_offset);

/*
 * Returns SXML_STATUS_OK when memory satisfies requirements: at least
 * requirements.byte_count bytes, starting at an address that is a multiple of
 * requirements.alignment. Otherwise returns SXML_STATUS_INVALID_ARGUMENT when
 * requirements.alignment is not valid, then SXML_STATUS_INSUFFICIENT_MEMORY
 * or SXML_STATUS_MISALIGNED_MEMORY. Every initialize function starts with
 * this check.
 *
 * Thread safety: safe to call from any thread.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_memory_view_check_requirements(
    sxml_memory_view memory, sxml_memory_requirements requirements);

/* ----------------------------------------------------- Allocator interface */

/*
 * One allocation: byte_count bytes, starting at a multiple of alignment.
 * byte_count is not 0 and alignment is a power of two.
 */
typedef struct sxml_allocation_request {
    size_t byte_count;
    size_t alignment;
} sxml_allocation_request;

/*
 * The operations of an allocator. Tables are static const and shared by every
 * allocator of a kind; per-allocator data lives only in the state pointer.
 *
 * Implementations may assume that the wrappers below have checked their
 * arguments: requests are valid and memory passed to release is not NULL.
 *
 * There is no reallocate operation (D22): objects grow by allocating anew,
 * and an arena grows by adding blocks.
 */
typedef struct sxml_allocator_operations {
    /* Allocates memory for request and writes its address to *memory, or
       returns SXML_STATUS_OUT_OF_MEMORY and leaves *memory untouched. The
       contents of new memory are unspecified. */
    sxml_status (*allocate)(void *state, sxml_allocation_request request,
                            void **memory);
    /* Releases memory that allocate of the same allocator returned. */
    void (*release)(void *state, void *memory);
} sxml_allocator_operations;

/*
 * The memory interface: state plus a table of operations, passed by value.
 *
 * An allocator whose operations is NULL is the null allocator: it never
 * allocates. A configuration with an allocator field uses it to say "this
 * object must not grow".
 *
 * Lifetime: an object that receives an allocator keeps it and uses it until
 * the object is finalized or destroyed; the allocator's state must outlive
 * the object. Thread safety is that of the implementation.
 */
typedef struct sxml_allocator {
    void *state;
    const sxml_allocator_operations *operations;
} sxml_allocator;

/* True when allocator is the null allocator (its operations is NULL). */
SXML_EXPORT bool sxml_allocator_is_null(sxml_allocator allocator);

/*
 * Allocates memory for request through allocator and writes its address to
 * *memory. Release it with sxml_allocator_release on the same allocator.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for the null allocator, a byte_count
 * of 0 or an invalid alignment; SXML_STATUS_OUT_OF_MEMORY when the allocator
 * has no memory; SXML_STATUS_MISALIGNED_MEMORY when the allocator broke its
 * contract and returned misaligned memory (which is then released). On
 * failure *memory is untouched.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_allocator_allocate(
    sxml_allocator allocator, sxml_allocation_request request, void **memory);

/*
 * Releases memory allocated by allocator. Does nothing when memory is NULL.
 */
SXML_EXPORT void sxml_allocator_release(sxml_allocator allocator,
                                        void *memory);

/*
 * The default allocator: the C library heap (malloc and free, or the
 * platform's aligned variants for alignments above what malloc guarantees).
 * src/allocator.c is the only place in sxml that calls them.
 *
 * Its state is NULL; it needs no setup or teardown. Thread safety: safe to
 * use from any thread, as the C library heap is.
 */
SXML_EXPORT sxml_allocator sxml_default_allocator(void);

/* ---------------------------------------------------- Counting allocator */

/*
 * A test double for the allocator interface (D23). It passes allocations on
 * to a backing allocator, counts them, and can be told to fail one of them,
 * so a test can drive every allocation failure path of the code under test:
 * run the code with failing_allocation_number 1, 2, 3, ... until it succeeds,
 * and check after each run that live_allocation_count is 0.
 *
 * Thread safety: not safe to use from several threads at once.
 */
typedef struct sxml_counting_allocator sxml_counting_allocator;

typedef struct sxml_counting_allocator_configuration {
    /* Provides the memory. Must not be the null allocator; must outlive the
       counting allocator. */
    sxml_allocator backing_allocator;
    /* The allocate call to fail with SXML_STATUS_OUT_OF_MEMORY, counting
       from 1 in the order calls arrive; 0 fails none. Only that one call
       fails; later calls are passed on again. */
    uint64_t failing_allocation_number;
} sxml_counting_allocator_configuration;

/* What a counting allocator has seen so far. */
typedef struct sxml_allocation_counts {
    /* Calls to allocate, failed ones included. */
    uint64_t attempted_allocation_count;
    /* Calls to allocate that failed, injected or not. */
    uint64_t failed_allocation_count;
    /* Calls to release with memory that was not NULL. */
    uint64_t release_count;
    /* Successful allocations not yet released. */
    uint64_t live_allocation_count;
    /* Sum of byte_count over successful allocations. */
    uint64_t allocated_byte_count;
} sxml_allocation_counts;

/*
 * Writes the memory requirements of a counting allocator to *requirements.
 * Returns SXML_STATUS_INVALID_ARGUMENT when configuration names the null
 * allocator as backing allocator.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_counting_allocator_memory_requirements(
    const sxml_counting_allocator_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a counting allocator in memory and writes its address to
 * *counting_allocator. memory must stay valid, unmoved, until
 * sxml_counting_allocator_finalize.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, or
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_counting_allocator_initialize(
    sxml_memory_view memory,
    const sxml_counting_allocator_configuration *configuration,
    sxml_counting_allocator **counting_allocator);

/*
 * Ends the counting allocator's use of its memory. Memory still allocated
 * through it is not released: it belongs to the backing allocator, and a
 * test should have released it (live_allocation_count 0) before this call.
 * Does nothing when counting_allocator is NULL.
 */
SXML_EXPORT void sxml_counting_allocator_finalize(
    sxml_counting_allocator *counting_allocator);

/*
 * Allocates a counting allocator through allocator and initializes it.
 * allocator is stored and must outlive the counting allocator. Returns the
 * failures of sxml_counting_allocator_initialize and
 * sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_counting_allocator_create(
    sxml_allocator allocator,
    const sxml_counting_allocator_configuration *configuration,
    sxml_counting_allocator **counting_allocator);

/*
 * Finalizes a counting allocator made by sxml_counting_allocator_create and
 * releases its memory. Does nothing when counting_allocator is NULL.
 */
SXML_EXPORT void sxml_counting_allocator_destroy(
    sxml_counting_allocator *counting_allocator);

/*
 * The allocator interface of counting_allocator, to pass to the code under
 * test. Valid until counting_allocator is finalized.
 */
SXML_EXPORT sxml_allocator sxml_counting_allocator_as_allocator(
    sxml_counting_allocator *counting_allocator);

/* The counts so far. */
SXML_EXPORT sxml_allocation_counts sxml_counting_allocator_read_counts(
    const sxml_counting_allocator *counting_allocator);

/* ------------------------------------------------------------------- Arena */

/*
 * An arena: allocations that are released together, by sxml_arena_reset,
 * sxml_arena_finalize or sxml_arena_destroy, never one by one. For the many
 * small objects of a tree or an evaluation (docs/C_GUIDELINES.md, "Ownership
 * and lifetimes"; D24).
 *
 * The arena first takes memory from initial_memory, then from blocks it
 * requests from block_allocator. With the null allocator as block allocator
 * it works within initial_memory alone.
 *
 * Thread safety: not safe to use from several threads at once.
 */
typedef struct sxml_arena sxml_arena;

typedef struct sxml_arena_configuration {
    /* Provides blocks when initial_memory is used up; the null allocator
       means the arena never grows and reports SXML_STATUS_CAPACITY_EXCEEDED
       instead. Must outlive the arena. */
    sxml_allocator block_allocator;
    /* Size of each block requested from block_allocator, including a small
       header; a larger allocation gets a block of its own. Must not be 0
       unless block_allocator is the null allocator. */
    size_t block_byte_count;
    /* Most bytes the arena may take from block_allocator in total, a bound
       for work on untrusted input; 0 means no bound. Reaching it gives
       SXML_STATUS_LIMIT_EXCEEDED. */
    size_t maximum_block_byte_count;
    /* Caller memory used before any block; may be empty. Borrowed: must stay
       valid until the arena is finalized. */
    sxml_memory_view initial_memory;
} sxml_arena_configuration;

/*
 * Writes the memory requirements of an arena to *requirements. They do not
 * include initial_memory or blocks. Returns SXML_STATUS_INVALID_ARGUMENT for
 * an invalid configuration.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_arena_memory_requirements(
    const sxml_arena_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places an arena in memory and writes its address to *arena. memory must
 * stay valid, unmoved, until sxml_arena_finalize. The arena does not
 * allocate until the first allocation that initial_memory cannot hold.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, or
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_arena_initialize(
    sxml_memory_view memory, const sxml_arena_configuration *configuration,
    sxml_arena **arena);

/*
 * Releases every block to the block allocator. All memory allocated from the
 * arena becomes invalid. Does nothing when arena is NULL.
 */
SXML_EXPORT void sxml_arena_finalize(sxml_arena *arena);

/*
 * Allocates an arena through allocator and initializes it. allocator is
 * stored and must outlive the arena; it may also serve as the block
 * allocator. Returns the failures of sxml_arena_initialize and
 * sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_arena_create(
    sxml_allocator allocator, const sxml_arena_configuration *configuration,
    sxml_arena **arena);

/*
 * Finalizes an arena made by sxml_arena_create and releases its memory.
 * Does nothing when arena is NULL.
 */
SXML_EXPORT void sxml_arena_destroy(sxml_arena *arena);

/*
 * Allocates memory for request from arena and writes its address to *memory.
 * The memory is valid until the arena is reset, finalized or destroyed.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid request,
 * SXML_STATUS_CAPACITY_EXCEEDED when the arena cannot grow,
 * SXML_STATUS_LIMIT_EXCEEDED when growing would pass
 * maximum_block_byte_count, SXML_STATUS_ARITHMETIC_OVERFLOW for a request
 * too large to compute a block for, and the failures of the block allocator.
 * On failure *memory is untouched and the arena is unchanged.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_arena_allocate(
    sxml_arena *arena, sxml_allocation_request request, void **memory);

/*
 * Releases every block to the block allocator and makes initial_memory
 * available again. All memory allocated from the arena becomes invalid.
 */
SXML_EXPORT void sxml_arena_reset(sxml_arena *arena);

/*
 * The allocator interface of arena, for code that takes an sxml_allocator.
 * Its release does nothing: memory returns to the arena only on reset.
 * Valid until arena is finalized.
 */
SXML_EXPORT sxml_allocator sxml_arena_as_allocator(sxml_arena *arena);
