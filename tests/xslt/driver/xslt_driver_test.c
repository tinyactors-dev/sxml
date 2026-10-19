/*
 * Unit tests of the XSLT 4.0 driver's modules (xslt_reading.h,
 * xslt_environment.h, xslt_catalog.h, xslt_applicability.h,
 * xslt_assertion.h, xslt_test_run.h), over files held in memory so they
 * run the same natively and under WASI. Imitates
 * tests/qt4/driver/driver_test.c: lifecycle, allocation-failure loop
 * (D23).
 */
#include <stdalign.h>
#include <string.h>

#include "sxml/memory.h"
#include "test_runner.h"
#include "xslt_applicability.h"
#include "xslt_assertion.h"
#include "xslt_catalog.h"
#include "xslt_test_run.h"

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

#define CATALOG_NAMESPACE "xmlns='http://www.w3.org/2012/10/xslt-test-catalog'"

static const memory_file suite_files[] = {
    {SXML_BYTE_VIEW_INITIALIZER("suite/catalog.xml"),
     SXML_BYTE_VIEW_INITIALIZER(
         "<catalog " CATALOG_NAMESPACE ">"
         "<test-set name='a' file='tests/insn/a/_a-test-set.xml'/>"
         "<test-set name='s' file='tests/strm/s/_s-test-set.xml'/>"
         "<test-set name='broken' file='tests/misc/broken.xml'/>"
         "<!--<test-set name='hidden' file='x.xml'/>-->"
         "</catalog>")},
    /* a-1 runs it (packet 4.15): the initial template makes /out. */
    {SXML_BYTE_VIEW_INITIALIZER("suite/tests/insn/a/shared.xsl"),
     SXML_BYTE_VIEW_INITIALIZER(
         "<xsl:stylesheet version='3.0'"
         " xmlns:xsl='http://www.w3.org/1999/XSL/Transform'>"
         "<xsl:template name='main'><out/></xsl:template>"
         "</xsl:stylesheet>")},
    {SXML_BYTE_VIEW_INITIALIZER("suite/tests/insn/a/_a-test-set.xml"),
     SXML_BYTE_VIEW_INITIALIZER(
         "<test-set " CATALOG_NAMESPACE " xmlns:f='urn:f' name='a'>"
         "<environment name='doc'><source role='.'>"
         "<content><![CDATA[<doc/>]]></content></source>"
         "<stylesheet file='shared.xsl'/></environment>"
         "<dependencies><spec value='XSLT20+'/></dependencies>"
         "<test-case name='a-1'><description/><created by='x' on='2026-01-01'/>"
         "<environment ref='doc'/><test><initial-template name='main'/></test>"
         "<result><assert>/out</assert></result></test-case>"
         "<test-case name='a-2'><description/><created by='x' on='2026-01-01'/>"
         "<dependencies><spec value='XSLT30 XSLT40'/>"
         "<feature value='higher_order_functions'/></dependencies>"
         "<test><stylesheet file='a-2.xsl' role='principal'/>"
         "<stylesheet file='inc/b.xsl' role='secondary'/>"
         "<param name='p' select='1' as='xs:integer' static='yes'/>"
         "<param name='f:q' select='2'/>"
         "<initial-function name='f:go'><param select='3'/></initial-function>"
         "<output serialize='yes' file='out/a-2.xml'/></test>"
         "<result><all-of><assert-result-document uri='r.xml'>"
         "<assert-xml><![CDATA[<r/>]]></assert-xml></assert-result-document>"
         "<serialization-matches>x</serialization-matches></all-of></result>"
         "</test-case>"
         "<test-case name='a-3'><description/><created by='x' on='2026-01-01'/>"
         "<environment><source role='.' file='d.xml' defines-stylesheet='true'"
         " streaming='true'/></environment>"
         "<dependencies><feature value='streaming'/></dependencies>"
         "<test><initial-mode name='#unnamed' select='//x'>"
         "<param name='t' select='1' tunnel='yes'/></initial-mode>"
         "<output tree='no' result-var='result'/></test>"
         "<result><any-of><error code='XTDE0040'/>"
         "<assert-warning/></any-of></result></test-case>"
         "<test-case name='a-4'><description/><created by='x' on='2026-01-01'/>"
         "<dependencies><spec value='XSLT10'/></dependencies>"
         "<test><package file='p.xsl' uri='urn:p' package-version='1.0'/>"
         "</test><result><assert-true/></result></test-case>"
         "<test-case name='a-5'><description/><created by='x' on='2026-01-01'/>"
         "<environment ref='nowhere'/><test/><result><assert/></result>"
         "</test-case>"
         "<test-case name='a-6'><description/><created by='x' on='2026-01-01'/>"
         "<test><stylesheet file='a-6.xsl'/><initial-template name='g:x'/>"
         "</test><result><assert/></result></test-case>"
         "<test-case name='a-7'><description/><created by='x' on='2026-01-01'/>"
         "<test/><result><not><assert/><assert/></not></result></test-case>"
         "<test-case name='a-8'><description/><created by='x' on='2026-01-01'/>"
         "<test/><result><assert-nothing/></result></test-case>"
         "<test-case name='a-9'><description/><created by='x' on='2026-01-01'/>"
         "<dependencies><feature value='streaming' satisfied='false'/>"
         "<year_component_values value='support year zero'/>"
         "<maximum_number_of_decimal_digits value='60'/>"
         "</dependencies><test/><result><assert/></result></test-case>"
         "<test-case name='a-10'><description/>"
         "<created by='x' on='2026-01-01'/><test/></test-case>"
         "<test-case name='a-11'><description/><created by='x' on='2026-01-01'/>"
         "<dependencies><feature value='schema_aware'/></dependencies>"
         "<test/><result><assert/></result></test-case>"
         "</test-set>")},
    {SXML_BYTE_VIEW_INITIALIZER("suite/tests/strm/s/_s-test-set.xml"),
     SXML_BYTE_VIEW_INITIALIZER(
         "<test-set " CATALOG_NAMESPACE " name='s'>"
         "<dependencies><spec value='XSLT30+'/>"
         "<feature value='streaming'/></dependencies>"
         "<test-case name='s-1'><description/><created by='x' on='2026-01-01'/>"
         "<test><stylesheet file='s-1.xsl'/></test>"
         "<result><assert/></result></test-case>"
         "</test-set>")},
    {SXML_BYTE_VIEW_INITIALIZER("suite/tests/misc/broken.xml"),
     SXML_BYTE_VIEW_INITIALIZER("<test-set " CATALOG_NAMESPACE ">\n<a></b>")},
};

