/*
 * The result assertions of XSLT 4.0 test cases, surveyed
 * (xslt_assertion.h). The walk imitates the explicit stack of
 * tests/qt4/driver/assertion.c (D71).
 */
#include "xslt_assertion.h"

#include "suite_path.h"
#include "sxml/name.h"
#include "sxml/string.h"
#include "sxml/types.h"
#include "sxml/xpath.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xslt_test_empty_text = {.bytes = NULL,
                                                         .byte_count = 0};

/* By sxml_xslt_test_assertion_kind. */
static const sxml_byte_view sxml_xslt_test_assertion_names[] = {
    SXML_BYTE_VIEW_INITIALIZER("all-of"),
    SXML_BYTE_VIEW_INITIALIZER("any-of"),
    SXML_BYTE_VIEW_INITIALIZER("not"),
    SXML_BYTE_VIEW_INITIALIZER("assert"),
    SXML_BYTE_VIEW_INITIALIZER("assert-eq"),
    SXML_BYTE_VIEW_INITIALIZER("assert-deep-eq"),
    SXML_BYTE_VIEW_INITIALIZER("assert-permutation"),
    SXML_BYTE_VIEW_INITIALIZER("assert-count"),
    SXML_BYTE_VIEW_INITIALIZER("assert-empty"),
    SXML_BYTE_VIEW_INITIALIZER("assert-true"),
    SXML_BYTE_VIEW_INITIALIZER("assert-false"),
    SXML_BYTE_VIEW_INITIALIZER("assert-type"),
    SXML_BYTE_VIEW_INITIALIZER("assert-string-value"),
    SXML_BYTE_VIEW_INITIALIZER("assert-xml"),
    SXML_BYTE_VIEW_INITIALIZER("error"),
    SXML_BYTE_VIEW_INITIALIZER("assert-serialization"),
    SXML_BYTE_VIEW_INITIALIZER("serialization-matches"),
    SXML_BYTE_VIEW_INITIALIZER("assert-serialization-error"),
    SXML_BYTE_VIEW_INITIALIZER("assert-message"),
    SXML_BYTE_VIEW_INITIALIZER("assert-result-document"),
    SXML_BYTE_VIEW_INITIALIZER("assert-warning"),
    SXML_BYTE_VIEW_INITIALIZER("assert-posture-and-sweep"),
};

_Static_assert(sizeof sxml_xslt_test_assertion_names /
                       sizeof sxml_xslt_test_assertion_names[0] ==
                   SXML_XSLT_TEST_ASSERTION_KIND_COUNT,
               "one name per assertion kind");

sxml_byte_view
sxml_xslt_test_assertion_kind_name(sxml_xslt_test_assertion_kind kind) {
    if ((size_t)kind >= SXML_XSLT_TEST_ASSERTION_KIND_COUNT) {
        return sxml_xslt_test_empty_text;
    }
    return sxml_xslt_test_assertion_names[kind];
}

bool sxml_xslt_test_assertion_kind_is_checked(
    sxml_xslt_test_assertion_kind kind) {
    return kind != SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_WARNING &&
           kind != SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_POSTURE_AND_SWEEP;
}

/* Finds the kind of an element name; false for none. */
static bool sxml_xslt_test_assertion_kind_of(sxml_byte_view local_name,
                                             sxml_xslt_test_assertion_kind *kind) {
    for (size_t kind_index = 0;
         kind_index < SXML_XSLT_TEST_ASSERTION_KIND_COUNT; kind_index += 1) {
        if (sxml_byte_view_equals(local_name,
                                  sxml_xslt_test_assertion_names[kind_index])) {
            *kind = (sxml_xslt_test_assertion_kind)kind_index;
            return true;
        }
    }
    return false;
}

/* How many assertions an element holds. */
typedef enum sxml_xslt_test_assertion_arity {
    /* A leaf: its children, if any, are its data. */
    SXML_XSLT_TEST_ASSERTION_ARITY_NONE,
    SXML_XSLT_TEST_ASSERTION_ARITY_ONE,
    SXML_XSLT_TEST_ASSERTION_ARITY_SOME,
} sxml_xslt_test_assertion_arity;

