#pragma once

/*
 * The CLDR data the sxml command serves (include/sxml/processor.h,
 * SXML_RESOURCE_KIND_COLLATION_DATA and SXML_RESOURCE_KIND_LOCALE_DATA;
 * D213, D220, D221): files of the vendored CLDR subset
 * (third_party/cldr), the root collation's allkeys_CLDR.txt without its
 * comments, the collation LDML files of the languages, their main/ files
 * reduced to the calendar names, their rbnf/ files, and the supplemental
 * eras and ordinal plural rules, compiled into the program by
 * apps/sxml/embed-cldr-data.cmake as string literals whose concatenation
 * is each file's text. The library itself has no copy (D184, D189); the
 * resolver of query_command.c joins the chunks of a file when a feature
 * asks for it.
 */
#include <stddef.h>

typedef struct sxml_cli_text_chunks {
    const char *const *chunks;
    size_t chunk_count;
} sxml_cli_text_chunks;

/* One embedded file: its path under CLDR's common/ directory
   ("uca/allkeys_CLDR.txt", "collation/de.xml", "main/de.xml") and its
   text. */
typedef struct sxml_cli_cldr_file {
    const char *path;
    sxml_cli_text_chunks text;
} sxml_cli_cldr_file;

typedef struct sxml_cli_cldr_file_view {
    const sxml_cli_cldr_file *files;
    size_t file_count;
} sxml_cli_cldr_file_view;

/* The embedded files, static. */
sxml_cli_cldr_file_view sxml_cli_cldr_data(void);