static const memory_file_table suite_table = {
    .files = suite_files,
    .file_count = sizeof suite_files / sizeof suite_files[0],
};

static sxml_xslt_test_catalog_configuration
suite_configuration(sxml_allocator allocator) {
    return (sxml_xslt_test_catalog_configuration){
        .allocator = allocator,
        .file_reader = {.state = (void *)&suite_table,
                        .operations = &memory_file_reader_operations},
        .catalog_path = TEXT("suite/catalog.xml")};
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

/* A loaded catalog with test set test_set_name open; NULL, with the
   failure recorded, when that fails. */
static sxml_xslt_test_catalog *open_catalog(sxml_test_state *state,
                                            sxml_byte_view test_set_name) {
    sxml_xslt_test_catalog *catalog = NULL;
    const sxml_xslt_test_catalog_configuration configuration =
        suite_configuration(sxml_default_allocator());
    sxml_status status = sxml_xslt_test_catalog_create(
        sxml_default_allocator(), &configuration, &catalog);
    sxml_qt4_problem problem = empty_problem();
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_catalog_load(catalog, &problem);
    }
    size_t test_set_index = 0;
    if (status == SXML_STATUS_OK &&
        !sxml_xslt_test_catalog_find_test_set(catalog, test_set_name,
                                              &test_set_index)) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_catalog_open_test_set(
            catalog, &(sxml_qt4_test_set_request){
                         .test_set_index = test_set_index,
                         .problem = &problem});
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    if (status != SXML_STATUS_OK) {
        sxml_xslt_test_catalog_destroy(catalog);
        return NULL;
    }
    return catalog;
}

/* Reads the test case called name of the open test set. */
typedef struct read_outcome {
    sxml_status status;
    sxml_qt4_problem problem;
} read_outcome;

