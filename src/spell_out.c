/*
 * Spell-out rules (src/language_table_internal.h; glossary
 * `spell_out_rule_set`, `plural_rule`; packet 7.17, D221): CLDR's
 * rule-based number formatting (UTS #35, Part 3, section 6; the rule
 * syntax of ICU's RuleBasedNumberFormat, which the section defers to),
 * read from an rbnf file and run for non-negative integers.
 *
 * Golden file: src/collation_tailoring.c (CLDR rules read from the files
 * the host serves into arrays in allocator memory, explicit loops instead
 * of recursion).
 *
 * The rules. A rule set is a list of rules by ascending base value; a
 * rule's divisor is the highest power of its radix (10, or the radix
 * attribute) not above its base value. A number is formatted by the last
 * rule whose base value is not above it, or the one before that when the
 * rule has a modulus substitution, the number is a multiple of the divisor
 * and the base value is not (ICU's rollback). A rule's text is literal
 * text and substitutions: →→ formats the number modulo the divisor, ←←
 * the number divided by the divisor, =…= the number itself, each with the
 * rule set named between the arrows (%name, or %%name for a private one;
 * the rule's own set when none) or with a decimal pattern (#,##0: the
 * digits); $(ordinal,one{…}other{…})$ chooses text by the ordinal plural
 * category of the number divided by the divisor (UTS #35, Part 3,
 * section 5). Text in brackets is optional: a rule whose base value is a
 * multiple of its divisor becomes two rules, one without the bracketed
 * text at the base value and one with it at the base value plus one; any
 * other rule keeps the text. A leading apostrophe keeps the spaces after
 * it.
 *
 * Choosing a rule set (F&O 4.0, section 4.8.1, the format modifier's
 * text in parentheses; XSLT 4.0, 12.4, the ordinal attribute): "%name"
 * names a public rule set, and a name the language does not have falls
 * back to its default ordinal or cardinal rules, by whether the name has
 * "ordinal" in it; "-ending" ("-er", "-a") takes the first public rule
 * set of the kind whose words for the number end with the ending; no
 * text takes the default: spellout-ordinal, else spellout-ordinal-
 * masculine, else the first spellout-ordinal-*, for ordinals;
 * spellout-cardinal, else spellout-numbering, else spellout-cardinal-
 * masculine, for cardinals; digits-ordinal for an ordinal of digits,
 * whose decimal pattern then writes nothing, so only the suffix is
 * written after the digits sxml formats (no suffix at all for a language
 * whose rules have no digits-ordinal, as German's).
 *
 * Case: W writes the words in upper case (Unicode's full mappings, so
 * "ß" becomes "SS"), Ww each word's first letter in upper case, w the
 * words as CLDR writes them.
 *
 * No recursion (D71): a substitution pushes a frame on an explicit stack
 * of at most SXML_SPELL_OUT_MAXIMUM_DEPTH frames; a run stops after
 * SXML_SPELL_OUT_MAXIMUM_STEP_COUNT steps (rules that loop), and its
 * words are then not written.
 */
#include "language_table_internal.h"

#include "cldr_internal.h"
#include "function_library_core_internal.h"
#include "memory_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>

#define SXML_SPELL_OUT_MAXIMUM_DEPTH 64U
#define SXML_SPELL_OUT_MAXIMUM_STEP_COUNT 4096U
/* The most digits of a number the rules take (below 2^64). */
#define SXML_SPELL_OUT_MAXIMUM_DIGIT_COUNT 19U

#define SXML_SPELL_OUT_SAME_RULE_SET SIZE_MAX

/* ------------------------------------------------------------- Model */

typedef enum part_kind {
    SXML_PART_KIND_TEXT,
    /* →→ */
    SXML_PART_KIND_MODULUS,
    /* ←← */
    SXML_PART_KIND_QUOTIENT,
    /* =…= */
    SXML_PART_KIND_SAME_VALUE,
    /* $(…)$ */
    SXML_PART_KIND_PLURAL,
} part_kind;

typedef struct rule_part {
    part_kind part_kind;
    /* TEXT: the literal text. PLURAL: the text between "$(" and ")$".
       Substitutions: the descriptor between the marks. */
    sxml_byte_view text;
    /* Substitutions: the rule set, SXML_SPELL_OUT_SAME_RULE_SET for the
       rule's own; unused with a decimal pattern. */
    size_t rule_set_index;
    bool is_decimal_pattern;
} rule_part;

typedef struct spell_out_rule {
    uint64_t base_value;
    uint64_t divisor;
    size_t first_part;
    size_t part_count;
    bool has_modulus;
} spell_out_rule;

typedef struct spell_out_rule_set {
    sxml_byte_view name;
    bool is_private;
    size_t first_rule;
    size_t rule_count;
} spell_out_rule_set;

typedef struct plural_rule {
    sxml_byte_view keyword;
    /* The condition before "@" (the samples). */
    sxml_byte_view condition;
} plural_rule;

struct sxml_spell_out_rules {
    spell_out_rule_set *rule_sets;
    size_t rule_set_count;
    spell_out_rule *rules;
    size_t rule_count;
    rule_part *parts;
    size_t part_count;
    plural_rule *plural_rules;
    size_t plural_rule_count;
    /* The rule texts with their brackets resolved. */
    uint8_t *text;
    size_t text_byte_count;
};

static bool view_is(sxml_byte_view view, const char *text) {
    return sxml_byte_view_equals(view, sxml_byte_view_from_c_string(text));
}

static bool starts_with(sxml_byte_view text, sxml_byte_view prefix) {
    return text.byte_count >= prefix.byte_count &&
           (prefix.byte_count == 0 || memcmp(text.bytes, prefix.bytes, prefix.byte_count) == 0);
}

static bool ends_with(sxml_byte_view text, sxml_byte_view suffix) {
    return text.byte_count >= suffix.byte_count &&
           (suffix.byte_count == 0 ||
            memcmp(text.bytes + text.byte_count - suffix.byte_count, suffix.bytes,
                   suffix.byte_count) == 0);
}

static sxml_byte_view after(sxml_byte_view text, size_t byte_count) {
    return (sxml_byte_view){.bytes = text.bytes + byte_count,
                            .byte_count = text.byte_count - byte_count};
}

