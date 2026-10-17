/*
 * sxml-qt4 list [--catalog=FILE] [--spec=XP40|XQ40] [--test-set=NAME]
 *               [TEST-LIST]
 *
 * Writes every selected QT4 test case with its resolved environment and
 * whether it runs for the target (catalog.h, applicability.h). Imitates
 * apps/sxml/command_check.c.
 *
 * Selection (glossary: test_selection): the test cases of TEST-LIST, in
 * its order; or every test case of the test set --test-set names; or,
 * with neither, every test case of the catalog. Giving both is a usage
 * error.
 *
 * Output, on standard output, one block per test case:
 *
 *     fn-doc fn-doc-6
 *       environment works-and-staff-uri (test set)
 *       source - docs/works.xml uri http://www.w3.org/fots/docs/works.xml
 *       ...
 *       skip: feature schemaImport
 *       out of scope: feature schemaImport
 *
 * ("out of scope" for a test case whose spec matches but that D95 leaves
 * out of the denominator, applicability.h), then "N test cases: A
 * applicable, S skipped; D in scope", where D is the D95 denominator of
 * the selection. Lines that would be empty
 * are left out; "-" stands for an empty value. Problems (an unknown test
 * set, test case or environment) go to standard error as
 * "sxml-qt4 list: FILE[:LINE]: description 'subject'" and the listing goes
 * on.
 *
 * Exit code: 3 when a file cannot be read, else 4 when sxml failed (out of
 * memory), else 1 after a problem, else 0. Skipped test cases are not
 * problems.
 */
#include "catalog.h"
#include "cli_arguments.h"
#include "driver.h"
#include "test_list.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_qt4_empty_text = {.bytes = NULL,
                                                   .byte_count = 0};

/* The entries of a test list: a few kilobytes per thousand lines. */
enum { SXML_QT4_LIST_ARENA_BLOCK_BYTE_COUNT = 16 * 1024 };

static const sxml_byte_view sxml_qt4_spec_choices[] = {
    SXML_BYTE_VIEW_INITIALIZER("XP40"),
    SXML_BYTE_VIEW_INITIALIZER("XQ40"),
};

