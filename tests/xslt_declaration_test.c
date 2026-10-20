/*
 * Tests for stylesheet functions and attribute sets
 * (src/xslt_lower_declaration.c; D150, packet 4.12): functions of several
 * arities, optional parameters and their defaults, recursion deeper than
 * any C stack, keyword arguments and function items, the result type,
 * the absent focus of a body, functions in no namespace, the static
 * errors of declarations; attribute sets on literal result elements,
 * xsl:element and xsl:copy, sets of several declarations and sets using
 * sets, their focus, XTSE0710 and XTSE0720.
 *
 * Golden file: tests/evaluator_template_test.c (read, lower, evaluate,
 * write the value by the adaptive method with items separated by a space,
 * "error:CODE" for an error).
 */
#include "evaluator_internal.h"
#include "processor_internal.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "test_runner.h"
#include "xslt_lower_internal.h"
#include "xslt_read_internal.h"
#include "xslt_whitespace_internal.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ Fixture */

#define XSL_NAMESPACE "xmlns:xsl='http://www.w3.org/1999/XSL/Transform'"

/* A stylesheet of declarations. */
#define STYLESHEET(declarations)                                                                   \
    "<xsl:stylesheet version='4.0' " XSL_NAMESPACE " xmlns:f='urn:f' "                           \
    "xmlns:xs='http://www.w3.org/2001/XMLSchema' exclude-result-prefixes='f xs'>" declarations   \
    "</xsl:stylesheet>"

/* A stylesheet whose initial template evaluates select. */
#define EVALUATE(declarations, select)                                                             \
    STYLESHEET("<xsl:template name='xsl:initial-template'><xsl:sequence select=\"" select "\"/>"   \
               "</xsl:template>" declarations)

static const char source_document[] =
    "<doc><item n='1'>one</item><item n='2'>two<b>bold</b></item><!--c--><?p q?></doc>";

/* The most global variables a test stylesheet declares. */
#define MAXIMUM_GLOBAL_COUNT 16U

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
    const sxml_xslt_ast *ast;
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
    if (status == SXML_STATUS_OK) {
        /* 4.2: the host strips the global context item's document. */
        sxml_item stripped = {.representation = 0};
        status = sxml_xslt_strip_document(
            &(sxml_xslt_whitespace_stripping){.ast = evaluation.ast, .processor = processor},
            document, &stripped);
        sxml_item_release(document);
        document = status == SXML_STATUS_OK ? stripped : (sxml_item){.representation = 0};
    }
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
    /* No stylesheet parameter is supplied: every global is computed. */
    sxml_evaluator_slot global_slots[MAXIMUM_GLOBAL_COUNT];
    const size_t global_count =
        unit->global_variable_count > 1 ? unit->global_variable_count - 1U : 0U;
    for (size_t slot_index = 0; slot_index < MAXIMUM_GLOBAL_COUNT; slot_index += 1) {
        global_slots[slot_index] = (sxml_evaluator_slot){
            .value = sxml_sequence_empty(), .slot_state = SXML_EVALUATOR_SLOT_STATE_ABSENT};
    }
    if (status == SXML_STATUS_OK && global_count > MAXIMUM_GLOBAL_COUNT) {
        status = SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){.unit = unit,
                                       .has_context_value = !request.has_no_context_item,
                                       .context_value = request.has_no_context_item
                                                            ? sxml_sequence_empty()
                                                            : sxml_sequence_from_item(document),
                                       .global_values = {.slots = global_slots,
                                                         .slot_count = global_count},
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
            (unit_evaluation){
                .processor = processor, .request = request, .ast = ast, .unit = unit},
            outcome);
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

/* A case with its own source document or without a context item. */
typedef struct context_case {
    const char *stylesheet;
    const char *expected_text;
    /* NULL for source_document. */
    const char *source;
    bool has_no_context_item;
} context_case;

typedef struct case_list {
    const transform_case *cases;
    size_t case_count;
} case_list;

