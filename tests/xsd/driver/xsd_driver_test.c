/*
 * Unit tests of the XSD 1.1 driver's modules (xsd_catalog.h,
 * xsd_applicability.h, xsd_test_run.h) and of the shared test budget
 * (budget.h), over files held in memory so they run the same natively and
 * under WASI. Imitates tests/xslt/driver/xslt_driver_test.c: fixtures,
 * lifecycle, allocation-failure loop (D23).
 */
#include <stdalign.h>
#include <string.h>

#include "budget.h"
#include "sxml/memory.h"
#include "test_runner.h"
#include "xsd_applicability.h"
#include "xsd_catalog.h"
#include "xsd_test_run.h"

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ------------------------------------------------------------- Fixtures */

typedef struct memory_file {
    sxml_byte_view path;
    sxml_byte_view contents;
} memory_file;

typedef struct memory_file_table {
    const memory_file *files;
    size_t file_count;
} memory_file_table;

/* The file reader test double: serves the table's files, copied into the
   request's allocator. */
static sxml_status read_memory_file(void *state,
                                    const sxml_cli_file_read_request *request,
                                    sxml_byte_view *contents) {
    const memory_file_table *table = state;
    for (size_t file_index = 0; file_index < table->file_count;
         file_index += 1) {
        const memory_file *file = &table->files[file_index];
        if (!sxml_byte_view_equals(file->path, request->path)) {
            continue;
        }
        void *memory = NULL;
        const sxml_status status = sxml_allocator_allocate(
            request->allocator,
            (sxml_allocation_request){.byte_count = file->contents.byte_count,
                                      .alignment = 1},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        memcpy(memory, file->contents.bytes, file->contents.byte_count);
        *contents = (sxml_byte_view){.bytes = memory,
                                     .byte_count = file->contents.byte_count};
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static const sxml_cli_file_reader_operations memory_file_reader_operations = {
    .read_file = read_memory_file,
};

#define SUITE_NAMESPACES                                                       \
    "xmlns='http://www.w3.org/XML/2004/xml-schema-test-suite/'"                \
    " xmlns:xlink='http://www.w3.org/1999/xlink'"

static const memory_file suite_files[] = {
    {SXML_BYTE_VIEW_INITIALIZER("suite/suite.xml"),
     SXML_BYTE_VIEW_INITIALIZER("<testSuite " SUITE_NAMESPACES
                                " name='s' releaseDate='2026-10-18'"
                                " schemaVersion='1.1'>"
                                "<testSetRef xlink:href='aMeta/one.testSet'/>"
                                "<testSetRef xlink:href='bMeta/sub/two.xml'/>"
                                "<testSetRef xlink:href='plain'/>"
                                "</testSuite>")},
    {SXML_BYTE_VIEW_INITIALIZER("suite/aMeta/one.testSet"),
     SXML_BYTE_VIEW_INITIALIZER(
         "<testSet " SUITE_NAMESPACES " name='One' contributor='C'"
         " version='1.1'>"
         "<testGroup name='g'>"
         "<schemaTest name='s'><schemaDocument xlink:href='../aData/s.xsd'/>"
         "<schemaDocument xlink:href='../aData/t.xsd'/>"
         "<expected validity='valid'/><current status='accepted' date='x'/>"
         "</schemaTest>"
         "<instanceTest name='i'><instanceDocument "
         "xlink:href='../aData/i.xml'/>"
         "<expected validity='invalid' version='1.0'/>"
         "<expected validity='valid'/></instanceTest>"
         "<instanceTest name='m'>"
         "<instanceDocument xlink:href='../aData/missing.xml'/>"
         "<expected validity='valid'/></instanceTest>"
         "</testGroup>"
         "<testGroup name='h' version='1.0 1.1'>"
         "<instanceTest name='old' version='1.0'>"
         "<instanceDocument xlink:href='../aData/i.xml'/>"
         "<expected validity='valid'/></instanceTest>"
         "<instanceTest name='q'><instanceDocument "
         "xlink:href='../aData/i.xml'/>"
         "<expected validity='valid'/><current status='queried' date='x'/>"
         "</instanceTest>"
         "<instanceTest name='n'><instanceDocument "
         "xlink:href='../aData/i.xml'/>"
         "<expected validity='notKnown'/></instanceTest>"
         "</testGroup>"
         "</testSet>")},
    {SXML_BYTE_VIEW_INITIALIZER("suite/aData/s.xsd"),
     SXML_BYTE_VIEW_INITIALIZER(
         "<schema xmlns='http://www.w3.org/2001/XMLSchema'>"
         "<element name='i'/></schema>")},
    {SXML_BYTE_VIEW_INITIALIZER("suite/aData/t.xsd"),
     SXML_BYTE_VIEW_INITIALIZER(
         "<schema xmlns='http://www.w3.org/2001/XMLSchema'/>")},
    {SXML_BYTE_VIEW_INITIALIZER("suite/aData/i.xml"),
     SXML_BYTE_VIEW_INITIALIZER("<i/>")},
    {SXML_BYTE_VIEW_INITIALIZER("suite/bMeta/sub/two.xml"),
     SXML_BYTE_VIEW_INITIALIZER(
         "<testSet " SUITE_NAMESPACES " name='Two' contributor='D'>"
         "<testGroup name='g'><schemaTest name='bad'><schemaDocument/>"
         "<expected validity='valid'/></schemaTest></testGroup>"
         "</testSet>")},
};

static const memory_file_table suite_table = {
    .files = suite_files,
    .file_count = sizeof suite_files / sizeof suite_files[0],
};

static sxml_xsd_test_catalog_configuration
suite_configuration(sxml_allocator allocator) {
    return (sxml_xsd_test_catalog_configuration){
        .allocator = allocator,
        .file_reader = {.state = (void *)&suite_table,
                        .operations = &memory_file_reader_operations},
        .catalog_path = TEXT("suite/suite.xml")};
}

static sxml_qt4_problem empty_problem(void) {
    return (sxml_qt4_problem){
        .description = {.bytes = NULL, .byte_count = 0},
        .subject = {.bytes = NULL, .byte_count = 0},
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0}};
}

/* A loaded catalog with test set test_set_index open; NULL, with the
   failure recorded, when that fails. */
static sxml_xsd_test_catalog *open_catalog(sxml_test_state *state,
                                           size_t test_set_index) {
    sxml_xsd_test_catalog *catalog = NULL;
    const sxml_xsd_test_catalog_configuration configuration =
        suite_configuration(sxml_default_allocator());
    sxml_status status = sxml_xsd_test_catalog_create(sxml_default_allocator(),
                                                      &configuration, &catalog);
    sxml_qt4_problem problem = empty_problem();
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_catalog_load(catalog, &problem);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_catalog_open_test_set(
            catalog,
            &(sxml_qt4_test_set_request){.test_set_index = test_set_index,
                                         .problem = &problem});
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    if (status != SXML_STATUS_OK) {
        sxml_xsd_test_catalog_destroy(catalog);
        return NULL;
    }
    return catalog;
}

/* Reads the test called name of the open test set. */
typedef struct read_outcome {
    sxml_status status;
    sxml_qt4_problem problem;
} read_outcome;

static read_outcome read_case(sxml_xsd_test_catalog *catalog,
                              sxml_byte_view name,
                              sxml_xsd_test_case *test_case) {
    read_outcome outcome = {.status = SXML_STATUS_INVALID_ARGUMENT,
                            .problem = empty_problem()};
    size_t test_case_index = 0;
    if (!sxml_xsd_test_catalog_find_test_case(catalog, name,
                                              &test_case_index)) {
        return outcome;
    }
    outcome.status = sxml_xsd_test_catalog_read_test_case(
        catalog,
        &(sxml_qt4_test_case_request){.test_case_index = test_case_index,
                                      .problem = &outcome.problem},
        test_case);
    return outcome;
}

/* -------------------------------------------------------- Version tokens */

static void test_version_tokens_apply_per_class(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, sxml_xsd_test_version_applies(TEXT("")));
    SXML_TEST_EXPECT(state, sxml_xsd_test_version_applies(TEXT("1.1")));
    SXML_TEST_EXPECT(state, sxml_xsd_test_version_applies(TEXT("1.0 1.1")));
    SXML_TEST_EXPECT(state, !sxml_xsd_test_version_applies(TEXT("1.0")));
    SXML_TEST_EXPECT(state,
                     sxml_xsd_test_version_applies(TEXT(" 1.1\n XML-1.0-5e ")));
    SXML_TEST_EXPECT(state,
                     !sxml_xsd_test_version_applies(TEXT("1.1 XML-1.1")));
    SXML_TEST_EXPECT(state,
                     sxml_xsd_test_version_applies(TEXT("XML-1.1 XML-1.0-5e")));
    SXML_TEST_EXPECT(state,
                     sxml_xsd_test_version_applies(TEXT("full-xpath-in-CTA")));
    SXML_TEST_EXPECT(
        state, !sxml_xsd_test_version_applies(TEXT("restricted-xpath-in-CTA")));
    SXML_TEST_EXPECT(state,
                     !sxml_xsd_test_version_applies(TEXT("Unicode_4.0.0")));
    SXML_TEST_EXPECT(state,
                     !sxml_xsd_test_version_applies(TEXT("1.1 future-token")));
    SXML_TEST_EXPECT(state,
                     sxml_xsd_test_version_is_supported(TEXT("1.1 XML-1.0")));
    SXML_TEST_EXPECT(state,
                     !sxml_xsd_test_version_is_supported(TEXT("1.0 1.1")));
    SXML_TEST_EXPECT(state, sxml_xsd_test_version_is_supported(TEXT("")));
}

/* -------------------------------------------------------------- Catalog */

static void test_catalog_names_test_sets_by_file(sxml_test_state *state) {
    sxml_xsd_test_catalog *catalog = open_catalog(state, 0);
    SXML_TEST_REQUIRE(state, catalog != NULL);
    SXML_TEST_EXPECT(state, sxml_xsd_test_catalog_test_set_count(catalog) == 3);
    const char *const names[] = {"a-one", "sub-two", "plain"};
    for (size_t entry_index = 0; entry_index < 3; entry_index += 1) {
        sxml_qt4_test_set_entry entry = {.name = TEXT(""),
                                         .file_path = TEXT("")};
        SXML_TEST_EXPECT(state,
                         sxml_xsd_test_catalog_get_test_set_entry(
                             catalog, entry_index, &entry) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                    entry.name, sxml_byte_view_from_c_string(
                                                    names[entry_index])));
    }
    size_t test_set_index = 9;
    SXML_TEST_EXPECT(state, sxml_xsd_test_catalog_find_test_set(
                                catalog, TEXT("sub-two"), &test_set_index));
    SXML_TEST_EXPECT(state, test_set_index == 1);
    SXML_TEST_EXPECT(state, !sxml_xsd_test_catalog_find_test_set(
                                catalog, TEXT("Two"), &test_set_index));
    sxml_xsd_test_catalog_destroy(catalog);
}

