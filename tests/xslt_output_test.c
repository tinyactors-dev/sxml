/*
 * Tests for output definitions, character maps and result documents
 * (src/xslt_output.c; D156, packet 4.13): xsl:output merged into one
 * definition (XTSE1560), the method chosen from the result, character maps
 * and their expansion (XTSE1580, XTSE1590, XTSE1600), xsl:result-document
 * delivered through a result document collector (href, format, value
 * templates over the definition, XTDE1490, XTDE1480 lexically and through
 * called templates), the principal result and fn:current-output-uri.
 *
 * Golden file: tests/xslt_declaration_test.c (read, lower, evaluate; the
 * outcome as text, "error:CODE" for an error).
 */
#include "evaluator_internal.h"
#include "function_library_parts_internal.h"
#include "processor_internal.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xslt.h"
#include "test_runner.h"
#include "xslt_lower_internal.h"
#include "xslt_output_internal.h"
#include "xslt_read_internal.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ Fixture */

#define XSL_NAMESPACE "xmlns:xsl='http://www.w3.org/1999/XSL/Transform'"

/* A stylesheet of declarations and an initial template. */
#define STYLESHEET(declarations, body)                                                            \
    "<xsl:stylesheet version='4.0' " XSL_NAMESPACE " xmlns:f='urn:f' "                           \
    "exclude-result-prefixes='f'>" declarations                                                   \
    "<xsl:template name='xsl:initial-template'>" body "</xsl:template></xsl:stylesheet>"

#define BASE_OUTPUT_URI "file:///out/principal.xml"

/* What one transformation gave: the serialized principal result, then
   " | URI=bytes" for each result document. */
typedef struct transform_outcome {
    sxml_status status;
    sxml_error_record error_record;
    char text[4096];
} transform_outcome;

static void append_text(transform_outcome *outcome, sxml_byte_view text) {
    const size_t used = strlen(outcome->text);
    const size_t room = sizeof(outcome->text) - used - 1;
    const size_t byte_count = text.byte_count < room ? text.byte_count : room;
    if (byte_count > 0) {
        memcpy(outcome->text + used, text.bytes, byte_count);
    }
    outcome->text[used + byte_count] = '\0';
}

/* What evaluating needs besides the unit. */
typedef struct evaluation_setup {
    sxml_processor *processor;
    sxml_ir_unit *unit;
    sxml_xslt_result_writer *writer;
} evaluation_setup;

/* Evaluates the unit with the writer in the evaluation environment and
   writes the raw principal result to *value. */
static sxml_status evaluate_with_writer(const evaluation_setup *setup, transform_outcome *outcome,
                                        sxml_sequence *value) {
    sxml_processor *processor = setup->processor;
    sxml_function_library *library = NULL;
    sxml_status status = sxml_function_library_create(
        sxml_default_allocator(),
        &(sxml_function_library_configuration){
            .allocator = sxml_default_allocator(),
            .item_heap = processor->item_heap,
            .parts = *sxml_function_library_xslt_parts()},
        &library);
    sxml_processor_document_loader loader_memory;
    sxml_processor_document_loader *loader = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_document_loader_initialize(
            (sxml_memory_view){.bytes = (uint8_t *)&loader_memory,
                               .byte_count = sizeof(loader_memory)},
            &(sxml_processor_document_loader_configuration){
                .evaluation_environment = {.processor = processor,
                                           .resource_resolver = processor->resource_resolver,
                                           .trace_sink = processor->trace_sink,
                                           .default_language = SXML_BYTE_VIEW_FROM_LITERAL("en"),
                                           .result_writer = setup->writer}},
            &loader);
    }
    sxml_dynamic_context *dynamic_context = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_create(
            sxml_default_allocator(),
            &(sxml_evaluator_configuration){
                .item_heap = processor->item_heap,
                .allocator = processor->allocator,
                .document_loader = sxml_processor_document_loader_as_document_loader(loader),
                .function_table = sxml_function_library_as_table(library)},
            &dynamic_context);
    }
    sxml_evaluator_slot global_slots[4];
    for (size_t slot_index = 0; slot_index < 4; slot_index += 1) {
        global_slots[slot_index] = (sxml_evaluator_slot){
            .value = sxml_sequence_empty(), .slot_state = SXML_EVALUATOR_SLOT_STATE_ABSENT};
    }
    const size_t global_count =
        setup->unit->global_variable_count > 1 ? setup->unit->global_variable_count - 1U : 0U;
    if (status == SXML_STATUS_OK && global_count > 4) {
        status = SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){.unit = setup->unit,
                                       .has_context_value = false,
                                       .context_value = sxml_sequence_empty(),
                                       .global_values = {.slots = global_slots,
                                                         .slot_count = global_count},
                                       .static_base_uri = {.bytes = NULL, .byte_count = 0},
                                       .error_record = &outcome->error_record},
            value);
    }
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_processor_document_loader_finalize(loader);
    sxml_function_library_destroy(library);
    return status;
}

