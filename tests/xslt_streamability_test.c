/*
 * Tests for the streamability analysis of XSLT 4.0 (packet 7.6a, D212;
 * XSLT 4.0, 19 "Streamability", 6.6.4, 10.2.3, 15.4, 18.1, 18.2.9 and
 * 19.8.5) through the public API alone: stylesheets with constructs
 * declared streamable compile, or are rejected with XTSE3430 at the
 * construct, and a construct sxml cannot classify is never rejected.
 * Every case checks for leaked cells and allocations; one case runs an
 * allocation-failure loop (D23).
 *
 * Golden file: tests/xslt_compatibility_test.c.
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/xdm.h"
#include "sxml/xslt.h"
#include "test_runner.h"

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

#define STYLESHEET(declarations)                                                          \
    "<xsl:stylesheet version='3.0' xmlns:xsl='http://www.w3.org/1999/XSL/Transform'"    \
    " xmlns:xs='http://www.w3.org/2001/XMLSchema' xmlns:f='urn:f'>" declarations         \
    "</xsl:stylesheet>"

/* A named template whose xsl:source-document streamable="yes" holds
   content. */
#define SOURCE_DOCUMENT(content)                                                         \
    STYLESHEET("<xsl:template name='main'><out><xsl:source-document streamable='yes'"   \
               " href='in.xml'>" content "</xsl:source-document></out></xsl:template>")

/* A streamable unnamed mode with a template rule. */
#define STREAMABLE_RULE(match, body)                                                     \
    STYLESHEET("<xsl:mode streamable='yes'/><xsl:template match='" match "'>" body       \
               "</xsl:template>")

/* ---------------------------------------------------------------- Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(uint64_t failing_allocation_number, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    return sxml_processor_create(allocator, &(sxml_processor_configuration){.allocator = allocator},
                                 &test_fixture->processor);
}

/* Destroys the fixture; true when nothing leaked. */
static bool fixture_destroy(fixture *test_fixture) {
    bool has_no_leak = true;
    if (test_fixture->processor != NULL) {
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(test_fixture->processor);
        sxml_item_heap_clear_raised_error(item_heap);
        has_no_leak = sxml_item_heap_read_counts(item_heap).live_cell_count == 0;
    }
    sxml_processor_destroy(test_fixture->processor);
    if (test_fixture->counting_allocator != NULL) {
        has_no_leak = has_no_leak && sxml_counting_allocator_read_counts(
                                         test_fixture->counting_allocator)
                                             .live_allocation_count == 0;
        sxml_counting_allocator_destroy(test_fixture->counting_allocator);
    }
    return has_no_leak;
}

/* Compiles stylesheet_text; the error record receives a static error. */
typedef struct compilation {
    sxml_byte_view stylesheet_text;
    sxml_error_record error_record;
} compilation;

static sxml_status compile(fixture *test_fixture, compilation *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_xslt_stylesheet *stylesheet = NULL;
    const sxml_status status = sxml_xslt_stylesheet_create(
        fixture_allocator(test_fixture),
        &(sxml_xslt_stylesheet_configuration){
            .processor = test_fixture->processor,
            .principal_module_text = run->stylesheet_text,
            .static_context = {.static_base_uri = TEXT("http://example.com/x/main.xsl")},
            .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT,
            .error_record = &run->error_record},
        &stylesheet);
    sxml_xslt_stylesheet_destroy(stylesheet);
    return status;
}

/* ------------------------------------------------------------------ Cases */

/* A stylesheet and whether it is rejected with XTSE3430. */
typedef struct streamability_case {
    const char *test_case_name;
    const char *stylesheet_text;
    bool is_rejected;
} streamability_case;