static read_outcome read_case(sxml_xslt_test_catalog *catalog,
                              sxml_byte_view name,
                              sxml_xslt_test_case *test_case) {
    read_outcome outcome = {.status = SXML_STATUS_INVALID_ARGUMENT,
                            .problem = empty_problem()};
    size_t test_case_index = 0;
    if (!sxml_xslt_test_catalog_find_test_case(catalog, name,
                                               &test_case_index)) {
        return outcome;
    }
    outcome.status = sxml_xslt_test_catalog_read_test_case(
        catalog,
        &(sxml_qt4_test_case_request){.test_case_index = test_case_index,
                                      .problem = &outcome.problem},
        test_case);
    return outcome;
}

/* ----------------------------------------------------------- Spec tokens */

static bool matches(sxml_byte_view token) {
    return sxml_xslt_test_spec_token_matches(
        (sxml_xslt_test_spec_token){.token = token, .version = 40});
}

static void
test_spec_tokens_match_xslt_versions(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, matches(TEXT("XSLT40")));
    SXML_TEST_EXPECT(state, matches(TEXT("XSLT40+")));
    SXML_TEST_EXPECT(state, matches(TEXT("XSLT10+")));
    SXML_TEST_EXPECT(state, matches(TEXT("XSLT30+")));
    SXML_TEST_EXPECT(state, !matches(TEXT("XSLT30")));
    SXML_TEST_EXPECT(state, !matches(TEXT("XSLT20")));
    SXML_TEST_EXPECT(state, !matches(TEXT("XSLT50+")));
    SXML_TEST_EXPECT(state, !matches(TEXT("XSLT4")));
    SXML_TEST_EXPECT(state, !matches(TEXT("XSLT400")));
    SXML_TEST_EXPECT(state, !matches(TEXT("XSLT40++")));
    SXML_TEST_EXPECT(state, !matches(TEXT("XP40")));
    SXML_TEST_EXPECT(state, !matches(TEXT("")));
}

static bool is_supported(sxml_byte_view dependency_type, sxml_byte_view value) {
    return sxml_xslt_test_dependency_is_supported((sxml_qt4_dependency_value){
        .dependency_type = dependency_type, .value = value});
}

static void
test_dependency_table_lists_what_sxml_provides(sxml_test_state *state) {
    SXML_TEST_EXPECT(state,
                     is_supported(TEXT("feature"), TEXT("higher_order_functions")));
    SXML_TEST_EXPECT(state, is_supported(TEXT("feature"), TEXT("serialization")));
    /* Schema-aware since D179. */
    SXML_TEST_EXPECT(state, is_supported(TEXT("feature"), TEXT("schema_aware")));
    SXML_TEST_EXPECT(state, !is_supported(TEXT("feature"), TEXT("streaming")));
    SXML_TEST_EXPECT(state, is_supported(TEXT("feature"), TEXT("XPath_3.1")));
    SXML_TEST_EXPECT(state,
                     is_supported(TEXT("feature"), TEXT("dynamic_evaluation")));
    SXML_TEST_EXPECT(state, is_supported(TEXT("year_component_values"),
                                         TEXT("support negative year")));
    SXML_TEST_EXPECT(state, !is_supported(TEXT("year_component_values"),
                                          TEXT("support")));
    SXML_TEST_EXPECT(state, is_supported(TEXT("maximum_number_of_decimal_digits"),
                                         TEXT("101")));
    SXML_TEST_EXPECT(state, !is_supported(
                                TEXT("maximum_number_of_decimal_digits"),
                                TEXT("x")));
    SXML_TEST_EXPECT(state, is_supported(TEXT("enable_assertions"), TEXT("")));
    SXML_TEST_EXPECT(state, !is_supported(TEXT("detect_accumulator_cycles"),
                                          TEXT("")));
    SXML_TEST_EXPECT(state, !is_supported(TEXT("on-multiple-match"),
                                          TEXT("error")));
    SXML_TEST_EXPECT(state, !is_supported(TEXT("spec"), TEXT("XSLT40")));
    SXML_TEST_EXPECT(state, sxml_xslt_test_feature_is_non_goal(TEXT("dtd")));
    SXML_TEST_EXPECT(state, !sxml_xslt_test_feature_is_non_goal(TEXT("HTML5")));
    SXML_TEST_EXPECT(state, sxml_xslt_test_group_is_non_goal(TEXT("strm")));
    SXML_TEST_EXPECT(state, sxml_xslt_test_group_is_non_goal(TEXT("sandp")));
    SXML_TEST_EXPECT(state, !sxml_xslt_test_group_is_non_goal(TEXT("insn")));
}

