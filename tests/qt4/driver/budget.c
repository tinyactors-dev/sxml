/*
 * The test budget (budget.h): the allocator of one test, moved here from
 * tests/qt4/driver/test_run.c so that every conformance driver shares it
 * (packet 5.3). Imitates the allocators of src/memory.c.
 */
#include "budget.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* The header before every block: its whole size and its offset. */
typedef struct sxml_qt4_budget_header {
    size_t total_byte_count;
    size_t header_byte_count;
} sxml_qt4_budget_header;

enum { SXML_QT4_BUDGET_MINIMUM_ALIGNMENT = 16 };

static sxml_status sxml_qt4_budget_allocate(void *state,
                                            sxml_allocation_request request,
                                            void **memory) {
    sxml_qt4_budget *budget = state;
    size_t alignment = request.alignment;
    if (alignment < SXML_QT4_BUDGET_MINIMUM_ALIGNMENT) {
        alignment = SXML_QT4_BUDGET_MINIMUM_ALIGNMENT;
    }
    /* The header fits in the first alignment bytes, which keeps the
       block's own alignment. */
    _Static_assert(sizeof(sxml_qt4_budget_header) <=
                       SXML_QT4_BUDGET_MINIMUM_ALIGNMENT,
                   "the budget header fits before the block");
    size_t total_byte_count = 0;
    if (sxml_size_add(request.byte_count, alignment, &total_byte_count) !=
            SXML_STATUS_OK ||
        total_byte_count > SXML_QT4_TEST_MAXIMUM_BYTE_COUNT -
                               budget->live_byte_count) {
        budget->was_exceeded = true;
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    void *block = NULL;
    const sxml_status status = sxml_allocator_allocate(
        budget->backing_allocator,
        (sxml_allocation_request){.byte_count = total_byte_count,
                                  .alignment = alignment},
        &block);
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    uint8_t *start = (uint8_t *)block + alignment;
    const sxml_qt4_budget_header header = {.total_byte_count = total_byte_count,
                                           .header_byte_count = alignment};
    memcpy(start - sizeof header, &header, sizeof header);
    budget->live_byte_count += total_byte_count;
    budget->live_allocation_count += 1;
    *memory = start;
    return SXML_STATUS_OK;
}

static void sxml_qt4_budget_release(void *state, void *memory) {
    sxml_qt4_budget *budget = state;
    uint8_t *start = memory;
    sxml_qt4_budget_header header;
    memcpy(&header, start - sizeof header, sizeof header);
    budget->live_byte_count -= header.total_byte_count;
    budget->live_allocation_count -= 1;
    sxml_allocator_release(budget->backing_allocator,
                           start - header.header_byte_count);
}

static const sxml_allocator_operations sxml_qt4_budget_operations = {
    .allocate = sxml_qt4_budget_allocate,
    .release = sxml_qt4_budget_release,
};

sxml_qt4_budget sxml_qt4_budget_start(sxml_allocator backing_allocator) {
    return (sxml_qt4_budget){.backing_allocator = backing_allocator,
                             .live_byte_count = 0,
                             .live_allocation_count = 0,
                             .was_exceeded = false};
}

sxml_allocator sxml_qt4_budget_allocator(sxml_qt4_budget *budget) {
    return (sxml_allocator){.state = budget,
                            .operations = &sxml_qt4_budget_operations};
}

void sxml_qt4_budget_finish(const sxml_qt4_budget *budget,
                            sxml_qt4_budget_verdict verdict) {
    if (budget->was_exceeded) {
        sxml_qt4_reason_append(verdict.reason, SXML_TEXT(" (memory limit)"));
    }
    if (budget->live_allocation_count != 0) {
        *verdict.outcome = SXML_QT4_TEST_OUTCOME_ERROR;
        sxml_qt4_reason_append(verdict.reason, SXML_TEXT(" [leaked "));
        sxml_qt4_reason_append_decimal(verdict.reason,
                                       budget->live_allocation_count);
        sxml_qt4_reason_append(verdict.reason, SXML_TEXT(" allocations]"));
    }
}