/* Reads ASCII digits; false for anything else or a value of 2^64 or
   more. */
static bool read_unsigned(sxml_byte_view text, uint64_t *value) {
    if (text.byte_count == 0) {
        return false;
    }
    uint64_t result = 0;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte < '0' || byte > '9' || result > (UINT64_MAX - 9U) / 10U) {
            return false;
        }
        result = result * 10U + (uint64_t)(byte - '0');
    }
    *value = result;
    return true;
}

/* ------------------------------------------------------- Rule texts */

/* The marks of substitutions: the arrows of CLDR's files and ICU's
   ASCII forms. */
static size_t substitution_mark_length(sxml_byte_view text, size_t offset, part_kind *kind) {
    const uint8_t byte = text.bytes[offset];
    if (byte == '>' || byte == '<' || byte == '=') {
        *kind = byte == '>' ? SXML_PART_KIND_MODULUS
                : byte == '<' ? SXML_PART_KIND_QUOTIENT
                              : SXML_PART_KIND_SAME_VALUE;
        return 1;
    }
    if (byte == 0xE2 && offset + 2 < text.byte_count && text.bytes[offset + 1] == 0x86 &&
        (text.bytes[offset + 2] == 0x92 || text.bytes[offset + 2] == 0x90)) {
        *kind = text.bytes[offset + 2] == 0x92 ? SXML_PART_KIND_MODULUS
                                               : SXML_PART_KIND_QUOTIENT;
        return 3;
    }
    return 0;
}

/* One rule as read, before its parts are made. */
typedef struct rule_text {
    uint64_t base_value;
    uint64_t radix;
    /* The body without the final ";" and a leading apostrophe. */
    sxml_byte_view body;
} rule_text;

/* Reads an rbnfrule entry; false for a rule sxml leaves out. */
static bool read_rule_text(const sxml_cldr_entry *entry, rule_text *read) {
    sxml_byte_view value_text = {.bytes = NULL, .byte_count = 0};
    if (!sxml_cldr_find_attribute(&(sxml_cldr_attribute_query){.path = entry->path,
                                                               .attribute_name =
                                                                   SXML_BYTE_VIEW_FROM_LITERAL(
                                                                       "value")},
                                  &value_text) ||
        !read_unsigned(value_text, &read->base_value)) {
        return false;
    }
    sxml_byte_view radix_text = {.bytes = NULL, .byte_count = 0};
    read->radix = 10;
    if (sxml_cldr_find_attribute(
            &(sxml_cldr_attribute_query){.path = entry->path,
                                         .attribute_name = SXML_BYTE_VIEW_FROM_LITERAL("radix")},
            &radix_text) &&
        (!read_unsigned(radix_text, &read->radix) || read->radix < 2)) {
        return false;
    }
    sxml_byte_view body = entry->value;
    while (body.byte_count > 0 && sxml_xml_byte_is_whitespace(body.bytes[body.byte_count - 1])) {
        body.byte_count -= 1;
    }
    if (body.byte_count > 0 && body.bytes[body.byte_count - 1] == ';') {
        body.byte_count -= 1;
    }
    while (body.byte_count > 0 && sxml_xml_byte_is_whitespace(body.bytes[0])) {
        body = after(body, 1);
    }
    if (body.byte_count > 0 && body.bytes[0] == '\'') {
        body = after(body, 1);
    }
    read->body = body;
    return true;
}

/* The divisor of a rule: the highest power of radix not above base. */
static uint64_t rule_divisor(uint64_t base_value, uint64_t radix) {
    uint64_t divisor = 1;
    while (divisor <= base_value / radix) {
        divisor *= radix;
    }
    return divisor;
}

/* The first bracket pair of a body: open < close, or none. */
typedef struct bracket_pair {
    bool is_found;
    size_t open;
    size_t close;
} bracket_pair;

static bracket_pair find_brackets(sxml_byte_view body) {
    bracket_pair pair = {.is_found = false, .open = 0, .close = 0};
    const uint8_t *open = body.byte_count == 0 ? NULL : memchr(body.bytes, '[', body.byte_count);
    if (open == NULL) {
        return pair;
    }
    const size_t open_offset = (size_t)(open - body.bytes);
    const uint8_t *close =
        memchr(open + 1, ']', body.byte_count - open_offset - 1);
    if (close == NULL) {
        return pair;
    }
    return (bracket_pair){
        .is_found = true, .open = open_offset, .close = (size_t)(close - body.bytes)};
}

/* ----------------------------------------------------- Growing arrays */

typedef struct grown_array {
    sxml_allocator allocator;
    void *items;
    size_t item_count;
    size_t item_capacity;
    size_t item_size;
    size_t item_alignment;
} grown_array;

