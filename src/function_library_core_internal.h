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
#include "collation_internal.h"
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

/* A parameter whose default is the string value of the context value
   ("$value := fn:string(.)"). */
#define SXML_CONTEXT_STRING_PARAMETER(name_text, type_text)                   \
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                 \
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER(type_text),                 \
     .parameter_default = SXML_PARAMETER_DEFAULT_CONTEXT_VALUE,               \
     .defaults_to_string_value = true}

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

/* ------------------------------------------------------------ Collations */

/*
 * Finds the collation collation_uri names for call, by the rules of F&O
 * 4.0, section 5.2.7 "Choosing a collation" (D116), and writes it to
 * *collation: the zero-length string is the default collation (the
 * Unicode codepoint collation until the static context hands its own
 * down); a relative URI is resolved against the static base URI; a URI
 * sxml does not support (sxml_collation_find), and a relative one without
 * a base URI, is FOCH0002. This is the one resolver every collation
 * argument and option goes through.
 */
SXML_NODISCARD sxml_status sxml_function_library_find_collation(
    const sxml_function_call *call, sxml_byte_view collation_uri,
    sxml_collation *collation);

/*
 * Resolves a "$collation as xs:string?" argument (the empty sequence is
 * the default collation) as sxml_function_library_find_collation does.
 */
SXML_NODISCARD sxml_status sxml_function_library_resolve_collation(
    const sxml_function_call *call, size_t argument_index, sxml_collation *collation);

/* The URI of the Unicode codepoint collation (F&O 4.0, section 5.2.3). */
sxml_byte_view sxml_function_library_codepoint_collation_uri(void);

/* Two atomic items to order under a collation. */
typedef struct sxml_collated_comparison {
    const sxml_collation *collation;
    /* As for sxml_atomic_item_compare_in_heap. */
    sxml_atomic_comparison comparison;
} sxml_collated_comparison;

/*
 * Orders two atomic items as fn:compare($first, $second, $collation)
 * does (F&O 4.0, fn:compare): strings, xs:anyURI and xs:untypedAtomic
 * under the collation, everything else as sxml_atomic_item_compare_in_heap
 * (whose failures it returns). The collation's working memory comes from
 * item_heap.
 */
SXML_NODISCARD sxml_status sxml_function_library_compare_collated(
    sxml_item_heap *item_heap, const sxml_collated_comparison *comparison,
    sxml_ordering *ordering);

/*
 * Writes to *key the collation key of an atomic item for hashing and
 * equality under collation (fn:distinct-values and its relatives): for a
 * string, xs:anyURI or xs:untypedAtomic under a collation other than the
 * codepoint collation, its sxml_collation_make_key bytes and true in
 * *is_keyed; otherwise *is_keyed is false and *key stays empty.
 */
typedef struct sxml_collated_key_request {
    const sxml_collation *collation;
    sxml_item item;
} sxml_collated_key_request;

typedef struct sxml_collated_key {
    bool is_keyed;
    /* Owned through the item heap's allocator when is_keyed. */
    sxml_collation_key key;
} sxml_collated_key;

SXML_NODISCARD sxml_status sxml_function_library_make_collated_key(
    sxml_item_heap *item_heap, sxml_collated_key_request request, sxml_collated_key *key);

/*
 * The item that contextual equality compares in place of item (F&O 4.0,
 * fn:distinct-values and fn:deep-equal; XQuery 4.0, 4.13.8 group by): a
 * date, time or Gregorian value without a timezone is compared as if it
 * had the implicit timezone of call's dynamic context. Writes that item,
 * owned, to *adjusted, or a zero item when item is compared as it is
 * (another value, a timezone, or no dynamic context). Returns the heap's
 * and the clock's failures.
 */
SXML_NODISCARD sxml_status sxml_function_library_adjust_to_implicit_timezone(
    const sxml_function_call *call, sxml_item item, sxml_item *adjusted);

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

