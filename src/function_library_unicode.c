/*
 * The Unicode part of the function library (packet 2.4, D107): F&O 4.0,
 * section 5.3 "Functions to assemble and disassemble strings": fn:char,
 * fn:characters, fn:graphemes and fn:normalize-unicode, over
 * the Unicode algorithms of src/unicode_internal.h.
 *
 * Golden file: src/function_library_math.c (a part of its own); results
 * of several strings built as fn:tokenize builds them
 * (src/function_library_regex.c).
 */
#include "function_library_core_internal.h"
#include "unicode_internal.h"

#include <string.h>

/* The part of bytes that range selects (within bytes); no pointer
   arithmetic on the NULL of an empty view. */
static sxml_byte_view slice_of(sxml_byte_view bytes, sxml_byte_range range) {
    if (range.byte_count == 0) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = bytes.bytes + range.byte_offset,
                            .byte_count = range.byte_count};
}

/* Appends the string of bytes to builder as one xs:string item. */
static sxml_status append_string(const sxml_function_call *call, sxml_sequence_builder *builder,
                                 sxml_byte_view bytes) {
    sxml_sequence string = sxml_sequence_empty();
    sxml_status status = sxml_function_library_make_string(call->item_heap, bytes, &string);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_append_item(builder, string.content);
        sxml_sequence_release(string);
    }
    return status;
}

/* Which pieces fn:characters and fn:graphemes cut a string into. */
typedef enum piece_kind {
    SXML_UNICODE_PIECE_KIND_CHARACTER,
    SXML_UNICODE_PIECE_KIND_GRAPHEME_CLUSTER,
} piece_kind;

/* The pieces of the string argument as a sequence of xs:string items:
   the empty sequence for the empty sequence and the zero-length string. */
