/*
 * The localization feature (src/language_table_internal.h; glossary
 * `language_table`, `localization`; packet 7.17, D221): language tables
 * built from the CLDR files the host serves, kept per language tag in the
 * processor.
 *
 * Golden file: src/collation_tailoring.c (CLDR files read at first use
 * through the shared module, kept per tag with the negative answers, at
 * most 64), src/timezone.c (the feature's state and definition).
 *
 * Names (UTS #35, Part 4, section 2): for the CLDR calendars gregorian
 * (AD, ISO, OS), buddhist (BE), japanese (JE) and islamic-civil (AH),
 * the wide names of the format context, with the abbreviated names of
 * the format and stand-alone contexts as abbreviations: months, days,
 * day periods (am and pm; the abbreviated name, the narrow one as its
 * abbreviation) and eras (the abbreviated name, as "AD"; the narrow one
 * as its abbreviation). Values are looked up along the locale chain with
 * CLDR's aliases (root's buddhist months are the gregorian ones, its
 * abbreviated months the wide ones, and so on). A language has names when
 * a file of its chain other than root is served. The eras' dates come
 * from supplementalData's calendarData: an era with a start counts from
 * it ("start" 1989-1-8 is Heisei 1), one with only an end counts back
 * from it (BC); the japanese calendar inherits the gregorian eras before
 * its own.
 *
 * Words: the spell-out rules of the most specific rbnf file of the chain
 * other than root (src/spell_out.c), with the ordinal plural rules of
 * supplemental/ordinals.xml.
 *
 * Everything lives in the processor's allocator; the names are copied
 * into one block per table, so the files are released after reading.
 */
#include "language_table_internal.h"

#include "cldr_internal.h"
#include "memory_internal.h"
#include "optional_feature_internal.h"
#include "processor_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- State */

/* The CLDR calendars of the table, in this order. */
typedef enum table_calendar {
    SXML_TABLE_CALENDAR_GREGORIAN,
    SXML_TABLE_CALENDAR_BUDDHIST,
    SXML_TABLE_CALENDAR_JAPANESE,
    SXML_TABLE_CALENDAR_ISLAMIC,
    SXML_TABLE_CALENDAR_COUNT,
} table_calendar;

static const char *const calendar_types[SXML_TABLE_CALENDAR_COUNT] = {
    "gregorian", "buddhist", "japanese", "islamic-civil"};

/* One language tag's table. */
typedef struct language_entry {
    uint8_t key[SXML_CLDR_MAXIMUM_TAG_BYTE_COUNT];
    size_t key_byte_count;
    bool are_names_read;
    bool has_names;
    sxml_calendar_names calendars[SXML_TABLE_CALENDAR_COUNT];
    /* The eras of all calendars and the names' text, one block each. */
    sxml_calendar_era *eras;
    uint8_t *name_text;
    bool are_words_read;
    /* The rbnf file the rules borrow from, and the rules (NULL for
       none). */
    sxml_cldr_file rules_file;
    sxml_spell_out_rules *rules;
} language_entry;

typedef struct localization_state {
    language_entry *entries[SXML_LANGUAGE_MAXIMUM_TABLE_COUNT];
    size_t entry_count;
    /* supplementalData's calendarData, and ordinals.xml, each read once
       (is_served false when not served). */
    bool is_supplemental_read;
    sxml_cldr_file supplemental;
    bool are_ordinals_read;
    sxml_cldr_file ordinals;
} localization_state;

static void release_entry(language_entry *entry, sxml_allocator allocator) {
    sxml_allocator_release(allocator, entry->eras);
    sxml_allocator_release(allocator, entry->name_text);
    sxml_spell_out_rules_release(entry->rules, allocator);
    sxml_cldr_file_release(&entry->rules_file);
    sxml_allocator_release(allocator, entry);
}

static void finalize_state(void *state, sxml_allocator allocator) {
    localization_state *localization = state;
    for (size_t entry_index = 0; entry_index < localization->entry_count; entry_index += 1) {
        release_entry(localization->entries[entry_index], allocator);
    }
    sxml_cldr_file_release(&localization->supplemental);
    sxml_cldr_file_release(&localization->ordinals);
    sxml_allocator_release(allocator, localization);
}

static sxml_status get_state(sxml_processor *processor, localization_state **state) {
    *state = sxml_processor_get_optional_feature_state(processor,
                                                       SXML_OPTIONAL_FEATURE_KIND_LOCALIZATION);
    if (*state != NULL) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        processor->allocator,
        (sxml_allocation_request){.byte_count = sizeof(localization_state),
                                  .alignment = alignof(localization_state)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(memory, 0, sizeof(localization_state));
    localization_state *made = memory;
    made->supplemental.allocator = processor->allocator;
    made->ordinals.allocator = processor->allocator;
    status = sxml_processor_set_optional_feature_state(
        processor, SXML_OPTIONAL_FEATURE_KIND_LOCALIZATION, memory);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(processor->allocator, memory);
        return status;
    }
    *state = made;
    return SXML_STATUS_OK;
}