/* --------------------------------------------------------------- Catalog */

static void test_catalog_reads_tests_and_environments(sxml_test_state *state) {
    sxml_xslt_test_catalog *catalog = open_catalog(state, TEXT("a"));
    SXML_TEST_REQUIRE(state, catalog != NULL);
    SXML_TEST_EXPECT(state, sxml_xslt_test_catalog_test_set_count(catalog) == 3);
    SXML_TEST_EXPECT(state, sxml_xslt_test_catalog_test_case_count(catalog) == 11);
    sxml_xslt_test_case test_case = {.name = {.bytes = NULL, .byte_count = 0}};

    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-1"), &test_case).status ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(test_case.group_name,
                                                  TEXT("insn")));
    SXML_TEST_EXPECT(state, test_case.environment.shared.origin ==
                                SXML_QT4_ENVIRONMENT_ORIGIN_TEST_SET);
    SXML_TEST_REQUIRE(state,
                      test_case.environment.source_details.source_detail_count ==
                          1);
    SXML_TEST_EXPECT(state, test_case.environment.source_details.source_details[0]
                                .has_content);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(test_case.environment.source_details
                                               .source_details[0]
                                               .content,
                                           TEXT("<doc/>")));
    SXML_TEST_REQUIRE(state, test_case.environment.modules.module_count == 1);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         test_case.environment.modules.modules[0].file_path,
                         TEXT("tests/insn/a/shared.xsl")));
    SXML_TEST_EXPECT(state, test_case.invocation.invocation_kind ==
                                SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         test_case.invocation.initial_template.local_name,
                         TEXT("main")));
    SXML_TEST_EXPECT(state, test_case.test_set_dependencies.dependency_count == 1);

    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-2"), &test_case).status ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, test_case.dependencies.dependency_count == 2);
    SXML_TEST_REQUIRE(state, test_case.modules.module_count == 2);
    SXML_TEST_EXPECT(state, test_case.modules.modules[1].module_role ==
                                SXML_XSLT_TEST_MODULE_ROLE_SECONDARY);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(test_case.modules.modules[1].file_path,
                                           TEXT("tests/insn/a/inc/b.xsl")));
    SXML_TEST_REQUIRE(state, test_case.parameters.parameter_count == 2);
    SXML_TEST_EXPECT(state, test_case.parameters.parameters[0].is_static);
    SXML_TEST_EXPECT(state, !test_case.parameters.parameters[1].is_static);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         test_case.parameters.parameters[1].name.namespace_uri,
                         TEXT("urn:f")));
    SXML_TEST_EXPECT(state, test_case.invocation.invocation_kind ==
                                SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION);
    SXML_TEST_EXPECT(state, test_case.invocation.parameters.parameter_count == 1);
    SXML_TEST_EXPECT(state, test_case.output.is_serialized);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(test_case.output.file_path,
                                                  TEXT("tests/insn/a/out/a-2.xml")));

    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-3"), &test_case).status ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, test_case.environment.shared.origin ==
                                SXML_QT4_ENVIRONMENT_ORIGIN_INLINE);
    SXML_TEST_EXPECT(state, test_case.invocation.invocation_kind ==
                                SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         test_case.invocation.initial_mode.lexical_name,
                         TEXT("#unnamed")));
    SXML_TEST_EXPECT(state, test_case.invocation.initial_mode.local_name
                                    .byte_count == 0);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         test_case.invocation.initial_match_selection,
                         TEXT("//x")));
    SXML_TEST_REQUIRE(state, test_case.invocation.parameters.parameter_count == 1);
    SXML_TEST_EXPECT(state,
                     test_case.invocation.parameters.parameters[0].is_tunnel);
    SXML_TEST_EXPECT(state, test_case.output.has_tree_choice);
    SXML_TEST_EXPECT(state, !test_case.output.builds_tree);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(test_case.output.result_variable,
                                                  TEXT("result")));

    read_outcome outcome = read_case(catalog, TEXT("a-5"), &test_case);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(outcome.problem.subject,
                                                  TEXT("nowhere")));
    outcome = read_case(catalog, TEXT("a-6"), &test_case);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(outcome.problem.description,
                                                  TEXT("unbound namespace prefix")));
    outcome = read_case(catalog, TEXT("a-10"), &test_case);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_MALFORMED_INPUT);
    sxml_xslt_test_catalog_destroy(catalog);
}

