/*
 * Tests for templates and modes (src/evaluator_template.c,
 * src/xslt_lower_template.c; D144, packet 4.8): template rules, conflict
 * resolution, modes and built-in templates, named templates and their
 * parameters, tunnel parameters, next-match, errors while matching, the
 * rule buckets rules are chosen from (perf-15), and apply-templates over
 * a tree deeper than any C stack.
 *
 * Golden file: tests/xslt_lower_test.c (read, lower, evaluate, write the
 * value by the adaptive method with items separated by a space,
 * "error:CODE" for an error). Stylesheets are read with
 * sxml_xslt_read_stylesheet and lowered with
 * sxml_xslt_lower_initial_template, invoked by applying templates to the
 * document node of a source document unless they have an
 * xsl:initial-template.
 */
#include "evaluator_internal.h"
#include "processor_internal.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "test_runner.h"
#include "xslt_lower_internal.h"
#include "xslt_read_internal.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ Fixture */

#define XSL_NAMESPACE "xmlns:xsl='http://www.w3.org/1999/XSL/Transform'"

/* A stylesheet of declarations. */
#define STYLESHEET(declarations)                                                                   \
    "<xsl:stylesheet version='4.0' " XSL_NAMESPACE ">" declarations "</xsl:stylesheet>"

static const char source_document[] =
    "<doc><item n='1'>one</item><item n='2'>two<b>bold</b></item><!--c--><?p q?></doc>";

/* What compiling and evaluating one stylesheet gave. */
typedef struct transform_outcome {
    sxml_status status;
    sxml_error_record error_record;
    char text[4096];
} transform_outcome;

/* Writes value by the adaptive method into outcome->text. */
static sxml_status serialize_value(sxml_processor *processor, sxml_sequence value,
                                   transform_outcome *outcome) {
    sxml_output_buffer *output_buffer = NULL;
    sxml_status status = sxml_output_buffer_create(
        sxml_default_allocator(),
        &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                            .maximum_byte_count = sizeof(outcome->text) - 1},
        &output_buffer);
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            processor->item_heap,
            &(sxml_serialization_request){
                .value = value,
                .serialization_parameters = {.serialization_method =
                                                 SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                             .has_item_separator = true,
                                             .item_separator = SXML_BYTE_VIEW_FROM_LITERAL(" ")},
                .error_record = &outcome->error_record},
            sxml_output_buffer_as_output_stream(output_buffer));
    }
    if (status == SXML_STATUS_OK) {
        const sxml_byte_view bytes = sxml_output_buffer_get_bytes(output_buffer);
        if (bytes.byte_count > 0) {
            memcpy(outcome->text, bytes.bytes, bytes.byte_count);
        }
        outcome->text[bytes.byte_count] = '\0';
    }
    sxml_output_buffer_destroy(output_buffer);
    return status;
}

/* One transformation to run. */
typedef struct transform_request {
    const char *stylesheet;
    /* NULL for source_document. */
    const char *source;
    size_t source_byte_count;
    /* Applies templates even when there is an xsl:initial-template. */
    bool applies_templates;
    /* No global context item: the focus of the initial template is
       absent. */
    bool has_no_context_item;
} transform_request;

/* A unit to evaluate for a request. */
typedef struct unit_evaluation {
    sxml_processor *processor;
    transform_request request;
    sxml_ir_unit *unit;
} unit_evaluation;