static const sxml_cli_option sxml_qt4_list_option_table[] = {
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("catalog"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("FILE"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value =
            SXML_BYTE_VIEW_INITIALIZER("third_party/qt4tests/catalog.xml"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("The catalog.xml of the suite"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("spec"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_CHOICE,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("SPEC"),
        .choices = {.texts = sxml_qt4_spec_choices,
                    .text_count = sizeof sxml_qt4_spec_choices /
                                  sizeof sxml_qt4_spec_choices[0]},
        .default_value = SXML_BYTE_VIEW_INITIALIZER("XP40"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "The language and version the run targets"),
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

static const sxml_cli_positional sxml_qt4_list_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("TEST-LIST"),
        .multiplicity = SXML_CLI_POSITIONAL_OPTIONAL,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "File naming test cases, one '<test-set> <test-case>' per line"),
    },
};

static const sxml_byte_view sxml_qt4_list_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml-qt4 list tests/qt4/torture-slice.txt"),
    SXML_BYTE_VIEW_INITIALIZER("sxml-qt4 list --test-set=fn-abs"),
    SXML_BYTE_VIEW_INITIALIZER(
        "sxml-qt4 list --catalog=suite/catalog.xml --spec=XQ40"),
};

/* The state of one run of the command. */
typedef struct sxml_qt4_list_run {
    const sxml_cli_invocation *invocation;
    sxml_qt4_catalog *catalog;
    sxml_qt4_target target;
    size_t listed_count;
    size_t applicable_count;
    size_t in_scope_count;
    bool has_problem;
    bool has_unreadable_file;
    /* Out of memory or a bug: the run stops. */
    bool has_failed;
} sxml_qt4_list_run;

/* ---------------------------------------------------------------- Problems */

static void sxml_qt4_write_problem(const sxml_qt4_list_run *run,
                                   const sxml_qt4_problem *problem) {
    sxml_cli_writer *error = run->invocation->standard_error;
    sxml_cli_writer_write(error, run->invocation->program->name);
    sxml_cli_writer_write(error, SXML_TEXT(" list: "));
    if (problem->location.source_name.byte_count != 0) {
        sxml_cli_writer_write_escaped(error, problem->location.source_name);
        if (problem->location.line_number != 0) {
            sxml_cli_writer_write_byte(error, ':');
            sxml_cli_writer_write_decimal(error, problem->location.line_number);
            if (problem->location.column_number != 0) {
                sxml_cli_writer_write_byte(error, ':');
                sxml_cli_writer_write_decimal(error,
                                              problem->location.column_number);
            }
        }
        sxml_cli_writer_write(error, SXML_TEXT(": "));
    }
    sxml_cli_writer_write_escaped(error, problem->description);
    if (problem->subject.byte_count != 0) {
        sxml_cli_writer_write_byte(error, ' ');
        sxml_cli_writer_write_quoted(error, problem->subject);
    }
    sxml_cli_writer_write_byte(error, '\n');
}

/* Reports a failed status: what kind of failure decides the exit code. */
static void sxml_qt4_report_failure(sxml_qt4_list_run *run, sxml_status status,
                                    sxml_qt4_problem *problem) {
    if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        run->has_unreadable_file = true;
    } else if (status == SXML_STATUS_MALFORMED_INPUT ||
               status == SXML_STATUS_INVALID_UTF8 ||
               status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        run->has_problem = true;
    } else {
        run->has_failed = true;
        if (problem->description.byte_count == 0) {
            problem->description = sxml_status_name(status);
        }
    }
    if (problem->description.byte_count == 0) {
        problem->description = sxml_status_name(status);
    }
    sxml_qt4_write_problem(run, problem);
}

static sxml_qt4_problem sxml_qt4_no_problem(void) {
    return (sxml_qt4_problem){.description = sxml_qt4_empty_text,
                              .subject = sxml_qt4_empty_text,
                              .location = {.source_name = sxml_qt4_empty_text,
                                           .line_number = 0,
                                           .column_number = 0,
                                           .byte_offset = 0}};
}

/* ----------------------------------------------------------------- Output */

/* Writes " value", or " -" for an empty value. */
static void sxml_qt4_write_value(sxml_cli_writer *output,
                                 sxml_byte_view value) {
    sxml_cli_writer_write_byte(output, ' ');
    if (value.byte_count == 0) {
        sxml_cli_writer_write_byte(output, '-');
    } else {
        sxml_cli_writer_write_escaped(output, value);
    }
}

/* Writes " label value" when value is not empty. */
typedef struct sxml_qt4_labelled_value {
    sxml_byte_view label;
    sxml_byte_view value;
} sxml_qt4_labelled_value;

static void sxml_qt4_write_optional(sxml_cli_writer *output,
                                    sxml_qt4_labelled_value field) {
    if (field.value.byte_count == 0) {
        return;
    }
    sxml_cli_writer_write_byte(output, ' ');
    sxml_cli_writer_write(output, field.label);
    sxml_qt4_write_value(output, field.value);
}

/* Starts a line indented by indent_level levels of two spaces with
   label. */
typedef struct sxml_qt4_line_start {
    size_t indent_level;
    sxml_byte_view label;
} sxml_qt4_line_start;

static void sxml_qt4_start_line(sxml_cli_writer *output,
                                sxml_qt4_line_start start) {
    sxml_cli_writer_write_spaces(output, 2 * start.indent_level);
    sxml_cli_writer_write(output, start.label);
}

static void sxml_qt4_write_sources(sxml_cli_writer *output,
                                   sxml_qt4_source_view sources,
                                   size_t indent_level) {
    for (size_t source_index = 0; source_index < sources.source_count;
         source_index += 1) {
        const sxml_qt4_source *source = &sources.sources[source_index];
        sxml_qt4_start_line(
            output, (sxml_qt4_line_start){.indent_level = indent_level,
                                          .label = SXML_TEXT("source")});
        sxml_qt4_write_value(output, source->role);
        sxml_qt4_write_value(output, source->file_path);
        sxml_qt4_write_optional(
            output, (sxml_qt4_labelled_value){.label = SXML_TEXT("uri"),
                                              .value = source->uri});
        sxml_qt4_write_optional(
            output, (sxml_qt4_labelled_value){.label = SXML_TEXT("validation"),
                                              .value = source->validation});
        if (source->is_declared) {
            sxml_cli_writer_write(output, SXML_TEXT(" declared"));
        }
        sxml_cli_writer_write_byte(output, '\n');
    }
}

static void sxml_qt4_write_resources(sxml_cli_writer *output,
                                     sxml_qt4_resource_view resources,
                                     size_t indent_level) {
    for (size_t resource_index = 0; resource_index < resources.resource_count;
         resource_index += 1) {
        const sxml_qt4_resource *resource =
            &resources.resources[resource_index];
        sxml_qt4_start_line(
            output, (sxml_qt4_line_start){.indent_level = indent_level,
                                          .label = SXML_TEXT("resource")});
        sxml_qt4_write_value(output, resource->uri);
        sxml_qt4_write_value(output, resource->file_path);
        sxml_qt4_write_optional(
            output, (sxml_qt4_labelled_value){.label = SXML_TEXT("media-type"),
                                              .value = resource->media_type});
        sxml_qt4_write_optional(
            output, (sxml_qt4_labelled_value){.label = SXML_TEXT("encoding"),
                                              .value = resource->encoding});
        sxml_cli_writer_write_byte(output, '\n');
    }
}

static sxml_byte_view sxml_qt4_origin_text(sxml_qt4_environment_origin origin) {
    switch (origin) {
    case SXML_QT4_ENVIRONMENT_ORIGIN_ABSENT:
        return sxml_qt4_empty_text;
    case SXML_QT4_ENVIRONMENT_ORIGIN_INLINE:
        return SXML_TEXT("(inline)");
    case SXML_QT4_ENVIRONMENT_ORIGIN_TEST_SET:
        return SXML_TEXT("(test set)");
    case SXML_QT4_ENVIRONMENT_ORIGIN_CATALOG:
        break;
    }
    return SXML_TEXT("(catalog)");
}

/* The environment's lines after "environment ...". */
static void
sxml_qt4_write_environment_lists(sxml_cli_writer *output,
                                 const sxml_qt4_environment *environment) {
    sxml_qt4_write_sources(output, environment->sources, 1);
    for (size_t parameter_index = 0;
         parameter_index < environment->parameters.parameter_count;
         parameter_index += 1) {
        const sxml_qt4_parameter *parameter =
            &environment->parameters.parameters[parameter_index];
        sxml_cli_writer_write(output, SXML_TEXT("  param"));
        sxml_qt4_write_value(output, parameter->name);
        sxml_qt4_write_optional(
            output, (sxml_qt4_labelled_value){.label = SXML_TEXT("select"),
                                              .value = parameter->select});
        sxml_qt4_write_optional(output, (sxml_qt4_labelled_value){
                                            .label = SXML_TEXT("as"),
                                            .value = parameter->declared_type});
        sxml_qt4_write_optional(
            output, (sxml_qt4_labelled_value){.label = SXML_TEXT("source"),
                                              .value = parameter->source});
        if (parameter->is_declared) {
            sxml_cli_writer_write(output, SXML_TEXT(" declared"));
        }
        sxml_cli_writer_write_byte(output, '\n');
    }
    for (size_t declaration_index = 0;
         declaration_index <
         environment->namespace_declarations.namespace_declaration_count;
         declaration_index += 1) {
        const sxml_qt4_namespace_declaration *declaration =
            &environment->namespace_declarations
                 .namespace_declarations[declaration_index];
        sxml_cli_writer_write(output, SXML_TEXT("  namespace"));
        sxml_qt4_write_value(output, declaration->namespace_prefix);
        sxml_qt4_write_value(output, declaration->namespace_uri);
        sxml_cli_writer_write_byte(output, '\n');
    }
    if (environment->has_context_item) {
        sxml_cli_writer_write(output, SXML_TEXT("  context-item"));
        sxml_qt4_write_value(output, environment->context_item_select);
        sxml_cli_writer_write_byte(output, '\n');
    }
    if (environment->static_base_uri_kind !=
        SXML_QT4_STATIC_BASE_URI_TEST_SET_FILE) {
        sxml_cli_writer_write(output, SXML_TEXT("  static-base-uri"));
        sxml_qt4_write_value(output, environment->static_base_uri_kind ==
                                             SXML_QT4_STATIC_BASE_URI_ABSENT
                                         ? SXML_TEXT("#UNDEFINED")
                                         : environment->static_base_uri);
        sxml_cli_writer_write_byte(output, '\n');
    }
    sxml_qt4_write_resources(output, environment->resources, 1);
}

/* The attribute names of the decimal-format properties, in
   sxml_decimal_format_property_kind order (catalog-schema.xsd). */
static const sxml_byte_view sxml_qt4_decimal_format_property_names[] = {
    SXML_BYTE_VIEW_INITIALIZER("decimal-separator"),
    SXML_BYTE_VIEW_INITIALIZER("grouping-separator"),
    SXML_BYTE_VIEW_INITIALIZER("exponent-separator"),
    SXML_BYTE_VIEW_INITIALIZER("percent"),
    SXML_BYTE_VIEW_INITIALIZER("per-mille"),
    SXML_BYTE_VIEW_INITIALIZER("zero-digit"),
    SXML_BYTE_VIEW_INITIALIZER("digit"),
    SXML_BYTE_VIEW_INITIALIZER("pattern-separator"),
    SXML_BYTE_VIEW_INITIALIZER("infinity"),
    SXML_BYTE_VIEW_INITIALIZER("NaN"),
    SXML_BYTE_VIEW_INITIALIZER("minus-sign"),
};

/* "  decimal-format NAME [{URI}LOCAL] PROPERTY VALUE ...". */
static void
sxml_qt4_write_decimal_format(sxml_cli_writer *output,
                              const sxml_qt4_decimal_format *decimal_format) {
    sxml_cli_writer_write(output, SXML_TEXT("  decimal-format"));
    sxml_qt4_write_value(output, decimal_format->name);
    if (decimal_format->namespace_uri.byte_count != 0) {
        sxml_cli_writer_write(output, SXML_TEXT(" {"));
        sxml_cli_writer_write_escaped(output, decimal_format->namespace_uri);
        sxml_cli_writer_write_byte(output, '}');
        sxml_cli_writer_write_escaped(output, decimal_format->local_name);
    }
    const size_t name_count = sizeof sxml_qt4_decimal_format_property_names /
                              sizeof sxml_qt4_decimal_format_property_names[0];
    const sxml_decimal_format_property_view properties =
        decimal_format->properties;
    for (size_t property_index = 0;
         property_index < properties.decimal_format_property_count;
         property_index += 1) {
        const sxml_decimal_format_property *property =
            &properties.decimal_format_properties[property_index];
        if ((size_t)property->property_kind < name_count) {
            sxml_cli_writer_write_byte(output, ' ');
            sxml_cli_writer_write(
                output,
                sxml_qt4_decimal_format_property_names[property->property_kind]);
            sxml_qt4_write_value(output, property->property_value);
        }
    }
    sxml_cli_writer_write_byte(output, '\n');
}

/* The environment's rarer lines: collections and what needs more than
   sxml has. */
static void
sxml_qt4_write_environment_extras(sxml_cli_writer *output,
                                  const sxml_qt4_environment *environment) {
    for (size_t collection_index = 0;
         collection_index < environment->collections.collection_count;
         collection_index += 1) {
        const sxml_qt4_collection *collection =
            &environment->collections.collections[collection_index];
        sxml_cli_writer_write(output, SXML_TEXT("  collection"));
        sxml_qt4_write_value(output, collection->uri);
        sxml_cli_writer_write_byte(output, '\n');
        sxml_qt4_write_sources(output, collection->sources, 2);
        sxml_qt4_write_resources(output, collection->resources, 2);
        for (size_t query_index = 0;
             query_index < collection->queries.text_count; query_index += 1) {
            sxml_cli_writer_write(output, SXML_TEXT("    query"));
            sxml_qt4_write_value(output,
                                 collection->queries.texts[query_index]);
            sxml_cli_writer_write_byte(output, '\n');
        }
    }
    for (size_t collation_index = 0;
         collation_index < environment->collations.collation_count;
         collation_index += 1) {
        const sxml_qt4_collation *collation =
            &environment->collations.collations[collation_index];
        sxml_cli_writer_write(output, SXML_TEXT("  collation"));
        sxml_qt4_write_value(output, collation->uri);
        if (collation->is_default) {
            sxml_cli_writer_write(output, SXML_TEXT(" default"));
        }
        sxml_cli_writer_write_byte(output, '\n');
    }
    for (size_t format_index = 0;
         format_index < environment->decimal_formats.decimal_format_count;
         format_index += 1) {
        sxml_qt4_write_decimal_format(
            output, &environment->decimal_formats.decimal_formats[format_index]);
    }
    for (size_t schema_index = 0;
         schema_index < environment->schemas.schema_count; schema_index += 1) {
        sxml_cli_writer_write(output, SXML_TEXT("  schema"));
        sxml_qt4_write_value(output,
                             environment->schemas.schemas[schema_index].uri);
        sxml_qt4_write_value(
            output, environment->schemas.schemas[schema_index].file_path);
        sxml_cli_writer_write_byte(output, '\n');
    }
    if (environment->has_function_library) {
        sxml_cli_writer_write_line(output, SXML_TEXT("  function-library"));
    }
    if (environment->has_sandpit) {
        sxml_cli_writer_write_line(output, SXML_TEXT("  sandpit"));
    }
}

static void sxml_qt4_write_dependencies(sxml_cli_writer *output,
                                        sxml_qt4_dependency_view dependencies) {
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
        sxml_qt4_write_value(output, dependency->dependency_type);
        sxml_qt4_write_value(output, dependency->value);
        if (!dependency->requires_satisfaction) {
            sxml_cli_writer_write(output, SXML_TEXT(" unsatisfied"));
        }
        sxml_cli_writer_write_byte(output, '\n');
    }
}