/* The entry of a language (lower case), added when absent; NULL when the
   tag is too long or the table is full. */
typedef struct entry_request {
    sxml_processor *processor;
    localization_state *state;
    sxml_byte_view language;
} entry_request;

static sxml_status find_entry(const entry_request *request, language_entry **found) {
    *found = NULL;
    sxml_cldr_tag_text key;
    if (!sxml_cldr_lower_tag(request->language, &key)) {
        return SXML_STATUS_OK;
    }
    localization_state *state = request->state;
    for (size_t entry_index = 0; entry_index < state->entry_count; entry_index += 1) {
        language_entry *entry = state->entries[entry_index];
        if (entry->key_byte_count == key.byte_count &&
            memcmp(entry->key, key.bytes, key.byte_count) == 0) {
            *found = entry;
            return SXML_STATUS_OK;
        }
    }
    if (state->entry_count == SXML_LANGUAGE_MAXIMUM_TABLE_COUNT) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    const sxml_allocator allocator = request->processor->allocator;
    const sxml_status status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = sizeof(language_entry),
                                  .alignment = alignof(language_entry)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(memory, 0, sizeof(language_entry));
    language_entry *entry = memory;
    memcpy(entry->key, key.bytes, key.byte_count);
    entry->key_byte_count = key.byte_count;
    entry->rules_file.allocator = allocator;
    state->entries[state->entry_count] = entry;
    state->entry_count += 1;
    *found = entry;
    return SXML_STATUS_OK;
}

/* The locale chain of an entry's tag; false for a tag that is no
   language tag. */
static bool entry_chain(const language_entry *entry, sxml_cldr_locale_chain *chain) {
    sxml_cldr_language_tag tag;
    sxml_cldr_read_language_tag(
        (sxml_byte_view){.bytes = entry->key, .byte_count = entry->key_byte_count}, &tag);
    return sxml_cldr_make_locale_chain(&tag, chain);
}

/* ------------------------------------------------------ Reading files */

/* A file of the chain: directory and locale name. */
typedef struct locale_file_request {
    sxml_processor *processor;
    sxml_byte_view directory;
    sxml_byte_view file_name;
    sxml_cldr_path_prefix_view kept_paths;
} locale_file_request;

static sxml_status read_locale_file(const locale_file_request *request, sxml_cldr_file *file) {
    sxml_cldr_uri uri;
    if (!sxml_cldr_make_uri(&(sxml_cldr_uri_parts){.uri_prefix = SXML_BYTE_VIEW_FROM_LITERAL(
                                                       SXML_LOCALE_DATA_URI_PREFIX),
                                                   .directory = request->directory,
                                                   .file_name = request->file_name},
                            &uri)) {
        return SXML_STATUS_OK;
    }
    return sxml_cldr_file_read(
        &(sxml_cldr_file_request){.processor = request->processor,
                                  .resource_kind = SXML_RESOURCE_KIND_LOCALE_DATA,
                                  .uri = {.bytes = uri.bytes, .byte_count = uri.byte_count},
                                  .kept_paths = request->kept_paths},
        file);
}

static sxml_byte_view name_view(const sxml_cldr_locale_name *name) {
    return (sxml_byte_view){.bytes = name->bytes, .byte_count = name->byte_count};
}

/* The paths of main/ files the names need. */
#define SXML_TABLE_KEPT_PATH(type, part) \
    SXML_BYTE_VIEW_INITIALIZER("dates/calendars/calendar[@type='" type "']/" part)
#define SXML_TABLE_KEPT_CALENDAR(type)                                                    \
    SXML_TABLE_KEPT_PATH(type, "months"), SXML_TABLE_KEPT_PATH(type, "days"),           \
        SXML_TABLE_KEPT_PATH(type, "dayPeriods"), SXML_TABLE_KEPT_PATH(type, "eras")

static const sxml_byte_view name_paths[] = {
    SXML_TABLE_KEPT_CALENDAR("gregorian"), SXML_TABLE_KEPT_CALENDAR("buddhist"),
    SXML_TABLE_KEPT_CALENDAR("japanese"), SXML_TABLE_KEPT_CALENDAR("islamic-civil"),
    SXML_TABLE_KEPT_CALENDAR("islamic"),
};

static const sxml_byte_view supplemental_paths[] = {
    SXML_BYTE_VIEW_INITIALIZER("calendarData/calendar[@type='gregorian']"),
    SXML_BYTE_VIEW_INITIALIZER("calendarData/calendar[@type='buddhist']"),
    SXML_BYTE_VIEW_INITIALIZER("calendarData/calendar[@type='japanese']"),
};