/* Delivers the principal result serialized and appends the collected
   result documents. */
typedef struct delivery_setup {
    sxml_processor *processor;
    sxml_xslt_result_writer *writer;
    sxml_xslt_result_document_collector *collector;
} delivery_setup;

static sxml_status deliver_and_collect(const delivery_setup *setup, sxml_sequence value,
                                       transform_outcome *outcome) {
    sxml_output_buffer *output_buffer = NULL;
    sxml_status status = sxml_output_buffer_create(
        sxml_default_allocator(),
        &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                            .maximum_byte_count = 2048},
        &output_buffer);
    sxml_sequence principal = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_result_writer_deliver_principal_result(
            setup->writer,
            &(sxml_xslt_principal_result_request){
                .value = value,
                .output_definition = NULL,
                .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED,
                .output_stream = sxml_output_buffer_as_output_stream(output_buffer),
                .error_record = &outcome->error_record},
            &principal);
    }
    if (status == SXML_STATUS_OK) {
        append_text(outcome, sxml_output_buffer_get_bytes(output_buffer));
        sxml_sequence_release(principal);
    }
    const size_t count =
        sxml_xslt_result_document_collector_get_result_document_count(setup->collector);
    for (size_t document_index = 0; status == SXML_STATUS_OK && document_index < count;
         document_index += 1) {
        sxml_xslt_collected_result_document document;
        status = sxml_xslt_result_document_collector_get_result_document(
            setup->collector, document_index, &document);
        if (status == SXML_STATUS_OK) {
            append_text(outcome, SXML_BYTE_VIEW_FROM_LITERAL(" | "));
            append_text(outcome, document.absolute_uri);
            append_text(outcome, SXML_BYTE_VIEW_FROM_LITERAL("="));
            append_text(outcome, document.serialized_bytes);
        }
    }
    sxml_output_buffer_destroy(output_buffer);
    return status;
}

/* The objects of one transformation. */
typedef struct transformation {
    sxml_processor *processor;
    sxml_xslt_ast *ast;
    sxml_xslt_stylesheet_outputs *outputs;
    sxml_ir_unit *unit;
    sxml_xslt_result_document_collector *collector;
    sxml_xslt_result_writer *writer;
} transformation;

static void release_transformation(transformation *objects) {
    sxml_xslt_result_writer_destroy(objects->writer);
    sxml_xslt_result_document_collector_destroy(objects->collector);
    sxml_ir_unit_release(objects->unit);
    sxml_xslt_stylesheet_outputs_destroy(objects->outputs);
    sxml_xslt_ast_destroy(objects->ast);
    if (objects->processor != NULL) {
        sxml_item_heap_clear_raised_error(objects->processor->item_heap);
    }
    sxml_processor_destroy(objects->processor);
}

/* Compiles the outputs, lowers, evaluates and delivers. */
static sxml_status run_compiled(transformation *objects, transform_outcome *outcome) {
    sxml_status status = sxml_xslt_stylesheet_outputs_create(
        sxml_default_allocator(),
        &(sxml_xslt_stylesheet_outputs_configuration){.ast = objects->ast,
                                                      .processor = objects->processor,
                                                      .error_record = &outcome->error_record},
        &objects->outputs);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lower_initial_template(
            &(sxml_xslt_lowering_request){.ast = objects->ast,
                                          .processor = objects->processor,
                                          .template_node = {.value = 0},
                                          .builds_result_document = false,
                                          .error_record = &outcome->error_record},
            &objects->unit);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_result_document_collector_create(
            sxml_default_allocator(),
            &(sxml_xslt_result_document_collector_configuration){
                .allocator = sxml_default_allocator(),
                .maximum_byte_count = 0,
                .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED},
            &objects->collector);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_result_writer_create(
            sxml_default_allocator(),
            &(sxml_xslt_result_writer_configuration){
                .outputs = objects->outputs,
                .processor = objects->processor,
                .result_document_handler =
                    sxml_xslt_result_document_collector_as_handler(objects->collector),
                .base_output_uri = SXML_BYTE_VIEW_FROM_LITERAL(BASE_OUTPUT_URI),
                .is_in_temporary_output_state = false,
                .prefers_xml_for_xhtml = false},
            &objects->writer);
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = evaluate_with_writer(
            &(evaluation_setup){
                .processor = objects->processor, .unit = objects->unit, .writer = objects->writer},
            outcome, &value);
    }
    if (status == SXML_STATUS_OK) {
        status = deliver_and_collect(&(delivery_setup){.processor = objects->processor,
                                                       .writer = objects->writer,
                                                       .collector = objects->collector},
                                     value, outcome);
    }
    sxml_sequence_release(value);
    return status;
}

