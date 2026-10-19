/*
 * QT4 result assertions (assertion.h). Imitates tests/qt4/driver/
 * environment.c for walking catalog elements and apps/sxml/command_xpath.c
 * for compiling and evaluating through the public XPath API.
 *
 * No recursion (D71): all-of, any-of and not are walked with a fixed stack
 * of SXML_QT4_MAXIMUM_ASSERTION_DEPTH frames, each a cursor over the
 * children of one combination and the verdict combined so far.
 */
#include "assertion.h"

#include "reason.h"
#include "suite_path.h"
#include "sxml/name.h"
#include "sxml/serialize.h"
#include "sxml/string.h"
#include "sxml/types.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_qt4_empty_text = {.bytes = NULL,
                                                   .byte_count = 0};

/* The wrapper element of assert-xml's comparison. */
#define SXML_QT4_XML_WRAPPER "sxml-assert-xml"

/* ------------------------------------------------------------------ State */

typedef struct sxml_qt4_assertion_state {
    const sxml_qt4_assertion_request *request;
    sxml_item_heap *item_heap;
    sxml_name_identifier result_name;
    sxml_name_identifier expected_name;
    /* Errors raised by the assertions' own expressions. */
    sxml_error_record error_record;
} sxml_qt4_assertion_state;

/* An expression with $result and $expected bound. */
typedef struct sxml_qt4_bound_expression {
    sxml_byte_view expression_text;
    sxml_sequence result;
    sxml_sequence expected;
} sxml_qt4_bound_expression;

/*
 * Compiles and evaluates bound->expression_text and writes its value,
 * owned, to *value. Returns a status with an error record, filled in
 * state->error_record, for the expression's own errors, and the other
 * failures of compiling and evaluating.
 */
static sxml_status
sxml_qt4_assertion_evaluate(sxml_qt4_assertion_state *state,
                            const sxml_qt4_bound_expression *bound,
                            sxml_sequence *value) {
    const sxml_qt4_assertion_request *request = state->request;
    const sxml_sequence_type any_type = {
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
        .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
    const sxml_variable_declaration declarations[] = {
        {.variable_name = state->result_name, .declared_type = any_type},
        {.variable_name = state->expected_name, .declared_type = any_type},
    };
    const sxml_variable_binding bindings[] = {
        {.variable_name = state->result_name, .value = bound->result},
        {.variable_name = state->expected_name, .value = bound->expected},
    };
    sxml_static_context_configuration static_context = request->static_context;
    static_context.variable_declarations = (sxml_variable_declaration_view){
        .variable_declarations = declarations,
        .variable_declaration_count =
            sizeof declarations / sizeof declarations[0]};
    memset(&state->error_record, 0, sizeof state->error_record);
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        request->allocator,
        &(sxml_xpath_expression_configuration){
            .processor = request->processor,
            .expression_text = bound->expression_text,
            .source_name = SXML_TEXT("assertion"),
            .static_context = static_context,
            .error_record = &state->error_record},
        &expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_dynamic_context_configuration dynamic_context;
    memset(&dynamic_context, 0, sizeof dynamic_context);
    dynamic_context.variable_bindings = (sxml_variable_binding_view){
        .variable_bindings = bindings,
        .variable_binding_count = sizeof bindings / sizeof bindings[0]};
    /* The result is the context value of the assertion too, as QT4
       drivers bind it (`?get(1, 1) = "one"`; D141, D143). */
    dynamic_context.has_context_value = true;
    dynamic_context.context_value = bound->result;
    status = sxml_xpath_expression_evaluate(
        expression,
        &(sxml_xpath_evaluation_request){.dynamic_context = dynamic_context,
                                         .error_record = &state->error_record},
        value);
    sxml_xpath_expression_destroy(expression);
    return status;
}

/* True when value is exactly the xs:boolean true. */
static bool sxml_qt4_is_true(sxml_sequence value) {
    uint64_t item_count = 0;
    if (sxml_sequence_count(value, NULL, &item_count) != SXML_STATUS_OK ||
        item_count != 1) {
        return false;
    }
    sxml_item item;
    memset(&item, 0, sizeof item);
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof datum);
    return sxml_sequence_get_item(
               value, (sxml_item_request){.item_index = 0, .error_record = NULL},
               &item) == SXML_STATUS_OK &&
           sxml_item_get_kind(item) == SXML_ITEM_KIND_ATOMIC &&
           sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK &&
           datum.datum_kind == SXML_DATUM_KIND_BOOLEAN &&
           datum.datum_details.boolean;
}

/* Makes an xs:string item of text. Returns SXML_STATUS_INVALID_UTF8 for
   text that is not UTF-8, and the heap's failures. */
static sxml_status sxml_qt4_make_string(sxml_qt4_assertion_state *state,
                                        sxml_byte_view text,
                                        sxml_sequence *value) {
    sxml_string string;
    memset(&string, 0, sizeof string);
    sxml_status status = sxml_string_from_utf8(text, &string);
    sxml_item item;
    memset(&item, 0, sizeof item);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            state->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = string}}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *value = sxml_sequence_from_item(item);
    }
    return status;
}

/* --------------------------------------------------------------- Verdicts */

/* A verdict to write: its kind and the pieces of its reason. */
typedef struct sxml_qt4_verdict_text {
    sxml_qt4_verdict_kind verdict_kind;
    sxml_byte_view assertion_name;
    sxml_byte_view description;
} sxml_qt4_verdict_text;

/* Starts the reason "NAME: DESCRIPTION" of a verdict. */
static sxml_status sxml_qt4_start_reason(sxml_qt4_assertion_state *state,
                                         sxml_qt4_verdict_text text,
                                         sxml_qt4_reason *reason) {
    const sxml_status status =
        sxml_qt4_reason_start(state->request->arena, reason);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_qt4_reason_append(reason, text.assertion_name);
    sxml_qt4_reason_append(reason, SXML_TEXT(": "));
    sxml_qt4_reason_append(reason, text.description);
    return SXML_STATUS_OK;
}

static void sxml_qt4_finish_reason(sxml_qt4_verdict_kind verdict_kind,
                                   sxml_qt4_reason *reason,
                                   sxml_qt4_assertion_verdict *verdict) {
    *verdict = (sxml_qt4_assertion_verdict){
        .verdict_kind = verdict_kind,
        .reason = sxml_qt4_reason_text(reason)};
}

/* A verdict whose reason is the description and nothing more. */
static sxml_status sxml_qt4_simple_verdict(sxml_qt4_assertion_state *state,
                                           sxml_qt4_verdict_text text,
                                           sxml_qt4_assertion_verdict *verdict) {
    sxml_qt4_reason reason;
    const sxml_status status = sxml_qt4_start_reason(state, text, &reason);
    if (status == SXML_STATUS_OK) {
        sxml_qt4_finish_reason(text.verdict_kind, &reason, verdict);
    }
    return status;
}

/* A fail whose reason ends with the error just raised. */
typedef struct sxml_qt4_error_verdict {
    sxml_byte_view assertion_name;
    sxml_byte_view description;
    sxml_qt4_raised_error raised_error;
} sxml_qt4_error_verdict;

static sxml_status
sxml_qt4_fail_with_error(sxml_qt4_assertion_state *state,
                         const sxml_qt4_error_verdict *error_verdict,
                         sxml_qt4_assertion_verdict *verdict) {
    sxml_qt4_reason reason;
    const sxml_status status = sxml_qt4_start_reason(
        state,
        (sxml_qt4_verdict_text){.verdict_kind = SXML_QT4_VERDICT_FAIL,
                                .assertion_name = error_verdict->assertion_name,
                                .description = error_verdict->description},
        &reason);
    if (status == SXML_STATUS_OK) {
        sxml_qt4_reason_append_error(&reason, error_verdict->raised_error);
        sxml_qt4_finish_reason(SXML_QT4_VERDICT_FAIL, &reason, verdict);
    }
    return status;
}

