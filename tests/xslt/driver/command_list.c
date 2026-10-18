/*
 * sxml-xslt-test list [--catalog=FILE] [--test-set=NAME] [TEST-LIST]
 *
 * Writes every selected XSLT 4.0 test case with its resolved environment,
 * its stylesheets, parameters and invocation, its plan (xslt_test_run.h),
 * its assertions and whether it runs (xslt_applicability.h). Imitates
 * tests/qt4/driver/command_list.c.
 *
 * Output, on standard output, one block per test case:
 *
 *     apply-templates conflict-resolution-01
 *       environment conflict-resolution-01 (test set)
 *       source . (content)
 *       stylesheet - tests/insn/apply-templates/conflict-resolution-01.xsl
 *       invoke apply-templates
 *       plan stylesheet tests/insn/apply-templates/...xsl, 0 secondary,
 *         0 static, 0 parameters, context item, delivery default
 *       assertions assert-xml 1
 *       skip: feature schema_aware
 *       out of scope: group strm
 *
 * (the plan on one line), then "N test cases: A applicable, S skipped; D
 * in scope". "-" stands for an empty value. Problems go to standard error
 * (selection.h) and the listing goes on: an unknown test set, test
 * case or environment, a test case without a principal stylesheet, an
 * unknown or malformed assertion, a name with an unbound prefix.
 *
 * Exit code: 3 when a file cannot be read, else 4 when sxml failed (out of
 * memory), else 1 after a problem, else 0.
 */
#include "cli_arguments.h"
#include "xslt_applicability.h"
#include "xslt_assertion.h"
#include "xslt_catalog.h"
#include "xslt_driver.h"
#include "selection.h"
#include "xslt_test_run.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xslt_test_empty_text = {.bytes = NULL,
                                                         .byte_count = 0};

static const sxml_cli_option sxml_xslt_test_list_option_table[] = {
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("catalog"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("FILE"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value =
            SXML_BYTE_VIEW_INITIALIZER("third_party/xslt40-test/catalog.xml"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("The catalog.xml of the suite"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("test-set"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("NAME"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = SXML_BYTE_VIEW_INITIALIZER(""),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "Select every test case of one test set"),
    },
};

static const sxml_cli_positional sxml_xslt_test_list_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("TEST-LIST"),
        .multiplicity = SXML_CLI_POSITIONAL_OPTIONAL,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "File naming test cases, one '<test-set> <test-case>' per line"),
    },
};

static const sxml_byte_view sxml_xslt_test_list_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml-xslt-test list tests/xslt/slice.txt"),
    SXML_BYTE_VIEW_INITIALIZER("sxml-xslt-test list --test-set=apply-templates"),
};

/* The state of one run of the command. */
typedef struct sxml_xslt_test_list_state {
    size_t listed_count;
    size_t applicable_count;
    size_t in_scope_count;
} sxml_xslt_test_list_state;

/* ----------------------------------------------------------------- Output */

/* Writes " value", or " -" for an empty value. */
static void sxml_xslt_test_write_value(sxml_cli_writer *output,
                                       sxml_byte_view value) {
    sxml_cli_writer_write_byte(output, ' ');
    if (value.byte_count == 0) {
        sxml_cli_writer_write_byte(output, '-');
    } else {
        sxml_cli_writer_write_escaped(output, value);
    }
}

/* Writes " label value" when value is not empty. */
typedef struct sxml_xslt_test_labelled_value {
    sxml_byte_view label;
    sxml_byte_view value;
} sxml_xslt_test_labelled_value;

static void sxml_xslt_test_write_optional(sxml_cli_writer *output,
                                          sxml_xslt_test_labelled_value field) {
    if (field.value.byte_count == 0) {
        return;
    }
    sxml_cli_writer_write_byte(output, ' ');
    sxml_cli_writer_write(output, field.label);
    sxml_xslt_test_write_value(output, field.value);
}

