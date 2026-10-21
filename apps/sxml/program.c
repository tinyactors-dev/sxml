/*
 * The command table of the sxml program (docs/CLI.md). Only commands that
 * work are listed: planned commands are documented in docs/CLI.md and added
 * here together with their code.
 */
#include "program.h"

static const sxml_cli_command *const sxml_program_commands[] = {
    &sxml_check_command,
    &sxml_serialize_command,
    &sxml_transform_command,
    &sxml_validate_command,
    &sxml_version_command,
    &sxml_xpath_command,
    &sxml_xquery_command,
};

static const sxml_byte_view sxml_program_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml check draft.xml"),
    SXML_BYTE_VIEW_INITIALIZER("sxml validate --schema=draft.xsd draft.xml"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xpath --context-item=draft.xml '//section/name/string()'"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xquery --context-item=draft.xml requirements.xq"),
    SXML_BYTE_VIEW_INITIALIZER("sxml transform outline.xsl draft.xml"),
};

static const sxml_cli_program sxml_program_entry = {
    .name = SXML_BYTE_VIEW_INITIALIZER("sxml"),
    .summary = SXML_BYTE_VIEW_INITIALIZER(
        "check, validate, query and transform XML documents"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "sxml checks XML 1.0 and 1.1 documents, validates them against DTDs\n"
        "and XSD 1.1 schemas, evaluates XPath 4.0 expressions and XQuery 4.0\n"
        "queries, and runs XSLT 4.0 stylesheets. Results go to standard\n"
        "output, errors to standard error. Documents, schemas and modules are\n"
        "read from local files; sxml never uses the network. A FILE operand\n"
        "of - reads standard input.\n"
        "\n"
        "Exit status: 0 on success; 1 when the operation fails (a document\n"
        "is not well-formed or not valid, a query or stylesheet raises an\n"
        "error); 2 for a usage error; 3 when a file cannot be read or\n"
        "written; 4 for an internal error or an exceeded limit."),
    .commands = sxml_program_commands,
    .command_count =
        sizeof sxml_program_commands / sizeof sxml_program_commands[0],
    .examples = {.texts = sxml_program_examples,
                 .text_count = sizeof sxml_program_examples / sizeof sxml_program_examples[0]},
    .bug_report_location =
        SXML_BYTE_VIEW_INITIALIZER("https://github.com/dhamidi/sxml/issues"),
};

const sxml_cli_program *sxml_program(void) { return &sxml_program_entry; }