/* A fail "NAME: got VALUE[, expected EXPECTED]". */
typedef struct sxml_qt4_value_verdict {
    sxml_byte_view assertion_name;
    sxml_sequence result;
    bool shows_expected;
    sxml_sequence expected;
} sxml_qt4_value_verdict;

static sxml_status
sxml_qt4_fail_with_value(sxml_qt4_assertion_state *state,
                         const sxml_qt4_value_verdict *value_verdict,
                         sxml_qt4_assertion_verdict *verdict) {
    sxml_qt4_reason reason;
    sxml_status status = sxml_qt4_start_reason(
        state,
        (sxml_qt4_verdict_text){.verdict_kind = SXML_QT4_VERDICT_FAIL,
                                .assertion_name = value_verdict->assertion_name,
                                .description = SXML_TEXT("got ")},
        &reason);
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_reason_append_value(
            &reason, (sxml_qt4_shown_value){.item_heap = state->item_heap,
                                            .value = value_verdict->result});
    }
    if (status == SXML_STATUS_OK && value_verdict->shows_expected) {
        sxml_qt4_reason_append(&reason, SXML_TEXT(", expected "));
        status = sxml_qt4_reason_append_value(
            &reason, (sxml_qt4_shown_value){.item_heap = state->item_heap,
                                            .value = value_verdict->expected});
    }
    if (status == SXML_STATUS_OK) {
        sxml_qt4_finish_reason(SXML_QT4_VERDICT_FAIL, &reason, verdict);
    }
    return status;
}

/* Turns a failed status into a verdict: the expression's own errors make
   a fail, everything else is returned. */
static sxml_status
sxml_qt4_fail_unless_failure(sxml_qt4_assertion_state *state,
                             const sxml_qt4_error_verdict *error_verdict,
                             sxml_qt4_assertion_verdict *verdict) {
    if (!sxml_status_has_error_record(error_verdict->raised_error.status)) {
        return error_verdict->raised_error.status;
    }
    return sxml_qt4_fail_with_error(state, error_verdict, verdict);
}

/* --------------------------------------------------------- Leaf elements */

/* One assertion element to check. */
typedef struct sxml_qt4_leaf {
    sxml_node_index element;
    sxml_byte_view name;
} sxml_qt4_leaf;

static sxml_status sxml_qt4_leaf_attribute(sxml_qt4_assertion_state *state,
                                           sxml_qt4_attribute_lookup lookup,
                                           sxml_qt4_attribute_value *value) {
    *value = (sxml_qt4_attribute_value){.is_present = false,
                                        .value = sxml_qt4_empty_text};
    return sxml_qt4_document_find_attribute(state->request->document, lookup,
                                            value);
}

static sxml_status sxml_qt4_leaf_text(sxml_qt4_assertion_state *state,
                                      sxml_node_index element,
                                      sxml_byte_view *text) {
    *text = sxml_qt4_empty_text;
    return sxml_qt4_document_get_text(
        state->request->document,
        &(sxml_qt4_text_request){.element = element,
                                 .arena = state->request->arena},
        text);
}

/* How a value assertion gets $expected. */
typedef enum sxml_qt4_expected_form {
    SXML_QT4_EXPECTED_NONE,
    /* The element's text is an XPath expression. */
    SXML_QT4_EXPECTED_EXPRESSION,
    /* The element's text itself, as xs:string. */
    SXML_QT4_EXPECTED_TEXT,
} sxml_qt4_expected_form;

/* A value assertion: $expected from the element, then a check that must
   be true. */
typedef struct sxml_qt4_value_assertion {
    sxml_byte_view name;
    sxml_byte_view check_text;
    sxml_qt4_expected_form expected_form;
    /* The reason of a fail shows $expected. */
    bool shows_expected;
} sxml_qt4_value_assertion;

/* catalog-schema.xsd, the documentation of each element. */
static const sxml_qt4_value_assertion sxml_qt4_value_assertions[] = {
    {.name = SXML_BYTE_VIEW_INITIALIZER("assert"),
     .expected_form = SXML_QT4_EXPECTED_EXPRESSION,
     .check_text = SXML_BYTE_VIEW_INITIALIZER("boolean($expected)"),
     .shows_expected = false},
    /* "equal to the result ... under the rules of the XPath 'eq'
       operator", 12 equal to the untyped atomic "12.0": "=" casts
       xs:untypedAtomic to the other operand's type (XPath 4.0, section
       4.8.2); deep-equal adds NaN = NaN. "eq" atomizes its operands, so
       a single node equals its typed value (QT4 string-constructor-027,
       switch-019 expect it; D135 (f)). */
    {.name = SXML_BYTE_VIEW_INITIALIZER("assert-eq"),
     .expected_form = SXML_QT4_EXPECTED_EXPRESSION,
     .check_text = SXML_BYTE_VIEW_INITIALIZER(
         "count($result) eq 1 and ($result instance of xs:anyAtomicType "
         "or $result instance of node()) "
         "and count($expected) eq 1 "
         "and (data($result) = $expected or deep-equal(data($result), $expected))"),
     .shows_expected = true},
    {.name = SXML_BYTE_VIEW_INITIALIZER("assert-deep-eq"),
     .expected_form = SXML_QT4_EXPECTED_EXPRESSION,
     .check_text =
         SXML_BYTE_VIEW_INITIALIZER("deep-equal($result, $expected)"),
     .shows_expected = true},
    /* Some reordering is deep-equal: every item occurs as often in both
       (deep-equal makes NaN equal to NaN). */
    {.name = SXML_BYTE_VIEW_INITIALIZER("assert-permutation"),
     .expected_form = SXML_QT4_EXPECTED_EXPRESSION,
     .check_text = SXML_BYTE_VIEW_INITIALIZER(
         "count($result) eq count($expected) and "
         "(every $sxml-item in $result satisfies "
         "count($result[deep-equal(., $sxml-item)]) eq "
         "count($expected[deep-equal(., $sxml-item)]))"),
     .shows_expected = true},
    {.name = SXML_BYTE_VIEW_INITIALIZER("assert-count"),
     .expected_form = SXML_QT4_EXPECTED_EXPRESSION,
     .check_text = SXML_BYTE_VIEW_INITIALIZER("count($result) eq $expected"),
     .shows_expected = false},
    /* "not an assertion on the effective boolean value". */
    {.name = SXML_BYTE_VIEW_INITIALIZER("assert-true"),
     .expected_form = SXML_QT4_EXPECTED_NONE,
     .check_text = SXML_BYTE_VIEW_INITIALIZER(
         "$result instance of xs:boolean and $result"),
     .shows_expected = false},
    {.name = SXML_BYTE_VIEW_INITIALIZER("assert-false"),
     .expected_form = SXML_QT4_EXPECTED_NONE,
     .check_text = SXML_BYTE_VIEW_INITIALIZER(
         "$result instance of xs:boolean and not($result)"),
     .shows_expected = false},
    {.name = SXML_BYTE_VIEW_INITIALIZER("assert-empty"),
     .expected_form = SXML_QT4_EXPECTED_NONE,
     .check_text = SXML_BYTE_VIEW_INITIALIZER("empty($result)"),
     .shows_expected = false},
    /* "string-join(for $r in $result return string($r), " ")". */
    {.name = SXML_BYTE_VIEW_INITIALIZER("assert-string-value"),
     .expected_form = SXML_QT4_EXPECTED_TEXT,
     .check_text = SXML_BYTE_VIEW_INITIALIZER(
         "string-join($result ! string(.), ' ') eq $expected"),
     .shows_expected = true},
};

/* The check of assert-string-value normalize-space="true". */
static const sxml_byte_view sxml_qt4_normalized_string_check =
    SXML_BYTE_VIEW_INITIALIZER(
        "normalize-space(string-join($result ! string(.), ' ')) eq "
        "normalize-space($expected)");

/* What one value assertion works with. */
typedef struct sxml_qt4_value_check {
    const sxml_qt4_value_assertion *assertion;
    sxml_node_index element;
    sxml_byte_view check_text;
} sxml_qt4_value_check;