static sxml_xslt_test_assertion_arity
sxml_xslt_test_assertion_arity_of(sxml_xslt_test_assertion_kind kind) {
    switch (kind) {
    case SXML_XSLT_TEST_ASSERTION_KIND_ALL_OF:
    case SXML_XSLT_TEST_ASSERTION_KIND_ANY_OF:
        return SXML_XSLT_TEST_ASSERTION_ARITY_SOME;
    case SXML_XSLT_TEST_ASSERTION_KIND_NOT:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_MESSAGE:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_RESULT_DOCUMENT:
        return SXML_XSLT_TEST_ASSERTION_ARITY_ONE;
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_EQ:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_DEEP_EQ:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_PERMUTATION:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_COUNT:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_EMPTY:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_TRUE:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_FALSE:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_TYPE:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_STRING_VALUE:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_XML:
    case SXML_XSLT_TEST_ASSERTION_KIND_ERROR:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_SERIALIZATION:
    case SXML_XSLT_TEST_ASSERTION_KIND_SERIALIZATION_MATCHES:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_SERIALIZATION_ERROR:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_WARNING:
    case SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_POSTURE_AND_SWEEP:
        break;
    }
    return SXML_XSLT_TEST_ASSERTION_ARITY_NONE;
}

/* One element whose child assertions are being walked. */
typedef struct sxml_xslt_test_assertion_frame {
    sxml_qt4_element_cursor cursor;
    sxml_xslt_test_assertion_arity arity;
    size_t child_count;
    sxml_byte_view name;
} sxml_xslt_test_assertion_frame;

/* Fills the problem and returns SXML_STATUS_MALFORMED_INPUT. */
typedef struct sxml_xslt_test_assertion_problem {
    sxml_byte_view description;
    sxml_byte_view subject;
} sxml_xslt_test_assertion_problem;

static sxml_status
sxml_xslt_test_malformed(const sxml_xslt_test_assertion_request *request,
                         sxml_xslt_test_assertion_problem problem) {
    if (request->problem != NULL) {
        *request->problem = (sxml_qt4_problem){
            .description = problem.description,
            .subject = problem.subject,
            .location = {.source_name = request->test_set_file_path,
                         .line_number = 0,
                         .column_number = 0,
                         .byte_offset = 0}};
    }
    return SXML_STATUS_MALFORMED_INPUT;
}

/* True when the frame has the number of children its arity asks. */
static bool
sxml_xslt_test_frame_is_complete(const sxml_xslt_test_assertion_frame *frame) {
    switch (frame->arity) {
    case SXML_XSLT_TEST_ASSERTION_ARITY_ONE:
        return frame->child_count == 1;
    case SXML_XSLT_TEST_ASSERTION_ARITY_SOME:
        return frame->child_count >= 1;
    case SXML_XSLT_TEST_ASSERTION_ARITY_NONE:
        break;
    }
    return true;
}

/* Starts a frame over the children of element. */
typedef struct sxml_xslt_test_frame_start {
    sxml_node_index element;
    sxml_xslt_test_assertion_arity arity;
    sxml_byte_view name;
} sxml_xslt_test_frame_start;

static sxml_status
sxml_xslt_test_start_frame(const sxml_xslt_test_assertion_request *request,
                           sxml_xslt_test_frame_start start,
                           sxml_xslt_test_assertion_frame *frame) {
    *frame = (sxml_xslt_test_assertion_frame){
        .cursor = {.document = request->document,
                   .has_next_node = false,
                   .next_node = {.index = 0}},
        .arity = start.arity,
        .child_count = 0,
        .name = start.name};
    return sxml_qt4_element_cursor_start(request->document, start.element,
                                         &frame->cursor);
}