/* Writes " label" when is_set. */
typedef struct sxml_xslt_test_labelled_flag {
    sxml_byte_view label;
    bool is_set;
} sxml_xslt_test_labelled_flag;

static void sxml_xslt_test_write_flag(sxml_cli_writer *output,
                                      sxml_xslt_test_labelled_flag flag) {
    if (flag.is_set) {
        sxml_cli_writer_write_byte(output, ' ');
        sxml_cli_writer_write(output, flag.label);
    }
}

/* Writes " NAME" as written, then " Q{uri}local" for a prefixed name. */
static void sxml_xslt_test_write_name(sxml_cli_writer *output,
                                      const sxml_xslt_test_name *name) {
    sxml_xslt_test_write_value(output, name->lexical_name);
    const bool is_eqname = name->lexical_name.byte_count > 1 &&
                           name->lexical_name.bytes[0] == 'Q' &&
                           name->lexical_name.bytes[1] == '{';
    if (name->namespace_uri.byte_count != 0 && !is_eqname) {
        sxml_cli_writer_write(output, SXML_TEXT(" Q{"));
        sxml_cli_writer_write_escaped(output, name->namespace_uri);
        sxml_cli_writer_write_byte(output, '}');
        sxml_cli_writer_write_escaped(output, name->local_name);
    }
}

static void sxml_xslt_test_write_sources(sxml_cli_writer *output,
                                         const sxml_xslt_test_environment *environment) {
    const sxml_qt4_source_view sources = environment->shared.sources;
    for (size_t source_index = 0; source_index < sources.source_count;
         source_index += 1) {
        const sxml_qt4_source *source = &sources.sources[source_index];
        const sxml_xslt_test_source_detail *detail =
            &environment->source_details.source_details[source_index];
        sxml_cli_writer_write(output, SXML_TEXT("  source"));
        sxml_xslt_test_write_value(output, source->role);
        if (detail->has_content) {
            sxml_cli_writer_write(output, SXML_TEXT(" (content)"));
        } else {
            sxml_xslt_test_write_value(output, source->file_path);
        }
        sxml_xslt_test_write_optional(
            output, (sxml_xslt_test_labelled_value){.label = SXML_TEXT("uri"),
                                                    .value = source->uri});
        sxml_xslt_test_write_optional(
            output,
            (sxml_xslt_test_labelled_value){.label = SXML_TEXT("validation"),
                                            .value = source->validation});
        sxml_xslt_test_write_optional(
            output, (sxml_xslt_test_labelled_value){.label = SXML_TEXT("select"),
                                                    .value = detail->select});
        sxml_xslt_test_write_flag(
            output, (sxml_xslt_test_labelled_flag){
                        .label = SXML_TEXT("streaming"),
                        .is_set = detail->is_streamed});
        sxml_xslt_test_write_flag(
            output, (sxml_xslt_test_labelled_flag){.label = SXML_TEXT("dtd"),
                                                   .is_set = detail->needs_dtd});
        sxml_xslt_test_write_flag(
            output, (sxml_xslt_test_labelled_flag){
                        .label = SXML_TEXT("defines-stylesheet"),
                        .is_set = detail->defines_stylesheet});
        sxml_cli_writer_write_byte(output, '\n');
    }
}