static sxml_status grow(grown_array *array, void **slot) {
    if (array->item_count == array->item_capacity) {
        const size_t capacity = array->item_capacity < 16 ? 16 : array->item_capacity * 2U;
        void *memory = NULL;
        const sxml_status status = sxml_allocator_allocate_array(
            array->allocator,
            (sxml_array_allocation_request){.element_count = capacity,
                                            .element_size = array->item_size,
                                            .element_alignment = array->item_alignment},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (array->item_count > 0) {
            memcpy(memory, array->items, array->item_count * array->item_size);
        }
        sxml_allocator_release(array->allocator, array->items);
        array->items = memory;
        array->item_capacity = capacity;
    }
    *slot = (uint8_t *)array->items + array->item_count * array->item_size;
    array->item_count += 1;
    return SXML_STATUS_OK;
}

/* --------------------------------------------------------- Reading */

/* The arrays being built. */
typedef struct rules_builder {
    sxml_allocator allocator;
    grown_array rule_sets;
    grown_array rules;
    grown_array parts;
    grown_array plural_rules;
    /* The rule texts, in a block sized before they are written. */
    uint8_t *text;
    size_t text_byte_count;
    size_t text_capacity;
} rules_builder;

static sxml_byte_view copy_text(rules_builder *builder, sxml_byte_view text) {
    uint8_t *start = builder->text + builder->text_byte_count;
    if (text.byte_count > 0 && builder->text_byte_count + text.byte_count <= builder->text_capacity) {
        memcpy(start, text.bytes, text.byte_count);
        builder->text_byte_count += text.byte_count;
    }
    return (sxml_byte_view){.bytes = start, .byte_count = text.byte_count};
}

static sxml_status add_part(rules_builder *builder, rule_part part) {
    void *slot = NULL;
    const sxml_status status = grow(&builder->parts, &slot);
    if (status == SXML_STATUS_OK) {
        *(rule_part *)slot = part;
    }
    return status;
}

/* Splits a body (brackets already resolved) into parts. Rule set names
   stay as text until every set is known. */
static sxml_status add_body_parts(rules_builder *builder, sxml_byte_view body,
                                  spell_out_rule *rule) {
    rule->first_part = builder->parts.item_count;
    rule->has_modulus = false;
    sxml_status status = SXML_STATUS_OK;
    size_t text_start = 0;
    size_t offset = 0;
    while (status == SXML_STATUS_OK && offset < body.byte_count) {
        part_kind kind = SXML_PART_KIND_TEXT;
        size_t mark_length = substitution_mark_length(body, offset, &kind);
        const bool is_plural = body.bytes[offset] == '$' && offset + 1 < body.byte_count &&
                               body.bytes[offset + 1] == '(';
        if (mark_length == 0 && !is_plural) {
            offset += 1;
            continue;
        }
        size_t end = offset;
        sxml_byte_view inside = {.bytes = NULL, .byte_count = 0};
        if (is_plural) {
            const sxml_byte_view rest = after(body, offset + 2);
            size_t close = 0;
            while (close + 1 < rest.byte_count &&
                   !(rest.bytes[close] == ')' && rest.bytes[close + 1] == '$')) {
                close += 1;
            }
            if (close + 1 >= rest.byte_count) {
                offset += 1;
                continue;
            }
            kind = SXML_PART_KIND_PLURAL;
            inside = (sxml_byte_view){.bytes = rest.bytes, .byte_count = close};
            end = offset + 2 + close + 2;
        } else {
            /* The descriptor runs to the next mark of the same kind. */
            size_t search = offset + mark_length;
            bool is_closed = false;
            while (search < body.byte_count) {
                part_kind closing = SXML_PART_KIND_TEXT;
                const size_t closing_length = substitution_mark_length(body, search, &closing);
                if (closing_length > 0 && closing == kind) {
                    inside = (sxml_byte_view){.bytes = body.bytes + offset + mark_length,
                                              .byte_count = search - offset - mark_length};
                    end = search + closing_length;
                    is_closed = true;
                    break;
                }
                search += 1;
            }
            if (!is_closed) {
                offset += mark_length;
                continue;
            }
        }
        if (offset > text_start) {
            status = add_part(builder, (rule_part){.part_kind = SXML_PART_KIND_TEXT,
                                                   .text = {.bytes = body.bytes + text_start,
                                                            .byte_count = offset - text_start}});
        }
        if (status == SXML_STATUS_OK) {
            const bool is_rule_set = inside.byte_count > 0 && inside.bytes[0] == '%';
            status = add_part(builder,
                              (rule_part){.part_kind = kind,
                                          .text = inside,
                                          .rule_set_index = SXML_SPELL_OUT_SAME_RULE_SET,
                                          .is_decimal_pattern = kind != SXML_PART_KIND_PLURAL &&
                                                                inside.byte_count > 0 &&
                                                                !is_rule_set});
        }
        rule->has_modulus = rule->has_modulus || kind == SXML_PART_KIND_MODULUS;
        offset = end;
        text_start = end;
    }
    if (status == SXML_STATUS_OK && body.byte_count > text_start) {
        status = add_part(builder, (rule_part){.part_kind = SXML_PART_KIND_TEXT,
                                               .text = after(body, text_start)});
    }
    rule->part_count = builder->parts.item_count - rule->first_part;
    return status;
}

static sxml_status add_rule(rules_builder *builder, rule_text read) {
    const uint64_t divisor = rule_divisor(read.base_value, read.radix);
    const bracket_pair brackets = find_brackets(read.body);
    sxml_byte_view full_body = read.body;
    if (brackets.is_found) {
        /* The text with the brackets removed. */
        const sxml_byte_view head = {.bytes = read.body.bytes, .byte_count = brackets.open};
        const sxml_byte_view inside = {.bytes = read.body.bytes + brackets.open + 1,
                                       .byte_count = brackets.close - brackets.open - 1};
        const sxml_byte_view tail = after(read.body, brackets.close + 1);
        full_body = copy_text(builder, head);
        full_body.byte_count += copy_text(builder, inside).byte_count;
        full_body.byte_count += copy_text(builder, tail).byte_count;
        if (read.base_value % divisor == 0 && read.base_value < UINT64_MAX) {
            /* The rule without the bracketed text, at the base value. */
            sxml_byte_view short_body = copy_text(builder, head);
            short_body.byte_count += copy_text(builder, tail).byte_count;
            void *slot = NULL;
            sxml_status status = grow(&builder->rules, &slot);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            spell_out_rule *rule = slot;
            *rule = (spell_out_rule){.base_value = read.base_value, .divisor = divisor};
            status = add_body_parts(builder, short_body, rule);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            read.base_value += 1;
        }
    }
    void *slot = NULL;
    const sxml_status status = grow(&builder->rules, &slot);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    spell_out_rule *rule = slot;
    *rule = (spell_out_rule){.base_value = read.base_value, .divisor = divisor};
    return add_body_parts(builder, full_body, rule);
}

/* The grouping, rule set and rule of an rbnfrule entry's path. */
typedef struct rule_place {
    sxml_byte_view rule_set_path;
    sxml_byte_view rule_set_name;
    bool is_private;
    bool is_kept_grouping;
} rule_place;

static rule_place read_rule_place(const sxml_cldr_entry *entry) {
    rule_place place = {.rule_set_path = sxml_cldr_parent_path(entry->path)};
    const sxml_byte_view grouping_path = sxml_cldr_parent_path(place.rule_set_path);
    sxml_byte_view grouping = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view access = {.bytes = NULL, .byte_count = 0};
    place.is_kept_grouping =
        sxml_cldr_find_attribute(
            &(sxml_cldr_attribute_query){.path = grouping_path,
                                         .attribute_name = SXML_BYTE_VIEW_FROM_LITERAL("type")},
            &grouping) &&
        (view_is(grouping, "SpelloutRules") || view_is(grouping, "OrdinalRules")) &&
        sxml_cldr_find_attribute(
            &(sxml_cldr_attribute_query){.path = place.rule_set_path,
                                         .attribute_name = SXML_BYTE_VIEW_FROM_LITERAL("type")},
            &place.rule_set_name);
    place.is_private =
        sxml_cldr_find_attribute(
            &(sxml_cldr_attribute_query){.path = place.rule_set_path,
                                         .attribute_name = SXML_BYTE_VIEW_FROM_LITERAL("access")},
            &access) &&
        view_is(access, "private");
    return place;
}

static sxml_status read_rule_sets(rules_builder *builder, const sxml_cldr_file *file) {
    sxml_status status = SXML_STATUS_OK;
    sxml_byte_view current_path = {.bytes = NULL, .byte_count = 0};
    spell_out_rule_set *current = NULL;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < file->entry_count;
         entry_index += 1) {
        const sxml_cldr_entry *entry = &file->entries[entry_index];
        rule_text read;
        if (entry->is_alias || !read_rule_text(entry, &read)) {
            continue;
        }
        const rule_place place = read_rule_place(entry);
        if (!place.is_kept_grouping) {
            continue;
        }
        if (current == NULL || !sxml_byte_view_equals(place.rule_set_path, current_path)) {
            void *slot = NULL;
            status = grow(&builder->rule_sets, &slot);
            if (status != SXML_STATUS_OK) {
                break;
            }
            current = slot;
            *current = (spell_out_rule_set){.name = place.rule_set_name,
                                            .is_private = place.is_private,
                                            .first_rule = builder->rules.item_count,
                                            .rule_count = 0};
            current_path = place.rule_set_path;
        }
        /* Rules of a set ascend; one that does not is left out. */
        const size_t rule_count = builder->rules.item_count;
        if (rule_count > current->first_rule &&
            ((spell_out_rule *)builder->rules.items)[rule_count - 1].base_value > read.base_value) {
            continue;
        }
        status = add_rule(builder, read);
        current = &((spell_out_rule_set *)builder->rule_sets.items)[builder->rule_sets.item_count - 1];
        current->rule_count = builder->rules.item_count - current->first_rule;
    }
    return status;
}

