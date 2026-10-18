#pragma once

#include "function_library_internal.h"
#include "sxml/status.h"
#include "xpath_static_internal.h"

/*
 * The functions XQuery adds to the host's library (docs/DECISIONS.md,
 * D154; packet 3.14): fn:load-xquery-module (F&O 4.0, 18.2), which
 * compiles a library module at run time and so needs the XQuery front end.
 * Its row lives in src/xquery_load_module.c, not in a library part, so
 * only a program that compiles XQuery links the XQuery grammar (the
 * JavaScript package's sxml.wasm does not, D146). Internal to sxml.
 *
 * sxml_xquery_compile_module puts this table in front of the host's
 * table for one compilation: the host's functions first, then these.
 * fn:function-lookup reads the processor's table and does not see them.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* The host's table and the library of the XQuery-only rows. */
typedef struct sxml_xquery_function_table {
    sxml_function_table host_table;
    sxml_function_library *library;
} sxml_xquery_function_table;

/*
 * Makes the library of the XQuery-only rows with host's allocator and item
 * heap, in front of host->function_table, into *table. Returns the
 * library's failures. Release with sxml_xquery_function_table_finalize
 * once the compilation is done (the unit keeps no reference to it).
 */
SXML_NODISCARD sxml_status sxml_xquery_function_table_initialize(
    sxml_xquery_function_table *table, const sxml_xpath_static_context *host);

/* The table to put into the compilation's static context. */
sxml_function_table sxml_xquery_function_table_as_table(sxml_xquery_function_table *table);

/* Releases the library; accepts a table whose initialize failed. */
void sxml_xquery_function_table_finalize(sxml_xquery_function_table *table);
