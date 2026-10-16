#pragma once

#include <stddef.h>
#include <stdint.h>

#include "document.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * Test lists (glossary: test_list): files such as
 * tests/qt4/torture-slice.txt that name QT4 test cases, one per line,
 *
 *     <test-set-name> <test-case-name>  # optional reason
 *
 * with blank lines and lines holding only a "#" comment ignored, as
 * tools/check-test-list.py reads them.
 */

typedef struct sxml_qt4_test_list_entry {
    /* Views into the list's text. */
    sxml_byte_view test_set_name;
    sxml_byte_view test_case_name;
    /* From 1. */
    uint32_t line_number;
} sxml_qt4_test_list_entry;

typedef struct sxml_qt4_test_list {
    const sxml_qt4_test_list_entry *entries;
    size_t entry_count;
} sxml_qt4_test_list;

/* A list to parse. */
typedef struct sxml_qt4_test_list_request {
    /* The list's text; borrowed, and the entries point into it, so it must
       outlive them. */
    sxml_byte_view text;
    /* The name of the list file, for the problem's location. */
    sxml_byte_view source_name;
    /* Receives the entries. */
    sxml_arena *arena;
    /* Filled on failure; may be NULL. */
    sxml_qt4_problem *problem;
} sxml_qt4_test_list_request;

/*
 * Parses request->text into *test_list. Returns
 * SXML_STATUS_MALFORMED_INPUT, with the problem's line set, for a line
 * that does not have exactly two fields before its comment, and the
 * arena's failures; *test_list is then untouched.
 */
SXML_NODISCARD sxml_status sxml_qt4_test_list_parse(
    const sxml_qt4_test_list_request *request, sxml_qt4_test_list *test_list);