sxml_status
sxml_xslt_test_survey_assertions(const sxml_xslt_test_assertion_request *request,
                                 sxml_xslt_test_assertion_survey *survey) {
    sxml_xslt_test_assertion_survey result = {
        .kind_counts = {0},
        .first_unchecked_kind = SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_WARNING,
        .has_unchecked_kind = false};
    /* The explicit stack (D71): the result element, then each wrapper. */
    sxml_xslt_test_assertion_frame
        frames[SXML_XSLT_TEST_MAXIMUM_ASSERTION_DEPTH + 1];
    size_t frame_count = 1;
    sxml_status status = sxml_xslt_test_start_frame(
        request,
        (sxml_xslt_test_frame_start){.element = request->result_element,
                                     .arity =
                                         SXML_XSLT_TEST_ASSERTION_ARITY_ONE,
                                     .name = SXML_TEXT("result")},
        &frames[0]);
    while (status == SXML_STATUS_OK && frame_count > 0) {
        sxml_xslt_test_assertion_frame *frame = &frames[frame_count - 1];
        sxml_qt4_element_step step = {.has_element = false,
                                      .element = {.index = 0},
                                      .local_name = sxml_xslt_test_empty_text};
        status = sxml_qt4_element_cursor_next(&frame->cursor, &step);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (!step.has_element) {
            if (!sxml_xslt_test_frame_is_complete(frame)) {
                status = sxml_xslt_test_malformed(
                    request, (sxml_xslt_test_assertion_problem){
                                 .description = SXML_TEXT(
                                     "wrong number of assertions in"),
                                 .subject = frame->name});
                break;
            }
            frame_count -= 1;
            continue;
        }
        sxml_xslt_test_assertion_kind kind =
            SXML_XSLT_TEST_ASSERTION_KIND_ASSERT;
        if (!sxml_xslt_test_assertion_kind_of(step.local_name, &kind)) {
            status = sxml_xslt_test_malformed(
                request, (sxml_xslt_test_assertion_problem){
                             .description = SXML_TEXT("unknown assertion"),
                             .subject = step.local_name});
            break;
        }
        frame->child_count += 1;
        result.kind_counts[kind] += 1;
        if (!result.has_unchecked_kind &&
            !sxml_xslt_test_assertion_kind_is_checked(kind)) {
            result.has_unchecked_kind = true;
            result.first_unchecked_kind = kind;
        }
        const sxml_xslt_test_assertion_arity arity =
            sxml_xslt_test_assertion_arity_of(kind);
        if (arity == SXML_XSLT_TEST_ASSERTION_ARITY_NONE) {
            continue;
        }
        if (frame_count > SXML_XSLT_TEST_MAXIMUM_ASSERTION_DEPTH) {
            status = SXML_STATUS_LIMIT_EXCEEDED;
            break;
        }
        status = sxml_xslt_test_start_frame(
            request,
            (sxml_xslt_test_frame_start){.element = step.element,
                                         .arity = arity,
                                         .name = step.local_name},
            &frames[frame_count]);
        frame_count += 1;
    }
    if (status == SXML_STATUS_OK) {
        *survey = result;
    }
    return status;
}

/* ------------------------------------------------------------- Checking */

/* A verdict with static text. */
static sxml_qt4_assertion_verdict
sxml_xslt_test_verdict(sxml_qt4_verdict_kind verdict_kind,
                       sxml_byte_view reason) {
    return (sxml_qt4_assertion_verdict){.verdict_kind = verdict_kind,
                                        .reason = reason};
}

/* The QT4 request for one assertion element checked against value (or
   against the raised error of the transformation, when value_outcome is
   NULL). */
typedef struct sxml_xslt_test_element_check {
    sxml_node_index element;
    sxml_byte_view name;
    const sxml_qt4_evaluation_outcome *value_outcome;
    /* Checks the children of element as a <result> holds them. */
    bool checks_children;
} sxml_xslt_test_element_check;

static sxml_status
sxml_xslt_test_check_element(const sxml_xslt_test_check_request *request,
                             const sxml_xslt_test_element_check *check,
                             sxml_qt4_assertion_verdict *verdict) {
    sxml_qt4_assertion_request base = request->base;
    base.result_element = check->element;
    if (check->value_outcome != NULL) {
        base.evaluation = *check->value_outcome;
    }
    if (check->checks_children) {
        return sxml_qt4_check_assertion(&base, verdict);
    }
    return sxml_qt4_check_assertion_element(&base, check->name, verdict);
}