static void
sxml_qt4_write_test_case(sxml_cli_writer *output,
                         const sxml_qt4_test_case *test_case,
                         const sxml_qt4_applicability *applicability) {
    sxml_cli_writer_write_escaped(output, test_case->test_set_name);
    sxml_cli_writer_write_byte(output, ' ');
    sxml_cli_writer_write_escaped(output, test_case->name);
    sxml_cli_writer_write_byte(output, '\n');
    if (test_case->query_file_path.byte_count != 0) {
        sxml_cli_writer_write(output, SXML_TEXT("  query-file"));
        sxml_qt4_write_value(output, test_case->query_file_path);
        sxml_cli_writer_write_byte(output, '\n');
    }
    const sxml_qt4_environment *environment = &test_case->environment;
    if (environment->origin != SXML_QT4_ENVIRONMENT_ORIGIN_ABSENT) {
        sxml_cli_writer_write(output, SXML_TEXT("  environment"));
        if (environment->name.byte_count != 0) {
            sxml_cli_writer_write_byte(output, ' ');
            sxml_cli_writer_write_escaped(output, environment->name);
        }
        sxml_cli_writer_write_byte(output, ' ');
        sxml_cli_writer_write(output,
                              sxml_qt4_origin_text(environment->origin));
        sxml_cli_writer_write_byte(output, '\n');
    }
    sxml_qt4_write_environment_lists(output, environment);
    sxml_qt4_write_environment_extras(output, environment);
    for (size_t module_index = 0;
         module_index < test_case->modules.module_count; module_index += 1) {
        const sxml_qt4_module *module =
            &test_case->modules.modules[module_index];
        sxml_cli_writer_write(output, SXML_TEXT("  module"));
        sxml_qt4_write_value(output, module->uri);
        sxml_qt4_write_value(output, module->file_path);
        sxml_qt4_write_optional(
            output, (sxml_qt4_labelled_value){.label = SXML_TEXT("location"),
                                              .value = module->location});
        sxml_cli_writer_write_byte(output, '\n');
    }
    sxml_qt4_write_dependencies(output, test_case->test_set_dependencies);
    sxml_qt4_write_dependencies(output, test_case->dependencies);
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

static void sxml_qt4_list_test_case(sxml_qt4_list_run *run,
                                    size_t test_case_index) {
    sxml_qt4_problem problem = sxml_qt4_no_problem();
    sxml_qt4_test_case test_case = {.name = sxml_qt4_empty_text};
    const sxml_status status = sxml_qt4_catalog_read_test_case(
        run->catalog,
        &(sxml_qt4_test_case_request){.test_case_index = test_case_index,
                                      .problem = &problem},
        &test_case);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_report_failure(run, status, &problem);
        return;
    }
    sxml_qt4_applicability applicability = {.is_applicable = false};
    sxml_qt4_test_case_decide_applicability(
        (sxml_qt4_test_case_target){.test_case = &test_case,
                                    .target = run->target},
        &applicability);
    sxml_qt4_write_test_case(run->invocation->standard_output, &test_case,
                             &applicability);
    run->listed_count += 1;
    if (applicability.is_applicable) {
        run->applicable_count += 1;
    }
    if (applicability.is_in_scope) {
        run->in_scope_count += 1;
    }
}

