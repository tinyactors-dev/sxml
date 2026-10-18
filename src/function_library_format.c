/*
 * The number formatting part of the function library (packet 2.9, D97):
 * fn:format-integer (F&O 4.0, section 4.8.1) and fn:format-number
 * (section 4.9.2), over the picture engine of src/format_internal.h.
 *
 * Golden file: src/function_library_math.c (a part of its own); option
 * maps read as src/function_library_deep_equal.c and coerced as
 * src/function_library_json.c does.
 *
 * Decimal formats: fn:format-number starts from the unnamed decimal
 * format and applies the properties of an options map. The decimal formats
 * of the static context (D104) do not reach the evaluator yet (Proposed in
 * docs/DECISIONS.md, D119): until they do, the unnamed format has the
 * default properties and every format name is FODF1280, as for a static
 * context that declares none.
 *
 * Numbers reach the engine as decimal text: xs:integer and xs:decimal
 * exactly, xs:double and xs:float by their shortest decimal form (section
 * 4.9.5), after multiplying by 100 or 1000 in their own type for percent
 * and per-mille, so an overflow gives infinity as the section says.
 */
#include "decimal_internal.h"
#include "evaluator_internal.h"
#include "format_internal.h"
#include "function_library_core_internal.h"
#include "sxml/types.h"
#include "xslt_number_internal.h"
#include "xml_internal.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------- Errors */

static const sxml_static_error invalid_integer_picture_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODF1310"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The picture string of fn:format-integer is not valid (F&O 4.0, section 4.8.1)"),
};

static const sxml_static_error invalid_number_picture_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODF1310"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The picture string of fn:format-number is not valid (F&O 4.0, section 4.9.3)"),
};

static const sxml_static_error unknown_decimal_format_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODF1280"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The static context has no decimal format of that name (F&O 4.0, section 4.9.2)"),
};

static const sxml_static_error invalid_decimal_format_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODF1290"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A decimal format property is not valid, or the markers are not distinct (F&O 4.0, "
        "sections 4.9.1 and 4.9.2)"),
};

static const sxml_static_error unknown_option_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The options map of fn:format-number has a key that is not an option (F&O 4.0, "
        "section 1.7)"),
};

static const sxml_static_error invalid_format_name_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The format-name option of fn:format-number must be an xs:NCName or an xs:QName "
        "(F&O 4.0, section 4.9.2)"),
};

/* ---------------------------------------------------------------- Text */

/* Makes the formatted text an xs:string and releases it. */
static sxml_status finish_text(const sxml_function_call *call, sxml_format_text *text,
                               sxml_sequence *result) {
    const sxml_status status =
        sxml_function_library_make_string(call->item_heap, sxml_format_text_view(text), result);
    sxml_format_text_release(text);
    return status;
}

/* The decimal digits of the magnitude of an int64_t (at most 20). */
typedef struct integer_digits {
    uint8_t digits[24];
    sxml_byte_view view;
} integer_digits;

static void write_integer_digits(int64_t integer, integer_digits *digits) {
    uint64_t magnitude = integer < 0 ? (uint64_t)(-(integer + 1)) + 1U : (uint64_t)integer;
    size_t digit_count = 0;
    do {
        digits->digits[sizeof(digits->digits) - 1 - digit_count] =
            (uint8_t)('0' + (magnitude % 10U));
        digit_count += 1;
        magnitude /= 10U;
    } while (magnitude > 0);
    digits->view = (sxml_byte_view){
        .bytes = digits->digits + sizeof(digits->digits) - digit_count, .byte_count = digit_count};
}

/* The magnitude of canonical xs:decimal text: without its "-". */
static sxml_byte_view decimal_magnitude(sxml_byte_view decimal_text) {
    if (decimal_text.byte_count > 0 && decimal_text.bytes[0] == '-') {
        return (sxml_byte_view){.bytes = decimal_text.bytes + 1,
                                .byte_count = decimal_text.byte_count - 1};
    }
    return decimal_text;
}

/* ----------------------------------------------------- fn:format-integer */

/*
 * fn:format-integer($value as xs:integer?, $picture as xs:string,
 * $language as xs:string? := ()) as xs:string (F&O 4.0, section 4.8.1).
 * Every language is formatted as English, the default language (D97);
 * the zero-length string for the empty sequence.
 */