/* Runs one case; true when its text is the expected one. */
static bool check_case(size_t case_index, const context_case *tested) {
    transform_outcome outcome;
    run_transform((transform_request){.stylesheet = tested->stylesheet,
                                      .source = tested->source,
                                      .source_byte_count =
                                          tested->source == NULL ? 0 : strlen(tested->source),
                                      .applies_templates = false,
                                      .has_no_context_item = tested->has_no_context_item},
                  &outcome);
    const bool matches = strcmp(outcome.text, tested->expected_text) == 0;
    if (!matches) {
        printf("case %zu: expected %s, got %s\n", case_index, tested->expected_text, outcome.text);
    }
    return matches;
}

static bool check_cases(sxml_test_state *state, case_list list) {
    bool all_match = true;
    for (size_t case_index = 0; case_index < list.case_count; case_index += 1) {
        const context_case tested = {.stylesheet = list.cases[case_index].stylesheet,
                                     .expected_text = list.cases[case_index].expected_text,
                                     .source = NULL,
                                     .has_no_context_item = false};
        all_match = check_case(case_index, &tested) && all_match;
    }
    SXML_TEST_EXPECT(state, all_match);
    return all_match;
}

typedef struct context_case_list {
    const context_case *cases;
    size_t case_count;
} context_case_list;

static bool check_context_cases(sxml_test_state *state, context_case_list list) {
    bool all_match = true;
    for (size_t case_index = 0; case_index < list.case_count; case_index += 1) {
        all_match = check_case(case_index, &list.cases[case_index]) && all_match;
    }
    SXML_TEST_EXPECT(state, all_match);
    return all_match;
}

#define CHECK_CONTEXT(state, cases)                                                                \
    check_context_cases((state),                                                                   \
                        (context_case_list){.cases = (cases), .case_count = SXML_TEST_COUNT(cases)})

#define CHECK(state, cases)                                                                        \
    check_cases((state), (case_list){.cases = (cases), .case_count = SXML_TEST_COUNT(cases)})

/* 10.3: functions called from templates and from each other. */
static void test_functions(sxml_test_state *state) {
    static const transform_case cases[] = {
        {EVALUATE("<xsl:function name='f:add' as='xs:integer'>"
                  "<xsl:param name='a' as='xs:integer'/><xsl:param name='b' as='xs:integer'/>"
                  "<xsl:sequence select='$a + $b'/></xsl:function>",
                  "f:add(2, 3)"),
         "5"},
        /* Arguments are coerced to the parameter types (xs:double from
           an untyped value). */
        {EVALUATE("<xsl:function name='f:half'><xsl:param name='x' as='xs:double'/>"
                  "<xsl:sequence select='$x div 2'/></xsl:function>",
                  "f:half(xs:untypedAtomic('3'))"),
         "1.5e0"},
        /* Two functions of one name with other arities (10.3.4.1). */
        {EVALUATE("<xsl:function name='f:f'><xsl:sequence select='0'/></xsl:function>"
                  "<xsl:function name='f:f'><xsl:param name='a'/><xsl:sequence select='$a'/>"
                  "</xsl:function>"
                  "<xsl:function name='f:f'><xsl:param name='a'/><xsl:param name='b'/>"
                  "<xsl:sequence select='$a * $b'/></xsl:function>",
                  "f:f(), f:f(7), f:f(2, 4)"),
         "0 7 8"},
        /* Mutual recursion, declared after its use. */
        {EVALUATE("<xsl:function name='f:even' as='xs:boolean'><xsl:param name='n'/>"
                  "<xsl:sequence select='if ($n = 0) then true() else f:odd($n - 1)'/>"
                  "</xsl:function>"
                  "<xsl:function name='f:odd' as='xs:boolean'><xsl:param name='n'/>"
                  "<xsl:sequence select='if ($n = 0) then false() else f:even($n - 1)'/>"
                  "</xsl:function>",
                  "f:even(10), f:odd(10)"),
         "true() false()"},
        /* A sequence constructor as body: a temporary tree's text. */
        {EVALUATE("<xsl:function name='f:wrap'><xsl:param name='s'/>"
                  "<b><xsl:value-of select='$s'/></b></xsl:function>",
                  "f:wrap('x')"),
         "<b>x</b>"},
        /* Named function references and dynamic calls. */
        {EVALUATE("<xsl:function name='f:sq'><xsl:param name='x'/>"
                  "<xsl:sequence select='$x * $x'/></xsl:function>",
                  "(1 to 3) ! f:sq#1(.), for-each(4, f:sq#1)"),
         "1 4 9 16"},
        /* A call from a template rule with its focus. */
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//item'/></xsl:template>"
                    "<xsl:template match='item'><xsl:sequence select='f:label(.)'/></xsl:template>"
                    "<xsl:function name='f:label' as='xs:string'>"
                    "<xsl:param name='e' as='element()'/>"
                    "<xsl:sequence select='$e/@n || \":\" || name($e)'/></xsl:function>"),
         "\"1:item\" \"2:item\""},
        /* 10.3.6: a deterministic function gives the same nodes for
           identical arguments; a proactive one new nodes each time. */
        {EVALUATE("<xsl:function name='f:e' new-each-time='no'><xsl:param name='n'/>"
                  "<e><xsl:value-of select='$n'/></e></xsl:function>",
                  "count((1, 2, 1, 2) ! f:e(.) | ()), f:e(1) is f:e(1), f:e(1) is f:e(1.0)"),
         "2 true() false()"},
        {EVALUATE("<xsl:function name='f:e' new-each-time='yes' cache='yes'>"
                  "<xsl:param name='n'/><e/></xsl:function>",
                  "f:e(1) is f:e(1)"),
         "false()"},
    };
    CHECK(state, cases);
}

