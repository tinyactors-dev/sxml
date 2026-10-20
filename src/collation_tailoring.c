/*
 * Tailorings of the UCA collation feature (src/collation_uca_internal.h;
 * glossary `tailoring`; packet 7.15b): the CLDR collation of a language,
 * read from the CLDR files the host serves, compiled into a table over
 * the root table, with its settings and its reordering (UTS #35, Part 5,
 * "Collation"; F&O 4.0, section 5.2.4, the lang and reorder parameters).
 *
 * Golden file: src/collation_uca.c (feature data read once into the
 * processor's slot, sorted entry arrays, explicit loops), src/timezone.c
 * (feature data named by a path under a URN). The language tags, the
 * locale chains and the reading of LDML files are the shared CLDR module
 * (src/cldr_internal.h, packet 7.17), which the localization feature
 * uses too.
 *
 * The data. A language's collations are in CLDR's common/collation/
 * <locale>.xml, which the feature asks the resolver for as
 * "urn:sxml:collation-data:collation/<locale>.xml" (an LDML document,
 * UTS #35, Part 5, section 2: ldml/collations/collation[@type]/cr holds
 * the rules, ldml/collations/defaultCollation the default type). The
 * locale of lang=de-AT is looked up as de_AT, then de, then root (CLDR's
 * inheritance by truncation); the collation type is "standard", the
 * file's defaultCollation, or the "-u-co-" keyword of the tag (de-u-co-
 * phonebk is the type phonebook). A language whose files have no
 * collation of the default type uses the root order (CLDR says "the root
 * collation order is valid for this language"); a language without any
 * file, or a type no file has, is not honoured.
 *
 * The rules (UTS #35, Part 5, sections 3.3 to 3.12): settings in square
 * brackets ([strength 2], [backwards 2], [caseFirst upper], [caseLevel
 * on], [alternate shifted], [maxVariable punct], [numericOrdering on],
 * [normalization on], [reorder Grek digit], [import de-u-co-phonebk]),
 * then resets (&X) and relations (< << <<< =, and their starred lists
 * with ranges), each relation optionally with an extension (/Y). Strings
 * are put into NFD; quotes and \u, \U and \x escapes are read; # starts
 * a comment. sxml does not support [before 2] and [before 3] resets
 * ([before 1] resets are, 3.7), special reset
 * positions ([first variable] and the like), prefixes (x|y), quaternary
 * relations, [suppressContractions] and [optimize]: rules with them are
 * not honoured, so the language falls back to the root order.
 *
 * How the weights are made. Each relation puts a new element after the
 * last element of the reset position, at its strength: the elements
 * tailored after one root element form a list, and a new one goes after
 * the reset's element, past the elements of weaker strength already
 * there (so &a<b then &a<<c gives a << c < b). When all rules are read,
 * the lists are walked: a primary relation adds one to the primary
 * offset and takes the common secondary and tertiary, a secondary
 * relation adds one to the secondary offset, a tertiary one to the
 * tertiary offset (src/collation_uca_internal.h: weights are the root
 * weight shifted left by 8 bits plus the offset, so the new weights sort
 * after the root element and before the next root weight). The case of
 * a tailored element is that of the first character of its string with a
 * primary weight (UTS #35, Part 5, section 3.14).
 *
 * No recursion (D71): imports are read with an explicit stack of rule
 * texts. Everything is in the processor's allocator; the tables live in
 * the feature's slot (D35).
 */
#include "collation_uca_internal.h"

#include "cldr_internal.h"
#include "memory_internal.h"
#include "optional_feature_internal.h"
#include "processor_internal.h"
#include "unicode_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- Limits */

/* The longest collation type ("phonebook"). */
#define SXML_TAILORING_MAXIMUM_TYPE_BYTE_COUNT 32U
/* The deepest nesting of [import]s, and the most imports in all. */
#define SXML_TAILORING_MAXIMUM_IMPORT_DEPTH 8U
#define SXML_TAILORING_MAXIMUM_IMPORT_COUNT 32U
/* The most elements of a reset position or a relation's string. */
#define SXML_TAILORING_MAXIMUM_POSITION_ELEMENT_COUNT 32U
/* The longest string of a rule, in codepoints after NFD. */
#define SXML_TAILORING_MAXIMUM_STRING_LENGTH SXML_UCA_MAXIMUM_CONTRACTION_LENGTH
/* The longest [reorder] text kept. */
#define SXML_TAILORING_MAXIMUM_REORDER_BYTE_COUNT 256U

/* No node: a list's end, or an element of the root. */
#define SXML_TAILORING_NO_NODE 0xFFFFFFFFU

/* ---------------------------------------------------- Growable arrays */

/* An array in allocator memory that grows by doubling. */
typedef struct growable_array {
    sxml_allocator allocator;
    void *items;
    size_t item_count;
    size_t item_capacity;
    size_t item_size;
    size_t item_alignment;
} growable_array;

/* Makes room for one more item and writes its address to *slot. */
static sxml_status grow_array(growable_array *array, void **slot) {
    const sxml_status status = sxml_allocator_reserve_array(
        array->allocator, &(sxml_array_reservation_request){.elements = &array->items,
                                                            .capacity = &array->item_capacity,
                                                            .element_count = array->item_count,
                                                            .required_count = array->item_count + 1,
                                                            .element_size = array->item_size,
                                                            .element_alignment = array->item_alignment,
                                                            .minimum_capacity = 16});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *slot = (uint8_t *)array->items + (array->item_count * array->item_size);
    array->item_count += 1;
    return SXML_STATUS_OK;
}

static void release_array(growable_array *array) {
    sxml_allocator_release(array->allocator, array->items);
    array->items = NULL;
    array->item_count = 0;
    array->item_capacity = 0;
}

/* Appends bytes to an array of bytes. */
static sxml_status append_bytes(growable_array *array, sxml_byte_view bytes) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index < bytes.byte_count;
         byte_index += 1) {
        void *slot = NULL;
        status = grow_array(array, &slot);
        if (status == SXML_STATUS_OK) {
            *(uint8_t *)slot = bytes.bytes[byte_index];
        }
    }
    return status;
}

static uint8_t lower_ascii(uint8_t byte) {
    return byte >= 'A' && byte <= 'Z' ? (uint8_t)(byte + 0x20U) : byte;
}

static bool view_is(sxml_byte_view view, const char *text) {
    return sxml_byte_view_equals(view, sxml_byte_view_from_c_string(text));
}

/* The LDML type of a BCP 47 "co" value (UTS #35, Part 1, section 3.6.4,
   the collation keyword's aliases). */
static sxml_byte_view collation_type_name(sxml_byte_view value) {
    static const char *const aliases[][2] = {
        {"phonebk", "phonebook"},
        {"trad", "traditional"},
        {"dict", "dictionary"},
        {"gb2312", "gb2312han"},
    };
    for (size_t alias_index = 0; alias_index < sizeof(aliases) / sizeof(aliases[0]);
         alias_index += 1) {
        if (view_is(value, aliases[alias_index][0])) {
            return sxml_byte_view_from_c_string(aliases[alias_index][1]);
        }
    }
    return value;
}

/* ------------------------------------------------- Reading collations */

/* What one locale file gave. */
typedef struct locale_rules {
    bool is_served;
    bool has_rules;
    /* The rules text, in allocator memory (rules.items). */
    growable_array rules;
    uint8_t default_type[SXML_TAILORING_MAXIMUM_TYPE_BYTE_COUNT];
    size_t default_type_byte_count;
    bool has_default_type;
} locale_rules;

/* A locale file read (src/cldr_internal.h) and the processor it was read
   for. */
typedef struct locale_file {
    sxml_processor *processor;
    sxml_cldr_file file;
} locale_file;

/* Reads collation/<name>.xml, keeping ldml/collations only. */
static sxml_status read_locale_file(const sxml_cldr_locale_name *name, locale_file *read) {
    read->file = (sxml_cldr_file){.allocator = read->processor->allocator};
    sxml_cldr_uri uri;
    if (!sxml_cldr_make_uri(
            &(sxml_cldr_uri_parts){
                .uri_prefix = SXML_BYTE_VIEW_FROM_LITERAL("urn:sxml:collation-data:"),
                .directory = SXML_BYTE_VIEW_FROM_LITERAL("collation/"),
                .file_name = {.bytes = name->bytes, .byte_count = name->byte_count}},
            &uri)) {
        return SXML_STATUS_OK;
    }
    static const sxml_byte_view kept_paths[] = {SXML_BYTE_VIEW_INITIALIZER("collations")};
    return sxml_cldr_file_read(
        &(sxml_cldr_file_request){
            .processor = read->processor,
            .resource_kind = SXML_RESOURCE_KIND_COLLATION_DATA,
            .uri = {.bytes = uri.bytes, .byte_count = uri.byte_count},
            .kept_paths = {.prefixes = kept_paths, .prefix_count = 1}},
        &read->file);
}

