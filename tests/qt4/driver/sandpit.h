#pragma once

#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/view.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Sandpits (catalog-schema.xsd, "sandpit"; D200): for a test case whose
 * environment has one, the driver copies the sandpit's directory into a
 * fresh temporary directory of the host's file system and gives the test's
 * processor a file system whose CURRENT directory is that copy (its
 * TEMPORARY directory a sibling of it), so that what the EXPath File tests
 * write stays there; the copy is deleted afterwards.
 *
 * Every file is reached through the host's sxml_file_system (the platform
 * layer's, apps/sxml/platform.h): the driver library touches no file
 * itself. Directories are walked with a work list (D71).
 *
 * Thread safety: one thread at a time.
 */

/* The longest absolute path of a sandpit directory. */
#define SXML_QT4_SANDPIT_PATH_CAPACITY ((size_t)4096)

typedef struct sxml_qt4_sandpit {
    sxml_file_system host;
    sxml_allocator allocator;
    /* <root>: the fresh directory; <root>/sandpit, the current directory;
       <root>/temp, the temporary-file directory. */
    uint8_t root[SXML_QT4_SANDPIT_PATH_CAPACITY];
    size_t root_byte_count;
    uint8_t current[SXML_QT4_SANDPIT_PATH_CAPACITY];
    size_t current_byte_count;
    uint8_t temporary[SXML_QT4_SANDPIT_PATH_CAPACITY];
    size_t temporary_byte_count;
    /* The file system the test's processor gets: the host's, with the
       directories above; its state is the sandpit. */
    sxml_file_system file_system;
} sxml_qt4_sandpit;

typedef struct sxml_qt4_sandpit_request {
    /* Not the null file system. */
    sxml_file_system host;
    /* The directory to copy: absolute, or relative to the host's CURRENT
       directory, "/" between its parts. */
    sxml_byte_view source_directory;
    /* For the walk's work list. */
    sxml_allocator allocator;
} sxml_qt4_sandpit_request;

/*
 * Makes *sandpit: a temporary directory with a copy of the source. Returns
 * SXML_STATUS_INPUT_OUTPUT_FAILED when the host refuses or a path is too
 * long, and the allocator's failures; what was copied is then deleted.
 */
SXML_NODISCARD sxml_status sxml_qt4_sandpit_create(const sxml_qt4_sandpit_request *request,
                                                   sxml_qt4_sandpit *sandpit);

/* Deletes the sandpit's directory and everything in it. */
void sxml_qt4_sandpit_destroy(sxml_qt4_sandpit *sandpit);
