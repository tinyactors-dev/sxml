#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "catalog.h"
#include "cli_command.h"
#include "document.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * The test selection of a conformance driver's list and run commands
 * (glossary: test_selection, test_catalog): the test cases of a test list
 * (test_list.h) in its order, every test case of one test set, or every
 * test case of the catalog; and the problems found on the way, written to
 * standard error as
 *
 *     PROGRAM COMMAND: FILE[:LINE]: description 'subject'
 *
 * after which the selection goes on. It walks any driver's catalog through
 * the test catalog interface below: the XSLT 4.0 driver's
 * (tests/xslt/driver/) and the XSD 1.1 driver's (tests/xsd/driver/).
 * Moved here from tests/xslt/driver/xslt_selection.c (packet 5.3), which
 * imitated the selection of command_list.c and command_run.c.
 */

/* The operations of a test catalog. Tables are static const; state is
   the catalog. */
typedef struct sxml_qt4_test_catalog_operations {
    /* The number of test sets. */
    size_t (*get_test_set_count)(const void *state);
    /* The name of test set test_set_index, empty past the last one. */
    sxml_byte_view (*get_test_set_name)(const void *state,
                                        size_t test_set_index);
    /* Finds the first test set called name. */
    bool (*find_test_set)(const void *state, sxml_byte_view name,
                          size_t *test_set_index);
    /* Opens a test set, closing the open one; fills the problem on
       failure. */
    sxml_status (*open_test_set)(void *state,
                                 const sxml_qt4_test_set_request *request);
    /* Writes the index of the open test set and returns true, or returns
       false when none is open. */
    bool (*get_open_test_set)(const void *state, size_t *test_set_index);
    /* The number of test cases of the open test set. */
    size_t (*get_test_case_count)(const void *state);
    /* Finds the first test case of the open test set called name. */
    bool (*find_test_case)(const void *state, sxml_byte_view name,
                           size_t *test_case_index);
} sxml_qt4_test_catalog_operations;

/* The test catalog interface (glossary: test_catalog), passed by value. */
typedef struct sxml_qt4_test_catalog {
    void *state;
    const sxml_qt4_test_catalog_operations *operations;
} sxml_qt4_test_catalog;

typedef struct sxml_qt4_selection sxml_qt4_selection;

/* Called once per selected test case, with its test set open in the
   catalog. */
typedef struct sxml_qt4_selection_visitor {
    void *state;
    void (*visit)(void *state, sxml_qt4_selection *selection,
                  size_t test_case_index);
} sxml_qt4_selection_visitor;

/* The state of one command's walk. A plain value. */
struct sxml_qt4_selection {
    const sxml_cli_invocation *invocation;
    /* "list" or "run", for problems. */
    sxml_byte_view command_name;
    sxml_qt4_test_catalog catalog;
    bool has_problem;
    bool has_unreadable_file;
    /* Out of memory or a bug: the walk stops. */
    bool has_failed;
};

/* An empty problem. */
sxml_qt4_problem sxml_qt4_selection_no_problem(void);

/*
 * Writes the problem to standard error and records what kind of failure
 * status is: unreadable file (exit 3), malformed input (exit 1), or
 * another failure (exit 4), which stops the walk.
 */
void sxml_qt4_selection_report_failure(sxml_qt4_selection *selection,
                                       sxml_status status,
                                       sxml_qt4_problem *problem);

/* What to walk: TEST-LIST when the invocation has a positional argument,
   else the test set named, else the catalog. */
typedef struct sxml_qt4_selection_request {
    bool has_test_set_name;
    sxml_byte_view test_set_name;
    sxml_qt4_selection_visitor visitor;
} sxml_qt4_selection_request;

/* Walks the selection, calling the visitor for each test case found;
   unknown names are problems. */
void sxml_qt4_selection_walk(sxml_qt4_selection *selection,
                             const sxml_qt4_selection_request *request);

/* The exit code of the walk: 3 for an unreadable file, else 4 after a
   failure, else 1 after a problem, else 0. */
sxml_cli_exit_code
sxml_qt4_selection_exit_code(const sxml_qt4_selection *selection);

/* The names of the test catalog's test sets, for a report's summary
   (report.h); state is the catalog. */
sxml_byte_view sxml_qt4_test_catalog_test_set_name(const void *state,
                                                   size_t test_set_index);