static void
test_catalog_reads_schema_and_instance_tests(sxml_test_state *state) {
    sxml_xsd_test_catalog *catalog = open_catalog(state, 0);
    SXML_TEST_REQUIRE(state, catalog != NULL);
    SXML_TEST_EXPECT(state,
                     sxml_xsd_test_catalog_test_case_count(catalog) == 6);
    sxml_xsd_test_case test_case = {.name = TEXT("")};
    SXML_TEST_REQUIRE(state,
                      read_case(catalog, TEXT("g/s"), &test_case).status ==
                          SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, test_case.kind == SXML_XSD_TEST_KIND_SCHEMA);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(test_case.contributor, TEXT("C")));
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(test_case.status, TEXT("accepted")));
    SXML_TEST_EXPECT(state, test_case.documents.file_path_count == 2);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(test_case.documents.file_paths[1],
                                           TEXT("aData/t.xsd")));
    SXML_TEST_EXPECT(state, test_case.schema_documents.file_path_count == 2);
    SXML_TEST_REQUIRE(state,
                      read_case(catalog, TEXT("g/i"), &test_case).status ==
                          SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, test_case.kind == SXML_XSD_TEST_KIND_INSTANCE);
    SXML_TEST_EXPECT(state, test_case.has_schema_test);
    SXML_TEST_EXPECT(state, test_case.schema_documents.file_path_count == 2);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(test_case.documents.file_paths[0],
                                           TEXT("aData/i.xml")));
    SXML_TEST_EXPECT(state, test_case.expectations.expectation_count == 2);
    SXML_TEST_EXPECT(state, test_case.status.byte_count == 0);
    SXML_TEST_REQUIRE(state,
                      read_case(catalog, TEXT("h/old"), &test_case).status ==
                          SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !test_case.has_schema_test);
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(test_case.group_version, TEXT("1.0 1.1")));
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(test_case.test_version, TEXT("1.0")));
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(test_case.test_set_version, TEXT("1.1")));
    sxml_xsd_test_catalog_destroy(catalog);
}