static sxml_status read_supplemental(sxml_processor *processor, localization_state *state) {
    if (state->is_supplemental_read) {
        return SXML_STATUS_OK;
    }
    const sxml_status status = read_locale_file(
        &(locale_file_request){
            .processor = processor,
            .directory = SXML_BYTE_VIEW_FROM_LITERAL("supplemental/"),
            .file_name = SXML_BYTE_VIEW_FROM_LITERAL("supplementalData"),
            .kept_paths = {.prefixes = supplemental_paths,
                           .prefix_count = sizeof(supplemental_paths) / sizeof(supplemental_paths[0])}},
        &state->supplemental);
    state->is_supplemental_read = status == SXML_STATUS_OK;
    return status;
}

/* --------------------------------------------------------- Names */

/* A path of the names: "dates/calendars/calendar[@type='" + calendar +
   "']/" + part. */
typedef struct name_path {
    uint8_t bytes[SXML_CLDR_MAXIMUM_PATH_BYTE_COUNT];
    size_t byte_count;
} name_path;

typedef struct name_path_parts {
    const char *calendar_type;
    const char *middle;
    const char *width;
    const char *element;
    sxml_byte_view element_type;
} name_path_parts;

static void make_name_path(const name_path_parts *parts, name_path *path) {
    const sxml_byte_view pieces[9] = {
        SXML_BYTE_VIEW_FROM_LITERAL("dates/calendars/calendar[@type='"),
        sxml_byte_view_from_c_string(parts->calendar_type),
        SXML_BYTE_VIEW_FROM_LITERAL("']/"),
        sxml_byte_view_from_c_string(parts->middle),
        sxml_byte_view_from_c_string(parts->width),
        SXML_BYTE_VIEW_FROM_LITERAL("']/"),
        sxml_byte_view_from_c_string(parts->element),
        parts->element_type,
        SXML_BYTE_VIEW_FROM_LITERAL("']")};
    path->byte_count = 0;
    for (size_t piece_index = 0; piece_index < 9; piece_index += 1) {
        const sxml_byte_view piece = pieces[piece_index];
        if (path->byte_count + piece.byte_count > sizeof(path->bytes)) {
            path->byte_count = 0;
            return;
        }
        if (piece.byte_count > 0) {
            memcpy(path->bytes + path->byte_count, piece.bytes, piece.byte_count);
        }
        path->byte_count += piece.byte_count;
    }
}

/* A kind of name: where its widths are. */
typedef struct name_kind {
    /* "months/monthContext[@type='format']/monthWidth[@type='" and so
       on, up to the width. */
    const char *format_middle;
    const char *stand_alone_middle;
    /* The widths of the name and its abbreviations. */
    const char *name_width;
    const char *first_abbreviation_width;
    const char *element;
} name_kind;

static const name_kind month_kind = {
    .format_middle = "months/monthContext[@type='format']/monthWidth[@type='",
    .stand_alone_middle = "months/monthContext[@type='stand-alone']/monthWidth[@type='",
    .name_width = "wide",
    .first_abbreviation_width = "abbreviated",
    .element = "month[@type='"};

static const name_kind day_kind = {
    .format_middle = "days/dayContext[@type='format']/dayWidth[@type='",
    .stand_alone_middle = "days/dayContext[@type='stand-alone']/dayWidth[@type='",
    .name_width = "wide",
    .first_abbreviation_width = "abbreviated",
    .element = "day[@type='"};

static const name_kind day_period_kind = {
    .format_middle = "dayPeriods/dayPeriodContext[@type='format']/dayPeriodWidth[@type='",
    .stand_alone_middle = "dayPeriods/dayPeriodContext[@type='stand-alone']/dayPeriodWidth[@type='",
    .name_width = "abbreviated",
    .first_abbreviation_width = "narrow",
    .element = "dayPeriod[@type='"};

/* Looks up one name and its abbreviations. */
typedef struct name_lookup {
    sxml_cldr_file_chain chain;
    const char *calendar_type;
    const name_kind *kind;
    sxml_byte_view element_type;
} name_lookup;

static sxml_byte_view find_name(const name_lookup *lookup, const char *middle,
                                const char *width) {
    name_path path;
    make_name_path(&(name_path_parts){.calendar_type = lookup->calendar_type,
                                      .middle = middle,
                                      .width = width,
                                      .element = lookup->kind->element,
                                      .element_type = lookup->element_type},
                   &path);
    sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
    if (path.byte_count == 0 ||
        !sxml_cldr_find_value(
            &(sxml_cldr_lookup){.chain = lookup->chain,
                                .path = {.bytes = path.bytes, .byte_count = path.byte_count}},
            &value)) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return value;
}

static sxml_date_name lookup_name(const name_lookup *lookup) {
    const name_kind *kind = lookup->kind;
    return (sxml_date_name){
        .name = find_name(lookup, kind->format_middle, kind->name_width),
        .abbreviations = {find_name(lookup, kind->format_middle, kind->first_abbreviation_width),
                          kind == &day_period_kind
                              ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                              : find_name(lookup, kind->stand_alone_middle,
                                          kind->first_abbreviation_width)}};
}