/* $expected of a check, or a fail verdict when has_verdict is true. */
typedef struct sxml_qt4_expected_answer {
    sxml_sequence expected;
    bool has_verdict;
    sxml_qt4_assertion_verdict verdict;
} sxml_qt4_expected_answer;

static sxml_status sxml_qt4_make_expected(sxml_qt4_assertion_state *state,
                                          const sxml_qt4_value_check *check,
                                          sxml_qt4_expected_answer *answer) {
    sxml_qt4_assertion_verdict *verdict = &answer->verdict;
    answer->expected = sxml_sequence_empty();
    answer->has_verdict = false;
    if (check->assertion->expected_form == SXML_QT4_EXPECTED_NONE) {
        return SXML_STATUS_OK;
    }
    sxml_byte_view text = sxml_qt4_empty_text;
    sxml_status status = sxml_qt4_leaf_text(state, check->element, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (check->assertion->expected_form == SXML_QT4_EXPECTED_TEXT) {
        status = sxml_qt4_make_string(state, text, &answer->expected);
        if (status == SXML_STATUS_INVALID_UTF8) {
            answer->has_verdict = true;
            return sxml_qt4_simple_verdict(
                state,
                (sxml_qt4_verdict_text){
                    .verdict_kind = SXML_QT4_VERDICT_FAIL,
                    .assertion_name = check->assertion->name,
                    .description = SXML_TEXT("the expected text is not UTF-8")},
                verdict);
        }
        return status;
    }
    status = sxml_qt4_assertion_evaluate(
        state,
        &(sxml_qt4_bound_expression){
            .expression_text = text,
            .result = state->request->evaluation.value,
            .expected = sxml_sequence_empty()},
        &answer->expected);
    if (status != SXML_STATUS_OK) {
        answer->has_verdict = true;
        return sxml_qt4_fail_unless_failure(
            state,
            &(sxml_qt4_error_verdict){
                .assertion_name = check->assertion->name,
                .description = SXML_TEXT("the expected value raised "),
                .raised_error = {.status = status,
                                 .error_record = &state->error_record}},
            verdict);
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_qt4_check_value(sxml_qt4_assertion_state *state,
                                        const sxml_qt4_value_check *check,
                                        sxml_qt4_assertion_verdict *verdict) {
    const sxml_sequence result = state->request->evaluation.value;
    sxml_qt4_expected_answer answer = {
        .expected = sxml_sequence_empty(),
        .has_verdict = false,
        .verdict = {.verdict_kind = SXML_QT4_VERDICT_FAIL,
                    .reason = sxml_qt4_empty_text}};
    sxml_status status = sxml_qt4_make_expected(state, check, &answer);
    if (status != SXML_STATUS_OK || answer.has_verdict) {
        if (status == SXML_STATUS_OK) {
            *verdict = answer.verdict;
        }
        return status;
    }
    sxml_sequence check_value = sxml_sequence_empty();
    status = sxml_qt4_assertion_evaluate(
        state,
        &(sxml_qt4_bound_expression){.expression_text = check->check_text,
                                     .result = result,
                                     .expected = answer.expected},
        &check_value);
    if (status != SXML_STATUS_OK) {
        status = sxml_qt4_fail_unless_failure(
            state,
            &(sxml_qt4_error_verdict){
                .assertion_name = check->assertion->name,
                .description = SXML_TEXT("comparing raised "),
                .raised_error = {.status = status,
                                 .error_record = &state->error_record}},
            verdict);
    } else if (sxml_qt4_is_true(check_value)) {
        *verdict = (sxml_qt4_assertion_verdict){
            .verdict_kind = SXML_QT4_VERDICT_PASS,
            .reason = sxml_qt4_empty_text};
    } else {
        status = sxml_qt4_fail_with_value(
            state,
            &(sxml_qt4_value_verdict){
                .assertion_name = check->assertion->name,
                .result = result,
                .shows_expected = check->assertion->shows_expected,
                .expected = answer.expected},
            verdict);
    }
    sxml_sequence_release(check_value);
    sxml_sequence_release(answer.expected);
    return status;
}

/* assert-type: "$result instance of TYPE" (catalog-schema.xsd). */
static sxml_status sxml_qt4_check_type(sxml_qt4_assertion_state *state,
                                       const sxml_qt4_leaf *leaf,
                                       sxml_qt4_assertion_verdict *verdict) {
    static const sxml_qt4_value_assertion type_assertion = {
        .name = SXML_BYTE_VIEW_INITIALIZER("assert-type"),
        .expected_form = SXML_QT4_EXPECTED_NONE,
        .check_text = SXML_BYTE_VIEW_INITIALIZER(""),
        .shows_expected = false};
    sxml_byte_view type_text = sxml_qt4_empty_text;
    sxml_status status = sxml_qt4_leaf_text(state, leaf->element, &type_text);
    sxml_byte_view check_text = sxml_qt4_empty_text;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_concatenate(
            state->request->arena,
            (sxml_qt4_concatenation){.first =
                                         SXML_TEXT("$result instance of "),
                                     .second = type_text},
            &check_text);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_qt4_check_value(
        state,
        &(sxml_qt4_value_check){.assertion = &type_assertion,
                                .element = leaf->element,
                                .check_text = check_text},
        verdict);
}

/* ------------------------------------------------------------- assert-xml */

/* The text of assert-xml: the element's or its file's. */
typedef struct sxml_qt4_expected_xml {
    sxml_byte_view text;
    /* File contents to release to the request's allocator; NULL for
       none. */
    void *file_contents;
} sxml_qt4_expected_xml;

static sxml_status sxml_qt4_read_expected_xml(sxml_qt4_assertion_state *state,
                                              const sxml_qt4_leaf *leaf,
                                              sxml_qt4_expected_xml *expected) {
    const sxml_qt4_assertion_request *request = state->request;
    sxml_qt4_attribute_value file = {.is_present = false,
                                     .value = sxml_qt4_empty_text};
    sxml_status status = sxml_qt4_leaf_attribute(
        state,
        (sxml_qt4_attribute_lookup){.element = leaf->element,
                                    .local_name = SXML_TEXT("file")},
        &file);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!file.is_present) {
        expected->file_contents = NULL;
        return sxml_qt4_leaf_text(state, leaf->element, &expected->text);
    }
    sxml_byte_view suite_path = sxml_qt4_empty_text;
    status = sxml_qt4_suite_path_resolve(
        &(sxml_qt4_suite_path_request){
            .containing_file = request->test_set_file_path,
            .reference = file.value,
            .arena = request->arena},
        &suite_path);
    sxml_byte_view contents = sxml_qt4_empty_text;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_catalog_read_file(
            request->catalog,
            &(sxml_qt4_file_request){.suite_path = suite_path,
                                     .allocator = request->allocator},
            &contents);
    }
    if (status == SXML_STATUS_OK) {
        expected->text = contents;
        expected->file_contents = (void *)contents.bytes;
    }
    return status;
}

static bool sxml_qt4_is_xml_space(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

/* Drops whitespace at both ends: outside the document element of a whole
   document it is not content (XML 1.0, [1] Misc). */
static sxml_byte_view sxml_qt4_trim_space(sxml_byte_view text) {
    while (text.byte_count != 0 && sxml_qt4_is_xml_space(text.bytes[0])) {
        text.bytes += 1;
        text.byte_count -= 1;
    }
    while (text.byte_count != 0 &&
           sxml_qt4_is_xml_space(text.bytes[text.byte_count - 1])) {
        text.byte_count -= 1;
    }
    return text;
}

/* Drops a byte order mark and an XML declaration (XML 1.0, [23]) from the
   start of text, which goes inside a wrapper element. */
static sxml_byte_view sxml_qt4_strip_declaration(sxml_byte_view text) {
    static const uint8_t byte_order_mark[] = {0xEF, 0xBB, 0xBF};
    if (text.byte_count >= sizeof byte_order_mark &&
        memcmp(text.bytes, byte_order_mark, sizeof byte_order_mark) == 0) {
        text.bytes += sizeof byte_order_mark;
        text.byte_count -= sizeof byte_order_mark;
    }
    const sxml_byte_view start = SXML_TEXT("<?xml");
    if (text.byte_count <= start.byte_count ||
        memcmp(text.bytes, start.bytes, start.byte_count) != 0 ||
        !sxml_qt4_is_xml_space(text.bytes[start.byte_count])) {
        return text;
    }
    for (size_t byte_index = start.byte_count; byte_index + 1 < text.byte_count;
         byte_index += 1) {
        if (text.bytes[byte_index] == '?' && text.bytes[byte_index + 1] == '>') {
            return sxml_qt4_trim_space(
                (sxml_byte_view){.bytes = text.bytes + byte_index + 2,
                                 .byte_count = text.byte_count - byte_index - 2});
        }
    }
    return text;
}

/* Writes an atomic item's string value with &, < and > escaped
   (Serialization 4.0, section 7.3, text nodes). */
static sxml_status sxml_qt4_write_escaped_string(sxml_output_stream stream,
                                                 sxml_byte_view text) {
    size_t run_start = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t byte_index = 0;
         byte_index < text.byte_count && status == SXML_STATUS_OK;
         byte_index += 1) {
        sxml_byte_view replacement = sxml_qt4_empty_text;
        switch (text.bytes[byte_index]) {
        case '&':
            replacement = SXML_TEXT("&amp;");
            break;
        case '<':
            replacement = SXML_TEXT("&lt;");
            break;
        case '>':
            replacement = SXML_TEXT("&gt;");
            break;
        case '\r':
            replacement = SXML_TEXT("&#xD;");
            break;
        default:
            continue;
        }
        status = sxml_output_stream_write(
            stream, (sxml_byte_view){.bytes = text.bytes + run_start,
                                     .byte_count = byte_index - run_start});
        if (status == SXML_STATUS_OK) {
            status = sxml_output_stream_write(stream, replacement);
        }
        run_start = byte_index + 1;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_output_stream_write(
            stream, (sxml_byte_view){.bytes = text.bytes + run_start,
                                     .byte_count = text.byte_count - run_start});
    }
    return status;
}

/*
 * Writes value, whose items are nodes and xs:string items, as the XML
 * output method writes a sequence (Serialization 4.0, section 2,
 * "Sequence Normalization": adjacent strings separated by a space become
 * text), to stream: strings escaped, nodes by the adaptive method, which
 * writes nodes as the XML method does.
 */
typedef struct sxml_qt4_xml_writing {
    sxml_item_heap *item_heap;
    sxml_sequence value;
    sxml_error_record *error_record;
    /* Writes nodes with version 1.1, which has references for the C0
       controls (Serialization 4.0, 5.1.2). */
    bool is_xml_version_1_1;
} sxml_qt4_xml_writing;

static sxml_status sxml_qt4_write_as_xml(const sxml_qt4_xml_writing *writing,
                                         sxml_output_stream stream) {
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(writing->value);
    bool was_string = false;
    for (;;) {
        sxml_iteration_step step = {.has_item = false};
        sxml_status status = sxml_sequence_iterator_next(
            &iterator, writing->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            return status;
        }
        if (sxml_item_get_kind(step.item) == SXML_ITEM_KIND_ATOMIC) {
            sxml_atomic_datum datum;
            memset(&datum, 0, sizeof datum);
            status = sxml_atomic_item_get_datum(step.item, &datum);
            if (status == SXML_STATUS_OK && was_string) {
                status = sxml_output_stream_write(stream, SXML_TEXT(" "));
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_qt4_write_escaped_string(
                    stream, datum.datum_details.string.utf8);
            }
            was_string = true;
        } else {
            status = sxml_serialize_sequence(
                writing->item_heap,
                &(sxml_serialization_request){
                    .value = sxml_sequence_from_item(step.item),
                    .serialization_parameters =
                        {.serialization_method =
                             SXML_SERIALIZATION_METHOD_ADAPTIVE,
                         .has_item_separator = false,
                         .item_separator = sxml_qt4_empty_text,
                         .has_version = writing->is_xml_version_1_1,
                         .version = writing->is_xml_version_1_1
                                        ? SXML_TEXT("1.1")
                                        : sxml_qt4_empty_text},
                    .error_record = writing->error_record},
                stream);
            was_string = false;
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
}

/* Both texts parsed inside a wrapper element, then deep-equal with
   comments and processing instructions (F&O 4.0, section 14.2.4.1). */
static const sxml_byte_view sxml_qt4_xml_check = SXML_BYTE_VIEW_INITIALIZER(
    "deep-equal(parse-xml('<" SXML_QT4_XML_WRAPPER ">' || $result || '</" SXML_QT4_XML_WRAPPER
    ">'), parse-xml('<" SXML_QT4_XML_WRAPPER ">' || $expected || '</" SXML_QT4_XML_WRAPPER
    ">'), {'comments': true(), 'processing-instructions': true()})");

/* The same for assert-xml xml-version="1.1": both texts are XML 1.1
   documents (XML 1.1, 4.3.4), whose character references may denote the
   C0 controls and whose prefixes may be undeclared. */
#define SXML_QT4_XML_1_1_START "<?xml version=\"1.1\"?><" SXML_QT4_XML_WRAPPER ">"
static const sxml_byte_view sxml_qt4_xml_1_1_check = SXML_BYTE_VIEW_INITIALIZER(
    "deep-equal(parse-xml('" SXML_QT4_XML_1_1_START "' || $result || '</" SXML_QT4_XML_WRAPPER
    ">'), parse-xml('" SXML_QT4_XML_1_1_START "' || $expected || '</" SXML_QT4_XML_WRAPPER
    ">'), {'comments': true(), 'processing-instructions': true()})");

/* What assert-xml holds while it compares. */
typedef struct sxml_qt4_xml_comparison {
    sxml_output_buffer *output_buffer;
    sxml_sequence written;
    sxml_sequence actual;
    sxml_sequence expected;
    sxml_sequence check_value;
    /* xml-version="1.1": the result is written and both texts are parsed
       as XML 1.1. */
    bool is_xml_version_1_1;
    /* Set when the comparison reached a verdict. */
    bool has_verdict;
    sxml_qt4_assertion_verdict verdict;
} sxml_qt4_xml_comparison;

/* Writes the result as XML into comparison->actual. */
static sxml_status sxml_qt4_serialize_result(sxml_qt4_assertion_state *state,
                                             sxml_qt4_xml_comparison *comparison) {
    static const sxml_byte_view name = SXML_BYTE_VIEW_INITIALIZER("assert-xml");
    /* Atomic items become their string values (Serialization 4.0, section
       2, step 3). */
    sxml_status status = sxml_qt4_assertion_evaluate(
        state,
        &(sxml_qt4_bound_expression){
            .expression_text = SXML_TEXT(
                "$result ! (if (. instance of node()) then . else string(.))"),
            .result = state->request->evaluation.value,
            .expected = sxml_sequence_empty()},
        &comparison->written);
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            state->request->allocator,
            &(sxml_output_buffer_configuration){
                .allocator = state->request->allocator,
                .maximum_byte_count = 0},
            &comparison->output_buffer);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_write_as_xml(
            &(sxml_qt4_xml_writing){
                .item_heap = state->item_heap,
                .value = comparison->written,
                .error_record = &state->error_record,
                .is_xml_version_1_1 = comparison->is_xml_version_1_1},
            sxml_output_buffer_as_output_stream(comparison->output_buffer));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_make_string(
            state, sxml_output_buffer_get_bytes(comparison->output_buffer),
            &comparison->actual);
    }
    if (status != SXML_STATUS_OK && sxml_status_has_error_record(status)) {
        comparison->has_verdict = true;
        return sxml_qt4_fail_with_error(
            state,
            &(sxml_qt4_error_verdict){
                .assertion_name = name,
                .description = SXML_TEXT("writing the result raised "),
                .raised_error = {.status = status,
                                 .error_record = &state->error_record}},
            &comparison->verdict);
    }
    return status;
}

static sxml_status sxml_qt4_compare_xml(sxml_qt4_assertion_state *state,
                                        const sxml_qt4_leaf *leaf,
                                        sxml_qt4_xml_comparison *comparison) {
    static const sxml_byte_view name = SXML_BYTE_VIEW_INITIALIZER("assert-xml");
    sxml_qt4_assertion_verdict *verdict = &comparison->verdict;
    sxml_qt4_attribute_value xml_version = {.is_present = false,
                                            .value = sxml_qt4_empty_text};
    sxml_status status = sxml_qt4_leaf_attribute(
        state,
        (sxml_qt4_attribute_lookup){.element = leaf->element,
                                    .local_name = SXML_TEXT("xml-version")},
        &xml_version);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    comparison->is_xml_version_1_1 =
        sxml_byte_view_equals(xml_version.value, SXML_TEXT("1.1"));
    status = sxml_qt4_serialize_result(state, comparison);
    if (status != SXML_STATUS_OK || comparison->has_verdict) {
        return status;
    }
    sxml_qt4_expected_xml expected_xml = {.text = sxml_qt4_empty_text,
                                          .file_contents = NULL};
    status = sxml_qt4_read_expected_xml(state, leaf, &expected_xml);
    if (status == SXML_STATUS_OK) {
        sxml_byte_view text = sxml_qt4_strip_declaration(expected_xml.text);
        /* Inline text that starts and ends with markup is a document's
           worth of XML too (the catalog: "parsing and canonicalizing the
           XML"): the whitespace around it, which the catalog writes
           after a CDATA section's element, is not content (XML 1.0,
           [1] Misc). */
        const sxml_byte_view trimmed = sxml_qt4_trim_space(text);
        if (expected_xml.file_contents != NULL ||
            (trimmed.byte_count >= 2 && trimmed.bytes[0] == '<' &&
             trimmed.bytes[trimmed.byte_count - 1] == '>')) {
            text = trimmed;
        }
        status = sxml_qt4_make_string(state, text, &comparison->expected);
    }
    sxml_allocator_release(state->request->allocator,
                           expected_xml.file_contents);
    if (status == SXML_STATUS_INVALID_UTF8) {
        return sxml_qt4_simple_verdict(
            state,
            (sxml_qt4_verdict_text){
                .verdict_kind = SXML_QT4_VERDICT_FAIL,
                .assertion_name = name,
                .description = SXML_TEXT("the expected XML is not UTF-8")},
            verdict);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_assertion_evaluate(
            state,
            &(sxml_qt4_bound_expression){.expression_text =
                                             comparison->is_xml_version_1_1
                                                 ? sxml_qt4_xml_1_1_check
                                                 : sxml_qt4_xml_check,
                                         .result = comparison->actual,
                                         .expected = comparison->expected},
            &comparison->check_value);
    }
    if (status != SXML_STATUS_OK) {
        return sxml_qt4_fail_unless_failure(
            state,
            &(sxml_qt4_error_verdict){
                .assertion_name = name,
                .description = SXML_TEXT("comparing raised "),
                .raised_error = {.status = status,
                                 .error_record = &state->error_record}},
            verdict);
    }
    if (sxml_qt4_is_true(comparison->check_value)) {
        *verdict = (sxml_qt4_assertion_verdict){
            .verdict_kind = SXML_QT4_VERDICT_PASS,
            .reason = sxml_qt4_empty_text};
        return SXML_STATUS_OK;
    }
    sxml_qt4_reason reason;
    status = sxml_qt4_start_reason(
        state,
        (sxml_qt4_verdict_text){.verdict_kind = SXML_QT4_VERDICT_FAIL,
                                .assertion_name = name,
                                .description = SXML_TEXT("got ")},
        &reason);
    if (status == SXML_STATUS_OK) {
        sxml_qt4_reason_append(
            &reason, sxml_output_buffer_get_bytes(comparison->output_buffer));
        sxml_qt4_finish_reason(SXML_QT4_VERDICT_FAIL, &reason, verdict);
    }
    return status;
}

static sxml_status sxml_qt4_check_xml(sxml_qt4_assertion_state *state,
                                      const sxml_qt4_leaf *leaf,
                                      sxml_qt4_assertion_verdict *verdict) {
    sxml_qt4_xml_comparison comparison = {.output_buffer = NULL,
                                          .written = sxml_sequence_empty(),
                                          .actual = sxml_sequence_empty(),
                                          .expected = sxml_sequence_empty(),
                                          .check_value = sxml_sequence_empty(),
                                          .is_xml_version_1_1 = false,
                                          .has_verdict = false,
                                          .verdict = {.verdict_kind =
                                                          SXML_QT4_VERDICT_FAIL,
                                                      .reason =
                                                          sxml_qt4_empty_text}};
    const sxml_status status = sxml_qt4_compare_xml(state, leaf, &comparison);
    if (status == SXML_STATUS_OK) {
        *verdict = comparison.verdict;
    }
    sxml_sequence_release(comparison.check_value);
    sxml_sequence_release(comparison.expected);
    sxml_sequence_release(comparison.actual);
    sxml_sequence_release(comparison.written);
    sxml_output_buffer_destroy(comparison.output_buffer);
    return status;
}

/* ------------------------------------------------------------------ error */

/* True when code, as the code attribute writes it (an NCName in the err
   namespace, err:NAME or Q{uri}local; catalog-schema.xsd, codeAttr), names
   the raised error. */
static bool sxml_qt4_error_code_matches(sxml_byte_view code,
                                        const sxml_error_code *raised) {
    sxml_byte_view namespace_uri = SXML_TEXT(SXML_ERROR_NAMESPACE_URI);
    sxml_byte_view local_name = code;
    if (code.byte_count > 2 && code.bytes[0] == 'Q' && code.bytes[1] == '{') {
        const uint8_t *end = memchr(code.bytes, '}', code.byte_count);
        if (end == NULL) {
            return false;
        }
        namespace_uri = (sxml_byte_view){
            .bytes = code.bytes + 2, .byte_count = (size_t)(end - code.bytes) - 2};
        local_name = (sxml_byte_view){
            .bytes = end + 1,
            .byte_count = code.byte_count - (size_t)(end - code.bytes) - 1};
    } else {
        const uint8_t *colon =
            code.byte_count == 0 ? NULL : memchr(code.bytes, ':', code.byte_count);
        if (colon != NULL) {
            local_name = (sxml_byte_view){
                .bytes = colon + 1,
                .byte_count = code.byte_count - (size_t)(colon - code.bytes) - 1};
        }
    }
    return sxml_byte_view_equals(namespace_uri, raised->namespace_uri) &&
           sxml_byte_view_equals(local_name, raised->local_name);
}

/* An error assertion and what it judges: the evaluation, or what
   serializing its value gave. */
typedef struct sxml_qt4_error_check {
    const sxml_qt4_leaf *leaf;
    const sxml_qt4_evaluation_outcome *outcome;
} sxml_qt4_error_check;

static sxml_status sxml_qt4_check_error(sxml_qt4_assertion_state *state,
                                        const sxml_qt4_error_check *check,
                                        sxml_qt4_assertion_verdict *verdict) {
    const sxml_qt4_leaf *leaf = check->leaf;
    const sxml_qt4_evaluation_outcome *evaluation = check->outcome;
    sxml_qt4_attribute_value code = {.is_present = false,
                                     .value = sxml_qt4_empty_text};
    sxml_status status = sxml_qt4_leaf_attribute(
        state,
        (sxml_qt4_attribute_lookup){.element = leaf->element,
                                    .local_name = SXML_TEXT("code")},
        &code);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_qt4_reason reason;
    status = sxml_qt4_start_reason(
        state,
        (sxml_qt4_verdict_text){.verdict_kind = SXML_QT4_VERDICT_FAIL,
                                .assertion_name = leaf->name,
                                .description = SXML_TEXT("expected ")},
        &reason);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_qt4_reason_append(&reason, code.value);
    if (evaluation->status == SXML_STATUS_OK) {
        sxml_qt4_reason_append(&reason, SXML_TEXT(", got "));
        status = sxml_qt4_reason_append_value(
            &reason, (sxml_qt4_shown_value){.item_heap = state->item_heap,
                                            .value = evaluation->value});
        if (status == SXML_STATUS_OK) {
            sxml_qt4_finish_reason(SXML_QT4_VERDICT_FAIL, &reason, verdict);
        }
        return status;
    }
    if (sxml_byte_view_equals(code.value, SXML_TEXT("*")) ||
        sxml_qt4_error_code_matches(code.value,
                                    &evaluation->error_record->code)) {
        *verdict = (sxml_qt4_assertion_verdict){
            .verdict_kind = SXML_QT4_VERDICT_PASS,
            .reason = sxml_qt4_empty_text};
        return SXML_STATUS_OK;
    }
    sxml_qt4_reason_append(&reason, SXML_TEXT(", got "));
    sxml_qt4_reason_append_error(
        &reason, (sxml_qt4_raised_error){.status = evaluation->status,
                                         .error_record = evaluation->error_record});
    sxml_qt4_finish_reason(SXML_QT4_VERDICT_WRONG_ERROR, &reason, verdict);
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- Serialization */

/* Serializes the evaluation's value with its serialization parameters
   into a new buffer (Serialization 4.0). Returns a status with an error
   record, filled in state->error_record, for a serialization error, and
   the buffer's failures; *output_buffer is to be destroyed either way. */
static sxml_status sxml_qt4_serialize_value(sxml_qt4_assertion_state *state,
                                            sxml_output_buffer **output_buffer) {
    const sxml_qt4_assertion_request *request = state->request;
    sxml_status status = sxml_output_buffer_create(
        request->allocator,
        &(sxml_output_buffer_configuration){.allocator = request->allocator,
                                            .maximum_byte_count = 0},
        output_buffer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(&state->error_record, 0, sizeof state->error_record);
    return sxml_serialize_sequence(
        state->item_heap,
        &(sxml_serialization_request){
            .value = request->evaluation.value,
            .serialization_parameters =
                request->evaluation.serialization_parameters,
            .error_record = &state->error_record},
        sxml_output_buffer_as_output_stream(*output_buffer));
}

/* True for flags fn:matches accepts the letters of (F&O 4.0, section
   5.6.2): the check text quotes them. */
static bool sxml_qt4_flags_are_letters(sxml_byte_view flags) {
    for (size_t byte_index = 0; byte_index < flags.byte_count;
         byte_index += 1) {
        const uint8_t byte = flags.bytes[byte_index];
        if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z'))) {
            return false;
        }
    }
    return true;
}

/* What serialization-matches holds while it compares. */
typedef struct sxml_qt4_serialization_match {
    sxml_output_buffer *output_buffer;
    sxml_sequence actual;
    sxml_sequence expected;
    sxml_sequence check_value;
    sxml_byte_view check_text;
    sxml_qt4_assertion_verdict verdict;
} sxml_qt4_serialization_match;

/* $expected and the check text: the pattern (text or file) and
   fn:matches with the flags. Returns SXML_STATUS_INVALID_UTF8 for a
   pattern that is not UTF-8 and SXML_STATUS_MALFORMED_INPUT for flags that
   are not letters. */
static sxml_status
sxml_qt4_read_pattern(sxml_qt4_assertion_state *state, const sxml_qt4_leaf *leaf,
                      sxml_qt4_serialization_match *match) {
    sxml_qt4_attribute_value flags = {.is_present = false,
                                      .value = sxml_qt4_empty_text};
    sxml_status status = sxml_qt4_leaf_attribute(
        state,
        (sxml_qt4_attribute_lookup){.element = leaf->element,
                                    .local_name = SXML_TEXT("flags")},
        &flags);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!sxml_qt4_flags_are_letters(flags.value)) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    sxml_byte_view with_flags = sxml_qt4_empty_text;
    status = sxml_qt4_concatenate(
        state->request->arena,
        (sxml_qt4_concatenation){.first =
                                     SXML_TEXT("matches($result, $expected, '"),
                                 .second = flags.value},
        &with_flags);
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_concatenate(
            state->request->arena,
            (sxml_qt4_concatenation){.first = with_flags,
                                     .second = SXML_TEXT("')")},
            &match->check_text);
    }
    sxml_qt4_expected_xml pattern = {.text = sxml_qt4_empty_text,
                                     .file_contents = NULL};
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_read_expected_xml(state, leaf, &pattern);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_make_string(state, pattern.text, &match->expected);
    }
    sxml_allocator_release(state->request->allocator, pattern.file_contents);
    return status;
}

