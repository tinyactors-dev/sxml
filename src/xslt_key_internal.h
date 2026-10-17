#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "function_library_internal.h"
#include "sxml/status.h"
#include "xpath_static_internal.h"
#include "xslt_lower_internal.h"

/*
 * Keys and the XSLT functions (XSLT 4.0, sections 20 and 24;
 * docs/DECISIONS.md D149; packet 4.10). Internal to sxml.
 *
 * Two kinds of XSLT function, one function table:
 *
 * - Functions written in C, the library part `xslt` that only XSLT
 *   compilation names (src/function_library_xslt.c, D101): current,
 *   document, system-property, available-system-properties,
 *   function-available, element-available, type-available,
 *   unparsed-entity-uri, unparsed-entity-public-id.
 * - Stylesheet functions lowered into the unit from short XPath texts,
 *   because they depend on the stylesheet or need the IR (src/xslt_key.c):
 *   fn:key#2 and fn:key#3 over the stylesheet's xsl:key declarations, and
 *   fn:copy-of (the deep copy of xsl:copy-of) and fn:snapshot.
 *
 * sxml_xslt_declare_functions puts one table (glossary
 * `xslt_function_table`) in front of the function table of every XPath
 * static context of the stylesheet: it answers the stylesheet functions,
 * then the rows of the XSLT-only parts, then the table that was there (the
 * processor's). A stylesheet function becomes an IR function of the unit
 * the first time an expression names it; sxml_xslt_lower_functions, after
 * everything else is lowered, lowers the bodies of those asked for (and of
 * those their bodies ask for), so a stylesheet that never calls fn:key
 * gets no index code.
 *
 * Keys (20.2). Every xsl:key of one expanded name is one key. Its index
 * for one tree is a map from key values to the nodes that have them, in
 * document order: a build function of the root, map:build over the nodes
 * of the tree (and their attributes), whose keys are the use values of
 * every declaration whose pattern matches the node (an error in a pattern
 * is no match, D118), each as key-normalize makes it (strings under the
 * key's collation; one string for a composite key). fn:key looks the
 * index up through key-index, which calls the build function the first
 * time an evaluation asks for that key and tree and keeps the map in the
 * evaluation memo (D149) for the rest of the evaluation, so the index is
 * built once per document, lazily. The internal rows key-name, key-index
 * and key-normalize are sxml_xslt_key_functions.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* The rows static expressions use (9.7): the first five of the part. */
extern const sxml_native_function_view sxml_xslt_static_functions;

/* The internal rows behind fn:key, in no namespace, in no part. */
extern const sxml_native_function_view sxml_xslt_key_functions;

/* The XSLT function table of one stylesheet lowering. */
typedef struct sxml_xslt_function_table sxml_xslt_function_table;

/*
 * Before anything of the stylesheet is lowered (after the main function
 * is added): reads the xsl:key declarations (XTSE1205, XTSE1222), makes
 * the table and puts it in front of the table of every static context in
 * static_contexts (the lowering's, writable; lowering->static_context_count
 * of them). The table borrows lowering. Returns SXML_STATUS_STATIC_ERROR
 * with the error record filled, and the failures of the allocator, the
 * name pool and the function library; *table is then NULL.
 */
SXML_NODISCARD sxml_status sxml_xslt_declare_functions(sxml_xslt_lowering *lowering,
                                                       sxml_xpath_static_context *static_contexts,
                                                       sxml_xslt_function_table **table);

/*
 * After everything else is lowered (templates, the main function, global
 * variables): lowers the stylesheet functions expressions asked for, and
 * those their bodies ask for. Outside a lowering machine, with no
 * function entered. Fails as lowering does. NULL is allowed.
 */
SXML_NODISCARD sxml_status sxml_xslt_lower_functions(sxml_xslt_function_table *table);

/* Releases the table; NULL is allowed. The static contexts must not be
   used for lowering afterwards. */
void sxml_xslt_release_functions(sxml_xslt_function_table *table);