/* The era names of a calendar: eraAbbr, with eraNarrow as abbreviation
   (root aliases eraNames to eraAbbr). */
static sxml_date_name lookup_era(sxml_cldr_file_chain chain, const char *calendar_type,
                                 sxml_byte_view era_type) {
    sxml_date_name name = {.name = {.bytes = NULL, .byte_count = 0}};
    const char *const widths[2] = {"eras/eraAbbr", "eras/eraNarrow"};
    for (size_t width_index = 0; width_index < 2; width_index += 1) {
        const sxml_byte_view pieces[7] = {
            SXML_BYTE_VIEW_FROM_LITERAL("dates/calendars/calendar[@type='"),
            sxml_byte_view_from_c_string(calendar_type),
            SXML_BYTE_VIEW_FROM_LITERAL("']/"),
            sxml_byte_view_from_c_string(widths[width_index]),
            SXML_BYTE_VIEW_FROM_LITERAL("/era[@type='"),
            era_type,
            SXML_BYTE_VIEW_FROM_LITERAL("']")};
        name_path path = {.byte_count = 0};
        bool fits = true;
        for (size_t piece_index = 0; fits && piece_index < 7; piece_index += 1) {
            fits = path.byte_count + pieces[piece_index].byte_count <= sizeof(path.bytes);
            if (fits && pieces[piece_index].byte_count > 0) {
                memcpy(path.bytes + path.byte_count, pieces[piece_index].bytes,
                       pieces[piece_index].byte_count);
                path.byte_count += pieces[piece_index].byte_count;
            }
        }
        sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
        if (fits &&
            sxml_cldr_find_value(
                &(sxml_cldr_lookup){.chain = chain,
                                    .path = {.bytes = path.bytes, .byte_count = path.byte_count}},
                &value)) {
            if (width_index == 0) {
                name.name = value;
            } else {
                name.abbreviations[0] = value;
            }
        }
    }
    return name;
}

/* ---------------------------------------------------------- Eras */

/* Reads "1989-1-8" or "-542-01-01". */
typedef struct era_date {
    int64_t year;
    int64_t month;
    int64_t day;
} era_date;

static bool read_era_date(sxml_byte_view text, era_date *date) {
    int64_t parts[3] = {0, 0, 0};
    size_t part_index = 0;
    size_t position = 0;
    bool is_negative = false;
    if (text.byte_count > 0 && text.bytes[0] == '-') {
        is_negative = true;
        position = 1;
    }
    bool has_digit = false;
    for (; position < text.byte_count; position += 1) {
        const uint8_t byte = text.bytes[position];
        if (byte == '-') {
            if (!has_digit || part_index == 2) {
                return false;
            }
            part_index += 1;
            has_digit = false;
            continue;
        }
        if (byte < '0' || byte > '9' || parts[part_index] > 100000000) {
            return false;
        }
        parts[part_index] = parts[part_index] * 10 + (byte - '0');
        has_digit = true;
    }
    if (part_index != 2 || !has_digit) {
        return false;
    }
    *date = (era_date){.year = is_negative ? -parts[0] : parts[0], .month = parts[1], .day = parts[2]};
    return true;
}

/* The number in an era type ("236"); false for none. */
static bool read_era_number(sxml_byte_view text, size_t *number) {
    size_t value = 0;
    if (text.byte_count == 0 || text.byte_count > 6) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        if (text.bytes[byte_index] < '0' || text.bytes[byte_index] > '9') {
            return false;
        }
        value = value * 10U + (size_t)(text.bytes[byte_index] - '0');
    }
    *number = value;
    return true;
}

/* The eras being collected for all calendars. */
typedef struct era_list {
    sxml_calendar_era *eras;
    size_t era_count;
    size_t era_capacity;
    sxml_allocator allocator;
} era_list;

