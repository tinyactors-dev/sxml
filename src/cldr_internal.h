#pragma once

/*
 * CLDR data (glossary `cldr_file`, `distinguished_path`, `locale_chain`;
 * packet 7.17, D221): the one mechanism by which optional features read
 * the Unicode Common Locale Data Repository as feature data (D189).
 * Internal to sxml; defined in src/cldr.c. Its users are the UCA
 * collation feature (src/collation_tailoring.c: common/collation/) and
 * the localization feature (src/language_table.c: common/main/,
 * common/rbnf/ and common/supplemental/).
 *
 * The data. A host serves CLDR's files as CLDR publishes them, under
 * "urn:sxml:" + a kind's name + ":" + the path below CLDR's common/
 * directory ("urn:sxml:collation-data:collation/de.xml",
 * "urn:sxml:locale-data:main/fr_CA.xml"; D220, D221), so a resolver
 * maps the URNs onto a CLDR checkout. sxml builds the paths from ASCII
 * letters, digits, "_" and "/" only.
 *
 * Language tags and locales (UTS #35, Part 1, sections 3 and 4.1):
 * a tag ("de-AT", "fr_CA", "de-u-co-phonebk") is read without case, "-"
 * and "_" alike; its locale chain is the CLDR file names of the tag
 * truncated subtag by subtag and then root ("de_AT", "de", "root"), the
 * inheritance by truncation CLDR's files follow (parentLocales of the
 * supplemental data are not read).
 *
 * Reading a file. An LDML document (UTS #35, Part 1, section 5) is read
 * into a table of its leaf elements below the root element: for each,
 * its distinguished path (the element names from the root's child down,
 * each with its attributes other than draft and references, as
 * "calendar[@type='gregorian']") and its text. Only the elements whose
 * paths start with one of the caller's prefixes are kept. An alias
 * element (section 4.1.3, "Alias Elements") is kept as an alias of its
 * parent's path.
 *
 * Finding a value (section 4.1.3, "Inheritance and Validity"): the value
 * at a path in a locale chain is the one of the most specific file that
 * has the path with a value other than the inheritance marker
 * "↑↑↑"; when no file has it, the longest prefix of the path that has
 * an alias (in the most specific file that has one) is replaced by the
 * alias's target, relative to the prefix, and the search starts again.
 *
 * Nothing here recurses (D71); explicit loops and a fixed stack of
 * element depths. Thread safety: the processor's thread (D35).
 */

#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------- Language tags */

/* The most subtags of a language tag read, and so the longest locale
   chain without root. */
#define SXML_CLDR_MAXIMUM_SUBTAG_COUNT 8U
/* The longest locale file name ("de_AT", "zh_Hant_TW"). */
#define SXML_CLDR_MAXIMUM_LOCALE_BYTE_COUNT 32U
/* The longest language tag read. */
#define SXML_CLDR_MAXIMUM_TAG_BYTE_COUNT 64U

/* A language tag read (BCP 47, as UTS #35, Part 1, section 3 uses it). Its
   views borrow the lower-case text it was read from. */
typedef struct sxml_cldr_language_tag {
    /* The subtags before any extension, lower case. */
    sxml_byte_view subtags[SXML_CLDR_MAXIMUM_SUBTAG_COUNT];
    size_t subtag_count;
    /* The value of the "co" keyword of the "-u-" extension; empty when
       absent. */
    sxml_byte_view collation_type;
    /* The tag has an extension or a keyword other than "-u-co-", which
       sxml does not read. */
    bool has_other_extension;
    bool is_valid;
} sxml_cldr_language_tag;

/* A tag's text in lower case, with "_" for "-" kept as it is. */
typedef struct sxml_cldr_tag_text {
    uint8_t bytes[SXML_CLDR_MAXIMUM_TAG_BYTE_COUNT];
    size_t byte_count;
} sxml_cldr_tag_text;

/* Writes text in ASCII lower case to *lowered; false when it is longer
   than SXML_CLDR_MAXIMUM_TAG_BYTE_COUNT. */
bool sxml_cldr_lower_tag(sxml_byte_view text, sxml_cldr_tag_text *lowered);

/*
 * Reads lower_case_text into *tag: subtags of 1 to 8 letters or digits
 * separated by "-" or "_", a first subtag of two or more, and after a
 * "u" singleton the keywords of the Unicode extension. is_valid is false
 * for an empty subtag, one longer than 8, an extension after no subtag,
 * more than SXML_CLDR_MAXIMUM_SUBTAG_COUNT subtags, and a "co" keyword
 * without a value.
 */
void sxml_cldr_read_language_tag(sxml_byte_view lower_case_text, sxml_cldr_language_tag *tag);

/* A CLDR locale file name, ASCII, without ".xml". */
typedef struct sxml_cldr_locale_name {
    uint8_t bytes[SXML_CLDR_MAXIMUM_LOCALE_BYTE_COUNT];
    size_t byte_count;
} sxml_cldr_locale_name;

/* The locale chain of a tag (glossary `locale_chain`): its file names
   from the most specific to root. */
typedef struct sxml_cldr_locale_chain {
    sxml_cldr_locale_name names[SXML_CLDR_MAXIMUM_SUBTAG_COUNT + 1U];
    size_t name_count;
} sxml_cldr_locale_chain;

/*
 * Writes the locale chain of a valid tag: for each truncation of its
 * subtags, the language in lower case, a script in title case and a
 * region or variant in upper case joined by "_" ("de_AT", "zh_Hant"),
 * then "root". A tag "und" or "root" gives "root" alone. Returns false,
 * with an empty chain, for a subtag that is not ASCII letters and digits
 * or a name that does not fit.
 */
