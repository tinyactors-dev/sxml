/*
 * fn:deep-equal($input1, $input2, $options := {}) (F&O 4.0, section
 * 2.2.4), the deep equality of two sequences (packet 1.20, D78; the 4.0
 * options from packet 2.13, D116).
 *
 * Golden file: src/memory.c (layout and comments); the resumable body as
 * fn:sort in src/function_library_sequence.c (D75).
 *
 * No recursion (D71): nested arrays, maps, JNode values, captured values
 * of function items, attributes and node children are compared on an
 * explicit stack of comparison pairs in item heap memory. Each pair is a
 * cursor over two collections; the loop advances the top cursor by one
 * step, compares the two items it yields, and pushes a new cursor for
 * items that have parts. Outside an unordered comparison the first
 * difference ends the comparison.
 *
 * Unordered comparisons (the "ordered": false option for the top-level
 * sequences, and the children of elements named in "unordered-elements")
 * are cursors of their own: for each unit of the first side they try the
 * unmatched units of the second side in turn, each try a trial. A
 * difference found while a trial runs drops the cursors pushed above the
 * trial's cursor and moves on to the next candidate (backtracking on the
 * same stack); when no candidate is left the difference stands. Matches
 * are kept (first fit), which finds a pairing whenever deep equality is an
 * equivalence, as it is without "items-equal".
 *
 * The "items-equal" callback makes the body resumable (D75): when two
 * items are about to be compared, the step asks the evaluator to call the
 * function and the next step reads its answer (true, false, or the empty
 * sequence for the normal rules).
 *
 * Rules (F&O 4.0, section 2.2.4):
 * - strings, xs:anyURI and xs:untypedAtomic by equal-strings: after
 *   fn:normalize-space with whitespace "normalize", after
 *   fn:normalize-unicode with a normalization form, under the collation;
 * - other atomic items: fn:compare with the implicit timezone, NaN equal to
 *   NaN; items that do not compare are not equal (no error); prefixes of
 *   QNames with namespace-prefixes, timezones with timezones, and type
 *   annotations with type-annotations;
 * - XNodes: same kind (and base URI with base-uri); documents by
 *   significant children; elements by name (prefix, in-scope namespaces,
 *   type annotation, is-id, is-idrefs and nilled with their options),
 *   attributes as a set and significant children (in any order when
 *   unordered); attributes, text, comments and processing instructions by
 *   name and equal-strings; namespace nodes by name and codepoints.
 *   Significant children drop comments and processing instructions unless
 *   their options say otherwise, merge adjacent text, and with whitespace
 *   "strip" or "normalize" drop whitespace-only text outside the scope of
 *   xml:space="preserve";
 * - JNodes: by their ·jvalue·s;
 * - maps: the same significant entries (with ignore-empty-entries an
 *   entry whose value is empty is not significant), values deep equal, in
 *   the same order with map-order;
 * - arrays: same size, members deep equal in order;
 * - other function items: the same item, or the same body, arity, name
 *   and signature with deep-equal captured values (so two references to a
 *   function, or two partial applications of it to equal arguments, are
 *   equal; two inline functions written separately are not compared for
 *   equivalence and differ).
 *
 * Without schemas, typed-values and type-variety make no difference, and
 * debug writes nothing.
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "unicode_internal.h"

#include <stdalign.h>
#include <string.h>

/* ---------------------------------------------------------------- Options */

typedef enum whitespace_handling {
    SXML_WHITESPACE_HANDLING_PRESERVE,
    /* Whitespace-only text runs among children are ignored. */
    SXML_WHITESPACE_HANDLING_STRIP,
    /* As strip, and strings compare after fn:normalize-space. */
    SXML_WHITESPACE_HANDLING_NORMALIZE,
} whitespace_handling;

typedef struct deep_equal_options {
    bool compares_comments;
    bool compares_processing_instructions;
    bool compares_map_order;
    bool compares_base_uri;
    bool compares_in_scope_namespaces;
    bool compares_namespace_prefixes;
    bool compares_timezones;
    bool compares_type_annotations;
    bool compares_id_property;
    bool compares_idrefs_property;
    bool compares_nilled_property;
    bool ignores_empty_entries;
    bool is_ordered;
    whitespace_handling whitespace;
    int16_t implicit_timezone_offset_minutes;
    sxml_collation collation;
    bool has_normalization_form;
    sxml_unicode_normalization_form normalization_form;
    bool has_items_equal;
    /* Borrowed from the options map. */
    sxml_item items_equal;
    /* xs:QName items, borrowed from the options map. */
    sxml_sequence unordered_elements;
} deep_equal_options;

static const sxml_static_error invalid_option_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An option of fn:deep-equal has a value of the wrong type (F&O 4.0, "
        "section 2.2.4)"),
};

static const sxml_static_error invalid_callback_result_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The items-equal function of fn:deep-equal returned a value that is not "
        "xs:boolean? (F&O 4.0, section 2.2.4)"),
};

/* The boolean options and where they go. */
typedef enum boolean_option_kind {
    SXML_BOOLEAN_OPTION_KIND_BASE_URI,
    SXML_BOOLEAN_OPTION_KIND_COMMENTS,
    SXML_BOOLEAN_OPTION_KIND_DEBUG,
    SXML_BOOLEAN_OPTION_KIND_FALSE_ON_ERROR,
    SXML_BOOLEAN_OPTION_KIND_ID_PROPERTY,
    SXML_BOOLEAN_OPTION_KIND_IDREFS_PROPERTY,
    SXML_BOOLEAN_OPTION_KIND_IGNORE_EMPTY_ENTRIES,
    SXML_BOOLEAN_OPTION_KIND_IN_SCOPE_NAMESPACES,
    SXML_BOOLEAN_OPTION_KIND_MAP_ORDER,
    SXML_BOOLEAN_OPTION_KIND_NAMESPACE_PREFIXES,
    SXML_BOOLEAN_OPTION_KIND_NILLED_PROPERTY,
    SXML_BOOLEAN_OPTION_KIND_ORDERED,
    SXML_BOOLEAN_OPTION_KIND_PROCESSING_INSTRUCTIONS,
    SXML_BOOLEAN_OPTION_KIND_TIMEZONES,
    SXML_BOOLEAN_OPTION_KIND_TYPE_ANNOTATIONS,
    SXML_BOOLEAN_OPTION_KIND_TYPE_VARIETY,
    SXML_BOOLEAN_OPTION_KIND_TYPED_VALUES,
} boolean_option_kind;

typedef struct boolean_option_row {
    sxml_byte_view option_name;
    boolean_option_kind option_kind;
} boolean_option_row;

static const boolean_option_row boolean_options[] = {
    {SXML_BYTE_VIEW_INITIALIZER("base-uri"), SXML_BOOLEAN_OPTION_KIND_BASE_URI},
    {SXML_BYTE_VIEW_INITIALIZER("comments"), SXML_BOOLEAN_OPTION_KIND_COMMENTS},
    {SXML_BYTE_VIEW_INITIALIZER("debug"), SXML_BOOLEAN_OPTION_KIND_DEBUG},
    {SXML_BYTE_VIEW_INITIALIZER("false-on-error"), SXML_BOOLEAN_OPTION_KIND_FALSE_ON_ERROR},
    {SXML_BYTE_VIEW_INITIALIZER("id-property"), SXML_BOOLEAN_OPTION_KIND_ID_PROPERTY},
    {SXML_BYTE_VIEW_INITIALIZER("idrefs-property"), SXML_BOOLEAN_OPTION_KIND_IDREFS_PROPERTY},
    {SXML_BYTE_VIEW_INITIALIZER("ignore-empty-entries"),
     SXML_BOOLEAN_OPTION_KIND_IGNORE_EMPTY_ENTRIES},
    {SXML_BYTE_VIEW_INITIALIZER("in-scope-namespaces"),
     SXML_BOOLEAN_OPTION_KIND_IN_SCOPE_NAMESPACES},
    {SXML_BYTE_VIEW_INITIALIZER("map-order"), SXML_BOOLEAN_OPTION_KIND_MAP_ORDER},
    {SXML_BYTE_VIEW_INITIALIZER("namespace-prefixes"),
     SXML_BOOLEAN_OPTION_KIND_NAMESPACE_PREFIXES},
    {SXML_BYTE_VIEW_INITIALIZER("nilled-property"), SXML_BOOLEAN_OPTION_KIND_NILLED_PROPERTY},
    {SXML_BYTE_VIEW_INITIALIZER("ordered"), SXML_BOOLEAN_OPTION_KIND_ORDERED},
    {SXML_BYTE_VIEW_INITIALIZER("processing-instructions"),
     SXML_BOOLEAN_OPTION_KIND_PROCESSING_INSTRUCTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("timezones"), SXML_BOOLEAN_OPTION_KIND_TIMEZONES},
    {SXML_BYTE_VIEW_INITIALIZER("type-annotations"), SXML_BOOLEAN_OPTION_KIND_TYPE_ANNOTATIONS},
    {SXML_BYTE_VIEW_INITIALIZER("type-variety"), SXML_BOOLEAN_OPTION_KIND_TYPE_VARIETY},
    {SXML_BYTE_VIEW_INITIALIZER("typed-values"), SXML_BOOLEAN_OPTION_KIND_TYPED_VALUES},
};

/* One boolean option's value to set. */
typedef struct boolean_setting {
    boolean_option_kind option_kind;
    bool value;
} boolean_setting;