static sxml_status add_era(era_list *list, sxml_calendar_era era) {
    if (list->era_count == list->era_capacity) {
        const size_t capacity = list->era_capacity < 16 ? 16 : list->era_capacity * 2U;
        void *memory = NULL;
        const sxml_status status = sxml_allocator_allocate_array(
            list->allocator,
            (sxml_array_allocation_request){.element_count = capacity,
                                            .element_size = sizeof(sxml_calendar_era),
                                            .element_alignment = alignof(sxml_calendar_era)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (list->era_count > 0) {
            memcpy(memory, list->eras, list->era_count * sizeof(sxml_calendar_era));
        }
        sxml_allocator_release(list->allocator, list->eras);
        list->eras = memory;
        list->era_capacity = capacity;
    }
    list->eras[list->era_count] = era;
    list->era_count += 1;
    return SXML_STATUS_OK;
}

/* Adds the eras of one calendar: by supplementalData's dates when it was
   served (in document order, which is the order of their dates), else
   the eras numbered 0 to era_count - 1 without dates. */
typedef struct era_source {
    sxml_cldr_file_chain chain;
    const sxml_cldr_file *supplemental;
    const char *calendar_type;
    /* Without dates. */
    size_t era_count;
} era_source;

static sxml_status add_calendar_eras(const era_source *source, era_list *list) {
    sxml_status status = SXML_STATUS_OK;
    if (source->supplemental == NULL || !source->supplemental->is_served) {
        for (size_t era_number = 0; status == SXML_STATUS_OK && era_number < source->era_count;
             era_number += 1) {
            uint8_t digits[4] = {(uint8_t)('0' + era_number % 10U)};
            status = add_era(list, (sxml_calendar_era){
                                       .name = lookup_era(source->chain, source->calendar_type,
                                                          (sxml_byte_view){.bytes = digits,
                                                                           .byte_count = 1})});
        }
        return status;
    }
    uint8_t parent_bytes[96];
    const sxml_byte_view head = SXML_BYTE_VIEW_FROM_LITERAL("calendarData/calendar[@type='");
    const sxml_byte_view type = sxml_byte_view_from_c_string(source->calendar_type);
    const sxml_byte_view tail = SXML_BYTE_VIEW_FROM_LITERAL("']/eras");
    memcpy(parent_bytes, head.bytes, head.byte_count);
    memcpy(parent_bytes + head.byte_count, type.bytes, type.byte_count);
    memcpy(parent_bytes + head.byte_count + type.byte_count, tail.bytes, tail.byte_count);
    const sxml_byte_view parent = {.bytes = parent_bytes,
                                   .byte_count = head.byte_count + type.byte_count + tail.byte_count};
    const sxml_cldr_file *file = source->supplemental;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < file->entry_count;
         entry_index += 1) {
        const sxml_cldr_entry *entry = &file->entries[entry_index];
        if (entry->is_alias || !sxml_byte_view_equals(sxml_cldr_parent_path(entry->path), parent)) {
            continue;
        }
        sxml_byte_view era_type = {.bytes = NULL, .byte_count = 0};
        sxml_byte_view start = {.bytes = NULL, .byte_count = 0};
        sxml_byte_view end = {.bytes = NULL, .byte_count = 0};
        size_t era_number = 0;
        if (!sxml_cldr_find_attribute(
                &(sxml_cldr_attribute_query){.path = entry->path,
                                             .attribute_name = SXML_BYTE_VIEW_FROM_LITERAL("type")},
                &era_type) ||
            !read_era_number(era_type, &era_number)) {
            continue;
        }
        const bool has_start = sxml_cldr_find_attribute(
            &(sxml_cldr_attribute_query){.path = entry->path,
                                         .attribute_name = SXML_BYTE_VIEW_FROM_LITERAL("start")},
            &start);
        const bool has_end = sxml_cldr_find_attribute(
            &(sxml_cldr_attribute_query){.path = entry->path,
                                         .attribute_name = SXML_BYTE_VIEW_FROM_LITERAL("end")},
            &end);
        era_date date;
        if (!(has_start ? read_era_date(start, &date) : has_end && read_era_date(end, &date))) {
            continue;
        }
        status = add_era(list, (sxml_calendar_era){
                                   .counts_backwards = !has_start,
                                   .year = date.year,
                                   .month = date.month,
                                   .day = date.day,
                                   .name = lookup_era(source->chain, source->calendar_type, era_type)});
    }
    return status;
}

/* ------------------------------------------------------- Building */

static sxml_status read_calendar_names(sxml_cldr_file_chain chain, table_calendar calendar,
                                       sxml_calendar_names *names) {
    static const char *const day_types[7] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
    static const char *const day_period_types[2] = {"am", "pm"};
    const char *calendar_type = calendar_types[calendar];
    for (size_t month_index = 0; month_index < 12; month_index += 1) {
        uint8_t digits[2] = {(uint8_t)('0' + (month_index + 1) / 10U),
                             (uint8_t)('0' + (month_index + 1) % 10U)};
        const sxml_byte_view month_type = month_index + 1 < 10
                                              ? (sxml_byte_view){.bytes = digits + 1, .byte_count = 1}
                                              : (sxml_byte_view){.bytes = digits, .byte_count = 2};
        names->months[month_index] = lookup_name(&(name_lookup){
            .chain = chain, .calendar_type = calendar_type, .kind = &month_kind, .element_type = month_type});
    }
    for (size_t day_index = 0; day_index < 7; day_index += 1) {
        names->days[day_index] = lookup_name(
            &(name_lookup){.chain = chain,
                           .calendar_type = calendar_type,
                           .kind = &day_kind,
                           .element_type = sxml_byte_view_from_c_string(day_types[day_index])});
    }
    for (size_t period_index = 0; period_index < 2; period_index += 1) {
        names->day_periods[period_index] = lookup_name(&(name_lookup){
            .chain = chain,
            .calendar_type = calendar_type,
            .kind = &day_period_kind,
            .element_type = sxml_byte_view_from_c_string(day_period_types[period_index])});
    }
    return SXML_STATUS_OK;
}

/* The bytes of every name, to copy into one block. */
static size_t name_byte_count(const sxml_date_name *name) {
    return name->name.byte_count + name->abbreviations[0].byte_count +
           name->abbreviations[1].byte_count;
}

/* Copies a name's views into the block at *offset. */
typedef struct name_copy {
    uint8_t *block;
    size_t offset;
} name_copy;

static void copy_view(name_copy *copy, sxml_byte_view *view) {
    if (view->byte_count == 0) {
        *view = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        return;
    }
    memcpy(copy->block + copy->offset, view->bytes, view->byte_count);
    view->bytes = copy->block + copy->offset;
    copy->offset += view->byte_count;
}

static void copy_name(name_copy *copy, sxml_date_name *name) {
    copy_view(copy, &name->name);
    copy_view(copy, &name->abbreviations[0]);
    copy_view(copy, &name->abbreviations[1]);
}

/* Copies every name of the entry and its eras into its blocks. */
typedef struct names_owner {
    sxml_allocator allocator;
    language_entry *entry;
    era_list *eras;
} names_owner;

static sxml_status keep_names(const names_owner *owner) {
    language_entry *entry = owner->entry;
    size_t byte_count = 0;
    for (size_t calendar = 0; calendar < SXML_TABLE_CALENDAR_COUNT; calendar += 1) {
        const sxml_calendar_names *names = &entry->calendars[calendar];
        for (size_t index = 0; index < 12; index += 1) {
            byte_count += name_byte_count(&names->months[index]);
        }
        for (size_t index = 0; index < 7; index += 1) {
            byte_count += name_byte_count(&names->days[index]);
        }
        byte_count += name_byte_count(&names->day_periods[0]) +
                      name_byte_count(&names->day_periods[1]);
    }
    for (size_t era_index = 0; era_index < owner->eras->era_count; era_index += 1) {
        byte_count += name_byte_count(&owner->eras->eras[era_index].name);
    }
    void *block = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        owner->allocator,
        (sxml_array_allocation_request){
            .element_count = byte_count, .element_size = 1, .element_alignment = 1},
        &block);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    name_copy copy = {.block = block, .offset = 0};
    for (size_t calendar = 0; calendar < SXML_TABLE_CALENDAR_COUNT; calendar += 1) {
        sxml_calendar_names *names = &entry->calendars[calendar];
        for (size_t index = 0; index < 12; index += 1) {
            copy_name(&copy, &names->months[index]);
        }
        for (size_t index = 0; index < 7; index += 1) {
            copy_name(&copy, &names->days[index]);
        }
        copy_name(&copy, &names->day_periods[0]);
        copy_name(&copy, &names->day_periods[1]);
    }
    for (size_t era_index = 0; era_index < owner->eras->era_count; era_index += 1) {
        copy_name(&copy, &owner->eras->eras[era_index].name);
    }
    entry->name_text = block;
    entry->eras = owner->eras->eras;
    owner->eras->eras = NULL;
    return SXML_STATUS_OK;
}