static void test_catalog_reports_broken_tests(sxml_test_state *state) {
    sxml_xsd_test_catalog *catalog = open_catalog(state, 1);
    SXML_TEST_REQUIRE(state, catalog != NULL);
    sxml_xsd_test_case test_case = {.name = TEXT("")};
    const read_outcome outcome = read_case(catalog, TEXT("g/bad"), &test_case);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(outcome.problem.location.source_name,
                                           TEXT("bMeta/sub/two.xml")));
    sxml_qt4_problem problem = empty_problem();
    /* "plain" names no file of the suite. */
    SXML_TEST_EXPECT(
        state,
        sxml_xsd_test_catalog_open_test_set(
            catalog, &(sxml_qt4_test_set_request){.test_set_index = 2,
                                                  .problem = &problem}) ==
            SXML_STATUS_INPUT_OUTPUT_FAILED);
    size_t open_index = 0;
    SXML_TEST_EXPECT(
        state, !sxml_xsd_test_catalog_get_open_test_set(catalog, &open_index));
    sxml_xsd_test_catalog_destroy(catalog);
}

/* ------------------------------------------------------------ Decisions */

static sxml_xsd_test_decision decide_named(sxml_xsd_test_catalog *catalog,
                                           sxml_byte_view name) {
    sxml_xsd_test_case test_case = {.name = TEXT("")};
    sxml_xsd_test_decision decision = {.expected_text = TEXT("")};
    decision.applicability.is_applicable = false;
    if (read_case(catalog, name, &test_case).status == SXML_STATUS_OK) {
        sxml_xsd_test_decide(&test_case, &decision);
    }
    return decision;
}