static void sxml_xslt_test_write_modules(sxml_cli_writer *output,
                                         sxml_xslt_test_module_view modules) {
    for (size_t module_index = 0; module_index < modules.module_count;
         module_index += 1) {
        const sxml_xslt_test_module *module = &modules.modules[module_index];
        sxml_cli_writer_write(output, module->module_kind ==
                                              SXML_XSLT_TEST_MODULE_KIND_PACKAGE
                                          ? SXML_TEXT("  package")
                                          : SXML_TEXT("  stylesheet"));
        switch (module->module_role) {
        case SXML_XSLT_TEST_MODULE_ROLE_UNSPECIFIED:
            sxml_xslt_test_write_value(output, sxml_xslt_test_empty_text);
            break;
        case SXML_XSLT_TEST_MODULE_ROLE_PRINCIPAL:
            sxml_cli_writer_write(output, SXML_TEXT(" principal"));
            break;
        case SXML_XSLT_TEST_MODULE_ROLE_SECONDARY:
            sxml_cli_writer_write(output, SXML_TEXT(" secondary"));
            break;
        }
        sxml_xslt_test_write_value(output, module->file_path);
        sxml_xslt_test_write_optional(
            output, (sxml_xslt_test_labelled_value){.label = SXML_TEXT("uri"),
                                                    .value = module->uri});
        sxml_xslt_test_write_optional(
            output, (sxml_xslt_test_labelled_value){
                        .label = SXML_TEXT("name"), .value = module->package_name});
        sxml_xslt_test_write_optional(
            output,
            (sxml_xslt_test_labelled_value){.label = SXML_TEXT("version"),
                                            .value = module->package_version});
        sxml_cli_writer_write_byte(output, '\n');
    }
}

/* "  param NAME [select S] [as T] [static] [tunnel]", indented by
   indent_level levels. */
typedef struct sxml_xslt_test_parameter_lines {
    sxml_xslt_test_parameter_view parameters;
    size_t indent_level;
} sxml_xslt_test_parameter_lines;

static void
sxml_xslt_test_write_parameters(sxml_cli_writer *output,
                                sxml_xslt_test_parameter_lines lines) {
    for (size_t parameter_index = 0;
         parameter_index < lines.parameters.parameter_count;
         parameter_index += 1) {
        const sxml_xslt_test_parameter *parameter =
            &lines.parameters.parameters[parameter_index];
        sxml_cli_writer_write_spaces(output, 2 * lines.indent_level);
        sxml_cli_writer_write(output, SXML_TEXT("param"));
        sxml_xslt_test_write_name(output, &parameter->name);
        sxml_xslt_test_write_optional(
            output,
            (sxml_xslt_test_labelled_value){.label = SXML_TEXT("select"),
                                            .value = parameter->select});
        sxml_xslt_test_write_optional(
            output,
            (sxml_xslt_test_labelled_value){.label = SXML_TEXT("as"),
                                            .value = parameter->declared_type});
        sxml_xslt_test_write_flag(
            output, (sxml_xslt_test_labelled_flag){
                        .label = SXML_TEXT("static"),
                        .is_set = parameter->is_static});
        sxml_xslt_test_write_flag(
            output, (sxml_xslt_test_labelled_flag){
                        .label = SXML_TEXT("tunnel"),
                        .is_set = parameter->is_tunnel});
        sxml_cli_writer_write_byte(output, '\n');
    }
}

/* The shared environment's rarer lines. */
static void
sxml_xslt_test_write_environment_extras(sxml_cli_writer *output,
                                        const sxml_qt4_environment *shared) {
    for (size_t resource_index = 0;
         resource_index < shared->resources.resource_count;
         resource_index += 1) {
        const sxml_qt4_resource *resource =
            &shared->resources.resources[resource_index];
        sxml_cli_writer_write(output, SXML_TEXT("  resource"));
        sxml_xslt_test_write_value(output, resource->uri);
        sxml_xslt_test_write_value(output, resource->file_path);
        sxml_xslt_test_write_optional(
            output,
            (sxml_xslt_test_labelled_value){.label = SXML_TEXT("media-type"),
                                            .value = resource->media_type});
        sxml_cli_writer_write_byte(output, '\n');
    }
    for (size_t collection_index = 0;
         collection_index < shared->collections.collection_count;
         collection_index += 1) {
        const sxml_qt4_collection *collection =
            &shared->collections.collections[collection_index];
        sxml_cli_writer_write(output, SXML_TEXT("  collection"));
        sxml_xslt_test_write_value(output, collection->uri);
        sxml_cli_writer_write(output, SXML_TEXT(" sources "));
        sxml_cli_writer_write_decimal(output,
                                      collection->sources.source_count);
        sxml_cli_writer_write_byte(output, '\n');
    }
    for (size_t collation_index = 0;
         collation_index < shared->collations.collation_count;
         collation_index += 1) {
        sxml_cli_writer_write(output, SXML_TEXT("  collation"));
        sxml_xslt_test_write_value(
            output, shared->collations.collations[collation_index].uri);
        sxml_cli_writer_write_byte(output, '\n');
    }
    for (size_t schema_index = 0; schema_index < shared->schemas.schema_count;
         schema_index += 1) {
        sxml_cli_writer_write(output, SXML_TEXT("  schema"));
        sxml_xslt_test_write_value(output,
                                   shared->schemas.schemas[schema_index].uri);
        sxml_xslt_test_write_value(
            output, shared->schemas.schemas[schema_index].file_path);
        sxml_cli_writer_write_byte(output, '\n');
    }
    if (shared->has_context_item) {
        sxml_cli_writer_write(output, SXML_TEXT("  context-item"));
        sxml_xslt_test_write_value(output, shared->context_item_select);
        sxml_cli_writer_write_byte(output, '\n');
    }
}