/* The index of the rule set called name, or SXML_SPELL_OUT_SAME_RULE_SET. */
static size_t find_rule_set(const sxml_spell_out_rules *rules, sxml_byte_view name) {
    for (size_t set_index = 0; set_index < rules->rule_set_count; set_index += 1) {
        if (sxml_byte_view_equals(rules->rule_sets[set_index].name, name)) {
            return set_index;
        }
    }
    return SXML_SPELL_OUT_SAME_RULE_SET;
}

/* Gives each substitution its rule set; a substitution naming a set the
   file does not have makes its rule unusable (a decimal pattern of no
   digits stands in, so nothing loops). */
static void resolve_rule_sets(sxml_spell_out_rules *rules) {
    for (size_t part_index = 0; part_index < rules->part_count; part_index += 1) {
        rule_part *part = &rules->parts[part_index];
        if (part->part_kind == SXML_PART_KIND_TEXT || part->part_kind == SXML_PART_KIND_PLURAL ||
            part->is_decimal_pattern || part->text.byte_count == 0) {
            continue;
        }
        sxml_byte_view name = part->text;
        while (name.byte_count > 0 && name.bytes[0] == '%') {
            name = after(name, 1);
        }
        part->rule_set_index = find_rule_set(rules, name);
        if (part->rule_set_index == SXML_SPELL_OUT_SAME_RULE_SET) {
            part->is_decimal_pattern = true;
        }
    }
}

/* True when the space-separated list has word. */
static bool list_has(sxml_byte_view list, sxml_byte_view word) {
    size_t position = 0;
    while (position < list.byte_count) {
        size_t end = position;
        while (end < list.byte_count && list.bytes[end] != ' ') {
            end += 1;
        }
        if (sxml_byte_view_equals(
                (sxml_byte_view){.bytes = list.bytes + position, .byte_count = end - position},
                word)) {
            return true;
        }
        position = end + 1;
    }
    return false;
}

/* Collects the ordinal plural rules of the locale, or of its language. */
static sxml_status read_plural_rules(rules_builder *builder, const sxml_spell_out_source *source) {
    if (source->plurals_file == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_byte_view language = source->locale_name;
    const uint8_t *separator =
        language.byte_count == 0 ? NULL : memchr(language.bytes, '_', language.byte_count);
    if (separator != NULL) {
        language.byte_count = (size_t)(separator - language.bytes);
    }
    const sxml_byte_view wanted[2] = {source->locale_name, language};
    sxml_status status = SXML_STATUS_OK;
    for (size_t wanted_index = 0; wanted_index < 2 && builder->plural_rules.item_count == 0;
         wanted_index += 1) {
        sxml_byte_view chosen_path = {.bytes = NULL, .byte_count = 0};
        const sxml_cldr_file *file = source->plurals_file;
        for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < file->entry_count;
             entry_index += 1) {
            const sxml_cldr_entry *entry = &file->entries[entry_index];
            const sxml_byte_view rules_path = sxml_cldr_parent_path(entry->path);
            sxml_byte_view locales = {.bytes = NULL, .byte_count = 0};
            sxml_byte_view keyword = {.bytes = NULL, .byte_count = 0};
            if (entry->is_alias ||
                !sxml_cldr_find_attribute(
                    &(sxml_cldr_attribute_query){
                        .path = rules_path,
                        .attribute_name = SXML_BYTE_VIEW_FROM_LITERAL("locales")},
                    &locales) ||
                !sxml_cldr_find_attribute(
                    &(sxml_cldr_attribute_query){
                        .path = entry->path, .attribute_name = SXML_BYTE_VIEW_FROM_LITERAL("count")},
                    &keyword)) {
                continue;
            }
            if (chosen_path.byte_count == 0) {
                if (!list_has(locales, wanted[wanted_index])) {
                    continue;
                }
                chosen_path = rules_path;
            } else if (!sxml_byte_view_equals(rules_path, chosen_path)) {
                continue;
            }
            sxml_byte_view condition = entry->value;
            const uint8_t *samples = condition.byte_count == 0
                                         ? NULL
                                         : memchr(condition.bytes, '@', condition.byte_count);
            if (samples != NULL) {
                condition.byte_count = (size_t)(samples - condition.bytes);
            }
            void *slot = NULL;
            status = grow(&builder->plural_rules, &slot);
            if (status == SXML_STATUS_OK) {
                *(plural_rule *)slot = (plural_rule){.keyword = keyword, .condition = condition};
            }
        }
    }
    return status;
}