static sxml_status call_format_integer(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    sxml_string picture_string = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 1, &picture_string);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_integer_picture picture;
    if (sxml_integer_picture_parse(picture_string.utf8, &picture) != SXML_STATUS_OK) {
        return sxml_function_library_report(call, &invalid_integer_picture_error);
    }
    if (!value.has_item) {
        return sxml_function_library_make_string(
            call->item_heap, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, result);
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(value.item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    integer_digits small_digits;
    sxml_byte_view magnitude_digits = {.bytes = NULL, .byte_count = 0};
    bool is_negative = false;
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        write_integer_digits(datum.datum_details.integer, &small_digits);
        magnitude_digits = small_digits.view;
        is_negative = datum.datum_details.integer < 0;
    } else if (datum.datum_kind == SXML_DATUM_KIND_DECIMAL) {
        magnitude_digits = decimal_magnitude(datum.datum_details.decimal_text);
        is_negative = magnitude_digits.byte_count != datum.datum_details.decimal_text.byte_count;
    } else {
        /* Coercion to xs:integer happened before the call. */
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* Radix conversion is quadratic in the digits of a long integer. */
    status = sxml_function_library_charge_steps(
        call, (uint64_t)magnitude_digits.byte_count * magnitude_digits.byte_count / 64U);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_format_text text = {.allocator = sxml_item_heap_as_allocator(call->item_heap),
                             .bytes = NULL,
                             .byte_count = 0,
                             .byte_capacity = 0};
    status = sxml_format_integer(&(sxml_integer_formatting){.picture = &picture,
                                                            .magnitude_digits = magnitude_digits,
                                                            .is_negative = is_negative},
                                 &text);
    if (status != SXML_STATUS_OK) {
        sxml_format_text_release(&text);
        return status;
    }
    return finish_text(call, &text, result);
}

/* -------------------------------------------------------- Options map */

/* The options of fn:format-number (F&O 4.0, section 4.9.2) and the
   decimal format property each one sets. */
typedef struct number_option {
    sxml_byte_view option_name;
    sxml_decimal_format_property_kind property_kind;
} number_option;

static const number_option number_options[] = {
    {SXML_BYTE_VIEW_INITIALIZER("decimal-separator"),
     SXML_DECIMAL_FORMAT_PROPERTY_DECIMAL_SEPARATOR},
    {SXML_BYTE_VIEW_INITIALIZER("grouping-separator"),
     SXML_DECIMAL_FORMAT_PROPERTY_GROUPING_SEPARATOR},
    {SXML_BYTE_VIEW_INITIALIZER("exponent-separator"),
     SXML_DECIMAL_FORMAT_PROPERTY_EXPONENT_SEPARATOR},
    {SXML_BYTE_VIEW_INITIALIZER("percent"), SXML_DECIMAL_FORMAT_PROPERTY_PERCENT},
    {SXML_BYTE_VIEW_INITIALIZER("per-mille"), SXML_DECIMAL_FORMAT_PROPERTY_PER_MILLE},
    {SXML_BYTE_VIEW_INITIALIZER("zero-digit"), SXML_DECIMAL_FORMAT_PROPERTY_ZERO_DIGIT},
    {SXML_BYTE_VIEW_INITIALIZER("digit"), SXML_DECIMAL_FORMAT_PROPERTY_DIGIT},
    {SXML_BYTE_VIEW_INITIALIZER("pattern-separator"),
     SXML_DECIMAL_FORMAT_PROPERTY_PATTERN_SEPARATOR},
    {SXML_BYTE_VIEW_INITIALIZER("infinity"), SXML_DECIMAL_FORMAT_PROPERTY_INFINITY},
    {SXML_BYTE_VIEW_INITIALIZER("NaN"), SXML_DECIMAL_FORMAT_PROPERTY_NAN},
    {SXML_BYTE_VIEW_INITIALIZER("minus-sign"), SXML_DECIMAL_FORMAT_PROPERTY_MINUS_SIGN},
};

#define SXML_FORMAT_OPTION_COUNT (sizeof(number_options) / sizeof(number_options[0]))

/* The properties an options map sets, with the coerced values whose text
   they borrow (owned; released by release_options). */
typedef struct number_option_values {
    sxml_decimal_format_property properties[SXML_FORMAT_OPTION_COUNT];
    sxml_sequence values[SXML_FORMAT_OPTION_COUNT];
    size_t property_count;
    /* The format-name option, borrowed from the map; empty when absent. */
    sxml_sequence format_name;
} number_option_values;

static void release_options(number_option_values *options) {
    for (size_t value_index = 0; value_index < options->property_count; value_index += 1) {
        sxml_sequence_release(options->values[value_index]);
        options->values[value_index] = sxml_sequence_empty();
    }
    options->property_count = 0;
}

/* The text of a string-like atomic item (xs:string, xs:untypedAtomic,
   xs:anyURI and their subtypes); false for other items. */
static bool read_item_text(sxml_item item, sxml_byte_view *text) {
    sxml_atomic_datum datum;
    if (sxml_item_get_kind(item) != SXML_ITEM_KIND_ATOMIC ||
        sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return false;
    }
    *text = datum.datum_details.string.utf8;
    return true;
}

/* True for an XML whitespace byte ([3] S). */
static bool byte_is_space(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* A decimal format name as text: a namespace URI and a local name. */
typedef struct format_name_text {
    sxml_byte_view namespace_uri;
    sxml_byte_view local_name;
} format_name_text;

/* Resolves prefix with the static namespaces of the running unit (XPath
   4.0, section 2.2.1); false when it is not bound. */
static bool resolve_prefix(const sxml_function_call *call, sxml_byte_view prefix,
                           sxml_byte_view *namespace_uri) {
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    if (call->dynamic_context == NULL) {
        return false;
    }
    const sxml_namespace_binding_view bindings =
        sxml_dynamic_context_get_static_namespaces(call->dynamic_context);
    for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding *binding = &bindings.namespace_bindings[binding_index];
        sxml_byte_view bound_prefix = {.bytes = NULL, .byte_count = 0};
        if (binding->namespace_prefix.index != 0 &&
            sxml_name_pool_get_namespace_prefix(name_pool, binding->namespace_prefix,
                                                &bound_prefix) == SXML_STATUS_OK &&
            sxml_byte_view_equals(bound_prefix, prefix)) {
            return sxml_name_pool_get_namespace_uri(name_pool, binding->namespace_uri,
                                                    namespace_uri) == SXML_STATUS_OK;
        }
    }
    return false;
}

/*
 * Reads a decimal format name (F&O 4.0, section 4.9.2: an xs:QName, or a
 * string holding a lexical QName or URIQualifiedName, an unprefixed name
 * being in no namespace) into *name. False for a string that is no name
 * or has an unbound prefix: no format has that name.
 */
static bool read_format_name_text(const sxml_function_call *call, sxml_item item,
                                  format_name_text *name) {
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    *name = (format_name_text){.namespace_uri = {.bytes = NULL, .byte_count = 0},
                               .local_name = {.bytes = NULL, .byte_count = 0}};
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK) {
        return false;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_QNAME) {
        sxml_name_parts parts;
        return sxml_name_pool_get_name_parts(name_pool, datum.datum_details.qname.expanded_name,
                                             &parts) == SXML_STATUS_OK &&
               sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri,
                                                &name->namespace_uri) == SXML_STATUS_OK &&
               (name->local_name = parts.local_name, true);
    }
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    if (!read_item_text(item, &text)) {
        return false;
    }
    /* The name is an xs:QName or xs:NCName after whitespace collapsing
       (XSD 1.1 Part 2, section 4.3.6): surrounding spaces are dropped. */
    while (text.byte_count > 0 && byte_is_space(text.bytes[0])) {
        text = (sxml_byte_view){.bytes = text.bytes + 1, .byte_count = text.byte_count - 1};
    }
    while (text.byte_count > 0 && byte_is_space(text.bytes[text.byte_count - 1])) {
        text.byte_count -= 1;
    }
    if (text.byte_count == 0) {
        return false;
    }
    if (text.byte_count > 2 && text.bytes[0] == 'Q' && text.bytes[1] == '{') {
        for (size_t byte_index = 2; byte_index < text.byte_count; byte_index += 1) {
            if (text.bytes[byte_index] == '}') {
                name->namespace_uri =
                    (sxml_byte_view){.bytes = text.bytes + 2, .byte_count = byte_index - 2};
                name->local_name = (sxml_byte_view){.bytes = text.bytes + byte_index + 1,
                                                    .byte_count = text.byte_count - byte_index - 1};
                return name->local_name.byte_count > 0;
            }
        }
        return false;
    }
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        if (text.bytes[byte_index] == ':') {
            name->local_name = (sxml_byte_view){.bytes = text.bytes + byte_index + 1,
                                                .byte_count = text.byte_count - byte_index - 1};
            return resolve_prefix(call,
                                  (sxml_byte_view){.bytes = text.bytes, .byte_count = byte_index},
                                  &name->namespace_uri);
        }
    }
    name->local_name = text;
    return true;
}

