/*
 * fn:deep-equal($input1, $input2, $options := {}) (F&O 4.0, section
 * 14.2.4), the deep equality of two sequences (packet 1.20, D78).
 *
 * Golden file: src/memory.c (layout and comments).
 *
 * No recursion (D71): nested arrays, maps, JNode values and node children
 * are compared on an explicit stack of comparison pairs in item heap
 * memory. Each pair is a cursor over two collections (two sequences, two
 * arrays, two maps, or the children of two nodes); the loop advances the
 * top cursor by one step, compares the two items it yields, and pushes a
 * new cursor for items that have parts. The first difference ends the
 * comparison.
 *
 * Rules (F&O 4.0, section 14.2.4.1, with the default options):
 * - atomic items: equal under fn:compare with the codepoint collation and
 *   the implicit timezone, NaN equal to NaN; items that do not compare are
 *   not equal (no error);
 * - XNodes: same kind; documents by children; elements by name,
 *   attributes (as a set, by name and string value) and children;
 *   attributes, namespace nodes and processing instructions by name and
 *   string value; text and comments by string value. Comments and
 *   processing instructions among children are ignored, and adjacent text
 *   nodes are merged first;
 * - JNodes: by their ·jvalue·s;
 * - maps: same size, every key of the first in the second, values deep
 *   equal (in any order unless map-order);
 * - arrays: same size, members deep equal in order;
 * - other function items: equal only when they are the same item.
 *
 * Options: "collation" (only the codepoint collation, else FOCH0002),
 * "comments", "processing-instructions", "map-order", "whitespace"
 * ("preserve", "strip", "normalize") and "debug" are supported. The other
 * options of section 14.2.4 are accepted at their default value; any
 * other value is SXML_STATUS_UNSUPPORTED_FEATURE in Phase 1, in particular
 * "items-equal" (a function to call), "ordered" false and
 * "unordered-elements", "ignore-empty-entries" and "false-on-error". A
 * string key that names no option is XPTY0004. Schema-related options make
 * no difference without schemas. Two function items that are not maps or
 * arrays are equal only with the same body, arity and captured values, so
 * two separately written but identical inline functions are not (F&O 4.0
 * compares their definitions).
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"

#include <stdalign.h>
#include <string.h>

/* ---------------------------------------------------------------- Options */

typedef enum whitespace_handling {
    SXML_WHITESPACE_HANDLING_PRESERVE,
    /* Whitespace-only text runs among children are ignored. */
    SXML_WHITESPACE_HANDLING_STRIP,
    /* Strings and text runs compare after fn:normalize-space. */
    SXML_WHITESPACE_HANDLING_NORMALIZE,
} whitespace_handling;

typedef struct deep_equal_options {
    bool compares_comments;
    bool compares_processing_instructions;
    bool compares_map_order;
    whitespace_handling whitespace;
    int16_t implicit_timezone_offset_minutes;
} deep_equal_options;

static const sxml_static_error invalid_option_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An option of fn:deep-equal has a value of the wrong type (F&O 4.0, "
        "section 14.2.4)"),
};

/* Options accepted at one boolean value only (their default). */
typedef struct fixed_option {
    sxml_byte_view option_name;
    bool default_value;
} fixed_option;

static const fixed_option fixed_options[] = {
    {SXML_BYTE_VIEW_INITIALIZER("base-uri"), false},
    {SXML_BYTE_VIEW_INITIALIZER("id-property"), false},
    {SXML_BYTE_VIEW_INITIALIZER("idrefs-property"), false},
    {SXML_BYTE_VIEW_INITIALIZER("in-scope-namespaces"), false},
    {SXML_BYTE_VIEW_INITIALIZER("namespace-prefixes"), false},
    {SXML_BYTE_VIEW_INITIALIZER("nilled-property"), true},
    {SXML_BYTE_VIEW_INITIALIZER("ordered"), true},
    {SXML_BYTE_VIEW_INITIALIZER("timezones"), false},
    {SXML_BYTE_VIEW_INITIALIZER("type-annotations"), false},
    {SXML_BYTE_VIEW_INITIALIZER("type-variety"), true},
    {SXML_BYTE_VIEW_INITIALIZER("typed-values"), true},
};

/* Options with a value other than a boolean that Phase 1 does not
   support at all (present and not empty is UNSUPPORTED_FEATURE). */
static const sxml_byte_view unsupported_options[] = {
    SXML_BYTE_VIEW_INITIALIZER("false-on-error"),
    SXML_BYTE_VIEW_INITIALIZER("ignore-empty-entries"),
    SXML_BYTE_VIEW_INITIALIZER("items-equal"),
    SXML_BYTE_VIEW_INITIALIZER("normalization-form"),
    SXML_BYTE_VIEW_INITIALIZER("unordered-elements"),
};