static void release_builder(rules_builder *builder) {
    sxml_allocator_release(builder->allocator, builder->rule_sets.items);
    sxml_allocator_release(builder->allocator, builder->rules.items);
    sxml_allocator_release(builder->allocator, builder->parts.items);
    sxml_allocator_release(builder->allocator, builder->plural_rules.items);
    sxml_allocator_release(builder->allocator, builder->text);
}

sxml_status sxml_spell_out_rules_read(const sxml_spell_out_source *source,
                                      sxml_spell_out_rules **rules) {
    *rules = NULL;
    const sxml_allocator allocator = source->allocator;
    rules_builder builder = {
        .allocator = allocator,
        .rule_sets = {.allocator = allocator,
                      .item_size = sizeof(spell_out_rule_set),
                      .item_alignment = alignof(spell_out_rule_set)},
        .rules = {.allocator = allocator,
                  .item_size = sizeof(spell_out_rule),
                  .item_alignment = alignof(spell_out_rule)},
        .parts = {.allocator = allocator,
                  .item_size = sizeof(rule_part),
                  .item_alignment = alignof(rule_part)},
        .plural_rules = {.allocator = allocator,
                         .item_size = sizeof(plural_rule),
                         .item_alignment = alignof(plural_rule)},
        .text = NULL,
        .text_byte_count = 0,
        .text_capacity = 0};
    /* Bracketed rules are written twice at most. */
    size_t text_capacity = 0;
    for (size_t entry_index = 0; entry_index < source->rules_file->entry_count; entry_index += 1) {
        text_capacity += 2U * source->rules_file->entries[entry_index].value.byte_count;
    }
    void *text = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        allocator,
        (sxml_array_allocation_request){
            .element_count = text_capacity, .element_size = 1, .element_alignment = 1},
        &text);
    builder.text = text;
    builder.text_capacity = text_capacity;
    if (status == SXML_STATUS_OK) {
        status = read_rule_sets(&builder, source->rules_file);
    }
    if (status == SXML_STATUS_OK) {
        status = read_plural_rules(&builder, source);
    }
    void *made = NULL;
    if (status == SXML_STATUS_OK && builder.rule_sets.item_count > 0) {
        status = sxml_allocator_allocate_array(
            allocator,
            (sxml_array_allocation_request){.element_count = 1,
                                            .element_size = sizeof(sxml_spell_out_rules),
                                            .element_alignment = alignof(sxml_spell_out_rules)},
            &made);
    }
    if (status != SXML_STATUS_OK || made == NULL) {
        release_builder(&builder);
        return status;
    }
    sxml_spell_out_rules *result = made;
    *result = (sxml_spell_out_rules){.rule_sets = builder.rule_sets.items,
                                     .rule_set_count = builder.rule_sets.item_count,
                                     .rules = builder.rules.items,
                                     .rule_count = builder.rules.item_count,
                                     .parts = builder.parts.items,
                                     .part_count = builder.parts.item_count,
                                     .plural_rules = builder.plural_rules.items,
                                     .plural_rule_count = builder.plural_rules.item_count,
                                     .text = builder.text,
                                     .text_byte_count = builder.text_byte_count};
    resolve_rule_sets(result);
    *rules = result;
    return SXML_STATUS_OK;
}

void sxml_spell_out_rules_release(sxml_spell_out_rules *rules, sxml_allocator allocator) {
    if (rules == NULL) {
        return;
    }
    sxml_allocator_release(allocator, rules->rule_sets);
    sxml_allocator_release(allocator, rules->rules);
    sxml_allocator_release(allocator, rules->parts);
    sxml_allocator_release(allocator, rules->plural_rules);
    sxml_allocator_release(allocator, rules->text);
    sxml_allocator_release(allocator, rules);
}

/* ------------------------------------------------------ Plural rules */

/* A scanner over a plural condition (UTS #35, Part 3, section 5.1):
   condition = and_condition ('or' and_condition)*, and_condition =
   relation ('and' relation)*, relation = operand ('%' value)? ('=' |
   '!=') range_list, range_list = (value | value '..' value) (','
   ...)*. */
typedef struct condition_scanner {
    sxml_byte_view text;
    size_t position;
} condition_scanner;

static void skip_spaces(condition_scanner *scanner) {
    while (scanner->position < scanner->text.byte_count &&
           sxml_xml_byte_is_whitespace(scanner->text.bytes[scanner->position])) {
        scanner->position += 1;
    }
}

static bool scan_word(condition_scanner *scanner, const char *word) {
    skip_spaces(scanner);
    const sxml_byte_view expected = sxml_byte_view_from_c_string(word);
    if (!starts_with(after(scanner->text, scanner->position), expected)) {
        return false;
    }
    scanner->position += expected.byte_count;
    return true;
}

static bool scan_number(condition_scanner *scanner, uint64_t *value) {
    skip_spaces(scanner);
    const size_t start = scanner->position;
    while (scanner->position < scanner->text.byte_count &&
           scanner->text.bytes[scanner->position] >= '0' &&
           scanner->text.bytes[scanner->position] <= '9') {
        scanner->position += 1;
    }
    return read_unsigned((sxml_byte_view){.bytes = scanner->text.bytes + start,
                                          .byte_count = scanner->position - start},
                         value);
}

/* Evaluates one relation for the integer value; false in *is_valid for
   text that is not one. */
typedef struct relation_result {
    bool is_valid;
    bool is_true;
} relation_result;