/* Evaluates unit with the source's document node as the context item. */
static sxml_status evaluate_unit(unit_evaluation evaluation, transform_outcome *outcome) {
    sxml_processor *processor = evaluation.processor;
    const transform_request request = evaluation.request;
    sxml_ir_unit *unit = evaluation.unit;
    const sxml_byte_view source =
        request.source == NULL
            ? sxml_byte_view_from_c_string(source_document)
            : (sxml_byte_view){.bytes = (const uint8_t *)request.source,
                               .byte_count = request.source_byte_count};
    sxml_item document = {.representation = 0};
    sxml_status status = sxml_processor_parse_document(
        processor,
        &(sxml_xml_parse_request){.input = source,
                                  .source_name = SXML_BYTE_VIEW_FROM_LITERAL("source.xml")},
        &document);
    sxml_dynamic_context *dynamic_context = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_create(
            sxml_default_allocator(),
            &(sxml_evaluator_configuration){.item_heap = processor->item_heap,
                                            .allocator = processor->allocator,
                                            .function_table =
                                                sxml_processor_get_function_table(processor)},
            &dynamic_context);
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){.unit = unit,
                                       .has_context_value = !request.has_no_context_item,
                                       .context_value = request.has_no_context_item
                                                            ? sxml_sequence_empty()
                                                            : sxml_sequence_from_item(document),
                                       .global_values = {.slots = NULL, .slot_count = 0},
                                       .static_base_uri = {.bytes = NULL, .byte_count = 0},
                                       .error_record = &outcome->error_record},
            &value);
    }
    if (status == SXML_STATUS_OK) {
        status = serialize_value(processor, value, outcome);
    }
    sxml_sequence_release(value);
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_item_release(document);
    return status;
}

/* Reads, lowers and evaluates the stylesheet; outcome->text is the value
   written, or "error:CODE" for an error with a code. */
static void run_transform(transform_request request, transform_outcome *outcome) {
    memset(outcome, 0, sizeof(*outcome));
    sxml_processor *processor = NULL;
    outcome->status = sxml_processor_create(
        sxml_default_allocator(),
        &(sxml_processor_configuration){.allocator = sxml_default_allocator()}, &processor);
    sxml_xslt_ast *ast = NULL;
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_xslt_read_stylesheet(
            &(sxml_xslt_read_request){
                .processor = processor,
                .principal_module_text = sxml_byte_view_from_c_string(request.stylesheet),
                .principal_module_uri = SXML_BYTE_VIEW_FROM_LITERAL("file:///test.xsl"),
                .error_record = &outcome->error_record},
            &ast);
    }
    sxml_ir_unit *unit = NULL;
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_xslt_lower_initial_template(
            &(sxml_xslt_lowering_request){.ast = ast,
                                          .processor = processor,
                                          .template_node = {.value = 0},
                                          .applies_templates = request.applies_templates,
                                          .builds_result_document = false,
                                          .error_record = &outcome->error_record},
            &unit);
    }
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = evaluate_unit(
            (unit_evaluation){.processor = processor, .request = request, .unit = unit}, outcome);
    }
    if (outcome->status != SXML_STATUS_OK) {
        if (sxml_status_has_error_record(outcome->status)) {
            const sxml_byte_view code = outcome->error_record.code.local_name;
            (void)snprintf(outcome->text, sizeof(outcome->text), "error:%.*s", (int)code.byte_count,
                           (const char *)code.bytes);
        } else {
            const sxml_byte_view description = outcome->error_record.diagnostic.description;
            (void)snprintf(outcome->text, sizeof(outcome->text), "status:%s %.*s",
                           (const char *)sxml_status_name(outcome->status).bytes,
                           (int)description.byte_count, (const char *)description.bytes);
        }
    }
    sxml_ir_unit_release(unit);
    sxml_xslt_ast_destroy(ast);
    if (processor != NULL) {
        sxml_item_heap_clear_raised_error(processor->item_heap);
    }
    sxml_processor_destroy(processor);
}

/* ---------------------------------------------------------- The cases */

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
        run_transform((transform_request){.stylesheet = list.cases[case_index].stylesheet,
                                          .source = NULL,
                                          .source_byte_count = 0,
                                          .applies_templates = false},
                      &outcome);
        const bool matches = strcmp(outcome.text, list.cases[case_index].expected_text) == 0;
        if (!matches) {
            printf("case %zu: expected %s, got %s\n", case_index,
                   list.cases[case_index].expected_text, outcome.text);
        }
        all_match = all_match && matches;
    }
    SXML_TEST_EXPECT(state, all_match);
    return all_match;
}

#define CHECK(state, cases)                                                                        \
    check_cases((state), (case_list){.cases = (cases), .case_count = SXML_TEST_COUNT(cases)})