static sxml_status
sxml_qt4_compare_serialization(sxml_qt4_assertion_state *state,
                               const sxml_qt4_leaf *leaf,
                               sxml_qt4_serialization_match *match) {
    sxml_status status = sxml_qt4_serialize_value(state, &match->output_buffer);
    if (status != SXML_STATUS_OK) {
        return sxml_qt4_fail_unless_failure(
            state,
            &(sxml_qt4_error_verdict){
                .assertion_name = leaf->name,
                .description = SXML_TEXT("serializing raised "),
                .raised_error = {.status = status,
                                 .error_record = &state->error_record}},
            &match->verdict);
    }
    status = sxml_qt4_make_string(
        state, sxml_output_buffer_get_bytes(match->output_buffer),
        &match->actual);
    if (status == SXML_STATUS_INVALID_UTF8) {
        return sxml_qt4_simple_verdict(
            state,
            (sxml_qt4_verdict_text){
                .verdict_kind = SXML_QT4_VERDICT_FAIL,
                .assertion_name = leaf->name,
                .description = SXML_TEXT("the serialized result is not UTF-8")},
            &match->verdict);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_read_pattern(state, leaf, match);
    }
    if (status == SXML_STATUS_INVALID_UTF8 ||
        status == SXML_STATUS_MALFORMED_INPUT) {
        return sxml_qt4_simple_verdict(
            state,
            (sxml_qt4_verdict_text){
                .verdict_kind = SXML_QT4_VERDICT_FAIL,
                .assertion_name = leaf->name,
                .description = SXML_TEXT(
                    "the pattern is not UTF-8 or the flags are not letters")},
            &match->verdict);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_assertion_evaluate(
            state,
            &(sxml_qt4_bound_expression){.expression_text = match->check_text,
                                         .result = match->actual,
                                         .expected = match->expected},
            &match->check_value);
        if (status != SXML_STATUS_OK) {
            return sxml_qt4_fail_unless_failure(
                state,
                &(sxml_qt4_error_verdict){
                    .assertion_name = leaf->name,
                    .description = SXML_TEXT("matching raised "),
                    .raised_error = {.status = status,
                                     .error_record = &state->error_record}},
                &match->verdict);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sxml_qt4_is_true(match->check_value)) {
        match->verdict = (sxml_qt4_assertion_verdict){
            .verdict_kind = SXML_QT4_VERDICT_PASS,
            .reason = sxml_qt4_empty_text};
        return SXML_STATUS_OK;
    }
    sxml_qt4_reason reason;
    status = sxml_qt4_start_reason(
        state,
        (sxml_qt4_verdict_text){.verdict_kind = SXML_QT4_VERDICT_FAIL,
                                .assertion_name = leaf->name,
                                .description = SXML_TEXT("got ")},
        &reason);
    if (status == SXML_STATUS_OK) {
        sxml_qt4_reason_append(&reason,
                               sxml_output_buffer_get_bytes(match->output_buffer));
        sxml_qt4_finish_reason(SXML_QT4_VERDICT_FAIL, &reason, &match->verdict);
    }
    return status;
}

/* serialization-matches (catalog-schema.xsd): "the output of the
   serializer (as a string) matches the regular expression". */
static sxml_status
sxml_qt4_check_serialization_match(sxml_qt4_assertion_state *state,
                                   const sxml_qt4_leaf *leaf,
                                   sxml_qt4_assertion_verdict *verdict) {
    sxml_qt4_serialization_match match = {
        .output_buffer = NULL,
        .actual = sxml_sequence_empty(),
        .expected = sxml_sequence_empty(),
        .check_value = sxml_sequence_empty(),
        .check_text = sxml_qt4_empty_text,
        .verdict = {.verdict_kind = SXML_QT4_VERDICT_FAIL,
                    .reason = sxml_qt4_empty_text}};
    const sxml_status status =
        sxml_qt4_compare_serialization(state, leaf, &match);
    if (status == SXML_STATUS_OK) {
        *verdict = match.verdict;
    }
    sxml_sequence_release(match.check_value);
    sxml_sequence_release(match.expected);
    sxml_sequence_release(match.actual);
    sxml_output_buffer_destroy(match.output_buffer);
    return status;
}

/* assert-serialization-error on a value (catalog-schema.xsd): "serializing
   the result produces a serialization error" with the code. */
static sxml_status
sxml_qt4_check_serialization_error(sxml_qt4_assertion_state *state,
                                   const sxml_qt4_leaf *leaf,
                                   sxml_qt4_assertion_verdict *verdict) {
    sxml_output_buffer *output_buffer = NULL;
    const sxml_status serialization_status =
        sxml_qt4_serialize_value(state, &output_buffer);
    sxml_output_buffer_destroy(output_buffer);
    if (serialization_status != SXML_STATUS_OK &&
        !sxml_status_has_error_record(serialization_status)) {
        return serialization_status;
    }
    const sxml_qt4_evaluation_outcome serialized = {
        .status = serialization_status,
        .value = state->request->evaluation.value,
        .error_record = &state->error_record,
        .serialization_parameters =
            state->request->evaluation.serialization_parameters};
    return sxml_qt4_check_error(
        state, &(sxml_qt4_error_check){.leaf = leaf, .outcome = &serialized},
        verdict);
}

/* ------------------------------------------------------------ Dispatching */

/* Assertions the driver cannot check: assert-serialization appears only in
   comments of the suite; the others need XSLT. */
static const sxml_byte_view sxml_qt4_unsupported_assertions[] = {
    SXML_BYTE_VIEW_INITIALIZER("assert-serialization"),
    SXML_BYTE_VIEW_INITIALIZER("assert-message"),
    SXML_BYTE_VIEW_INITIALIZER("assert-warning"),
    SXML_BYTE_VIEW_INITIALIZER("assert-result-document"),
};

/* The checks of every kind but error: a raised error fails them. */
static sxml_status sxml_qt4_check_on_value(sxml_qt4_assertion_state *state,
                                           const sxml_qt4_leaf *leaf,
                                           sxml_qt4_assertion_verdict *verdict) {
    if (sxml_byte_view_equals(leaf->name, SXML_TEXT("assert-type"))) {
        return sxml_qt4_check_type(state, leaf, verdict);
    }
    if (sxml_byte_view_equals(leaf->name, SXML_TEXT("assert-xml"))) {
        return sxml_qt4_check_xml(state, leaf, verdict);
    }
    if (sxml_byte_view_equals(leaf->name, SXML_TEXT("serialization-matches"))) {
        return sxml_qt4_check_serialization_match(state, leaf, verdict);
    }
    if (sxml_byte_view_equals(leaf->name,
                              SXML_TEXT("assert-serialization-error"))) {
        return sxml_qt4_check_serialization_error(state, leaf, verdict);
    }
    for (size_t assertion_index = 0;
         assertion_index <
         sizeof sxml_qt4_value_assertions / sizeof sxml_qt4_value_assertions[0];
         assertion_index += 1) {
        const sxml_qt4_value_assertion *assertion =
            &sxml_qt4_value_assertions[assertion_index];
        if (!sxml_byte_view_equals(leaf->name, assertion->name)) {
            continue;
        }
        sxml_byte_view check_text = assertion->check_text;
        if (assertion->expected_form == SXML_QT4_EXPECTED_TEXT) {
            sxml_qt4_attribute_value normalize = {.is_present = false,
                                                  .value = sxml_qt4_empty_text};
            const sxml_status status = sxml_qt4_leaf_attribute(
                state,
                (sxml_qt4_attribute_lookup){
                    .element = leaf->element,
                    .local_name = SXML_TEXT("normalize-space")},
                &normalize);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            /* The QT4 catalog's default is false, the XSLT 4.0 catalog's
               true (D157). */
            if (sxml_byte_view_equals(normalize.value, SXML_TEXT("true")) ||
                sxml_byte_view_equals(normalize.value, SXML_TEXT("1")) ||
                (!normalize.is_present &&
                 state->request->normalizes_string_values_by_default)) {
                check_text = sxml_qt4_normalized_string_check;
            }
        }
        return sxml_qt4_check_value(
            state,
            &(sxml_qt4_value_check){.assertion = assertion,
                                    .element = leaf->element,
                                    .check_text = check_text},
            verdict);
    }
    return sxml_qt4_simple_verdict(
        state,
        (sxml_qt4_verdict_text){.verdict_kind = SXML_QT4_VERDICT_UNSUPPORTED,
                                .assertion_name = leaf->name,
                                .description =
                                    SXML_TEXT("unknown assertion")},
        verdict);
}

static sxml_status sxml_qt4_check_leaf(sxml_qt4_assertion_state *state,
                                       const sxml_qt4_leaf *leaf,
                                       sxml_qt4_assertion_verdict *verdict) {
    for (size_t unsupported_index = 0;
         unsupported_index < sizeof sxml_qt4_unsupported_assertions /
                                 sizeof sxml_qt4_unsupported_assertions[0];
         unsupported_index += 1) {
        if (sxml_byte_view_equals(
                leaf->name, sxml_qt4_unsupported_assertions[unsupported_index])) {
            return sxml_qt4_simple_verdict(
                state,
                (sxml_qt4_verdict_text){
                    .verdict_kind = SXML_QT4_VERDICT_UNSUPPORTED,
                    .assertion_name = leaf->name,
                    .description = SXML_TEXT(
                        "not checked by the driver")},
                verdict);
        }
    }
    const sxml_qt4_evaluation_outcome *evaluation = &state->request->evaluation;
    if (sxml_byte_view_equals(leaf->name, SXML_TEXT("error")) ||
        (sxml_byte_view_equals(leaf->name,
                               SXML_TEXT("assert-serialization-error")) &&
         evaluation->status != SXML_STATUS_OK)) {
        return sxml_qt4_check_error(
            state,
            &(sxml_qt4_error_check){.leaf = leaf, .outcome = evaluation},
            verdict);
    }
    if (evaluation->status != SXML_STATUS_OK) {
        return sxml_qt4_fail_with_error(
            state,
            &(sxml_qt4_error_verdict){
                .assertion_name = leaf->name,
                .description = SXML_TEXT("got error "),
                .raised_error = {.status = evaluation->status,
                                 .error_record = evaluation->error_record}},
            verdict);
    }
    return sxml_qt4_check_on_value(state, leaf, verdict);
}

/* ----------------------------------------------------------- Combinations */

typedef enum sxml_qt4_combination {
    /* all-of, and <result> itself. */
    SXML_QT4_COMBINATION_ALL_OF,
    SXML_QT4_COMBINATION_ANY_OF,
    SXML_QT4_COMBINATION_NOT,
} sxml_qt4_combination;

typedef struct sxml_qt4_assertion_frame {
    sxml_qt4_combination combination;
    sxml_qt4_element_cursor cursor;
    size_t child_count;
    sxml_qt4_assertion_verdict verdict;
} sxml_qt4_assertion_frame;

/* Combines a child's verdict into frame. Ties keep the earlier child, so
   a reason is the first child's that decided. */
static void sxml_qt4_combine(sxml_qt4_assertion_frame *frame,
                             sxml_qt4_assertion_verdict child) {
    frame->child_count += 1;
    if (frame->child_count == 1) {
        frame->verdict = child;
        return;
    }
    if ((frame->combination == SXML_QT4_COMBINATION_ALL_OF &&
         child.verdict_kind < frame->verdict.verdict_kind) ||
        (frame->combination == SXML_QT4_COMBINATION_ANY_OF &&
         child.verdict_kind > frame->verdict.verdict_kind)) {
        frame->verdict = child;
    }
}

/* The verdict of a frame whose children are all checked. */
static sxml_qt4_assertion_verdict
sxml_qt4_finish_frame(const sxml_qt4_assertion_frame *frame) {
    if (frame->child_count == 0) {
        return (sxml_qt4_assertion_verdict){
            .verdict_kind = SXML_QT4_VERDICT_FAIL,
            .reason = SXML_TEXT("no assertion")};
    }
    if (frame->combination != SXML_QT4_COMBINATION_NOT) {
        return frame->verdict;
    }
    switch (frame->verdict.verdict_kind) {
    case SXML_QT4_VERDICT_FAIL:
        return (sxml_qt4_assertion_verdict){
            .verdict_kind = SXML_QT4_VERDICT_PASS,
            .reason = sxml_qt4_empty_text};
    case SXML_QT4_VERDICT_UNSUPPORTED:
        return frame->verdict;
    case SXML_QT4_VERDICT_WRONG_ERROR:
    case SXML_QT4_VERDICT_PASS:
        break;
    }
    return (sxml_qt4_assertion_verdict){
        .verdict_kind = SXML_QT4_VERDICT_FAIL,
        .reason = SXML_TEXT("not: the assertion holds")};
}

/* The combination an element name starts, or false for a leaf. */
static bool sxml_qt4_find_combination(sxml_byte_view name,
                                      sxml_qt4_combination *combination) {
    if (sxml_byte_view_equals(name, SXML_TEXT("all-of"))) {
        *combination = SXML_QT4_COMBINATION_ALL_OF;
        return true;
    }
    if (sxml_byte_view_equals(name, SXML_TEXT("any-of"))) {
        *combination = SXML_QT4_COMBINATION_ANY_OF;
        return true;
    }
    if (sxml_byte_view_equals(name, SXML_TEXT("not"))) {
        *combination = SXML_QT4_COMBINATION_NOT;
        return true;
    }
    return false;
}

/* Pushes the frame of a combination element. */
typedef struct sxml_qt4_frame_push {
    sxml_qt4_combination combination;
    sxml_node_index element;
} sxml_qt4_frame_push;

typedef struct sxml_qt4_frame_stack {
    sxml_qt4_assertion_frame frames[SXML_QT4_MAXIMUM_ASSERTION_DEPTH];
    size_t frame_count;
} sxml_qt4_frame_stack;

static sxml_status sxml_qt4_push_frame(sxml_qt4_assertion_state *state,
                                       sxml_qt4_frame_stack *stack,
                                       sxml_qt4_frame_push push) {
    if (stack->frame_count == SXML_QT4_MAXIMUM_ASSERTION_DEPTH) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_qt4_assertion_frame *frame = &stack->frames[stack->frame_count];
    frame->combination = push.combination;
    frame->child_count = 0;
    frame->verdict = (sxml_qt4_assertion_verdict){
        .verdict_kind = SXML_QT4_VERDICT_FAIL, .reason = sxml_qt4_empty_text};
    const sxml_status status = sxml_qt4_element_cursor_start(
        state->request->document, push.element, &frame->cursor);
    if (status == SXML_STATUS_OK) {
        stack->frame_count += 1;
    }
    return status;
}

/* Where the walk starts: at the children of request->result_element, or
   at that element itself. */
typedef struct sxml_qt4_assertion_start {
    sxml_qt4_assertion_verdict *verdict;
    /* Of the frame over the children of request->result_element: ALL_OF
       for a <result>, or the combination the element is. */
    sxml_qt4_combination combination;
} sxml_qt4_assertion_start;

static sxml_status
sxml_qt4_check_from(const sxml_qt4_assertion_request *request,
                    sxml_qt4_assertion_start start);

sxml_status sxml_qt4_check_assertion(const sxml_qt4_assertion_request *request,
                                     sxml_qt4_assertion_verdict *verdict) {
    return sxml_qt4_check_from(
        request,
        (sxml_qt4_assertion_start){.verdict = verdict,
                                   .combination = SXML_QT4_COMBINATION_ALL_OF});
}

sxml_status
sxml_qt4_check_assertion_element(const sxml_qt4_assertion_request *request,
                                 sxml_byte_view element_name,
                                 sxml_qt4_assertion_verdict *verdict) {
    sxml_qt4_combination combination = SXML_QT4_COMBINATION_ALL_OF;
    if (sxml_qt4_find_combination(element_name, &combination)) {
        return sxml_qt4_check_from(
            request, (sxml_qt4_assertion_start){.verdict = verdict,
                                                .combination = combination});
    }
    /* A leaf: the walk of a <result> holding only it. */
    sxml_qt4_assertion_state state = {
        .request = request,
        .item_heap = sxml_processor_get_item_heap(request->processor),
        .result_name = {.index = 0},
        .expected_name = {.index = 0}};
    memset(&state.error_record, 0, sizeof state.error_record);
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(request->processor);
    sxml_status status = sxml_name_pool_intern_name(
        name_pool,
        &(sxml_name_request){.namespace_uri = {.index = 0},
                             .local_name = SXML_TEXT("result")},
        &state.result_name);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = {.index = 0},
                                 .local_name = SXML_TEXT("expected")},
            &state.expected_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_check_leaf(
            &state,
            &(sxml_qt4_leaf){.element = request->result_element,
                             .name = element_name},
            verdict);
    }
    sxml_item_heap_clear_raised_error(state.item_heap);
    return status;
}