static void
test_decisions_follow_versions_status_and_expected(sxml_test_state *state) {
    sxml_xsd_test_catalog *catalog = open_catalog(state, 0);
    SXML_TEST_REQUIRE(state, catalog != NULL);
    sxml_xsd_test_decision decision = decide_named(catalog, TEXT("g/i"));
    SXML_TEST_EXPECT(state, decision.applicability.is_applicable);
    SXML_TEST_EXPECT(state, decision.applicability.is_in_scope);
    SXML_TEST_EXPECT(state, decision.expected_validity ==
                                SXML_XSD_TEST_VALIDITY_VALID);
    decision = decide_named(catalog, TEXT("h/old"));
    SXML_TEST_EXPECT(state, !decision.applicability.is_applicable);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(decision.applicability.reason,
                                                  TEXT("version")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(decision.applicability.detail,
                                                  TEXT("1.0")));
    decision = decide_named(catalog, TEXT("h/q"));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(decision.applicability.reason,
                                                  TEXT("status")));
    decision = decide_named(catalog, TEXT("h/n"));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(decision.applicability.reason,
                                                  TEXT("expected")));
    SXML_TEST_EXPECT(state, !decision.applicability.is_in_scope);
    sxml_xsd_test_catalog_destroy(catalog);
}

/* ----------------------------------------------------------------- Runs */

static sxml_arena *make_arena(void) {
    sxml_arena *arena = NULL;
    if (sxml_arena_create(
            sxml_default_allocator(),
            &(sxml_arena_configuration){
                .block_allocator = sxml_default_allocator(),
                .block_byte_count = 4096,
                .maximum_block_byte_count = 0,
                .initial_memory = {.bytes = NULL, .byte_count = 0}},
            &arena) != SXML_STATUS_OK) {
        return NULL;
    }
    return arena;
}