static sxml_byte_view
sxml_xslt_test_origin_text(sxml_qt4_environment_origin origin) {
    switch (origin) {
    case SXML_QT4_ENVIRONMENT_ORIGIN_ABSENT:
        return sxml_xslt_test_empty_text;
    case SXML_QT4_ENVIRONMENT_ORIGIN_INLINE:
        return SXML_TEXT("(inline)");
    case SXML_QT4_ENVIRONMENT_ORIGIN_TEST_SET:
        return SXML_TEXT("(test set)");
    case SXML_QT4_ENVIRONMENT_ORIGIN_CATALOG:
        break;
    }
    return SXML_TEXT("(catalog)");
}

static void
sxml_xslt_test_write_environment(sxml_cli_writer *output,
                                 const sxml_xslt_test_environment *environment) {
    if (environment->shared.origin != SXML_QT4_ENVIRONMENT_ORIGIN_ABSENT) {
        sxml_cli_writer_write(output, SXML_TEXT("  environment"));
        if (environment->shared.name.byte_count != 0) {
            sxml_cli_writer_write_byte(output, ' ');
            sxml_cli_writer_write_escaped(output, environment->shared.name);
        }
        sxml_cli_writer_write_byte(output, ' ');
        sxml_cli_writer_write(
            output, sxml_xslt_test_origin_text(environment->shared.origin));
        sxml_cli_writer_write_byte(output, '\n');
    }
    sxml_xslt_test_write_sources(output, environment);
    sxml_xslt_test_write_modules(output, environment->modules);
    sxml_xslt_test_write_parameters(
        output, (sxml_xslt_test_parameter_lines){
                    .parameters = environment->parameters, .indent_level = 1});
    sxml_xslt_test_write_environment_extras(output, &environment->shared);
}

static void
sxml_xslt_test_write_invocation(sxml_cli_writer *output,
                                const sxml_xslt_test_invocation *invocation) {
    sxml_cli_writer_write(output, SXML_TEXT("  invoke "));
    sxml_cli_writer_write(
        output, sxml_xslt_test_invocation_name(invocation->invocation_kind));
    switch (invocation->invocation_kind) {
    case SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES:
        if (invocation->has_initial_mode) {
            sxml_cli_writer_write(output, SXML_TEXT(" mode"));
            sxml_xslt_test_write_name(output, &invocation->initial_mode);
        }
        sxml_xslt_test_write_optional(
            output, (sxml_xslt_test_labelled_value){
                        .label = SXML_TEXT("select"),
                        .value = invocation->initial_match_selection});
        break;
    case SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE:
        sxml_xslt_test_write_name(output, &invocation->initial_template);
        sxml_xslt_test_write_flag(
            output, (sxml_xslt_test_labelled_flag){
                        .label = SXML_TEXT("(initial-mode ignored)"),
                        .is_set = invocation->has_ignored_initial_mode});
        break;
    case SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION:
        sxml_xslt_test_write_name(output, &invocation->initial_function);
        break;
    }
    sxml_cli_writer_write_byte(output, '\n');
    sxml_xslt_test_write_parameters(
        output, (sxml_xslt_test_parameter_lines){
                    .parameters = invocation->parameters, .indent_level = 2});
}