/* The default collation type of a file (ldml/collations/defaultCollation,
   without whitespace). */
static void read_default_type(const sxml_cldr_file *file, locale_rules *rules) {
    const sxml_cldr_entry *entry =
        sxml_cldr_file_find(file, SXML_BYTE_VIEW_FROM_LITERAL("collations/defaultCollation"));
    rules->has_default_type = entry != NULL;
    rules->default_type_byte_count = 0;
    for (size_t byte_index = 0; entry != NULL && byte_index < entry->value.byte_count;
         byte_index += 1) {
        const uint8_t byte = entry->value.bytes[byte_index];
        if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r') {
            continue;
        }
        if (rules->default_type_byte_count == sizeof(rules->default_type)) {
            rules->has_default_type = false;
            break;
        }
        rules->default_type[rules->default_type_byte_count] = byte;
        rules->default_type_byte_count += 1;
    }
    if (rules->default_type_byte_count == 0) {
        rules->has_default_type = false;
    }
}

/* The rules of a collation type of a file (ldml/collations/collation
   [@type]/cr; a collation with an alt attribute has another path), copied
   into rules->rules. */
typedef struct rules_query {
    const sxml_cldr_file *file;
    sxml_byte_view collation_type;
} rules_query;

static sxml_status read_type_rules(rules_query query, locale_rules *rules) {
    uint8_t path_bytes[SXML_TAILORING_MAXIMUM_TYPE_BYTE_COUNT + 48];
    static const char head[] = "collations/collation[@type='";
    static const char tail[] = "']/cr";
    const size_t head_byte_count = sizeof(head) - 1;
    const size_t tail_byte_count = sizeof(tail) - 1;
    rules->has_rules = false;
    if (query.collation_type.byte_count > SXML_TAILORING_MAXIMUM_TYPE_BYTE_COUNT) {
        return SXML_STATUS_OK;
    }
    memcpy(path_bytes, head, head_byte_count);
    memcpy(path_bytes + head_byte_count, query.collation_type.bytes,
           query.collation_type.byte_count);
    memcpy(path_bytes + head_byte_count + query.collation_type.byte_count, tail, tail_byte_count);
    const sxml_cldr_entry *entry = sxml_cldr_file_find(
        query.file,
        (sxml_byte_view){.bytes = path_bytes,
                         .byte_count = head_byte_count + query.collation_type.byte_count +
                                       tail_byte_count});
    if (entry == NULL) {
        return SXML_STATUS_OK;
    }
    rules->rules.item_count = 0;
    const sxml_status status = append_bytes(&rules->rules, entry->value);
    rules->has_rules = status == SXML_STATUS_OK;
    return status;
}

/* The rules of a language tag's collation (the module comment). */
typedef struct tag_rules {
    bool is_honoured;
    /* The rules text, in allocator memory; empty for the root order. */
    growable_array rules;
} tag_rules;

typedef struct tag_request {
    sxml_processor *processor;
    /* Lower case. */
    sxml_byte_view tag_text;
} tag_request;