bool sxml_cldr_make_locale_chain(const sxml_cldr_language_tag *tag,
                                 sxml_cldr_locale_chain *chain);

/* True when name is "root". */
bool sxml_cldr_locale_is_root(const sxml_cldr_locale_name *name);

/* --------------------------------------------------------------- URIs */

/* The longest URI of a CLDR file. */
#define SXML_CLDR_MAXIMUM_URI_BYTE_COUNT 160U

typedef struct sxml_cldr_uri {
    uint8_t bytes[SXML_CLDR_MAXIMUM_URI_BYTE_COUNT];
    size_t byte_count;
} sxml_cldr_uri;

/* The parts of a CLDR file's URI: uri_prefix + directory + file_name +
   ".xml", as "urn:sxml:locale-data:" "main/" "de_AT". */
typedef struct sxml_cldr_uri_parts {
    sxml_byte_view uri_prefix;
    sxml_byte_view directory;
    sxml_byte_view file_name;
} sxml_cldr_uri_parts;

/* Joins the parts into *uri; false when they do not fit. */
bool sxml_cldr_make_uri(const sxml_cldr_uri_parts *parts, sxml_cldr_uri *uri);

/* ------------------------------------------------------------- Files */

/* One leaf element or alias of a file. */
typedef struct sxml_cldr_entry {
    /* Its distinguished path (the parent's path for an alias), borrowed
       from the file. */
    sxml_byte_view path;
    /* Its text; for an alias the path attribute, as
       "../monthWidth[@type='wide']". */
    sxml_byte_view value;
    bool is_alias;
} sxml_cldr_entry;

/*
 * A CLDR file read (glossary `cldr_file`). Zero-initialize with an
 * allocator, read with sxml_cldr_file_read, and release with
 * sxml_cldr_file_release on every path.
 */
typedef struct sxml_cldr_file {
    sxml_allocator allocator;
    /* The resolver served the file and it is well-formed XML. */
    bool is_served;
    /* In document order. */
    sxml_cldr_entry *entries;
    size_t entry_count;
    /* The entries' indexes ordered by path, for lookups. */
    size_t *sorted_indexes;
    /* The block the paths and values live in. */
    uint8_t *text;
} sxml_cldr_file;

/* The prefixes of the paths to keep. */
typedef struct sxml_cldr_path_prefix_view {
    const sxml_byte_view *prefixes;
    size_t prefix_count;
} sxml_cldr_path_prefix_view;

/* A file to read. Views are borrowed for the call. */
typedef struct sxml_cldr_file_request {
    sxml_processor *processor;
    /* A kind whose form is BYTES (include/sxml/processor.h). */
    sxml_resource_kind resource_kind;
    sxml_byte_view uri;
    /* Elements below one of these paths ("collations",
       "dates/calendars/calendar[@type='gregorian']/months") are kept;
       none means every element. */
    sxml_cldr_path_prefix_view kept_paths;
} sxml_cldr_file_request;

/*
 * Asks the processor's resolver for request->uri and reads it into *file,
 * whose allocator must be set. A file the resolver does not serve, or
 * that is not well-formed XML within the processor's limits, leaves
 * file->is_served false and no entries (the caller treats it as absent).
 * Returns the failures of the resolver and the allocator; *file then has
 * no entries.
 */
SXML_NODISCARD sxml_status sxml_cldr_file_read(const sxml_cldr_file_request *request,
                                               sxml_cldr_file *file);

/* Releases the entries of file and leaves it empty (its allocator
   stays). */
void sxml_cldr_file_release(sxml_cldr_file *file);

/* The entry of file with path that is not an alias, or NULL. */
const sxml_cldr_entry *sxml_cldr_file_find(const sxml_cldr_file *file, sxml_byte_view path);

/* ------------------------------------------------------------- Lookups */

/* The longest path a lookup follows. */
#define SXML_CLDR_MAXIMUM_PATH_BYTE_COUNT 480U

/* The files of a locale chain, most specific first; files not served
   count as empty. */
typedef struct sxml_cldr_file_chain {
    const sxml_cldr_file *files;
    size_t file_count;
} sxml_cldr_file_chain;

/* A value to find. */
typedef struct sxml_cldr_lookup {
    sxml_cldr_file_chain chain;
    sxml_byte_view path;
} sxml_cldr_lookup;

/*
 * Writes the value at lookup->path in the chain, with aliases followed
 * (see the comment at the top), to *value, borrowed from a file of the
 * chain; false when the chain has none, a path grows beyond
 * SXML_CLDR_MAXIMUM_PATH_BYTE_COUNT, or more than 16 aliases are
 * followed (a cycle).
 */
bool sxml_cldr_find_value(const sxml_cldr_lookup *lookup, sxml_byte_view *value);

/* ------------------------------------------------------- Path segments */

/* Writes to *attribute_value the value of the attribute attribute_name
   in the last segment of path ("ruleset[@type='x'][@access='private']"
   has type x); false when the segment has none. */
typedef struct sxml_cldr_attribute_query {
    sxml_byte_view path;
    sxml_byte_view attribute_name;
} sxml_cldr_attribute_query;

bool sxml_cldr_find_attribute(const sxml_cldr_attribute_query *query,
                              sxml_byte_view *attribute_value);

/* The path without its last segment ("a/b[@x='1']/c" gives "a/b[@x='1']");
   empty for a path of one segment. */
sxml_byte_view sxml_cldr_parent_path(sxml_byte_view path);