static sxml_status
sxml_qt4_check_from(const sxml_qt4_assertion_request *request,
                    sxml_qt4_assertion_start start) {
    sxml_qt4_assertion_verdict *verdict = start.verdict;
    sxml_qt4_assertion_state state = {
        .request = request,
        .item_heap = sxml_processor_get_item_heap(request->processor),
        .result_name = {.index = 0},
        .expected_name = {.index = 0}};
    memset(&state.error_record, 0, sizeof state.error_record);
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(request->processor);
    sxml_status status = sxml_name_pool_intern_name(
        name_pool,
        &(sxml_name_request){.namespace_uri = {.index = 0},
                             .local_name = SXML_TEXT("result")},
        &state.result_name);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = {.index = 0},
                                 .local_name = SXML_TEXT("expected")},
            &state.expected_name);
    }
    sxml_qt4_frame_stack stack;
    stack.frame_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_push_frame(
            &state, &stack,
            (sxml_qt4_frame_push){.combination = start.combination,
                                  .element = request->result_element});
    }
    while (status == SXML_STATUS_OK) {
        sxml_qt4_assertion_frame *frame = &stack.frames[stack.frame_count - 1];
        sxml_qt4_element_step step = {.has_element = false};
        status = sxml_qt4_element_cursor_next(&frame->cursor, &step);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (!step.has_element) {
            const sxml_qt4_assertion_verdict finished =
                sxml_qt4_finish_frame(frame);
            stack.frame_count -= 1;
            if (stack.frame_count == 0) {
                *verdict = finished;
                return SXML_STATUS_OK;
            }
            sxml_qt4_combine(&stack.frames[stack.frame_count - 1], finished);
            continue;
        }
        sxml_qt4_combination combination = SXML_QT4_COMBINATION_ALL_OF;
        if (sxml_qt4_find_combination(step.local_name, &combination)) {
            status = sxml_qt4_push_frame(
                &state, &stack,
                (sxml_qt4_frame_push){.combination = combination,
                                      .element = step.element});
            continue;
        }
        sxml_qt4_assertion_verdict leaf_verdict = {
            .verdict_kind = SXML_QT4_VERDICT_FAIL,
            .reason = sxml_qt4_empty_text};
        status = sxml_qt4_check_leaf(
            &state,
            &(sxml_qt4_leaf){.element = step.element, .name = step.local_name},
            &leaf_verdict);
        if (status == SXML_STATUS_OK) {
            sxml_qt4_combine(frame, leaf_verdict);
        }
    }
    sxml_item_heap_clear_raised_error(state.item_heap);
    return status;
}
