#pragma once

#include "function_library_internal.h"
#include "sxml/status.h"
#include "xpath_static_internal.h"

/*
 * The functions XQuery adds to the host's library (docs/DECISIONS.md,
 * D154, D205): fn:put (XQUF 3.0, 6.1), which only XQuery has. Internal to
 * sxml.
 *
 * fn:load-xquery-module (F&O 4.0, 18.2) is not among them: its row is the
 * library part xquery_module (src/function_library_xquery_module.c), which
 * serves every language, and its body is sxml_xquery_load_module_body
 * below (src/xquery_load_module.c). Every build links XQuery (D272).
 *
 * sxml_xquery_compile_module puts this table in front of the host's
 * table for one compilation: the host's functions first, then these.
 * fn:function-lookup reads the processor's table and does not see them.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* The resumable body of fn:load-xquery-module: compiles the module, asks
   the machine for a unit evaluation and returns the module record. */
extern const sxml_resumable_native_body sxml_xquery_load_module_body;

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