/* ---------------------------------------------------------------- Steps */

/*
 * Charges step_count evaluation steps to the evaluation running (D89,
 * D91): a body that loops over many items, codepoints or comparisons
 * without running tasks (fn:string-join, fn:sort, fn:distinct-values,
 * fn:deep-equal, the regex functions) calls it before the work, so
 * maximum_evaluation_step_count bounds it too. Does nothing without a
 * dynamic context. Returns SXML_STATUS_LIMIT_EXCEEDED past the bound.
 */
SXML_NODISCARD sxml_status sxml_function_library_charge_steps(
    const sxml_function_call *call, uint64_t step_count);

/*
 * Charges one step per item of sequence (counting it, which reads a lazy
 * sequence), as sxml_function_library_charge_steps. Also returns the
 * sequence's failures.
 */
SXML_NODISCARD sxml_status sxml_function_library_charge_items(
    const sxml_function_call *call, sxml_sequence sequence);

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

/* Appends the canonical text of an atomic item, its value cast to
   xs:string (F&O 4.0, section 23.1.2); defined in
   src/function_library_string.c. Returns the cast's and the heap's
   failures. */
SXML_NODISCARD sxml_status sxml_function_library_text_append_atomic(sxml_library_text *text,
                                                                    sxml_item item);

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

/* --------------------------------------------------------------- Options */

/*
 * The option parameter conventions (F&O 4.0, section 1.7), read once for
 * every function that adopts them; defined in
 * src/function_library_options.c. A function names its options and the
 * type each value is coerced to; its body then checks enumerations and
 * other permitted values itself.
 */
typedef struct sxml_option_definition {
    /* The option name: a key that is the same key as this xs:string. */
    sxml_byte_view option_name;
    /* The required type of the value. */
    sxml_sequence_type option_type;
} sxml_option_definition;

typedef struct sxml_option_definition_view {
    const sxml_option_definition *definitions;
    size_t definition_count;
} sxml_option_definition_view;

/* A definition in a static table: name_text a string literal, type_index
   an sxml_builtin_type, occurrence_value an sxml_occurrence. */
#define SXML_FUNCTION_OPTION(name_text, type_index, occurrence_value)         \
    {.option_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                    \
     .option_type = {.item_type = {.index = (type_index)},                    \
                     .occurrence = (occurrence_value)}}

/* The most options one function defines (fn:transform has 33). */
#define SXML_FUNCTION_OPTION_LIMIT ((size_t)40)

/* Which options of argument argument_index (map(*)? after coercion) to
   read. */
typedef struct sxml_option_request {
    size_t argument_index;
    sxml_option_definition_view definitions;
} sxml_option_request;

/* The values found, by position among the definitions; each owned,
   coerced to its type. Zero-initialize; release with
   sxml_function_library_release_options on every path. */
typedef struct sxml_option_values {
    bool is_present[SXML_FUNCTION_OPTION_LIMIT];
    sxml_sequence values[SXML_FUNCTION_OPTION_LIMIT];
} sxml_option_values;

/* True for an options map key that is an xs:QName in a namespace: an
   implementation-defined option, which sxml ignores (F&O 4.0, section
   1.7, option parameter conventions); any other key that names no option
   is XPTY0004. */
bool sxml_function_library_option_key_is_extension(sxml_item key,
                                                   const sxml_name_pool *name_pool);

/*
 * Reads the options map of request->argument_index into *values: a key
 * that names no definition is XPTY0004 unless it is an xs:QName in a
 * namespace (an implementation-defined option, ignored); each value is
 * coerced to its definition's type by the coercion rules (XPTY0004,
 * FORG0001, ... when it cannot be). Without a dynamic context values are
 * taken as they are. More than SXML_FUNCTION_OPTION_LIMIT definitions is
 * SXML_STATUS_INVALID_ARGUMENT. Returns the map's, the coercion's and the
 * heap's failures.
 */