/* True when identifier names name. */
static bool format_name_is(const sxml_name_pool *name_pool, sxml_name_identifier identifier,
                           const format_name_text *name) {
    sxml_name_parts parts;
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    return identifier.index != 0 &&
           sxml_name_pool_get_name_parts(name_pool, identifier, &parts) == SXML_STATUS_OK &&
           sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri, &namespace_uri) ==
               SXML_STATUS_OK &&
           sxml_byte_view_equals(parts.local_name, name->local_name) &&
           sxml_byte_view_equals(namespace_uri, name->namespace_uri);
}

/*
 * Finds the properties of the decimal format the name item names among
 * the statically known decimal formats of the running unit (XPath 4.0,
 * section 2.2.1; read lexically, D131), or of the unnamed one when
 * has_name is false (none declared: the defaults, no properties).
 * FODF1280 for a name the static context does not know.
 */
typedef struct format_name_request {
    bool has_name;
    sxml_item name;
} format_name_request;

static sxml_status find_decimal_format(const sxml_function_call *call,
                                       format_name_request request,
                                       sxml_decimal_format_property_view *properties) {
    *properties = (sxml_decimal_format_property_view){.decimal_format_properties = NULL,
                                                      .decimal_format_property_count = 0};
    const sxml_ir_static_context *context =
        call->dynamic_context == NULL
            ? NULL
            : sxml_dynamic_context_get_static_context(call->dynamic_context);
    const sxml_decimal_format_view formats =
        context == NULL ? (sxml_decimal_format_view){.decimal_formats = NULL,
                                                     .decimal_format_count = 0}
                        : context->decimal_formats;
    format_name_text name;
    if (request.has_name && !read_format_name_text(call, request.name, &name)) {
        return sxml_function_library_report(call, &unknown_decimal_format_error);
    }
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    for (size_t format_index = 0; format_index < formats.decimal_format_count;
         format_index += 1) {
        const sxml_decimal_format *format = &formats.decimal_formats[format_index];
        if (request.has_name ? format_name_is(name_pool, format->format_name, &name)
                             : format->format_name.index == 0) {
            *properties = format->decimal_format_properties;
            return SXML_STATUS_OK;
        }
    }
    if (request.has_name) {
        return sxml_function_library_report(call, &unknown_decimal_format_error);
    }
    return SXML_STATUS_OK;
}

/* Checks the type of the format-name option, (xs:NCName | xs:QName)?:
   an xs:QName or text (relabelled as xs:NCName, XPath 4.0, section
   3.4.3); another type is XPTY0004. */
