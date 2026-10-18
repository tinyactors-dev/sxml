/*
 * Running one XSD 1.1 test (xsd_test_run.h). Imitates
 * tests/xslt/driver/xslt_test_run.c and xslt_execution.c: the decision
 * first, then an isolated run in a processor of its own through the
 * shared test budget (budget.h).
 */
#include "xsd_test_run.h"

#include "budget.h"
#include "reason.h"
#include "sxml/processor.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xsd_test_empty_text = {.bytes = NULL,
                                                        .byte_count = 0};

/* As the QT4 driver: entity expansion bounded, nesting not. */
enum { SXML_XSD_TEST_RUN_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

/* What running a test decided. */
typedef struct sxml_xsd_test_answer {
    sxml_qt4_test_outcome outcome;
    sxml_qt4_reason *reason;
} sxml_xsd_test_answer;

/* Ends the reason with "STAGE[ SUBJECT]: STATUS" and makes the outcome an
   error. */
typedef struct sxml_xsd_test_failure {
    sxml_byte_view stage;
    sxml_byte_view subject;
    sxml_status status;
} sxml_xsd_test_failure;

static void sxml_xsd_test_fail(sxml_xsd_test_answer *answer,
                               sxml_xsd_test_failure failure) {
    answer->outcome = SXML_QT4_TEST_OUTCOME_ERROR;
    sxml_qt4_reason_append(answer->reason, failure.stage);
    if (failure.subject.byte_count != 0) {
        sxml_qt4_reason_append(answer->reason, SXML_TEXT(" "));
        sxml_qt4_reason_append(answer->reason, failure.subject);
    }
    sxml_qt4_reason_append(answer->reason, SXML_TEXT(": "));
    sxml_qt4_reason_append(answer->reason,
                           failure.status == SXML_STATUS_INPUT_OUTPUT_FAILED
                               ? SXML_TEXT("cannot read")
                               : sxml_status_name(failure.status));
}

/* Reads the suite documents of a view and releases them: they exist. */
typedef struct sxml_xsd_test_document_check {
    sxml_xsd_test_catalog *catalog;
    sxml_allocator allocator;
    sxml_xsd_test_document_view documents;
} sxml_xsd_test_document_check;

static bool
sxml_xsd_test_read_documents(const sxml_xsd_test_document_check *check,
                             sxml_xsd_test_answer *answer) {
    for (size_t document_index = 0;
         document_index < check->documents.file_path_count;
         document_index += 1) {
        const sxml_byte_view file_path =
            check->documents.file_paths[document_index];
        sxml_byte_view contents = sxml_xsd_test_empty_text;
        const sxml_status status = sxml_xsd_test_catalog_read_file(
            check->catalog,
            &(sxml_qt4_file_request){.suite_path = file_path,
                                     .allocator = check->allocator},
            &contents);
        if (status != SXML_STATUS_OK) {
            sxml_xsd_test_fail(
                answer, (sxml_xsd_test_failure){.stage = SXML_TEXT("document"),
                                                .subject = file_path,
                                                .status = status});
            return false;
        }
        sxml_allocator_release(check->allocator, (void *)contents.bytes);
    }
    return true;
}

/* Runs an applicable test in its own processor and budget. */
static void sxml_xsd_test_run_isolated(const sxml_xsd_test_run_request *request,
                                       sxml_xsd_test_answer *answer) {
    sxml_qt4_budget budget = sxml_qt4_budget_start(request->allocator);
    const sxml_allocator allocator = sxml_qt4_budget_allocator(&budget);
    sxml_processor *processor = NULL;
    const sxml_status status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .limits = {.xml_limits =
                           {.maximum_depth = 0,
                            .maximum_entity_expansion_byte_count =
                                SXML_XSD_TEST_RUN_MAXIMUM_EXPANSION_BYTE_COUNT},
                       .maximum_evaluation_step_count =
                           SXML_QT4_TEST_MAXIMUM_STEP_COUNT}},
        &processor);
    const sxml_xsd_test_case *test_case = request->test_case;
    if (status != SXML_STATUS_OK) {
        sxml_xsd_test_fail(
            answer, (sxml_xsd_test_failure){.stage = SXML_TEXT("processor"),
                                            .subject = sxml_xsd_test_empty_text,
                                            .status = status});
    } else if (sxml_xsd_test_read_documents(
                   &(sxml_xsd_test_document_check){
                       .catalog = request->catalog,
                       .allocator = allocator,
                       .documents = test_case->schema_documents},
                   answer) &&
               (test_case->kind == SXML_XSD_TEST_KIND_SCHEMA ||
                sxml_xsd_test_read_documents(
                    &(sxml_xsd_test_document_check){.catalog = request->catalog,
                                                    .allocator = allocator,
                                                    .documents =
                                                        test_case->documents},
                    answer))) {
        /* Packets 5.4 and 5.7 compile and validate here (xsd_test_run.h);
           until then the outcome is that of an unsupported feature. */
        answer->outcome = SXML_QT4_TEST_OUTCOME_NOT_RUN;
        sxml_qt4_reason_append(answer->reason,
                               SXML_TEXT("XSD not implemented"));
    }
    sxml_processor_destroy(processor);
    sxml_qt4_budget_finish(
        &budget, (sxml_qt4_budget_verdict){.outcome = &answer->outcome,
                                           .reason = answer->reason});
}

sxml_status
sxml_xsd_test_run_test_case(const sxml_xsd_test_run_request *request,
                            sxml_qt4_test_result *result) {
    sxml_qt4_reason reason = {
        .bytes = NULL, .byte_count = 0, .capacity = 0, .is_truncated = false};
    const sxml_status status = sxml_qt4_reason_start(request->arena, &reason);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xsd_test_answer answer = {.outcome = SXML_QT4_TEST_OUTCOME_FAIL,
                                   .reason = &reason};
    sxml_xsd_test_decision decision;
    sxml_xsd_test_decide(request->test_case, &decision);
    if (!decision.applicability.is_applicable) {
        answer.outcome = SXML_QT4_TEST_OUTCOME_NOT_RUN;
        sxml_qt4_reason_append(&reason, SXML_TEXT("skip: "));
        sxml_qt4_reason_append(&reason, decision.applicability.reason);
        if (decision.applicability.detail.byte_count != 0) {
            sxml_qt4_reason_append(&reason, SXML_TEXT(" "));
            sxml_qt4_reason_append(&reason, decision.applicability.detail);
        }
    } else {
        sxml_xsd_test_run_isolated(request, &answer);
    }
    *result = (sxml_qt4_test_result){
        .outcome = answer.outcome,
        .reason = answer.outcome == SXML_QT4_TEST_OUTCOME_PASS
                      ? sxml_xsd_test_empty_text
                      : sxml_qt4_reason_text(&reason),
        .is_in_scope = decision.applicability.is_in_scope};
    return SXML_STATUS_OK;
}