static sxml_qt4_test_result run_named(sxml_xsd_test_catalog *catalog,
                                      sxml_arena *arena, sxml_byte_view name) {
    sxml_qt4_test_result result = {.outcome = SXML_QT4_TEST_OUTCOME_PASS,
                                   .reason = TEXT(""),
                                   .is_in_scope = false};
    sxml_xsd_test_case test_case = {.name = TEXT("")};
    if (read_case(catalog, name, &test_case).status != SXML_STATUS_OK ||
        sxml_xsd_test_run_test_case(
            &(sxml_xsd_test_run_request){.catalog = catalog,
                                         .test_case = &test_case,
                                         .allocator = sxml_default_allocator(),
                                         .arena = arena},
            &result) != SXML_STATUS_OK) {
        result.outcome = SXML_QT4_TEST_OUTCOME_ERROR;
    }
    return result;
}

/* Schema tests compile (packet 5.4); instance tests validate (packet
   5.7). */
static void test_runs_compile_schemas_and_validate(sxml_test_state *state) {
    sxml_xsd_test_catalog *catalog = open_catalog(state, 0);
    SXML_TEST_REQUIRE(state, catalog != NULL);
    sxml_arena *arena = make_arena();
    SXML_TEST_REQUIRE(state, arena != NULL);
    sxml_qt4_test_result result = run_named(catalog, arena, TEXT("g/s"));
    SXML_TEST_EXPECT(state, result.outcome == SXML_QT4_TEST_OUTCOME_PASS);
    SXML_TEST_EXPECT(state, result.is_in_scope);
    result = run_named(catalog, arena, TEXT("g/i"));
    SXML_TEST_EXPECT(state, result.outcome == SXML_QT4_TEST_OUTCOME_PASS);
    SXML_TEST_EXPECT(state, result.is_in_scope);
    result = run_named(catalog, arena, TEXT("g/m"));
    SXML_TEST_EXPECT(state, result.outcome == SXML_QT4_TEST_OUTCOME_ERROR);
    SXML_TEST_EXPECT(
        state,
        sxml_byte_view_equals(result.reason,
                              TEXT("document aData/missing.xml: cannot read")));
    result = run_named(catalog, arena, TEXT("h/q"));
    SXML_TEST_EXPECT(state, result.outcome == SXML_QT4_TEST_OUTCOME_NOT_RUN);
    SXML_TEST_EXPECT(state, !result.is_in_scope);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                result.reason, TEXT("skip: status queried")));
    sxml_arena_destroy(arena);
    sxml_xsd_test_catalog_destroy(catalog);
}

static void test_budget_turns_a_leak_into_an_error(sxml_test_state *state) {
    sxml_arena *arena = make_arena();
    SXML_TEST_REQUIRE(state, arena != NULL);
    sxml_qt4_reason reason = {.bytes = NULL};
    SXML_TEST_REQUIRE(state,
                      sxml_qt4_reason_start(arena, &reason) == SXML_STATUS_OK);
    sxml_qt4_budget budget = sxml_qt4_budget_start(sxml_default_allocator());
    const sxml_allocator allocator = sxml_qt4_budget_allocator(&budget);
    void *memory = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_allocator_allocate(
                   allocator,
                   (sxml_allocation_request){.byte_count = 24, .alignment = 8},
                   &memory) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, budget.live_allocation_count == 1);
    sxml_qt4_test_outcome outcome = SXML_QT4_TEST_OUTCOME_PASS;
    sxml_qt4_budget_finish(
        &budget,
        (sxml_qt4_budget_verdict){.outcome = &outcome, .reason = &reason});
    SXML_TEST_EXPECT(state, outcome == SXML_QT4_TEST_OUTCOME_ERROR);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(sxml_qt4_reason_text(&reason),
                                           TEXT(" [leaked 1 allocations]")));
    sxml_allocator_release(allocator, memory);
    SXML_TEST_EXPECT(state, budget.live_allocation_count == 0);
    SXML_TEST_EXPECT(state, budget.live_byte_count == 0);
    sxml_arena_destroy(arena);
}

/* ------------------------------------------------------------ Lifecycle */