static const streamability_case streamability_cases[] = {
    /* 18.1: grounded content (count absorbs a crawling selection,
       19.8.2). */
    {"count-descendants", SOURCE_DOCUMENT("<xsl:value-of select='count(.//item)'/>"), false},
    /* 19.8.8.8: a scanning expression is crawling. */
    {"scanning-expression", SOURCE_DOCUMENT("<xsl:value-of select='sum(//item/@price)'/>"), false},
    /* 19.8.4.36: streamed nodes in the result are not grounded. */
    {"streamed-result", SOURCE_DOCUMENT("<xsl:sequence select='*'/>"), true},
    /* 19.8.1: two consuming operands. */
    {"two-consuming-operands", SOURCE_DOCUMENT("<xsl:value-of select='sum(*/@v), count(*)'/>"),
     true},
    /* 19.8.1: two consuming instructions in one sequence constructor. */
    {"two-consuming-instructions",
     SOURCE_DOCUMENT("<xsl:value-of select='count(*)'/><xsl:value-of select='count(*)'/>"), true},
    /* 19.8.4.10: consuming branches of a choice operand group. */
    {"choose-branches",
     SOURCE_DOCUMENT("<xsl:choose><xsl:when test='@a'><xsl:value-of select='a'/></xsl:when>"
                     "<xsl:otherwise><xsl:value-of select='b'/></xsl:otherwise></xsl:choose>"),
     false},
    /* 19.8.8.9: the following axis is roaming. */
    {"following-axis", SOURCE_DOCUMENT("<xsl:value-of select='count(*/following::x)'/>"), true},
    /* 19.8.4.18: a for-each over streamed nodes with a grounded body. */
    {"for-each", SOURCE_DOCUMENT("<xsl:for-each select='*/item'><i><xsl:value-of select='@id'/></i>"
                                 "</xsl:for-each>"),
     false},
    /* 19.8.4.18: sorting streamed nodes. */
    {"for-each-sort", SOURCE_DOCUMENT("<xsl:for-each select='*/item'><xsl:sort select='@id'/>"
                                      "<xsl:value-of select='@id'/></xsl:for-each>"),
     true},
    /* 19.8.4.20: each prong of xsl:fork may consume. */
    {"fork", SOURCE_DOCUMENT("<xsl:fork><xsl:sequence select='count(*/a)'/>"
                             "<xsl:sequence select='count(*/b)'/></xsl:fork>"),
     false},
    /* 19.8.4.41: a variable bound to streamed nodes. */
    {"variable-bound-to-streamed-node",
     SOURCE_DOCUMENT("<xsl:variable name='v' select='*'/><xsl:value-of select='count($v)'/>"),
     true},
    /* 19.8.4.37: a source document that is not declared streamable is
       not checked. */
    {"unstreamed-source-document",
     STYLESHEET("<xsl:template name='main'><xsl:source-document href='in.xml'>"
                "<xsl:sequence select='//x/following::y'/></xsl:source-document>"
                "</xsl:template>"),
     false},
    /* 6.6.4: a streamable template rule. */
    {"template-rule", STREAMABLE_RULE("item[@type = \"a\"]", "<i><xsl:apply-templates/></i>"),
     false},
    /* 6.6.4, 19.8.10: a match pattern that is not motionless. */
    {"pattern-not-motionless", STREAMABLE_RULE("item[child]", "<i/>"), true},
    {"pattern-positional", STREAMABLE_RULE("item[1]", "<i/>"), true},
    /* 6.6.4: the body of a template rule is grounded. */
    {"template-body-not-grounded", STREAMABLE_RULE("item", "<xsl:sequence select='.'/>"), true},
    /* 19.8.4.5: apply-templates in a mode that is not streamable. */
    {"apply-templates-other-mode",
     STYLESHEET("<xsl:mode streamable='yes'/><xsl:mode name='m'/>"
                "<xsl:template match='item'><xsl:apply-templates mode='m'/></xsl:template>"),
     true},
    /* 19.8.5.2: an absorbing function, and a call of it. */
    {"absorbing-function",
     STYLESHEET("<xsl:function name='f:count' as='xs:integer' streamability='absorbing'>"
                "<xsl:param name='input' as='node()*'/><xsl:sequence select='count($input//*)'/>"
                "</xsl:function><xsl:template name='main'><xsl:source-document streamable='yes'"
                " href='in.xml'><xsl:value-of select='f:count(*)'/></xsl:source-document>"
                "</xsl:template>"),
     false},
    /* 19.8.5.3: an inspection function may not consume. */
    {"inspection-function-consuming",
     STYLESHEET("<xsl:function name='f:text' as='xs:string' streamability='inspection'>"
                "<xsl:param name='input' as='node()'/><xsl:sequence select='string($input)'/>"
                "</xsl:function>"),
     true},
    /* 19.8.5.3: its streaming parameter allows one node only. */
    {"inspection-function-many-nodes",
     STYLESHEET("<xsl:function name='f:name' as='xs:string*' streamability='inspection'>"
                "<xsl:param name='input' as='node()*'/><xsl:sequence select='$input/name()'/>"
                "</xsl:function>"),
     true},
    /* 18.2.9: a streamable accumulator. */
    {"accumulator",
     STYLESHEET("<xsl:accumulator name='a' as='xs:integer' initial-value='0' streamable='yes'>"
                "<xsl:accumulator-rule match='item' select='$value + 1'/></xsl:accumulator>"),
     false},
    {"accumulator-consuming-rule",
     STYLESHEET("<xsl:accumulator name='a' as='xs:string' initial-value=\"''\" streamable='yes'>"
                "<xsl:accumulator-rule match='item' select='string(.)'/></xsl:accumulator>"),
     true},
    /* 10.2.3: a streamable attribute set. */
    {"attribute-set",
     STYLESHEET("<xsl:attribute-set name='s' streamable='yes'><xsl:attribute name='a'"
                " select='name()'/></xsl:attribute-set>"),
     false},
    {"attribute-set-consuming",
     STYLESHEET("<xsl:attribute-set name='s' streamable='yes'><xsl:attribute name='a'"
                " select='string(.)'/></xsl:attribute-set>"),
     true},
    /* 15.4: a streamable merge source reads documents. */
    {"merge-source-without-documents",
     STYLESHEET("<xsl:template name='main'><xsl:merge><xsl:merge-source select='//x'"
                " streamable='yes'><xsl:merge-key select='.'/></xsl:merge-source>"
                "<xsl:merge-action><xsl:sequence select='current-merge-key()'/>"
                "</xsl:merge-action></xsl:merge></xsl:template>"),
     true},
    /* A dynamic call without a signature sxml can see: uncertain, never
       rejected (19.8.8.11). */
    {"dynamic-call-uncertain",
     SOURCE_DOCUMENT("<xsl:variable name='f' select='count#1'/><xsl:value-of select='$f(*)'/>"),
     false},
};