static void test_catalog_reports_broken_files(sxml_test_state *state) {
    sxml_xslt_test_catalog *catalog = open_catalog(state, TEXT("a"));
    SXML_TEST_REQUIRE(state, catalog != NULL);
    size_t test_set_index = 0;
    SXML_TEST_REQUIRE(state, sxml_xslt_test_catalog_find_test_set(
                                 catalog, TEXT("broken"), &test_set_index));
    SXML_TEST_EXPECT(state, !sxml_xslt_test_catalog_find_test_set(
                                catalog, TEXT("hidden"), &test_set_index));
    SXML_TEST_REQUIRE(state, sxml_xslt_test_catalog_find_test_set(
                                 catalog, TEXT("broken"), &test_set_index));
    sxml_qt4_problem problem = empty_problem();
    SXML_TEST_EXPECT(state, sxml_xslt_test_catalog_open_test_set(
                                catalog, &(sxml_qt4_test_set_request){
                                             .test_set_index = test_set_index,
                                             .problem = &problem}) ==
                                SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state, !sxml_xslt_test_catalog_get_open_test_set(
                                catalog, &test_set_index));
    SXML_TEST_EXPECT(state, sxml_xslt_test_catalog_test_case_count(catalog) == 0);
    sxml_xslt_test_catalog_destroy(catalog);

    sxml_xslt_test_catalog *missing = NULL;
    SXML_TEST_REQUIRE(state,
                      sxml_xslt_test_catalog_create(
                          sxml_default_allocator(),
                          &(sxml_xslt_test_catalog_configuration){
                              .allocator = sxml_default_allocator(),
                              .file_reader = suite_configuration(
                                                 sxml_default_allocator())
                                                 .file_reader,
                              .catalog_path = TEXT("elsewhere/catalog.xml")},
                          &missing) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_xslt_test_catalog_load(missing, &problem) ==
                                SXML_STATUS_INPUT_OUTPUT_FAILED);
    sxml_xslt_test_catalog_destroy(missing);
}

/* ---------------------------------------------------------- Applicability */

static sxml_qt4_applicability decide(sxml_test_state *state,
                                     sxml_xslt_test_catalog *catalog,
                                     sxml_byte_view name) {
    sxml_xslt_test_case test_case = {.name = {.bytes = NULL, .byte_count = 0}};
    sxml_qt4_applicability applicability = {.is_applicable = false};
    SXML_TEST_EXPECT(state, read_case(catalog, name, &test_case).status ==
                                SXML_STATUS_OK);
    sxml_xslt_test_decide_applicability(&test_case, &applicability);
    return applicability;
}

static void
test_applicability_follows_the_rules_in_order(sxml_test_state *state) {
    sxml_xslt_test_catalog *catalog = open_catalog(state, TEXT("a"));
    SXML_TEST_REQUIRE(state, catalog != NULL);
    sxml_qt4_applicability applicability = decide(state, catalog, TEXT("a-1"));
    SXML_TEST_EXPECT(state, applicability.is_applicable);
    SXML_TEST_EXPECT(state, applicability.is_in_scope);

    /* The case's spec replaces the set's; one matching token is enough. */
    applicability = decide(state, catalog, TEXT("a-2"));
    SXML_TEST_EXPECT(state, applicability.is_applicable);

    applicability = decide(state, catalog, TEXT("a-3"));
    SXML_TEST_EXPECT(state, !applicability.is_applicable);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(applicability.reason,
                                                  TEXT("feature")));
    SXML_TEST_EXPECT(state, !applicability.is_in_scope);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(applicability.non_goal,
                                                  TEXT("streaming")));
    SXML_TEST_EXPECT(state, !applicability.is_schema_aware);

    /* Schema-aware cases run, outside the D95 denominator (D179). */
    applicability = decide(state, catalog, TEXT("a-11"));
    SXML_TEST_EXPECT(state, applicability.is_applicable);
    SXML_TEST_EXPECT(state, !applicability.is_in_scope);
    SXML_TEST_EXPECT(state, applicability.is_schema_aware);

    applicability = decide(state, catalog, TEXT("a-4"));
    SXML_TEST_EXPECT(state, !applicability.is_applicable);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(applicability.reason,
                                                  TEXT("spec")));
    SXML_TEST_EXPECT(state, !applicability.is_in_scope);

    /* Asking for a non-goal to be absent keeps the case in scope. */
    applicability = decide(state, catalog, TEXT("a-9"));
    SXML_TEST_EXPECT(state, applicability.is_applicable);
    SXML_TEST_EXPECT(state, applicability.is_in_scope);
    sxml_xslt_test_catalog_destroy(catalog);

    catalog = open_catalog(state, TEXT("s"));
    SXML_TEST_REQUIRE(state, catalog != NULL);
    applicability = decide(state, catalog, TEXT("s-1"));
    SXML_TEST_EXPECT(state, !applicability.is_applicable);
    SXML_TEST_EXPECT(state, !applicability.is_in_scope);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(applicability.non_goal_kind,
                                                  TEXT("group")));
    sxml_xslt_test_catalog_destroy(catalog);
}