/* A value the processor makes for a check: an xs:string of text. */
static sxml_status sxml_xslt_test_make_string(sxml_processor *processor,
                                              sxml_byte_view text,
                                              sxml_sequence *value) {
    sxml_string string = {.utf8 = sxml_xslt_test_empty_text,
                          .codepoint_count = 0,
                          .is_ascii = true};
    sxml_status status = sxml_string_from_utf8(text, &string);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            sxml_processor_get_item_heap(processor),
            &(sxml_atomic_item_request){
                .type = {.index = SXML_TYPE_XS_STRING},
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = string}}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *value = sxml_sequence_from_item(item);
    }
    return status;
}

/* serialization-matches and assert-serialization on the serialized bytes:
   the bytes as an xs:string, which the text method writes unchanged. */
typedef struct sxml_xslt_test_serialized_check {
    sxml_node_index element;
    sxml_byte_view name;
} sxml_xslt_test_serialized_check;

/* assert-serialization (catalog-schema.xsd): the serialization equals the
   expected text up to whitespace, or both are XML that is deep-equal once
   parsed (a difference a conformant serializer may make, such as attribute
   order or quotes); text that does not parse is a difference. */
static const sxml_byte_view sxml_xslt_test_serialization_check =
    SXML_BYTE_VIEW_INITIALIZER(
        "if (normalize-space($result) eq normalize-space($expected)) "
        "then true() else deep-equal(parse-xml-fragment($result), "
        "parse-xml-fragment($expected))");

/* The expected text of assert-serialization: its file= or its text. */
static sxml_status
sxml_xslt_test_expected_text(const sxml_xslt_test_check_request *request,
                             sxml_node_index element, sxml_byte_view *text) {
    const sxml_qt4_assertion_request *base = &request->base;
    sxml_qt4_attribute_value file = {.is_present = false,
                                     .value = sxml_xslt_test_empty_text};
    sxml_status status = sxml_qt4_document_find_attribute(
        base->document,
        (sxml_qt4_attribute_lookup){.element = element,
                                    .local_name = SXML_TEXT("file")},
        &file);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!file.is_present) {
        return sxml_qt4_document_get_text(
            base->document,
            &(sxml_qt4_text_request){.element = element, .arena = base->arena},
            text);
    }
    sxml_byte_view suite_path = sxml_xslt_test_empty_text;
    status = sxml_qt4_suite_path_resolve(
        &(sxml_qt4_suite_path_request){.containing_file =
                                           base->test_set_file_path,
                                       .reference = file.value,
                                       .arena = base->arena},
        &suite_path);
    sxml_byte_view contents = sxml_xslt_test_empty_text;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_catalog_read_file(
            base->catalog,
            &(sxml_qt4_file_request){.suite_path = suite_path,
                                     .allocator = base->allocator},
            &contents);
    }
    if (status == SXML_STATUS_OK) {
        /* Kept in the arena: the reader's memory is released here. */
        status = sxml_qt4_concatenate(
            base->arena,
            (sxml_qt4_concatenation){.first = contents,
                                     .second = sxml_xslt_test_empty_text},
            text);
        sxml_allocator_release(base->allocator, (void *)contents.bytes);
    }
    return status;
}

/* Evaluates sxml_xslt_test_serialization_check with $result and
   $expected bound to two strings. */
typedef struct sxml_xslt_test_comparison {
    sxml_sequence result;
    sxml_sequence expected;
} sxml_xslt_test_comparison;