static void set_boolean_option(deep_equal_options *options, boolean_setting setting) {
    const bool value = setting.value;
    switch (setting.option_kind) {
    case SXML_BOOLEAN_OPTION_KIND_BASE_URI:
        options->compares_base_uri = value;
        return;
    case SXML_BOOLEAN_OPTION_KIND_COMMENTS:
        options->compares_comments = value;
        return;
    case SXML_BOOLEAN_OPTION_KIND_ID_PROPERTY:
        options->compares_id_property = value;
        return;
    case SXML_BOOLEAN_OPTION_KIND_IDREFS_PROPERTY:
        options->compares_idrefs_property = value;
        return;
    case SXML_BOOLEAN_OPTION_KIND_IGNORE_EMPTY_ENTRIES:
        options->ignores_empty_entries = value;
        return;
    case SXML_BOOLEAN_OPTION_KIND_IN_SCOPE_NAMESPACES:
        options->compares_in_scope_namespaces = value;
        return;
    case SXML_BOOLEAN_OPTION_KIND_MAP_ORDER:
        options->compares_map_order = value;
        return;
    case SXML_BOOLEAN_OPTION_KIND_NAMESPACE_PREFIXES:
        options->compares_namespace_prefixes = value;
        return;
    case SXML_BOOLEAN_OPTION_KIND_NILLED_PROPERTY:
        options->compares_nilled_property = value;
        return;
    case SXML_BOOLEAN_OPTION_KIND_ORDERED:
        options->is_ordered = value;
        return;
    case SXML_BOOLEAN_OPTION_KIND_PROCESSING_INSTRUCTIONS:
        options->compares_processing_instructions = value;
        return;
    case SXML_BOOLEAN_OPTION_KIND_TIMEZONES:
        options->compares_timezones = value;
        return;
    case SXML_BOOLEAN_OPTION_KIND_TYPE_ANNOTATIONS:
        options->compares_type_annotations = value;
        return;
    case SXML_BOOLEAN_OPTION_KIND_DEBUG:
    case SXML_BOOLEAN_OPTION_KIND_FALSE_ON_ERROR:
    case SXML_BOOLEAN_OPTION_KIND_TYPE_VARIETY:
    case SXML_BOOLEAN_OPTION_KIND_TYPED_VALUES:
        /* No effect without schemas (and no diagnostics). */
        return;
    }
}

/* Every string-keyed option of fn:deep-equal (F&O 4.0, section 2.2.4). */
static const sxml_byte_view other_options[] = {
    SXML_BYTE_VIEW_INITIALIZER("collation"),
    SXML_BYTE_VIEW_INITIALIZER("items-equal"),
    SXML_BYTE_VIEW_INITIALIZER("normalization-form"),
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

static bool is_known_option(sxml_byte_view name) {
    for (size_t row_index = 0; row_index < sizeof(boolean_options) / sizeof(boolean_options[0]); row_index += 1) {
        if (sxml_byte_view_equals(name, boolean_options[row_index].option_name)) {
            return true;
        }
    }
    for (size_t row_index = 0; row_index < sizeof(other_options) / sizeof(other_options[0]); row_index += 1) {
        if (sxml_byte_view_equals(name, other_options[row_index])) {
            return true;
        }
    }
    return false;
}

/* An option to look up in the options map. */
typedef struct option_query {
    sxml_item options;
    sxml_byte_view option_name;
} option_query;

/* The value of the option, borrowed from the map; empty when absent. */
static sxml_status find_option(const sxml_function_call *call, option_query query,
                               sxml_sequence *value) {
    sxml_sequence key = sxml_sequence_empty();
    sxml_status status =
        sxml_function_library_make_string(call->item_heap, query.option_name, &key);
    sxml_map_lookup lookup = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_find(query.options, key.content, &lookup);
        sxml_sequence_release(key);
    }
    if (status == SXML_STATUS_OK) {
        *value = lookup.is_found ? lookup.entry.value : sxml_sequence_empty();
    }
    return status;
}

/* The one item of value, or false when it has none or several. */
static bool read_single_item(sxml_sequence value, sxml_item *item) {
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        return false;
    }
    *item = value.content;
    return true;
}

static bool is_whitespace_byte(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* text without leading and trailing whitespace. */
static sxml_byte_view trim_text(sxml_byte_view text) {
    size_t first = 0;
    size_t end = text.byte_count;
    while (first < end && is_whitespace_byte(text.bytes[first])) {
        first += 1;
    }
    while (end > first && is_whitespace_byte(text.bytes[end - 1])) {
        end -= 1;
    }
    if (first == end) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = text.bytes + first, .byte_count = end - first};
}

/* A boolean option's value after coercion. */
typedef struct boolean_value {
    bool is_true;
} boolean_value;

/* Coerces a boolean option's value (F&O 4.0, section 1.5.3: the coercion
   rules, so a node or xs:untypedAtomic is cast from its string). */
static sxml_status read_boolean_value(const sxml_function_call *call, sxml_sequence value,
                                      boolean_value *coerced) {
    sxml_item item;
    sxml_sequence atomized = sxml_sequence_empty();
    sxml_status status = SXML_STATUS_OK;
    if (read_single_item(value, &item) && sxml_item_get_kind(item) == SXML_ITEM_KIND_NODE &&
        call->dynamic_context != NULL) {
        status = sxml_evaluator_atomize(
            call->dynamic_context,
            &(sxml_atomization_request){.value = value, .error_record = call->error_record},
            &atomized);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        value = atomized;
    }
    sxml_atomic_datum datum;
    bool is_valid = read_single_item(value, &item) &&
                    sxml_item_get_kind(item) == SXML_ITEM_KIND_ATOMIC &&
                    sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK;
    if (is_valid && datum.datum_kind == SXML_DATUM_KIND_BOOLEAN) {
        coerced->is_true = datum.datum_details.boolean;
    } else if (is_valid && datum.datum_kind == SXML_DATUM_KIND_STRING &&
               sxml_item_get_type_annotation(item).index ==
                   sxml_builtin_type_identifier(SXML_TYPE_XS_UNTYPED_ATOMIC).index) {
        const sxml_byte_view text = trim_text(datum.datum_details.string.utf8);
        coerced->is_true = sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("true")) ||
                           sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("1"));
        is_valid = coerced->is_true || sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("false")) ||
                   sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("0"));
    } else {
        is_valid = false;
    }
    sxml_sequence_release(atomized);
    return is_valid ? SXML_STATUS_OK : sxml_function_library_report(call, &invalid_option_error);
}

/* Reads a string option into *text; *is_present is false when absent. */
typedef struct string_option {
    bool is_present;
    sxml_string text;
} string_option;

static sxml_status read_string_option(const sxml_function_call *call, option_query query,
                                      string_option *option) {
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = find_option(call, query, &value);
    *option = (string_option){.is_present = false, .text = {.utf8 = {.bytes = NULL}}};
    if (status != SXML_STATUS_OK ||
        sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        return status;
    }
    sxml_item item;
    sxml_atomic_datum datum;
    if (!read_single_item(value, &item) || sxml_item_get_kind(item) != SXML_ITEM_KIND_ATOMIC ||
        sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return sxml_function_library_report(call, &invalid_option_error);
    }
    *option = (string_option){.is_present = true, .text = datum.datum_details.string};
    return SXML_STATUS_OK;
}

/* The forms of fn:normalize-unicode (F&O 4.0, section 5.3.14). */
typedef struct normalization_form_row {
    sxml_byte_view form_name;
    sxml_unicode_normalization_form normalization_form;
} normalization_form_row;

static const normalization_form_row normalization_forms[] = {
    {SXML_BYTE_VIEW_INITIALIZER("NFC"), SXML_UNICODE_NORMALIZATION_FORM_NFC},
    {SXML_BYTE_VIEW_INITIALIZER("NFD"), SXML_UNICODE_NORMALIZATION_FORM_NFD},
    {SXML_BYTE_VIEW_INITIALIZER("NFKC"), SXML_UNICODE_NORMALIZATION_FORM_NFKC},
    {SXML_BYTE_VIEW_INITIALIZER("NFKD"), SXML_UNICODE_NORMALIZATION_FORM_NFKD},
    {SXML_BYTE_VIEW_INITIALIZER("FULLY-NORMALIZED"),
     SXML_UNICODE_NORMALIZATION_FORM_FULLY_NORMALIZED},
};