static sxml_status call_split(const sxml_function_call *call, piece_kind kind,
                              sxml_sequence *result) {
    sxml_string value = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_steps(call, value.codepoint_count);
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    if (kind == SXML_UNICODE_PIECE_KIND_CHARACTER) {
        sxml_codepoint_iterator iterator = {.string = value, .byte_offset = 0};
        uint32_t codepoint = 0;
        size_t start_offset = 0;
        while (status == SXML_STATUS_OK && sxml_codepoint_iterator_next(&iterator, &codepoint)) {
            status = append_string(
                call, builder,
                slice_of(value.utf8,
                         (sxml_byte_range){.byte_offset = start_offset,
                                           .byte_count = iterator.byte_offset - start_offset}));
            start_offset = iterator.byte_offset;
        }
    } else {
        sxml_grapheme_cluster_iterator iterator = {.string = value, .byte_offset = 0};
        sxml_byte_range cluster = {.byte_offset = 0, .byte_count = 0};
        while (status == SXML_STATUS_OK &&
               sxml_grapheme_cluster_iterator_next(&iterator, &cluster)) {
            status = append_string(call, builder, slice_of(value.utf8, cluster));
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* fn:characters($value as xs:string?) as xs:string* (F&O 4.0, section
   5.3.2): string-to-codepoints($value) ! char(.). */
static sxml_status call_characters(const sxml_function_call *call, sxml_sequence *result) {
    return call_split(call, SXML_UNICODE_PIECE_KIND_CHARACTER, result);
}

/* fn:graphemes($value as xs:string?) as xs:string* (F&O 4.0, section
   5.3.3): the extended grapheme clusters of UAX #29. */
static sxml_status call_graphemes(const sxml_function_call *call, sxml_sequence *result) {
    return call_split(call, SXML_UNICODE_PIECE_KIND_GRAPHEME_CLUSTER, result);
}

/* --------------------------------------------------------------- fn:char */

static const sxml_static_error invalid_character_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCH0005"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The value is neither the codepoint of a permitted character, nor an HTML5 "
        "character reference name, nor one of \\n \\r \\t \\b \\f (F&O 4.0, fn:char)"),
};

/* A permitted character (F&O 4.0, section 17.4.3: XML 1.1 characters). */
static bool is_permitted_character(int64_t codepoint) {
    return (codepoint >= 0x1 && codepoint <= 0xD7FF) ||
           (codepoint >= 0xE000 && codepoint <= 0xFFFD) ||
           (codepoint >= 0x10000 && codepoint <= 0x10FFFF);
}

/* The codepoint of a backslash-escape \n \r \t \b \f, or 0. */
static uint32_t escaped_codepoint(sxml_byte_view name) {
    if (name.byte_count != 2 || name.bytes[0] != '\\') {
        return 0;
    }
    switch (name.bytes[1]) {
    case 'n':
        return 0x0AU;
    case 'r':
        return 0x0DU;
    case 't':
        return 0x09U;
    case 'b':
        return 0x08U;
    case 'f':
        return 0x0CU;
    default:
        return 0;
    }
}

/* fn:char($value as (xs:string | xs:positiveInteger)) as xs:string (F&O
   4.0, section 5.3.1). */
static sxml_status call_char(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = value.has_item ? sxml_atomic_item_get_datum(value.item, &datum)
                                : SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_unicode_character_reference_value reference = {.codepoints = {0, 0},
                                                        .codepoint_count = 0};
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        if (is_permitted_character(datum.datum_details.integer)) {
            reference.codepoints[0] = (uint32_t)datum.datum_details.integer;
            reference.codepoint_count = 1;
        }
    } else if (datum.datum_kind == SXML_DATUM_KIND_STRING) {
        const sxml_byte_view name = datum.datum_details.string.utf8;
        const uint32_t escaped = escaped_codepoint(name);
        if (escaped != 0) {
            reference.codepoints[0] = escaped;
            reference.codepoint_count = 1;
        } else if (!sxml_unicode_find_character_reference(name, &reference)) {
            reference.codepoint_count = 0;
        }
    } else if (datum.datum_kind != SXML_DATUM_KIND_DECIMAL) {
        /* Coercion leaves a string or a positive integer; an integer
           beyond int64_t is a decimal datum and no codepoint. */
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (reference.codepoint_count == 0) {
        return sxml_function_library_report(call, &invalid_character_error);
    }
    sxml_library_text text = {.item_heap = call->item_heap};
    for (size_t codepoint_index = 0;
         status == SXML_STATUS_OK && codepoint_index < reference.codepoint_count;
         codepoint_index += 1) {
        status = sxml_function_library_text_append_codepoint(
            &text, reference.codepoints[codepoint_index]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(
            call->item_heap, sxml_function_library_text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* ---------------------------------------------------- fn:normalize-unicode */

static const sxml_static_error unsupported_form_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCH0003"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The normalization form is not one of NFC, NFD, NFKC, NFKD, FULLY-NORMALIZED or "
        "\"\" (F&O 4.0, fn:normalize-unicode)"),
};

/* The longest form name, "FULLY-NORMALIZED". */
#define SXML_UNICODE_MAXIMUM_FORM_BYTE_COUNT 16U

/* The effective value of $form: upper-case(normalize-space($form)), kept
   while it is short ASCII (no supported form is anything else). */
typedef struct effective_form {
    char bytes[SXML_UNICODE_MAXIMUM_FORM_BYTE_COUNT];
    size_t byte_count;
    bool is_too_long;
} effective_form;

static bool is_xml_whitespace(uint32_t codepoint) {
    return codepoint == ' ' || codepoint == '\t' || codepoint == '\n' || codepoint == '\r';
}

static void append_form_codepoint(effective_form *form, uint32_t codepoint) {
    if (codepoint >= 0x80U || form->byte_count >= SXML_UNICODE_MAXIMUM_FORM_BYTE_COUNT) {
        form->is_too_long = true;
        return;
    }
    form->bytes[form->byte_count] = (char)codepoint;
    form->byte_count += 1;
}

static void read_effective_form(sxml_string form_text, effective_form *form) {
    *form = (effective_form){.byte_count = 0, .is_too_long = false};
    sxml_codepoint_iterator iterator = {.string = form_text, .byte_offset = 0};
    uint32_t codepoint = 0;
    bool has_pending_space = false;
    while (!form->is_too_long && sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        if (is_xml_whitespace(codepoint)) {
            /* fn:normalize-space: runs become one space, none at the
               ends. */
            has_pending_space = form->byte_count > 0;
            continue;
        }
        if (has_pending_space) {
            append_form_codepoint(form, ' ');
            has_pending_space = false;
        }
        sxml_case_mapping mapping = {.codepoint_count = 0};
        sxml_function_library_map_case(
            (sxml_case_mapping_request){.codepoint = codepoint,
                                        .mapping_kind = SXML_CASE_MAPPING_KIND_UPPER},
            &mapping);
        for (size_t mapping_index = 0; mapping_index < mapping.codepoint_count;
             mapping_index += 1) {
            append_form_codepoint(form, mapping.codepoints[mapping_index]);
        }
    }
}

static bool form_is(const effective_form *form, sxml_byte_view name) {
    return form->byte_count == name.byte_count &&
           memcmp(form->bytes, name.bytes, name.byte_count) == 0;
}

/* The form an effective value names, or false. */
static bool find_form(const effective_form *form, sxml_unicode_normalization_form *found) {
    static const struct {
        sxml_byte_view name;
        sxml_unicode_normalization_form normalization_form;
    } forms[] = {
        {SXML_BYTE_VIEW_INITIALIZER("NFC"), SXML_UNICODE_NORMALIZATION_FORM_NFC},
        {SXML_BYTE_VIEW_INITIALIZER("NFD"), SXML_UNICODE_NORMALIZATION_FORM_NFD},
        {SXML_BYTE_VIEW_INITIALIZER("NFKC"), SXML_UNICODE_NORMALIZATION_FORM_NFKC},
        {SXML_BYTE_VIEW_INITIALIZER("NFKD"), SXML_UNICODE_NORMALIZATION_FORM_NFKD},
        {SXML_BYTE_VIEW_INITIALIZER("FULLY-NORMALIZED"),
         SXML_UNICODE_NORMALIZATION_FORM_FULLY_NORMALIZED},
    };
    if (form->is_too_long) {
        return false;
    }
    for (size_t form_index = 0; form_index < sizeof(forms) / sizeof(forms[0]); form_index += 1) {
        if (form_is(form, forms[form_index].name)) {
            *found = forms[form_index].normalization_form;
            return true;
        }
    }
    return false;
}

/* fn:normalize-unicode($value as xs:string?, $form as xs:string? := "NFC")
   as xs:string (F&O 4.0, section 5.3.14): $form is :default-on-empty, so
   the empty sequence is "NFC" too. */
static sxml_status call_normalize_unicode(const sxml_function_call *call,
                                          sxml_sequence *result) {
    sxml_string value;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    sxml_optional_item form_argument = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 1, &form_argument);
    }
    sxml_unicode_normalization_form normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFC;
    bool is_unchanged = false;
    if (status == SXML_STATUS_OK && form_argument.has_item) {
        sxml_string form_text;
        status = sxml_function_library_read_string_argument(call, 1, &form_text);
        effective_form form;
        if (status == SXML_STATUS_OK) {
            read_effective_form(form_text, &form);
            /* The zero-length string: no normalization. */
            is_unchanged = !form.is_too_long && form.byte_count == 0;
            if (!is_unchanged && !find_form(&form, &normalization_form)) {
                status = sxml_function_library_report(call, &unsupported_form_error);
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_steps(call, value.codepoint_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (is_unchanged) {
        return sxml_function_library_make_string(call->item_heap, value.utf8, result);
    }
    sxml_unicode_normalized_text text = {.bytes = NULL, .byte_count = 0};
    status = sxml_unicode_normalize(
        &(sxml_unicode_normalization_request){
            .allocator = sxml_item_heap_as_allocator(call->item_heap),
            .normalization_form = normalization_form,
            .input = value},
        &text);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(
            call->item_heap, sxml_unicode_normalized_text_view(&text), result);
        sxml_unicode_normalized_text_release(&text);
    }
    return status;
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter value_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?")};
static const sxml_function_parameter char_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "(xs:string | xs:positiveInteger)")};
static const sxml_function_parameter normalize_unicode_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("form", "xs:string?")};

static const sxml_native_function unicode_functions[] = {
    SXML_FUNCTION_ROW("char", char_parameters, "xs:string", 0, call_char),
    SXML_FUNCTION_ROW("characters", value_parameters, "xs:string*", 0, call_characters),
    SXML_FUNCTION_ROW("graphemes", value_parameters, "xs:string*", 0, call_graphemes),
    SXML_FUNCTION_ROW("normalize-unicode", normalize_unicode_parameters, "xs:string", 0,
                      call_normalize_unicode),
};

const sxml_native_function_view sxml_function_library_unicode =
    SXML_LIBRARY_PART(unicode_functions);
