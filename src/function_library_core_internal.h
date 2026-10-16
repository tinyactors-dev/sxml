#pragma once

/*
 * What the core parts of the function library share (packet 1.20,
 * docs/DECISIONS.md D78). Internal to sxml.
 *
 * The rows of src/function_library_numeric.c, _string.c, _sequence.c,
 * _node.c, _context.c and _error.c read their arguments, build text and
 * check collations the same way; these helpers are defined in
 * src/function_library.c, the case mappings in src/function_library_case.c.
 *
 * Arguments reach a body already coerced to the parameter types (XPath
 * 4.0, section 3.4), so an "xs:string?" argument is empty or one xs:string
 * item, and a parameter beyond the arity of the call (a default supplied by
 * the body) reads as the empty sequence.
 *
 * Thread safety: the thread of the item heap (D35).
 */
#include "function_library_internal.h"
#include "sxml/string.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ Rows */

/* One parameter of a row: name, F&O type text, default (D55). */
#define SXML_FUNCTION_PARAMETER(name_text, type_text, default_kind)           \
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                 \
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER(type_text),                 \
     .parameter_default = (default_kind)}

/* A required parameter. */
#define SXML_REQUIRED_PARAMETER(name_text, type_text)                         \
    SXML_FUNCTION_PARAMETER(name_text, type_text,                             \
                            SXML_PARAMETER_DEFAULT_REQUIRED)

/* A parameter whose default the body supplies ("()", "", 0, ...). */
#define SXML_OPTIONAL_PARAMETER(name_text, type_text)                         \
    SXML_FUNCTION_PARAMETER(name_text, type_text,                             \
                            SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY)

/* A parameter whose default is the context value ("$node := ."). */
#define SXML_CONTEXT_PARAMETER(name_text, type_text)                          \
    SXML_FUNCTION_PARAMETER(name_text, type_text,                             \
                            SXML_PARAMETER_DEFAULT_CONTEXT_VALUE)

/* A row in the fn namespace with a leaf body; parameter_array is a static
   array of parameters. */
#define SXML_FUNCTION_ROW(name_text, parameter_array, result_text,            \
                          property_bits, body)                                \
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,                          \
     .properties = (property_bits),                                           \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                     \
     .parameters = {(parameter_array),                                        \
                    sizeof(parameter_array) / sizeof((parameter_array)[0])},  \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                  \
     .call = (body),                                                          \
     .resumable_body = NULL}

/* A row in the fn namespace without parameters. */
#define SXML_NULLARY_FUNCTION_ROW(name_text, result_text, property_bits,      \
                                  body)                                       \
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,                          \
     .properties = (property_bits),                                           \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                     \
     .parameters = {NULL, 0},                                                 \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                  \
     .call = (body),                                                          \
     .resumable_body = NULL}

/* The view of a part over its row array. */
#define SXML_LIBRARY_PART(row_array)                                          \
    {.native_functions = (row_array),                                         \
     .native_function_count = sizeof(row_array) / sizeof((row_array)[0])}

/* ------------------------------------------------------------ Arguments */

/*
 * Argument argument_index of call, borrowed; the empty sequence when the
 * call has fewer arguments (a parameter whose default the body supplies).
 */
sxml_sequence sxml_function_library_get_argument(const sxml_function_call *call,
                                                 size_t argument_index);

/* An argument of zero or one item. */
typedef struct sxml_optional_item {
    bool has_item;
    /* Borrowed from the argument. */
    sxml_item item;
} sxml_optional_item;

/*
 * Reads argument argument_index of call, of type T? after coercion, into
 * *optional_item. Returns a lazy sequence's failures, and
 * SXML_STATUS_INVALID_ARGUMENT for more than one item (coercion prevents
 * it).
 */
SXML_NODISCARD sxml_status sxml_function_library_read_optional_item(
    const sxml_function_call *call, size_t argument_index,
    sxml_optional_item *optional_item);

/*
 * Reads an "xs:string?" argument into *string, borrowed from the item: the
 * empty string for the empty sequence (F&O 4.0, section 1.5.2: "the empty
 * sequence is treated as the zero-length string"). Failures as
 * sxml_function_library_read_optional_item.
 */
SXML_NODISCARD sxml_status sxml_function_library_read_string_argument(
    const sxml_function_call *call, size_t argument_index, sxml_string *string);

/*
 * Checks a "$collation as xs:string?" argument: absent, empty or the
 * Unicode codepoint collation URI are the default collation (F&O 4.0,
 * section 5.3.2); any other URI is FOCH0002, since Phase 1 supports no
 * other collation.
 */
SXML_NODISCARD sxml_status sxml_function_library_check_collation(
    const sxml_function_call *call, size_t argument_index);

/* The URI of the Unicode codepoint collation (F&O 4.0, section 5.3.2). */
sxml_byte_view sxml_function_library_codepoint_collation_uri(void);