static relation_result scan_relation(condition_scanner *scanner, uint64_t value) {
    relation_result result = {.is_valid = false, .is_true = false};
    skip_spaces(scanner);
    if (scanner->position >= scanner->text.byte_count) {
        return result;
    }
    /* The operands of an integer: n and i are the value, the others (the
       fraction's digits and the exponent) zero. */
    const uint8_t operand = scanner->text.bytes[scanner->position];
    scanner->position += 1;
    uint64_t operand_value = operand == 'n' || operand == 'i' ? value : 0U;
    if (scan_word(scanner, "%")) {
        uint64_t modulus = 0;
        if (!scan_number(scanner, &modulus) || modulus == 0) {
            return result;
        }
        operand_value %= modulus;
    }
    bool is_negated = false;
    if (scan_word(scanner, "!=")) {
        is_negated = true;
    } else if (!scan_word(scanner, "=")) {
        return result;
    }
    bool is_in_list = false;
    do {
        uint64_t low = 0;
        if (!scan_number(scanner, &low)) {
            return result;
        }
        uint64_t high = low;
        if (scan_word(scanner, "..") && !scan_number(scanner, &high)) {
            return result;
        }
        is_in_list = is_in_list || (operand_value >= low && operand_value <= high);
    } while (scan_word(scanner, ","));
    result.is_valid = true;
    result.is_true = is_negated ? !is_in_list : is_in_list;
    return result;
}

static bool condition_holds(sxml_byte_view condition, uint64_t value) {
    condition_scanner scanner = {.text = condition, .position = 0};
    skip_spaces(&scanner);
    if (scanner.position == condition.byte_count) {
        /* "other" has no condition. */
        return true;
    }
    bool is_any_true = false;
    bool is_all_true = true;
    while (true) {
        const relation_result relation = scan_relation(&scanner, value);
        if (!relation.is_valid) {
            return false;
        }
        is_all_true = is_all_true && relation.is_true;
        if (scan_word(&scanner, "and")) {
            continue;
        }
        if (scan_word(&scanner, "or")) {
            is_any_true = is_any_true || is_all_true;
            is_all_true = true;
            continue;
        }
        skip_spaces(&scanner);
        if (scanner.position != condition.byte_count) {
            return false;
        }
        return is_any_true || is_all_true;
    }
}

/* The plural keyword of value: the first rule that holds, else
   "other". */
static sxml_byte_view plural_keyword(const sxml_spell_out_rules *rules, uint64_t value) {
    for (size_t rule_index = 0; rule_index < rules->plural_rule_count; rule_index += 1) {
        const plural_rule *rule = &rules->plural_rules[rule_index];
        if (!view_is(rule->keyword, "other") && condition_holds(rule->condition, value)) {
            return rule->keyword;
        }
    }
    return SXML_BYTE_VIEW_FROM_LITERAL("other");
}

/* The text $(ordinal,one{st}other{th})$ gives value: the keyword's, else
   other's (a cardinal choice always takes other: sxml reads only the
   ordinal plural rules). */
static sxml_byte_view plural_text(const sxml_spell_out_rules *rules, rule_part part,
                                  uint64_t value) {
    const sxml_byte_view inside = part.text;
    const uint8_t *comma = inside.byte_count == 0 ? NULL : memchr(inside.bytes, ',', inside.byte_count);
    if (comma == NULL) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    const sxml_byte_view kind = {.bytes = inside.bytes, .byte_count = (size_t)(comma - inside.bytes)};
    const sxml_byte_view wanted = view_is(kind, "ordinal") ? plural_keyword(rules, value)
                                                           : SXML_BYTE_VIEW_FROM_LITERAL("other");
    sxml_byte_view other = {.bytes = NULL, .byte_count = 0};
    size_t position = (size_t)(comma - inside.bytes) + 1;
    while (position < inside.byte_count) {
        const uint8_t *open = memchr(inside.bytes + position, '{', inside.byte_count - position);
        if (open == NULL) {
            break;
        }
        const size_t open_offset = (size_t)(open - inside.bytes);
        const uint8_t *close = memchr(open, '}', inside.byte_count - open_offset);
        if (close == NULL) {
            break;
        }
        const sxml_byte_view keyword = {.bytes = inside.bytes + position,
                                        .byte_count = open_offset - position};
        const sxml_byte_view text = {.bytes = open + 1, .byte_count = (size_t)(close - open) - 1};
        if (sxml_byte_view_equals(keyword, wanted)) {
            return text;
        }
        if (view_is(keyword, "other")) {
            other = text;
        }
        position = (size_t)(close - inside.bytes) + 1;
    }
    return other;
}

/* ---------------------------------------------------------- Running */

/* The rule of set_index for value (the module comment). */
static const spell_out_rule *select_rule(const sxml_spell_out_rules *rules, size_t set_index,
                                         uint64_t value) {
    const spell_out_rule_set *set = &rules->rule_sets[set_index];
    size_t found = SIZE_MAX;
    for (size_t rule_index = 0; rule_index < set->rule_count; rule_index += 1) {
        if (rules->rules[set->first_rule + rule_index].base_value > value) {
            break;
        }
        found = rule_index;
    }
    if (found == SIZE_MAX) {
        return NULL;
    }
    const spell_out_rule *rule = &rules->rules[set->first_rule + found];
    if (rule->has_modulus && value % rule->divisor == 0 && rule->base_value % rule->divisor != 0 &&
        found > 0) {
        rule = &rules->rules[set->first_rule + found - 1];
    }
    return rule;
}

/* One substitution being formatted. */
typedef struct run_frame {
    size_t rule_set_index;
    uint64_t value;
    const spell_out_rule *rule;
    size_t next_part;
} run_frame;

/* A run of the rules: what to write, and how. */
typedef struct rules_run {
    const sxml_spell_out_rules *rules;
    size_t rule_set_index;
    uint64_t value;
    /* Decimal patterns write nothing (an ordinal suffix of digits). */
    bool is_suffix_only;
} rules_run;

/* Appends value's digits (a decimal pattern; "#,##0" groups by three
   with ","). */
typedef struct decimal_writing {
    uint64_t value;
    bool is_grouped;
} decimal_writing;

