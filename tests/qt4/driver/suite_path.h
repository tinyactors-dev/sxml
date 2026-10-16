#pragma once

#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * Suite paths (glossary: suite_path): where a file of the QT4 suite is,
 * relative to the directory of catalog.xml, as "fn/abs.xml" or
 * "docs/works.xml". Every file attribute of a catalog or test-set file is
 * a relative reference resolved against the file that contains it
 * (docs/QT4_CATALOG.md, "Files"); keeping the result relative to the suite
 * makes driver output the same wherever the suite lives, natively and under
 * WASI.
 */

/* A reference to resolve. Views are borrowed for the call. */
typedef struct sxml_qt4_suite_path_request {
    /* The suite path of the file whose attribute holds the reference. */
    sxml_byte_view containing_file;
    /* The reference as written: a relative path with "/" separators (RFC
       3986, section 4.2, without scheme, query or fragment). One that
       starts with "/" is taken as it is. */
    sxml_byte_view reference;
    /* Receives the bytes of the result. */
    sxml_arena *arena;
} sxml_qt4_suite_path_request;

/*
 * Resolves request->reference against the directory of
 * request->containing_file and removes "." and ".." segments and empty
 * segments (RFC 3986, section 5.2.4, applied to paths), writing the result,
 * in the arena, to *suite_path. A ".." that would leave the suite directory
 * is kept, so "../x" stays "../x". Returns the arena's failures, leaving
 * *suite_path untouched.
 */
SXML_NODISCARD sxml_status sxml_qt4_suite_path_resolve(
    const sxml_qt4_suite_path_request *request, sxml_byte_view *suite_path);

/* Two pieces to put one after the other. */
typedef struct sxml_qt4_concatenation {
    sxml_byte_view first;
    sxml_byte_view second;
} sxml_qt4_concatenation;

/*
 * Writes first followed by second, in arena, to *result: for example the
 * directory of catalog.xml and a suite path, which give the path to read.
 * Returns the arena's failures, leaving *result untouched.
 */
SXML_NODISCARD sxml_status
sxml_qt4_concatenate(sxml_arena *arena, sxml_qt4_concatenation concatenation,
                     sxml_byte_view *result);