static void test_catalog_lifecycle_on_caller_memory(sxml_test_state *state) {
    const sxml_xsd_test_catalog_configuration configuration =
        suite_configuration(sxml_default_allocator());
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_REQUIRE(state,
                      sxml_xsd_test_catalog_memory_requirements(
                          &configuration, &requirements) == SXML_STATUS_OK);
    alignas(max_align_t) uint8_t memory[1024];
    SXML_TEST_REQUIRE(state, requirements.byte_count <= sizeof memory);
    sxml_xsd_test_catalog *catalog = NULL;
    SXML_TEST_REQUIRE(
        state,
        sxml_xsd_test_catalog_initialize(
            (sxml_memory_view){.bytes = memory, .byte_count = sizeof memory},
            &configuration, &catalog) == SXML_STATUS_OK);
    sxml_qt4_problem problem = empty_problem();
    SXML_TEST_EXPECT(state, sxml_xsd_test_catalog_load(catalog, &problem) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_xsd_test_catalog_load(catalog, &problem) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_xsd_test_catalog_finalize(catalog);
    sxml_xsd_test_catalog_configuration invalid = configuration;
    invalid.catalog_path = TEXT("");
    SXML_TEST_EXPECT(state, sxml_xsd_test_catalog_memory_requirements(
                                &invalid, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
}

static sxml_status walk_the_suite(sxml_allocator allocator) {
    sxml_xsd_test_catalog *catalog = NULL;
    const sxml_xsd_test_catalog_configuration configuration =
        suite_configuration(allocator);
    sxml_status status =
        sxml_xsd_test_catalog_create(allocator, &configuration, &catalog);
    sxml_qt4_problem problem = empty_problem();
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_catalog_load(catalog, &problem);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_catalog_open_test_set(
            catalog, &(sxml_qt4_test_set_request){.test_set_index = 0,
                                                  .problem = &problem});
    }
    const size_t test_case_count =
        status == SXML_STATUS_OK
            ? sxml_xsd_test_catalog_test_case_count(catalog)
            : 0;
    for (size_t test_case_index = 0;
         status == SXML_STATUS_OK && test_case_index < test_case_count;
         test_case_index += 1) {
        sxml_xsd_test_case test_case = {
            .name = {.bytes = NULL, .byte_count = 0}};
        status = sxml_xsd_test_catalog_read_test_case(
            catalog,
            &(sxml_qt4_test_case_request){.test_case_index = test_case_index,
                                          .problem = &problem},
            &test_case);
    }
    sxml_xsd_test_catalog_destroy(catalog);
    return status;
}

static void
test_catalog_survives_every_allocation_failure(sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        sxml_counting_allocator *counting_allocator = NULL;
        SXML_TEST_REQUIRE(
            state,
            sxml_counting_allocator_create(
                sxml_default_allocator(),
                &(sxml_counting_allocator_configuration){
                    .backing_allocator = sxml_default_allocator(),
                    .failing_allocation_number = failing_allocation_number},
                &counting_allocator) == SXML_STATUS_OK);
        const sxml_status status = walk_the_suite(
            sxml_counting_allocator_as_allocator(counting_allocator));
        const sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(counting_allocator);
        sxml_counting_allocator_destroy(counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        if (counts.live_allocation_count != 0 ||
            status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
        failing_allocation_number += 1;
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 20);
}

static const sxml_test_case xsd_driver_test_cases[] = {
    {"version_tokens_apply_per_class", test_version_tokens_apply_per_class},
    {"catalog_names_test_sets_by_file", test_catalog_names_test_sets_by_file},
    {"catalog_reads_schema_and_instance_tests",
     test_catalog_reads_schema_and_instance_tests},
    {"catalog_reports_broken_tests", test_catalog_reports_broken_tests},
    {"decisions_follow_versions_status_and_expected",
     test_decisions_follow_versions_status_and_expected},
    {"runs_compile_schemas_and_validate",
     test_runs_compile_schemas_and_validate},
    {"budget_turns_a_leak_into_an_error",
     test_budget_turns_a_leak_into_an_error},
    {"catalog_lifecycle_on_caller_memory",
     test_catalog_lifecycle_on_caller_memory},
    {"catalog_survives_every_allocation_failure",
     test_catalog_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xsd_driver",
            .cases = xsd_driver_test_cases,
            .case_count = SXML_TEST_COUNT(xsd_driver_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