/* ------------------------------------------------------------ Assertions */

static sxml_status survey(sxml_xslt_test_catalog *catalog, sxml_byte_view name,
                          sxml_xslt_test_assertion_survey *result) {
    sxml_xslt_test_case test_case = {.name = {.bytes = NULL, .byte_count = 0}};
    const read_outcome outcome = read_case(catalog, name, &test_case);
    if (outcome.status != SXML_STATUS_OK) {
        return outcome.status;
    }
    sxml_qt4_problem problem = empty_problem();
    return sxml_xslt_test_survey_assertions(
        &(sxml_xslt_test_assertion_request){
            .document = sxml_xslt_test_catalog_get_test_set_document(catalog),
            .result_element = test_case.result,
            .test_set_file_path = test_case.test_set_file_path,
            .problem = &problem},
        result);
}

static void test_assertions_are_surveyed(sxml_test_state *state) {
    sxml_xslt_test_catalog *catalog = open_catalog(state, TEXT("a"));
    SXML_TEST_REQUIRE(state, catalog != NULL);
    sxml_xslt_test_assertion_survey result = {.kind_counts = {0}};
    SXML_TEST_REQUIRE(state, survey(catalog, TEXT("a-2"), &result) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     result.kind_counts[SXML_XSLT_TEST_ASSERTION_KIND_ALL_OF] == 1);
    SXML_TEST_EXPECT(
        state,
        result.kind_counts[SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_RESULT_DOCUMENT] ==
            1);
    SXML_TEST_EXPECT(
        state, result.kind_counts[SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_XML] == 1);
    SXML_TEST_EXPECT(state, !result.has_unchecked_kind);

    SXML_TEST_REQUIRE(state, survey(catalog, TEXT("a-3"), &result) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, result.has_unchecked_kind);
    SXML_TEST_EXPECT(state, result.first_unchecked_kind ==
                                SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_WARNING);

    /* not holds one assertion; an unknown element is no assertion. */
    SXML_TEST_EXPECT(state, survey(catalog, TEXT("a-7"), &result) ==
                                SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state, survey(catalog, TEXT("a-8"), &result) ==
                                SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         sxml_xslt_test_assertion_kind_name(
                             SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_POSTURE_AND_SWEEP),
                         TEXT("assert-posture-and-sweep")));
    sxml_xslt_test_catalog_destroy(catalog);
}

/* ------------------------------------------------------------- Plan, run */