SXML_NODISCARD sxml_status sxml_function_library_read_options(
    const sxml_function_call *call, const sxml_option_request *request,
    sxml_option_values *values);

/* Releases the values and leaves *values empty. */
void sxml_function_library_release_options(sxml_option_values *values);

/* The single atomic item of a present option, as read (types of exactly
   one atomic item only). */
typedef struct sxml_option_lookup {
    const sxml_option_values *values;
    size_t option_index;
} sxml_option_lookup;

/* True when the option is present with the xs:boolean value true. */
bool sxml_function_library_option_is_true(sxml_option_lookup lookup);

/* True when the option is present with the xs:boolean value false. */
bool sxml_function_library_option_is_false(sxml_option_lookup lookup);

/* The string of a present option of a string type; NULL text otherwise. */
sxml_string sxml_function_library_option_string(sxml_option_lookup lookup);

/* ---------------------------------------------------- Parsing options */

/* What the XML parsing options of fn:doc, fn:parse-xml and
   fn:parse-xml-fragment ask for that sxml honours (F&O 4.0, sections
   17.1.1 and 17.2.1). */
typedef struct sxml_document_options {
    /* strip-space="all": whitespace-only text nodes are removed. */
    bool strips_space;
    /* strip-space="none": none are, not even by the host's document
       preparation (sxml_document_request.skips_preparation). */
    bool keeps_space;
    /* dtd-validation=true: the document is validated against its DTD
       (FODC0007 when it is not valid; D199). */
    bool validates_dtd;
    /* trust-external=true: external entities and an external DTD may be
       read; otherwise a reference to one is FODC0016. */
    bool trusts_external;
    /* The xsd-validation option's xs:string item, owned (the caller
       releases it); empty when absent or "skip". */
    sxml_sequence xsd_validation;
} sxml_document_options;

/*
 * Reads fn:doc's options map from argument argument_index into *options,
 * defined in src/function_library_json.c: dtd-validation and xinclude true
 * are FODC0013, a strip-space value outside the enumeration XPTY0004;
 * xsd-validation is kept for sxml_schema_scope_validate_document; stable,
 * trust-external and the location options are accepted.
 */
SXML_NODISCARD sxml_status sxml_function_library_read_document_options(
    const sxml_function_call *call, size_t argument_index, sxml_document_options *options);

/*
 * Copies a document node into a new document without the whitespace-only
 * text nodes outside xml:space="preserve" (strip-space="all"), keeping its
 * document URI and base URI, and writes it, owned, to *result. Defined in
 * src/function_library_json.c.
 */
SXML_NODISCARD sxml_status sxml_function_library_strip_document(
    const sxml_function_call *call, sxml_item document_node, sxml_sequence *result);

/*
 * Reads the text of fn:json-doc's source (F&O 4.0, section 17.4.5:
 * fn:unparsed-text($source), argument 0) and writes it, an owned
 * xs:string, to *text; the empty sequence for an empty $source. Defined in
 * src/function_library_resource.c for the json-doc row of
 * src/function_library_json.c (D143). Returns fn:unparsed-text's errors.
 */
SXML_NODISCARD sxml_status sxml_function_library_read_json_document(
    const sxml_function_call *call, sxml_sequence *text);

/* ------------------------------------------------------------ Deep equality */

/*
 * fn:deep-equal($input1, $input2, $options := {}) (F&O 4.0, section
 * 2.2.4), defined in src/function_library_deep_equal.c: the resumable body
 * of the row in src/function_library_sequence.c (it calls the items-equal
 * option's function, D116), and a leaf entry for callers that pass no
 * options map (array:index-of), which returns
 * SXML_STATUS_UNSUPPORTED_FEATURE should a call be needed.
 */
extern const sxml_resumable_native_body sxml_function_library_deep_equal_body;

SXML_NODISCARD sxml_status sxml_function_library_call_deep_equal(
    const sxml_function_call *call, sxml_sequence *result);