/* 6.6, 6.8: rules chosen by priority and declaration order; positions. */
static void test_template_rules(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:template match='/'><r><xsl:apply-templates select='//item'/></r>"
                    "</xsl:template>"
                    "<xsl:template match='item'>[<xsl:value-of select='@n, position(), last()'/>]"
                    "</xsl:template>"),
         "<r>[1 1 2][2 2 2]</r>"},
        /* The higher priority wins; then the later declaration. */
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//item'/></xsl:template>"
                    "<xsl:template match='item' priority='2'>A</xsl:template>"
                    "<xsl:template match='item[@n=2]'>B</xsl:template>"
                    "<xsl:template match='item' priority='2'>C</xsl:template>"),
         "C C"},
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//item'/></xsl:template>"
                    "<xsl:template match='item'>A</xsl:template>"
                    "<xsl:template match='item[@n=2]'>B</xsl:template>"),
         "A B"},
        /* The separator (6.7) and atomic items. */
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='1 to 3' separator=','/>"
                    "</xsl:template>"
                    "<xsl:template match='.[. instance of xs:integer]' "
                    "xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
                    "<xsl:sequence select='. * 10'/></xsl:template>"),
         "10 , 20 , 30"},
        /* 6.5: an error while matching is no match. */
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//item'/></xsl:template>"
                    "<xsl:template match='item[error()]'>E</xsl:template>"
                    "<xsl:template match='item' priority='-1'>F</xsl:template>"),
         "F F"},
    };
    CHECK(state, cases);
}

/* perf-15: the rules tried for an item are those of its rule buckets
   (its node kind, or not an XNode, and its name), merged in the mode's
   order, so the rule chosen is the one the whole mode would give: names
   and kinds interleaved by priority, a named rule below a kind-wide one,
   next-match from one bucket into the other, items that are not XNodes,
   on-multiple-match across buckets, names in the default namespace. */
static void test_rule_buckets(sxml_test_state *state) {
    static const transform_case cases[] = {
        /* sxml gives the union one default priority, 0, so
           the later "item" wins over it for the second item too. */
        {STYLESHEET("<xsl:template match='/'><r><xsl:apply-templates select='//node() | //@*'/>"
                    "</r></xsl:template>"
                    "<xsl:template match='item[@n=\"2\"] | b'>U</xsl:template>"
                    "<xsl:template match='item'>I</xsl:template>"
                    "<xsl:template match='doc'>D</xsl:template>"
                    "<xsl:template match='*'>E</xsl:template>"
                    "<xsl:template match='@n'>A</xsl:template>"
                    "<xsl:template match='text()'>T</xsl:template>"
                    "<xsl:template match='node()' priority='-2'>N</xsl:template>"),
         "<r>DIATIATUTNN</r>"},
        {STYLESHEET("<xsl:template match='/'><r><xsl:apply-templates select='//item'/></r>"
                    "</xsl:template>"
                    "<xsl:template match='item' priority='-1'>L</xsl:template>"
                    "<xsl:template match='*[@n=\"1\"]'>K</xsl:template>"),
         "<r>KL</r>"},
        {STYLESHEET("<xsl:template match='/'><r><xsl:apply-templates select='//item | //text()'/>"
                    "</r></xsl:template>"
                    "<xsl:template match='item'>I<xsl:next-match/></xsl:template>"
                    "<xsl:template match='*' priority='-1'>E<xsl:next-match/></xsl:template>"
                    "<xsl:template match='text()' priority='-1'>T<xsl:next-match/></xsl:template>"
                    "<xsl:template match='node()' priority='-2'>N</xsl:template>"),
         "<r>IENTNIENTNTN</r>"},
        {STYLESHEET("<xsl:template match='/'><r><xsl:apply-templates select='1, //b, \"x\"'/>"
                    "</r></xsl:template>"
                    "<xsl:template match='.[. instance of xs:integer]' "
                    "xmlns:xs='http://www.w3.org/2001/XMLSchema'>int</xsl:template>"
                    "<xsl:template match='b'>B</xsl:template>"
                    "<xsl:template match='.' priority='-3'>any</xsl:template>"),
         "<r>intBany</r>"},
        {STYLESHEET("<xsl:mode on-multiple-match='fail'/>"
                    "<xsl:template match='/'><xsl:apply-templates select='//b'/></xsl:template>"
                    "<xsl:template match='b' priority='1'>B</xsl:template>"
                    "<xsl:template match='*' priority='1'>S</xsl:template>"),
         "error:XTDE0540"},
        {STYLESHEET("<xsl:mode on-multiple-match='fail'/>"
                    "<xsl:template match='/'><xsl:apply-templates select='//b'/></xsl:template>"
                    "<xsl:template match='b' priority='1'>B</xsl:template>"
                    "<xsl:template match='*[@n]' priority='1'>S</xsl:template>"),
         "B"},
        {"<xsl:stylesheet version='4.0' xpath-default-namespace='urn:x' " XSL_NAMESPACE ">"
         "<xsl:template match='/'><r><xsl:apply-templates select='//*:item'/></r></xsl:template>"
         "<xsl:template match='item' priority='5'>X</xsl:template>"
         "<xsl:template match='Q{}b'>B</xsl:template></xsl:stylesheet>",
         "<r>onetwoB</r>"},
    };
    CHECK(state, cases);
}