/* 10.3.1, 10.3.2: optional parameters and their defaults, keyword
   arguments. */
static void test_optional_parameters(sxml_test_state *state) {
    static const transform_case cases[] = {
        /* A parameter left out before a keyword argument is () (the
           core's rule for library functions, src/xpath_lower.c), not its
           default. */
        {EVALUATE("<xsl:function name='f:g'><xsl:param name='a'/>"
                  "<xsl:param name='b' required='no' select='10'/>"
                  "<xsl:param name='c' required='no' as='xs:string'>z</xsl:param>"
                  "<xsl:sequence select='$a, $b, $c'/></xsl:function>",
                  "f:g(1), f:g(1, 2), f:g(1, 2, 'y'), f:g(1, c := 'w')"),
         "1 10 \"z\" 1 2 \"z\" 1 2 \"y\" 1 \"w\""},
        /* No select and no content: the empty sequence. */
        {EVALUATE("<xsl:function name='f:h'><xsl:param name='a' required='no'/>"
                  "<xsl:sequence select='count($a)'/></xsl:function>",
                  "f:h(), f:h((1, 2))"),
         "0 2"},
        /* "." is the caller's context value. */
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//item'/></xsl:template>"
                    "<xsl:template match='item'><xsl:sequence select='f:n()'/></xsl:template>"
                    "<xsl:function name='f:n'><xsl:param name='e' required='no' select='.'/>"
                    "<xsl:sequence select='string($e/@n)'/></xsl:function>"),
         "\"1\" \"2\""},
        {EVALUATE("<xsl:function name='f:g'><xsl:param name='a' required='no' select='1'/>"
                  "<xsl:param name='b'/><xsl:sequence select='$a'/></xsl:function>",
                  "f:g(1)"),
         "error:XTSE0761"},
        {EVALUATE("<xsl:function name='f:g'><xsl:param name='a' select='1'/>"
                  "<xsl:sequence select='$a'/></xsl:function>",
                  "f:g(1)"),
         "error:XTSE0760"},
        {EVALUATE("<xsl:function name='f:g'><xsl:param name='a'/><xsl:param name='a'/>"
                  "<xsl:sequence select='$a'/></xsl:function>",
                  "f:g(1, 2)"),
         "error:XTSE0580"},
        /* 9.3: an explicit default of the wrong type is XTTE0600, the
           implicit () keeps the coercion's XPTY0004; also for template
           parameters. */
        {EVALUATE("<xsl:function name='f:g'><xsl:param name='a' as='xs:integer' required='no' "
                  "select='1.5'/><xsl:sequence select='$a'/></xsl:function>",
                  "f:g()"),
         "error:XTTE0600"},
        {EVALUATE("<xsl:function name='f:g'><xsl:param name='a' as='xs:integer' required='no'/>"
                  "<xsl:sequence select='$a'/></xsl:function>",
                  "f:g()"),
         "error:XPTY0004"},
        {STYLESHEET("<xsl:template match='/'><xsl:call-template name='t'/></xsl:template>"
                    "<xsl:template name='t'><xsl:param name='p' as='xs:integer' select='1.5'/>"
                    "<xsl:sequence select='$p'/></xsl:template>"),
         "error:XTTE0600"},
    };
    CHECK(state, cases);
}