static void sxml_xslt_test_write_output(sxml_cli_writer *output,
                                        const sxml_xslt_test_output *test_output) {
    if (!test_output->is_present) {
        return;
    }
    sxml_cli_writer_write(output, SXML_TEXT("  output"));
    sxml_xslt_test_write_flag(
        output, (sxml_xslt_test_labelled_flag){
                    .label = SXML_TEXT("serialize"),
                    .is_set = test_output->is_serialized});
    if (test_output->has_tree_choice) {
        sxml_cli_writer_write(output, test_output->builds_tree
                                          ? SXML_TEXT(" tree")
                                          : SXML_TEXT(" raw"));
    }
    sxml_xslt_test_write_flag(
        output, (sxml_xslt_test_labelled_flag){
                    .label = SXML_TEXT("not-well-formed"),
                    .is_set = !test_output->is_well_formed});
    sxml_xslt_test_write_optional(
        output, (sxml_xslt_test_labelled_value){
                    .label = SXML_TEXT("file"), .value = test_output->file_path});
    sxml_xslt_test_write_optional(
        output,
        (sxml_xslt_test_labelled_value){.label = SXML_TEXT("result-var"),
                                        .value = test_output->result_variable});
    sxml_cli_writer_write_byte(output, '\n');
}

static void sxml_xslt_test_write_dependencies(
    sxml_cli_writer *output, sxml_qt4_dependency_view dependencies) {
    for (size_t dependency_index = 0;
         dependency_index < dependencies.dependency_count;
         dependency_index += 1) {
        const sxml_qt4_dependency *dependency =
            &dependencies.dependencies[dependency_index];
        if (sxml_byte_view_equals(dependency->dependency_type,
                                  SXML_TEXT("spec"))) {
            continue;
        }
        sxml_cli_writer_write(output, SXML_TEXT("  dependency"));
        sxml_xslt_test_write_value(output, dependency->dependency_type);
        sxml_xslt_test_write_value(output, dependency->value);
        sxml_xslt_test_write_flag(
            output, (sxml_xslt_test_labelled_flag){
                        .label = SXML_TEXT("unsatisfied"),
                        .is_set = !dependency->requires_satisfaction});
        sxml_cli_writer_write_byte(output, '\n');
    }
}

/* "  plan PRINCIPAL, N secondary, S static, P parameters[, context item],
   delivery D". */
static void sxml_xslt_test_write_plan(sxml_cli_writer *output,
                                      const sxml_xslt_test_case *test_case,
                                      const sxml_xslt_test_plan *plan) {
    sxml_cli_writer_write(output, SXML_TEXT("  plan"));
    if (plan->principal_kind == SXML_XSLT_TEST_PRINCIPAL_KIND_EMBEDDED) {
        sxml_cli_writer_write(output, SXML_TEXT(" embedded"));
        sxml_xslt_test_write_value(
            output, test_case->environment.shared.sources
                        .sources[plan->principal_source_index]
                        .file_path);
    } else if (plan->principal_module_source ==
               SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_PACKAGE) {
        sxml_cli_writer_write(output, SXML_TEXT(" package"));
        sxml_xslt_test_write_value(output, plan->principal_module->uri);
    } else {
        sxml_cli_writer_write(output, SXML_TEXT(" stylesheet"));
        sxml_xslt_test_write_value(output, plan->principal_module->file_path);
    }
    sxml_cli_writer_write(output, SXML_TEXT(", "));
    sxml_cli_writer_write_decimal(output, plan->secondary_module_count);
    sxml_cli_writer_write(output, SXML_TEXT(" secondary, "));
    sxml_cli_writer_write_decimal(output, plan->static_parameter_count);
    sxml_cli_writer_write(output, SXML_TEXT(" static, "));
    sxml_cli_writer_write_decimal(output, plan->stylesheet_parameter_count);
    sxml_cli_writer_write(output, SXML_TEXT(" parameters"));
    if (plan->has_global_context_item) {
        sxml_cli_writer_write(output, SXML_TEXT(", context item"));
    }
    sxml_cli_writer_write(output, SXML_TEXT(", delivery "));
    sxml_cli_writer_write(output,
                          sxml_xslt_test_delivery_name(plan->result_delivery));
    sxml_cli_writer_write_byte(output, '\n');
}