/* Appends one bucket to text as "category:name=positions", "item" for
   the name item_name, "*" for name 0; positions in the mode. */
typedef struct bucket_text {
    const sxml_ir_unit *unit;
    const sxml_ir_mode *mode;
    sxml_name_identifier item_name;
    char *text;
    size_t byte_count;
    size_t written;
} bucket_text;

static void append_bucket(bucket_text *output, const sxml_ir_rule_bucket *bucket) {
    const char *name = bucket->node_name.index == 0                         ? "*"
                       : bucket->node_name.index == output->item_name.index ? "item"
                                                                            : "?";
    output->written += (size_t)snprintf(output->text + output->written,
                                        output->byte_count - output->written, "%s%u:%s=",
                                        output->written == 0 ? "" : " ",
                                        (unsigned)bucket->item_category, name);
    const sxml_ir_rule_number_view numbers =
        sxml_ir_unit_get_rule_numbers(output->unit, bucket->rules);
    for (size_t number_index = 0; number_index < numbers.rule_number_count; number_index += 1) {
        output->written += (size_t)snprintf(
            output->text + output->written, output->byte_count - output->written, "%s%u",
            number_index == 0 ? "" : ",",
            (unsigned)(numbers.rule_numbers[number_index] -
                       output->mode->template_rules.first_template_rule_index));
    }
}

/* Writes the rule buckets of output->unit's mode 1 (the unnamed mode)
   into output->text: "1:item=0 3:*=1,3". */
static void describe_buckets(sxml_processor *processor, bucket_text *output) {
    output->mode = sxml_ir_unit_get_mode(output->unit, (sxml_ir_mode_index){.value = 1});
    output->written = 0;
    output->text[0] = '\0';
    if (sxml_name_pool_intern_name(sxml_item_heap_get_name_pool(processor->item_heap),
                                   &(sxml_name_request){.namespace_uri = {.index = 0},
                                                        .local_name =
                                                            SXML_BYTE_VIEW_FROM_LITERAL("item")},
                                   &output->item_name) != SXML_STATUS_OK) {
        return;
    }
    const sxml_ir_rule_bucket_view buckets =
        sxml_ir_unit_get_rule_buckets(output->unit, output->mode->rule_buckets);
    for (size_t bucket_index = 0; bucket_index < buckets.rule_bucket_count; bucket_index += 1) {
        append_bucket(output, &buckets.rule_buckets[bucket_index]);
    }
}

/* perf-15: a rule is only in the buckets of what its pattern may match.
   The mode's order is item (0), text() and * (-0.5, the later first),
   node() (-2): "item" is in the bucket of elements named item, "*" in
   that of elements of any name, "text()" in that of text nodes, node()
   in every category's (OTHER, 7, holds every rule). */