static void test_streamability_cases(sxml_test_state *state) {
    for (size_t case_index = 0;
         case_index < sizeof(streamability_cases) / sizeof(streamability_cases[0]); case_index += 1) {
        const streamability_case *current = &streamability_cases[case_index];
        fixture test_fixture;
        SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        compilation run = {.stylesheet_text = sxml_byte_view_from_c_string(current->stylesheet_text)};
        const sxml_status status = compile(&test_fixture, &run);
        const bool passed =
            current->is_rejected
                ? status == SXML_STATUS_STATIC_ERROR &&
                      sxml_byte_view_equals(run.error_record.code.local_name, TEXT("XTSE3430"))
                : status == SXML_STATUS_OK;
        (void)sxml_test_record_check(state, (sxml_test_check){.passed = passed,
                                                              .expression = current->test_case_name,
                                                              .file_name = __FILE__,
                                                              .line_number = __LINE__});
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
}

/* The error points at the construct declared streamable. */
static void test_error_location(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    compilation run = {.stylesheet_text = TEXT(
                           "<xsl:stylesheet version='3.0'"
                           " xmlns:xsl='http://www.w3.org/1999/XSL/Transform'>\n"
                           "<xsl:template name='main'>\n"
                           "  <xsl:source-document streamable='yes' href='in.xml'>"
                           "<xsl:sequence select='.'/></xsl:source-document>\n"
                           "</xsl:template></xsl:stylesheet>")};
    SXML_TEST_EXPECT(state, compile(&test_fixture, &run) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(run.error_record.code.local_name, TEXT("XTSE3430")));
    SXML_TEST_EXPECT(state, run.error_record.diagnostic.location.line_number == 3);
    SXML_TEST_EXPECT(state, run.error_record.diagnostic.location.column_number == 3);
    SXML_TEST_EXPECT(state, run.error_record.diagnostic.description.byte_count > 0);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Every allocation that fails gives OUT_OF_MEMORY or the error, and
   leaks nothing (D23). */
static void test_allocation_failures(sxml_test_state *state) {
    static const char *const stylesheets[2] = {
        SOURCE_DOCUMENT("<xsl:for-each select='*/item'><i><xsl:value-of select='sum(.//@v)'/></i>"
                        "</xsl:for-each>"),
        SOURCE_DOCUMENT("<xsl:sequence select='*'/>")};
    for (size_t stylesheet_index = 0; stylesheet_index < 2; stylesheet_index += 1) {
        bool has_succeeded = false;
        for (uint64_t failing = 1; failing < 100000 && !has_succeeded; failing += 1) {
            fixture test_fixture;
            if (fixture_create(failing, &test_fixture) != SXML_STATUS_OK) {
                SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
                continue;
            }
            compilation run = {.stylesheet_text =
                                   sxml_byte_view_from_c_string(stylesheets[stylesheet_index])};
            const sxml_status status = compile(&test_fixture, &run);
            const sxml_status expected =
                stylesheet_index == 0 ? SXML_STATUS_OK : SXML_STATUS_STATIC_ERROR;
            SXML_TEST_EXPECT(state, status == expected || status == SXML_STATUS_OUT_OF_MEMORY);
            has_succeeded = status == expected &&
                            sxml_counting_allocator_read_counts(test_fixture.counting_allocator)
                                    .failed_allocation_count == 0;
            SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
        }
        SXML_TEST_EXPECT(state, has_succeeded);
    }
}

static const sxml_test_case xslt_streamability_test_cases[] = {
    {.name = "streamability_cases", .function = test_streamability_cases},
    {.name = "error_location", .function = test_error_location},
    {.name = "allocation_failures", .function = test_allocation_failures},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_streamability",
            .cases = xslt_streamability_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_streamability_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