/* "  assertions KIND COUNT ...", in kind order. */
static void
sxml_xslt_test_write_survey(sxml_cli_writer *output,
                            const sxml_xslt_test_assertion_survey *survey) {
    sxml_cli_writer_write(output, SXML_TEXT("  assertions"));
    for (size_t kind_index = 0;
         kind_index < SXML_XSLT_TEST_ASSERTION_KIND_COUNT; kind_index += 1) {
        if (survey->kind_counts[kind_index] == 0) {
            continue;
        }
        sxml_cli_writer_write_byte(output, ' ');
        sxml_cli_writer_write(output,
                              sxml_xslt_test_assertion_kind_name(
                                  (sxml_xslt_test_assertion_kind)kind_index));
        sxml_cli_writer_write_byte(output, ' ');
        sxml_cli_writer_write_decimal(output, survey->kind_counts[kind_index]);
    }
    sxml_cli_writer_write_byte(output, '\n');
}

static void
sxml_xslt_test_write_applicability(sxml_cli_writer *output,
                                   const sxml_qt4_applicability *applicability) {
    if (!applicability->is_applicable) {
        sxml_cli_writer_write(output, SXML_TEXT("  skip: "));
        sxml_cli_writer_write_escaped(output, applicability->reason);
        if (applicability->detail.byte_count != 0) {
            sxml_cli_writer_write_byte(output, ' ');
            sxml_cli_writer_write_escaped(output, applicability->detail);
        }
        if (applicability->is_inverted) {
            sxml_cli_writer_write(output, SXML_TEXT(" (satisfied)"));
        }
        sxml_cli_writer_write_byte(output, '\n');
    }
    if (applicability->non_goal_kind.byte_count != 0) {
        sxml_cli_writer_write(output, SXML_TEXT("  out of scope: "));
        sxml_cli_writer_write_escaped(output, applicability->non_goal_kind);
        sxml_cli_writer_write_byte(output, ' ');
        sxml_cli_writer_write_escaped(output, applicability->non_goal);
        sxml_cli_writer_write_byte(output, '\n');
    }
}

/* -------------------------------------------------------------- Selection */

/* Writes the plan and the assertions, or reports why they cannot be
   made. */
static void sxml_xslt_test_list_plan(sxml_qt4_selection *selection,
                                     const sxml_xslt_test_case *test_case) {
    sxml_cli_writer *output = selection->invocation->standard_output;
    sxml_qt4_problem problem = sxml_qt4_selection_no_problem();
    sxml_xslt_test_plan plan = {.principal_module = NULL};
    sxml_status status = sxml_xslt_test_make_plan(test_case, &problem, &plan);
    if (status == SXML_STATUS_OK) {
        sxml_xslt_test_write_plan(output, test_case, &plan);
    } else {
        sxml_qt4_selection_report_failure(selection, status, &problem);
    }
    sxml_xslt_test_assertion_survey survey = {
        .kind_counts = {0},
        .first_unchecked_kind = SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_WARNING,
        .has_unchecked_kind = false};
    problem = sxml_qt4_selection_no_problem();
    status = sxml_xslt_test_survey_assertions(
        &(sxml_xslt_test_assertion_request){
            .document =
                sxml_xslt_test_catalog_get_test_set_document(sxml_xslt_test_selection_catalog(selection)),
            .result_element = test_case->result,
            .test_set_file_path = test_case->test_set_file_path,
            .problem = &problem},
        &survey);
    if (status == SXML_STATUS_OK) {
        sxml_xslt_test_write_survey(output, &survey);
    } else {
        sxml_qt4_selection_report_failure(selection, status, &problem);
    }
}