static void test_rule_bucket_contents(sxml_test_state *state) {
    sxml_processor *processor = NULL;
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_status status = sxml_processor_create(
        sxml_default_allocator(),
        &(sxml_processor_configuration){.allocator = sxml_default_allocator()}, &processor);
    sxml_xslt_ast *ast = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_read_stylesheet(
            &(sxml_xslt_read_request){
                .processor = processor,
                .principal_module_text = sxml_byte_view_from_c_string(
                    STYLESHEET("<xsl:template match='item'/><xsl:template match='*'/>"
                               "<xsl:template match='text()'/>"
                               "<xsl:template match='node()' priority='-2'/>")),
                .principal_module_uri = SXML_BYTE_VIEW_FROM_LITERAL("file:///test.xsl"),
                .error_record = &error_record},
            &ast);
    }
    sxml_ir_unit *unit = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lower_initial_template(
            &(sxml_xslt_lowering_request){.ast = ast,
                                          .processor = processor,
                                          .template_node = {.value = 0},
                                          .applies_templates = false,
                                          .builds_result_document = false,
                                          .error_record = &error_record},
            &unit);
    }
    char text[512] = "";
    if (status == SXML_STATUS_OK) {
        describe_buckets(processor, &(bucket_text){.unit = unit,
                                                   .mode = NULL,
                                                   .item_name = {.index = 0},
                                                   .text = text,
                                                   .byte_count = sizeof(text),
                                                   .written = 0});
    }
    const char *expected = "0:*=3 1:*=2,3 1:item=0 2:*=3 3:*=1,3 4:*=3 5:*=3 6:*=3 7:*=0,1,2,3";
    const bool matches = strcmp(text, expected) == 0;
    if (!matches) {
        printf("expected %s, got %s\n", expected, text);
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK && matches);
    sxml_ir_unit_release(unit);
    sxml_xslt_ast_destroy(ast);
    sxml_processor_destroy(processor);
}

/* 6.10: the built-in templates of each on-no-match. */
static void test_built_in_templates(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:template match='/'><r><xsl:apply-templates/></r></xsl:template>"),
         "<r>onetwobold</r>"},
        {STYLESHEET("<xsl:mode on-no-match='shallow-copy'/>"
                    "<xsl:template match='b'><i/></xsl:template>"),
         "<doc><item n=\"1\">one</item><item n=\"2\">two<i/></item><!--c--><?p q?></doc>"},
        {STYLESHEET("<xsl:mode on-no-match='deep-copy'/>"
                    "<xsl:template match='/'><xsl:apply-templates select='//item'/></xsl:template>"
                    "<xsl:template match='b'><i/></xsl:template>"),
         "<item n=\"1\">one</item> <item n=\"2\">two<b>bold</b></item>"},
        {STYLESHEET("<xsl:mode on-no-match='shallow-skip'/>"
                    "<xsl:template match='b'><i/></xsl:template>"),
         "<i/>"},
        {STYLESHEET("<xsl:mode on-no-match='deep-skip'/>"
                    "<xsl:template match='b'><i/></xsl:template>"),
         ""},
        {STYLESHEET("<xsl:mode on-no-match='fail'/>"
                    "<xsl:template match='/'><xsl:apply-templates/></xsl:template>"),
         "error:XTDE0555"},
        {STYLESHEET("<xsl:mode on-multiple-match='fail'/>"
                    "<xsl:template match='/'><xsl:apply-templates select='//item'/></xsl:template>"
                    "<xsl:template match='item'>A</xsl:template>"
                    "<xsl:template match='item[@n]'>B</xsl:template>"
                    "<xsl:template match='item[. = .]'>C</xsl:template>"),
         "error:XTDE0540"},
    };
    CHECK(state, cases);
}

/* 6.9: named modes, #current, #default, #all. */
static void test_modes(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:template match='/'>"
                    "<xsl:apply-templates select='//item' mode='m'/></xsl:template>"
                    "<xsl:template match='item' mode='m'>m<xsl:apply-templates select='b' "
                    "mode='#current'/></xsl:template>"
                    "<xsl:template match='b' mode='m'>B</xsl:template>"
                    "<xsl:template match='item'>u</xsl:template>"),
         "m m B"},
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//b' mode='x'/>"
                    "<xsl:apply-templates select='//b'/></xsl:template>"
                    "<xsl:template match='b' mode='#all'>all</xsl:template>"),
         "all all"},
        {"<xsl:stylesheet version='4.0' default-mode='d' " XSL_NAMESPACE ">"
         "<xsl:template match='/'><xsl:apply-templates select='//b'/>"
         "<xsl:apply-templates select='//b' mode='#unnamed'/></xsl:template>"
         "<xsl:template match='b'>d</xsl:template>"
         "<xsl:template match='b' mode='#unnamed'>u</xsl:template></xsl:stylesheet>",
         "d u"},
    };
    CHECK(state, cases);
}