static sxml_status
sxml_xslt_test_compare_serialization(const sxml_xslt_test_check_request *request,
                                     sxml_xslt_test_comparison comparison,
                                     bool *is_equal) {
    const sxml_qt4_assertion_request *base = &request->base;
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(base->processor);
    sxml_name_identifier names[2] = {{.index = 0}, {.index = 0}};
    const sxml_byte_view local_names[2] = {SXML_TEXT("result"),
                                           SXML_TEXT("expected")};
    sxml_status status = SXML_STATUS_OK;
    for (size_t name_index = 0; status == SXML_STATUS_OK && name_index < 2;
         name_index += 1) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = {.index = 0},
                                 .local_name = local_names[name_index]},
            &names[name_index]);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_sequence_type any_type = {
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
        .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
    const sxml_variable_declaration declarations[] = {
        {.variable_name = names[0], .declared_type = any_type},
        {.variable_name = names[1], .declared_type = any_type},
    };
    const sxml_variable_binding bindings[] = {
        {.variable_name = names[0], .value = comparison.result},
        {.variable_name = names[1], .value = comparison.expected},
    };
    sxml_static_context_configuration static_context = base->static_context;
    static_context.variable_declarations = (sxml_variable_declaration_view){
        .variable_declarations = declarations, .variable_declaration_count = 2};
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof error_record);
    sxml_xpath_expression *expression = NULL;
    status = sxml_xpath_expression_create(
        base->allocator,
        &(sxml_xpath_expression_configuration){
            .processor = base->processor,
            .expression_text = sxml_xslt_test_serialization_check,
            .source_name = SXML_TEXT("assert-serialization"),
            .static_context = static_context,
            .error_record = &error_record},
        &expression);
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        sxml_dynamic_context_configuration dynamic_context;
        memset(&dynamic_context, 0, sizeof dynamic_context);
        dynamic_context.variable_bindings = (sxml_variable_binding_view){
            .variable_bindings = bindings, .variable_binding_count = 2};
        status = sxml_xpath_expression_evaluate(
            expression,
            &(sxml_xpath_evaluation_request){.dynamic_context = dynamic_context,
                                             .error_record = &error_record},
            &value);
    }
    sxml_xpath_expression_destroy(expression);
    *is_equal = false;
    if (status == SXML_STATUS_OK) {
        sxml_item item = {.representation = 0};
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof datum);
        if (sxml_sequence_get_item(value,
                                   (sxml_item_request){.item_index = 0,
                                                       .error_record = NULL},
                                   &item) == SXML_STATUS_OK &&
            sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK &&
            datum.datum_kind == SXML_DATUM_KIND_BOOLEAN) {
            *is_equal = datum.datum_details.boolean;
        }
        sxml_sequence_release(value);
    }
    sxml_item_heap_clear_raised_error(
        sxml_processor_get_item_heap(base->processor));
    /* An error of the comparison itself is a difference. */
    return sxml_status_has_error_record(status) ? SXML_STATUS_OK : status;
}

static sxml_status
sxml_xslt_test_check_serialized(const sxml_xslt_test_check_request *request,
                                sxml_xslt_test_serialized_check check,
                                sxml_qt4_assertion_verdict *verdict) {
    if (request->base.evaluation.status != SXML_STATUS_OK) {
        return sxml_xslt_test_check_element(
            request,
            &(sxml_xslt_test_element_check){.element = check.element,
                                            .name = check.name,
                                            .value_outcome = NULL,
                                            .checks_children = false},
            verdict);
    }
    sxml_processor *processor = request->base.processor;
    sxml_sequence serialized = sxml_sequence_empty();
    sxml_status status = sxml_xslt_test_make_string(
        processor, request->serialized_bytes, &serialized);
    if (status == SXML_STATUS_INVALID_UTF8) {
        /* As the QT4 driver: output in another encoding is not compared. */
        *verdict = sxml_xslt_test_verdict(
            SXML_QT4_VERDICT_FAIL,
            SXML_TEXT("the serialization is not UTF-8 (an encoding parameter)"));
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sxml_byte_view_equals(check.name, SXML_TEXT("serialization-matches"))) {
        sxml_qt4_evaluation_outcome outcome = request->base.evaluation;
        outcome.value = serialized;
        memset(&outcome.serialization_parameters, 0,
               sizeof outcome.serialization_parameters);
        outcome.serialization_parameters.serialization_method =
            SXML_SERIALIZATION_METHOD_TEXT;
        status = sxml_xslt_test_check_element(
            request,
            &(sxml_xslt_test_element_check){.element = check.element,
                                            .name = check.name,
                                            .value_outcome = &outcome,
                                            .checks_children = false},
            verdict);
        sxml_sequence_release(serialized);
        return status;
    }
    sxml_byte_view expected_text = sxml_xslt_test_empty_text;
    status = sxml_xslt_test_expected_text(request, check.element, &expected_text);
    sxml_sequence expected = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_make_string(processor, expected_text, &expected);
    }
    bool is_equal = false;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_compare_serialization(
            request,
            (sxml_xslt_test_comparison){.result = serialized,
                                        .expected = expected},
            &is_equal);
        sxml_sequence_release(expected);
    }
    sxml_sequence_release(serialized);
    if (status == SXML_STATUS_OK) {
        *verdict = is_equal
                       ? sxml_xslt_test_verdict(SXML_QT4_VERDICT_PASS,
                                                sxml_xslt_test_empty_text)
                       : sxml_xslt_test_verdict(
                             SXML_QT4_VERDICT_FAIL,
                             SXML_TEXT("assert-serialization: the "
                                       "serialization differs"));
    }
    return status;
}