/* Opens test set test_set_index unless it is open already. Returns true
   when it is open. */
static bool sxml_qt4_open_test_set(sxml_qt4_list_run *run,
                                   size_t test_set_index) {
    size_t open_index = 0;
    if (sxml_qt4_catalog_get_open_test_set(run->catalog, &open_index) &&
        open_index == test_set_index) {
        return true;
    }
    sxml_qt4_problem problem = sxml_qt4_no_problem();
    const sxml_status status = sxml_qt4_catalog_open_test_set(
        run->catalog,
        &(sxml_qt4_test_set_request){.test_set_index = test_set_index,
                                     .problem = &problem});
    if (status != SXML_STATUS_OK) {
        sxml_qt4_report_failure(run, status, &problem);
        return false;
    }
    return true;
}

static void sxml_qt4_list_test_set(sxml_qt4_list_run *run,
                                   size_t test_set_index) {
    if (!sxml_qt4_open_test_set(run, test_set_index)) {
        return;
    }
    const size_t test_case_count =
        sxml_qt4_catalog_test_case_count(run->catalog);
    for (size_t test_case_index = 0;
         test_case_index < test_case_count && !run->has_failed;
         test_case_index += 1) {
        sxml_qt4_list_test_case(run, test_case_index);
    }
}

/* A name the selection asked for that the catalog does not have. */
typedef struct sxml_qt4_unknown_name {
    sxml_byte_view description;
    sxml_byte_view name;
    sxml_byte_view source_name;
    uint32_t line_number;
} sxml_qt4_unknown_name;

