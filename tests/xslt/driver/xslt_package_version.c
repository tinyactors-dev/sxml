/*
 * Package versions and version ranges for the test resolver (XSLT 4.0,
 * 3.4.1; packet 4.16, D160): the resolver serves the highest version of a
 * package that the range of the xsl:use-package matches, the catalog's
 * package_version_resolution "highest_version" (D117).
 *
 * The same grammar and order as src/xslt_package.c, which checks the
 * version of the package it is given (the driver uses the public API
 * only; D160 proposes a public function for both).
 */
#include "xslt_package_version.h"

#include "sxml/memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool is_space(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

static sxml_byte_view trim(sxml_byte_view text) {
    while (text.byte_count > 0 && is_space(text.bytes[0])) {
        text.bytes += 1;
        text.byte_count -= 1;
    }
    while (text.byte_count > 0 && is_space(text.bytes[text.byte_count - 1])) {
        text.byte_count -= 1;
    }
    return text;
}

/* One portion of a version (3.4.1). */
typedef struct version_portion {
    bool is_integer;
    uint64_t integer;
    sxml_byte_view name;
} version_portion;

#define MAXIMUM_VERSION_PORTIONS 16U

typedef struct parsed_version {
    version_portion portions[MAXIMUM_VERSION_PORTIONS];
    size_t portion_count;
} parsed_version;

static bool is_name_start_byte(uint8_t byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '_' ||
           byte >= 0x80U;
}

static bool is_name_byte(uint8_t byte) {
    return is_name_start_byte(byte) || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.';
}

/* PackageVersion ::= NumericPart ( "-" NamePart )?, NumericPart ::=
   IntegerLiteral ( "." IntegerLiteral )*. */
static bool parse_version(sxml_byte_view text, parsed_version *version) {
    version->portion_count = 0;
    size_t position = 0;
    while (true) {
        if (position >= text.byte_count || text.bytes[position] < '0' ||
            text.bytes[position] > '9' || version->portion_count == MAXIMUM_VERSION_PORTIONS) {
            return false;
        }
        uint64_t integer = 0;
        while (position < text.byte_count && text.bytes[position] >= '0' &&
               text.bytes[position] <= '9') {
            if (integer > UINT64_MAX / 10U - 10U) {
                return false;
            }
            integer = integer * 10U + (uint64_t)(text.bytes[position] - '0');
            position += 1;
        }
        version->portions[version->portion_count] = (version_portion){
            .is_integer = true, .integer = integer, .name = {.bytes = NULL, .byte_count = 0}};
        version->portion_count += 1;
        if (position < text.byte_count && text.bytes[position] == '.') {
            position += 1;
            continue;
        }
        break;
    }
    if (position == text.byte_count) {
        return true;
    }
    if (text.bytes[position] != '-' || position + 1U >= text.byte_count ||
        !is_name_start_byte(text.bytes[position + 1U]) ||
        version->portion_count == MAXIMUM_VERSION_PORTIONS) {
        return false;
    }
    const size_t start = position + 1U;
    for (position = start; position < text.byte_count; position += 1) {
        if (!is_name_byte(text.bytes[position])) {
            return false;
        }
    }
    version->portions[version->portion_count] = (version_portion){
        .is_integer = false,
        .integer = 0,
        .name = {.bytes = text.bytes + start, .byte_count = text.byte_count - start}};
    version->portion_count += 1;
    return true;
}

/* Trailing zero portions dropped: zero integers not followed by an
   integer (3.4.1). */
static parsed_version normalized(parsed_version version) {
    size_t integer_end = 0;
    while (integer_end < version.portion_count && version.portions[integer_end].is_integer) {
        integer_end += 1;
    }
    size_t kept = integer_end;
    while (kept > 0 && version.portions[kept - 1U].integer == 0) {
        kept -= 1;
    }
    if (kept < integer_end) {
        const size_t tail = version.portion_count - integer_end;
        memmove(&version.portions[kept], &version.portions[integer_end],
                tail * sizeof(version_portion));
        version.portion_count = kept + tail;
    }
    return version;
}

static int compare_portions(const version_portion *first, const version_portion *second) {
    if (first->is_integer && second->is_integer) {
        return first->integer < second->integer ? -1 : first->integer > second->integer ? 1 : 0;
    }
    if (first->is_integer != second->is_integer) {
        /* An NCName comes first. */
        return first->is_integer ? 1 : -1;
    }
    const size_t common = first->name.byte_count < second->name.byte_count
                              ? first->name.byte_count
                              : second->name.byte_count;
    const int order = common == 0 ? 0 : memcmp(first->name.bytes, second->name.bytes, common);
    if (order != 0) {
        return order < 0 ? -1 : 1;
    }
    return first->name.byte_count < second->name.byte_count   ? -1
           : first->name.byte_count > second->name.byte_count ? 1
                                                              : 0;
}