/* assert-message: the best verdict of the contained assertion over the
   messages. */
static sxml_status
sxml_xslt_test_check_messages(const sxml_xslt_test_check_request *request,
                              sxml_node_index element,
                              sxml_qt4_assertion_verdict *verdict) {
    *verdict = sxml_xslt_test_verdict(
        SXML_QT4_VERDICT_FAIL, SXML_TEXT("assert-message: no message"));
    sxml_status status = SXML_STATUS_OK;
    for (size_t message_index = 0;
         status == SXML_STATUS_OK &&
         message_index < request->messages.sequence_count &&
         verdict->verdict_kind != SXML_QT4_VERDICT_PASS;
         message_index += 1) {
        sxml_qt4_evaluation_outcome outcome = request->base.evaluation;
        outcome.status = SXML_STATUS_OK;
        outcome.value = request->messages.sequences[message_index];
        sxml_qt4_assertion_verdict message_verdict = *verdict;
        status = sxml_xslt_test_check_element(
            request,
            &(sxml_xslt_test_element_check){.element = element,
                                            .name = SXML_TEXT("assert-message"),
                                            .value_outcome = &outcome,
                                            .checks_children = true},
            &message_verdict);
        if (status == SXML_STATUS_OK &&
            (message_index == 0 ||
             message_verdict.verdict_kind > verdict->verdict_kind)) {
            *verdict = message_verdict;
        }
    }
    return status;
}

/* uri= of assert-result-document against the base output URI: a relative
   reference is appended to the base's directory (the driver's base output
   URIs end in "/" or name a file). */
static sxml_status
sxml_xslt_test_result_document_uri(const sxml_xslt_test_check_request *request,
                                   sxml_byte_view reference,
                                   sxml_byte_view *uri) {
    const sxml_byte_view base = request->base_output_uri;
    if (memchr(reference.bytes == NULL ? (const uint8_t *)"" : reference.bytes,
               ':', reference.byte_count) != NULL ||
        base.byte_count == 0) {
        *uri = reference;
        return SXML_STATUS_OK;
    }
    size_t directory_byte_count = base.byte_count;
    while (directory_byte_count > 0 &&
           base.bytes[directory_byte_count - 1] != '/') {
        directory_byte_count -= 1;
    }
    return sxml_qt4_concatenate(
        request->base.arena,
        (sxml_qt4_concatenation){
            .first = {.bytes = base.bytes, .byte_count = directory_byte_count},
            .second = reference},
        uri);
}

/* assert-result-document: the contained assertion on the result document
   of uri=. */
static sxml_status
sxml_xslt_test_check_result_document(const sxml_xslt_test_check_request *request,
                                     sxml_node_index element,
                                     sxml_qt4_assertion_verdict *verdict) {
    sxml_qt4_attribute_value reference = {.is_present = false,
                                          .value = sxml_xslt_test_empty_text};
    sxml_status status = sxml_qt4_document_find_attribute(
        request->base.document,
        (sxml_qt4_attribute_lookup){.element = element,
                                    .local_name = SXML_TEXT("uri")},
        &reference);
    sxml_byte_view uri = sxml_xslt_test_empty_text;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_result_document_uri(request, reference.value, &uri);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (request->base.evaluation.status != SXML_STATUS_OK) {
        *verdict = sxml_xslt_test_verdict(
            SXML_QT4_VERDICT_FAIL,
            SXML_TEXT("assert-result-document: the transformation failed"));
        return SXML_STATUS_OK;
    }
    const size_t document_count =
        request->collector == NULL
            ? 0
            : sxml_xslt_result_document_collector_get_result_document_count(
                  request->collector);
    for (size_t document_index = 0; document_index < document_count;
         document_index += 1) {
        sxml_xslt_collected_result_document document = {
            .absolute_uri = sxml_xslt_test_empty_text,
            .value = sxml_sequence_empty(),
            .serialized_bytes = sxml_xslt_test_empty_text};
        status = sxml_xslt_result_document_collector_get_result_document(
            request->collector, document_index, &document);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!sxml_byte_view_equals(document.absolute_uri, uri)) {
            continue;
        }
        sxml_qt4_evaluation_outcome outcome = request->base.evaluation;
        outcome.value = document.value;
        return sxml_xslt_test_check_element(
            request,
            &(sxml_xslt_test_element_check){
                .element = element,
                .name = SXML_TEXT("assert-result-document"),
                .value_outcome = &outcome,
                .checks_children = true},
            verdict);
    }
    *verdict = sxml_xslt_test_verdict(
        SXML_QT4_VERDICT_FAIL,
        SXML_TEXT("assert-result-document: no result document of that URI"));
    return SXML_STATUS_OK;
}