static void sxml_xslt_test_list_one(void *state,
                                    sxml_qt4_selection *selection,
                                    size_t test_case_index) {
    sxml_xslt_test_list_state *list_state = state;
    sxml_qt4_problem problem = sxml_qt4_selection_no_problem();
    sxml_xslt_test_case test_case = {.name = sxml_xslt_test_empty_text};
    const sxml_status status = sxml_xslt_test_catalog_read_test_case(
        sxml_xslt_test_selection_catalog(selection),
        &(sxml_qt4_test_case_request){.test_case_index = test_case_index,
                                      .problem = &problem},
        &test_case);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_selection_report_failure(selection, status, &problem);
        return;
    }
    sxml_qt4_applicability applicability = {.is_applicable = false};
    sxml_xslt_test_decide_applicability(&test_case, &applicability);
    sxml_cli_writer *output = selection->invocation->standard_output;
    sxml_cli_writer_write_escaped(output, test_case.test_set_name);
    sxml_cli_writer_write_byte(output, ' ');
    sxml_cli_writer_write_escaped(output, test_case.name);
    sxml_cli_writer_write_byte(output, '\n');
    sxml_xslt_test_write_environment(output, &test_case.environment);
    sxml_xslt_test_write_modules(output, test_case.modules);
    sxml_xslt_test_write_parameters(
        output, (sxml_xslt_test_parameter_lines){
                    .parameters = test_case.parameters, .indent_level = 1});
    sxml_xslt_test_write_invocation(output, &test_case.invocation);
    sxml_xslt_test_write_output(output, &test_case.output);
    sxml_xslt_test_write_dependencies(output, test_case.test_set_dependencies);
    sxml_xslt_test_write_dependencies(output, test_case.dependencies);
    sxml_xslt_test_list_plan(selection, &test_case);
    sxml_xslt_test_write_applicability(output, &applicability);
    list_state->listed_count += 1;
    if (applicability.is_applicable) {
        list_state->applicable_count += 1;
    }
    if (applicability.is_in_scope) {
        list_state->in_scope_count += 1;
    }
}

/* ---------------------------------------------------------------- Command */

static sxml_status
sxml_xslt_test_find_option(const sxml_cli_invocation *invocation,
                           sxml_byte_view long_name,
                           sxml_cli_option_match *match) {
    return sxml_cli_arguments_find_option(
        invocation->arguments,
        (sxml_cli_option_lookup){.long_name = long_name, .occurrence_index = 0},
        match);
}

static void sxml_xslt_test_write_counts(sxml_cli_writer *output,
                                        const sxml_xslt_test_list_state *state) {
    sxml_cli_writer_write_decimal(output, state->listed_count);
    sxml_cli_writer_write(output, state->listed_count == 1
                                      ? SXML_TEXT(" test case: ")
                                      : SXML_TEXT(" test cases: "));
    sxml_cli_writer_write_decimal(output, state->applicable_count);
    sxml_cli_writer_write(output, SXML_TEXT(" applicable, "));
    sxml_cli_writer_write_decimal(output,
                                  state->listed_count - state->applicable_count);
    sxml_cli_writer_write(output, SXML_TEXT(" skipped; "));
    sxml_cli_writer_write_decimal(output, state->in_scope_count);
    sxml_cli_writer_write_line(output, SXML_TEXT(" in scope"));
}