static sxml_status find_tag_rules(const tag_request *request, tag_rules *found) {
    sxml_processor *processor = request->processor;
    found->is_honoured = false;
    found->rules = (growable_array){.allocator = processor->allocator,
                                    .item_size = 1,
                                    .item_alignment = 1};
    sxml_cldr_language_tag tag;
    sxml_cldr_read_language_tag(request->tag_text, &tag);
    sxml_cldr_locale_chain chain;
    /* Keywords and extensions other than "-u-co-" are not honoured. */
    if (!tag.is_valid || tag.has_other_extension || !sxml_cldr_make_locale_chain(&tag, &chain)) {
        return SXML_STATUS_OK;
    }
    const bool is_explicit_type = tag.collation_type.byte_count > 0;
    uint8_t type_bytes[SXML_TAILORING_MAXIMUM_TYPE_BYTE_COUNT];
    sxml_byte_view collation_type = is_explicit_type ? collation_type_name(tag.collation_type)
                                                     : sxml_byte_view_from_c_string("standard");
    bool has_chosen_type = is_explicit_type;
    bool is_language_served = false;
    const bool is_root_tag = chain.name_count == 1;
    locale_rules rules;
    memset(&rules, 0, sizeof(rules));
    rules.rules = found->rules;
    /* de_AT, de, then root. */
    sxml_status status = SXML_STATUS_OK;
    for (size_t name_index = 0; status == SXML_STATUS_OK && name_index < chain.name_count;
         name_index += 1) {
        const bool is_root_level = sxml_cldr_locale_is_root(&chain.names[name_index]);
        locale_file read = {.processor = processor};
        status = read_locale_file(&chain.names[name_index], &read);
        if (status != SXML_STATUS_OK || !read.file.is_served) {
            sxml_cldr_file_release(&read.file);
            continue;
        }
        if (!is_root_level || is_root_tag) {
            is_language_served = true;
        }
        if (!has_chosen_type) {
            /* The first file served decides the type: its
               defaultCollation, else "standard". */
            read_default_type(&read.file, &rules);
            if (rules.has_default_type) {
                memcpy(type_bytes, rules.default_type, rules.default_type_byte_count);
                collation_type = (sxml_byte_view){.bytes = type_bytes,
                                                  .byte_count = rules.default_type_byte_count};
            }
            has_chosen_type = true;
        }
        status = read_type_rules((rules_query){.file = &read.file, .collation_type = collation_type},
                                 &rules);
        sxml_cldr_file_release(&read.file);
        if (status == SXML_STATUS_OK && rules.has_rules) {
            /* root's types count only for a language CLDR has. */
            found->rules = rules.rules;
            found->is_honoured = is_language_served;
            return SXML_STATUS_OK;
        }
    }
    found->rules = rules.rules;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* No file has the type: the root order for the standard type of a
       language CLDR has. */
    found->is_honoured = (is_language_served || is_root_tag) && view_is(collation_type, "standard");
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- The builder */

/* The strengths of relations, in order. */
typedef enum relation_strength {
    SXML_TAILORING_STRENGTH_PRIMARY = 1,
    SXML_TAILORING_STRENGTH_SECONDARY = 2,
    SXML_TAILORING_STRENGTH_TERTIARY = 3,
    SXML_TAILORING_STRENGTH_IDENTICAL = 4,
} relation_strength;

/* An element of a tailored string: a root element, or a node. */
typedef struct element_reference {
    sxml_uca_element element;
    uint32_t node;
} element_reference;

/* A string the rules tailor. */
typedef struct tailored_entry {
    uint32_t codepoints[SXML_TAILORING_MAXIMUM_STRING_LENGTH];
    uint32_t codepoint_count;
    /* Its elements, in the builder's references. */
    uint32_t first_reference;
    uint32_t reference_count;
} tailored_entry;

/* A tailored element in the list after its anchor. */
typedef struct tailoring_node {
    uint32_t anchor;
    uint32_t next;
    relation_strength strength;
    bool is_upper;
    /* Made when the rules are read. */
    sxml_uca_element element;
} tailoring_node;

/* A root element that tailored elements follow. */
typedef struct node_anchor {
    sxml_uca_element element;
    uint32_t first_node;
} node_anchor;

/* One rules text being read: its codepoints and where. */
typedef struct rules_frame {
    uint32_t *codepoints;
    size_t codepoint_count;
    size_t position;
} rules_frame;

typedef struct tailoring_builder {
    sxml_processor *processor;
    sxml_allocator allocator;
    const sxml_collation_element_table *root;
    growable_array entries;
    growable_array references;
    growable_array nodes;
    growable_array anchors;
    sxml_collation settings;
    uint8_t reorder_codes[SXML_TAILORING_MAXIMUM_REORDER_BYTE_COUNT];
    size_t reorder_code_byte_count;
    bool has_reorder;
    /* False once the rules use something sxml does not support. */
    bool is_supported;
    /* The reset position: the elements of the reset or of the last
       relation's string. */
    element_reference position[SXML_TAILORING_MAXIMUM_POSITION_ELEMENT_COUNT];
    size_t position_count;
    bool has_position;
    rules_frame frames[SXML_TAILORING_MAXIMUM_IMPORT_DEPTH];
    size_t frame_count;
    size_t import_count;
} tailoring_builder;

static void release_builder(tailoring_builder *builder) {
    release_array(&builder->entries);
    release_array(&builder->references);
    release_array(&builder->nodes);
    release_array(&builder->anchors);
    for (size_t frame_index = 0; frame_index < builder->frame_count; frame_index += 1) {
        sxml_allocator_release(builder->allocator, builder->frames[frame_index].codepoints);
    }
    builder->frame_count = 0;
}

/*
 * Replaces the escapes \uXXXX and \UXXXXXXXX of a rules text by their
 * characters, also between quotes, as CLDR's rules are read by ICU's
 * resource compiler before its rule parser sees them (en_US_POSIX quotes
 * '\u0020'). Other backslashes are left to the rule syntax.
 */
static int escape_digit_value(uint32_t codepoint) {
    if (codepoint >= '0' && codepoint <= '9') {
        return (int)(codepoint - '0');
    }
    if (codepoint >= 'a' && codepoint <= 'f') {
        return (int)(codepoint - 'a' + 10);
    }
    if (codepoint >= 'A' && codepoint <= 'F') {
        return (int)(codepoint - 'A' + 10);
    }
    return -1;
}

static void unescape_codepoints(rules_frame *frame) {
    size_t written = 0;
    size_t position = 0;
    while (position < frame->codepoint_count) {
        const uint32_t codepoint = frame->codepoints[position];
        size_t digit_count = 0;
        if (codepoint == '\\' && position + 1 < frame->codepoint_count) {
            const uint32_t letter = frame->codepoints[position + 1];
            digit_count = letter == 'u' ? 4 : letter == 'U' ? 8 : 0;
        }
        uint32_t value = 0;
        bool is_escape = digit_count > 0 && position + 2 + digit_count <= frame->codepoint_count;
        for (size_t digit_index = 0; is_escape && digit_index < digit_count; digit_index += 1) {
            const int digit = escape_digit_value(frame->codepoints[position + 2 + digit_index]);
            is_escape = digit >= 0;
            value = (value * 16U) + (uint32_t)(digit < 0 ? 0 : digit);
        }
        if (is_escape && value <= 0x10FFFFU) {
            frame->codepoints[written] = value;
            position += 2 + digit_count;
        } else {
            frame->codepoints[written] = codepoint;
            position += 1;
        }
        written += 1;
    }
    frame->codepoint_count = written;
}

/* Pushes the rules text text (UTF-8) as a new frame. */
static sxml_status push_rules(tailoring_builder *builder, sxml_byte_view text) {
    if (builder->frame_count == SXML_TAILORING_MAXIMUM_IMPORT_DEPTH ||
        builder->import_count == SXML_TAILORING_MAXIMUM_IMPORT_COUNT) {
        builder->is_supported = false;
        return SXML_STATUS_OK;
    }
    builder->import_count += 1;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        builder->allocator,
        (sxml_array_allocation_request){.element_count = text.byte_count == 0 ? 1 : text.byte_count,
                                 .element_size = sizeof(uint32_t),
                                 .element_alignment = alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    rules_frame *frame = &builder->frames[builder->frame_count];
    frame->codepoints = memory;
    frame->codepoint_count = 0;
    frame->position = 0;
    builder->frame_count += 1;
    sxml_codepoint_iterator iterator = {
        .string = {.utf8 = text, .codepoint_count = 0, .is_ascii = false}, .byte_offset = 0};
    uint32_t codepoint = 0;
    while (sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        frame->codepoints[frame->codepoint_count] = codepoint;
        frame->codepoint_count += 1;
    }
    unescape_codepoints(frame);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ NFD and lookup */

/* A string of a rule, in NFD. */
typedef struct rule_string {
    uint32_t codepoints[SXML_TAILORING_MAXIMUM_STRING_LENGTH];
    size_t codepoint_count;
    bool is_too_long;
} rule_string;

/* Appends codepoint's canonical decomposition to text, then puts the
   combining marks in canonical order (UAX #15). */
static void append_decomposed(rule_string *text, uint32_t codepoint) {
    sxml_unicode_decomposition decomposition;
    sxml_unicode_decompose(
        (sxml_unicode_decomposition_request){
            .codepoint = codepoint, .decomposition_kind = SXML_UNICODE_DECOMPOSITION_KIND_CANONICAL},
        &decomposition);
    for (size_t part_index = 0; part_index < decomposition.codepoint_count; part_index += 1) {
        if (text->codepoint_count == SXML_TAILORING_MAXIMUM_STRING_LENGTH) {
            text->is_too_long = true;
            return;
        }
        text->codepoints[text->codepoint_count] = decomposition.codepoints[part_index];
        text->codepoint_count += 1;
        size_t position = text->codepoint_count - 1;
        while (position > 0) {
            const uint8_t combining_class =
                sxml_unicode_get_combining_class(text->codepoints[position]);
            if (combining_class == 0 ||
                sxml_unicode_get_combining_class(text->codepoints[position - 1]) <= combining_class) {
                break;
            }
            const uint32_t moved = text->codepoints[position];
            text->codepoints[position] = text->codepoints[position - 1];
            text->codepoints[position - 1] = moved;
            position -= 1;
        }
    }
}

/* The elements of a string, as references. */
typedef struct reference_list {
    element_reference references[SXML_TAILORING_MAXIMUM_POSITION_ELEMENT_COUNT];
    size_t reference_count;
    bool is_too_long;
} reference_list;

static void add_reference(reference_list *list, element_reference reference) {
    if (list->reference_count == SXML_TAILORING_MAXIMUM_POSITION_ELEMENT_COUNT) {
        list->is_too_long = true;
        return;
    }
    list->references[list->reference_count] = reference;
    list->reference_count += 1;
}

static void add_entry_references(reference_list *list, sxml_uca_entry entry) {
    for (uint32_t element_index = 0; element_index < entry.element_count; element_index += 1) {
        add_reference(list, (element_reference){.element = entry.elements[element_index],
                                                .node = SXML_TAILORING_NO_NODE});
    }
}

/*
 * The elements of text under the rules read so far: at each position the
 * longest tailored string or root entry that matches (contiguously),
 * else implicit weights. With only_root, the root's elements alone.
 */
typedef struct string_lookup {
    const tailoring_builder *builder;
    const rule_string *text;
    bool is_root_only;
} string_lookup;

static void find_string_elements(string_lookup lookup, reference_list *list) {
    const tailoring_builder *builder = lookup.builder;
    const rule_string *text = lookup.text;
    list->reference_count = 0;
    list->is_too_long = false;
    size_t position = 0;
    while (position < text->codepoint_count) {
        const size_t remaining = text->codepoint_count - position;
        /* The root: the longest contraction, else the entry. */
        size_t root_length = 0;
        sxml_uca_entry root_entry = {.elements = NULL, .element_count = 0};
        for (size_t length = remaining; length >= 2 && root_length == 0; length -= 1) {
            if (sxml_uca_find_contraction(
                    builder->root,
                    (sxml_uca_codepoints){.codepoints = text->codepoints + position,
                                          .codepoint_count = length},
                    &root_entry)) {
                root_length = length;
            }
        }
        if (root_length == 0 &&
            sxml_uca_find_mapping(builder->root, text->codepoints[position], &root_entry)) {
            root_length = 1;
        }
        /* The tailored strings. */
        size_t tailored_length = 0;
        const tailored_entry *tailored = NULL;
        const tailored_entry *entries = builder->entries.items;
        for (size_t entry_index = 0; !lookup.is_root_only && entry_index < builder->entries.item_count;
             entry_index += 1) {
            const tailored_entry *entry = &entries[entry_index];
            if (entry->codepoint_count > remaining || entry->codepoint_count < tailored_length ||
                entry->codepoint_count == 0 ||
                memcmp(entry->codepoints, text->codepoints + position,
                       entry->codepoint_count * sizeof(uint32_t)) != 0) {
                continue;
            }
            tailored_length = entry->codepoint_count;
            tailored = entry;
        }
        if (tailored != NULL && tailored_length >= root_length) {
            const element_reference *references = builder->references.items;
            for (uint32_t reference_index = 0; reference_index < tailored->reference_count;
                 reference_index += 1) {
                add_reference(list, references[tailored->first_reference + reference_index]);
            }
            position += tailored_length;
        } else if (root_length > 0) {
            add_entry_references(list, root_entry);
            position += root_length;
        } else {
            sxml_uca_element implicit[2];
            sxml_uca_make_implicit_elements(builder->root, text->codepoints[position], implicit);
            add_entry_references(list, (sxml_uca_entry){.elements = implicit, .element_count = 2});
            position += 1;
        }
    }
}

/* Whether a string is upper case: its first element with a primary
   weight in the root (UTS #35, Part 5, section 3.14). */
static bool is_upper_string(const tailoring_builder *builder, const rule_string *text) {
    reference_list list;
    find_string_elements(
        (string_lookup){.builder = builder, .text = text, .is_root_only = true}, &list);
    for (size_t reference_index = 0; reference_index < list.reference_count;
         reference_index += 1) {
        const sxml_uca_element *element = &list.references[reference_index].element;
        if (element->primary != 0) {
            return (element->flags & SXML_UCA_ELEMENT_FLAG_UPPER) != 0;
        }
    }
    return false;
}

/* ------------------------------------------------------ Relations */

static bool same_root_element(const sxml_uca_element *first, const sxml_uca_element *second) {
    return first->primary == second->primary && first->secondary == second->secondary &&
           first->tertiary == second->tertiary && first->primary_offset == 0 &&
           second->primary_offset == 0 && first->secondary_offset == 0 &&
           second->secondary_offset == 0 && first->tertiary_offset == 0 &&
           second->tertiary_offset == 0;
}

/* A new node after reference at strength, in *node. */
typedef struct node_insertion {
    element_reference after;
    relation_strength strength;
    bool is_upper;
} node_insertion;

static sxml_status insert_node(tailoring_builder *builder, node_insertion insertion,
                               uint32_t *made) {
    uint32_t anchor_index = 0;
    /* The node before the new one, or NO_NODE for the anchor itself. */
    uint32_t previous = SXML_TAILORING_NO_NODE;
    if (insertion.after.node != SXML_TAILORING_NO_NODE) {
        const tailoring_node *nodes = builder->nodes.items;
        anchor_index = nodes[insertion.after.node].anchor;
        previous = insertion.after.node;
    } else {
        const node_anchor *anchors = builder->anchors.items;
        size_t found = builder->anchors.item_count;
        for (size_t index = 0; index < builder->anchors.item_count; index += 1) {
            if (same_root_element(&anchors[index].element, &insertion.after.element)) {
                found = index;
                break;
            }
        }
        if (found == builder->anchors.item_count) {
            void *slot = NULL;
            const sxml_status status = grow_array(&builder->anchors, &slot);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            *(node_anchor *)slot = (node_anchor){.element = insertion.after.element,
                                                 .first_node = SXML_TAILORING_NO_NODE};
        }
        anchor_index = (uint32_t)found;
    }
    void *slot = NULL;
    const sxml_status status = grow_array(&builder->nodes, &slot);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t node_index = (uint32_t)(builder->nodes.item_count - 1);
    tailoring_node *nodes = builder->nodes.items;
    node_anchor *anchors = builder->anchors.items;
    /* Past the nodes of weaker strength after the reset's element. */
    uint32_t next = previous == SXML_TAILORING_NO_NODE ? anchors[anchor_index].first_node
                                                       : nodes[previous].next;
    while (next != SXML_TAILORING_NO_NODE && nodes[next].strength > insertion.strength) {
        previous = next;
        next = nodes[next].next;
    }
    nodes[node_index] = (tailoring_node){.anchor = anchor_index,
                                         .next = next,
                                         .strength = insertion.strength,
                                         .is_upper = insertion.is_upper,
                                         .element = {0}};
    if (previous == SXML_TAILORING_NO_NODE) {
        anchors[anchor_index].first_node = node_index;
    } else {
        nodes[previous].next = node_index;
    }
    *made = node_index;
    return SXML_STATUS_OK;
}

/* Adds or replaces the entry of text with references. */
static sxml_status set_entry(tailoring_builder *builder, const rule_string *text,
                             const reference_list *list) {
    const uint32_t first_reference = (uint32_t)builder->references.item_count;
    for (size_t reference_index = 0; reference_index < list->reference_count;
         reference_index += 1) {
        void *slot = NULL;
        const sxml_status status = grow_array(&builder->references, &slot);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        *(element_reference *)slot = list->references[reference_index];
    }
    tailored_entry *entries = builder->entries.items;
    tailored_entry *entry = NULL;
    for (size_t entry_index = 0; entry_index < builder->entries.item_count; entry_index += 1) {
        if (entries[entry_index].codepoint_count == text->codepoint_count &&
            memcmp(entries[entry_index].codepoints, text->codepoints,
                   text->codepoint_count * sizeof(uint32_t)) == 0) {
            entry = &entries[entry_index];
            break;
        }
    }
    if (entry == NULL) {
        void *slot = NULL;
        const sxml_status status = grow_array(&builder->entries, &slot);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        entry = slot;
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->codepoints, text->codepoints, text->codepoint_count * sizeof(uint32_t));
        entry->codepoint_count = (uint32_t)text->codepoint_count;
    }
    entry->first_reference = first_reference;
    entry->reference_count = (uint32_t)list->reference_count;
    return SXML_STATUS_OK;
}

/* One relation: its strength, string and extension. */
typedef struct relation {
    relation_strength strength;
    const rule_string *text;
    const rule_string *extension;
} relation;

static sxml_status apply_relation(tailoring_builder *builder, const relation *applied) {
    if (!builder->has_position || builder->position_count == 0 || applied->text->is_too_long ||
        applied->text->codepoint_count == 0 ||
        (applied->extension != NULL && applied->extension->is_too_long)) {
        builder->is_supported = false;
        return SXML_STATUS_OK;
    }
    reference_list list;
    list.reference_count = 0;
    list.is_too_long = false;
    for (size_t position_index = 0; position_index + 1 < builder->position_count;
         position_index += 1) {
        add_reference(&list, builder->position[position_index]);
    }
    const element_reference last = builder->position[builder->position_count - 1];
    if (applied->strength == SXML_TAILORING_STRENGTH_IDENTICAL) {
        add_reference(&list, last);
    } else {
        uint32_t node = 0;
        const sxml_status status = insert_node(
            builder,
            (node_insertion){.after = last,
                             .strength = applied->strength,
                             .is_upper = is_upper_string(builder, applied->text)},
            &node);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        add_reference(&list, (element_reference){.element = {0}, .node = node});
    }
    /* The string is the position of the next relation, without the
       extension. */
    memcpy(builder->position, list.references, list.reference_count * sizeof(element_reference));
    builder->position_count = list.reference_count;
    if (applied->extension != NULL) {
        reference_list extension;
        find_string_elements(
            (string_lookup){.builder = builder, .text = applied->extension, .is_root_only = false},
            &extension);
        for (size_t reference_index = 0; reference_index < extension.reference_count;
             reference_index += 1) {
            add_reference(&list, extension.references[reference_index]);
        }
        list.is_too_long = list.is_too_long || extension.is_too_long;
    }
    if (list.is_too_long) {
        builder->is_supported = false;
        return SXML_STATUS_OK;
    }
    return set_entry(builder, applied->text, &list);
}

/* A reset: &X, or &[before 1]X (UTS #35, Part 5, section 3.7). */
typedef struct reset {
    const rule_string *text;
    bool is_before_primary;
} reset;

static void apply_reset(tailoring_builder *builder, const reset *applied) {
    const rule_string *text = applied->text;
    reference_list list;
    find_string_elements((string_lookup){.builder = builder, .text = text, .is_root_only = false},
                         &list);
    if (text->is_too_long || list.is_too_long || list.reference_count == 0) {
        builder->is_supported = false;
        return;
    }
    if (applied->is_before_primary) {
        /* 3.7: [before 1] resets to the position just before the last
           element at primary strength. Its root element's primary less
           one, with common secondary and tertiary weights, is that
           position: what is tailored after it sorts after every element
           of a smaller primary and before the reset's element. A
           tailored element there, or a primary of 1 or less, is not
           supported. */
        element_reference *last = &list.references[list.reference_count - 1];
        if (last->node != SXML_TAILORING_NO_NODE || last->element.primary <= 1 ||
            last->element.primary_offset != 0) {
            builder->is_supported = false;
            return;
        }
        last->element = (sxml_uca_element){.primary = (uint16_t)(last->element.primary - 1U),
                                           .secondary = 0x20U,
                                           .tertiary = 0x02U,
                                           .primary_offset = 0,
                                           .secondary_offset = 0,
                                           .tertiary_offset = 0,
                                           .flags = 0};
    }
    memcpy(builder->position, list.references, list.reference_count * sizeof(element_reference));
    builder->position_count = list.reference_count;
    builder->has_position = true;
}

/* -------------------------------------------------------- Rule syntax */

static rules_frame *current_frame(tailoring_builder *builder) {
    return &builder->frames[builder->frame_count - 1];
}

static bool is_frame_end(const rules_frame *frame) {
    return frame->position >= frame->codepoint_count;
}

static uint32_t peek(const rules_frame *frame) {
    return is_frame_end(frame) ? 0 : frame->codepoints[frame->position];
}

/* Pattern_White_Space (UAX #31). */
static bool is_rule_white_space(uint32_t codepoint) {
    return (codepoint >= 0x09U && codepoint <= 0x0DU) || codepoint == 0x20U ||
           codepoint == 0x85U || codepoint == 0x200EU || codepoint == 0x200FU ||
           codepoint == 0x2028U || codepoint == 0x2029U;
}

/* The ASCII characters that are syntax in rules unless quoted (UTS #35,
   Part 5, section 3.3: every ASCII character but letters and digits). */
static bool is_syntax_character(uint32_t codepoint) {
    return (codepoint >= 0x21U && codepoint <= 0x2FU) || (codepoint >= 0x3AU && codepoint <= 0x40U) ||
           (codepoint >= 0x5BU && codepoint <= 0x60U) || (codepoint >= 0x7BU && codepoint <= 0x7EU);
}

/* Skips white space and comments ("#" to the end of the line). */
static void skip_white_space(rules_frame *frame) {
    while (!is_frame_end(frame)) {
        const uint32_t codepoint = peek(frame);
        if (is_rule_white_space(codepoint)) {
            frame->position += 1;
        } else if (codepoint == '#') {
            while (!is_frame_end(frame) && peek(frame) != '\n' && peek(frame) != '\r') {
                frame->position += 1;
            }
        } else {
            return;
        }
    }
}

static int hex_value(uint32_t codepoint) {
    if (codepoint >= '0' && codepoint <= '9') {
        return (int)(codepoint - '0');
    }
    if (codepoint >= 'a' && codepoint <= 'f') {
        return (int)(codepoint - 'a' + 10);
    }
    if (codepoint >= 'A' && codepoint <= 'F') {
        return (int)(codepoint - 'A' + 10);
    }
    return -1;
}

/* Reads exactly digit_count hex digits (or, for 0, up to "}" after
   "\x{"); false when they are not there. */
static bool read_hex_digits(rules_frame *frame, size_t digit_count, uint32_t *value) {
    uint32_t number = 0;
    size_t read = 0;
    while ((digit_count == 0 ? peek(frame) != '}' : read < digit_count) && read < 8) {
        const int digit = hex_value(peek(frame));
        if (is_frame_end(frame) || digit < 0) {
            return false;
        }
        number = (number * 16U) + (uint32_t)digit;
        read += 1;
        frame->position += 1;
    }
    if (read == 0 || number > 0x10FFFFU) {
        return false;
    }
    if (digit_count == 0) {
        frame->position += 1;
    }
    *value = number;
    return true;
}

/* Reads one escaped character after "\". */
static bool read_escape(rules_frame *frame, uint32_t *codepoint) {
    if (is_frame_end(frame)) {
        return false;
    }
    const uint32_t letter = peek(frame);
    frame->position += 1;
    if (letter == 'u') {
        return read_hex_digits(frame, 4, codepoint);
    }
    if (letter == 'U') {
        return read_hex_digits(frame, 8, codepoint);
    }
    if (letter == 'x') {
        if (peek(frame) == '{') {
            frame->position += 1;
            return read_hex_digits(frame, 0, codepoint);
        }
        return read_hex_digits(frame, 2, codepoint);
    }
    if (letter == 't') {
        *codepoint = '\t';
    } else if (letter == 'n') {
        *codepoint = '\n';
    } else if (letter == 'r') {
        *codepoint = '\r';
    } else {
        *codepoint = letter;
    }
    return true;
}

/*
 * Reads the characters of one string (before NFD) into raw: letters and
 * other characters that are not syntax, quoted text, escapes. Stops at
 * white space and at a syntax character. False for a malformed quote or
 * escape.
 */
typedef struct raw_string {
    uint32_t codepoints[SXML_TAILORING_MAXIMUM_STRING_LENGTH * 4];
    size_t codepoint_count;
    bool is_too_long;
} raw_string;

static void add_raw(raw_string *raw, uint32_t codepoint) {
    if (raw->codepoint_count == sizeof(raw->codepoints) / sizeof(raw->codepoints[0])) {
        raw->is_too_long = true;
        return;
    }
    raw->codepoints[raw->codepoint_count] = codepoint;
    raw->codepoint_count += 1;
}

/* Reads one character of a string; *has_character false at its end. */
static bool read_string_character(rules_frame *frame, raw_string *raw, bool *has_character) {
    *has_character = false;
    const uint32_t codepoint = peek(frame);
    if (is_frame_end(frame) || is_rule_white_space(codepoint)) {
        return true;
    }
    if (codepoint == '\'') {
        frame->position += 1;
        if (peek(frame) == '\'') {
            frame->position += 1;
            add_raw(raw, '\'');
            *has_character = true;
            return true;
        }
        while (!is_frame_end(frame) && peek(frame) != '\'') {
            add_raw(raw, peek(frame));
            frame->position += 1;
        }
        if (is_frame_end(frame)) {
            return false;
        }
        frame->position += 1;
        *has_character = true;
        return true;
    }
    if (codepoint == '\\') {
        frame->position += 1;
        uint32_t escaped = 0;
        if (!read_escape(frame, &escaped)) {
            return false;
        }
        add_raw(raw, escaped);
        *has_character = true;
        return true;
    }
    if (is_syntax_character(codepoint)) {
        return true;
    }
    add_raw(raw, codepoint);
    frame->position += 1;
    *has_character = true;
    return true;
}

static bool read_raw_string(rules_frame *frame, raw_string *raw) {
    raw->codepoint_count = 0;
    raw->is_too_long = false;
    bool has_character = true;
    while (has_character) {
        if (!read_string_character(frame, raw, &has_character)) {
            return false;
        }
    }
    return raw->codepoint_count > 0;
}

static void normalize_raw(const raw_string *raw, rule_string *text) {
    text->codepoint_count = 0;
    text->is_too_long = raw->is_too_long;
    for (size_t codepoint_index = 0; codepoint_index < raw->codepoint_count; codepoint_index += 1) {
        append_decomposed(text, raw->codepoints[codepoint_index]);
    }
}

/* The text of an option between "[" and "]", nested brackets included;
   the frame is after "[". False when "]" is missing. */
typedef struct option_text {
    const uint32_t *codepoints;
    size_t codepoint_count;
} option_text;

static bool read_option(rules_frame *frame, option_text *option) {
    const size_t start = frame->position;
    size_t depth = 1;
    while (!is_frame_end(frame)) {
        const uint32_t codepoint = peek(frame);
        if (codepoint == '\\') {
            frame->position += 2;
            continue;
        }
        if (codepoint == '\'') {
            frame->position += 1;
            while (!is_frame_end(frame) && peek(frame) != '\'') {
                frame->position += 1;
            }
        } else if (codepoint == '[') {
            depth += 1;
        } else if (codepoint == ']') {
            depth -= 1;
            if (depth == 0) {
                option->codepoints = frame->codepoints + start;
                option->codepoint_count = frame->position - start;
                frame->position += 1;
                return true;
            }
        }
        frame->position += 1;
    }
    return false;
}

/* The words of an option, ASCII, lower case, at most a few. */
#define SXML_TAILORING_MAXIMUM_OPTION_WORD_COUNT 64U
#define SXML_TAILORING_MAXIMUM_OPTION_WORD_BYTE_COUNT 32U

typedef struct option_words {
    uint8_t words[SXML_TAILORING_MAXIMUM_OPTION_WORD_COUNT]
                 [SXML_TAILORING_MAXIMUM_OPTION_WORD_BYTE_COUNT];
    size_t word_byte_counts[SXML_TAILORING_MAXIMUM_OPTION_WORD_COUNT];
    size_t word_count;
    /* A word that is not ASCII or too long, or too many words. */
    bool is_unusual;
} option_words;

static void split_option(const option_text *option, option_words *words) {
    words->word_count = 0;
    words->is_unusual = false;
    size_t position = 0;
    while (position < option->codepoint_count) {
        if (is_rule_white_space(option->codepoints[position])) {
            position += 1;
            continue;
        }
        if (words->word_count == SXML_TAILORING_MAXIMUM_OPTION_WORD_COUNT) {
            words->is_unusual = true;
            return;
        }
        size_t byte_count = 0;
        while (position < option->codepoint_count &&
               !is_rule_white_space(option->codepoints[position])) {
            const uint32_t codepoint = option->codepoints[position];
            if (codepoint >= 0x80U || byte_count == SXML_TAILORING_MAXIMUM_OPTION_WORD_BYTE_COUNT) {
                words->is_unusual = true;
            } else {
                words->words[words->word_count][byte_count] = lower_ascii((uint8_t)codepoint);
                byte_count += 1;
            }
            position += 1;
        }
        words->word_byte_counts[words->word_count] = byte_count;
        words->word_count += 1;
    }
}

static sxml_byte_view option_word(const option_words *words, size_t word_index) {
    return (sxml_byte_view){.bytes = words->words[word_index],
                            .byte_count = words->word_byte_counts[word_index]};
}

/* Applies a setting option to the builder's settings; false for one sxml
   does not know. */
static bool apply_setting(tailoring_builder *builder, const option_words *words) {
    sxml_collation *settings = &builder->settings;
    const sxml_byte_view name = option_word(words, 0);
    const sxml_byte_view value = words->word_count > 1 ? option_word(words, 1)
                                                       : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    const bool is_on = view_is(value, "on");
    const bool is_on_or_off = is_on || view_is(value, "off");
    if (words->word_count != 2) {
        return false;
    }
    if (view_is(name, "strength")) {
        static const char *const strengths[] = {"1", "2", "3", "4", "i"};
        for (size_t strength_index = 0; strength_index < 5; strength_index += 1) {
            if (view_is(value, strengths[strength_index])) {
                settings->strength = (sxml_collation_strength)(strength_index + 1);
                settings->explicit_parameters |= SXML_COLLATION_PARAMETER_STRENGTH;
                return true;
            }
        }
        return false;
    }
    if (view_is(name, "alternate")) {
        if (!view_is(value, "shifted") && !view_is(value, "non-ignorable")) {
            return false;
        }
        settings->alternate = view_is(value, "shifted") ? SXML_COLLATION_ALTERNATE_SHIFTED
                                                        : SXML_COLLATION_ALTERNATE_NON_IGNORABLE;
        settings->explicit_parameters |= SXML_COLLATION_PARAMETER_ALTERNATE;
        return true;
    }
    if (view_is(name, "backwards")) {
        if (!view_is(value, "2")) {
            return false;
        }
        settings->is_backwards = true;
        settings->explicit_parameters |= SXML_COLLATION_PARAMETER_BACKWARDS;
        return true;
    }
    if (view_is(name, "caselevel") || view_is(name, "numericordering")) {
        if (!is_on_or_off) {
            return false;
        }
        if (view_is(name, "caselevel")) {
            settings->has_case_level = is_on;
            settings->explicit_parameters |= SXML_COLLATION_PARAMETER_CASE_LEVEL;
        } else {
            settings->is_numeric = is_on;
            settings->explicit_parameters |= SXML_COLLATION_PARAMETER_NUMERIC;
        }
        return true;
    }
    if (view_is(name, "normalization")) {
        /* sxml always normalizes (src/collation.c, normalization). */
        return is_on_or_off;
    }
    if (view_is(name, "casefirst")) {
        static const char *const case_firsts[] = {"off", "lower", "upper"};
        for (size_t case_index = 0; case_index < 3; case_index += 1) {
            if (view_is(value, case_firsts[case_index])) {
                settings->case_first = (sxml_collation_case_first)case_index;
                settings->explicit_parameters |= SXML_COLLATION_PARAMETER_CASE_FIRST;
                return true;
            }
        }
        return false;
    }
    if (view_is(name, "maxvariable")) {
        static const char *const groups[] = {"space", "punct", "symbol", "currency"};
        for (size_t group_index = 0; group_index < 4; group_index += 1) {
            if (view_is(value, groups[group_index])) {
                settings->maximum_variable = (sxml_collation_variable_group)group_index;
                settings->explicit_parameters |= SXML_COLLATION_PARAMETER_MAXIMUM_VARIABLE;
                return true;
            }
        }
        return false;
    }
    return false;
}

/* Keeps the codes of [reorder ...] (applied when the table is made; a
   later [reorder] replaces an earlier one). */
static bool keep_reorder(tailoring_builder *builder, const option_words *words) {
    builder->reorder_code_byte_count = 0;
    for (size_t word_index = 1; word_index < words->word_count; word_index += 1) {
        const sxml_byte_view word = option_word(words, word_index);
        if (builder->reorder_code_byte_count + word.byte_count + 1 > sizeof(builder->reorder_codes)) {
            return false;
        }
        memcpy(builder->reorder_codes + builder->reorder_code_byte_count, word.bytes, word.byte_count);
        builder->reorder_code_byte_count += word.byte_count;
        builder->reorder_codes[builder->reorder_code_byte_count] = ' ';
        builder->reorder_code_byte_count += 1;
    }
    builder->has_reorder = true;
    return true;
}

/* Reads [import tag]: pushes the rules of tag. */
static sxml_status import_rules(tailoring_builder *builder, const option_words *words) {
    if (words->word_count != 2) {
        builder->is_supported = false;
        return SXML_STATUS_OK;
    }
    tag_rules imported;
    sxml_status status = find_tag_rules(
        &(tag_request){.processor = builder->processor, .tag_text = option_word(words, 1)},
        &imported);
    if (status == SXML_STATUS_OK && !imported.is_honoured) {
        builder->is_supported = false;
    }
    if (status == SXML_STATUS_OK && imported.is_honoured) {
        status = push_rules(builder, (sxml_byte_view){.bytes = imported.rules.items,
                                                      .byte_count = imported.rules.item_count});
    }
    release_array(&imported.rules);
    return status;
}

/* After "&[": reads "before 1]"; false (and the frame anywhere) for any
   other option, which the caller does not support. */
static bool read_before_option(rules_frame *frame) {
    option_text option;
    if (!read_option(frame, &option)) {
        return false;
    }
    option_words words;
    split_option(&option, &words);
    return !words.is_unusual && words.word_count == 2 &&
           view_is(option_word(&words, 0), "before") && view_is(option_word(&words, 1), "1");
}

static sxml_status read_top_option(tailoring_builder *builder, rules_frame *frame) {
    option_text option;
    if (!read_option(frame, &option)) {
        builder->is_supported = false;
        return SXML_STATUS_OK;
    }
    option_words words;
    split_option(&option, &words);
    if (words.is_unusual || words.word_count == 0) {
        builder->is_supported = false;
        return SXML_STATUS_OK;
    }
    const sxml_byte_view name = option_word(&words, 0);
    if (view_is(name, "import")) {
        return import_rules(builder, &words);
    }
    if (view_is(name, "reorder")) {
        builder->is_supported = keep_reorder(builder, &words) && builder->is_supported;
        return SXML_STATUS_OK;
    }
    if (!apply_setting(builder, &words)) {
        /* [suppressContractions], [optimize], [hiraganaQ] and the
           like. */
        builder->is_supported = false;
    }
    return SXML_STATUS_OK;
}

/* Reads "<", "<<", "<<<", "<<<<" or "=" and a "*"; false when the frame
   is not at a relation. */
typedef struct relation_operator {
    relation_strength strength;
    bool is_starred;
    bool is_quaternary;
} relation_operator;

static bool read_relation_operator(rules_frame *frame, relation_operator *operator_read) {
    operator_read->is_starred = false;
    operator_read->is_quaternary = false;
    if (peek(frame) == '=') {
        frame->position += 1;
        operator_read->strength = SXML_TAILORING_STRENGTH_IDENTICAL;
    } else if (peek(frame) == '<') {
        size_t count = 0;
        while (peek(frame) == '<' && count < 4) {
            frame->position += 1;
            count += 1;
        }
        operator_read->is_quaternary = count == 4;
        operator_read->strength = count == 1   ? SXML_TAILORING_STRENGTH_PRIMARY
                                  : count == 2 ? SXML_TAILORING_STRENGTH_SECONDARY
                                               : SXML_TAILORING_STRENGTH_TERTIARY;
    } else {
        return false;
    }
    if (peek(frame) == '*') {
        frame->position += 1;
        operator_read->is_starred = true;
    }
    return true;
}

/* A starred relation: each character of the list, with ranges "a-c". */
static sxml_status apply_starred(tailoring_builder *builder, rules_frame *frame,
                                 relation_strength strength) {
    raw_string raw;
    raw.codepoint_count = 0;
    raw.is_too_long = false;
    bool has_character = true;
    uint32_t previous = 0;
    bool has_previous = false;
    size_t applied_count = 0;
    while (has_character && builder->is_supported) {
        if (peek(frame) == '-' && has_previous) {
            frame->position += 1;
            raw.codepoint_count = 0;
            if (!read_string_character(frame, &raw, &has_character) || !has_character ||
                raw.codepoint_count != 1 || raw.codepoints[0] <= previous ||
                raw.codepoints[0] - previous > 0xFFFFU) {
                builder->is_supported = false;
                return SXML_STATUS_OK;
            }
            const uint32_t last = raw.codepoints[0];
            for (uint32_t codepoint = previous + 1; codepoint <= last && builder->is_supported;
                 codepoint += 1) {
                rule_string text;
                text.codepoint_count = 0;
                text.is_too_long = false;
                append_decomposed(&text, codepoint);
                const sxml_status status = apply_relation(
                    builder, &(relation){.strength = strength, .text = &text, .extension = NULL});
                if (status != SXML_STATUS_OK) {
                    return status;
                }
            }
            has_previous = false;
            continue;
        }
        raw.codepoint_count = 0;
        if (!read_string_character(frame, &raw, &has_character)) {
            builder->is_supported = false;
            return SXML_STATUS_OK;
        }
        for (size_t codepoint_index = 0; codepoint_index < raw.codepoint_count; codepoint_index += 1) {
            rule_string text;
            text.codepoint_count = 0;
            text.is_too_long = false;
            append_decomposed(&text, raw.codepoints[codepoint_index]);
            const sxml_status status = apply_relation(
                builder, &(relation){.strength = strength, .text = &text, .extension = NULL});
            if (status != SXML_STATUS_OK) {
                return status;
            }
            previous = raw.codepoints[codepoint_index];
            has_previous = true;
            applied_count += 1;
        }
    }
    if (applied_count == 0) {
        builder->is_supported = false;
    }
    return SXML_STATUS_OK;
}

/* Reads one item of the rules at the frame: an option, a reset or a
   relation. */
static sxml_status read_rule_item(tailoring_builder *builder) {
    rules_frame *frame = current_frame(builder);
    const uint32_t codepoint = peek(frame);
    if (codepoint == '[') {
        frame->position += 1;
        return read_top_option(builder, frame);
    }
    if (codepoint == '&') {
        frame->position += 1;
        skip_white_space(frame);
        const bool has_option = peek(frame) == '[';
        bool is_before = false;
        if (has_option) {
            frame->position += 1;
            is_before = read_before_option(frame);
            skip_white_space(frame);
        }
        raw_string raw;
        if (has_option != is_before || !read_raw_string(frame, &raw)) {
            /* [before 2], [before 3] and special reset positions. */
            builder->is_supported = false;
            return SXML_STATUS_OK;
        }
        rule_string text;
        normalize_raw(&raw, &text);
        apply_reset(builder, &(reset){.text = &text, .is_before_primary = is_before});
        return SXML_STATUS_OK;
    }
    relation_operator operator_read;
    if (!read_relation_operator(frame, &operator_read) || operator_read.is_quaternary) {
        builder->is_supported = false;
        return SXML_STATUS_OK;
    }
    skip_white_space(frame);
    if (operator_read.is_starred) {
        return apply_starred(builder, frame, operator_read.strength);
    }
    raw_string raw;
    if (!read_raw_string(frame, &raw)) {
        builder->is_supported = false;
        return SXML_STATUS_OK;
    }
    skip_white_space(frame);
    if (peek(frame) == '|') {
        /* A prefix (context before the string). */
        builder->is_supported = false;
        return SXML_STATUS_OK;
    }
    rule_string text;
    normalize_raw(&raw, &text);
    rule_string extension;
    bool has_extension = false;
    if (peek(frame) == '/') {
        frame->position += 1;
        skip_white_space(frame);
        raw_string raw_extension;
        if (!read_raw_string(frame, &raw_extension)) {
            builder->is_supported = false;
            return SXML_STATUS_OK;
        }
        normalize_raw(&raw_extension, &extension);
        has_extension = true;
    }
    return apply_relation(builder, &(relation){.strength = operator_read.strength,
                                               .text = &text,
                                               .extension = has_extension ? &extension : NULL});
}

/* Reads every frame to its end (imports push frames, read first). */
static sxml_status read_all_rules(tailoring_builder *builder) {
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && builder->is_supported && builder->frame_count > 0) {
        rules_frame *frame = current_frame(builder);
        skip_white_space(frame);
        if (is_frame_end(frame)) {
            sxml_allocator_release(builder->allocator, frame->codepoints);
            builder->frame_count -= 1;
            continue;
        }
        status = read_rule_item(builder);
    }
    return status;
}

/* ------------------------------------------------------ Making the table */

/* The weights of the nodes, list by list (the module comment). */
static bool make_node_weights(tailoring_builder *builder) {
    tailoring_node *nodes = builder->nodes.items;
    const node_anchor *anchors = builder->anchors.items;
    for (size_t anchor_index = 0; anchor_index < builder->anchors.item_count; anchor_index += 1) {
        sxml_uca_element current = anchors[anchor_index].element;
        for (uint32_t node_index = anchors[anchor_index].first_node;
             node_index != SXML_TAILORING_NO_NODE; node_index = nodes[node_index].next) {
            switch (nodes[node_index].strength) {
            case SXML_TAILORING_STRENGTH_PRIMARY:
                if (current.primary_offset == 0xFFU || current.primary == 0) {
                    return false;
                }
                current.primary_offset += 1;
                current.secondary = 0x20U;
                current.secondary_offset = 0;
                current.tertiary = 0x02U;
                current.tertiary_offset = 0;
                break;
            case SXML_TAILORING_STRENGTH_SECONDARY:
                if (current.secondary_offset == 0xFFU || current.secondary == 0) {
                    return false;
                }
                current.secondary_offset += 1;
                current.tertiary = 0x02U;
                current.tertiary_offset = 0;
                break;
            case SXML_TAILORING_STRENGTH_TERTIARY:
                if (current.tertiary_offset == 0xFFU || current.tertiary == 0) {
                    return false;
                }
                current.tertiary_offset += 1;
                break;
            case SXML_TAILORING_STRENGTH_IDENTICAL:
                break;
            }
            current.flags = (uint8_t)((current.flags & SXML_UCA_ELEMENT_FLAG_VARIABLE_MARKED) |
                                      (nodes[node_index].is_upper ? SXML_UCA_ELEMENT_FLAG_UPPER : 0U));
            nodes[node_index].element = current;
        }
    }
    return true;
}

/* Allocates a tailoring table over parent with nothing in it. */
static sxml_status make_empty_table(sxml_allocator allocator,
                                    const sxml_collation_element_table *parent,
                                    sxml_collation_element_table **made) {
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = sizeof(sxml_collation_element_table),
                                  .alignment = alignof(sxml_collation_element_table)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_collation_element_table *table = memory;
    memset(table, 0, sizeof(*table));
    table->allocator = allocator;
    table->parent = parent;
    table->root = parent->root;
    table->settings = sxml_collation_codepoint();
    *made = table;
    return SXML_STATUS_OK;
}

/* Makes the table of the builder's entries (NULL when the rules tailor
   nothing). */
static sxml_status make_tailored_table(tailoring_builder *builder,
                                       sxml_collation_element_table **made) {
    *made = NULL;
    if (!make_node_weights(builder)) {
        builder->is_supported = false;
        return SXML_STATUS_OK;
    }
    if (builder->entries.item_count == 0 && builder->settings.explicit_parameters == 0 &&
        !builder->has_reorder) {
        return SXML_STATUS_OK;
    }
    sxml_collation_element_table *table = NULL;
    sxml_status status = make_empty_table(builder->allocator, builder->root, &table);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->settings = builder->settings;
    const tailored_entry *entries = builder->entries.items;
    const element_reference *references = builder->references.items;
    const tailoring_node *nodes = builder->nodes.items;
    size_t element_count = 0;
    for (size_t entry_index = 0; entry_index < builder->entries.item_count; entry_index += 1) {
        element_count += entries[entry_index].reference_count;
        if (entries[entry_index].codepoint_count == 1) {
            table->mapping_count += 1;
        } else {
            table->contraction_count += 1;
        }
    }
    void *memory = NULL;
    status = sxml_allocator_allocate_array(builder->allocator,
                                     (sxml_array_allocation_request){.element_count = element_count,
                                                              .element_size = sizeof(sxml_uca_element),
                                                              .element_alignment = alignof(sxml_uca_element)},
                                     &memory);
    table->elements = memory;
    table->element_count = element_count;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(builder->allocator,
                                         (sxml_array_allocation_request){.element_count = table->mapping_count,
                                                                  .element_size = sizeof(sxml_uca_mapping),
                                                                  .element_alignment = alignof(sxml_uca_mapping)},
                                         &memory);
        table->mappings = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            builder->allocator,
            (sxml_array_allocation_request){.element_count = table->contraction_count,
                                     .element_size = sizeof(sxml_uca_contraction),
                                     .element_alignment = alignof(sxml_uca_contraction)},
            &memory);
        table->contractions = memory;
    }
    if (status != SXML_STATUS_OK) {
        sxml_uca_release_table(table);
        return status;
    }
    size_t element_offset = 0;
    size_t mapping_count = 0;
    size_t contraction_count = 0;
    for (size_t entry_index = 0; entry_index < builder->entries.item_count; entry_index += 1) {
        const tailored_entry *entry = &entries[entry_index];
        for (uint32_t reference_index = 0; reference_index < entry->reference_count;
             reference_index += 1) {
            const element_reference *reference =
                &references[entry->first_reference + reference_index];
            table->elements[element_offset + reference_index] =
                reference->node == SXML_TAILORING_NO_NODE ? reference->element
                                                          : nodes[reference->node].element;
        }
        if (entry->codepoint_count == 1) {
            table->mappings[mapping_count] =
                (sxml_uca_mapping){.codepoint = entry->codepoints[0],
                                   .element_offset = (uint32_t)element_offset,
                                   .element_count = entry->reference_count};
            mapping_count += 1;
        } else {
            sxml_uca_contraction *contraction = &table->contractions[contraction_count];
            memset(contraction, 0, sizeof(*contraction));
            memcpy(contraction->codepoints, entry->codepoints,
                   entry->codepoint_count * sizeof(uint32_t));
            contraction->codepoint_count = entry->codepoint_count;
            contraction->element_offset = (uint32_t)element_offset;
            contraction->element_count = entry->reference_count;
            contraction_count += 1;
        }
        element_offset += entry->reference_count;
    }
    sxml_uca_sort_entries(table);
    if (builder->has_reorder) {
        bool is_honoured = false;
        status = sxml_uca_set_reordering(
            &(sxml_uca_reordering_request){
                .table = table,
                .codes = {.bytes = builder->reorder_codes,
                          .byte_count = builder->reorder_code_byte_count}},
            &is_honoured);
        if (status != SXML_STATUS_OK || !is_honoured) {
            builder->is_supported = builder->is_supported && is_honoured;
            sxml_uca_release_table(table);
            return status;
        }
    }
    *made = table;
    return SXML_STATUS_OK;
}