/* One leaf of the walk. */
static sxml_status
sxml_xslt_test_check_leaf(const sxml_xslt_test_check_request *request,
                          const sxml_qt4_element_step *step,
                          sxml_qt4_assertion_verdict *verdict) {
    sxml_xslt_test_assertion_kind kind = SXML_XSLT_TEST_ASSERTION_KIND_ASSERT;
    (void)sxml_xslt_test_assertion_kind_of(step->local_name, &kind);
    if (kind == SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_MESSAGE) {
        return sxml_xslt_test_check_messages(request, step->element, verdict);
    }
    if (kind == SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_RESULT_DOCUMENT) {
        return sxml_xslt_test_check_result_document(request, step->element,
                                                    verdict);
    }
    if (request->is_serialized &&
        (kind == SXML_XSLT_TEST_ASSERTION_KIND_SERIALIZATION_MATCHES ||
         kind == SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_SERIALIZATION)) {
        return sxml_xslt_test_check_serialized(
            request,
            (sxml_xslt_test_serialized_check){.element = step->element,
                                              .name = step->local_name},
            verdict);
    }
    return sxml_xslt_test_check_element(
        request,
        &(sxml_xslt_test_element_check){.element = step->element,
                                        .name = step->local_name,
                                        .value_outcome = NULL,
                                        .checks_children = false},
        verdict);
}

/* A combination being walked: all-of (and <result>), any-of, not. */
typedef enum sxml_xslt_test_combination {
    SXML_XSLT_TEST_COMBINATION_ALL_OF,
    SXML_XSLT_TEST_COMBINATION_ANY_OF,
    SXML_XSLT_TEST_COMBINATION_NOT,
} sxml_xslt_test_combination;

typedef struct sxml_xslt_test_check_frame {
    sxml_xslt_test_combination combination;
    sxml_qt4_element_cursor cursor;
    size_t child_count;
    sxml_qt4_assertion_verdict verdict;
} sxml_xslt_test_check_frame;

/* As the QT4 driver combines: ties keep the earlier child. */
static void sxml_xslt_test_combine(sxml_xslt_test_check_frame *frame,
                                   sxml_qt4_assertion_verdict child) {
    frame->child_count += 1;
    if (frame->child_count == 1 ||
        (frame->combination == SXML_XSLT_TEST_COMBINATION_ALL_OF &&
         child.verdict_kind < frame->verdict.verdict_kind) ||
        (frame->combination == SXML_XSLT_TEST_COMBINATION_ANY_OF &&
         child.verdict_kind > frame->verdict.verdict_kind)) {
        frame->verdict = child;
    }
}

static sxml_qt4_assertion_verdict
sxml_xslt_test_finish_frame(const sxml_xslt_test_check_frame *frame) {
    if (frame->child_count == 0) {
        return sxml_xslt_test_verdict(SXML_QT4_VERDICT_FAIL,
                                      SXML_TEXT("no assertion"));
    }
    if (frame->combination != SXML_XSLT_TEST_COMBINATION_NOT) {
        return frame->verdict;
    }
    switch (frame->verdict.verdict_kind) {
    case SXML_QT4_VERDICT_FAIL:
        return sxml_xslt_test_verdict(SXML_QT4_VERDICT_PASS,
                                      sxml_xslt_test_empty_text);
    case SXML_QT4_VERDICT_UNSUPPORTED:
        return frame->verdict;
    case SXML_QT4_VERDICT_WRONG_ERROR:
    case SXML_QT4_VERDICT_PASS:
        break;
    }
    return sxml_xslt_test_verdict(SXML_QT4_VERDICT_FAIL,
                                  SXML_TEXT("not: the assertion holds"));
}