static sxml_cli_exit_code
sxml_xslt_test_run_list_command(const sxml_cli_invocation *invocation) {
    const sxml_cli_option_match absent = {.is_present = false,
                                          .occurrence_count = 0,
                                          .value = sxml_xslt_test_empty_text};
    sxml_cli_option_match catalog_option = absent;
    sxml_cli_option_match test_set_option = absent;
    sxml_status status = sxml_xslt_test_find_option(
        invocation, SXML_TEXT("catalog"), &catalog_option);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_find_option(invocation, SXML_TEXT("test-set"),
                                            &test_set_option);
    }
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (test_set_option.is_present &&
        sxml_cli_arguments_positional_count(invocation->arguments) != 0) {
        return sxml_cli_invocation_report_usage_error(
            invocation, SXML_TEXT("give TEST-LIST or --test-set, not both"));
    }
    sxml_xslt_test_catalog *catalog = NULL;
    status = sxml_xslt_test_catalog_create(
        invocation->allocator,
        &(sxml_xslt_test_catalog_configuration){
            .allocator = invocation->allocator,
            .file_reader = invocation->file_reader,
            .catalog_path = catalog_option.value},
        &catalog);
    if (status != SXML_STATUS_OK) {
        return status == SXML_STATUS_INVALID_ARGUMENT
                   ? sxml_cli_invocation_report_usage_error(
                         invocation, SXML_TEXT("--catalog must not be empty"))
                   : SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_qt4_selection selection = {.invocation = invocation,
                                          .command_name = SXML_TEXT("list"),
                                          .catalog = sxml_xslt_test_catalog_as_test_catalog(catalog),
                                          .has_problem = false,
                                          .has_unreadable_file = false,
                                          .has_failed = false};
    sxml_xslt_test_list_state state = {
        .listed_count = 0, .applicable_count = 0, .in_scope_count = 0};
    sxml_qt4_problem problem = sxml_qt4_selection_no_problem();
    status = sxml_xslt_test_catalog_load(catalog, &problem);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_selection_report_failure(&selection, status, &problem);
    } else {
        sxml_qt4_selection_walk(
            &selection,
            &(sxml_qt4_selection_request){
                .has_test_set_name = test_set_option.is_present,
                .test_set_name = test_set_option.value,
                .visitor = {.state = &state,
                            .visit = sxml_xslt_test_list_one}});
        sxml_xslt_test_write_counts(invocation->standard_output, &state);
    }
    sxml_xslt_test_catalog_destroy(catalog);
    return sxml_qt4_selection_exit_code(&selection);
}

const sxml_cli_command sxml_xslt_test_list_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("list"),
    .summary = SXML_BYTE_VIEW_INITIALIZER(
        "List test cases with their stylesheets, plans and applicability"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Reads the XSLT 4.0 catalog with sxml's XML parser and writes each\n"
        "selected test case: its environment (sources, stylesheets,\n"
        "packages, parameters, resources, collections), the stylesheets\n"
        "and parameters of its test, how it invokes the stylesheet, its\n"
        "output and dependencies, the plan of its transformation, the\n"
        "kinds of its assertions, and, when it cannot run for XSLT 4.0,\n"
        "\"skip:\" and the reason; \"out of scope:\" when it is outside the\n"
        "D95 denominator. The last line counts the test cases, those that\n"
        "run, and those in scope. The selection is the test cases of\n"
        "TEST-LIST, those of the test set --test-set names, or with neither\n"
        "every test case of the catalog. Exits 1 when a test set, test\n"
        "case or environment does not exist or a test case is malformed, 3\n"
        "when a file cannot be read."),
    .options = sxml_xslt_test_list_option_table,
    .option_count = sizeof sxml_xslt_test_list_option_table /
                    sizeof sxml_xslt_test_list_option_table[0],
    .positionals = sxml_xslt_test_list_positionals,
    .positional_count = sizeof sxml_xslt_test_list_positionals /
                        sizeof sxml_xslt_test_list_positionals[0],
    .examples = {.texts = sxml_xslt_test_list_examples,
                 .text_count = sizeof sxml_xslt_test_list_examples /
                               sizeof sxml_xslt_test_list_examples[0]},
    .run = sxml_xslt_test_run_list_command,
};
