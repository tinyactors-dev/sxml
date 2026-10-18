#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reason.h"
#include "sxml/memory.h"
#include "test_run.h"

/*
 * The test budget (glossary: test_budget): the allocator of one test of a
 * conformance driver (QT4, XSLT 4.0, XSD 1.1). It passes allocations on
 * to the driver's own allocator, counts live allocations and bytes, and
 * refuses (out of memory) any allocation past
 * SXML_QT4_TEST_MAXIMUM_BYTE_COUNT live bytes (test_run.h, D89). Each
 * block carries a header with its size, as release is not told it. After
 * the test everything must have been released: a live allocation left is
 * a leak, and the test's outcome an error.
 *
 * A plain value: start it, hand out its allocator, finish it. Thread
 * safety: one thread.
 */
typedef struct sxml_qt4_budget {
    sxml_allocator backing_allocator;
    size_t live_byte_count;
    uint64_t live_allocation_count;
    /* An allocation was refused for the byte limit. */
    bool was_exceeded;
} sxml_qt4_budget;

/* A budget over backing_allocator with nothing allocated. */
sxml_qt4_budget sxml_qt4_budget_start(sxml_allocator backing_allocator);

/* The allocator that draws on *budget, which must outlive every use of
   it. */
sxml_allocator sxml_qt4_budget_allocator(sxml_qt4_budget *budget);

/* What a finished test said, to be completed by its budget. */
typedef struct sxml_qt4_budget_verdict {
    sxml_qt4_test_outcome *outcome;
    sxml_qt4_reason *reason;
} sxml_qt4_budget_verdict;

/*
 * Completes a test's verdict once the test released everything: appends
 * " (memory limit)" when an allocation was refused for the byte limit,
 * and, when allocations are still live, makes the outcome an error and
 * appends " [leaked N allocations]". The leaked blocks stay with the
 * backing allocator, where the sanitizers report them.
 */
void sxml_qt4_budget_finish(const sxml_qt4_budget *budget,
                            sxml_qt4_budget_verdict verdict);