/* 10.1, 9.2, 10.1.6: named templates, parameters, tunnel parameters. */
static void test_named_templates(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:template name='xsl:initial-template'>"
                    "<xsl:call-template name='t'><xsl:with-param name='p' select='2'/>"
                    "</xsl:call-template></xsl:template>"
                    "<xsl:template name='t'><xsl:param name='p'/><xsl:param name='q' select='$p + 1'/>"
                    "<xsl:sequence select='$p, $q, name(.)'/></xsl:template>"),
         "2 3 \"\""},
        {STYLESHEET("<xsl:template name='xsl:initial-template'><xsl:call-template name='t'/>"
                    "</xsl:template>"
                    "<xsl:template name='t'><xsl:param name='p' as='xs:integer' "
                    "xmlns:xs='http://www.w3.org/2001/XMLSchema'/>"
                    "</xsl:template>"),
         "error:XTDE0700"},
        {STYLESHEET("<xsl:template name='xsl:initial-template'><xsl:call-template name='u'/>"
                    "</xsl:template>"),
         "error:XTSE0650"},
        {STYLESHEET("<xsl:template name='xsl:initial-template'><xsl:call-template name='t'>"
                    "<xsl:with-param name='x' select='1'/></xsl:call-template></xsl:template>"
                    "<xsl:template name='t'/>"),
         "error:XTSE0680"},
        /* Tunnel parameters pass through templates that do not declare
           them. */
        {STYLESHEET("<xsl:template name='xsl:initial-template'>"
                    "<xsl:apply-templates select='/doc'><xsl:with-param name='t' select='7' "
                    "tunnel='yes'/></xsl:apply-templates></xsl:template>"
                    "<xsl:template match='doc'><xsl:apply-templates select='item[1]'/>"
                    "</xsl:template>"
                    "<xsl:template match='item'><xsl:call-template name='n'/></xsl:template>"
                    "<xsl:template name='n'><xsl:param name='t' tunnel='yes'/>"
                    "<xsl:param name='u' tunnel='yes' select='0'/>"
                    "<xsl:sequence select='$t, $u'/></xsl:template>"),
         "7 0"},
        /* A parameter with content and no as is a document node (9.4). */
        {STYLESHEET("<xsl:template name='xsl:initial-template'><xsl:call-template name='t'>"
                    "<xsl:with-param name='p'><a/></xsl:with-param></xsl:call-template>"
                    "</xsl:template>"
                    "<xsl:template name='t'><xsl:param name='p'/>"
                    "<xsl:sequence select='$p/a instance of element()'/></xsl:template>"),
         "true()"},
        /* The as of a template (6.1). */
        {STYLESHEET("<xsl:template name='xsl:initial-template' as='xs:integer' "
                    "xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
                    "<xsl:sequence select='\"a\"'/></xsl:template>"),
         "error:XTTE0505"},
    };
    CHECK(state, cases);
}

/* 6.11: next-match passes the parameters on; built-in fallback. */
static void test_next_match(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//item[2]'>"
                    "<xsl:with-param name='p' select='5'/></xsl:apply-templates></xsl:template>"
                    "<xsl:template match='item' priority='3'>a<xsl:next-match/></xsl:template>"
                    "<xsl:template match='item[@n]' priority='2'><xsl:param name='p'/>"
                    "<xsl:value-of select='$p'/><xsl:next-match/></xsl:template>"),
         "a 5 two bold"},
        {STYLESHEET("<xsl:template name='xsl:initial-template'><xsl:next-match/></xsl:template>"),
         "error:XTDE0560"},
    };
    CHECK(state, cases);
}