static sxml_status append_decimal(sxml_format_text *text, decimal_writing writing) {
    uint8_t digits[24];
    size_t digit_count = 0;
    uint64_t remaining = writing.value;
    do {
        digits[digit_count] = (uint8_t)('0' + remaining % 10U);
        digit_count += 1;
        remaining /= 10U;
    } while (remaining > 0);
    sxml_status status = SXML_STATUS_OK;
    for (size_t digit_index = digit_count; status == SXML_STATUS_OK && digit_index > 0;
         digit_index -= 1) {
        status = sxml_format_text_append(
            text, (sxml_byte_view){.bytes = &digits[digit_index - 1], .byte_count = 1});
        if (status == SXML_STATUS_OK && writing.is_grouped && digit_index > 1 &&
            (digit_index - 1) % 3 == 0) {
            status = sxml_format_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL(","));
        }
    }
    return status;
}

/* Runs the rules into text; *is_written false when they cannot format
   the value (no rule, too deep, too long). */
static sxml_status run_rules(rules_run run, sxml_format_text *text, bool *is_written) {
    *is_written = false;
    const sxml_spell_out_rules *rules = run.rules;
    run_frame frames[SXML_SPELL_OUT_MAXIMUM_DEPTH];
    size_t frame_count = 0;
    const spell_out_rule *first = select_rule(rules, run.rule_set_index, run.value);
    if (first == NULL) {
        return SXML_STATUS_OK;
    }
    frames[0] = (run_frame){.rule_set_index = run.rule_set_index,
                            .value = run.value,
                            .rule = first,
                            .next_part = 0};
    frame_count = 1;
    sxml_status status = SXML_STATUS_OK;
    size_t step_count = 0;
    while (status == SXML_STATUS_OK && frame_count > 0) {
        step_count += 1;
        if (step_count > SXML_SPELL_OUT_MAXIMUM_STEP_COUNT) {
            return SXML_STATUS_OK;
        }
        run_frame *frame = &frames[frame_count - 1];
        if (frame->next_part == frame->rule->part_count) {
            frame_count -= 1;
            continue;
        }
        const rule_part part = rules->parts[frame->rule->first_part + frame->next_part];
        frame->next_part += 1;
        if (part.part_kind == SXML_PART_KIND_TEXT) {
            status = sxml_format_text_append(text, part.text);
            continue;
        }
        if (part.part_kind == SXML_PART_KIND_PLURAL) {
            status = sxml_format_text_append(
                text, plural_text(rules, part, frame->value / frame->rule->divisor));
            continue;
        }
        uint64_t value = frame->value;
        if (part.part_kind == SXML_PART_KIND_MODULUS) {
            value = frame->value % frame->rule->divisor;
        } else if (part.part_kind == SXML_PART_KIND_QUOTIENT) {
            value = frame->value / frame->rule->divisor;
        }
        if (part.is_decimal_pattern) {
            if (!run.is_suffix_only) {
                status = append_decimal(
                    text, (decimal_writing){.value = value,
                                            .is_grouped = memchr(part.text.bytes, ',',
                                                                 part.text.byte_count) != NULL});
            }
            continue;
        }
        const size_t set_index = part.rule_set_index == SXML_SPELL_OUT_SAME_RULE_SET
                                     ? frame->rule_set_index
                                     : part.rule_set_index;
        const spell_out_rule *rule = select_rule(rules, set_index, value);
        if (rule == NULL || frame_count == SXML_SPELL_OUT_MAXIMUM_DEPTH ||
            (part.part_kind == SXML_PART_KIND_SAME_VALUE && set_index == frame->rule_set_index)) {
            return SXML_STATUS_OK;
        }
        frames[frame_count] = (run_frame){
            .rule_set_index = set_index, .value = value, .rule = rule, .next_part = 0};
        frame_count += 1;
    }
    *is_written = status == SXML_STATUS_OK;
    return status;
}

/* ------------------------------------------------- Choosing rule sets */

/* The public rule set called name, or SXML_SPELL_OUT_SAME_RULE_SET. */
static size_t find_public(const sxml_spell_out_rules *rules, sxml_byte_view name) {
    const size_t found = find_rule_set(rules, name);
    return found != SXML_SPELL_OUT_SAME_RULE_SET && !rules->rule_sets[found].is_private
               ? found
               : SXML_SPELL_OUT_SAME_RULE_SET;
}

/* True when name is family or family "-" something. */
static bool is_of_family(sxml_byte_view name, sxml_byte_view family) {
    return starts_with(name, family) &&
           (name.byte_count == family.byte_count || name.bytes[family.byte_count] == '-');
}

/* The default rule set of a family (the module comment). */
static size_t default_rule_set(const sxml_spell_out_rules *rules, sxml_byte_view family) {
    size_t found = find_public(rules, family);
    if (found == SXML_SPELL_OUT_SAME_RULE_SET && view_is(family, "spellout-cardinal")) {
        found = find_public(rules, SXML_BYTE_VIEW_FROM_LITERAL("spellout-numbering"));
    }
    for (size_t set_index = 0; found == SXML_SPELL_OUT_SAME_RULE_SET && set_index < rules->rule_set_count;
         set_index += 1) {
        const spell_out_rule_set *set = &rules->rule_sets[set_index];
        if (!set->is_private && is_of_family(set->name, family) &&
            ends_with(set->name, SXML_BYTE_VIEW_FROM_LITERAL("-masculine"))) {
            found = set_index;
        }
    }
    for (size_t set_index = 0; found == SXML_SPELL_OUT_SAME_RULE_SET && set_index < rules->rule_set_count;
         set_index += 1) {
        const spell_out_rule_set *set = &rules->rule_sets[set_index];
        if (!set->is_private && is_of_family(set->name, family)) {
            found = set_index;
        }
    }
    return found;
}

/* Applies the case of the numbering kind to words, appending them to
   text. */
typedef struct cased_words {
    sxml_byte_view words;
    sxml_numbering_kind numbering_kind;
} cased_words;

static sxml_status append_cased(cased_words cased, sxml_format_text *text) {
    if (cased.numbering_kind != SXML_NUMBERING_KIND_UPPER_WORDS &&
        cased.numbering_kind != SXML_NUMBERING_KIND_TITLE_WORDS) {
        return sxml_format_text_append(text, cased.words);
    }
    sxml_status status = SXML_STATUS_OK;
    size_t offset = 0;
    bool is_word_start = true;
    while (status == SXML_STATUS_OK && offset < cased.words.byte_count) {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(cased.words, offset);
        offset += decoded.encoded_byte_count;
        const bool maps = cased.numbering_kind == SXML_NUMBERING_KIND_UPPER_WORDS || is_word_start;
        is_word_start = decoded.codepoint == ' ';
        if (!maps) {
            status = sxml_format_text_append_codepoint(text, decoded.codepoint);
            continue;
        }
        sxml_case_mapping mapping;
        sxml_function_library_map_case(
            (sxml_case_mapping_request){.codepoint = decoded.codepoint,
                                        .mapping_kind = SXML_CASE_MAPPING_KIND_UPPER},
            &mapping);
        for (size_t mapped_index = 0; status == SXML_STATUS_OK && mapped_index < mapping.codepoint_count;
             mapped_index += 1) {
            status = sxml_format_text_append_codepoint(text, mapping.codepoints[mapped_index]);
        }
    }
    return status;
}