static void run_transform(const char *stylesheet, transform_outcome *outcome) {
    memset(outcome, 0, sizeof(*outcome));
    transformation objects;
    memset(&objects, 0, sizeof(objects));
    outcome->status = sxml_processor_create(
        sxml_default_allocator(),
        &(sxml_processor_configuration){.allocator = sxml_default_allocator()},
        &objects.processor);
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_xslt_read_stylesheet(
            &(sxml_xslt_read_request){.processor = objects.processor,
                                      .principal_module_text =
                                          sxml_byte_view_from_c_string(stylesheet),
                                      .principal_module_uri =
                                          SXML_BYTE_VIEW_FROM_LITERAL("file:///test.xsl"),
                                      .error_record = &outcome->error_record},
            &objects.ast);
    }
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = run_compiled(&objects, outcome);
    }
    if (outcome->status != SXML_STATUS_OK) {
        if (sxml_status_has_error_record(outcome->status)) {
            const sxml_byte_view code = outcome->error_record.code.local_name;
            (void)snprintf(outcome->text, sizeof(outcome->text), "error:%.*s",
                           (int)code.byte_count, (const char *)code.bytes);
        } else {
            const sxml_byte_view description = outcome->error_record.diagnostic.description;
            (void)snprintf(outcome->text, sizeof(outcome->text), "status:%s %.*s",
                           (const char *)sxml_status_name(outcome->status).bytes,
                           (int)description.byte_count, (const char *)description.bytes);
        }
    }
    release_transformation(&objects);
}

typedef struct transform_case {
    const char *stylesheet;
    const char *expected_text;
} transform_case;

typedef struct case_list {
    const transform_case *cases;
    size_t case_count;
} case_list;

static bool check_cases(sxml_test_state *state, case_list list) {
    bool all_match = true;
    for (size_t case_index = 0; case_index < list.case_count; case_index += 1) {
        transform_outcome outcome;
        run_transform(list.cases[case_index].stylesheet, &outcome);
        const bool matches = strcmp(outcome.text, list.cases[case_index].expected_text) == 0;
        if (!matches) {
            printf("case %zu: expected %s, got %s\n", case_index,
                   list.cases[case_index].expected_text, outcome.text);
        }
        all_match = matches && all_match;
    }
    SXML_TEST_EXPECT(state, all_match);
    return all_match;
}

#define CHECK(state, cases)                                                                        \
    check_cases((state), (case_list){.cases = (cases), .case_count = SXML_TEST_COUNT(cases)})

/* ---------------------------------------------------------- The cases */

/* 26.1.1, 26.1.2: the unnamed definition, merged; the method chosen. */
static void test_output_definitions(sxml_test_state *state) {
    static const transform_case cases[] = {
        /* No xsl:output: xml with a declaration (omit-xml-declaration
           defaults to no). */
        {STYLESHEET("", "<a>x</a>"), "<?xml version=\"1.0\" encoding=\"UTF-8\"?><a>x</a>"},
        {STYLESHEET("<xsl:output method='text'/>", "<a>x</a><b>y</b>"), "xy"},
        /* Two declarations of one definition merge. */
        {STYLESHEET("<xsl:output method='xml'/><xsl:output omit-xml-declaration='yes'/>",
                    "<a/>"),
         "<a/>"},
        /* The same value twice is no conflict; different values are. */
        {STYLESHEET("<xsl:output method='text'/><xsl:output method='text'/>", "<a>1</a>"), "1"},
        {STYLESHEET("<xsl:output method='text'/><xsl:output method='xml'/>", "<a>1</a>"),
         "error:XTSE1560"},
        /* A map first: json. */
        {STYLESHEET("", "<xsl:sequence select=\"map{'a': 1}\"/>"), "{\"a\":1}"},
        /* item-separator, and "#absent". */
        {STYLESHEET("<xsl:output method='text' item-separator='|'/>",
                    "<xsl:sequence select='1 to 3'/>"),
         "1|2|3"},
        {STYLESHEET("<xsl:output method='text' item-separator='#absent'/>",
                    "<xsl:sequence select='1 to 3'/>"),
         "1 2 3"},
        /* A value the parameter does not permit is a static error. */
        {STYLESHEET("<xsl:output method='xml' encoding='no-such-encoding'/>", "<a/>"),
         "error:SESU0007"},
        /* cdata-section-elements of two declarations are one list. */
        {STYLESHEET("<xsl:output omit-xml-declaration='yes' cdata-section-elements='a'/>"
                    "<xsl:output cdata-section-elements='b'/>",
                    "<r><a>1</a><b>2</b></r>"),
         "<r><a><![CDATA[1]]></a><b><![CDATA[2]]></b></r>"},
    };
    CHECK(state, cases);
}

