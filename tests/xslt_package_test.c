/*
 * Tests for XSLT 4.0 packages (src/xslt_package_internal.h; XSLT 4.0,
 * 3.4; packet 4.16, D160): stylesheets that use packages the resolver
 * serves, compiled and run through include/sxml/xslt.h; component
 * visibility, xsl:accept, xsl:expose, xsl:override with xsl:original
 * (the binding example of 3.4.5.5 among them), modes and attribute sets
 * across packages, abstract components, the static errors of 3.4; and the
 * version ranges of 3.4.1. Every transformation checks for leaked cells
 * and allocations.
 *
 * Golden file: tests/xslt_test.c.
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"
#include "sxml/xslt.h"
#include "test_runner.h"
#include "xslt_package_internal.h"

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

#define XSL_NAMESPACE "xmlns:xsl='http://www.w3.org/1999/XSL/Transform'"

/* An xsl:package of name urn:NAME around declarations. */
#define PACKAGE(name, attributes, declarations)                                                    \
    "<xsl:package name='urn:" name "' package-version='1.0' version='3.0' " XSL_NAMESPACE          \
    " xmlns:xs='http://www.w3.org/2001/XMLSchema' xmlns:f='urn:f' exclude-result-prefixes='#all'"  \
    " " attributes ">" declarations "</xsl:package>"

/* ---------------------------------------------------------------- Fixture */

/* 3.4.5.5's example: A final, B private, C public. */
static const char binding_text[] =
    PACKAGE("binding", "",
            "<xsl:variable name='A' visibility='final' select='$B + 1'/>"
            "<xsl:variable name='B' visibility='private' select='$C * 2'/>"
            "<xsl:variable name='C' visibility='public' select='22'/>");

static const char library_text[] =
    PACKAGE("library", "",
            "<xsl:expose component='function' names='f:*' visibility='public'/>"
            "<xsl:expose component='template' names='greet' visibility='public'/>"
            "<xsl:function name='f:twice'><xsl:param name='n'/><xsl:sequence "
            "select='f:hidden($n) * 2'/></xsl:function>"
            "<xsl:function name='f:hidden' visibility='private'><xsl:param name='n'/><xsl:sequence "
            "select='$n'/></xsl:function>"
            "<xsl:function name='f:hook' visibility='public'><xsl:sequence select=\"'base'\"/>"
            "</xsl:function>"
            "<xsl:function name='f:call-hook' visibility='final'><xsl:sequence select='f:hook()'/>"
            "</xsl:function>"
            "<xsl:function name='f:abstract' visibility='abstract'/>"
            "<xsl:function name='f:call-abstract' visibility='public'><xsl:sequence "
            "select='f:abstract()'/></xsl:function>"
            "<xsl:template name='greet'><xsl:param name='who' select=\"'world'\"/>"
            "<xsl:value-of select=\"'hello ' || $who\"/></xsl:template>"
            "<xsl:attribute-set name='style' visibility='public'><xsl:attribute name='a'>1"
            "</xsl:attribute></xsl:attribute-set>"
            "<xsl:mode name='m' visibility='public'/>"
            "<xsl:template match='*' mode='m'><base/></xsl:template>"
            "<xsl:mode/>"
            "<xsl:template match='*'><library-unnamed/></xsl:template>"
            "<xsl:template name='apply-m' visibility='public'><xsl:apply-templates "
            "select='parse-xml(\"&lt;e/&gt;\")/*' mode='m'/></xsl:template>"
            "<xsl:variable name='private-value' select='1'/>");

static const char other_text[] =
    PACKAGE("other", "",
            "<xsl:function name='f:hidden' visibility='private'><xsl:param "
            "name='n'/><xsl:sequence select='-1'/></xsl:function>"
            "<xsl:function name='f:minus' visibility='public'><xsl:param "
            "name='n'/><xsl:sequence select='f:hidden($n)'/></xsl:function>"
            "<xsl:function name='f:twice' visibility='public'><xsl:param "
            "name='n'/><xsl:sequence select='0'/></xsl:function>");

static const sxml_resource_entry fixture_resources[] = {
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:binding"),
     .content = {.bytes = (const uint8_t *)binding_text, .byte_count = sizeof(binding_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:library"),
     .content = {.bytes = (const uint8_t *)library_text, .byte_count = sizeof(library_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:other"),
     .content = {.bytes = (const uint8_t *)other_text, .byte_count = sizeof(other_text) - 1}},
};

static const sxml_resource_entry_view fixture_resource_view = {
    .resource_entries = fixture_resources,
    .resource_entry_count = sizeof(fixture_resources) / sizeof(fixture_resources[0])};

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL, .output_buffer = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number = 0},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){.allocator = allocator,
                                        .resource_resolver =
                                            sxml_table_resource_resolver(&fixture_resource_view)},
        &test_fixture->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            sxml_default_allocator(),
            &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                                .maximum_byte_count = 0},
            &test_fixture->output_buffer);
    }
    return status;
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
    sxml_output_buffer_destroy(test_fixture->output_buffer);
    if (test_fixture->counting_allocator != NULL) {
        has_no_leak =
            has_no_leak && sxml_counting_allocator_read_counts(test_fixture->counting_allocator)
                                   .live_allocation_count == 0;
        sxml_counting_allocator_destroy(test_fixture->counting_allocator);
    }
    return has_no_leak;
}