/* Compiles the collation of a language tag (lower case) over root. */
typedef struct language_compilation {
    sxml_processor *processor;
    const sxml_collation_element_table *root;
    sxml_byte_view tag_text;
} language_compilation;

static sxml_status compile_language(const language_compilation *compilation,
                                    sxml_uca_kept_tailoring *kept) {
    sxml_processor *processor = compilation->processor;
    kept->table = NULL;
    kept->made_table = NULL;
    kept->is_honoured = false;
    tag_rules rules;
    sxml_status status = find_tag_rules(
        &(tag_request){.processor = processor, .tag_text = compilation->tag_text}, &rules);
    if (status != SXML_STATUS_OK || !rules.is_honoured) {
        release_array(&rules.rules);
        return status;
    }
    tailoring_builder builder;
    memset(&builder, 0, sizeof(builder));
    builder.processor = processor;
    builder.allocator = processor->allocator;
    builder.root = compilation->root;
    builder.is_supported = true;
    builder.settings = sxml_collation_codepoint();
    builder.entries = (growable_array){.allocator = processor->allocator,
                                       .item_size = sizeof(tailored_entry),
                                       .item_alignment = alignof(tailored_entry)};
    builder.references = (growable_array){.allocator = processor->allocator,
                                          .item_size = sizeof(element_reference),
                                          .item_alignment = alignof(element_reference)};
    builder.nodes = (growable_array){.allocator = processor->allocator,
                                     .item_size = sizeof(tailoring_node),
                                     .item_alignment = alignof(tailoring_node)};
    builder.anchors = (growable_array){.allocator = processor->allocator,
                                       .item_size = sizeof(node_anchor),
                                       .item_alignment = alignof(node_anchor)};
    status = push_rules(&builder, (sxml_byte_view){.bytes = rules.rules.items,
                                                   .byte_count = rules.rules.item_count});
    release_array(&rules.rules);
    if (status == SXML_STATUS_OK) {
        status = read_all_rules(&builder);
    }
    sxml_collation_element_table *table = NULL;
    if (status == SXML_STATUS_OK && builder.is_supported) {
        status = make_tailored_table(&builder, &table);
    }
    if (status == SXML_STATUS_OK && builder.is_supported) {
        kept->table = table;
        kept->made_table = table;
        kept->is_honoured = true;
    } else {
        sxml_uca_release_table(table);
    }
    release_builder(&builder);
    return status;
}

