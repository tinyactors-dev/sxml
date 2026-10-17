/*
 * The result assertions of XSLT 4.0 test cases, surveyed
 * (xslt_assertion.h). The walk imitates the explicit stack of
 * tests/qt4/driver/assertion.c (D71).
 */
#include "xslt_assertion.h"

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