/* An option to look up in the options map. */
typedef struct option_query {
    sxml_item options;
    sxml_byte_view option_name;
} option_query;

/* The value of the option, empty when absent. */
static sxml_status find_option(const sxml_function_call *call, option_query query,
                               sxml_sequence *value) {
    const sxml_item options = query.options;
    const sxml_byte_view option_name = query.option_name;
    sxml_sequence key = sxml_sequence_empty();
    sxml_status status = sxml_function_library_make_string(call->item_heap, option_name, &key);
    sxml_map_lookup lookup = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_find(options, key.content, &lookup);
        sxml_sequence_release(key);
    }
    if (status == SXML_STATUS_OK) {
        *value = lookup.is_found ? lookup.entry.value : sxml_sequence_empty();
    }
    return status;
}

/* Reads a boolean option; *is_present is false when the map lacks it. */
typedef struct boolean_option {
    bool is_present;
    bool value;
} boolean_option;

static sxml_status read_boolean_option(const sxml_function_call *call, option_query query,
                                       boolean_option *option) {
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = find_option(call, query, &value);
    *option = (boolean_option){.is_present = false, .value = false};
    if (status != SXML_STATUS_OK ||
        sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        return status;
    }
    sxml_atomic_datum datum;
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
        sxml_item_get_kind(value.content) != SXML_ITEM_KIND_ATOMIC ||
        sxml_atomic_item_get_datum(value.content, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_BOOLEAN) {
        return sxml_function_library_report(call, &invalid_option_error);
    }
    *option = (boolean_option){.is_present = true, .value = datum.datum_details.boolean};
    return SXML_STATUS_OK;
}

static sxml_status read_string_option(const sxml_function_call *call, option_query query,
                                      sxml_string *text) {
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = find_option(call, query, &value);
    *text = (sxml_string){.utf8 = {.bytes = NULL, .byte_count = 0}};
    if (status != SXML_STATUS_OK ||
        sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        return status;
    }
    sxml_atomic_datum datum;
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
        sxml_item_get_kind(value.content) != SXML_ITEM_KIND_ATOMIC ||
        sxml_atomic_item_get_datum(value.content, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return sxml_function_library_report(call, &invalid_option_error);
    }
    *text = datum.datum_details.string;
    return SXML_STATUS_OK;
}

static const sxml_static_error unsupported_collation_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCH0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The collation is not supported: only the Unicode codepoint collation is "
        "(F&O 4.0, section 5.3.2)"),
};

/* Every option of fn:deep-equal (F&O 4.0, section 14.2.4). */
static const sxml_byte_view known_options[] = {
    SXML_BYTE_VIEW_INITIALIZER("base-uri"),
    SXML_BYTE_VIEW_INITIALIZER("collation"),
    SXML_BYTE_VIEW_INITIALIZER("comments"),
    SXML_BYTE_VIEW_INITIALIZER("debug"),
    SXML_BYTE_VIEW_INITIALIZER("false-on-error"),
    SXML_BYTE_VIEW_INITIALIZER("id-property"),
    SXML_BYTE_VIEW_INITIALIZER("idrefs-property"),
    SXML_BYTE_VIEW_INITIALIZER("ignore-empty-entries"),
    SXML_BYTE_VIEW_INITIALIZER("in-scope-namespaces"),
    SXML_BYTE_VIEW_INITIALIZER("items-equal"),
    SXML_BYTE_VIEW_INITIALIZER("map-order"),
    SXML_BYTE_VIEW_INITIALIZER("namespace-prefixes"),
    SXML_BYTE_VIEW_INITIALIZER("nilled-property"),
    SXML_BYTE_VIEW_INITIALIZER("normalization-form"),
    SXML_BYTE_VIEW_INITIALIZER("ordered"),
    SXML_BYTE_VIEW_INITIALIZER("processing-instructions"),
    SXML_BYTE_VIEW_INITIALIZER("timezones"),
    SXML_BYTE_VIEW_INITIALIZER("type-annotations"),
    SXML_BYTE_VIEW_INITIALIZER("type-variety"),
    SXML_BYTE_VIEW_INITIALIZER("typed-values"),
    SXML_BYTE_VIEW_INITIALIZER("unordered-elements"),
    SXML_BYTE_VIEW_INITIALIZER("whitespace"),
};

static const sxml_static_error unknown_option_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The options map of fn:deep-equal has a key that is not an option (F&O 4.0, "
        "section 1.5.3; QT4 deep-equal-40-options-005)"),
};

/* Rejects string keys that name no option; keys of other types (QNames
   of implementation-defined options) are ignored. */