/* The files of an entry's chain, read with the names' paths. */
typedef struct chain_files {
    sxml_cldr_file files[SXML_CLDR_MAXIMUM_SUBTAG_COUNT + 1U];
    size_t file_count;
    bool has_language_file;
} chain_files;

static sxml_status read_chain_files(sxml_processor *processor, const sxml_cldr_locale_chain *chain,
                                    chain_files *files) {
    sxml_status status = SXML_STATUS_OK;
    files->file_count = 0;
    files->has_language_file = false;
    for (size_t name_index = 0; status == SXML_STATUS_OK && name_index < chain->name_count;
         name_index += 1) {
        sxml_cldr_file *file = &files->files[files->file_count];
        *file = (sxml_cldr_file){.allocator = processor->allocator};
        files->file_count += 1;
        status = read_locale_file(
            &(locale_file_request){
                .processor = processor,
                .directory = SXML_BYTE_VIEW_FROM_LITERAL("main/"),
                .file_name = name_view(&chain->names[name_index]),
                .kept_paths = {.prefixes = name_paths,
                               .prefix_count = sizeof(name_paths) / sizeof(name_paths[0])}},
            file);
        if (status == SXML_STATUS_OK && file->is_served &&
            !sxml_cldr_locale_is_root(&chain->names[name_index])) {
            files->has_language_file = true;
        }
    }
    return status;
}

/* The era sets of each calendar: where they start in the list. */
typedef struct era_layout {
    size_t first[SXML_TABLE_CALENDAR_COUNT];
    size_t count[SXML_TABLE_CALENDAR_COUNT];
} era_layout;