static sxml_status check_format_name(const sxml_function_call *call, sxml_sequence value) {
    if (sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    const bool is_single_atomic =
        sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
        sxml_item_get_kind(value.content) == SXML_ITEM_KIND_ATOMIC &&
        sxml_atomic_item_get_datum(value.content, &datum) == SXML_STATUS_OK;
    if (!is_single_atomic ||
        (datum.datum_kind != SXML_DATUM_KIND_QNAME && !read_item_text(value.content, &text))) {
        return sxml_function_library_report(call, &invalid_format_name_error);
    }
    return SXML_STATUS_OK;
}

/* Coerces an option value to xs:string (the type of every property
   option; "matching '.'" and "matching '.|.:.*'" are checked by the
   engine). */
static sxml_status coerce_option_text(const sxml_function_call *call, sxml_sequence value,
                                      sxml_sequence *coerced) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_evaluator_coerce(
        call->dynamic_context,
        &(sxml_coercion_request){
            .value = value,
            .sequence_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                              .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
            .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT,
            .error_record = call->error_record},
        coerced);
}

/* The option an entry's key names; SXML_FORMAT_OPTION_COUNT for
   format-name, and above it for a key that names no option. */
#define SXML_FORMAT_NAME_OPTION SXML_FORMAT_OPTION_COUNT
#define SXML_FORMAT_NO_OPTION (SXML_FORMAT_OPTION_COUNT + 1U)

static size_t find_option(sxml_byte_view key_text) {
    if (sxml_byte_view_equals(key_text, SXML_BYTE_VIEW_FROM_LITERAL("format-name"))) {
        return SXML_FORMAT_NAME_OPTION;
    }
    for (size_t option_index = 0; option_index < SXML_FORMAT_OPTION_COUNT; option_index += 1) {
        if (sxml_byte_view_equals(key_text, number_options[option_index].option_name)) {
            return option_index;
        }
    }
    return SXML_FORMAT_NO_OPTION;
}

/* True when key is an xs:QName in a namespace: an implementation-defined
   option, which sxml ignores (F&O 4.0, section 1.7). */
static bool is_extension_key(const sxml_function_call *call, sxml_item key) {
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(key, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_QNAME) {
        return false;
    }
    sxml_name_parts parts;
    return sxml_name_pool_get_name_parts(sxml_item_heap_get_name_pool(call->item_heap),
                                         datum.datum_details.qname.expanded_name,
                                         &parts) == SXML_STATUS_OK &&
           parts.namespace_uri.index != SXML_NAMESPACE_NONE;
}

/*
 * Reads the options map (F&O 4.0, section 1.7, option parameter
 * conventions) into *options, which must be empty: each property option
 * coerced to xs:string, format-name checked.
 */
static sxml_status read_options_map(const sxml_function_call *call, sxml_item map,
                                    number_option_values *options) {
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(map, &entry_count);
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(map, entry_index, &entry);
        if (status != SXML_STATUS_OK) {
            break;
        }
        sxml_byte_view key_text = {.bytes = NULL, .byte_count = 0};
        const size_t option_index =
            read_item_text(entry.key, &key_text) ? find_option(key_text) : SXML_FORMAT_NO_OPTION;
        if (option_index == SXML_FORMAT_NO_OPTION) {
            if (is_extension_key(call, entry.key)) {
                continue;
            }
            return sxml_function_library_report(call, &unknown_option_error);
        }
        if (option_index == SXML_FORMAT_NAME_OPTION) {
            status = check_format_name(call, entry.value);
            options->format_name = entry.value;
            continue;
        }
        sxml_sequence coerced = sxml_sequence_empty();
        status = coerce_option_text(call, entry.value, &coerced);
        if (status != SXML_STATUS_OK) {
            break;
        }
        sxml_byte_view value_text = {.bytes = NULL, .byte_count = 0};
        (void)read_item_text(coerced.content, &value_text);
        options->values[options->property_count] = coerced;
        options->properties[options->property_count] = (sxml_decimal_format_property){
            .property_kind = number_options[option_index].property_kind,
            .property_value = value_text};
        options->property_count += 1;
    }
    return status;
}

/* ------------------------------------------------------ fn:format-number */

/* The number to format, as the engine takes it. */
typedef struct number_input {
    sxml_number_class number_class;
    sxml_number_sign sign;
    sxml_atomic_datum datum;
} number_input;

static number_input classify_number(sxml_atomic_datum datum) {
    number_input input = {.number_class = SXML_NUMBER_CLASS_FINITE,
                          .sign = SXML_NUMBER_SIGN_POSITIVE,
                          .datum = datum};
    double floating = 0.0;
    switch (datum.datum_kind) {
    case SXML_DATUM_KIND_DOUBLE:
        floating = datum.datum_details.double_number;
        break;
    case SXML_DATUM_KIND_FLOAT:
        floating = (double)datum.datum_details.float_number;
        break;
    case SXML_DATUM_KIND_INTEGER:
        input.sign = datum.datum_details.integer < 0 ? SXML_NUMBER_SIGN_NEGATIVE
                                                     : SXML_NUMBER_SIGN_POSITIVE;
        return input;
    case SXML_DATUM_KIND_DECIMAL:
        /* Canonical text has no "-0" (xs:decimal zero is positive). */
        input.sign = datum.datum_details.decimal_text.byte_count > 0 &&
                             datum.datum_details.decimal_text.bytes[0] == '-'
                         ? SXML_NUMBER_SIGN_NEGATIVE
                         : SXML_NUMBER_SIGN_POSITIVE;
        return input;
    case SXML_DATUM_KIND_BOOLEAN:
    case SXML_DATUM_KIND_STRING:
    case SXML_DATUM_KIND_QNAME:
    case SXML_DATUM_KIND_DATE_TIME:
    case SXML_DATUM_KIND_DURATION:
    case SXML_DATUM_KIND_BINARY:
        input.number_class = SXML_NUMBER_CLASS_NOT_A_NUMBER;
        return input;
    }
    if (isnan(floating)) {
        input.number_class = SXML_NUMBER_CLASS_NOT_A_NUMBER;
        return input;
    }
    /* Negative zero is taken as negative (section 4.9.5). */
    input.sign = signbit(floating) ? SXML_NUMBER_SIGN_NEGATIVE : SXML_NUMBER_SIGN_POSITIVE;
    if (isinf(floating)) {
        input.number_class = SXML_NUMBER_CLASS_INFINITE;
    }
    return input;
}