/* 26.1.6: character maps, used by xsl:output and by each other. */
static void test_character_maps(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:character-map name='m'><xsl:output-character character='a' "
                    "string='[A]'/></xsl:character-map>"
                    "<xsl:output method='text' use-character-maps='m'/>",
                    "<r>bab</r>"),
         "b[A]b"},
        /* A map's own characters come after those of the maps it uses. */
        {STYLESHEET("<xsl:character-map name='m' use-character-maps='n'>"
                    "<xsl:output-character character='a' string='1'/></xsl:character-map>"
                    "<xsl:character-map name='n'><xsl:output-character character='a' "
                    "string='2'/><xsl:output-character character='b' string='3'/>"
                    "</xsl:character-map>"
                    "<xsl:output method='text' use-character-maps='m'/>",
                    "<r>ab</r>"),
         "13"},
        /* Mapped characters are not escaped. */
        {STYLESHEET("<xsl:character-map name='m'><xsl:output-character character='&#xAB;' "
                    "string='&lt;'/></xsl:character-map>"
                    "<xsl:output omit-xml-declaration='yes' use-character-maps='m'/>",
                    "<r>&#xAB;b</r>"),
         "<r><b</r>"},
        {STYLESHEET("<xsl:output use-character-maps='none'/>", "<r/>"), "error:XTSE1590"},
        {STYLESHEET("<xsl:character-map name='m' use-character-maps='n'/>"
                    "<xsl:character-map name='n' use-character-maps='m'/>",
                    "<r/>"),
         "error:XTSE1600"},
        {STYLESHEET("<xsl:character-map name='m'/><xsl:character-map name='m'/>", "<r/>"),
         "error:XTSE1580"},
        /* fn:character-map (26.1.7). */
        {STYLESHEET("<xsl:character-map name='m'><xsl:output-character character='a' "
                    "string='x'/></xsl:character-map><xsl:output method='text'/>",
                    "<xsl:value-of select=\"character-map(xs:QName('m'))?a\" "
                    "xmlns:xs='http://www.w3.org/2001/XMLSchema'/>"),
         "x"},
    };
    CHECK(state, cases);
}

/* 26.2: result documents through the collector. */
static void test_result_documents(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:output method='text'/>",
                    "<xsl:result-document href='a.txt' method='text'>hello</xsl:result-document>"
                    "main"),
         "main | file:///out/a.txt=hello"},
        /* The definition named by format, attributes over it. */
        {STYLESHEET("<xsl:output method='text'/>"
                    "<xsl:output name='f:x' method='xml' omit-xml-declaration='yes'/>",
                    "<xsl:result-document href='{1 + 1}.xml' format='f:x'><a/>"
                    "</xsl:result-document>"
                    "<xsl:result-document href='3.xml' format='f:x' indent='yes'>"
                    "<a><b/></a></xsl:result-document>"),
         " | file:///out/2.xml=<a/> | file:///out/3.xml=<a>\n  <b/>\n</a>"},
        /* select instead of content. */
        {STYLESHEET("<xsl:output method='text'/>",
                    "<xsl:result-document href='s' method='text' select='1 to 2' "
                    "item-separator=','/>"),
         " | file:///out/s=1,2"},
        {STYLESHEET("",
                    "<xsl:result-document href='a'/><xsl:result-document href='a'/>"),
         "error:XTDE1490"},
        /* href="" is the principal result's URI: the result document is
           the principal result, delivered as one (26.2.2, D157); with a
           principal result that is not empty it is XTDE1490. */
        {STYLESHEET("<xsl:output method='text'/>",
                    "<xsl:result-document method='text'>x</xsl:result-document>"),
         "x"},
        {STYLESHEET("<xsl:output method='text'/>",
                    "<xsl:result-document method='text'>x</xsl:result-document>y"),
         "error:XTDE1490"},
        {STYLESHEET("", "<xsl:result-document href='a' format='f:none'/>"), "error:XTDE1460"},
        /* Temporary output state, lexically and through a called
           template. */
        {STYLESHEET("", "<xsl:variable name='v'><xsl:result-document href='a'/></xsl:variable>"
                        "<xsl:sequence select='$v'/>"),
         "error:XTDE1480"},
        {STYLESHEET("<xsl:template name='t'><xsl:result-document href='a'/></xsl:template>",
                    "<xsl:variable name='v'><xsl:call-template name='t'/></xsl:variable>"
                    "<xsl:sequence select='$v'/>"),
         "error:XTDE1480"},
        {STYLESHEET("<xsl:output method='text'/>"
                    "<xsl:template name='t'><xsl:result-document href='a' "
                    "method='text'>t</xsl:result-document></xsl:template>",
                    "<xsl:call-template name='t'/>"),
         " | file:///out/a=t"},
        /* A nested result document completes first. */
        {STYLESHEET("<xsl:output method='text'/>",
                    "<xsl:result-document href='outer' method='text'>o<xsl:result-document "
                    "href='inner' method='text'>i</xsl:result-document></xsl:result-document>"),
         " | file:///out/inner=i | file:///out/outer=o"},
    };
    CHECK(state, cases);
}