/* 10.3.3: the result type and the absent focus of a body; 10.3.4: names,
   precedence and overlapping arity ranges; 10.3.1: functions in no
   namespace. */
static void test_function_rules(sxml_test_state *state) {
    static const transform_case cases[] = {
        {EVALUATE("<xsl:function name='f:bad' as='xs:integer'>"
                  "<xsl:sequence select='\"a\"'/></xsl:function>",
                  "f:bad()"),
         "error:XTTE0780"},
        /* A text node is atomized to the declared string type. */
        {EVALUATE("<xsl:function name='f:t' as='xs:string'><xsl:value-of select='1, 2'/>"
                  "</xsl:function>",
                  "f:t()"),
         "\"1 2\""},
        {EVALUATE("<xsl:function name='f:dot'><xsl:sequence select='.'/></xsl:function>",
                  "f:dot()"),
         "error:XPDY0002"},
        {EVALUATE("<xsl:function name='f:a'><xsl:sequence select='1'/></xsl:function>"
                  "<xsl:function name='f:a'><xsl:sequence select='2'/></xsl:function>",
                  "f:a()"),
         "error:XTSE0770"},
        {EVALUATE("<xsl:function name='f:a'><xsl:param name='x' required='no'/>"
                  "<xsl:sequence select='1'/></xsl:function>"
                  "<xsl:function name='f:a'><xsl:param name='x'/><xsl:param name='y'/>"
                  "<xsl:sequence select='2'/></xsl:function>"
                  "<xsl:function name='f:a'><xsl:param name='x'/>"
                  "<xsl:sequence select='3'/></xsl:function>",
                  "f:a()"),
         "error:XTSE0770"},
        {EVALUATE("<xsl:function name='xs:a'><xsl:sequence select='1'/></xsl:function>",
                  "1"),
         "error:XTSE0080"},
        {EVALUATE("<xsl:function name='f:a'><xsl:sequence select='1'/></xsl:function>",
                  "f:a(1)"),
         "error:XPST0017"},
        /* 4.0: a private function in no namespace, found before fn. */
        {EVALUATE("<xsl:function name='double'><xsl:param name='x'/>"
                  "<xsl:sequence select='2 * $x'/></xsl:function>"
                  "<xsl:function name='count'><xsl:param name='x'/>"
                  "<xsl:sequence select='\"mine\"'/></xsl:function>",
                  "double(4), count((1, 2)), sum((1, 2))"),
         "8 \"mine\" 3"},
        /* An attribute set of a function body: its focus is absent. */
        {EVALUATE("<xsl:function name='f:e'><e xsl:use-attribute-sets='s'/></xsl:function>"
                  "<xsl:attribute-set name='s'><xsl:attribute name='a'>1</xsl:attribute>"
                  "</xsl:attribute-set>",
                  "f:e()"),
         "<e a=\"1\"/>"},
    };
    CHECK(state, cases);
}