/* The shortest decimal text of a finite double or float magnitude, owned
   through the heap's allocator. */
typedef struct floating_text {
    void *memory;
    sxml_byte_view text;
} floating_text;

/* A finite floating point magnitude to write as decimal text. */
typedef struct floating_request {
    bool is_float;
    double double_number;
    float float_number;
} floating_request;

static sxml_status write_floating_text(const sxml_function_call *call, floating_request request,
                                       floating_text *text) {
    const sxml_decimal_configuration *configuration =
        sxml_item_heap_get_decimal_configuration(call->item_heap);
    sxml_decimal decimal;
    sxml_status status =
        request.is_float
            ? sxml_decimal_from_float_shortest(configuration, request.float_number, &decimal)
            : sxml_decimal_from_double_shortest(configuration, request.double_number, &decimal);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const size_t byte_count = sxml_decimal_canonical_text_byte_count(&decimal);
    void *memory = NULL;
    status = sxml_allocator_allocate(
        sxml_item_heap_as_allocator(call->item_heap),
        (sxml_allocation_request){.byte_count = byte_count, .alignment = 1}, &memory);
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_write_canonical_text(
            &decimal, (sxml_memory_view){.bytes = memory, .byte_count = byte_count}, &text->text);
        if (status != SXML_STATUS_OK) {
            sxml_allocator_release(sxml_item_heap_as_allocator(call->item_heap), memory);
            memory = NULL;
        }
    }
    sxml_decimal_finalize(&decimal);
    text->memory = memory;
    return status;
}

/* A number and the picture it is formatted by. */
typedef struct number_writing {
    const sxml_number_picture *picture;
    number_input input;
} number_writing;

/* Formats the number: the adjusted number (section 4.9.5) in its own
   type, then the engine. */
static sxml_status write_number(const sxml_function_call *call, number_writing writing,
                                sxml_format_text *text) {
    const uint32_t multiplier =
        sxml_number_picture_get_multiplier(writing.picture, writing.input.sign);
    const sxml_atomic_datum datum = writing.input.datum;
    sxml_number_formatting formatting = {.picture = writing.picture,
                                         .number_class = writing.input.number_class,
                                         .sign = writing.input.sign,
                                         .magnitude_text = {.bytes = NULL, .byte_count = 0},
                                         .decimal_point_shift = 0};
    if (formatting.number_class != SXML_NUMBER_CLASS_FINITE) {
        return sxml_format_number(&formatting, text);
    }
    integer_digits small_digits;
    floating_text floating = {.memory = NULL, .text = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = SXML_STATUS_OK;
    const uint32_t shift = multiplier == 100U ? 2U : multiplier == 1000U ? 3U : 0U;
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        write_integer_digits(datum.datum_details.integer, &small_digits);
        formatting.magnitude_text = small_digits.view;
        formatting.decimal_point_shift = shift;
    } else if (datum.datum_kind == SXML_DATUM_KIND_DECIMAL) {
        formatting.magnitude_text = decimal_magnitude(datum.datum_details.decimal_text);
        formatting.decimal_point_shift = shift;
    } else if (datum.datum_kind == SXML_DATUM_KIND_FLOAT) {
        const float adjusted = fabsf(datum.datum_details.float_number) * (float)multiplier;
        if (isinf(adjusted)) {
            formatting.number_class = SXML_NUMBER_CLASS_INFINITE;
        } else {
            status = write_floating_text(
                call,
                (floating_request){.is_float = true, .double_number = 0.0, .float_number = adjusted},
                &floating);
        }
    } else {
        const double adjusted = fabs(datum.datum_details.double_number) * (double)multiplier;
        if (isinf(adjusted)) {
            formatting.number_class = SXML_NUMBER_CLASS_INFINITE;
        } else {
            status = write_floating_text(
                call,
                (floating_request){
                    .is_float = false, .double_number = adjusted, .float_number = 0.0F},
                &floating);
        }
    }
    if (floating.memory != NULL) {
        formatting.magnitude_text = floating.text;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_steps(call, formatting.magnitude_text.byte_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_format_number(&formatting, text);
    }
    if (floating.memory != NULL) {
        sxml_allocator_release(sxml_item_heap_as_allocator(call->item_heap), floating.memory);
    }
    return status;
}

/*
 * Reads $options into the decimal format settings: absent or empty for
 * the unnamed decimal format, a string for a named one, a map for the
 * unnamed or named one with its entries applied. *options holds the
 * coerced values the settings borrow.
 */
static sxml_status read_decimal_format(const sxml_function_call *call,
                                       number_option_values *options,
                                       sxml_decimal_format_settings *settings) {
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 2, &argument);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    format_name_request name_request = {.has_name = false, .name = {.representation = 0}};
    if (argument.has_item && sxml_item_get_kind(argument.item) == SXML_ITEM_KIND_ATOMIC) {
        /* A lexical QName or URIQualifiedName (section 4.9.2). */
        name_request = (format_name_request){.has_name = true, .name = argument.item};
    } else if (argument.has_item) {
        status = read_options_map(call, argument.item, options);
        if (status == SXML_STATUS_OK && sxml_sequence_get_representation(options->format_name) ==
                                            SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
            name_request = (format_name_request){.has_name = true,
                                                 .name = options->format_name.content};
        }
    }
    sxml_decimal_format_property_view format_properties;
    if (status == SXML_STATUS_OK) {
        status = find_decimal_format(call, name_request, &format_properties);
    }
    /* The named (or unnamed) format, then the map's properties over it. */
    const sxml_decimal_format_settings default_settings = sxml_decimal_format_default_settings();
    sxml_decimal_format_settings base_settings = default_settings;
    if (status == SXML_STATUS_OK &&
        sxml_decimal_format_apply(
            &(sxml_decimal_format_change){.base_settings = &default_settings,
                                          .decimal_format_properties = format_properties},
            &base_settings) != SXML_STATUS_OK) {
        status = sxml_function_library_report(call, &invalid_decimal_format_error);
    }
    if (status == SXML_STATUS_OK &&
        sxml_decimal_format_apply(
            &(sxml_decimal_format_change){
                .base_settings = &base_settings,
                .decimal_format_properties = {.decimal_format_properties = options->properties,
                                              .decimal_format_property_count =
                                                  options->property_count}},
            settings) != SXML_STATUS_OK) {
        status = sxml_function_library_report(call, &invalid_decimal_format_error);
    }
    return status;
}