/* The positions of fn:subsequence and fn:substring (F&O 4.0, sections
   14.1.14 and 5.4.3): p with round($start) <= p < round($start) +
   round($length), as indices from 0. */
typedef struct sxml_position_span {
    bool is_empty;
    /* Set only inside the computation; never on a returned span. */
    bool has_overflow;
    uint64_t first_index;
    /* False: to the end. */
    bool has_end;
    uint64_t end_index;
} sxml_position_span;

/* Which arguments are $start (xs:numeric) and $length (xs:numeric?). */
typedef struct sxml_position_arguments {
    size_t start_argument;
    size_t length_argument;
} sxml_position_arguments;

/*
 * Computes the span of $start and $length into *span: doubles (and
 * floats) as doubles, with NaN and infinities as F&O 4.0 says, integers
 * and decimals exactly (QT4 subsequence-40-011, PR2543), raising FOAR0002
 * when integer positions pass int64_t. Returns the arguments' failures.
 */
SXML_NODISCARD sxml_status sxml_function_library_read_position_span(
    const sxml_function_call *call, sxml_position_arguments arguments,
    sxml_position_span *span);

/* -------------------------------------------------------------- Results */

/*
 * Makes an xs:string item of utf8 (copied; well-formed UTF-8) and writes
 * it, owned, to *result. Returns the heap's failures.
 */
SXML_NODISCARD sxml_status sxml_function_library_make_string(
    sxml_item_heap *item_heap, sxml_byte_view utf8, sxml_sequence *result);

/* The xs:boolean item of is_true as a singleton sequence. */
/* A truth value to make into an item. */
typedef struct sxml_boolean_result {
    bool is_true;
} sxml_boolean_result;

sxml_sequence sxml_function_library_make_boolean(sxml_boolean_result value);

/* Reports a static error record through call->error_record and returns
   its status. */
SXML_NODISCARD sxml_status sxml_function_library_report(
    const sxml_function_call *call, const sxml_static_error *error);

/* ------------------------------------------------------------------ Text */

/*
 * UTF-8 being built in item heap memory (counted against the heap's
 * limits): a body appends pieces, then makes the string item.
 * Zero-initialize, set item_heap, and call sxml_function_library_text_
 * release on every path.
 */
typedef struct sxml_library_text {
    sxml_item_heap *item_heap;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
} sxml_library_text;

/* Appends bytes (copied). Returns the heap's failures and
   SXML_STATUS_ARITHMETIC_OVERFLOW; text is then unchanged. */
SXML_NODISCARD sxml_status sxml_function_library_text_append(
    sxml_library_text *text, sxml_byte_view bytes);

/* Appends one codepoint (below 0x110000, not a surrogate) as UTF-8. */
SXML_NODISCARD sxml_status sxml_function_library_text_append_codepoint(
    sxml_library_text *text, uint32_t codepoint);

/* The text built so far, borrowed from text. */
sxml_byte_view sxml_function_library_text_view(const sxml_library_text *text);

/* Releases the memory of text and leaves it empty. */
void sxml_function_library_text_release(sxml_library_text *text);

/* ---------------------------------------------------------- Case mapping */

/* Which case a codepoint is mapped to (F&O 4.0, fn:upper-case and
   fn:lower-case). */
typedef enum sxml_case_mapping_kind {
    SXML_CASE_MAPPING_KIND_UPPER,
    SXML_CASE_MAPPING_KIND_LOWER,
} sxml_case_mapping_kind;

/* The codepoints one codepoint maps to: one, or up to three for the full
   mappings of SpecialCasing.txt ("ß" to "SS"). */
typedef struct sxml_case_mapping {
    uint32_t codepoints[3];
    size_t codepoint_count;
} sxml_case_mapping;

/* The query of sxml_function_library_map_case. */
typedef struct sxml_case_mapping_request {
    uint32_t codepoint;
    sxml_case_mapping_kind mapping_kind;
} sxml_case_mapping_request;

/*
 * Writes the Unicode full case mapping of request.codepoint (Unicode 16.0,
 * UnicodeData.txt simple mappings overridden by the unconditional entries
 * of SpecialCasing.txt; no language-sensitive mappings, as F&O 4.0
 * requires) to *mapping: the codepoint itself when it has none. Defined in
 * src/function_library_case.c, generated by tools/generate-case-tables.py.
 */
void sxml_function_library_map_case(sxml_case_mapping_request request,
                                    sxml_case_mapping *mapping);

/* ------------------------------------------------------------ Deep equality */

/*
 * The body of fn:deep-equal($input1, $input2, $options := {}) (F&O 4.0,
 * section 14.2.4), a row of src/function_library_sequence.c, defined in
 * src/function_library_deep_equal.c.
 */
SXML_NODISCARD sxml_status sxml_function_library_call_deep_equal(
    const sxml_function_call *call, sxml_sequence *result);