static void test_plans_choose_the_principal_module(sxml_test_state *state) {
    sxml_xslt_test_catalog *catalog = open_catalog(state, TEXT("a"));
    SXML_TEST_REQUIRE(state, catalog != NULL);
    sxml_xslt_test_case test_case = {.name = {.bytes = NULL, .byte_count = 0}};
    sxml_xslt_test_plan plan = {.principal_module = NULL};
    sxml_qt4_problem problem = empty_problem();

    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-1"), &test_case).status ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_xslt_test_make_plan(&test_case, &problem,
                                                      &plan) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, plan.principal_module ==
                                &test_case.environment.modules.modules[0]);
    SXML_TEST_EXPECT(state, plan.has_global_context_item);
    SXML_TEST_EXPECT(state,
                     plan.result_delivery == SXML_XSLT_RESULT_DELIVERY_DEFAULT);

    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-2"), &test_case).status ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_xslt_test_make_plan(&test_case, &problem,
                                                      &plan) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, plan.principal_module == &test_case.modules.modules[0]);
    SXML_TEST_EXPECT(state, plan.secondary_module_count == 1);
    SXML_TEST_EXPECT(state, plan.static_parameter_count == 1);
    SXML_TEST_EXPECT(state, plan.stylesheet_parameter_count == 1);
    SXML_TEST_EXPECT(state, plan.result_delivery ==
                                SXML_XSLT_RESULT_DELIVERY_SERIALIZED);

    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-3"), &test_case).status ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_xslt_test_make_plan(&test_case, &problem,
                                                      &plan) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, plan.principal_kind ==
                                SXML_XSLT_TEST_PRINCIPAL_KIND_EMBEDDED);
    SXML_TEST_EXPECT(state, plan.result_delivery == SXML_XSLT_RESULT_DELIVERY_RAW);

    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-4"), &test_case).status ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_xslt_test_make_plan(&test_case, &problem,
                                                      &plan) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     plan.principal_module_source ==
                         SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_PACKAGE);

    SXML_TEST_REQUIRE(state, read_case(catalog, TEXT("a-9"), &test_case).status ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_xslt_test_make_plan(&test_case, &problem,
                                                     &plan) ==
                                SXML_STATUS_MALFORMED_INPUT);
    sxml_xslt_test_catalog_destroy(catalog);
}

/* Runs the test case called name and checks outcome and reason. */
typedef struct run_expectation {
    sxml_byte_view name;
    sxml_qt4_test_outcome outcome;
    sxml_byte_view reason;
} run_expectation;

static void test_runs_judge_outcomes(sxml_test_state *state) {
    static const run_expectation expectations[] = {
        {SXML_BYTE_VIEW_INITIALIZER("a-1"), SXML_QT4_TEST_OUTCOME_PASS,
         SXML_BYTE_VIEW_INITIALIZER("")},
        {SXML_BYTE_VIEW_INITIALIZER("a-3"), SXML_QT4_TEST_OUTCOME_NOT_RUN,
         SXML_BYTE_VIEW_INITIALIZER("skip: feature streaming")},
        {SXML_BYTE_VIEW_INITIALIZER("a-4"), SXML_QT4_TEST_OUTCOME_NOT_RUN,
         SXML_BYTE_VIEW_INITIALIZER("skip: spec XSLT10")},
        {SXML_BYTE_VIEW_INITIALIZER("a-7"), SXML_QT4_TEST_OUTCOME_ERROR,
         SXML_BYTE_VIEW_INITIALIZER("wrong number of assertions in not")},
        {SXML_BYTE_VIEW_INITIALIZER("a-9"), SXML_QT4_TEST_OUTCOME_ERROR,
         SXML_BYTE_VIEW_INITIALIZER("no principal stylesheet in a-9")},
    };
    sxml_xslt_test_catalog *catalog = open_catalog(state, TEXT("a"));
    SXML_TEST_REQUIRE(state, catalog != NULL);
    sxml_arena *arena = make_arena();
    SXML_TEST_REQUIRE(state, arena != NULL);
    for (size_t expectation_index = 0;
         expectation_index < SXML_TEST_COUNT(expectations);
         expectation_index += 1) {
        const run_expectation *expectation = &expectations[expectation_index];
        sxml_xslt_test_case test_case = {
            .name = {.bytes = NULL, .byte_count = 0}};
        SXML_TEST_REQUIRE(state,
                          read_case(catalog, expectation->name, &test_case)
                                  .status == SXML_STATUS_OK);
        sxml_qt4_test_result result = {.outcome = SXML_QT4_TEST_OUTCOME_PASS};
        SXML_TEST_REQUIRE(state,
                          sxml_xslt_test_run_test_case(
                              &(sxml_xslt_test_run_request){
                                  .catalog = catalog,
                                  .test_case = &test_case,
                                  .allocator = sxml_default_allocator(),
                                  .arena = arena},
                              &result) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, result.outcome == expectation->outcome);
        SXML_TEST_EXPECT(state,
                         sxml_byte_view_equals(result.reason, expectation->reason));
    }
    sxml_arena_destroy(arena);
    sxml_xslt_test_catalog_destroy(catalog);
}