static void sxml_qt4_report_unknown(sxml_qt4_list_run *run,
                                    sxml_qt4_unknown_name unknown) {
    run->has_problem = true;
    sxml_qt4_write_problem(
        run,
        &(sxml_qt4_problem){.description = unknown.description,
                            .subject = unknown.name,
                            .location = {.source_name = unknown.source_name,
                                         .line_number = unknown.line_number,
                                         .column_number = 0,
                                         .byte_offset = 0}});
}

static void sxml_qt4_list_test_list(sxml_qt4_list_run *run,
                                    sxml_byte_view list_path) {
    const sxml_allocator allocator = run->invocation->allocator;
    sxml_byte_view text = sxml_qt4_empty_text;
    sxml_status status = sxml_cli_file_reader_read(
        run->invocation->file_reader,
        &(sxml_cli_file_read_request){.path = list_path,
                                      .allocator = allocator},
        &text);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_problem problem = {
            .description = SXML_TEXT("cannot read"),
            .subject = list_path,
            .location = {.source_name = sxml_qt4_empty_text,
                         .line_number = 0,
                         .column_number = 0,
                         .byte_offset = 0}};
        sxml_qt4_report_failure(run, status, &problem);
        return;
    }
    sxml_arena *arena = NULL;
    status = sxml_arena_create(
        allocator,
        &(sxml_arena_configuration){
            .block_allocator = allocator,
            .block_byte_count = SXML_QT4_LIST_ARENA_BLOCK_BYTE_COUNT,
            .maximum_block_byte_count = 0,
            .initial_memory = {.bytes = NULL, .byte_count = 0}},
        &arena);
    sxml_qt4_problem problem = sxml_qt4_no_problem();
    sxml_qt4_test_list test_list = {.entries = NULL, .entry_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_test_list_parse(
            &(sxml_qt4_test_list_request){.text = text,
                                          .source_name = list_path,
                                          .arena = arena,
                                          .problem = &problem},
            &test_list);
    }
    if (status != SXML_STATUS_OK) {
        sxml_qt4_report_failure(run, status, &problem);
    }
    for (size_t entry_index = 0;
         entry_index < test_list.entry_count && !run->has_failed;
         entry_index += 1) {
        const sxml_qt4_test_list_entry *entry = &test_list.entries[entry_index];
        size_t test_set_index = 0;
        if (!sxml_qt4_catalog_find_test_set(run->catalog, entry->test_set_name,
                                            &test_set_index)) {
            sxml_qt4_report_unknown(
                run, (sxml_qt4_unknown_name){
                         .description = SXML_TEXT("unknown test set"),
                         .name = entry->test_set_name,
                         .source_name = list_path,
                         .line_number = entry->line_number});
            continue;
        }
        if (!sxml_qt4_open_test_set(run, test_set_index)) {
            continue;
        }
        size_t test_case_index = 0;
        if (!sxml_qt4_catalog_find_test_case(
                run->catalog, entry->test_case_name, &test_case_index)) {
            sxml_qt4_report_unknown(
                run, (sxml_qt4_unknown_name){
                         .description = SXML_TEXT("unknown test case"),
                         .name = entry->test_case_name,
                         .source_name = list_path,
                         .line_number = entry->line_number});
            continue;
        }
        sxml_qt4_list_test_case(run, test_case_index);
    }
    sxml_arena_destroy(arena);
    sxml_allocator_release(allocator, (void *)text.bytes);
}

