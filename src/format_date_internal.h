#pragma once

/*
 * The date picture engine (packet 2.14, docs/DECISIONS.md D97, D119):
 * pictures of the date formatting functions fn:format-dateTime,
 * fn:format-date and fn:format-time (F&O 4.0, section 9.9.4, "The
 * date/time formatting functions"). Internal to sxml; used by
 * src/function_library_format_date.c and open to xsl:number's relatives
 * in XSLT (4.10).
 *
 * The engine extends the number picture engine of src/format_internal.h:
 * a variable marker's first presentation modifier is a primary format
 * token of fn:format-integer, parsed and formatted there. Pictures are
 * scanned by hand (D97). Only the calendars AD and ISO and the English
 * language are supported; other calendars and languages are formatted as
 * AD and English after a fallback marker (section 9.9.4.8).
 *
 * Text is UTF-8 and must be well-formed. Nothing here recurses (D71) or
 * keeps state between calls.
 *
 * Thread safety: every function is safe to call from any thread; a
 * sxml_format_text belongs to the thread of its allocator.
 */

#include "format_internal.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

#include <stdbool.h>

/* Which date formatting function formats: it decides which components a
   picture may name (section 9.9.4.1, FOFD1350). */
typedef enum sxml_date_value_kind {
    SXML_DATE_VALUE_KIND_DATE_TIME,
    SXML_DATE_VALUE_KIND_DATE,
    SXML_DATE_VALUE_KIND_TIME,
} sxml_date_value_kind;

/* The calendars sxml formats in (D97). */
typedef enum sxml_calendar_kind {
    /* Anno Domini: the Gregorian calendar without a year zero, with eras
       AD and BC. */
    SXML_CALENDAR_KIND_AD,
    /* ISO 8601: XSD 1.1 year numbers, era "-" for negative years. */
    SXML_CALENDAR_KIND_ISO,
} sxml_calendar_kind;

/* The calendar a $calendar argument chooses. */
typedef struct sxml_calendar_choice {
    sxml_calendar_kind calendar_kind;
    /* The calendar asked for is not supported: AD is used, and the
       fallback marker "[Calendar: AD]" comes first in the result. */
    bool needs_fallback_marker;
} sxml_calendar_choice;

/*
 * Chooses the calendar calendar_name names (section 9.9.4.8): an EQName,
 * "Q{uri}local", "prefix:local" or "local". A name in no namespace must be
 * a designator of the section's table: AD and ISO are used as they are,
 * the others fall back to AD with a marker; a name in a namespace falls
 * back to AD with a marker too. A prefix is not resolved (the dynamic
 * context has no statically known namespaces), so any prefix counts as a
 * namespace.
 *
 * Returns SXML_STATUS_MALFORMED_INPUT for a name that is not an EQName or
 * a designator sxml does not know (the caller raises FOFD1340); *choice
 * is then untouched.
 */
SXML_NODISCARD sxml_status sxml_calendar_choose(sxml_byte_view calendar_name,
                                                sxml_calendar_choice *choice);

/* True when language is English ("en", or "en-" and a subtag, in any
   case), the only language sxml formats in (D97). */
bool sxml_date_language_is_english(sxml_byte_view language);

/* Why a picture was rejected. */
typedef enum sxml_date_picture_error {
    SXML_DATE_PICTURE_ERROR_NONE,
    /* The syntax of the picture is not correct: FOFD1340. */
    SXML_DATE_PICTURE_ERROR_SYNTAX,
    /* A component the value lacks, such as the year of an xs:time:
       FOFD1350. */
    SXML_DATE_PICTURE_ERROR_UNAVAILABLE_COMPONENT,
} sxml_date_picture_error;

/* A date, time or dateTime to format by a picture. */
typedef struct sxml_date_formatting {
    /* Borrowed for the call. */
    sxml_byte_view picture_string;
    /* The components; those value_kind lacks are ignored. */
    sxml_date_time value;
    sxml_date_value_kind value_kind;
    sxml_calendar_choice calendar;
    /* The language asked for is not English: the fallback marker
       "[Language: en]" comes first in the result. */
    bool needs_language_marker;
} sxml_date_formatting;

/*
 * Appends the value formatted by the picture to text (F&O 4.0, sections
 * 9.9.4.1 to 9.9.4.7): literal text with "[[" and "]]" undoubled, and
 * each variable marker replaced by its component, numbered by the
 * fn:format-integer engine or named in English, fitted to the width
 * modifier. Checks the whole picture before writing anything: first its
 * syntax, then the components it names. Uses text's allocator for
 * scratch memory.
 *
 * Returns SXML_STATUS_MALFORMED_INPUT with *picture_error set when the
 * picture is rejected (text is then unchanged), and the allocator's
 * failures and SXML_STATUS_ARITHMETIC_OVERFLOW, after which text may hold
 * part of the result.
 */
SXML_NODISCARD sxml_status sxml_format_date_time(const sxml_date_formatting *formatting,
                                                 sxml_format_text *text,
                                                 sxml_date_picture_error *picture_error);
