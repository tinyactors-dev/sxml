#pragma once

#include "cli_command.h"
#include "cli_output.h"
#include "sxml/status.h"

/*
 * The platform layer of the sxml program (docs/CLI.md, D44): the only code
 * that touches the C library's stdio. Everything else in apps/ reaches
 * standard output and standard error through these output streams.
 *
 * Implemented in platform_stdio.c with ISO C stdio, which Linux, macOS,
 * Windows (MSVC) and WASI all provide.
 *
 * Thread safety: call from the main thread only.
 */

/*
 * Prepares standard output and standard error for byte-exact writes: on
 * Windows it switches them to binary mode, so "\n" is not written as
 * "\r\n"; elsewhere it does nothing. Call once, before any write. Returns
 * SXML_STATUS_INPUT_OUTPUT_FAILED when the platform refuses.
 */
SXML_NODISCARD sxml_status sxml_platform_prepare_streams(void);

/* The process's standard output as an output stream. */
sxml_cli_output_stream sxml_platform_standard_output(void);

/* The process's standard error as an output stream. */
sxml_cli_output_stream sxml_platform_standard_error(void);

/*
 * Flushes standard output and standard error. Returns
 * SXML_STATUS_INPUT_OUTPUT_FAILED when a buffered write failed.
 */
SXML_NODISCARD sxml_status sxml_platform_flush_streams(void);

/*
 * The file reader over ISO C stdio: reads named files in binary mode, and
 * standard input for "-" (docs/CLI.md: standard input only when "-" is
 * given).
 */
sxml_cli_file_reader sxml_platform_file_reader(void);

/*
 * Writes the working directory into memory and a view of it to *path: an
 * absolute path with '/' between its parts (backslashes of Windows become
 * '/'), for the file names sxml transform turns into URIs
 * (apps/sxml/README.md). *path is empty when the platform cannot tell or
 * the path does not fit in memory; that is not a failure.
 */
void sxml_platform_read_working_directory(sxml_memory_view memory, sxml_byte_view *path);