/* --------------------------------------------------------------- Running */

/* Compiles text (an xsl:package) and calls xsl:initial-template, the
   result serialized as text into the output buffer; the error code of a
   failure in *error_record. */
static sxml_status run_package(fixture *test_fixture, sxml_byte_view text,
                               sxml_error_record *error_record) {
    memset(error_record, 0, sizeof(*error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xslt_stylesheet *stylesheet = NULL;
    sxml_status status = sxml_xslt_stylesheet_create(
        fixture_allocator(test_fixture),
        &(sxml_xslt_stylesheet_configuration){
            .processor = test_fixture->processor,
            .principal_module_text = text,
            .static_context = {.static_base_uri = TEXT("http://example.com/x/main.xsl")},
            .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT,
            .error_record = error_record},
        &stylesheet);
    if (status == SXML_STATUS_OK) {
        sxml_dynamic_context_configuration dynamic_context;
        memset(&dynamic_context, 0, sizeof(dynamic_context));
        sxml_sequence principal = sxml_sequence_empty();
        status = sxml_xslt_stylesheet_transform(
            stylesheet,
            &(sxml_xslt_transformation_request){
                .dynamic_context = dynamic_context,
                .initial_match_selection = sxml_sequence_empty(),
                .template_parameters = {.variable_bindings = NULL, .variable_binding_count = 0},
                .tunnel_parameters = {.variable_bindings = NULL, .variable_binding_count = 0},
                .function_arguments = {.sequences = NULL, .sequence_count = 0},
                .base_output_uri = TEXT("http://example.com/out/main.xml"),
                .output_definition = NULL,
                .output_stream = sxml_output_buffer_as_output_stream(test_fixture->output_buffer),
                .result_document_handler = {.state = NULL, .operations = NULL},
                .error_record = error_record,
                .initial_mode_name = {.index = 0},
                .initial_template_name = {.index = 0},
                .initial_function_name = {.index = 0},
                .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE,
                .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED,
                .has_initial_match_selection = false,
                .selects_unnamed_mode = false},
            &principal);
        sxml_sequence_release(principal);
    }
    sxml_xslt_stylesheet_destroy(stylesheet);
    return status;
}

static bool output_is(const fixture *test_fixture, sxml_byte_view expected) {
    return sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture->output_buffer),
                                 expected);
}

/* ------------------------------------------------------------ The cases */

/* A main package: uses packages, then an initial template. */
#define MAIN(uses, body)                                                                           \
    PACKAGE("main", "declared-modes='no'",                                                         \
            "<xsl:output method='text'/>" uses                                                     \
            "<xsl:template name='xsl:initial-template' visibility='public'>" body                  \
            "</xsl:template>")

typedef struct package_case {
    const char *test_case_name;
    sxml_byte_view text;
    /* The text output, or the expected error code. */
    sxml_byte_view expected_output;
    sxml_byte_view expected_error_code;
} package_case;

#define RUNS(name, text, output)                                                                   \
    {                                                                                              \
        name, SXML_BYTE_VIEW_INITIALIZER(text), SXML_BYTE_VIEW_INITIALIZER(output), {              \
            .bytes = NULL, .byte_count = 0                                                         \
        }                                                                                          \
    }
#define FAILS(name, text, code)                                                                    \
    {                                                                                              \
        name, SXML_BYTE_VIEW_INITIALIZER(text), {.bytes = NULL, .byte_count = 0},                  \
            SXML_BYTE_VIEW_INITIALIZER(code)                                                       \
    }

