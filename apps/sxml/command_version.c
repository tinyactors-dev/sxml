/*
 * sxml version [--format=text|json]: prints the version of the sxml library
 * the program runs on (include/sxml/version.h).
 *
 * The model for every command (docs/CLI.md, "Implementation map"): the
 * options and the command entry are static const data next to the function,
 * the function reads its arguments through cli_arguments.h, writes its result
 * to invocation->standard_output and returns an exit code.
 */
#include "cli_arguments.h"
#include "program.h"
#include "sxml/memory.h"
#include "sxml/version.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_version_formats[] = {
    SXML_BYTE_VIEW_INITIALIZER("text"),
    SXML_BYTE_VIEW_INITIALIZER("json"),
};

static const sxml_cli_option sxml_version_options[] = {
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("format"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_CHOICE,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("FORMAT"),
        .choices = {.texts = sxml_version_formats,
                    .text_count = sizeof sxml_version_formats /
                                  sizeof sxml_version_formats[0]},
        .default_value = SXML_BYTE_VIEW_INITIALIZER("text"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("Output format"),
    },
};

static const sxml_byte_view sxml_version_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml version"),
    SXML_BYTE_VIEW_INITIALIZER("sxml version --format=json"),
};

static sxml_cli_exit_code sxml_run_version_command(
    const sxml_cli_invocation *invocation) {
    sxml_cli_option_match format = {
        .is_present = false,
        .occurrence_count = 0,
        .value = {.bytes = NULL, .byte_count = 0},
    };
    const sxml_status status = sxml_cli_arguments_find_option(
        invocation->arguments,
        (sxml_cli_option_lookup){.long_name = SXML_TEXT("format"),
                                 .occurrence_index = 0},
        &format);
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    const sxml_byte_view version_text =
        sxml_byte_view_from_c_string(sxml_version_text());
    sxml_cli_writer *output = invocation->standard_output;
    if (sxml_byte_view_equals(format.value, SXML_TEXT("json"))) {
        /* A stable shape: fields may be added, never changed (docs/CLI.md). */
        sxml_cli_writer_write(output, SXML_TEXT("{\"name\":"));
        sxml_cli_writer_write_json_string(output, invocation->program->name);
        sxml_cli_writer_write(output, SXML_TEXT(",\"version\":"));
        sxml_cli_writer_write_json_string(output, version_text);
        sxml_cli_writer_write_line(output, SXML_TEXT("}"));
        return SXML_CLI_EXIT_SUCCESS;
    }
    /* The same line as "sxml --version". */
    sxml_cli_writer_write(output, invocation->program->name);
    sxml_cli_writer_write_byte(output, ' ');
    sxml_cli_writer_write_line(output, version_text);
    return SXML_CLI_EXIT_SUCCESS;
}

const sxml_cli_command sxml_version_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("version"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("Print the sxml library version"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Prints the version of the sxml library this program runs on, the same\n"
        "line as 'sxml --version'. With --format=json, prints one JSON object\n"
        "with the fields \"name\" and \"version\"."),
    .options = sxml_version_options,
    .option_count = sizeof sxml_version_options / sizeof sxml_version_options[0],
    .positionals = NULL,
    .positional_count = 0,
    .examples = {.texts = sxml_version_examples,
                 .text_count = sizeof sxml_version_examples /
                               sizeof sxml_version_examples[0]},
    .run = sxml_run_version_command,
};