/* D71: a tree 100,000 levels deep is processed on the machine's stacks. */
static void test_deep_tree(sxml_test_state *state) {
    const size_t depth = 100000;
    static const char open_tag[] = "<a>";
    static const char close_tag[] = "</a>";
    const size_t byte_count = depth * (sizeof(open_tag) - 1 + sizeof(close_tag) - 1) + 1;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        sxml_default_allocator(),
        (sxml_allocation_request){.byte_count = byte_count, .alignment = 1}, &memory);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    if (status != SXML_STATUS_OK) {
        return;
    }
    char *source = memory;
    size_t written = 0;
    for (size_t level = 0; level < depth; level += 1) {
        memcpy(source + written, open_tag, sizeof(open_tag) - 1);
        written += sizeof(open_tag) - 1;
    }
    source[written] = 'x';
    written += 1;
    for (size_t level = 0; level < depth; level += 1) {
        memcpy(source + written, close_tag, sizeof(close_tag) - 1);
        written += sizeof(close_tag) - 1;
    }
    transform_outcome outcome;
    run_transform((transform_request){.stylesheet = STYLESHEET(
                                          "<xsl:template match='a'><xsl:param name='d' select='0'/>"
                                          "<xsl:apply-templates><xsl:with-param name='d' "
                                          "select='$d + 1'/></xsl:apply-templates></xsl:template>"
                                          "<xsl:template match='text()'><xsl:param name='d'/>"
                                          "<xsl:sequence select='$d'/></xsl:template>"),
                                      .source = source,
                                      .source_byte_count = written,
                                      .applies_templates = true},
                  &outcome);
    sxml_allocator_release(sxml_default_allocator(), memory);
    const bool matches = strcmp(outcome.text, "100000") == 0;
    if (!matches) {
        printf("deep tree: got %s\n", outcome.text);
    }
    SXML_TEST_EXPECT(state, matches);
}

/* 10.1.2, 10.1.4: without a global context item, the focus of a named
   template is absent: reading it raises XPDY0002, through call-template
   too; a template requiring one raises XTTE3090. */
static void test_absent_focus(sxml_test_state *state) {
    static const struct {
        const char *stylesheet;
        const char *expected_text;
    } cases[] = {
        {STYLESHEET("<xsl:template name='xsl:initial-template'><a/></xsl:template>"), "<a/>"},
        {STYLESHEET("<xsl:template name='xsl:initial-template'><xsl:sequence select='.'/>"
                    "</xsl:template>"),
         "error:XPDY0002"},
        {STYLESHEET("<xsl:template name='xsl:initial-template'><xsl:call-template name='t'/>"
                    "</xsl:template><xsl:template name='t'><xsl:sequence select='name(.)'/>"
                    "</xsl:template>"),
         "error:XPDY0002"},
        {STYLESHEET("<xsl:template name='xsl:initial-template'><xsl:context-item use='required'/>"
                    "<a/></xsl:template>"),
         "error:XTTE3090"},
    };
    bool all_match = true;
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        transform_outcome outcome;
        run_transform((transform_request){.stylesheet = cases[case_index].stylesheet,
                                          .source = NULL,
                                          .source_byte_count = 0,
                                          .applies_templates = false,
                                          .has_no_context_item = true},
                      &outcome);
        const bool matches = strcmp(outcome.text, cases[case_index].expected_text) == 0;
        if (!matches) {
            printf("case %zu: expected %s, got %s\n", case_index, cases[case_index].expected_text,
                   outcome.text);
        }
        all_match = all_match && matches;
    }
    SXML_TEST_EXPECT(state, all_match);
}

static const sxml_test_case evaluator_template_test_cases[] = {
    {.name = "absent_focus", .function = test_absent_focus},
    {.name = "template_rules", .function = test_template_rules},
    {.name = "rule_buckets", .function = test_rule_buckets},
    {.name = "rule_bucket_contents", .function = test_rule_bucket_contents},
    {.name = "built_in_templates", .function = test_built_in_templates},
    {.name = "modes", .function = test_modes},
    {.name = "named_templates", .function = test_named_templates},
    {.name = "next_match", .function = test_next_match},
    {.name = "deep_tree", .function = test_deep_tree},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "evaluator_template",
            .cases = evaluator_template_test_cases,
            .case_count = SXML_TEST_COUNT(evaluator_template_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
