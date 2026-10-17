#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cli_command.h"
#include "document.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "xslt_catalog.h"

/*
 * The test selection of sxml-xslt-test list and run (glossary:
 * test_selection): the test cases of a test list (test_list.h) in its
 * order, every test case of one test set, or every test case of the
 * catalog; and the problems found on the way, written to standard error
 * as
 *
 *     sxml-xslt-test COMMAND: FILE[:LINE]: description 'subject'
 *
 * after which the selection goes on. Imitates the selection of
 * tests/qt4/driver/command_list.c and command_run.c.
 */

typedef struct sxml_xslt_test_selection sxml_xslt_test_selection;

/* Called once per selected test case, with its test set open in the
   catalog. */
typedef struct sxml_xslt_test_visitor {
    void *state;
    void (*visit)(void *state, sxml_xslt_test_selection *selection,
                  size_t test_case_index);
} sxml_xslt_test_visitor;

/* The state of one command's walk. A plain value. */
struct sxml_xslt_test_selection {
    const sxml_cli_invocation *invocation;
    /* "list" or "run", for problems. */
    sxml_byte_view command_name;
    sxml_xslt_test_catalog *catalog;
    bool has_problem;
    bool has_unreadable_file;
    /* Out of memory or a bug: the walk stops. */
    bool has_failed;
};

/* An empty problem. */
sxml_qt4_problem sxml_xslt_test_no_problem(void);

/*
 * Writes the problem to standard error and records what kind of failure
 * status is: unreadable file (exit 3), malformed input (exit 1), or
 * another failure (exit 4), which stops the walk.
 */
void sxml_xslt_test_selection_report_failure(sxml_xslt_test_selection *selection,
                                             sxml_status status,
                                             sxml_qt4_problem *problem);

/* What to walk: TEST-LIST when the invocation has a positional argument,
   else the test set named, else the catalog. */
typedef struct sxml_xslt_test_selection_request {
    bool has_test_set_name;
    sxml_byte_view test_set_name;
    sxml_xslt_test_visitor visitor;
} sxml_xslt_test_selection_request;

/* Walks the selection, calling the visitor for each test case found;
   unknown names are problems. */
void sxml_xslt_test_selection_walk(
    sxml_xslt_test_selection *selection,
    const sxml_xslt_test_selection_request *request);

/* The exit code of the walk: 3 for an unreadable file, else 4 after a
   failure, else 1 after a problem, else 0. */
sxml_cli_exit_code
sxml_xslt_test_selection_exit_code(const sxml_xslt_test_selection *selection);