/* Starts a frame over the children of element. */
typedef struct sxml_xslt_test_check_push {
    sxml_xslt_test_combination combination;
    sxml_node_index element;
} sxml_xslt_test_check_push;

static sxml_status
sxml_xslt_test_push_check_frame(const sxml_xslt_test_check_request *request,
                                sxml_xslt_test_check_push push,
                                sxml_xslt_test_check_frame *frame) {
    *frame = (sxml_xslt_test_check_frame){
        .combination = push.combination,
        .cursor = {.document = request->base.document,
                   .has_next_node = false,
                   .next_node = {.index = 0}},
        .child_count = 0,
        .verdict = sxml_xslt_test_verdict(SXML_QT4_VERDICT_FAIL,
                                          sxml_xslt_test_empty_text)};
    return sxml_qt4_element_cursor_start(request->base.document, push.element,
                                         &frame->cursor);
}

sxml_status
sxml_xslt_test_check_assertions(const sxml_xslt_test_check_request *request,
                                sxml_qt4_assertion_verdict *verdict) {
    sxml_xslt_test_check_frame frames[SXML_XSLT_TEST_MAXIMUM_ASSERTION_DEPTH];
    size_t frame_count = 1;
    sxml_status status = sxml_xslt_test_push_check_frame(
        request,
        (sxml_xslt_test_check_push){.combination =
                                        SXML_XSLT_TEST_COMBINATION_ALL_OF,
                                    .element = request->base.result_element},
        &frames[0]);
    while (status == SXML_STATUS_OK) {
        sxml_xslt_test_check_frame *frame = &frames[frame_count - 1];
        sxml_qt4_element_step step = {.has_element = false,
                                      .element = {.index = 0},
                                      .local_name = sxml_xslt_test_empty_text};
        status = sxml_qt4_element_cursor_next(&frame->cursor, &step);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (!step.has_element) {
            const sxml_qt4_assertion_verdict finished =
                sxml_xslt_test_finish_frame(frame);
            frame_count -= 1;
            if (frame_count == 0) {
                *verdict = finished;
                return SXML_STATUS_OK;
            }
            sxml_xslt_test_combine(&frames[frame_count - 1], finished);
            continue;
        }
        sxml_xslt_test_assertion_kind kind = SXML_XSLT_TEST_ASSERTION_KIND_ASSERT;
        (void)sxml_xslt_test_assertion_kind_of(step.local_name, &kind);
        if (kind == SXML_XSLT_TEST_ASSERTION_KIND_ALL_OF ||
            kind == SXML_XSLT_TEST_ASSERTION_KIND_ANY_OF ||
            kind == SXML_XSLT_TEST_ASSERTION_KIND_NOT) {
            if (frame_count == SXML_XSLT_TEST_MAXIMUM_ASSERTION_DEPTH) {
                status = SXML_STATUS_LIMIT_EXCEEDED;
                break;
            }
            status = sxml_xslt_test_push_check_frame(
                request,
                (sxml_xslt_test_check_push){
                    .combination =
                        kind == SXML_XSLT_TEST_ASSERTION_KIND_ALL_OF
                            ? SXML_XSLT_TEST_COMBINATION_ALL_OF
                        : kind == SXML_XSLT_TEST_ASSERTION_KIND_ANY_OF
                            ? SXML_XSLT_TEST_COMBINATION_ANY_OF
                            : SXML_XSLT_TEST_COMBINATION_NOT,
                    .element = step.element},
                &frames[frame_count]);
            frame_count += 1;
            continue;
        }
        sxml_qt4_assertion_verdict leaf_verdict =
            sxml_xslt_test_verdict(SXML_QT4_VERDICT_FAIL, sxml_xslt_test_empty_text);
        status = sxml_xslt_test_check_leaf(request, &step, &leaf_verdict);
        if (status == SXML_STATUS_OK) {
            sxml_xslt_test_combine(frame, leaf_verdict);
        }
    }
    return status;
}