/*
 * fn:format-number($value as xs:numeric?, $picture as xs:string,
 * $options as (xs:string | map(*))? := {}) as xs:string (F&O 4.0, section
 * 4.9.2). The empty sequence is formatted as NaN.
 */
static sxml_status call_format_number(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    sxml_string picture_string = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 1, &picture_string);
    }
    number_option_values options = {.property_count = 0,
                                    .format_name = {.content = {.representation = 0}}};
    sxml_decimal_format_settings settings;
    if (status == SXML_STATUS_OK) {
        status = read_decimal_format(call, &options, &settings);
    }
    sxml_number_picture picture;
    if (status == SXML_STATUS_OK &&
        sxml_number_picture_parse(
            &(sxml_number_picture_request){.picture_string = picture_string.utf8,
                                           .settings = &settings},
            &picture) != SXML_STATUS_OK) {
        status = sxml_function_library_report(call, &invalid_number_picture_error);
    }
    number_input input = {.number_class = SXML_NUMBER_CLASS_NOT_A_NUMBER,
                          .sign = SXML_NUMBER_SIGN_POSITIVE,
                          .datum = {.datum_kind = SXML_DATUM_KIND_DOUBLE}};
    if (status == SXML_STATUS_OK && value.has_item) {
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(value.item, &datum);
        if (status == SXML_STATUS_OK) {
            input = classify_number(datum);
        }
    }
    sxml_format_text text = {.allocator = sxml_item_heap_as_allocator(call->item_heap),
                             .bytes = NULL,
                             .byte_count = 0,
                             .byte_capacity = 0};
    if (status == SXML_STATUS_OK) {
        status = write_number(call, (number_writing){.picture = &picture, .input = input}, &text);
    }
    if (status == SXML_STATUS_OK) {
        status = finish_text(call, &text, result);
    }
    sxml_format_text_release(&text);
    release_options(&options);
    return status;
}

/* ------------------------------------------------- xsl:number's place marker */

static const sxml_static_error invalid_number_attribute_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE0030"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An attribute of xsl:number (grouping-size, lang or letter-value) has a value that is "
        "not allowed (XSLT 4.0, 12.4)"),
};

/* An optional xs:string argument: has_text false for the empty sequence. */
typedef struct optional_text {
    bool has_text;
    sxml_byte_view text;
} optional_text;

static sxml_status read_optional_text(const sxml_function_call *call, size_t argument_index,
                                      optional_text *text) {
    *text = (optional_text){.has_text = false, .text = {.bytes = NULL, .byte_count = 0}};
    sxml_optional_item item = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &item);
    if (status != SXML_STATUS_OK || !item.has_item) {
        return status;
    }
    sxml_string string = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    status = sxml_function_library_read_string_argument(call, argument_index, &string);
    if (status == SXML_STATUS_OK) {
        *text = (optional_text){.has_text = true, .text = string.utf8};
    }
    return status;
}

/* grouping-size: the lexical space of xs:integer (12.4); a size of zero
   or less (and an absurdly large one) inserts no separators. */
static bool read_grouping_size(sxml_byte_view text, size_t *grouping_size) {
    sxml_byte_view digits = sxml_xml_text_trim_whitespace(text);
    bool is_negative = false;
    if (digits.byte_count > 0 && (digits.bytes[0] == '-' || digits.bytes[0] == '+')) {
        is_negative = digits.bytes[0] == '-';
        digits = (sxml_byte_view){.bytes = digits.bytes + 1, .byte_count = digits.byte_count - 1};
    }
    if (digits.byte_count == 0) {
        return false;
    }
    size_t size = 0;
    for (size_t digit_index = 0; digit_index < digits.byte_count; digit_index += 1) {
        const uint8_t digit = digits.bytes[digit_index];
        if (digit < '0' || digit > '9') {
            return false;
        }
        size = size > (SIZE_MAX - 9U) / 10U ? 0 : size * 10U + (size_t)(digit - '0');
    }
    *grouping_size = is_negative ? 0 : size;
    return true;
}