/* The order of two versions (3.4.1): -1, 0 or 1. */
typedef struct version_pair {
    const parsed_version *first;
    const parsed_version *second;
} version_pair;

static int compare_versions(version_pair pair) {
    const parsed_version first = normalized(*pair.first);
    const parsed_version second = normalized(*pair.second);
    const size_t common =
        first.portion_count < second.portion_count ? first.portion_count : second.portion_count;
    for (size_t index = 0; index < common; index += 1) {
        const int order = compare_portions(&first.portions[index], &second.portions[index]);
        if (order != 0) {
            return order;
        }
    }
    if (first.portion_count == second.portion_count) {
        return 0;
    }
    if (first.portion_count < second.portion_count) {
        return second.portions[common].is_integer ? -1 : 1;
    }
    return first.portions[common].is_integer ? 1 : -1;
}

/* The leading portions of version are those of prefix. */
static bool has_prefix(version_pair pair) {
    if (pair.first->portion_count < pair.second->portion_count) {
        return false;
    }
    for (size_t index = 0; index < pair.second->portion_count; index += 1) {
        if (compare_portions(&pair.first->portions[index], &pair.second->portions[index]) != 0) {
            return false;
        }
    }
    return true;
}

static bool ends_with(sxml_byte_view text, sxml_byte_view suffix) {
    return text.byte_count >= suffix.byte_count &&
           memcmp(text.bytes + text.byte_count - suffix.byte_count, suffix.bytes,
                  suffix.byte_count) == 0;
}

/* The end of a range "to X" (X a version or a prefix V.*). */
typedef struct range_end {
    parsed_version version;
    bool is_prefix;
} range_end;

static bool parse_range_end(sxml_byte_view text, range_end *end) {
    end->is_prefix = ends_with(text, SXML_BYTE_VIEW_FROM_LITERAL(".*"));
    if (end->is_prefix) {
        text.byte_count -= 2;
    }
    return parse_version(text, &end->version);
}

static bool is_at_most(const parsed_version *version, const range_end *end) {
    return compare_versions((version_pair){.first = version, .second = &end->version}) <= 0 ||
           (end->is_prefix &&
            has_prefix((version_pair){.first = version, .second = &end->version}));
}

/* One VersionRange matching version. */
typedef struct range_test {
    sxml_byte_view range;
    const parsed_version *version;
} range_test;

/* Whether the range has the grammar's syntax, and then whether it
   matches. */
typedef struct range_outcome {
    bool is_valid;
    bool matches;
} range_outcome;

static range_outcome matches_range(range_test test) {
    const range_outcome invalid = {.is_valid = false, .matches = false};
    sxml_byte_view range = trim(test.range);
    /* VersionTo: "to" S (PackageVersion | VersionPrefix). */
    if (range.byte_count > 3 && memcmp(range.bytes, "to", 2) == 0 && is_space(range.bytes[2])) {
        range_end end;
        if (!parse_range_end(trim((sxml_byte_view){.bytes = range.bytes + 2,
                                                   .byte_count = range.byte_count - 2}),
                             &end)) {
            return invalid;
        }
        return (range_outcome){.is_valid = true, .matches = is_at_most(test.version, &end)};
    }
    /* VersionFromTo: PackageVersion S "to" S (...). */
    for (size_t position = 1; position + 3U < range.byte_count; position += 1) {
        if (is_space(range.bytes[position - 1U]) && range.bytes[position] == 't' &&
            range.bytes[position + 1U] == 'o' && is_space(range.bytes[position + 2U])) {
            parsed_version from;
            range_end end;
            if (!parse_version(trim((sxml_byte_view){.bytes = range.bytes, .byte_count = position}),
                               &from) ||
                !parse_range_end(
                    trim((sxml_byte_view){.bytes = range.bytes + position + 2U,
                                          .byte_count = range.byte_count - position - 2U}),
                    &end)) {
                return invalid;
            }
            return (range_outcome){.is_valid = true,
                                   .matches = compare_versions((version_pair){
                                                  .first = test.version, .second = &from}) >= 0 &&
                                              is_at_most(test.version, &end)};
        }
    }
    parsed_version bound;
    if (ends_with(range, SXML_BYTE_VIEW_FROM_LITERAL("+"))) {
        if (!parse_version(
                (sxml_byte_view){.bytes = range.bytes, .byte_count = range.byte_count - 1},
                &bound)) {
            return invalid;
        }
        return (range_outcome){.is_valid = true,
                               .matches = compare_versions((version_pair){.first = test.version,
                                                                          .second = &bound}) >= 0};
    }
    if (ends_with(range, SXML_BYTE_VIEW_FROM_LITERAL(".*"))) {
        if (!parse_version(
                (sxml_byte_view){.bytes = range.bytes, .byte_count = range.byte_count - 2},
                &bound)) {
            return invalid;
        }
        return (range_outcome){
            .is_valid = true,
            .matches = has_prefix((version_pair){.first = test.version, .second = &bound})};
    }
    if (!parse_version(range, &bound)) {
        return invalid;
    }
    return (range_outcome){
        .is_valid = true,
        .matches = compare_versions((version_pair){.first = test.version, .second = &bound}) == 0};
}