/* Recursion deeper than any C stack: each call is a frame of the
   evaluator's machine (D71, D75). */
static void test_deep_recursion(sxml_test_state *state) {
    static const transform_case cases[] = {
        {EVALUATE("<xsl:function name='f:sum' as='xs:integer'><xsl:param name='n'/>"
                  "<xsl:sequence select='if ($n = 0) then 0 else $n + f:sum($n - 1)'/>"
                  "</xsl:function>",
                  "f:sum(100000)"),
         "5000050000"},
    };
    CHECK(state, cases);
}

/* 9.5, 9.11: a global variable is computed after the globals the
   components its initializer calls read; an initializer may call a
   template. */
static void test_global_dependencies(sxml_test_state *state) {
    static const transform_case cases[] = {
        {EVALUATE("<xsl:variable name='a' select='f:g() + 1'/>"
                  "<xsl:function name='f:g'><xsl:sequence select='$b * 10'/></xsl:function>"
                  "<xsl:variable name='b' select='4'/>",
                  "$a"),
         "41"},
        {EVALUATE("<xsl:variable name='a'><xsl:call-template name='t'/></xsl:variable>"
                  "<xsl:template name='t'><xsl:value-of select='$b'/></xsl:template>"
                  "<xsl:variable name='b' select='5'/>",
                  "string($a)"),
         "\"5\""},
        {EVALUATE("<xsl:variable name='a'><e xsl:use-attribute-sets='s'/></xsl:variable>"
                  "<xsl:attribute-set name='s'><xsl:attribute name='x' select='$b'/>"
                  "</xsl:attribute-set>"
                  "<xsl:variable name='b' select='6'/>",
                  "string($a/e/@x)"),
         "\"6\""},
    };
    CHECK(state, cases);
}

/* 10.2: attribute sets on literal result elements, xsl:element and
   xsl:copy; later attributes of one name win (5.7.1). */
static void test_attribute_sets(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:template name='xsl:initial-template'>"
                    "<e xsl:use-attribute-sets='s t' b='lre'><xsl:attribute name='c'>own"
                    "</xsl:attribute></e></xsl:template>"
                    "<xsl:attribute-set name='s'><xsl:attribute name='a'>s</xsl:attribute>"
                    "<xsl:attribute name='b'>s</xsl:attribute></xsl:attribute-set>"
                    "<xsl:attribute-set name='t'><xsl:attribute name='c'>t</xsl:attribute>"
                    "<xsl:attribute name='a'>t</xsl:attribute></xsl:attribute-set>"),
         "<e a=\"t\" b=\"lre\" c=\"own\"/>"},
        /* Several declarations of one name, and sets using sets. */
        {STYLESHEET("<xsl:template name='xsl:initial-template'>"
                    "<xsl:element name='e' use-attribute-sets='s'/></xsl:template>"
                    "<xsl:attribute-set name='s' use-attribute-sets='u'>"
                    "<xsl:attribute name='a'>1</xsl:attribute></xsl:attribute-set>"
                    "<xsl:attribute-set name='u'><xsl:attribute name='u'>u</xsl:attribute>"
                    "<xsl:attribute name='a'>0</xsl:attribute></xsl:attribute-set>"
                    "<xsl:attribute-set name='s'><xsl:attribute name='b'>2</xsl:attribute>"
                    "</xsl:attribute-set>"),
         "<e u=\"u\" a=\"1\" b=\"2\"/>"},
        /* The focus of the element's content; of xsl:copy select's item. */
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//item'/>"
                    "<xsl:copy select='//b' use-attribute-sets='p'/></xsl:template>"
                    "<xsl:template match='item'><x xsl:use-attribute-sets='p'/></xsl:template>"
                    "<xsl:attribute-set name='p'><xsl:attribute name='at'"
                    " select='name(), position()'/></xsl:attribute-set>"),
         "<x at=\"item 1\"/> <x at=\"item 2\"/> <b at=\"b 1\"/>"},
        {STYLESHEET("<xsl:template name='xsl:initial-template'>"
                    "<e xsl:use-attribute-sets='none'/></xsl:template>"),
         "error:XTSE0710"},
        {STYLESHEET("<xsl:template name='xsl:initial-template'><e/></xsl:template>"
                    "<xsl:attribute-set name='s' use-attribute-sets='t'/>"
                    "<xsl:attribute-set name='t' use-attribute-sets='s'/>"),
         "error:XTSE0720"},
        /* 6.11, 10.2: an attribute set leaves the current template rule
           as it was, so xsl:next-match in it overrides the invoking rule;
           a template with context-item use="absent" has none. */
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//b'/></xsl:template>"
                    "<xsl:template match='b' priority='2'><x xsl:use-attribute-sets='n'/>"
                    "</xsl:template><xsl:template match='b'>low</xsl:template>"
                    "<xsl:attribute-set name='n'><xsl:attribute name='a'><xsl:next-match/>"
                    "</xsl:attribute></xsl:attribute-set>"),
         "<x a=\"low\"/>"},
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//b'/></xsl:template>"
                    "<xsl:template match='b'><xsl:call-template name='t'/></xsl:template>"
                    "<xsl:template name='t'><xsl:context-item use='absent'/><xsl:next-match/>"
                    "</xsl:template>"),
         "error:XTDE0560"},
        /* 10.1.4: XTSE3088 for as with use="absent". */
        {STYLESHEET("<xsl:template match='/'><xsl:call-template name='t'/></xsl:template>"
                    "<xsl:template name='t'><xsl:context-item as='node()' use='absent'/>"
                    "</xsl:template>"),
         "error:XTSE3088"},
    };
    CHECK(state, cases);
}