static sxml_status check_option_keys(const sxml_function_call *call, sxml_item options) {
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(options, &entry_count);
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(options, entry_index, &entry);
        sxml_atomic_datum datum;
        if (status != SXML_STATUS_OK ||
            sxml_atomic_item_get_datum(entry.key, &datum) != SXML_STATUS_OK ||
            datum.datum_kind != SXML_DATUM_KIND_STRING) {
            continue;
        }
        bool is_known = false;
        for (size_t option_index = 0;
             option_index < sizeof(known_options) / sizeof(known_options[0]); option_index += 1) {
            is_known = is_known || sxml_byte_view_equals(datum.datum_details.string.utf8,
                                                         known_options[option_index]);
        }
        if (!is_known) {
            return sxml_function_library_report(call, &unknown_option_error);
        }
    }
    return status;
}

/* Reads an options map (F&O 4.0, section 1.5.3, option parameter
   conventions). */
static sxml_status read_options_map(const sxml_function_call *call, sxml_item options,
                                    deep_equal_options *result) {
    boolean_option option;
    sxml_status status = check_option_keys(call, options);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = read_boolean_option(call, (option_query){.options = options, .option_name = SXML_BYTE_VIEW_FROM_LITERAL("comments")}, &option);
    result->compares_comments = option.value;
    if (status == SXML_STATUS_OK) {
        status = read_boolean_option(call, (option_query){.options = options, .option_name = SXML_BYTE_VIEW_FROM_LITERAL("processing-instructions")}, &option);
        result->compares_processing_instructions = option.value;
    }
    if (status == SXML_STATUS_OK) {
        status = read_boolean_option(call, (option_query){.options = options, .option_name = SXML_BYTE_VIEW_FROM_LITERAL("map-order")}, &option);
        result->compares_map_order = option.value;
    }
    if (status == SXML_STATUS_OK) {
        status = read_boolean_option(call, (option_query){.options = options, .option_name = SXML_BYTE_VIEW_FROM_LITERAL("debug")}, &option);
    }
    for (size_t option_index = 0;
         status == SXML_STATUS_OK &&
         option_index < sizeof(fixed_options) / sizeof(fixed_options[0]);
         option_index += 1) {
        status = read_boolean_option(call, (option_query){.options = options, .option_name = fixed_options[option_index].option_name}, &option);
        if (status == SXML_STATUS_OK && option.is_present &&
            option.value != fixed_options[option_index].default_value) {
            status = SXML_STATUS_UNSUPPORTED_FEATURE;
        }
    }
    for (size_t option_index = 0;
         status == SXML_STATUS_OK &&
         option_index < sizeof(unsupported_options) / sizeof(unsupported_options[0]);
         option_index += 1) {
        sxml_sequence value = sxml_sequence_empty();
        status = find_option(call, (option_query){.options = options, .option_name = unsupported_options[option_index]}, &value);
        if (status == SXML_STATUS_OK &&
            sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_EMPTY) {
            status = SXML_STATUS_UNSUPPORTED_FEATURE;
        }
    }
    sxml_string text;
    if (status == SXML_STATUS_OK) {
        status = read_string_option(call, (option_query){.options = options, .option_name = SXML_BYTE_VIEW_FROM_LITERAL("whitespace")}, &text);
    }
    if (status == SXML_STATUS_OK && text.utf8.byte_count > 0) {
        if (sxml_byte_view_equals(text.utf8, SXML_BYTE_VIEW_FROM_LITERAL("strip"))) {
            result->whitespace = SXML_WHITESPACE_HANDLING_STRIP;
        } else if (sxml_byte_view_equals(text.utf8, SXML_BYTE_VIEW_FROM_LITERAL("normalize"))) {
            result->whitespace = SXML_WHITESPACE_HANDLING_NORMALIZE;
        } else if (!sxml_byte_view_equals(text.utf8, SXML_BYTE_VIEW_FROM_LITERAL("preserve"))) {
            status = sxml_function_library_report(call, &invalid_option_error);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = read_string_option(call, (option_query){.options = options, .option_name = SXML_BYTE_VIEW_FROM_LITERAL("collation")}, &text);
    }
    if (status == SXML_STATUS_OK && text.utf8.byte_count > 0 &&
        !sxml_byte_view_equals(text.utf8, sxml_function_library_codepoint_collation_uri())) {
        status = sxml_function_library_report(call, &unsupported_collation_error);
    }
    return status;
}

static sxml_status read_options(const sxml_function_call *call,
                                deep_equal_options *options) {
    *options = (deep_equal_options){.compares_comments = false,
                                    .compares_processing_instructions = false,
                                    .compares_map_order = false,
                                    .whitespace = SXML_WHITESPACE_HANDLING_PRESERVE,
                                    .implicit_timezone_offset_minutes = 0};
    sxml_status status = SXML_STATUS_OK;
    if (call->dynamic_context != NULL) {
        status = sxml_dynamic_context_read_implicit_timezone(
            call->dynamic_context, &options->implicit_timezone_offset_minutes);
    }
    sxml_optional_item argument = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 2, &argument);
    }
    if (status != SXML_STATUS_OK || !argument.has_item) {
        return status;
    }
    if (sxml_item_get_kind(argument.item) == SXML_ITEM_KIND_MAP) {
        return read_options_map(call, argument.item, options);
    }
    /* A string: the collation (F&O 4.0, fn:deep-equal, $options). */
    return sxml_function_library_check_collation(call, 2);
}