static sxml_status read_text_options(const sxml_function_call *call, sxml_item options,
                                     deep_equal_options *result) {
    string_option option;
    sxml_status status = read_string_option(
        call,
        (option_query){.options = options,
                       .option_name = SXML_BYTE_VIEW_FROM_LITERAL("whitespace")},
        &option);
    if (status == SXML_STATUS_OK && option.is_present) {
        if (sxml_byte_view_equals(option.text.utf8, SXML_BYTE_VIEW_FROM_LITERAL("strip"))) {
            result->whitespace = SXML_WHITESPACE_HANDLING_STRIP;
        } else if (sxml_byte_view_equals(option.text.utf8,
                                         SXML_BYTE_VIEW_FROM_LITERAL("normalize"))) {
            result->whitespace = SXML_WHITESPACE_HANDLING_NORMALIZE;
        } else if (!sxml_byte_view_equals(option.text.utf8,
                                          SXML_BYTE_VIEW_FROM_LITERAL("preserve"))) {
            status = sxml_function_library_report(call, &invalid_option_error);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = read_string_option(
            call,
            (option_query){.options = options,
                           .option_name = SXML_BYTE_VIEW_FROM_LITERAL("normalization-form")},
            &option);
    }
    if (status == SXML_STATUS_OK && option.is_present && option.text.utf8.byte_count > 0) {
        const sxml_byte_view form = trim_text(option.text.utf8);
        bool is_found = false;
        for (size_t row_index = 0; row_index < sizeof(normalization_forms) / sizeof(normalization_forms[0]);
             row_index += 1) {
            if (sxml_byte_view_equals(form, normalization_forms[row_index].form_name)) {
                result->has_normalization_form = true;
                result->normalization_form = normalization_forms[row_index].normalization_form;
                is_found = true;
            }
        }
        if (!is_found) {
            status = sxml_function_library_report(call, &invalid_option_error);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = read_string_option(
            call,
            (option_query){.options = options,
                           .option_name = SXML_BYTE_VIEW_FROM_LITERAL("collation")},
            &option);
    }
    if (status == SXML_STATUS_OK && option.is_present) {
        /* F&O 4.0, section 5.2.7, through the one resolver (D116). */
        status = sxml_function_library_find_collation(call, option.text.utf8,
                                                      &result->collation);
    }
    return status;
}

static sxml_status read_callback_options(const sxml_function_call *call, sxml_item options,
                                         deep_equal_options *result) {
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = find_option(
        call,
        (option_query){.options = options,
                       .option_name = SXML_BYTE_VIEW_FROM_LITERAL("items-equal")},
        &value);
    sxml_item item;
    if (status == SXML_STATUS_OK &&
        sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        if (!read_single_item(value, &item) ||
            sxml_item_get_kind(item) != SXML_ITEM_KIND_FUNCTION) {
            return sxml_function_library_report(call, &invalid_option_error);
        }
        result->has_items_equal = true;
        result->items_equal = item;
    }
    if (status == SXML_STATUS_OK) {
        status = find_option(
            call,
            (option_query){.options = options,
                           .option_name = SXML_BYTE_VIEW_FROM_LITERAL("unordered-elements")},
            &value);
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_atomic_datum datum;
        if (sxml_item_get_kind(step.item) != SXML_ITEM_KIND_ATOMIC ||
            sxml_atomic_item_get_datum(step.item, &datum) != SXML_STATUS_OK ||
            datum.datum_kind != SXML_DATUM_KIND_QNAME) {
            return sxml_function_library_report(call, &invalid_option_error);
        }
    }
    if (status == SXML_STATUS_OK) {
        result->unordered_elements = value;
    }
    return status;
}

/* Reads an options map (F&O 4.0, section 1.5.3, option parameter
   conventions): string keys must name options; keys of other types (QNames
   of implementation-defined options) are ignored. */
static sxml_status read_options_map(const sxml_function_call *call, sxml_item options,
                                    deep_equal_options *result) {
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
        if (!is_known_option(datum.datum_details.string.utf8)) {
            return sxml_function_library_report(call, &unknown_option_error);
        }
    }
    for (size_t row_index = 0; status == SXML_STATUS_OK &&
                         row_index < sizeof(boolean_options) / sizeof(boolean_options[0]);
         row_index += 1) {
        sxml_sequence value = sxml_sequence_empty();
        status = find_option(call,
                             (option_query){.options = options,
                                            .option_name = boolean_options[row_index].option_name},
                             &value);
        boolean_value coerced = {.is_true = false};
        if (status == SXML_STATUS_OK &&
            sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_EMPTY) {
            status = read_boolean_value(call, value, &coerced);
            if (status == SXML_STATUS_OK) {
                set_boolean_option(result,
                                   (boolean_setting){.option_kind = boolean_options[row_index].option_kind,
                                                     .value = coerced.is_true});
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        status = read_text_options(call, options, result);
    }
    if (status == SXML_STATUS_OK) {
        status = read_callback_options(call, options, result);
    }
    return status;
}

static sxml_status read_options(const sxml_function_call *call,
                                deep_equal_options *options) {
    *options = (deep_equal_options){.whitespace = SXML_WHITESPACE_HANDLING_PRESERVE,
                                    .is_ordered = true,
                                    .collation = sxml_collation_codepoint(),
                                    .unordered_elements = sxml_sequence_empty()};
    sxml_status status = SXML_STATUS_OK;
    if (call->dynamic_context != NULL) {
        status = sxml_dynamic_context_read_implicit_timezone(
            call->dynamic_context, &options->implicit_timezone_offset_minutes);
    }
    sxml_optional_item argument = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 2, &argument);
    }
    if (status == SXML_STATUS_OK && !argument.has_item) {
        /* The default collation (F&O 4.0, section 5.2.7). */
        return sxml_function_library_find_collation(
            call, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &options->collation);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sxml_item_get_kind(argument.item) == SXML_ITEM_KIND_MAP) {
        status = sxml_function_library_find_collation(
            call, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &options->collation);
        return status == SXML_STATUS_OK ? read_options_map(call, argument.item, options)
                                        : status;
    }
    /* A string: the collation (F&O 4.0, fn:deep-equal, $options). */
    return sxml_function_library_resolve_collation(call, 2, &options->collation);
}

/* ---------------------------------------------------- Comparison state */

/* Where a children cursor is on one side. */
typedef struct child_position {
    sxml_node_reference next_child;
    bool has_next_child;
} child_position;

/* What one significant child (or top-level item) of a side is. */
typedef enum unit_kind {
    SXML_UNIT_KIND_END,
    SXML_UNIT_KIND_NODE,
    SXML_UNIT_KIND_TEXT,
    /* A top-level item of an unordered comparison. */
    SXML_UNIT_KIND_ITEM,
} unit_kind;

typedef struct comparison_unit {
    unit_kind kind;
    /* ITEM: borrowed from the arguments. */
    sxml_item item;
    /* NODE; TEXT when has_single_node: the one text node of the run. */
    sxml_node_reference node;
    bool has_single_node;
    /* TEXT in an unordered cursor: heap bytes the cursor owns. */
    uint8_t *text_bytes;
    size_t text_byte_count;
} comparison_unit;

/* Units in item heap memory. */
typedef struct unit_list {
    comparison_unit *units;
    size_t unit_count;
    size_t unit_capacity;
} unit_list;

typedef enum pair_kind {
    /* Two sequences, item by item. */
    SXML_PAIR_KIND_SEQUENCES,
    /* Two arrays, member by member. */
    SXML_PAIR_KIND_ARRAYS,
    /* Two maps, entry by entry of the first. */
    SXML_PAIR_KIND_MAPS,
    /* The captured values of two function items, value by value. */
    SXML_PAIR_KIND_CAPTURED_VALUES,
    /* The attributes of two elements, by name. */
    SXML_PAIR_KIND_ATTRIBUTES,
    /* The significant children of two documents or elements, in order. */
    SXML_PAIR_KIND_CHILDREN,
    /* Two lists of units in any order. */
    SXML_PAIR_KIND_UNORDERED,
} pair_kind;

/* One cursor of the explicit stack (glossary `comparison_pair`). Every
   value is borrowed from the arguments of the call, except the unit lists
   and matches of an unordered cursor. */
typedef struct comparison_pair {
    pair_kind kind;
    sxml_sequence first_sequence;
    sxml_sequence second_sequence;
    sxml_item first_item;
    sxml_item second_item;
    sxml_node_reference first_node;
    sxml_node_reference second_node;
    child_position first_position;
    child_position second_position;
    /* CHILDREN: within xml:space="preserve". */
    bool preserves_space;
    uint64_t next_index;
    uint64_t count;
    /* MAPS: significant entries of the first map seen so far. */
    uint64_t significant_index;
    /* UNORDERED. */
    unit_list first_units;
    unit_list second_units;
    bool *is_matched;
    uint64_t candidate_index;
    bool is_in_trial;
} comparison_pair;

/* The state of one deep-equal call: the resumable body's state. */
typedef struct deep_comparison {
    /* Borrowed for each step. */
    const sxml_function_call *call;
    sxml_item_heap *item_heap;
    deep_equal_options options;
    comparison_pair *pairs;
    size_t pair_count;
    size_t pair_capacity;
    bool is_equal;
    /* Merged text runs of the current children units. */
    sxml_library_text first_text;
    sxml_library_text second_text;
    /* The items-equal call asked for: its two arguments, owned. */
    bool has_pending_call;
    sxml_sequence call_arguments[2];
} deep_comparison;

static void release_units(deep_comparison *comparison, unit_list *list) {
    for (size_t unit_index = 0; unit_index < list->unit_count; unit_index += 1) {
        sxml_item_heap_release_bytes(
            comparison->item_heap,
            (sxml_heap_bytes){.memory = list->units[unit_index].text_bytes,
                              .byte_count = list->units[unit_index].text_byte_count});
    }
    sxml_item_heap_release_bytes(
        comparison->item_heap,
        (sxml_heap_bytes){.memory = list->units,
                          .byte_count = list->unit_capacity * sizeof(comparison_unit)});
    *list = (unit_list){.units = NULL, .unit_count = 0, .unit_capacity = 0};
}

/* Releases what an unordered cursor owns. */
static void release_pair(deep_comparison *comparison, comparison_pair *pair) {
    if (pair->kind != SXML_PAIR_KIND_UNORDERED) {
        return;
    }
    sxml_item_heap_release_bytes(
        comparison->item_heap,
        (sxml_heap_bytes){.memory = pair->is_matched,
                          .byte_count = pair->second_units.unit_capacity * sizeof(bool)});
    pair->is_matched = NULL;
    release_units(comparison, &pair->first_units);
    release_units(comparison, &pair->second_units);
}

/* Pops cursors until pair_count are left. */
static void truncate_pairs(deep_comparison *comparison, size_t pair_count) {
    while (comparison->pair_count > pair_count) {
        comparison->pair_count -= 1;
        release_pair(comparison, &comparison->pairs[comparison->pair_count]);
    }
}

static void release_comparison(deep_comparison *comparison) {
    if (comparison->item_heap == NULL) {
        return;
    }
    truncate_pairs(comparison, 0);
    sxml_item_heap_release_bytes(
        comparison->item_heap,
        (sxml_heap_bytes){.memory = comparison->pairs,
                          .byte_count = comparison->pair_capacity * sizeof(comparison_pair)});
    comparison->pairs = NULL;
    comparison->pair_capacity = 0;
    sxml_function_library_text_release(&comparison->first_text);
    sxml_function_library_text_release(&comparison->second_text);
    sxml_sequence_release(comparison->call_arguments[0]);
    sxml_sequence_release(comparison->call_arguments[1]);
    comparison->call_arguments[0] = sxml_sequence_empty();
    comparison->call_arguments[1] = sxml_sequence_empty();
}

static sxml_status push_pair(deep_comparison *comparison, const comparison_pair *pair) {
    if (comparison->pair_count == comparison->pair_capacity) {
        const size_t capacity =
            comparison->pair_capacity == 0 ? 16 : comparison->pair_capacity * 2;
        size_t byte_count = 0;
        sxml_status status = sxml_size_multiply(capacity, sizeof(comparison_pair), &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_item_heap_allocate_bytes(
                comparison->item_heap,
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
            comparison->item_heap,
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

/* ------------------------------------------------------- Comparing text */

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

/* A string after the whitespace and normalization-form options. */
typedef struct prepared_text {
    sxml_library_text spaced;
    sxml_unicode_normalized_text normalized;
    sxml_string string;
} prepared_text;

static void release_prepared_text(prepared_text *text) {
    sxml_function_library_text_release(&text->spaced);
    sxml_unicode_normalized_text_release(&text->normalized);
}

/* Applies fn:normalize-space and fn:normalize-unicode as the options ask
   (F&O 4.0, fn:deep-equal, equal-strings). */
static sxml_status prepare_text(const deep_comparison *comparison, sxml_byte_view input,
                                prepared_text *text) {
    *text = (prepared_text){.spaced = {.item_heap = comparison->item_heap},
                            .normalized = {.allocator =
                                               sxml_item_heap_as_allocator(comparison->item_heap),
                                           .bytes = NULL,
                                           .byte_count = 0}};
    sxml_byte_view current = input;
    sxml_status status = SXML_STATUS_OK;
    if (comparison->options.whitespace == SXML_WHITESPACE_HANDLING_NORMALIZE) {
        normalized_reader reader = {.text = input, .byte_index = 0};
        for (int32_t next_byte = next_normalized_byte(&reader);
             status == SXML_STATUS_OK && next_byte >= 0; next_byte = next_normalized_byte(&reader)) {
            const uint8_t value = (uint8_t)next_byte;
            status = sxml_function_library_text_append(
                &text->spaced, (sxml_byte_view){.bytes = &value, .byte_count = 1});
        }
        current = sxml_function_library_text_view(&text->spaced);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_string_from_utf8(current, &text->string);
    }
    if (status == SXML_STATUS_OK && comparison->options.has_normalization_form) {
        status = sxml_unicode_normalize(
            &(sxml_unicode_normalization_request){
                .allocator = sxml_item_heap_as_allocator(comparison->item_heap),
                .normalization_form = comparison->options.normalization_form,
                .input = text->string},
            &text->normalized);
        if (status == SXML_STATUS_OK) {
            status = sxml_string_from_utf8(sxml_unicode_normalized_text_view(&text->normalized),
                                           &text->string);
        }
    }
    if (status != SXML_STATUS_OK) {
        release_prepared_text(text);
    }
    return status;
}

typedef struct text_pair {
    sxml_byte_view first_text;
    sxml_byte_view second_text;
} text_pair;

/* equal-strings of F&O 4.0, fn:deep-equal: sets is_equal. */
static sxml_status compare_strings(deep_comparison *comparison, text_pair pair) {
    if (comparison->options.whitespace != SXML_WHITESPACE_HANDLING_NORMALIZE &&
        !comparison->options.has_normalization_form &&
        sxml_collation_is_codepoint_order(&comparison->options.collation)) {
        comparison->is_equal = sxml_byte_view_equals(pair.first_text, pair.second_text);
        return SXML_STATUS_OK;
    }
    prepared_text first;
    prepared_text second;
    sxml_status status = prepare_text(comparison, pair.first_text, &first);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = prepare_text(comparison, pair.second_text, &second);
    if (status != SXML_STATUS_OK) {
        release_prepared_text(&first);
        return status;
    }
    sxml_ordering ordering = SXML_ORDERING_LESS;
    status = sxml_collation_compare(
        &(sxml_collation_comparison){
            .collation = &comparison->options.collation,
            .allocator = sxml_item_heap_as_allocator(comparison->item_heap),
            .first = first.string,
            .second = second.string},
        &ordering);
    if (status == SXML_STATUS_OK) {
        comparison->is_equal = ordering == SXML_ORDERING_EQUAL;
    }
    release_prepared_text(&first);
    release_prepared_text(&second);
    return status;
}

static bool is_all_whitespace(sxml_byte_view text) {
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        if (!is_whitespace_byte(text.bytes[byte_index])) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------ Comparing items */

static bool is_date_time(sxml_atomic_datum datum) {
    return datum.datum_kind == SXML_DATUM_KIND_DATE_TIME;
}

static sxml_status compare_atomic(deep_comparison *comparison, sxml_item first,
                                  sxml_item second) {
    const deep_equal_options *options = &comparison->options;
    if (options->compares_type_annotations &&
        sxml_item_get_type_annotation(first).index != sxml_item_get_type_annotation(second).index) {
        comparison->is_equal = false;
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum first_datum;
    sxml_atomic_datum second_datum;
    sxml_status status = sxml_atomic_item_get_datum(first, &first_datum);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(second, &second_datum);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (first_datum.datum_kind == SXML_DATUM_KIND_STRING &&
        second_datum.datum_kind == SXML_DATUM_KIND_STRING) {
        return compare_strings(
            comparison, (text_pair){.first_text = first_datum.datum_details.string.utf8,
                                    .second_text = second_datum.datum_details.string.utf8});
    }
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    status = sxml_atomic_item_compare_in_heap(
        comparison->item_heap,
        &(sxml_atomic_comparison){
            .first = first,
            .second = second,
            .implicit_timezone_offset_minutes = options->implicit_timezone_offset_minutes,
            .error_record = NULL,
            .name_pool = sxml_item_heap_get_name_pool(comparison->item_heap)},
        &ordering);
    if (status == SXML_STATUS_TYPE_ERROR || status == SXML_STATUS_INVALID_ARGUMENT ||
        status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        /* Items that do not compare are not deep-equal (F&O 4.0, section
           2.2.4). */
        comparison->is_equal = false;
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    bool is_equal = ordering == SXML_ORDERING_EQUAL;
    if (is_equal && options->compares_namespace_prefixes &&
        first_datum.datum_kind == SXML_DATUM_KIND_QNAME &&
        second_datum.datum_kind == SXML_DATUM_KIND_QNAME) {
        is_equal = first_datum.datum_details.qname.namespace_prefix.index ==
                   second_datum.datum_details.qname.namespace_prefix.index;
    }
    if (is_equal && options->compares_timezones && is_date_time(first_datum) &&
        is_date_time(second_datum)) {
        const sxml_date_time *first_value = &first_datum.datum_details.date_time;
        const sxml_date_time *second_value = &second_datum.datum_details.date_time;
        is_equal = first_value->has_timezone == second_value->has_timezone &&
                   (!first_value->has_timezone ||
                    first_value->timezone_offset_minutes == second_value->timezone_offset_minutes);
    }
    comparison->is_equal = is_equal;
    return SXML_STATUS_OK;
}

typedef struct node_pair {
    sxml_node_reference first_node;
    sxml_node_reference second_node;
} node_pair;

/* Compares the contents of two leaf nodes (attributes, text, comments,
   processing instructions; namespace nodes by codepoints). */
static sxml_status compare_contents(deep_comparison *comparison, node_pair pair,
                                    sxml_node_kind node_kind) {
    sxml_byte_view first_content;
    sxml_byte_view second_content;
    sxml_status status = sxml_node_get_content(pair.first_node, &first_content);
    if (status == SXML_STATUS_OK) {
        status = sxml_node_get_content(pair.second_node, &second_content);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (node_kind == SXML_NODE_KIND_NAMESPACE) {
        comparison->is_equal = sxml_byte_view_equals(first_content, second_content);
        return SXML_STATUS_OK;
    }
    return compare_strings(comparison, (text_pair){.first_text = first_content,
                                                   .second_text = second_content});
}

static child_position first_child_position(sxml_node_reference node,
                                           const sxml_node_description *description) {
    return (child_position){
        .next_child = {.tree = node.tree,
                       .node_index = description->first_child,
                       .namespace_number = 0},
        .has_next_child = description->has_children};
}

/* Whether element is in the scope of xml:space="preserve" (XML 1.0,
   section 2.10): the nearest xml:space attribute on it or an ancestor. */
typedef struct space_rule {
    bool preserves_space;
} space_rule;

static sxml_status find_preserved_space(const deep_comparison *comparison,
                                        sxml_node_reference element, space_rule *rule) {
    rule->preserves_space = false;
    sxml_node_reference current = element;
    for (;;) {
        sxml_node_description description;
        sxml_status status = sxml_node_describe(current, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (description.node_kind != SXML_NODE_KIND_ELEMENT) {
            return SXML_STATUS_OK;
        }
        for (uint32_t number = 1; number <= description.attribute_count; number += 1) {
            const sxml_node_reference attribute = {
                .tree = current.tree,
                .node_index = {.index = current.node_index.index + number},
                .namespace_number = 0};
            sxml_node_description attribute_description;
            sxml_name_parts parts;
            status = sxml_node_describe(attribute, &attribute_description);
            if (status == SXML_STATUS_OK) {
                status = sxml_name_pool_get_name_parts(
                    sxml_item_heap_get_name_pool(comparison->item_heap),
                    attribute_description.node_name, &parts);
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (parts.namespace_uri.index == SXML_NAMESPACE_XML &&
                sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("space"))) {
                sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
                status = sxml_node_get_content(attribute, &value);
                rule->preserves_space =
                    status == SXML_STATUS_OK &&
                    sxml_byte_view_equals(trim_text(value), SXML_BYTE_VIEW_FROM_LITERAL("preserve"));
                return status;
            }
        }
        if (!description.has_parent) {
            return SXML_STATUS_OK;
        }
        current.node_index = description.parent;
    }
}

/* Whether an element's children compare in any order. */
typedef struct children_order {
    bool is_unordered;
} children_order;

/* Whether the element name is listed in unordered-elements. */
static sxml_status find_children_order(const deep_comparison *comparison,
                                       sxml_name_identifier element_name, children_order *order) {
    order->is_unordered = false;
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(comparison->options.unordered_elements);
    for (;;) {
        sxml_iteration_step step;
        sxml_status status =
            sxml_sequence_iterator_next(&iterator, comparison->call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            return status;
        }
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(step.item, &datum);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (datum.datum_details.qname.expanded_name.index == element_name.index) {
            order->is_unordered = true;
            return SXML_STATUS_OK;
        }
    }
}

static sxml_status compare_base_uris(deep_comparison *comparison, node_pair pair) {
    sxml_sequence first = sxml_sequence_empty();
    sxml_sequence second = sxml_sequence_empty();
    sxml_status status = sxml_node_make_base_uri(
        comparison->item_heap,
        &(sxml_node_value_request){.node = pair.first_node, .error_record = NULL}, &first);
    if (status == SXML_STATUS_OK) {
        status = sxml_node_make_base_uri(
            comparison->item_heap,
            &(sxml_node_value_request){.node = pair.second_node, .error_record = NULL}, &second);
    }
    if (status == SXML_STATUS_OK) {
        const bool is_first_empty =
            sxml_sequence_get_representation(first) == SXML_SEQUENCE_REPRESENTATION_EMPTY;
        const bool is_second_empty =
            sxml_sequence_get_representation(second) == SXML_SEQUENCE_REPRESENTATION_EMPTY;
        comparison->is_equal = is_first_empty == is_second_empty &&
                               (is_first_empty || sxml_atomic_item_is_atomic_equal(
                                                      first.content, second.content));
    }
    sxml_sequence_release(first);
    sxml_sequence_release(second);
    return status;
}

static sxml_status compare_in_scope_namespaces(deep_comparison *comparison, node_pair pair) {
    sxml_namespace_binding_view first;
    sxml_namespace_binding_view second;
    sxml_status status = sxml_node_get_in_scope_namespaces(pair.first_node, &first);
    if (status == SXML_STATUS_OK) {
        status = sxml_node_get_in_scope_namespaces(pair.second_node, &second);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    bool is_equal = first.namespace_binding_count == second.namespace_binding_count;
    for (size_t first_index = 0; is_equal && first_index < first.namespace_binding_count;
         first_index += 1) {
        const sxml_namespace_binding *binding = &first.namespace_bindings[first_index];
        bool is_found = false;
        for (size_t second_index = 0; !is_found && second_index < second.namespace_binding_count;
             second_index += 1) {
            const sxml_namespace_binding *other = &second.namespace_bindings[second_index];
            is_found = other->namespace_prefix.index == binding->namespace_prefix.index &&
                       other->namespace_uri.index == binding->namespace_uri.index;
        }
        is_equal = is_found;
    }
    comparison->is_equal = is_equal;
    return SXML_STATUS_OK;
}

/* The properties of two nodes that options make significant (F&O 4.0,
   fn:deep-equal): prefix, type annotation, is-id, is-idrefs, nilled, base
   URI and in-scope namespaces. */
typedef struct described_pair {
    node_pair nodes;
    const sxml_node_description *first;
    const sxml_node_description *second;
} described_pair;

static sxml_status compare_node_properties(deep_comparison *comparison, described_pair pair) {
    const deep_equal_options *options = &comparison->options;
    const sxml_node_description *first = pair.first;
    const sxml_node_description *second = pair.second;
    const bool has_name = first->node_kind == SXML_NODE_KIND_ELEMENT ||
                          first->node_kind == SXML_NODE_KIND_ATTRIBUTE;
    if ((has_name && options->compares_namespace_prefixes &&
         first->namespace_prefix.index != second->namespace_prefix.index) ||
        (has_name && options->compares_type_annotations &&
         first->type_annotation.index != second->type_annotation.index) ||
        (has_name && options->compares_id_property && first->is_id != second->is_id) ||
        (has_name && options->compares_idrefs_property &&
         first->is_idrefs != second->is_idrefs) ||
        (first->node_kind == SXML_NODE_KIND_ELEMENT && options->compares_nilled_property &&
         first->is_nilled != second->is_nilled)) {
        comparison->is_equal = false;
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    if (options->compares_base_uri && first->node_kind != SXML_NODE_KIND_NAMESPACE) {
        status = compare_base_uris(comparison, pair.nodes);
    }
    if (status == SXML_STATUS_OK && comparison->is_equal &&
        options->compares_in_scope_namespaces && first->node_kind == SXML_NODE_KIND_ELEMENT) {
        status = compare_in_scope_namespaces(comparison, pair.nodes);
    }
    return status;
}

/* One side of a children walk: its position, merged text and the space
   rule. */
typedef struct child_side {
    child_position *position;
    sxml_library_text *text;
    bool preserves_space;
} child_side;

static void advance_position(child_position *position,
                             const sxml_node_description *description) {
    position->has_next_child = description->has_next_sibling;
    position->next_child.node_index = description->next_sibling;
}

/* Reads the next significant child of one side: a node that is compared,
   or a run of text nodes merged into side.text, skipping the children the
   options ignore (F&O 4.0, fn:deep-equal, significant-children). */
static sxml_status read_unit(const deep_comparison *comparison, child_side side,
                             comparison_unit *unit) {
    child_position *position = side.position;
    for (;;) {
        sxml_function_library_text_release(side.text);
        size_t text_node_count = 0;
        sxml_node_reference first_text_node = {.tree = NULL};
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
                if (text_node_count > 0) {
                    break;
                }
                *unit = (comparison_unit){.kind = SXML_UNIT_KIND_NODE,
                                          .node = position->next_child};
                advance_position(position, &description);
                return SXML_STATUS_OK;
            }
            sxml_byte_view content;
            status = sxml_node_get_content(position->next_child, &content);
            if (status == SXML_STATUS_OK) {
                status = sxml_function_library_text_append(side.text, content);
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (text_node_count == 0) {
                first_text_node = position->next_child;
            }
            text_node_count += 1;
            advance_position(position, &description);
        }
        if (text_node_count == 0) {
            *unit = (comparison_unit){.kind = SXML_UNIT_KIND_END};
            return SXML_STATUS_OK;
        }
        if (comparison->options.whitespace != SXML_WHITESPACE_HANDLING_PRESERVE &&
            !side.preserves_space &&
            is_all_whitespace(sxml_function_library_text_view(side.text))) {
            continue;
        }
        *unit = (comparison_unit){.kind = SXML_UNIT_KIND_TEXT,
                                  .node = first_text_node,
                                  .has_single_node = text_node_count == 1};
        return SXML_STATUS_OK;
    }
}

static sxml_status append_unit(deep_comparison *comparison, unit_list *list,
                               const comparison_unit *unit) {
    if (list->unit_count == list->unit_capacity) {
        const size_t capacity = list->unit_capacity == 0 ? 8 : list->unit_capacity * 2;
        size_t byte_count = 0;
        sxml_status status = sxml_size_multiply(capacity, sizeof(comparison_unit), &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_item_heap_allocate_bytes(
                comparison->item_heap,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(comparison_unit)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (list->unit_count > 0) {
            memcpy(memory, list->units, list->unit_count * sizeof(comparison_unit));
        }
        sxml_item_heap_release_bytes(
            comparison->item_heap,
            (sxml_heap_bytes){.memory = list->units,
                              .byte_count = list->unit_capacity * sizeof(comparison_unit)});
        list->units = memory;
        list->unit_capacity = capacity;
    }
    list->units[list->unit_count] = *unit;
    list->unit_count += 1;
    return SXML_STATUS_OK;
}

/* Collects the significant children of one parent into list, copying
   merged text into heap memory the list owns. */
typedef struct children_request {
    sxml_node_reference parent;
    const sxml_node_description *description;
    bool preserves_space;
} children_request;

static sxml_status collect_children(deep_comparison *comparison, children_request request,
                                    unit_list *list) {
    child_position position = first_child_position(request.parent, request.description);
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        comparison_unit unit;
        status = read_unit(comparison,
                           (child_side){.position = &position,
                                        .text = &comparison->first_text,
                                        .preserves_space = request.preserves_space},
                           &unit);
        if (status != SXML_STATUS_OK || unit.kind == SXML_UNIT_KIND_END) {
            break;
        }
        if (unit.kind == SXML_UNIT_KIND_TEXT) {
            const sxml_byte_view text = sxml_function_library_text_view(&comparison->first_text);
            void *memory = NULL;
            if (text.byte_count > 0) {
                status = sxml_item_heap_allocate_bytes(
                    comparison->item_heap,
                    (sxml_allocation_request){.byte_count = text.byte_count, .alignment = 1},
                    &memory);
                if (status != SXML_STATUS_OK) {
                    break;
                }
                memcpy(memory, text.bytes, text.byte_count);
            }
            unit.text_bytes = memory;
            unit.text_byte_count = text.byte_count;
        }
        status = append_unit(comparison, list, &unit);
        if (status != SXML_STATUS_OK) {
            sxml_item_heap_release_bytes(
                comparison->item_heap,
                (sxml_heap_bytes){.memory = unit.text_bytes, .byte_count = unit.text_byte_count});
            break;
        }
    }
    sxml_function_library_text_release(&comparison->first_text);
    return status;
}

/* Pushes an unordered cursor over two unit lists it takes over, or
   finds them of different lengths. */
static sxml_status push_unordered(deep_comparison *comparison, unit_list first,
                                  unit_list second) {
    comparison_pair pair = {.kind = SXML_PAIR_KIND_UNORDERED,
                            .first_units = first,
                            .second_units = second,
                            .is_matched = NULL,
                            .next_index = 0,
                            .count = first.unit_count,
                            .candidate_index = 0,
                            .is_in_trial = false};
    sxml_status status = SXML_STATUS_OK;
    if (first.unit_count != second.unit_count) {
        comparison->is_equal = false;
    } else if (second.unit_capacity > 0) {
        void *memory = NULL;
        status = sxml_item_heap_allocate_bytes(
            comparison->item_heap,
            (sxml_allocation_request){.byte_count = second.unit_capacity * sizeof(bool),
                                      .alignment = alignof(bool)},
            &memory);
        if (status == SXML_STATUS_OK) {
            memset(memory, 0, second.unit_capacity * sizeof(bool));
            pair.is_matched = memory;
        }
    }
    if (status == SXML_STATUS_OK && comparison->is_equal && first.unit_count > 0) {
        status = push_pair(comparison, &pair);
        if (status == SXML_STATUS_OK) {
            return SXML_STATUS_OK;
        }
    }
    release_pair(comparison, &pair);
    return status;
}

/* Pushes the cursor over the significant children of two parents: in
   order, or unordered for an element listed in unordered-elements. */
typedef struct parent_pair {
    node_pair nodes;
    const sxml_node_description *first;
    const sxml_node_description *second;
    bool is_unordered;
} parent_pair;

static sxml_status push_children(deep_comparison *comparison, parent_pair pair) {
    space_rule first_rule = {.preserves_space = false};
    sxml_status status = SXML_STATUS_OK;
    if (comparison->options.whitespace != SXML_WHITESPACE_HANDLING_PRESERVE &&
        pair.first->node_kind == SXML_NODE_KIND_ELEMENT) {
        status = find_preserved_space(comparison, pair.nodes.first_node, &first_rule);
    }
    space_rule second_rule = {.preserves_space = false};
    if (status == SXML_STATUS_OK &&
        comparison->options.whitespace != SXML_WHITESPACE_HANDLING_PRESERVE &&
        pair.second->node_kind == SXML_NODE_KIND_ELEMENT) {
        status = find_preserved_space(comparison, pair.nodes.second_node, &second_rule);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool preserves_space = first_rule.preserves_space;
    const bool second_preserves_space = second_rule.preserves_space;
    if (!pair.is_unordered) {
        /* Both sides follow the first's space rule unless the second's
           keeps more. */
        return push_pair(comparison,
                         &(comparison_pair){
                             .kind = SXML_PAIR_KIND_CHILDREN,
                             .first_position = first_child_position(pair.nodes.first_node,
                                                                    pair.first),
                             .second_position = first_child_position(pair.nodes.second_node,
                                                                     pair.second),
                             .preserves_space = preserves_space || second_preserves_space,
                             .next_index = 0,
                             .count = 0});
    }
    unit_list first = {.units = NULL};
    unit_list second = {.units = NULL};
    status = collect_children(comparison,
                              (children_request){.parent = pair.nodes.first_node,
                                                 .description = pair.first,
                                                 .preserves_space = preserves_space},
                              &first);
    if (status == SXML_STATUS_OK) {
        status = collect_children(comparison,
                                  (children_request){.parent = pair.nodes.second_node,
                                                     .description = pair.second,
                                                     .preserves_space = second_preserves_space},
                                  &second);
    }
    if (status != SXML_STATUS_OK) {
        release_units(comparison, &first);
        release_units(comparison, &second);
        return status;
    }
    return push_unordered(comparison, first, second);
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
    status = compare_node_properties(
        comparison, (described_pair){.nodes = pair, .first = &first, .second = &second});
    if (status != SXML_STATUS_OK || !comparison->is_equal) {
        return status;
    }
    children_order order = {.is_unordered = false};
    switch (first.node_kind) {
    case SXML_NODE_KIND_ELEMENT:
        if (first.attribute_count != second.attribute_count) {
            comparison->is_equal = false;
            return SXML_STATUS_OK;
        }
        status = find_children_order(comparison, first.node_name, &order);
        if (status == SXML_STATUS_OK) {
            status = push_children(comparison, (parent_pair){.nodes = pair,
                                                             .first = &first,
                                                             .second = &second,
                                                             .is_unordered = order.is_unordered});
        }
        if (status != SXML_STATUS_OK || !comparison->is_equal || first.attribute_count == 0) {
            return status;
        }
        /* The attributes, compared first: their cursor is on top. */
        return push_pair(comparison, &(comparison_pair){.kind = SXML_PAIR_KIND_ATTRIBUTES,
                                                        .first_node = pair.first_node,
                                                        .second_node = pair.second_node,
                                                        .next_index = 0,
                                                        .count = first.attribute_count});
    case SXML_NODE_KIND_DOCUMENT:
        return push_children(comparison, (parent_pair){.nodes = pair,
                                                       .first = &first,
                                                       .second = &second,
                                                       .is_unordered = false});
    case SXML_NODE_KIND_ATTRIBUTE:
    case SXML_NODE_KIND_TEXT:
    case SXML_NODE_KIND_COMMENT:
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
    case SXML_NODE_KIND_NAMESPACE:
        return compare_contents(comparison, pair, first.node_kind);
    case SXML_NODE_KIND_ANY:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Two function items that are neither maps nor arrays: equal when they are
   the same item, or have the same body, arity, name and signature and
   deep-equal captured values (pushed as a cursor). */
static sxml_status compare_functions(deep_comparison *comparison, sxml_item first,
                                     sxml_item second) {
    if (first.representation == second.representation &&
        first.payload.bits == second.payload.bits &&
        first.type_or_node_index == second.type_or_node_index) {
        return SXML_STATUS_OK;
    }
    sxml_function_item_description first_description;
    sxml_function_item_description second_description;
    if (sxml_function_item_describe(first, &first_description) != SXML_STATUS_OK ||
        sxml_function_item_describe(second, &second_description) != SXML_STATUS_OK ||
        first_description.body.state != second_description.body.state ||
        first_description.body.operations != second_description.body.operations ||
        first_description.arity != second_description.arity ||
        first_description.function_name.index != second_description.function_name.index ||
        first_description.signature.index != second_description.signature.index ||
        first_description.captured_values.sequence_count !=
            second_description.captured_values.sequence_count) {
        comparison->is_equal = false;
        return SXML_STATUS_OK;
    }
    if (first_description.captured_values.sequence_count == 0) {
        return SXML_STATUS_OK;
    }
    return push_pair(comparison,
                     &(comparison_pair){
                         .kind = SXML_PAIR_KIND_CAPTURED_VALUES,
                         .first_item = first,
                         .second_item = second,
                         .next_index = 0,
                         .count = first_description.captured_values.sequence_count});
}

/* The number of significant entries of a map (all of them unless
   ignore-empty-entries). */
static sxml_status count_significant_entries(const deep_comparison *comparison, sxml_item map,
                                             size_t *entry_count) {
    size_t size = 0;
    sxml_status status = sxml_map_get_size(map, &size);
    if (status != SXML_STATUS_OK || !comparison->options.ignores_empty_entries) {
        *entry_count = size;
        return status;
    }
    size_t significant = 0;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < size;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(map, entry_index, &entry);
        if (status == SXML_STATUS_OK &&
            sxml_sequence_get_representation(entry.value) != SXML_SEQUENCE_REPRESENTATION_EMPTY) {
            significant += 1;
        }
    }
    *entry_count = significant;
    return status;
}

static sxml_status compare_collections(deep_comparison *comparison, sxml_item first,
                                       sxml_item second) {
    const bool is_map = sxml_item_get_kind(first) == SXML_ITEM_KIND_MAP;
    size_t first_size = 0;
    size_t second_size = 0;
    size_t first_entry_count = 0;
    sxml_status status = SXML_STATUS_OK;
    if (is_map) {
        status = count_significant_entries(comparison, first, &first_size);
        if (status == SXML_STATUS_OK) {
            status = count_significant_entries(comparison, second, &second_size);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_map_get_size(first, &first_entry_count);
        }
    } else {
        status = sxml_array_get_size(first, &first_size);
        if (status == SXML_STATUS_OK) {
            status = sxml_array_get_size(second, &second_size);
        }
        first_entry_count = first_size;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (first_size != second_size) {
        comparison->is_equal = false;
        return SXML_STATUS_OK;
    }
    if (first_entry_count == 0) {
        return SXML_STATUS_OK;
    }
    return push_pair(comparison,
                     &(comparison_pair){
                         .kind = is_map ? SXML_PAIR_KIND_MAPS : SXML_PAIR_KIND_ARRAYS,
                         .first_item = first,
                         .second_item = second,
                         .next_index = 0,
                         .count = first_entry_count,
                         .significant_index = 0});
}

/* The normal rules for two items (F&O 4.0, fn:deep-equal). */
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
    case SXML_ITEM_KIND_ARRAY:
        return compare_collections(comparison, first, second);
    case SXML_ITEM_KIND_FUNCTION:
        return compare_functions(comparison, first, second);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Two items about to be compared: through items-equal first when the
   options name one (the call is asked for and the next step decides),
   else by the normal rules. The items are borrowed; when owned is true
   they are owned and handed over. */
typedef struct item_pair {
    sxml_item first;
    sxml_item second;
    bool is_owned;
} item_pair;

static sxml_status compare_item_pair(deep_comparison *comparison, item_pair pair) {
    if (!comparison->options.has_items_equal) {
        const sxml_status status = compare_items(comparison, pair.first, pair.second);
        if (pair.is_owned) {
            sxml_item_release(pair.first);
            sxml_item_release(pair.second);
        }
        return status;
    }
    if (!pair.is_owned) {
        sxml_item_retain(pair.first);
        sxml_item_retain(pair.second);
    }
    comparison->call_arguments[0] = sxml_sequence_from_item(pair.first);
    comparison->call_arguments[1] = sxml_sequence_from_item(pair.second);
    comparison->has_pending_call = true;
    return SXML_STATUS_OK;
}

/* Two nodes about to be compared, as compare_item_pair. */
static sxml_status compare_node_pair(deep_comparison *comparison, node_pair pair) {
    if (!comparison->options.has_items_equal) {
        return compare_nodes(comparison, pair);
    }
    return compare_item_pair(comparison, (item_pair){.first = sxml_node_item_make(pair.first_node),
                                                     .second = sxml_node_item_make(pair.second_node),
                                                     .is_owned = true});
}

/* Reads the answer of an items-equal call (owned, taken over). */
static sxml_status receive_call_result(deep_comparison *comparison, sxml_sequence result) {
    sxml_status status = SXML_STATUS_OK;
    sxml_item item;
    sxml_atomic_datum datum;
    if (sxml_sequence_get_representation(result) == SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        /* The normal rules decide. */
        status = compare_items(comparison, comparison->call_arguments[0].content,
                               comparison->call_arguments[1].content);
    } else if (read_single_item(result, &item) &&
               sxml_item_get_kind(item) == SXML_ITEM_KIND_ATOMIC &&
               sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK &&
               datum.datum_kind == SXML_DATUM_KIND_BOOLEAN) {
        comparison->is_equal = datum.datum_details.boolean;
    } else {
        status = sxml_function_library_report(comparison->call, &invalid_callback_result_error);
    }
    sxml_sequence_release(result);
    sxml_sequence_release(comparison->call_arguments[0]);
    sxml_sequence_release(comparison->call_arguments[1]);
    comparison->call_arguments[0] = sxml_sequence_empty();
    comparison->call_arguments[1] = sxml_sequence_empty();
    return status;
}

/* --------------------------------------------------------------- Cursors */

/* One step of a children cursor: compares the next units of both sides,
   popping the cursor at their end. */
static sxml_status step_children(deep_comparison *comparison, comparison_pair *pair) {
    comparison_unit first;
    comparison_unit second;
    sxml_status status = read_unit(comparison,
                                   (child_side){.position = &pair->first_position,
                                                .text = &comparison->first_text,
                                                .preserves_space = pair->preserves_space},
                                   &first);
    if (status == SXML_STATUS_OK) {
        status = read_unit(comparison,
                           (child_side){.position = &pair->second_position,
                                        .text = &comparison->second_text,
                                        .preserves_space = pair->preserves_space},
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
        truncate_pairs(comparison, comparison->pair_count - 1);
        return SXML_STATUS_OK;
    case SXML_UNIT_KIND_TEXT:
        if (comparison->options.has_items_equal && first.has_single_node &&
            second.has_single_node) {
            return compare_node_pair(
                comparison, (node_pair){.first_node = first.node, .second_node = second.node});
        }
        return compare_strings(
            comparison,
            (text_pair){.first_text = sxml_function_library_text_view(&comparison->first_text),
                        .second_text = sxml_function_library_text_view(&comparison->second_text)});
    case SXML_UNIT_KIND_NODE:
    case SXML_UNIT_KIND_ITEM:
        break;
    }
    return compare_node_pair(comparison,
                             (node_pair){.first_node = first.node, .second_node = second.node});
}

/* One step of an attributes cursor: the next attribute of the first
   element against the attribute of the same name of the second. */
static sxml_status step_attributes(deep_comparison *comparison, comparison_pair *pair) {
    const uint32_t first_number = (uint32_t)pair->next_index + 1U;
    pair->next_index += 1;
    const sxml_node_reference first = {
        .tree = pair->first_node.tree,
        .node_index = {.index = pair->first_node.node_index.index + first_number},
        .namespace_number = 0};
    sxml_node_description first_description;
    sxml_status status = sxml_node_describe(first, &first_description);
    for (uint32_t second_number = 1; status == SXML_STATUS_OK && second_number <= pair->count;
         second_number += 1) {
        const sxml_node_reference second = {
            .tree = pair->second_node.tree,
            .node_index = {.index = pair->second_node.node_index.index + second_number},
            .namespace_number = 0};
        sxml_node_description second_description;
        status = sxml_node_describe(second, &second_description);
        if (status == SXML_STATUS_OK &&
            second_description.node_name.index == first_description.node_name.index) {
            return compare_node_pair(comparison,
                                     (node_pair){.first_node = first, .second_node = second});
        }
    }
    if (status == SXML_STATUS_OK) {
        comparison->is_equal = false;
    }
    return status;
}

/* The two units of an unordered trial. */
typedef struct unit_pair {
    const comparison_unit *first;
    const comparison_unit *second;
} unit_pair;

static sxml_status compare_units(deep_comparison *comparison, unit_pair pair) {
    if (pair.first->kind != pair.second->kind) {
        comparison->is_equal = false;
        return SXML_STATUS_OK;
    }
    switch (pair.first->kind) {
    case SXML_UNIT_KIND_ITEM:
        return compare_item_pair(comparison, (item_pair){.first = pair.first->item,
                                                         .second = pair.second->item,
                                                         .is_owned = false});
    case SXML_UNIT_KIND_TEXT:
        if (comparison->options.has_items_equal && pair.first->has_single_node &&
            pair.second->has_single_node) {
            break;
        }
        return compare_strings(
            comparison,
            (text_pair){.first_text = {.bytes = pair.first->text_bytes,
                                       .byte_count = pair.first->text_byte_count},
                        .second_text = {.bytes = pair.second->text_bytes,
                                        .byte_count = pair.second->text_byte_count}});
    case SXML_UNIT_KIND_NODE:
        break;
    case SXML_UNIT_KIND_END:
        return SXML_STATUS_OK;
    }
    return compare_node_pair(comparison, (node_pair){.first_node = pair.first->node,
                                                     .second_node = pair.second->node});
}

/* One step of an unordered cursor: completes a trial that ended equal, or
   starts the next trial, or finds no candidate left. */
static sxml_status step_unordered(deep_comparison *comparison, size_t pair_index) {
    comparison_pair *pair = &comparison->pairs[pair_index];
    if (pair->is_in_trial) {
        pair->is_matched[pair->candidate_index] = true;
        pair->next_index += 1;
        pair->candidate_index = 0;
        pair->is_in_trial = false;
    }
    if (pair->next_index == pair->count) {
        truncate_pairs(comparison, pair_index);
        return SXML_STATUS_OK;
    }
    while (pair->candidate_index < pair->second_units.unit_count &&
           pair->is_matched[pair->candidate_index]) {
        pair->candidate_index += 1;
    }
    if (pair->candidate_index == pair->second_units.unit_count) {
        comparison->is_equal = false;
        return SXML_STATUS_OK;
    }
    pair->is_in_trial = true;
    const unit_pair units = {.first = &pair->first_units.units[pair->next_index],
                             .second = &pair->second_units.units[pair->candidate_index]};
    return compare_units(comparison, units);
}

/* One step of the cursor on top. */
static sxml_status step_pair(deep_comparison *comparison) {
    const size_t pair_index = comparison->pair_count - 1;
    comparison_pair *pair = &comparison->pairs[pair_index];
    if (pair->kind == SXML_PAIR_KIND_CHILDREN) {
        return step_children(comparison, pair);
    }
    if (pair->kind == SXML_PAIR_KIND_UNORDERED) {
        return step_unordered(comparison, pair_index);
    }
    if (pair->next_index == pair->count) {
        truncate_pairs(comparison, pair_index);
        return SXML_STATUS_OK;
    }
    if (pair->kind == SXML_PAIR_KIND_ATTRIBUTES) {
        return step_attributes(comparison, pair);
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
        return status == SXML_STATUS_OK
                   ? compare_item_pair(comparison, (item_pair){.first = first,
                                                               .second = second,
                                                               .is_owned = false})
                   : status;
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
    case SXML_PAIR_KIND_CAPTURED_VALUES: {
        sxml_function_item_description first;
        sxml_function_item_description second;
        sxml_status status = sxml_function_item_describe(pair->first_item, &first);
        if (status == SXML_STATUS_OK) {
            status = sxml_function_item_describe(pair->second_item, &second);
        }
        return status == SXML_STATUS_OK
                   ? push_sequences(comparison, first.captured_values.sequences[index],
                                    second.captured_values.sequences[index])
                   : status;
    }
    case SXML_PAIR_KIND_MAPS:
        break;
    case SXML_PAIR_KIND_ATTRIBUTES:
    case SXML_PAIR_KIND_CHILDREN:
    case SXML_PAIR_KIND_UNORDERED:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_map_entry entry;
    sxml_status status = sxml_map_get_entry(pair->first_item, (size_t)index, &entry);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (comparison->options.ignores_empty_entries &&
        sxml_sequence_get_representation(entry.value) == SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        return SXML_STATUS_OK;
    }
    const uint64_t significant_index = pair->significant_index;
    pair->significant_index += 1;
    sxml_map_lookup lookup = {.is_found = false};
    status = sxml_map_find(pair->second_item, entry.key, &lookup);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_significant =
        lookup.is_found &&
        !(comparison->options.ignores_empty_entries &&
          sxml_sequence_get_representation(lookup.entry.value) ==
              SXML_SEQUENCE_REPRESENTATION_EMPTY);
    if (!is_significant) {
        comparison->is_equal = false;
        return SXML_STATUS_OK;
    }
    if (comparison->options.compares_map_order) {
        /* The position of the entry among the significant entries of the
           second map. */
        uint64_t second_index = lookup.entry_index;
        if (comparison->options.ignores_empty_entries) {
            second_index = 0;
            for (size_t entry_index = 0; status == SXML_STATUS_OK &&
                                         entry_index < lookup.entry_index;
                 entry_index += 1) {
                sxml_map_entry earlier;
                status = sxml_map_get_entry(pair->second_item, entry_index, &earlier);
                if (status == SXML_STATUS_OK && sxml_sequence_get_representation(earlier.value) !=
                                                    SXML_SEQUENCE_REPRESENTATION_EMPTY) {
                    second_index += 1;
                }
            }
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (second_index != significant_index) {
            comparison->is_equal = false;
            return SXML_STATUS_OK;
        }
    }
    return push_sequences(comparison, entry.value, lookup.entry.value);
}

/* ------------------------------------------------------------ The loop */

/* After a difference: the innermost unordered cursor with a trial running
   drops the cursors above it and moves to its next candidate. False when
   no trial runs, so the difference stands. */
static bool backtrack(deep_comparison *comparison) {
    for (size_t pair_index = comparison->pair_count; pair_index > 0; pair_index -= 1) {
        comparison_pair *pair = &comparison->pairs[pair_index - 1];
        if (pair->kind == SXML_PAIR_KIND_UNORDERED && pair->is_in_trial) {
            truncate_pairs(comparison, pair_index);
            pair = &comparison->pairs[pair_index - 1];
            pair->is_in_trial = false;
            pair->candidate_index += 1;
            comparison->is_equal = true;
            return true;
        }
    }
    return false;
}

/* Runs cursors until the comparison is decided or an items-equal call is
   needed (comparison->has_pending_call). */
static sxml_status run_comparison(deep_comparison *comparison) {
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && !comparison->has_pending_call &&
           comparison->pair_count > 0) {
        if (!comparison->is_equal) {
            if (!backtrack(comparison)) {
                break;
            }
            continue;
        }
        /* One step per pair compared (D89, D91). */
        status = sxml_function_library_charge_steps(comparison->call, 1);
        if (status == SXML_STATUS_OK) {
            status = step_pair(comparison);
        }
    }
    return status;
}

/* Reads the options and pushes the first cursor (step 0). */
static sxml_status start_comparison(deep_comparison *comparison) {
    const sxml_function_call *call = comparison->call;
    sxml_status status = read_options(call, &comparison->options);
    const sxml_sequence first = sxml_function_library_get_argument(call, 0);
    const sxml_sequence second = sxml_function_library_get_argument(call, 1);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (comparison->options.is_ordered) {
        return push_sequences(comparison, first, second);
    }
    /* "ordered": false: the items of both sequences in any order. */
    unit_list lists[2] = {{.units = NULL}, {.units = NULL}};
    const sxml_sequence inputs[2] = {first, second};
    for (size_t side_index = 0; status == SXML_STATUS_OK && side_index < 2; side_index += 1) {
        sxml_sequence_iterator iterator = sxml_sequence_iterator_start(inputs[side_index]);
        for (;;) {
            sxml_iteration_step step;
            status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
            if (status != SXML_STATUS_OK || !step.has_item) {
                break;
            }
            status = append_unit(comparison, &lists[side_index],
                                 &(comparison_unit){.kind = SXML_UNIT_KIND_ITEM, .item = step.item});
            if (status != SXML_STATUS_OK) {
                break;
            }
        }
    }
    if (status != SXML_STATUS_OK) {
        release_units(comparison, &lists[0]);
        release_units(comparison, &lists[1]);
        return status;
    }
    return push_unordered(comparison, lists[0], lists[1]);
}

/* Starts a comparison in zeroed state memory. */
static void initialize_comparison(deep_comparison *comparison, const sxml_function_call *call) {
    *comparison = (deep_comparison){.call = call,
                                    .item_heap = call->item_heap,
                                    .pairs = NULL,
                                    .pair_count = 0,
                                    .pair_capacity = 0,
                                    .is_equal = true,
                                    .first_text = {.item_heap = call->item_heap},
                                    .second_text = {.item_heap = call->item_heap},
                                    .has_pending_call = false,
                                    .call_arguments = {sxml_sequence_empty(),
                                                       sxml_sequence_empty()}};
}

sxml_status sxml_function_library_call_deep_equal(const sxml_function_call *call,
                                                  sxml_sequence *result) {
    deep_comparison comparison;
    initialize_comparison(&comparison, call);
    sxml_status status = start_comparison(&comparison);
    if (status == SXML_STATUS_OK) {
        status = run_comparison(&comparison);
    }
    if (status == SXML_STATUS_OK && comparison.has_pending_call) {
        /* Callers of this leaf entry pass no options map (array:index-of
           passes a collation), so no items-equal call can be made. */
        status = SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    release_comparison(&comparison);
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean(
            (sxml_boolean_result){.is_true = comparison.is_equal});
    }
    return status;
}

/* One step of fn:deep-equal as a resumable body (D75). */
static sxml_status step_deep_equal(const sxml_native_step_request *request,
                                   sxml_native_step *step) {
    deep_comparison *comparison = request->state;
    sxml_status status = SXML_STATUS_OK;
    if (request->step_number == 0) {
        initialize_comparison(comparison, request->call);
        status = start_comparison(comparison);
    } else {
        comparison->call = request->call;
        comparison->has_pending_call = false;
        status = receive_call_result(comparison, request->call_result);
    }
    if (status == SXML_STATUS_OK) {
        status = run_comparison(comparison);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (comparison->has_pending_call) {
        *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_CALL,
                                   .result = sxml_sequence_empty(),
                                   .function_item = comparison->options.items_equal,
                                   .arguments = {.sequences = comparison->call_arguments,
                                                 .sequence_count = 2}};
        return SXML_STATUS_OK;
    }
    *step = (sxml_native_step){
        .step_kind = SXML_NATIVE_STEP_KIND_FINISH,
        .result = sxml_function_library_make_boolean(
            (sxml_boolean_result){.is_true = comparison->is_equal}),
        .function_item = {.representation = 0},
        .arguments = {.sequences = NULL, .sequence_count = 0}};
    return SXML_STATUS_OK;
}

static void release_deep_equal_state(void *state) { release_comparison(state); }

const sxml_resumable_native_body sxml_function_library_deep_equal_body = {
    .step = step_deep_equal,
    .release_state = release_deep_equal_state,
    .state_byte_count = sizeof(deep_comparison),
    .state_alignment = alignof(deep_comparison),
};