/* Runs one rule set into scratch text. */
typedef struct candidate_run {
    const sxml_spell_out_rules *rules;
    size_t rule_set_index;
    uint64_t value;
    bool is_suffix_only;
} candidate_run;

static sxml_status run_candidate(candidate_run candidate, sxml_format_text *scratch,
                                 bool *is_written) {
    scratch->byte_count = 0;
    return run_rules((rules_run){.rules = candidate.rules,
                                 .rule_set_index = candidate.rule_set_index,
                                 .value = candidate.value,
                                 .is_suffix_only = candidate.is_suffix_only},
                     scratch, is_written);
}

static sxml_status append_spell_out_words(const void *state,
                                          const sxml_number_words_request *request,
                                          sxml_number_words_outcome *outcome) {
    const sxml_spell_out_rules *rules = state;
    outcome->is_written = false;
    uint64_t value = 0;
    if (request->magnitude_digits.byte_count > SXML_SPELL_OUT_MAXIMUM_DIGIT_COUNT ||
        (request->magnitude_digits.byte_count > 0 &&
         !read_unsigned(request->magnitude_digits, &value))) {
        return SXML_STATUS_OK;
    }
    const bool is_digits = request->numbering_kind == SXML_NUMBERING_KIND_DIGITS;
    sxml_byte_view family = is_digits ? SXML_BYTE_VIEW_FROM_LITERAL("digits-ordinal")
                            : request->is_ordinal ? SXML_BYTE_VIEW_FROM_LITERAL("spellout-ordinal")
                                                  : SXML_BYTE_VIEW_FROM_LITERAL("spellout-cardinal");
    const sxml_byte_view variant = request->numbering_variant;
    size_t chosen = SXML_SPELL_OUT_SAME_RULE_SET;
    sxml_byte_view ending = {.bytes = NULL, .byte_count = 0};
    if (variant.byte_count > 0 && variant.bytes[0] == '%') {
        sxml_byte_view name = variant;
        while (name.byte_count > 0 && name.bytes[0] == '%') {
            name = after(name, 1);
        }
        if (!is_digits) {
            chosen = find_public(rules, name);
            if (chosen == SXML_SPELL_OUT_SAME_RULE_SET) {
                /* A rule set the language lacks: its default of the kind
                   the name says. */
                bool has_ordinal = false;
                const sxml_byte_view ordinal = SXML_BYTE_VIEW_FROM_LITERAL("ordinal");
                for (size_t offset = 0; offset + ordinal.byte_count <= name.byte_count; offset += 1) {
                    has_ordinal = has_ordinal || memcmp(name.bytes + offset, ordinal.bytes,
                                                        ordinal.byte_count) == 0;
                }
                family = has_ordinal ? SXML_BYTE_VIEW_FROM_LITERAL("spellout-ordinal")
                                     : SXML_BYTE_VIEW_FROM_LITERAL("spellout-cardinal");
            }
        }
    } else if (variant.byte_count > 1 && variant.bytes[0] == '-') {
        ending = after(variant, 1);
    }
    sxml_format_text scratch = {.allocator = request->text->allocator};
    sxml_status status = SXML_STATUS_OK;
    bool is_written = false;
    if (chosen == SXML_SPELL_OUT_SAME_RULE_SET && ending.byte_count > 0) {
        /* The first rule set of the family, the default first, whose
           words end with the ending. */
        const size_t default_index = default_rule_set(rules, family);
        for (size_t candidate_index = 0;
             status == SXML_STATUS_OK && chosen == SXML_SPELL_OUT_SAME_RULE_SET &&
             candidate_index <= rules->rule_set_count;
             candidate_index += 1) {
            const size_t set_index = candidate_index == 0 ? default_index : candidate_index - 1;
            if (set_index == SXML_SPELL_OUT_SAME_RULE_SET ||
                (candidate_index > 0 && set_index == default_index) ||
                rules->rule_sets[set_index].is_private ||
                !is_of_family(rules->rule_sets[set_index].name, family)) {
                continue;
            }
            status = run_candidate((candidate_run){.rules = rules,
                                                   .rule_set_index = set_index,
                                                   .value = value,
                                                   .is_suffix_only = is_digits},
                                   &scratch, &is_written);
            if (status == SXML_STATUS_OK && is_written &&
                ends_with(sxml_format_text_view(&scratch), ending)) {
                chosen = set_index;
            }
        }
    }
    if (chosen == SXML_SPELL_OUT_SAME_RULE_SET) {
        chosen = default_rule_set(rules, family);
    }
    if (chosen == SXML_SPELL_OUT_SAME_RULE_SET && is_digits) {
        /* A language whose rules have no ordinal suffixes (German): none,
           rather than an English one. */
        outcome->is_written = true;
        sxml_format_text_release(&scratch);
        return status;
    }
    if (status == SXML_STATUS_OK && chosen != SXML_SPELL_OUT_SAME_RULE_SET) {
        status = run_candidate((candidate_run){.rules = rules,
                                               .rule_set_index = chosen,
                                               .value = value,
                                               .is_suffix_only = is_digits},
                               &scratch, &is_written);
        if (status == SXML_STATUS_OK && is_written) {
            status = append_cased((cased_words){.words = sxml_format_text_view(&scratch),
                                                .numbering_kind = request->numbering_kind},
                                  request->text);
            outcome->is_written = status == SXML_STATUS_OK;
        }
    }
    sxml_format_text_release(&scratch);
    return status;
}

const sxml_number_words_operations sxml_spell_out_number_words_operations = {
    .append_words = append_spell_out_words,
};