static sxml_status collect_eras(const era_source *base, era_list *list, era_layout *layout) {
    sxml_status status = SXML_STATUS_OK;
    /* gregorian, buddhist; japanese repeats the gregorian eras first
       (calendarData's inheritEras); islamic-civil has its one era. */
    static const size_t undated_counts[SXML_TABLE_CALENDAR_COUNT] = {2, 1, 0, 1};
    for (size_t calendar = 0; status == SXML_STATUS_OK && calendar < SXML_TABLE_CALENDAR_COUNT;
         calendar += 1) {
        layout->first[calendar] = list->era_count;
        era_source source = *base;
        source.era_count = undated_counts[calendar];
        if (calendar == SXML_TABLE_CALENDAR_JAPANESE) {
            source.calendar_type = calendar_types[SXML_TABLE_CALENDAR_GREGORIAN];
            status = add_calendar_eras(&source, list);
        }
        source.calendar_type = calendar_types[calendar];
        if (calendar == SXML_TABLE_CALENDAR_ISLAMIC) {
            /* Dates of the tabular calendar are computed (src/calendar.c);
               its era is AH from 1 AH on. */
            source.supplemental = NULL;
        }
        if (status == SXML_STATUS_OK) {
            status = add_calendar_eras(&source, list);
        }
        layout->count[calendar] = list->era_count - layout->first[calendar];
    }
    return status;
}

static sxml_status build_names(sxml_processor *processor, localization_state *state,
                               language_entry *entry) {
    sxml_cldr_locale_chain chain;
    if (!entry_chain(entry, &chain)) {
        entry->are_names_read = true;
        return SXML_STATUS_OK;
    }
    chain_files files;
    sxml_status status = read_chain_files(processor, &chain, &files);
    if (status == SXML_STATUS_OK && files.has_language_file) {
        status = read_supplemental(processor, state);
    }
    era_list eras = {.allocator = processor->allocator};
    era_layout layout;
    memset(&layout, 0, sizeof(layout));
    const sxml_cldr_file_chain file_chain = {.files = files.files, .file_count = files.file_count};
    if (status == SXML_STATUS_OK && files.has_language_file) {
        for (size_t calendar = 0; status == SXML_STATUS_OK && calendar < SXML_TABLE_CALENDAR_COUNT;
             calendar += 1) {
            status = read_calendar_names(file_chain, (table_calendar)calendar,
                                         &entry->calendars[calendar]);
        }
        if (status == SXML_STATUS_OK) {
            status = collect_eras(&(era_source){.chain = file_chain,
                                                .supplemental = &state->supplemental,
                                                .calendar_type = NULL,
                                                .era_count = 0},
                                  &eras, &layout);
        }
        if (status == SXML_STATUS_OK) {
            status = keep_names(&(names_owner){
                .allocator = processor->allocator, .entry = entry, .eras = &eras});
        }
    }
    if (status == SXML_STATUS_OK) {
        entry->are_names_read = true;
        entry->has_names = files.has_language_file;
        for (size_t calendar = 0; entry->has_names && calendar < SXML_TABLE_CALENDAR_COUNT;
             calendar += 1) {
            sxml_calendar_names *names = &entry->calendars[calendar];
            names->eras = entry->eras + layout.first[calendar];
            names->era_count = layout.count[calendar];
            names->has_era_dates = state->supplemental.is_served;
        }
    } else {
        memset(entry->calendars, 0, sizeof(entry->calendars));
    }
    sxml_allocator_release(processor->allocator, eras.eras);
    for (size_t file_index = 0; file_index < files.file_count; file_index += 1) {
        sxml_cldr_file_release(&files.files[file_index]);
    }
    return status;
}

/* True when every month and day of the names has a name. */
static bool has_every_name(const sxml_calendar_names *names) {
    for (size_t index = 0; index < 12; index += 1) {
        if (names->months[index].name.byte_count == 0) {
            return false;
        }
    }
    for (size_t index = 0; index < 7; index += 1) {
        if (names->days[index].name.byte_count == 0) {
            return false;
        }
    }
    return true;
}