sxml_status sxml_xslt_test_package_version_matches(sxml_xslt_test_package_version_query query,
                                                   sxml_xslt_test_package_version_match *match) {
    parsed_version version;
    if (!parse_version(trim(query.package_version), &version)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_byte_view range = trim(query.version_range);
    if (range.byte_count == 1 && range.bytes[0] == '*') {
        match->matches = true;
        return SXML_STATUS_OK;
    }
    bool is_matched = false;
    size_t start = 0;
    for (size_t position = 0; position <= range.byte_count; position += 1) {
        if (position < range.byte_count && range.bytes[position] != ',') {
            continue;
        }
        const range_outcome outcome = matches_range(
            (range_test){.range = {.bytes = range.bytes + start, .byte_count = position - start},
                         .version = &version});
        if (!outcome.is_valid) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        is_matched = is_matched || outcome.matches;
        start = position + 1U;
    }
    match->matches = is_matched;
    return SXML_STATUS_OK;
}

int sxml_xslt_test_compare_package_versions(sxml_xslt_test_package_version_pair pair) {
    parsed_version first;
    parsed_version second;
    const bool is_first_valid = parse_version(trim(pair.first), &first);
    const bool is_second_valid = parse_version(trim(pair.second), &second);
    if (!is_first_valid || !is_second_valid) {
        return is_first_valid ? 1 : is_second_valid ? -1 : 0;
    }
    return compare_versions((version_pair){.first = &first, .second = &second});
}

/* The value of attribute_name on the first element whose local name is
   "package" (prefixed or not), as written; empty when none. */
static bool is_name_character(uint8_t byte) { return is_name_byte(byte) || byte == ':'; }

sxml_byte_view sxml_xslt_test_package_attribute(sxml_xslt_test_package_attribute_query query) {
    const sxml_byte_view none = {.bytes = NULL, .byte_count = 0};
    const sxml_byte_view text = query.module_text;
    for (size_t position = 0; position + 8U < text.byte_count; position += 1) {
        if (text.bytes[position] != '<' || text.bytes[position + 1U] == '?' ||
            text.bytes[position + 1U] == '!') {
            continue;
        }
        size_t name_end = position + 1U;
        while (name_end < text.byte_count && is_name_character(text.bytes[name_end])) {
            name_end += 1;
        }
        const sxml_byte_view element_name = {.bytes = text.bytes + position + 1U,
                                             .byte_count = name_end - position - 1U};
        const bool is_package =
            element_name.byte_count >= 7 &&
            memcmp(element_name.bytes + element_name.byte_count - 7, "package", 7) == 0 &&
            (element_name.byte_count == 7 ||
             element_name.bytes[element_name.byte_count - 8] == ':');
        if (!is_package) {
            /* Only the document element counts. */
            return none;
        }
        size_t cursor = name_end;
        while (cursor < text.byte_count && text.bytes[cursor] != '>') {
            while (cursor < text.byte_count && is_space(text.bytes[cursor])) {
                cursor += 1;
            }
            const size_t attribute_start = cursor;
            while (cursor < text.byte_count && is_name_character(text.bytes[cursor])) {
                cursor += 1;
            }
            const sxml_byte_view attribute = {.bytes = text.bytes + attribute_start,
                                              .byte_count = cursor - attribute_start};
            while (cursor < text.byte_count && is_space(text.bytes[cursor])) {
                cursor += 1;
            }
            if (attribute.byte_count == 0 || cursor >= text.byte_count ||
                text.bytes[cursor] != '=') {
                return none;
            }
            cursor += 1;
            while (cursor < text.byte_count && is_space(text.bytes[cursor])) {
                cursor += 1;
            }
            if (cursor >= text.byte_count ||
                (text.bytes[cursor] != '"' && text.bytes[cursor] != '\'')) {
                return none;
            }
            const uint8_t quote = text.bytes[cursor];
            const size_t value_start = cursor + 1U;
            cursor = value_start;
            while (cursor < text.byte_count && text.bytes[cursor] != quote) {
                cursor += 1;
            }
            if (sxml_byte_view_equals(attribute, query.attribute_name)) {
                return trim((sxml_byte_view){.bytes = text.bytes + value_start,
                                             .byte_count = cursor - value_start});
            }
            cursor += 1;
        }
        return none;
    }
    return none;
}