/* ---------------------------------------------------------------- Command */

/* The options of one invocation. */
typedef struct sxml_qt4_list_options {
    sxml_cli_option_match catalog;
    sxml_cli_option_match spec;
    sxml_cli_option_match test_set;
} sxml_qt4_list_options;

static sxml_status sxml_qt4_find_option(const sxml_cli_invocation *invocation,
                                        sxml_byte_view long_name,
                                        sxml_cli_option_match *match) {
    return sxml_cli_arguments_find_option(
        invocation->arguments,
        (sxml_cli_option_lookup){.long_name = long_name, .occurrence_index = 0},
        match);
}

static sxml_cli_exit_code
sxml_qt4_list_exit_code(const sxml_qt4_list_run *run) {
    if (run->has_unreadable_file) {
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    if (run->has_failed) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    return run->has_problem ? SXML_CLI_EXIT_OPERATION_FAILED
                            : SXML_CLI_EXIT_SUCCESS;
}

/* Lists the selection the options and positional name. */
static void sxml_qt4_list_selection(sxml_qt4_list_run *run,
                                    const sxml_qt4_list_options *options) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_byte_view list_path = sxml_qt4_empty_text;
    if (sxml_cli_arguments_get_positional(invocation->arguments, 0,
                                          &list_path) == SXML_STATUS_OK) {
        sxml_qt4_list_test_list(run, list_path);
        return;
    }
    if (options->test_set.is_present) {
        size_t test_set_index = 0;
        if (!sxml_qt4_catalog_find_test_set(
                run->catalog, options->test_set.value, &test_set_index)) {
            sxml_qt4_report_unknown(
                run, (sxml_qt4_unknown_name){.description =
                                                 SXML_TEXT("unknown test set"),
                                             .name = options->test_set.value,
                                             .source_name = sxml_qt4_empty_text,
                                             .line_number = 0});
            return;
        }
        sxml_qt4_list_test_set(run, test_set_index);
        return;
    }
    const size_t test_set_count = sxml_qt4_catalog_test_set_count(run->catalog);
    for (size_t test_set_index = 0;
         test_set_index < test_set_count && !run->has_failed;
         test_set_index += 1) {
        sxml_qt4_list_test_set(run, test_set_index);
    }
}

