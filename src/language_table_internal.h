#pragma once

/*
 * The localization feature (glossary `language_table`, `localization`,
 * `spell_out_rule_set`, `plural_rule`; packet 7.17, D221): the optional
 * feature sxml_localization_feature (include/sxml/processor.h) that gives
 * the formatting functions the names and words of languages other than
 * English and calendars other than AD and ISO, from CLDR files the host
 * serves (src/cldr_internal.h). Internal to sxml; defined in
 * src/language_table.c and src/spell_out.c.
 *
 * The data, asked for as SXML_RESOURCE_KIND_LOCALE_DATA under
 * "urn:sxml:locale-data:" and a path below CLDR's common/ directory:
 * - main/<locale>.xml along the language's locale chain: month, day,
 *   era and day period names (UTS #35, Part 4, section 2, "Calendar
 *   Elements"), read from ldml/dates/calendars;
 * - rbnf/<locale>.xml: the spell-out rules (UTS #35, Part 3, section 6,
 *   "Rule-Based Number Formatting"), from the most specific served file
 *   of the chain other than root;
 * - supplemental/supplementalData.xml: the eras' start dates
 *   (supplementalData/calendarData; UTS #35, Part 4, section 3.1);
 * - supplemental/ordinals.xml: the ordinal plural rules the spell-out
 *   rules' "$(ordinal,...)$" choose by (UTS #35, Part 3, section 5).
 *
 * A language table (glossary `language_table`) is built at the first use
 * of a language tag and kept in the processor's feature state with the
 * answer "no data" too, for up to SXML_LANGUAGE_MAXIMUM_TABLE_COUNT tags,
 * so a query cannot grow the state; a tag beyond them has no table.
 *
 * The D162 places: the $language and $calendar arguments of the date
 * formatting functions (src/function_library_format_date.c), the
 * $language of fn:format-integer and the lang of xsl:number
 * (src/function_library_format.c). Each asks
 * sxml_processor_find_optional_feature for
 * SXML_OPTIONAL_FEATURE_KIND_LOCALIZATION and keeps sxml's English and
 * the fallback markers without it.
 *
 * Nothing here recurses (D71): spell-out rules are run with an explicit
 * stack of frames. Thread safety: the processor's thread (D35).
 */

#include "format_date_internal.h"
#include "format_internal.h"

#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The language tags a processor keeps tables for. */
#define SXML_LANGUAGE_MAXIMUM_TABLE_COUNT 64U

/* The URI prefix of the localization feature's files. */
#define SXML_LOCALE_DATA_URI_PREFIX "urn:sxml:locale-data:"

/* ------------------------------------------------------------- Names */

/* A name with its abbreviations; views borrow the language table. */
typedef struct sxml_date_name {
    /* The wide (full) name; empty when CLDR has none. */
    sxml_byte_view name;
    /* The abbreviated names of the format and stand-alone contexts;
       empty when absent. */
    sxml_byte_view abbreviations[2];
} sxml_date_name;

/* An era of a calendar (UTS #35, Part 4, section 3.1: calendarData's
   eras). */
typedef struct sxml_calendar_era {
    /* The era has only an end date and counts its years backwards from
       it (BC, before 1 AD). */
    bool counts_backwards;
    /* The start date, or the end date of a backward era, in the
       calendar's own reckoning (the Gregorian one for AD, BE and JE). */
    int64_t year;
    int64_t month;
    int64_t day;
    sxml_date_name name;
} sxml_calendar_era;

/* The names of one calendar in one language. */
typedef struct sxml_calendar_names {
    sxml_date_name months[12];
    /* Monday first, as ISO 8601 numbers days. */
    sxml_date_name days[7];
    /* am, pm. */
    sxml_date_name day_periods[2];
    /* AD and OS: BC, AD by era number; AH: AH; BE and JE: the eras in
       order of their start dates, JE after the Gregorian eras it inherits
       (calendarData's inheritEras). */
    const sxml_calendar_era *eras;
    size_t era_count;
    /* The eras have their start dates (supplementalData was served):
       BE and JE need them. */
    bool has_era_dates;
} sxml_calendar_names;

/* --------------------------------------------------------- Operations */

/* The names a date formatting function asks for. Views are borrowed for
   the call. */
typedef struct sxml_date_names_request {
    sxml_processor *processor;
    /* An xs:language value. */
    sxml_byte_view language;
    sxml_calendar_kind calendar_kind;
} sxml_date_names_request;

typedef struct sxml_date_names_outcome {
    /* NULL when the language has no CLDR data (no file of its locale
       chain other than root is served) or the calendar's names or eras
       are missing; else kept in the processor until it is finalized. */
    const sxml_calendar_names *names;
} sxml_date_names_outcome;

/* The words fn:format-integer or xsl:number ask for. */
typedef struct sxml_number_words_query {
    sxml_processor *processor;
    sxml_byte_view language;
} sxml_number_words_query;

/* The operations of the localization feature. */
typedef struct sxml_localization_operations {
    /* Finds the names of request's language and calendar. Returns the
       failures of the resolver and the allocator. */
    sxml_status (*find_date_names)(const sxml_date_names_request *request,
                                   sxml_date_names_outcome *outcome);
    /* Finds the spell-out rules of query's language as number words for
       sxml_integer_formatting: operations NULL when it has none. Returns
       the failures of the resolver and the allocator. */
    sxml_status (*find_number_words)(const sxml_number_words_query *query,
                                     sxml_number_words *words);
} sxml_localization_operations;

/* ------------------------------------------------- Spell-out rules */

/* The spell-out rules of one language (glossary `spell_out_rule_set`),
   defined in src/spell_out.c. */
typedef struct sxml_spell_out_rules sxml_spell_out_rules;

struct sxml_cldr_file;

/* The rules to read: an rbnf file and the ordinal plural rules. Both
   files must outlive the rules, which borrow their text. */
typedef struct sxml_spell_out_source {
    sxml_allocator allocator;
    /* rbnf/<locale>.xml read with "rbnf" kept. */
    const struct sxml_cldr_file *rules_file;
    /* supplemental/ordinals.xml read with "plurals" kept, or NULL. */
    const struct sxml_cldr_file *plurals_file;
    /* The locale whose plural rules apply ("de", "fr_CA"): the rules of
       the first pluralRules whose locales list it, else its language. */
    sxml_byte_view locale_name;
} sxml_spell_out_source;

/*
 * Reads the rule sets of the SpelloutRules and OrdinalRules groupings of
 * source->rules_file into *rules (allocated; release with
 * sxml_spell_out_rules_release). Rules whose base is not a number (the
 * negative, fraction, infinity and NaN rules) are left out: sxml formats
 * integers and writes the sign itself (F&O 4.0, section 4.8.1). *rules is
 * NULL when no rule set has rules. Returns the allocator's failures.
 */
SXML_NODISCARD sxml_status sxml_spell_out_rules_read(const sxml_spell_out_source *source,
                                                     sxml_spell_out_rules **rules);

void sxml_spell_out_rules_release(sxml_spell_out_rules *rules, sxml_allocator allocator);

/* The number words operations over rules, whose state is the rules. */
extern const sxml_number_words_operations sxml_spell_out_number_words_operations;

/* The localization feature (src/language_table.c). */
extern const sxml_optional_feature_definition sxml_localization_feature_definition;