/* 26.1.8: disable-output-escaping, ignored in temporary output state
   and when cdata is present (11.4.3; D188). */
static void test_disabled_output_escaping(sxml_test_state *state) {
#define NO_DECLARATION "<xsl:output omit-xml-declaration='yes'/>"
    static const transform_case cases[] = {
        {STYLESHEET(NO_DECLARATION,
                    "<a>&lt;<xsl:text disable-output-escaping='yes'>&lt;b/&gt;</xsl:text>"
                    "<xsl:value-of select=\"'&amp;amp;'\" disable-output-escaping='yes'/></a>"),
         "<a>&lt;<b/>&amp;</a>"},
        /* A text node at the top of the result, and in a result document. */
        {STYLESHEET(NO_DECLARATION,
                    "<xsl:text disable-output-escaping='yes'>&lt;b/&gt;</xsl:text>"
                    "<xsl:result-document href='r'><xsl:text disable-output-escaping='yes'>"
                    "&lt;c/&gt;</xsl:text></xsl:result-document>"),
         "<b/> | file:///out/r=<c/>"},
        {STYLESHEET(NO_DECLARATION,
                    "<xsl:variable name='v'><a><xsl:value-of select=\"'&lt;'\" "
                    "disable-output-escaping='yes'/></a></xsl:variable>"
                    "<xsl:copy-of select='$v'/>"),
         "<a>&lt;</a>"},
        {STYLESHEET(NO_DECLARATION
                    "<xsl:template name='t'><xsl:text disable-output-escaping='yes'>&lt;"
                    "</xsl:text></xsl:template>",
                    "<xsl:variable name='v'><a><xsl:call-template name='t'/></a></xsl:variable>"
                    "<xsl:copy-of select='$v'/><b><xsl:call-template name='t'/></b>"),
         "<a>&lt;</a><b><</b>"},
        {STYLESHEET(NO_DECLARATION,
                    "<a><xsl:text cdata='no' disable-output-escaping='yes'>&lt;</xsl:text></a>"),
         "<a>&lt;</a>"},
        {STYLESHEET("<xsl:output method='text'/>",
                    "<a><xsl:text disable-output-escaping='yes'>&lt;</xsl:text></a>"),
         "<"},
    };
#undef NO_DECLARATION
    CHECK(state, cases);
}

/* 26.3: fn:current-output-uri. */
static void test_current_output_uri(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:output method='text'/>",
                    "<xsl:value-of select='current-output-uri()'/>"
                    "<xsl:result-document href='r' method='text'><xsl:value-of "
                    "select='current-output-uri()'/></xsl:result-document>"),
         BASE_OUTPUT_URI " | file:///out/r=file:///out/r"},
        /* Templates the content calls see the result document's URI. */
        {STYLESHEET("<xsl:output method='text'/>"
                    "<xsl:template name='t'><xsl:value-of select='current-output-uri()'/>"
                    "</xsl:template>",
                    "<xsl:result-document href='r' method='text'><xsl:call-template name='t'/>"
                    "</xsl:result-document>"),
         " | file:///out/r=file:///out/r"},
    };
    CHECK(state, cases);
}

static const sxml_test_case xslt_output_test_cases[] = {
    {.name = "output_definitions", .function = test_output_definitions},
    {.name = "character_maps", .function = test_character_maps},
    {.name = "result_documents", .function = test_result_documents},
    {.name = "disabled_output_escaping", .function = test_disabled_output_escaping},
    {.name = "current_output_uri", .function = test_current_output_uri},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_output",
            .cases = xslt_output_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_output_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