/* 4.2.2: xsl:strip-space and xsl:preserve-space on the source document,
   best match by precedence and priority, xml:space. */
static void test_whitespace_stripping(sxml_test_state *state) {
    static const char source[] = "<doc> <a> <b> </b> x </a> <c> </c>"
                                 "<d xml:space='preserve'> <e> </e> </d>"
                                 "<f xml:space='preserve'><g xml:space='default'> </g></f></doc>";
    static const context_case cases[] = {
        {STYLESHEET("<xsl:template match='/'><xsl:sequence select='count(//text())'/>"
                    "</xsl:template>"),
         "10", source, false},
        {STYLESHEET("<xsl:strip-space elements='*'/>"
                    "<xsl:template match='/'><xsl:sequence select='count(//text())'/>"
                    "</xsl:template>"),
         "4", source, false},
        /* A name beats "*"; xml:space="default" gives the declarations
           back. */
        {STYLESHEET("<xsl:strip-space elements='*'/><xsl:preserve-space elements='c'/>"
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='count(//c/text()), count(//g/text())'/>"
                    "</xsl:template>"),
         "1 0", source, false},
        {STYLESHEET("<xsl:strip-space elements='a'/><xsl:preserve-space elements='a'/>"
                    "<xsl:template match='/'/>"),
         "error:XTSE0270", source, false},
    };
    CHECK_CONTEXT(state, cases);
}

/* 3.4.9: xsl:global-context-item. */
static void test_global_context_item(sxml_test_state *state) {
    static const context_case cases[] = {
        {STYLESHEET("<xsl:global-context-item use='required' as='document-node()'/>"
                    "<xsl:template name='xsl:initial-template'><xsl:sequence select='name(*)'/>"
                    "</xsl:template>"),
         "\"doc\"", NULL, false},
        {STYLESHEET("<xsl:global-context-item use='required'/>"
                    "<xsl:template name='xsl:initial-template'><a/></xsl:template>"),
         "error:XTDE3086", NULL, true},
        {STYLESHEET("<xsl:global-context-item as='element()'/>"
                    "<xsl:template name='xsl:initial-template'><a/></xsl:template>"),
         "error:XTTE0590", NULL, false},
        {STYLESHEET("<xsl:global-context-item use='absent'/>"
                    "<xsl:template name='xsl:initial-template'><a/></xsl:template>"),
         "<a/>", NULL, true},
        {STYLESHEET("<xsl:global-context-item use='absent'/>"
                    "<xsl:template name='xsl:initial-template'><a/></xsl:template>"),
         "error:XTSE3088", NULL, false},
    };
    CHECK_CONTEXT(state, cases);
}