/* ------------------------------------------------------------- Lifecycle */

static void test_catalog_lifecycle_on_caller_memory(sxml_test_state *state) {
    const sxml_xslt_test_catalog_configuration configuration =
        suite_configuration(sxml_default_allocator());
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_REQUIRE(state, sxml_xslt_test_catalog_memory_requirements(
                                 &configuration, &requirements) ==
                                 SXML_STATUS_OK);
    alignas(max_align_t) unsigned char memory[1024];
    SXML_TEST_REQUIRE(state, requirements.byte_count <= sizeof memory);
    sxml_xslt_test_catalog *catalog = NULL;
    SXML_TEST_REQUIRE(state, sxml_xslt_test_catalog_initialize(
                                 (sxml_memory_view){.bytes = memory,
                                                    .byte_count = sizeof memory},
                                 &configuration, &catalog) == SXML_STATUS_OK);
    sxml_qt4_problem problem = empty_problem();
    SXML_TEST_EXPECT(state, sxml_xslt_test_catalog_load(catalog, &problem) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_xslt_test_catalog_load(catalog, &problem) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_xslt_test_catalog_finalize(catalog);
    sxml_xslt_test_catalog_finalize(NULL);
    sxml_xslt_test_catalog_destroy(NULL);
    const sxml_xslt_test_catalog_configuration empty_path = {
        .allocator = sxml_default_allocator(),
        .file_reader = configuration.file_reader,
        .catalog_path = {.bytes = NULL, .byte_count = 0}};
    SXML_TEST_EXPECT(state, sxml_xslt_test_catalog_memory_requirements(
                                &empty_path, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
}

/* Loads the suite and reads every test case of test set "a"; problems
   of the cases themselves do not count. */
static sxml_status walk_the_suite(sxml_allocator allocator) {
    sxml_xslt_test_catalog *catalog = NULL;
    const sxml_xslt_test_catalog_configuration configuration =
        suite_configuration(allocator);
    sxml_status status =
        sxml_xslt_test_catalog_create(allocator, &configuration, &catalog);
    sxml_qt4_problem problem = empty_problem();
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_catalog_load(catalog, &problem);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_catalog_open_test_set(
            catalog, &(sxml_qt4_test_set_request){.test_set_index = 0,
                                                 .problem = &problem});
    }
    const size_t test_case_count =
        status == SXML_STATUS_OK ? sxml_xslt_test_catalog_test_case_count(catalog)
                                 : 0;
    for (size_t test_case_index = 0;
         status == SXML_STATUS_OK && test_case_index < test_case_count;
         test_case_index += 1) {
        sxml_xslt_test_case test_case = {
            .name = {.bytes = NULL, .byte_count = 0}};
        status = sxml_xslt_test_catalog_read_test_case(
            catalog,
            &(sxml_qt4_test_case_request){.test_case_index = test_case_index,
                                          .problem = &problem},
            &test_case);
        if (status == SXML_STATUS_MALFORMED_INPUT) {
            status = SXML_STATUS_OK;
        }
    }
    sxml_xslt_test_catalog_destroy(catalog);
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

static const sxml_test_case xslt_driver_test_cases[] = {
    {"spec_tokens_match_xslt_versions", test_spec_tokens_match_xslt_versions},
    {"dependency_table_lists_what_sxml_provides",
     test_dependency_table_lists_what_sxml_provides},
    {"catalog_reads_tests_and_environments",
     test_catalog_reads_tests_and_environments},
    {"catalog_reports_broken_files", test_catalog_reports_broken_files},
    {"applicability_follows_the_rules_in_order",
     test_applicability_follows_the_rules_in_order},
    {"assertions_are_surveyed", test_assertions_are_surveyed},
    {"plans_choose_the_principal_module",
     test_plans_choose_the_principal_module},
    {"runs_judge_outcomes", test_runs_judge_outcomes},
    {"catalog_lifecycle_on_caller_memory",
     test_catalog_lifecycle_on_caller_memory},
    {"catalog_survives_every_allocation_failure",
     test_catalog_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_driver",
            .cases = xslt_driver_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_driver_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