static const package_case package_cases[] = {
    /* 3.4.5.5: ((22 + 3) * 2) + 1, the overriding C reached from A and B. */
    RUNS("binding-example",
         MAIN("<xsl:use-package name='urn:binding'><xsl:override><xsl:variable name='C' "
              "select='$xsl:original + 3'/></xsl:override></xsl:use-package>",
              "<xsl:value-of select='$A'/>"),
         "51"),
    RUNS("public-function-private-helper",
         MAIN("<xsl:use-package name='urn:library'/>", "<xsl:value-of select='f:twice(4)'/>"), "8"),
    FAILS("private-function-hidden",
          MAIN("<xsl:use-package name='urn:library'/>", "<xsl:value-of select='f:hidden(4)'/>"),
          "XPST0017"),
    RUNS("private-names-do-not-clash",
         MAIN("<xsl:use-package name='urn:library'/><xsl:use-package name='urn:other'>"
              "<xsl:accept component='function' names='f:twice#1' visibility='hidden'/>"
              "</xsl:use-package>",
              "<xsl:value-of select='f:twice(4), f:minus(4)'/>"),
         "8 -1"),
    FAILS("homonymous-accepted",
          MAIN("<xsl:use-package name='urn:library'/><xsl:use-package name='urn:other'/>",
               "<xsl:value-of select='1'/>"),
          "XTSE3050"),
    RUNS("override-function-reached-inside",
         MAIN("<xsl:use-package name='urn:library'><xsl:override><xsl:function "
              "name='f:hook' visibility='public'><xsl:sequence select=\"'new ' || "
              "xsl:original()\"/></xsl:function></xsl:override></xsl:use-package>",
              "<xsl:value-of select='f:call-hook()'/>"),
         "new base"),
    FAILS("override-final",
          MAIN("<xsl:use-package name='urn:library'><xsl:override><xsl:function "
               "name='f:call-hook'><xsl:sequence select='1'/></xsl:function>"
               "</xsl:override></xsl:use-package>",
               "<xsl:value-of select='1'/>"),
          "XTSE3060"),
    FAILS("override-nothing",
          MAIN("<xsl:use-package name='urn:library'><xsl:override><xsl:function "
               "name='f:absent'><xsl:sequence select='1'/></xsl:function>"
               "</xsl:override></xsl:use-package>",
               "<xsl:value-of select='1'/>"),
          "XTSE3058"),
    RUNS("override-abstract",
         MAIN("<xsl:use-package name='urn:library'><xsl:override><xsl:function "
              "name='f:abstract'><xsl:sequence select=\"'concrete'\"/></xsl:function>"
              "</xsl:override></xsl:use-package>",
              "<xsl:value-of select='f:call-abstract()'/>"),
         "concrete"),
    FAILS(
        "abstract-called",
        MAIN("<xsl:use-package name='urn:library'/>", "<xsl:value-of select='f:call-abstract()'/>"),
        "XTDE3052"),
    RUNS("template-override-original",
         MAIN("<xsl:use-package name='urn:library'><xsl:override><xsl:template name='greet'>"
              "<xsl:param name='who' select=\"'world'\"/>[<xsl:call-template "
              "name='xsl:original'><xsl:with-param name='who' select='$who'/>"
              "</xsl:call-template>]</xsl:template></xsl:override></xsl:use-package>",
              "<xsl:call-template name='greet'><xsl:with-param name='who' select=\"'you'\"/>"
              "</xsl:call-template>"),
         "[hello you]"),
    RUNS("mode-override-rules-first",
         MAIN("<xsl:use-package name='urn:library'><xsl:override><xsl:template match='e' "
              "mode='m'>override</xsl:template></xsl:override></xsl:use-package>",
              "<xsl:call-template name='apply-m'/>"),
         "override"),
    RUNS("unnamed-modes-apart",
         MAIN("<xsl:use-package name='urn:library'/><xsl:template match='*'>main-unnamed"
              "</xsl:template>",
              "<xsl:apply-templates select='parse-xml(\"&lt;e/&gt;\")/*'/>"),
         "main-unnamed"),
    RUNS("attribute-set-override",
         MAIN("<xsl:use-package name='urn:library'><xsl:override><xsl:attribute-set "
              "name='style' use-attribute-sets='xsl:original'><xsl:attribute name='b'>2"
              "</xsl:attribute></xsl:attribute-set></xsl:override></xsl:use-package>",
              "<xsl:variable name='e'><e xsl:use-attribute-sets='style'/></xsl:variable>"
              "<xsl:value-of select='$e/e/@* ! name()'/>"),
         "a b"),
    FAILS("private-variable",
          MAIN("<xsl:use-package name='urn:library'/>", "<xsl:value-of select='$private-value'/>"),
          "XPST0008"),
    FAILS("version-out-of-range",
          MAIN("<xsl:use-package name='urn:library' package-version='2.0+'/>",
               "<xsl:value-of select='1'/>"),
          "XTSE3003"),
    FAILS("version-range-syntax",
          MAIN("<xsl:use-package name='urn:library' package-version='1..0'/>",
               "<xsl:value-of select='1'/>"),
          "XTSE0020"),
    FAILS("no-such-package",
          MAIN("<xsl:use-package name='urn:absent'/>", "<xsl:value-of select='1'/>"), "XTSE3000"),
    FAILS("expose-unknown",
          PACKAGE("main", "",
                  "<xsl:expose component='template' names='absent' "
                  "visibility='public'/><xsl:template name='xsl:initial-template' "
                  "visibility='public'/>"),
          "XTSE3020"),
    FAILS("accept-unknown",
          MAIN("<xsl:use-package name='urn:library'><xsl:accept component='function' "
               "names='f:absent#0' visibility='public'/></xsl:use-package>",
               "<xsl:value-of select='1'/>"),
          "XTSE3030"),
    FAILS("abstract-top-level",
          PACKAGE("main", "",
                  "<xsl:function name='f:a' visibility='abstract'/><xsl:template "
                  "name='xsl:initial-template' visibility='public'/>"),
          "XTSE3080"),
    FAILS("private-initial-template",
          PACKAGE("main", "", "<xsl:template name='xsl:initial-template'>x</xsl:template>"),
          "XTDE0040"),
};

