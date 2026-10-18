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
    &sxml_version_command,
    &sxml_xpath_command,
    &sxml_xquery_command,
};

static const sxml_byte_view sxml_program_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml check books.xml"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xpath --context-item=books.xml '//title/string()'"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xquery --context-item=books.xml titles.xq"),
    SXML_BYTE_VIEW_INITIALIZER("sxml transform list.xsl books.xml"),
};

static const sxml_cli_program sxml_program_entry = {
    .name = SXML_BYTE_VIEW_INITIALIZER("sxml"),
    .summary = SXML_BYTE_VIEW_INITIALIZER(
        "check, query and transform XML documents"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "sxml checks XML 1.0 documents, evaluates XPath 4.0 expressions and\n"
        "XQuery 4.0 queries, and runs XSLT 4.0 stylesheets. Results go to\n"
        "standard output, errors to standard error. Documents and modules\n"
        "are read from local files; sxml never uses the network. A FILE\n"
        "operand of - reads standard input.\n"
        "\n"
        "Exit status: 0 on success; 1 when the operation fails (a document\n"
        "is not well-formed, a query or stylesheet raises an error); 2 for a\n"
        "usage error; 3 when a file cannot be read or written; 4 for an\n"
        "internal error or an exceeded limit."),
    .commands = sxml_program_commands,
    .command_count =
        sizeof sxml_program_commands / sizeof sxml_program_commands[0],
    .examples = {.texts = sxml_program_examples,
                 .text_count = sizeof sxml_program_examples / sizeof sxml_program_examples[0]},
    .bug_report_location =
        SXML_BYTE_VIEW_INITIALIZER("https://github.com/dhamidi/sxml/issues"),
};

const sxml_cli_program *sxml_program(void) { return &sxml_program_entry; }