/* ------------------------------------------------------- Comparing text */

static bool is_whitespace_byte(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* Reads normalized text (fn:normalize-space) one byte at a time. */
typedef struct normalized_reader {
    sxml_byte_view text;
    size_t byte_index;
    bool is_space_pending;
    /* True once a byte was read: whitespace before it is leading. */
    bool has_output;
} normalized_reader;

/* The next byte of the normalized text, or -1 at its end. */
static int32_t next_normalized_byte(normalized_reader *reader) {
    while (reader->byte_index < reader->text.byte_count &&
           is_whitespace_byte(reader->text.bytes[reader->byte_index])) {
        reader->is_space_pending = true;
        reader->byte_index += 1;
    }
    if (reader->byte_index >= reader->text.byte_count) {
        return -1;
    }
    if (reader->is_space_pending) {
        reader->is_space_pending = false;
        /* Leading whitespace gives no space. */
        if (reader->has_output) {
            return ' ';
        }
    }
    reader->has_output = true;
    const int32_t byte = reader->text.bytes[reader->byte_index];
    reader->byte_index += 1;
    return byte;
}

typedef struct text_pair {
    sxml_byte_view first_text;
    sxml_byte_view second_text;
} text_pair;

static bool texts_are_equal(const deep_equal_options *options, text_pair pair) {
    if (options->whitespace != SXML_WHITESPACE_HANDLING_NORMALIZE) {
        return sxml_byte_view_equals(pair.first_text, pair.second_text);
    }
    normalized_reader first = {.text = pair.first_text, .byte_index = 0};
    normalized_reader second = {.text = pair.second_text, .byte_index = 0};
    for (;;) {
        const int32_t first_byte = next_normalized_byte(&first);
        const int32_t second_byte = next_normalized_byte(&second);
        if (first_byte != second_byte) {
            return false;
        }
        if (first_byte < 0) {
            return true;
        }
    }
}

static bool is_all_whitespace(sxml_byte_view text) {
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        if (!is_whitespace_byte(text.bytes[byte_index])) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------ Comparison pairs */

typedef enum pair_kind {
    /* Two sequences, item by item. */
    SXML_PAIR_KIND_SEQUENCES,
    /* Two arrays, member by member. */
    SXML_PAIR_KIND_ARRAYS,
    /* Two maps, entry by entry of the first. */
    SXML_PAIR_KIND_MAPS,
    /* The children of two documents or elements, unit by unit. */
    SXML_PAIR_KIND_CHILDREN,
} pair_kind;

/* Where a children cursor is on one side. */
typedef struct child_position {
    sxml_node_reference next_child;
    bool has_next_child;
} child_position;

/* One cursor of the explicit stack (glossary `comparison_pair`). Every
   value is borrowed from the arguments of the call. */
typedef struct comparison_pair {
    pair_kind kind;
    sxml_sequence first_sequence;
    sxml_sequence second_sequence;
    sxml_item first_item;
    sxml_item second_item;
    child_position first_position;
    child_position second_position;
    uint64_t next_index;
    uint64_t count;
} comparison_pair;

/* The state of one deep-equal call. */
typedef struct deep_comparison {
    const sxml_function_call *call;
    deep_equal_options options;
    comparison_pair *pairs;
    size_t pair_count;
    size_t pair_capacity;
    bool is_equal;
    /* Merged text runs of the current children units. */
    sxml_library_text first_text;
    sxml_library_text second_text;
} deep_comparison;

static sxml_status push_pair(deep_comparison *comparison, const comparison_pair *pair) {
    if (comparison->pair_count == comparison->pair_capacity) {
        const size_t capacity =
            comparison->pair_capacity == 0 ? 16 : comparison->pair_capacity * 2;
        size_t byte_count = 0;
        sxml_status status = sxml_size_multiply(capacity, sizeof(comparison_pair), &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_item_heap_allocate_bytes(
                comparison->call->item_heap,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(comparison_pair)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (comparison->pair_count > 0) {
            memcpy(memory, comparison->pairs, comparison->pair_count * sizeof(comparison_pair));
        }
        sxml_item_heap_release_bytes(
            comparison->call->item_heap,
            (sxml_heap_bytes){.memory = comparison->pairs,
                              .byte_count = comparison->pair_capacity * sizeof(comparison_pair)});
        comparison->pairs = memory;
        comparison->pair_capacity = capacity;
    }
    comparison->pairs[comparison->pair_count] = *pair;
    comparison->pair_count += 1;
    return SXML_STATUS_OK;
}

/* Pushes two sequences when they have the same length. */
static sxml_status push_sequences(deep_comparison *comparison, sxml_sequence first,
                                  sxml_sequence second) {
    uint64_t first_count = 0;
    uint64_t second_count = 0;
    sxml_status status =
        sxml_sequence_count(first, comparison->call->error_record, &first_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(second, comparison->call->error_record, &second_count);
    }
    if (status != SXML_STATUS_OK || first_count != second_count) {
        comparison->is_equal = comparison->is_equal && first_count == second_count;
        return status;
    }
    if (first_count == 0) {
        return SXML_STATUS_OK;
    }
    return push_pair(comparison,
                     &(comparison_pair){.kind = SXML_PAIR_KIND_SEQUENCES,
                                        .first_sequence = first,
                                        .second_sequence = second,
                                        .next_index = 0,
                                        .count = first_count});
}

/* ------------------------------------------------------ Comparing items */

static sxml_status compare_atomic(deep_comparison *comparison, sxml_item first,
                                  sxml_item second) {
    if (comparison->options.whitespace == SXML_WHITESPACE_HANDLING_NORMALIZE) {
        sxml_atomic_datum first_datum;
        sxml_atomic_datum second_datum;
        if (sxml_atomic_item_get_datum(first, &first_datum) == SXML_STATUS_OK &&
            sxml_atomic_item_get_datum(second, &second_datum) == SXML_STATUS_OK &&
            first_datum.datum_kind == SXML_DATUM_KIND_STRING &&
            second_datum.datum_kind == SXML_DATUM_KIND_STRING) {
            comparison->is_equal = texts_are_equal(
                &comparison->options,
                (text_pair){.first_text = first_datum.datum_details.string.utf8,
                            .second_text = second_datum.datum_details.string.utf8});
            return SXML_STATUS_OK;
        }
    }
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    const sxml_status status = sxml_atomic_item_compare_in_heap(
        comparison->call->item_heap,
        &(sxml_atomic_comparison){
            .first = first,
            .second = second,
            .implicit_timezone_offset_minutes =
                comparison->options.implicit_timezone_offset_minutes,
            .error_record = NULL,
            .name_pool = sxml_item_heap_get_name_pool(comparison->call->item_heap)},
        &ordering);
    if (status == SXML_STATUS_TYPE_ERROR || status == SXML_STATUS_INVALID_ARGUMENT ||
        status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        /* Items that do not compare are not deep-equal (F&O 4.0, section
           14.2.4.1). */
        comparison->is_equal = false;
        return SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK) {
        comparison->is_equal = ordering == SXML_ORDERING_EQUAL;
    }
    return status;
}

/* The attributes of two elements as sets (F&O 4.0, section 14.2.4.1). */
typedef struct element_pair {
    sxml_node_reference first_element;
    sxml_node_reference second_element;
    uint32_t attribute_count;
} element_pair;

static sxml_status compare_attributes(deep_comparison *comparison, element_pair pair) {
    for (uint32_t first_index = 1; first_index <= pair.attribute_count; first_index += 1) {
        const sxml_node_reference first = {
            .tree = pair.first_element.tree,
            .node_index = {.index = pair.first_element.node_index.index + first_index},
            .namespace_number = 0};
        sxml_node_description first_description;
        sxml_byte_view first_content;
        sxml_status status = sxml_node_describe(first, &first_description);
        if (status == SXML_STATUS_OK) {
            status = sxml_node_get_content(first, &first_content);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        bool is_matched = false;
        for (uint32_t second_index = 1; !is_matched && second_index <= pair.attribute_count;
             second_index += 1) {
            const sxml_node_reference second = {
                .tree = pair.second_element.tree,
                .node_index = {.index = pair.second_element.node_index.index + second_index},
                .namespace_number = 0};
            sxml_node_description second_description;
            status = sxml_node_describe(second, &second_description);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (second_description.node_name.index != first_description.node_name.index) {
                continue;
            }
            sxml_byte_view second_content;
            status = sxml_node_get_content(second, &second_content);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            is_matched = texts_are_equal(&comparison->options,
                                         (text_pair){.first_text = first_content,
                                                     .second_text = second_content});
            if (!is_matched) {
                break;
            }
        }
        if (!is_matched) {
            comparison->is_equal = false;
            return SXML_STATUS_OK;
        }
    }
    return SXML_STATUS_OK;
}

typedef struct node_pair {
    sxml_node_reference first_node;
    sxml_node_reference second_node;
} node_pair;

/* Compares the contents of two leaf nodes (attributes, text, comments,
   processing instructions, namespace nodes). */
static sxml_status compare_contents(deep_comparison *comparison, node_pair pair) {
    sxml_byte_view first_content;
    sxml_byte_view second_content;
    sxml_status status = sxml_node_get_content(pair.first_node, &first_content);
    if (status == SXML_STATUS_OK) {
        status = sxml_node_get_content(pair.second_node, &second_content);
    }
    if (status == SXML_STATUS_OK) {
        comparison->is_equal = texts_are_equal(
            &comparison->options,
            (text_pair){.first_text = first_content, .second_text = second_content});
    }
    return status;
}

static child_position first_child_position(sxml_node_reference node,
                                           const sxml_node_description *description) {
    return (child_position){
        .next_child = {.tree = node.tree,
                       .node_index = description->first_child,
                       .namespace_number = 0},
        .has_next_child = description->has_children};
}

static sxml_status compare_nodes(deep_comparison *comparison, node_pair pair) {
    sxml_node_description first;
    sxml_node_description second;
    sxml_status status = sxml_node_describe(pair.first_node, &first);
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(pair.second_node, &second);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (first.node_kind != second.node_kind || first.node_name.index != second.node_name.index) {
        comparison->is_equal = false;
        return SXML_STATUS_OK;
    }
    switch (first.node_kind) {
    case SXML_NODE_KIND_ELEMENT:
        if (first.attribute_count != second.attribute_count) {
            comparison->is_equal = false;
            return SXML_STATUS_OK;
        }
        status = compare_attributes(comparison,
                                    (element_pair){.first_element = pair.first_node,
                                                   .second_element = pair.second_node,
                                                   .attribute_count = first.attribute_count});
        if (status != SXML_STATUS_OK || !comparison->is_equal) {
            return status;
        }
        return push_pair(comparison,
                         &(comparison_pair){
                             .kind = SXML_PAIR_KIND_CHILDREN,
                             .first_position = first_child_position(pair.first_node, &first),
                             .second_position = first_child_position(pair.second_node, &second),
                             .next_index = 0,
                             .count = 0});
    case SXML_NODE_KIND_DOCUMENT:
        return push_pair(comparison,
                         &(comparison_pair){
                             .kind = SXML_PAIR_KIND_CHILDREN,
                             .first_position = first_child_position(pair.first_node, &first),
                             .second_position = first_child_position(pair.second_node, &second),
                             .next_index = 0,
                             .count = 0});
    case SXML_NODE_KIND_ATTRIBUTE:
    case SXML_NODE_KIND_TEXT:
    case SXML_NODE_KIND_COMMENT:
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
    case SXML_NODE_KIND_NAMESPACE:
        return compare_contents(comparison, pair);
    case SXML_NODE_KIND_ANY:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Two function items that are neither maps nor arrays: equal when they
   have the same function identity (F&O 4.0, section 14.2.4.1): the same
   item, or the same body and arity with identical captured values, as two
   references to one named function give. */
static bool is_same_function(sxml_item first, sxml_item second) {
    if (first.representation == second.representation &&
        first.payload.bits == second.payload.bits) {
        return true;
    }
    sxml_function_item_description first_description;
    sxml_function_item_description second_description;
    if (sxml_function_item_describe(first, &first_description) != SXML_STATUS_OK ||
        sxml_function_item_describe(second, &second_description) != SXML_STATUS_OK ||
        first_description.body.state != second_description.body.state ||
        first_description.body.operations != second_description.body.operations ||
        first_description.arity != second_description.arity ||
        first_description.function_name.index != second_description.function_name.index ||
        first_description.captured_values.sequence_count !=
            second_description.captured_values.sequence_count) {
        return false;
    }
    for (size_t value_index = 0;
         value_index < first_description.captured_values.sequence_count; value_index += 1) {
        const sxml_item first_value =
            first_description.captured_values.sequences[value_index].content;
        const sxml_item second_value =
            second_description.captured_values.sequences[value_index].content;
        if (first_value.representation != second_value.representation ||
            first_value.payload.bits != second_value.payload.bits ||
            first_value.type_or_node_index != second_value.type_or_node_index) {
            return false;
        }
    }
    return true;
}

static sxml_status compare_items(deep_comparison *comparison, sxml_item first,
                                 sxml_item second) {
    const sxml_item_kind kind = sxml_item_get_kind(first);
    if (kind != sxml_item_get_kind(second)) {
        comparison->is_equal = false;
        return SXML_STATUS_OK;
    }
    switch (kind) {
    case SXML_ITEM_KIND_ATOMIC:
        return compare_atomic(comparison, first, second);
    case SXML_ITEM_KIND_NODE: {
        node_pair pair;
        sxml_status status = sxml_item_get_node(first, &pair.first_node);
        if (status == SXML_STATUS_OK) {
            status = sxml_item_get_node(second, &pair.second_node);
        }
        return status == SXML_STATUS_OK ? compare_nodes(comparison, pair) : status;
    }
    case SXML_ITEM_KIND_JSON_NODE: {
        sxml_json_node_description first_description;
        sxml_json_node_description second_description;
        sxml_status status = sxml_json_node_describe(first, &first_description);
        if (status == SXML_STATUS_OK) {
            status = sxml_json_node_describe(second, &second_description);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return push_sequences(comparison, first_description.json_value,
                              second_description.json_value);
    }
    case SXML_ITEM_KIND_MAP:
    case SXML_ITEM_KIND_ARRAY: {
        size_t first_size = 0;
        size_t second_size = 0;
        sxml_status status = kind == SXML_ITEM_KIND_MAP ? sxml_map_get_size(first, &first_size)
                                                        : sxml_array_get_size(first, &first_size);
        if (status == SXML_STATUS_OK) {
            status = kind == SXML_ITEM_KIND_MAP ? sxml_map_get_size(second, &second_size)
                                                : sxml_array_get_size(second, &second_size);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (first_size != second_size) {
            comparison->is_equal = false;
            return SXML_STATUS_OK;
        }
        if (first_size == 0) {
            return SXML_STATUS_OK;
        }
        return push_pair(comparison,
                         &(comparison_pair){
                             .kind = kind == SXML_ITEM_KIND_MAP ? SXML_PAIR_KIND_MAPS : SXML_PAIR_KIND_ARRAYS,
                             .first_item = first,
                             .second_item = second,
                             .next_index = 0,
                             .count = first_size});
    }
    case SXML_ITEM_KIND_FUNCTION:
        comparison->is_equal = is_same_function(first, second);
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* --------------------------------------------------------------- Children */

/* What the next significant child unit of one side is. */
typedef enum unit_kind {
    SXML_UNIT_KIND_END,
    SXML_UNIT_KIND_NODE,
    SXML_UNIT_KIND_TEXT,
} unit_kind;

typedef struct child_unit {
    unit_kind kind;
    /* SXML_UNIT_KIND_NODE. */
    sxml_node_reference node;
} child_unit;

static void advance_position(child_position *position,
                             const sxml_node_description *description) {
    position->has_next_child = description->has_next_sibling;
    position->next_child.node_index = description->next_sibling;
}

/* Reads the next unit of one side: an element (or other node that is
   compared), or a run of text nodes merged into text, skipping the
   children the options ignore. */
/* One side of a children cursor: its position and its merged text. */
typedef struct child_side {
    child_position *position;
    sxml_library_text *text;
} child_side;

static sxml_status read_unit(const deep_comparison *comparison, child_side side,
                             child_unit *unit) {
    child_position *position = side.position;
    sxml_library_text *text = side.text;
    for (;;) {
        sxml_function_library_text_release(text);
        bool has_text = false;
        while (position->has_next_child) {
            sxml_node_description description;
            sxml_status status = sxml_node_describe(position->next_child, &description);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            const bool is_ignored =
                (description.node_kind == SXML_NODE_KIND_COMMENT &&
                 !comparison->options.compares_comments) ||
                (description.node_kind == SXML_NODE_KIND_PROCESSING_INSTRUCTION &&
                 !comparison->options.compares_processing_instructions);
            if (is_ignored) {
                advance_position(position, &description);
                continue;
            }
            if (description.node_kind != SXML_NODE_KIND_TEXT) {
                if (has_text) {
                    break;
                }
                *unit = (child_unit){.kind = SXML_UNIT_KIND_NODE, .node = position->next_child};
                advance_position(position, &description);
                return SXML_STATUS_OK;
            }
            sxml_byte_view content;
            status = sxml_node_get_content(position->next_child, &content);
            if (status == SXML_STATUS_OK) {
                status = sxml_function_library_text_append(text, content);
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            has_text = true;
            advance_position(position, &description);
        }
        if (!has_text) {
            *unit = (child_unit){.kind = SXML_UNIT_KIND_END, .node = {.tree = NULL}};
            return SXML_STATUS_OK;
        }
        if (comparison->options.whitespace == SXML_WHITESPACE_HANDLING_STRIP &&
            is_all_whitespace(sxml_function_library_text_view(text))) {
            continue;
        }
        *unit = (child_unit){.kind = SXML_UNIT_KIND_TEXT, .node = {.tree = NULL}};
        return SXML_STATUS_OK;
    }
}

/* One step of a children cursor: compares the next units of both sides,
   popping the cursor at their end. */
static sxml_status step_children(deep_comparison *comparison) {
    comparison_pair *pair = &comparison->pairs[comparison->pair_count - 1];
    child_unit first;
    child_unit second;
    sxml_status status = read_unit(
        comparison,
        (child_side){.position = &pair->first_position, .text = &comparison->first_text}, &first);
    if (status == SXML_STATUS_OK) {
        status = read_unit(
            comparison,
            (child_side){.position = &pair->second_position, .text = &comparison->second_text},
            &second);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (first.kind != second.kind) {
        comparison->is_equal = false;
        return SXML_STATUS_OK;
    }
    switch (first.kind) {
    case SXML_UNIT_KIND_END:
        comparison->pair_count -= 1;
        return SXML_STATUS_OK;
    case SXML_UNIT_KIND_TEXT:
        comparison->is_equal = texts_are_equal(
            &comparison->options,
            (text_pair){.first_text = sxml_function_library_text_view(&comparison->first_text),
                        .second_text =
                            sxml_function_library_text_view(&comparison->second_text)});
        return SXML_STATUS_OK;
    case SXML_UNIT_KIND_NODE:
        break;
    }
    return compare_nodes(comparison,
                         (node_pair){.first_node = first.node, .second_node = second.node});
}

/* ------------------------------------------------------------ The loop */

/* One step of the cursor on top. */
static sxml_status step_pair(deep_comparison *comparison) {
    comparison_pair *pair = &comparison->pairs[comparison->pair_count - 1];
    if (pair->kind == SXML_PAIR_KIND_CHILDREN) {
        return step_children(comparison);
    }
    if (pair->next_index == pair->count) {
        comparison->pair_count -= 1;
        return SXML_STATUS_OK;
    }
    const uint64_t index = pair->next_index;
    pair->next_index += 1;
    switch (pair->kind) {
    case SXML_PAIR_KIND_SEQUENCES: {
        sxml_item first = {.representation = 0};
        sxml_item second = {.representation = 0};
        sxml_status status = sxml_sequence_get_item(
            pair->first_sequence,
            (sxml_item_request){.item_index = index,
                                .error_record = comparison->call->error_record},
            &first);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(
                pair->second_sequence,
                (sxml_item_request){.item_index = index,
                                    .error_record = comparison->call->error_record},
                &second);
        }
        return status == SXML_STATUS_OK ? compare_items(comparison, first, second) : status;
    }
    case SXML_PAIR_KIND_ARRAYS: {
        sxml_sequence first = sxml_sequence_empty();
        sxml_sequence second = sxml_sequence_empty();
        sxml_status status = sxml_array_get_member(pair->first_item, (size_t)index, &first);
        if (status == SXML_STATUS_OK) {
            status = sxml_array_get_member(pair->second_item, (size_t)index, &second);
        }
        return status == SXML_STATUS_OK ? push_sequences(comparison, first, second) : status;
    }
    case SXML_PAIR_KIND_MAPS: {
        const sxml_item second_map = pair->second_item;
        const bool compares_order = comparison->options.compares_map_order;
        sxml_map_entry entry;
        sxml_status status = sxml_map_get_entry(pair->first_item, (size_t)index, &entry);
        sxml_map_lookup lookup = {.is_found = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_map_find(second_map, entry.key, &lookup);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!lookup.is_found || (compares_order && lookup.entry_index != index)) {
            comparison->is_equal = false;
            return SXML_STATUS_OK;
        }
        return push_sequences(comparison, entry.value, lookup.entry.value);
    }
    case SXML_PAIR_KIND_CHILDREN:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

sxml_status sxml_function_library_call_deep_equal(const sxml_function_call *call,
                                                  sxml_sequence *result) {
    deep_comparison comparison = {.call = call,
                                  .pairs = NULL,
                                  .pair_count = 0,
                                  .pair_capacity = 0,
                                  .is_equal = true,
                                  .first_text = {.item_heap = call->item_heap},
                                  .second_text = {.item_heap = call->item_heap}};
    sxml_status status = read_options(call, &comparison.options);
    if (status == SXML_STATUS_OK) {
        status = push_sequences(&comparison, sxml_function_library_get_argument(call, 0),
                                sxml_function_library_get_argument(call, 1));
    }
    while (status == SXML_STATUS_OK && comparison.is_equal && comparison.pair_count > 0) {
        status = step_pair(&comparison);
    }
    sxml_item_heap_release_bytes(
        call->item_heap,
        (sxml_heap_bytes){.memory = comparison.pairs,
                          .byte_count = comparison.pair_capacity * sizeof(comparison_pair)});
    sxml_function_library_text_release(&comparison.first_text);
    sxml_function_library_text_release(&comparison.second_text);
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = comparison.is_equal});
    }
    return status;
}