static bool is_ascii_letter(uint8_t byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

static bool is_ascii_letter_or_digit(uint8_t byte) {
    return is_ascii_letter(byte) || (byte >= '0' && byte <= '9');
}

/* lang: an xs:language ([a-zA-Z]{1,8}(-[a-zA-Z0-9]{1,8})*) or "" (12.4). */
static bool is_language_or_empty(sxml_byte_view text) {
    const sxml_byte_view language = sxml_xml_text_trim_whitespace(text);
    size_t part_length = 0;
    size_t part_count = 0;
    for (size_t byte_index = 0; byte_index <= language.byte_count; byte_index += 1) {
        if (byte_index == language.byte_count || language.bytes[byte_index] == '-') {
            if (part_length == 0 && language.byte_count > 0) {
                return false;
            }
            part_length = 0;
            part_count += 1;
            continue;
        }
        part_length += 1;
        const uint8_t byte = language.bytes[byte_index];
        if (part_length > 8 ||
            !(part_count > 0 ? is_ascii_letter_or_digit(byte) : is_ascii_letter(byte))) {
            return false;
        }
    }
    return true;
}

/* The decimal digits of an xs:integer item and its sign. */
typedef struct place_marker_number {
    integer_digits small_digits;
    sxml_byte_view magnitude_digits;
    bool is_negative;
} place_marker_number;

static sxml_status read_place_marker_number(sxml_item item, place_marker_number *number) {
    sxml_atomic_datum datum;
    const sxml_status status = sxml_atomic_item_get_datum(item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        write_integer_digits(datum.datum_details.integer, &number->small_digits);
        number->magnitude_digits = number->small_digits.view;
        number->is_negative = datum.datum_details.integer < 0;
        return SXML_STATUS_OK;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_DECIMAL) {
        number->magnitude_digits = decimal_magnitude(datum.datum_details.decimal_text);
        number->is_negative =
            number->magnitude_digits.byte_count != datum.datum_details.decimal_text.byte_count;
        return SXML_STATUS_OK;
    }
    /* Lowering passes xs:integer values only. */
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* The settings of 12.4 every number of the place marker is formatted
   with. */
typedef struct place_marker_settings {
    sxml_byte_view format;
    sxml_byte_view grouping_separator;
    size_t grouping_size;
    bool is_ordinal;
} place_marker_settings;

static sxml_status read_place_marker_settings(const sxml_function_call *call,
                                              place_marker_settings *settings) {
    sxml_string format = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = sxml_function_library_read_string_argument(call, 1, &format);
    optional_text texts[5];
    for (size_t text_index = 0; status == SXML_STATUS_OK && text_index < 5; text_index += 1) {
        status = read_optional_text(call, text_index + 2, &texts[text_index]);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const optional_text separator = texts[0];
    const optional_text size = texts[1];
    const optional_text ordinal = texts[2];
    const optional_text language = texts[3];
    const optional_text letter_value = texts[4];
    *settings = (place_marker_settings){.format = format.utf8,
                                        .grouping_separator = separator.text,
                                        .grouping_size = 0,
                                        .is_ordinal = false};
    size_t grouping_size = 0;
    if (size.has_text && !read_grouping_size(size.text, &grouping_size)) {
        return sxml_function_library_report(call, &invalid_number_attribute_error);
    }
    if ((language.has_text && !is_language_or_empty(language.text)) ||
        (letter_value.has_text &&
         !sxml_byte_view_equals(sxml_xml_text_trim_whitespace(letter_value.text),
                                SXML_BYTE_VIEW_FROM_LITERAL("alphabetic")) &&
         !sxml_byte_view_equals(sxml_xml_text_trim_whitespace(letter_value.text),
                                SXML_BYTE_VIEW_FROM_LITERAL("traditional")))) {
        return sxml_function_library_report(call, &invalid_number_attribute_error);
    }
    /* "If only one of the grouping-separator and grouping-size attributes
       is specified, then it is ignored." */
    if (separator.has_text && size.has_text) {
        settings->grouping_size = grouping_size;
    }
    /* Cardinal for absent, "", "no", "0" and "false"; any other value asks
       for the English ordinal (the only language, D97). */
    const sxml_byte_view ordinal_text = sxml_xml_text_trim_whitespace(ordinal.text);
    settings->is_ordinal =
        ordinal.has_text && ordinal_text.byte_count > 0 &&
        !sxml_byte_view_equals(ordinal_text, SXML_BYTE_VIEW_FROM_LITERAL("no")) &&
        !sxml_byte_view_equals(ordinal_text, SXML_BYTE_VIEW_FROM_LITERAL("0")) &&
        !sxml_byte_view_equals(ordinal_text, SXML_BYTE_VIEW_FROM_LITERAL("false"));
    return SXML_STATUS_OK;
}

/* Appends the number at number_index of the place marker with the part of
   the format that formats it (12.4). */
typedef struct place_marker_item {
    const place_marker_settings *settings;
    sxml_item number_item;
    size_t number_index;
} place_marker_item;

static sxml_status append_place_marker_number(place_marker_item request,
                                              sxml_format_text *text) {
    const sxml_number_format_part part = sxml_number_format_find_part(
        (sxml_number_format_query){.format = request.settings->format,
                                   .number_index = request.number_index});
    place_marker_number number;
    sxml_status status = read_place_marker_number(request.number_item, &number);
    if (status == SXML_STATUS_OK && request.number_index > 0) {
        status = sxml_format_text_append(text, part.separator);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* A token the engine cannot parse as a primary format token (a letter
       followed by digits, say) numbers as "1" does (12.4: an unsupported
       numbering sequence "must use a format token of 1"). */
    sxml_integer_picture picture;
    if (sxml_integer_picture_parse_primary_format_token(part.format_token, &picture) !=
        SXML_STATUS_OK) {
        status = sxml_integer_picture_parse_primary_format_token(SXML_BYTE_VIEW_FROM_LITERAL("1"),
                                                                 &picture);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    picture.is_ordinal = request.settings->is_ordinal;
    const size_t start = text->byte_count;
    status = sxml_format_integer(
        &(sxml_integer_formatting){.picture = &picture,
                                   .magnitude_digits = number.magnitude_digits,
                                   .is_negative = number.is_negative,
                                   .grouping_separator = request.settings->grouping_separator,
                                   .grouping_size = request.settings->grouping_size},
        text);
    if (status == SXML_STATUS_OK && picture.numbering_kind == SXML_NUMBERING_KIND_TITLE_WORDS) {
        /* Title-case words of xsl:number capitalize every word, a hyphen
           becoming a space ("Twenty Two", as Saxon writes them; QT4
           number-0105 and number-2506), where fn:format-integer keeps
           "Twenty-two" (D119). The words are ASCII. */
        for (size_t byte_index = start; byte_index + 1 < text->byte_count; byte_index += 1) {
            const uint8_t next = text->bytes[byte_index + 1];
            if (text->bytes[byte_index] == '-' && next >= 'a' && next <= 'z') {
                text->bytes[byte_index] = ' ';
                text->bytes[byte_index + 1] = (uint8_t)(next - 'a' + 'A');
            }
        }
    }
    return status;
}

/*
 * format-place-marker($numbers as xs:integer*, $format as xs:string,
 * $grouping-separator as xs:string?, $grouping-size as xs:string?,
 * $ordinal as xs:string?, $lang as xs:string?, $letter-value as
 * xs:string?) as xs:string: the place marker of xsl:number formatted by
 * the attributes of XSLT 4.0, 12.4 (src/xslt_number_internal.h). lang
 * and letter-value are checked and otherwise ignored: English only (D97).
 * An empty place marker gives the prefix and the suffix (as Saxon does;
 * QT4 number-0806 and number-3205 expect it).
 */
static sxml_status call_format_place_marker(const sxml_function_call *call,
                                            sxml_sequence *result) {
    place_marker_settings settings;
    sxml_status status = read_place_marker_settings(call, &settings);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_sequence numbers = sxml_function_library_get_argument(call, 0);
    sxml_format_text text = {.allocator = sxml_item_heap_as_allocator(call->item_heap),
                             .bytes = NULL,
                             .byte_count = 0,
                             .byte_capacity = 0};
    const sxml_number_format_affixes affixes = sxml_number_format_find_affixes(settings.format);
    status = sxml_format_text_append(&text, affixes.prefix);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(numbers);
    for (size_t number_index = 0; status == SXML_STATUS_OK; number_index += 1) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        status = sxml_function_library_charge_steps(call, 1);
        if (status == SXML_STATUS_OK) {
            status = append_place_marker_number(
                (place_marker_item){
                    .settings = &settings, .number_item = step.item, .number_index = number_index},
                &text);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_format_text_append(&text, affixes.suffix);
    }
    if (status != SXML_STATUS_OK) {
        sxml_format_text_release(&text);
        return status;
    }
    return finish_text(call, &text, result);
}

static const sxml_function_parameter place_marker_parameters[] = {
    SXML_REQUIRED_PARAMETER("numbers", "xs:integer*"),
    SXML_REQUIRED_PARAMETER("format", "xs:string"),
    SXML_REQUIRED_PARAMETER("grouping-separator", "xs:string?"),
    SXML_REQUIRED_PARAMETER("grouping-size", "xs:string?"),
    SXML_REQUIRED_PARAMETER("ordinal", "xs:string?"),
    SXML_REQUIRED_PARAMETER("lang", "xs:string?"),
    SXML_REQUIRED_PARAMETER("letter-value", "xs:string?"),
};

const sxml_native_function sxml_xslt_format_place_marker_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("format-place-marker"),
    .parameters = {place_marker_parameters,
                   sizeof(place_marker_parameters) / sizeof(place_marker_parameters[0])},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:string"),
    .call = call_format_place_marker,
    .resumable_body = NULL};

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter format_integer_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:integer?"),
    SXML_REQUIRED_PARAMETER("picture", "xs:string"),
    SXML_OPTIONAL_PARAMETER("language", "xs:string?"),
};

static const sxml_function_parameter format_number_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:numeric?"),
    SXML_REQUIRED_PARAMETER("picture", "xs:string"),
    SXML_OPTIONAL_PARAMETER("options", "(xs:string | map(*))?"),
};

static const sxml_native_function format_functions[] = {
    SXML_FUNCTION_ROW("format-integer", format_integer_parameters, "xs:string", 0,
                      call_format_integer),
    /* Context-dependent: decimal formats and namespaces (section 4.9.2);
       the options map is coerced in the dynamic context. */
    SXML_FUNCTION_ROW("format-number", format_number_parameters, "xs:string",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_format_number),
};

const sxml_native_function_view sxml_function_library_format =
    SXML_LIBRARY_PART(format_functions);