/* 5.5: xsl:decimal-format, merged by import precedence. */
static void test_decimal_formats(sxml_test_state *state) {
    static const transform_case cases[] = {
        {EVALUATE("<xsl:decimal-format name='f:de' decimal-separator=',' "
                  "grouping-separator='.'/>"
                  "<xsl:decimal-format NaN='none'/>",
                  "format-number(1234.5, '#.##0,00', 'f:de'), format-number(number('x'), '0')"),
         "\"1.234,50\" \"none\""},
        {EVALUATE("<xsl:decimal-format decimal-separator='a'/><xsl:decimal-format "
                  "decimal-separator='b'/>",
                  "1"),
         "error:XTSE1290"},
        {EVALUATE("<xsl:decimal-format decimal-separator=',' grouping-separator=','/>", "1"),
         "error:XTSE1300"},
    };
    CHECK(state, cases);
}

/* 5.4: named item types and nominative record types, in any order. */
static void test_named_types(sxml_test_state *state) {
    static const transform_case cases[] = {
        {EVALUATE("<xsl:function name='f:twice' as='f:number'><xsl:param name='n' as='f:number'/>"
                  "<xsl:sequence select='$n * 2'/></xsl:function>"
                  "<xsl:item-type name='f:number' as='f:integer'/>"
                  "<xsl:item-type name='f:integer' as='xs:integer'/>",
                  "f:twice(4), 4 instance of f:number, 'a' instance of f:number"),
         "8 true() false()"},
        {EVALUATE("<xsl:record-type name='f:point'><xsl:field name='x' as='xs:integer'/>"
                  "<xsl:field name='y' as='f:point?'/></xsl:record-type>",
                  "{'x': 1, 'y': {'x': 2, 'y': ()}} instance of f:point, "
                  "let $p as f:point := {'x': 1, 'y': {'x': 2, 'y': ()}} "
                  "return ($p instance of f:point, $p?y?x)"),
         "false() true() 2"},
        {EVALUATE("<xsl:record-type name='f:pair'><xsl:field name='a'/><xsl:field name='a'/>"
                  "</xsl:record-type>",
                  "1"),
         "error:XTSE4050"},
        {EVALUATE("<xsl:item-type name='f:a' as='f:b'/><xsl:item-type name='f:b' as='f:a'/>",
                  "1"),
         "error:XTSE4035"},
        {EVALUATE("<xsl:item-type name='f:a' as='f:undeclared'/>", "1"), "error:XPST0051"},
        {EVALUATE("<xsl:item-type name='f:a' as='xs:string'/>"
                  "<xsl:item-type name='f:a' as='xs:integer'/>",
                  "1"),
         "error:XTSE4030"},
    };
    CHECK(state, cases);
}

static const sxml_test_case xslt_declaration_test_cases[] = {
    {.name = "whitespace_stripping", .function = test_whitespace_stripping},
    {.name = "global_context_item", .function = test_global_context_item},
    {.name = "decimal_formats", .function = test_decimal_formats},
    {.name = "named_types", .function = test_named_types},
    {.name = "functions", .function = test_functions},
    {.name = "optional_parameters", .function = test_optional_parameters},
    {.name = "function_rules", .function = test_function_rules},
    {.name = "deep_recursion", .function = test_deep_recursion},
    {.name = "attribute_sets", .function = test_attribute_sets},
    {.name = "global_dependencies", .function = test_global_dependencies},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_declaration",
            .cases = xslt_declaration_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_declaration_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
