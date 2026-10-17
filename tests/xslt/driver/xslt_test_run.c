/*
 * Running one XSLT 4.0 test case (xslt_test_run.h). Imitates
 * tests/qt4/driver/test_run.c for outcomes and reasons.
 */
#include "xslt_test_run.h"

#include "reason.h"
#include "xslt_applicability.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xslt_test_empty_text = {.bytes = NULL,
                                                         .byte_count = 0};

sxml_byte_view
sxml_xslt_test_delivery_name(sxml_xslt_result_delivery result_delivery) {
    switch (result_delivery) {
    case SXML_XSLT_RESULT_DELIVERY_DEFAULT:
        return SXML_TEXT("default");
    case SXML_XSLT_RESULT_DELIVERY_RAW:
        return SXML_TEXT("raw");
    case SXML_XSLT_RESULT_DELIVERY_TREE:
        return SXML_TEXT("tree");
    case SXML_XSLT_RESULT_DELIVERY_SERIALIZED:
        break;
    }
    return SXML_TEXT("serialized");
}

sxml_byte_view
sxml_xslt_test_invocation_name(sxml_xslt_invocation_kind invocation_kind) {
    switch (invocation_kind) {
    case SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES:
        return SXML_TEXT("apply-templates");
    case SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE:
        return SXML_TEXT("call-template");
    case SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION:
        break;
    }
    return SXML_TEXT("call-function");
}

/* ------------------------------------------------------------------ Plan */

/* The first module of view whose role is not secondary; NULL for none. */
static const sxml_xslt_test_module *
sxml_xslt_test_first_principal(sxml_xslt_test_module_view modules) {
    for (size_t module_index = 0; module_index < modules.module_count;
         module_index += 1) {
        if (modules.modules[module_index].module_role !=
            SXML_XSLT_TEST_MODULE_ROLE_SECONDARY) {
            return &modules.modules[module_index];
        }
    }
    return NULL;
}

static size_t
sxml_xslt_test_count_static(sxml_xslt_test_parameter_view parameters) {
    size_t static_count = 0;
    for (size_t parameter_index = 0;
         parameter_index < parameters.parameter_count; parameter_index += 1) {
        if (parameters.parameters[parameter_index].is_static) {
            static_count += 1;
        }
    }
    return static_count;
}

/* The delivery <output> asks for (catalog-schema.xsd, "output"). */
static sxml_xslt_result_delivery
sxml_xslt_test_delivery_of(const sxml_xslt_test_output *output) {
    if (output->is_serialized) {
        return SXML_XSLT_RESULT_DELIVERY_SERIALIZED;
    }
    if (output->has_tree_choice) {
        return output->builds_tree ? SXML_XSLT_RESULT_DELIVERY_TREE
                                   : SXML_XSLT_RESULT_DELIVERY_RAW;
    }
    return SXML_XSLT_RESULT_DELIVERY_DEFAULT;
}

/* Finds the principal module of the plan; false for none. */
static bool sxml_xslt_test_find_principal(const sxml_xslt_test_case *test_case,
                                          sxml_xslt_test_plan *plan) {
    const sxml_xslt_test_module *module =
        sxml_xslt_test_first_principal(test_case->modules);
    if (module == NULL) {
        module = sxml_xslt_test_first_principal(test_case->environment.modules);
    }
    if (module != NULL) {
        plan->principal_kind = SXML_XSLT_TEST_PRINCIPAL_KIND_MODULE;
        plan->principal_module = module;
        plan->principal_module_source =
            module->module_kind == SXML_XSLT_TEST_MODULE_KIND_PACKAGE
                ? SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_PACKAGE
                : SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT;
        return true;
    }
    const sxml_xslt_test_source_detail_view details =
        test_case->environment.source_details;
    for (size_t source_index = 0; source_index < details.source_detail_count;
         source_index += 1) {
        if (details.source_details[source_index].defines_stylesheet) {
            plan->principal_kind = SXML_XSLT_TEST_PRINCIPAL_KIND_EMBEDDED;
            plan->principal_source_index = source_index;
            plan->principal_module_source =
                SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT;
            return true;
        }
    }
    return false;
}

