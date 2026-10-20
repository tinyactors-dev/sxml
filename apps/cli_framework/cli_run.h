#pragma once

#include "cli_command.h"
#include "cli_output.h"
#include "sxml/memory.h"
#include "sxml/view.h"

/*
 * Running a program: parse the command line against the program's table,
 * show help or a usage error, or run the command, and map the outcome to an
 * exit code (apps/sxml/README.md, "Exit codes").
 *
 *     sxml [-h | --help]          the program's help, exit 0
 *     sxml --version              "sxml 0.2.0", exit 0
 *     sxml help [COMMAND]         help, exit 0
 *     sxml COMMAND [ARGUMENT...]  the command's function
 *     sxml                        usage error, exit 2
 */

typedef struct sxml_cli_run_request {
    /* The arguments after the program name (argv[1] onwards). Borrowed for
       the call. */
    sxml_cli_argument_list argument_list;
    /* Where results and help go. */
    sxml_cli_output_stream standard_output;
    /* Where diagnostics and usage errors go. */
    sxml_cli_output_stream standard_error;
    /* Memory for the parsed arguments, and for commands. */
    sxml_allocator allocator;
    /* What "--version" reports after the program name, such as "0.2.0". */
    sxml_byte_view version_text;
    /* Passed to commands that read files; may have no operations. */
    sxml_cli_file_reader file_reader;
    /* Passed to commands that write files (--output); may have no
       operations. */
    sxml_cli_file_writer file_writer;
    /* Passed to commands: the working directory, an absolute path with
       '/' between its parts; empty when unknown. Borrowed for the call. */
    sxml_byte_view working_directory;
    /* Passed to commands that give EXPath File access (sxml
       --allow-file-access, the QT4 driver's sandpits; D200); the null
       file system when the platform has none. */
    sxml_file_system file_system;
    /* Passed to commands that load shared libraries (sxml
       --code-language, D288); may have no operations. */
    sxml_cli_library_loader library_loader;
} sxml_cli_run_request;

/*
 * Runs program as request describes and returns the exit code. Everything
 * the program writes goes to the request's streams; nothing else is touched.
 *
 * A table that breaks the rules of sxml_cli_program_validate gives
 * SXML_CLI_EXIT_INTERNAL_ERROR, as does running out of memory. A failed write
 * to either stream turns SXML_CLI_EXIT_SUCCESS and
 * SXML_CLI_EXIT_OPERATION_FAILED into SXML_CLI_EXIT_INPUT_OUTPUT_ERROR.
 */
sxml_cli_exit_code sxml_cli_run(const sxml_cli_program *program,
                                const sxml_cli_run_request *request);