/* ------------------------------------------------------- Kept tailorings */

/* Writes the key of a language and a reordering, lower case, with "-"
   for "_"; false when it does not fit. */
typedef struct tailoring_key {
    uint8_t bytes[SXML_UCA_MAXIMUM_TAILORING_KEY_BYTE_COUNT];
    size_t byte_count;
} tailoring_key;

typedef struct key_parts {
    sxml_byte_view language;
    sxml_byte_view reordering;
} key_parts;

static bool make_key(key_parts parts, tailoring_key *key) {
    if (parts.language.byte_count + parts.reordering.byte_count + 1 > sizeof(key->bytes)) {
        return false;
    }
    key->byte_count = 0;
    for (size_t byte_index = 0; byte_index < parts.language.byte_count; byte_index += 1) {
        const uint8_t byte = lower_ascii(parts.language.bytes[byte_index]);
        key->bytes[key->byte_count] = byte == '_' ? (uint8_t)'-' : byte;
        key->byte_count += 1;
    }
    key->bytes[key->byte_count] = '|';
    key->byte_count += 1;
    for (size_t byte_index = 0; byte_index < parts.reordering.byte_count; byte_index += 1) {
        key->bytes[key->byte_count] = lower_ascii(parts.reordering.bytes[byte_index]);
        key->byte_count += 1;
    }
    return true;
}