static sxml_status find_date_names(const sxml_date_names_request *request,
                                   sxml_date_names_outcome *outcome) {
    outcome->names = NULL;
    localization_state *state = NULL;
    sxml_status status = get_state(request->processor, &state);
    language_entry *entry = NULL;
    if (status == SXML_STATUS_OK) {
        status = find_entry(
            &(entry_request){.processor = request->processor, .state = state, .language = request->language},
            &entry);
    }
    if (status != SXML_STATUS_OK || entry == NULL) {
        return status;
    }
    if (!entry->are_names_read) {
        status = build_names(request->processor, state, entry);
    }
    if (status != SXML_STATUS_OK || !entry->has_names) {
        return status;
    }
    table_calendar calendar = SXML_TABLE_CALENDAR_GREGORIAN;
    switch (request->calendar_kind) {
    case SXML_CALENDAR_KIND_BE:
        calendar = SXML_TABLE_CALENDAR_BUDDHIST;
        break;
    case SXML_CALENDAR_KIND_JE:
        calendar = SXML_TABLE_CALENDAR_JAPANESE;
        break;
    case SXML_CALENDAR_KIND_AH:
        calendar = SXML_TABLE_CALENDAR_ISLAMIC;
        break;
    case SXML_CALENDAR_KIND_AD:
    case SXML_CALENDAR_KIND_ISO:
    case SXML_CALENDAR_KIND_OS:
        break;
    }
    const sxml_calendar_names *names = &entry->calendars[calendar];
    const bool needs_dates = request->calendar_kind == SXML_CALENDAR_KIND_BE ||
                             request->calendar_kind == SXML_CALENDAR_KIND_JE;
    if (has_every_name(names) && (!needs_dates || (names->has_era_dates && names->era_count > 0))) {
        outcome->names = names;
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Words */

static const sxml_byte_view rules_paths[] = {SXML_BYTE_VIEW_INITIALIZER("rbnf")};
static const sxml_byte_view ordinal_paths[] = {
    SXML_BYTE_VIEW_INITIALIZER("plurals[@type='ordinal']")};

static sxml_status read_ordinals(sxml_processor *processor, localization_state *state) {
    if (state->are_ordinals_read) {
        return SXML_STATUS_OK;
    }
    const sxml_status status = read_locale_file(
        &(locale_file_request){.processor = processor,
                               .directory = SXML_BYTE_VIEW_FROM_LITERAL("supplemental/"),
                               .file_name = SXML_BYTE_VIEW_FROM_LITERAL("ordinals"),
                               .kept_paths = {.prefixes = ordinal_paths, .prefix_count = 1}},
        &state->ordinals);
    state->are_ordinals_read = status == SXML_STATUS_OK;
    return status;
}

static sxml_status build_words(sxml_processor *processor, localization_state *state,
                               language_entry *entry) {
    sxml_cldr_locale_chain chain;
    if (!entry_chain(entry, &chain)) {
        entry->are_words_read = true;
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    size_t found_index = chain.name_count;
    /* The most specific rbnf file other than root. */
    for (size_t name_index = 0; status == SXML_STATUS_OK && name_index < chain.name_count;
         name_index += 1) {
        if (sxml_cldr_locale_is_root(&chain.names[name_index])) {
            break;
        }
        status = read_locale_file(
            &(locale_file_request){.processor = processor,
                                   .directory = SXML_BYTE_VIEW_FROM_LITERAL("rbnf/"),
                                   .file_name = name_view(&chain.names[name_index]),
                                   .kept_paths = {.prefixes = rules_paths, .prefix_count = 1}},
            &entry->rules_file);
        if (status == SXML_STATUS_OK && entry->rules_file.is_served &&
            entry->rules_file.entry_count > 0) {
            found_index = name_index;
            break;
        }
        sxml_cldr_file_release(&entry->rules_file);
    }
    if (status == SXML_STATUS_OK && found_index < chain.name_count) {
        status = read_ordinals(processor, state);
    }
    if (status == SXML_STATUS_OK && found_index < chain.name_count) {
        status = sxml_spell_out_rules_read(
            &(sxml_spell_out_source){
                .allocator = processor->allocator,
                .rules_file = &entry->rules_file,
                .plurals_file = state->ordinals.is_served ? &state->ordinals : NULL,
                .locale_name = name_view(&chain.names[found_index])},
            &entry->rules);
    }
    if (status == SXML_STATUS_OK) {
        entry->are_words_read = true;
    } else {
        sxml_cldr_file_release(&entry->rules_file);
    }
    return status;
}

static sxml_status find_number_words(const sxml_number_words_query *query,
                                     sxml_number_words *words) {
    *words = (sxml_number_words){.state = NULL, .operations = NULL};
    localization_state *state = NULL;
    sxml_status status = get_state(query->processor, &state);
    language_entry *entry = NULL;
    if (status == SXML_STATUS_OK) {
        status = find_entry(
            &(entry_request){.processor = query->processor, .state = state, .language = query->language},
            &entry);
    }
    if (status != SXML_STATUS_OK || entry == NULL) {
        return status;
    }
    if (!entry->are_words_read) {
        status = build_words(query->processor, state, entry);
    }
    if (status == SXML_STATUS_OK && entry->rules != NULL) {
        *words = (sxml_number_words){.state = entry->rules,
                                     .operations = &sxml_spell_out_number_words_operations};
    }
    return status;
}

/* ---------------------------------------------------------- Definition */

static const sxml_localization_operations localization_operations = {
    .find_date_names = find_date_names,
    .find_number_words = find_number_words,
};

const sxml_optional_feature_definition sxml_localization_feature_definition = {
    .feature_kind = SXML_OPTIONAL_FEATURE_KIND_LOCALIZATION,
    .feature_name = SXML_BYTE_VIEW_INITIALIZER("localization"),
    .operations = &localization_operations,
    .finalize_state = finalize_state,
    .library_functions = NULL,
};

sxml_optional_feature sxml_localization_feature(void) {
    return (sxml_optional_feature){.definition = &sxml_localization_feature_definition};
}
