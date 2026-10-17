#pragma once

#include <stddef.h>
#include <stdint.h>

#include "function_library_internal.h"
#include "sxml/view.h"

/*
 * The library part registry (docs/DECISIONS.md, D45, D101). Internal to
 * sxml.
 *
 * Every file src/function_library_<part>.c that defines
 *
 *     const sxml_native_function_view sxml_function_library_<part> = ...;
 *
 * is a library part (glossary `library_part`). CMake (cmake/SxmlParts.cmake)
 * reads those files and generates the registry below into the build tree,
 * so a new part is one new file: the processor, the fuzz fixture and the
 * tests find it without an edit. A file without that definition (the case
 * tables, deep equality, src/function_library.c itself) is not a part.
 *
 * A part serves XPath, XQuery and XSLT unless its file also defines its
 * language set (glossary `language_set`), for example an XSLT-only part
 * (XSLT 4.0 functions such as fn:current, packet 4.10):
 *
 *     const sxml_language_set sxml_function_library_<part>_languages =
 *         SXML_LANGUAGE_XSLT;
 *
 * CMake reads the language names from that line, so the per-language lists
 * are static data; the registry also points at the variable, and a unit
 * test checks that the two agree. A part file needs no declaration of its
 * own: the generated unit declares the view and the language set.
 *
 * Thread safety: the registry is static const data, safe from any thread.
 */

/* One language a compilation serves; values are bits of a language set. */
typedef enum sxml_language {
    SXML_LANGUAGE_XPATH = 1,
    SXML_LANGUAGE_XQUERY = 2,
    SXML_LANGUAGE_XSLT = 4,
} sxml_language;

/* A set of languages: sxml_language bits joined by "|". */
typedef uint32_t sxml_language_set;

#define SXML_LANGUAGE_SET_ALL                                                 \
    ((sxml_language_set)(SXML_LANGUAGE_XPATH | SXML_LANGUAGE_XQUERY |          \
                         SXML_LANGUAGE_XSLT))

/* A registered library part. */
typedef struct sxml_function_library_part {
    /* The <part> of its file name, for diagnostics and tests. */
    sxml_byte_view part_name;
    const sxml_native_function_view *native_functions;
    /* The languages it serves, as CMake read them from the file. */
    sxml_language_set languages;
    /* The part's own sxml_function_library_<part>_languages, or NULL when
       the file defines none (every language). */
    const sxml_language_set *declared_languages;
} sxml_function_library_part;

typedef struct sxml_function_library_part_view {
    const sxml_function_library_part *parts;
    size_t part_count;
} sxml_function_library_part_view;

/* Every registered part, sorted by part name. Defined in the generated
   function_library_parts.c. */
sxml_function_library_part_view sxml_function_library_registered_parts(void);

/*
 * The parts serving language, sorted by part name, as a list to put into
 * sxml_function_library_configuration.parts (static data, valid for the
 * life of the program). A value that is not one sxml_language gives the
 * empty list. Defined in the generated function_library_parts.c.
 */
sxml_native_function_view_list sxml_function_library_parts_for_language(
    sxml_language language);
