#pragma once

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
 * SXML_STATUS_INVALID_ARGUMENT when the platform refuses.
 */
SXML_NODISCARD sxml_status sxml_platform_prepare_streams(void);

/* The process's standard output as an output stream. */
sxml_cli_output_stream sxml_platform_standard_output(void);

/* The process's standard error as an output stream. */
sxml_cli_output_stream sxml_platform_standard_error(void);

/*
 * Flushes standard output and standard error. Returns
 * SXML_STATUS_INVALID_ARGUMENT when a buffered write failed: the stream
 * interface has no input/output status yet (docs/DECISIONS.md, P1).
 */
SXML_NODISCARD sxml_status sxml_platform_flush_streams(void);