static sxml_uca_kept_tailoring *find_kept(sxml_uca_feature_state *state, const tailoring_key *key) {
    for (size_t kept_index = 0; kept_index < state->tailoring_count; kept_index += 1) {
        sxml_uca_kept_tailoring *kept = &state->tailorings[kept_index];
        if (kept->key_byte_count == key->byte_count &&
            memcmp(kept->key, key->bytes, key->byte_count) == 0) {
            return kept;
        }
    }
    return NULL;
}

static sxml_uca_kept_tailoring *add_kept(sxml_uca_feature_state *state, const tailoring_key *key) {
    if (state->tailoring_count == SXML_UCA_MAXIMUM_TAILORING_COUNT) {
        return NULL;
    }
    sxml_uca_kept_tailoring *kept = &state->tailorings[state->tailoring_count];
    state->tailoring_count += 1;
    memcpy(kept->key, key->bytes, key->byte_count);
    kept->key_byte_count = key->byte_count;
    kept->table = NULL;
    kept->made_table = NULL;
    kept->is_honoured = false;
    return kept;
}

static const sxml_collation_element_table *kept_table(const sxml_uca_feature_state *state,
                                                      const sxml_uca_kept_tailoring *kept) {
    return kept->table != NULL ? kept->table : state->table;
}