sxml_status sxml_xslt_test_make_plan(const sxml_xslt_test_case *test_case,
                                     sxml_qt4_problem *problem,
                                     sxml_xslt_test_plan *plan) {
    sxml_xslt_test_plan result = {
        .principal_kind = SXML_XSLT_TEST_PRINCIPAL_KIND_MODULE,
        .principal_module = NULL,
        .principal_source_index = 0,
        .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT,
        .secondary_module_count = 0,
        .static_parameter_count = 0,
        .stylesheet_parameter_count = 0,
        .has_global_context_item = false,
        .global_context_source_index = 0,
        .invocation_kind = test_case->invocation.invocation_kind,
        .result_delivery = sxml_xslt_test_delivery_of(&test_case->output)};
    if (!sxml_xslt_test_find_principal(test_case, &result)) {
        if (problem != NULL) {
            *problem = (sxml_qt4_problem){
                .description = SXML_TEXT("no principal stylesheet in"),
                .subject = test_case->name,
                .location = {.source_name = test_case->test_set_file_path,
                             .line_number = 0,
                             .column_number = 0,
                             .byte_offset = 0}};
        }
        return SXML_STATUS_MALFORMED_INPUT;
    }
    const size_t module_count = test_case->modules.module_count +
                                test_case->environment.modules.module_count;
    result.secondary_module_count =
        result.principal_kind == SXML_XSLT_TEST_PRINCIPAL_KIND_MODULE
            ? module_count - 1
            : module_count;
    const sxml_xslt_test_parameter_view lists[2] = {
        test_case->parameters, test_case->environment.parameters};
    for (size_t list_index = 0; list_index < 2; list_index += 1) {
        const size_t static_count =
            sxml_xslt_test_count_static(lists[list_index]);
        result.static_parameter_count += static_count;
        result.stylesheet_parameter_count +=
            lists[list_index].parameter_count - static_count;
    }
    const sxml_qt4_source_view sources = test_case->environment.shared.sources;
    for (size_t source_index = 0; source_index < sources.source_count;
         source_index += 1) {
        if (sxml_byte_view_equals(sources.sources[source_index].role,
                                  SXML_TEXT("."))) {
            result.has_global_context_item = true;
            result.global_context_source_index = source_index;
            break;
        }
    }
    *plan = result;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------------- Run */

/* The problem of a plan, as the reason of an error outcome. */
static void sxml_xslt_test_append_problem(sxml_qt4_reason *reason,
                                          const sxml_qt4_problem *problem) {
    sxml_qt4_reason_append(reason, problem->description);
    if (problem->subject.byte_count != 0) {
        sxml_qt4_reason_append(reason, SXML_TEXT(" "));
        sxml_qt4_reason_append(reason, problem->subject);
    }
}

/* Decides the outcome of an applicable test case. */
typedef struct sxml_xslt_test_judgement {
    sxml_qt4_test_outcome outcome;
    sxml_qt4_reason *reason;
} sxml_xslt_test_judgement;

static void
sxml_xslt_test_judge_applicable(const sxml_xslt_test_run_request *request,
                                sxml_xslt_test_judgement *judgement) {
    const sxml_xslt_test_case *test_case = request->test_case;
    sxml_qt4_problem problem = {.description = sxml_xslt_test_empty_text,
                                .subject = sxml_xslt_test_empty_text,
                                .location = {.source_name =
                                                 sxml_xslt_test_empty_text,
                                             .line_number = 0,
                                             .column_number = 0,
                                             .byte_offset = 0}};
    sxml_xslt_test_assertion_survey survey = {
        .kind_counts = {0},
        .first_unchecked_kind = SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_WARNING,
        .has_unchecked_kind = false};
    const sxml_qt4_document *document =
        sxml_xslt_test_catalog_get_test_set_document(request->catalog);
    sxml_status status = SXML_STATUS_INVALID_ARGUMENT;
    if (document != NULL) {
        status = sxml_xslt_test_survey_assertions(
            &(sxml_xslt_test_assertion_request){
                .document = document,
                .result_element = test_case->result,
                .test_set_file_path = test_case->test_set_file_path,
                .problem = &problem},
            &survey);
    }
    if (status != SXML_STATUS_OK) {
        judgement->outcome = SXML_QT4_TEST_OUTCOME_ERROR;
        if (problem.description.byte_count == 0) {
            problem.description = sxml_status_name(status);
        }
        sxml_xslt_test_append_problem(judgement->reason, &problem);
        return;
    }
    if (survey.has_unchecked_kind) {
        judgement->outcome = SXML_QT4_TEST_OUTCOME_NOT_RUN;
        sxml_qt4_reason_append(judgement->reason, SXML_TEXT("unsupported: "));
        sxml_qt4_reason_append(
            judgement->reason,
            sxml_xslt_test_assertion_kind_name(survey.first_unchecked_kind));
        return;
    }
    sxml_xslt_test_plan plan = {.principal_module = NULL};
    status = sxml_xslt_test_make_plan(test_case, &problem, &plan);
    if (status != SXML_STATUS_OK) {
        judgement->outcome = SXML_QT4_TEST_OUTCOME_ERROR;
        sxml_xslt_test_append_problem(judgement->reason, &problem);
        return;
    }
    /* include/sxml/xslt.h has no implementation before packet 4.15; the
       plan is complete, and running it is that packet's (D117). */
    judgement->outcome = SXML_QT4_TEST_OUTCOME_NOT_RUN;
    sxml_qt4_reason_append(judgement->reason,
                           SXML_TEXT("XSLT not implemented"));
}

sxml_status
sxml_xslt_test_run_test_case(const sxml_xslt_test_run_request *request,
                             sxml_qt4_test_result *result) {
    sxml_qt4_reason reason = {.bytes = NULL,
                              .byte_count = 0,
                              .capacity = 0,
                              .is_truncated = false};
    const sxml_status status = sxml_qt4_reason_start(request->arena, &reason);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_qt4_applicability applicability = {.is_applicable = false};
    sxml_xslt_test_decide_applicability(request->test_case, &applicability);
    sxml_xslt_test_judgement judgement = {
        .outcome = SXML_QT4_TEST_OUTCOME_NOT_RUN, .reason = &reason};
    if (!applicability.is_applicable) {
        sxml_qt4_reason_append(&reason, SXML_TEXT("skip: "));
        sxml_qt4_reason_append(&reason, applicability.reason);
        if (applicability.detail.byte_count != 0) {
            sxml_qt4_reason_append(&reason, SXML_TEXT(" "));
            sxml_qt4_reason_append(&reason, applicability.detail);
        }
        if (applicability.is_inverted) {
            sxml_qt4_reason_append(&reason, SXML_TEXT(" (satisfied)"));
        }
    } else {
        sxml_xslt_test_judge_applicable(request, &judgement);
    }
    *result = (sxml_qt4_test_result){
        .outcome = judgement.outcome,
        .reason = judgement.outcome == SXML_QT4_TEST_OUTCOME_PASS
                      ? sxml_xslt_test_empty_text
                      : sxml_qt4_reason_text(&reason),
        .is_in_scope = applicability.is_in_scope};
    return SXML_STATUS_OK;
}