static void test_packages_bind_their_components(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < sizeof(package_cases) / sizeof(package_cases[0]);
         case_index += 1) {
        const package_case *current = &package_cases[case_index];
        fixture test_fixture;
        SXML_TEST_EXPECT(state, fixture_create(&test_fixture) == SXML_STATUS_OK);
        sxml_error_record error_record;
        const sxml_status status = run_package(&test_fixture, current->text, &error_record);
        const bool passed =
            current->expected_error_code.byte_count > 0
                ? status != SXML_STATUS_OK && sxml_byte_view_equals(error_record.code.local_name,
                                                                    current->expected_error_code)
                : status == SXML_STATUS_OK && output_is(&test_fixture, current->expected_output);
        (void)sxml_test_record_check(state, (sxml_test_check){.passed = passed,
                                                              .expression = current->test_case_name,
                                                              .file_name = __FILE__,
                                                              .line_number = __LINE__});
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
}

/* --------------------------------------------------- Versions (3.4.1) */

typedef struct version_case {
    const char *package_version;
    const char *version_range;
    bool matches;
} version_case;

/* The examples of 3.4.1. */
static const version_case version_cases[] = {
    {"1.0", "*", true},
    {"1", "1.0", true},
    {"1.0.2", "1.0.3", false},
    {"9.6.1.2", "9.5.0.8, 9.6.1.2", true},
    {"9.7", "9.5.0.8, 9.6+", true},
    {"1.3.10.2", "1.3.*", true},
    {"1.3-beta", "1.3.*", true},
    {"1.35", "1.3.*", false},
    {"1.4", "1.3.*", false},
    {"1.3-beta", "1.3+", false},
    {"2.1", "1.3+", true},
    {"1.3-gamma", "1.3-beta+", true},
    {"1.3-alpha", "1.3-beta+", false},
    {"4.0-beta", "to 4.0", true},
    {"4.0.1", "to 4.0", false},
    {"3.3.4621", "to 3.3.*", true},
    {"3.4.0-beta", "to 3.3.*", false},
    {"5.0", "1 to 5", true},
    {"5.1", "1 to 5", false},
    {"5.7.2", "1 to 5.*", true},
    {"6.0-beta", "1 to 5.*", false},
    {"1.0-beta.2", "1.0-beta to 1.0", true},
    {"1.0.1", "1.0-beta to 1.0", false},
    {"2.0", "2.0-rc1+", true},
};

static void test_version_ranges_match_as_specified(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < sizeof(version_cases) / sizeof(version_cases[0]);
         case_index += 1) {
        const version_case *current = &version_cases[case_index];
        sxml_xslt_package_version_match match = {.matches = !current->matches};
        const sxml_status status = sxml_xslt_package_version_matches(
            (sxml_xslt_package_version_query){
                .package_version = sxml_byte_view_from_c_string(current->package_version),
                .version_range = sxml_byte_view_from_c_string(current->version_range)},
            &match);
        (void)sxml_test_record_check(state,
                                     (sxml_test_check){.passed = status == SXML_STATUS_OK &&
                                                                 match.matches == current->matches,
                                                       .expression = current->version_range,
                                                       .file_name = __FILE__,
                                                       .line_number = __LINE__});
    }
    sxml_xslt_package_version_match match = {.matches = false};
    SXML_TEST_EXPECT(state, sxml_xslt_package_version_matches(
                                (sxml_xslt_package_version_query){.package_version = TEXT("1.0"),
                                                                  .version_range = TEXT("1..0")},
                                &match) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_xslt_package_version_matches(
                                (sxml_xslt_package_version_query){.package_version = TEXT("1.0 x"),
                                                                  .version_range = TEXT("*")},
                                &match) == SXML_STATUS_INVALID_ARGUMENT);
}

static const sxml_test_case xslt_package_test_cases[] = {
    {.name = "packages_bind_their_components", .function = test_packages_bind_their_components},
    {.name = "version_ranges_match_as_specified",
     .function = test_version_ranges_match_as_specified},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_package",
            .cases = xslt_package_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_package_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