sxml_status sxml_uca_build_tailoring(const sxml_uca_tailoring_build *build,
                                     sxml_uca_tailoring *tailoring) {
    sxml_uca_feature_state *state = build->state;
    const sxml_uca_tailoring_request *request = build->request;
    tailoring->table = state->table;
    tailoring->is_honoured = false;
    tailoring_key key;
    if (!make_key((key_parts){.language = request->language, .reordering = request->reordering},
                  &key)) {
        return SXML_STATUS_OK;
    }
    const sxml_uca_kept_tailoring *found = find_kept(state, &key);
    if (found != NULL) {
        tailoring->table = kept_table(state, found);
        tailoring->is_honoured = found->is_honoured;
        return SXML_STATUS_OK;
    }
    /* The language alone first, kept under its own key. */
    tailoring_key language_key;
    (void)make_key((key_parts){.language = request->language,
                               .reordering = {.bytes = NULL, .byte_count = 0}},
                   &language_key);
    sxml_uca_kept_tailoring *language = find_kept(state, &language_key);
    if (language == NULL && request->language.byte_count > 0) {
        language = add_kept(state, &language_key);
        if (language == NULL) {
            return SXML_STATUS_OK;
        }
        /* The tag in lower case, as the key has it. */
        const sxml_status status = compile_language(
            &(language_compilation){
                .processor = build->processor,
                .root = state->table,
                .tag_text = {.bytes = language_key.bytes, .byte_count = language_key.byte_count - 1}},
            language);
        if (status != SXML_STATUS_OK) {
            /* Not kept: asked again next time. */
            state->tailoring_count -= 1;
            return status;
        }
    }
    const bool is_language_honoured = language == NULL || language->is_honoured;
    const sxml_collation_element_table *language_table =
        language == NULL ? state->table : kept_table(state, language);
    if (request->reordering.byte_count == 0) {
        tailoring->table = language_table;
        tailoring->is_honoured = is_language_honoured;
        return SXML_STATUS_OK;
    }
    sxml_uca_kept_tailoring *reordered = add_kept(state, &key);
    if (reordered == NULL) {
        tailoring->table = language_table;
        return SXML_STATUS_OK;
    }
    sxml_collation_element_table *table = NULL;
    sxml_status status = make_empty_table(build->processor->allocator, language_table, &table);
    bool is_honoured = false;
    if (status == SXML_STATUS_OK) {
        status = sxml_uca_set_reordering(
            &(sxml_uca_reordering_request){.table = table, .codes = request->reordering},
            &is_honoured);
    }
    if (status != SXML_STATUS_OK) {
        sxml_uca_release_table(table);
        state->tailoring_count -= 1;
        return status;
    }
    if (is_honoured) {
        reordered->table = table;
        reordered->made_table = table;
    } else {
        /* The language's order without the reordering. */
        sxml_uca_release_table(table);
        reordered->table = language == NULL ? NULL : language->table;
    }
    reordered->is_honoured = is_honoured && is_language_honoured;
    tailoring->table = kept_table(state, reordered);
    tailoring->is_honoured = reordered->is_honoured;
    return SXML_STATUS_OK;
}