static sxml_cli_exit_code
sxml_qt4_run_list_command(const sxml_cli_invocation *invocation) {
    const sxml_cli_option_match absent = {.is_present = false,
                                          .occurrence_count = 0,
                                          .value = sxml_qt4_empty_text};
    sxml_qt4_list_options options = {
        .catalog = absent, .spec = absent, .test_set = absent};
    sxml_status status = sxml_qt4_find_option(invocation, SXML_TEXT("catalog"),
                                              &options.catalog);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_qt4_find_option(invocation, SXML_TEXT("spec"), &options.spec);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_find_option(invocation, SXML_TEXT("test-set"),
                                      &options.test_set);
    }
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (options.test_set.is_present &&
        sxml_cli_arguments_positional_count(invocation->arguments) != 0) {
        return sxml_cli_invocation_report_usage_error(
            invocation, SXML_TEXT("give TEST-LIST or --test-set, not both"));
    }
    sxml_qt4_catalog *catalog = NULL;
    status = sxml_qt4_catalog_create(invocation->allocator,
                                     &(sxml_qt4_catalog_configuration){
                                         .allocator = invocation->allocator,
                                         .file_reader = invocation->file_reader,
                                         .catalog_path = options.catalog.value},
                                     &catalog);
    if (status != SXML_STATUS_OK) {
        return status == SXML_STATUS_INVALID_ARGUMENT
                   ? sxml_cli_invocation_report_usage_error(
                         invocation, SXML_TEXT("--catalog must not be empty"))
                   : SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_qt4_list_run run = {
        .invocation = invocation,
        .catalog = catalog,
        .target = {.language = sxml_byte_view_equals(options.spec.value,
                                                     SXML_TEXT("XQ40"))
                                   ? SXML_QT4_LANGUAGE_XQUERY
                                   : SXML_QT4_LANGUAGE_XPATH,
                   .version = 40},
        .listed_count = 0,
        .applicable_count = 0,
        .in_scope_count = 0,
        .has_problem = false,
        .has_unreadable_file = false,
        .has_failed = false};
    sxml_qt4_problem problem = sxml_qt4_no_problem();
    status = sxml_qt4_catalog_load(catalog, &problem);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_report_failure(&run, status, &problem);
    } else {
        sxml_qt4_list_selection(&run, &options);
        sxml_cli_writer *output = invocation->standard_output;
        sxml_cli_writer_write_decimal(output, run.listed_count);
        sxml_cli_writer_write(output, run.listed_count == 1
                                          ? SXML_TEXT(" test case: ")
                                          : SXML_TEXT(" test cases: "));
        sxml_cli_writer_write_decimal(output, run.applicable_count);
        sxml_cli_writer_write(output, SXML_TEXT(" applicable, "));
        sxml_cli_writer_write_decimal(output,
                                      run.listed_count - run.applicable_count);
        sxml_cli_writer_write(output, SXML_TEXT(" skipped; "));
        sxml_cli_writer_write_decimal(output, run.in_scope_count);
        sxml_cli_writer_write_line(output, SXML_TEXT(" in scope"));
    }
    sxml_qt4_catalog_destroy(catalog);
    return sxml_qt4_list_exit_code(&run);
}

