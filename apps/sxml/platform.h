#pragma once

#include "cli_command.h"
#include "cli_output.h"
#include "sxml/processor.h"
#include "sxml/status.h"

#include <stdint.h>

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

/* ------------------------------------------------------------ File system */

/* The longest path, with its terminating NUL, the file system handles. */
#define SXML_PLATFORM_PATH_CAPACITY ((size_t)4096)

/* A NUL-terminated path in a buffer of its own, so it travels as one
   object. */
typedef struct sxml_platform_path {
    char text[SXML_PLATFORM_PATH_CAPACITY];
} sxml_platform_path;

/*
 * The state of the platform's file system (platform_file_system.c), in
 * caller memory: the directories it resolves against and buffers for the
 * paths of one operation. Fields are private.
 */
typedef struct sxml_platform_file_system_state {
    sxml_platform_path current_directory;
    sxml_platform_path temporary_directory;
    sxml_platform_path first_path;
    sxml_platform_path second_path;
    sxml_platform_path result_path;
    uint64_t temporary_counter;
} sxml_platform_file_system_state;

/* Absolute directories; empty for the process's working directory and
   for $TMPDIR (else /tmp, and under WASI the current directory). */
typedef struct sxml_platform_file_system_configuration {
    sxml_byte_view current_directory;
    sxml_byte_view temporary_directory;
} sxml_platform_file_system_configuration;

/*
 * Makes the file system of EXPath File (include/sxml/processor.h,
 * sxml_file_system; D200) over the platform's files, with *state, which
 * must outlive it, and writes it to *file_system. Returns
 * SXML_STATUS_INPUT_OUTPUT_FAILED when a directory is not absolute or too
 * long, and SXML_STATUS_UNSUPPORTED_FEATURE on Windows, which has no
 * implementation yet; *file_system is then the null file system.
 */
SXML_NODISCARD sxml_status sxml_platform_file_system_initialize(
    sxml_platform_file_system_state *state,
    const sxml_platform_file_system_configuration *configuration,
    sxml_file_system *file_system);

/* ------------------------------------------------------------ File writer */

/* The state of the platform's file writer (platform_file_writer.c), in
   caller memory: a buffer for one path and the counter of temporary names.
   Fields are private. */
typedef struct sxml_platform_file_writer_state {
    sxml_platform_path path;
    uint64_t temporary_counter;
} sxml_platform_file_writer_state;

/*
 * The file writer over the platform's files (cli_command.h,
 * sxml_cli_file_writer), with *state, which it initializes and which must
 * outlive it: writes go to a temporary file beside the target, moved into
 * place in one step on commit (POSIX rename and link; MoveFileEx on
 * Windows).
 */
sxml_cli_file_writer sxml_platform_file_writer(sxml_platform_file_writer_state *state);