const sxml_cli_command sxml_qt4_list_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("list"),
    .summary = SXML_BYTE_VIEW_INITIALIZER(
        "List test cases with their environments and applicability"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Reads the QT4 catalog with sxml's XML parser and writes each "
        "selected\n"
        "test case: its query file, its environment (sources, parameters,\n"
        "namespaces, static base URI, resources, collections) with file paths\n"
        "relative to the catalog's directory, its dependencies, and, when it\n"
        "cannot run for SPEC, \"skip:\" and the reason; \"out of scope:\"\n"
        "when it needs a non-goal of D95. The last line counts the test\n"
        "cases, those that run, and those in scope: the denominator of the\n"
        "pass-rate targets. The selection is the test cases of TEST-LIST,\n"
        "those of the test set --test-set names, or with neither every test\n"
        "case of the catalog. Exits 1 when a test set, test case or\n"
        "environment does not exist, 3 when a file cannot be "
        "read."),
    .options = sxml_qt4_list_option_table,
    .option_count = sizeof sxml_qt4_list_option_table /
                    sizeof sxml_qt4_list_option_table[0],
    .positionals = sxml_qt4_list_positionals,
    .positional_count =
        sizeof sxml_qt4_list_positionals / sizeof sxml_qt4_list_positionals[0],
    .examples = {.texts = sxml_qt4_list_examples,
                 .text_count = sizeof sxml_qt4_list_examples /
                               sizeof sxml_qt4_list_examples[0]},
    .run = sxml_qt4_run_list_command,
};
