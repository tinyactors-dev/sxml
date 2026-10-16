/*
 * Fuzz target: the lexical forms of casting (src/cast.c, src/cast_lexical.c,
 * and through xs:decimal and xs:integer, src/decimal.c; packet 1.26).
 *
 * The input is a byte that picks the target type (its value modulo the
 * number of rows of cast_targets) and the text to cast, which must be
 * well-formed UTF-8. The text is cast as an xs:string would be (F&O 4.0,
 * section 19.2), and the canonical text of every item of the result is
 * written (section 19.1.2). For an atomic target type, the canonical text
 * must cast back to an item with the same canonical text: a canonical
 * representation is in the lexical space and denotes the same value (XSD
 * 1.1 Part 2, section 2.3.2.2).
 *
 * tools/fuzz-seeds.py reads the local names of cast_targets from this
 * file to make seeds from xs:TYPE("...") constructor calls of the QT4
 * test suite, so each row keeps the form {SXML_TYPE_..., "localName"}.
 *
 * Golden file: tests/cast_test.c (cast_fixture, outcome_append_item).
 */
#include "cast_internal.h"
#include "fuzz_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bytes of canonical text kept per item for the round trip. */
#define SXML_FUZZ_CANONICAL_TEXT_CAPACITY 4096U

typedef struct cast_target {
    sxml_builtin_type builtin_type;
    const char *local_name;
} cast_target;

static const cast_target cast_targets[] = {
    {SXML_TYPE_XS_UNTYPED_ATOMIC, "untypedAtomic"},
    {SXML_TYPE_XS_STRING, "string"},
    {SXML_TYPE_XS_BOOLEAN, "boolean"},
    {SXML_TYPE_XS_DECIMAL, "decimal"},
    {SXML_TYPE_XS_FLOAT, "float"},
    {SXML_TYPE_XS_DOUBLE, "double"},
    {SXML_TYPE_XS_DURATION, "duration"},
    {SXML_TYPE_XS_DATE_TIME, "dateTime"},
    {SXML_TYPE_XS_TIME, "time"},
    {SXML_TYPE_XS_DATE, "date"},
    {SXML_TYPE_XS_G_YEAR_MONTH, "gYearMonth"},
    {SXML_TYPE_XS_G_YEAR, "gYear"},
    {SXML_TYPE_XS_G_MONTH_DAY, "gMonthDay"},
    {SXML_TYPE_XS_G_DAY, "gDay"},
    {SXML_TYPE_XS_G_MONTH, "gMonth"},
    {SXML_TYPE_XS_HEX_BINARY, "hexBinary"},
    {SXML_TYPE_XS_BASE64_BINARY, "base64Binary"},
    {SXML_TYPE_XS_ANY_URI, "anyURI"},
    {SXML_TYPE_XS_QNAME, "QName"},
    {SXML_TYPE_XS_INTEGER, "integer"},
    {SXML_TYPE_XS_NON_POSITIVE_INTEGER, "nonPositiveInteger"},
    {SXML_TYPE_XS_NEGATIVE_INTEGER, "negativeInteger"},
    {SXML_TYPE_XS_LONG, "long"},
    {SXML_TYPE_XS_INT, "int"},
    {SXML_TYPE_XS_SHORT, "short"},
    {SXML_TYPE_XS_BYTE, "byte"},
    {SXML_TYPE_XS_NON_NEGATIVE_INTEGER, "nonNegativeInteger"},
    {SXML_TYPE_XS_UNSIGNED_LONG, "unsignedLong"},
    {SXML_TYPE_XS_UNSIGNED_INT, "unsignedInt"},
    {SXML_TYPE_XS_UNSIGNED_SHORT, "unsignedShort"},
    {SXML_TYPE_XS_UNSIGNED_BYTE, "unsignedByte"},
    {SXML_TYPE_XS_POSITIVE_INTEGER, "positiveInteger"},
    {SXML_TYPE_XS_NORMALIZED_STRING, "normalizedString"},
    {SXML_TYPE_XS_TOKEN, "token"},
    {SXML_TYPE_XS_LANGUAGE, "language"},
    {SXML_TYPE_XS_NMTOKEN, "NMTOKEN"},
    {SXML_TYPE_XS_NAME, "Name"},
    {SXML_TYPE_XS_NCNAME, "NCName"},
    {SXML_TYPE_XS_ID, "ID"},
    {SXML_TYPE_XS_IDREF, "IDREF"},
    {SXML_TYPE_XS_ENTITY, "ENTITY"},
    {SXML_TYPE_XS_DAY_TIME_DURATION, "dayTimeDuration"},
    {SXML_TYPE_XS_YEAR_MONTH_DURATION, "yearMonthDuration"},
    {SXML_TYPE_XS_DATE_TIME_STAMP, "dateTimeStamp"},
    /* List and union types: no round trip, since each item's canonical
       text belongs to its item type. */
    {SXML_TYPE_XS_NMTOKENS, "NMTOKENS"},
    {SXML_TYPE_XS_IDREFS, "IDREFS"},
    {SXML_TYPE_XS_ENTITIES, "ENTITIES"},
    {SXML_TYPE_XS_NUMERIC, "numeric"},
};

#define SXML_FUZZ_CAST_TARGET_COUNT (sizeof(cast_targets) / sizeof(cast_targets[0]))

/* The canonical text of one item, copied. */
typedef struct canonical_copy {
    uint8_t bytes[SXML_FUZZ_CANONICAL_TEXT_CAPACITY];
    size_t byte_count;
    bool is_written;
} canonical_copy;

/* What one cast is. */
typedef struct text_cast {
    sxml_byte_view text;
    sxml_type_identifier target_type;
    sxml_namespace_binding_view namespace_bindings;
} text_cast;

/* Writes the canonical text of item to *copy (is_written false when it
   does not fit). */
static void copy_canonical_text(const sxml_fuzz_fixture *fixture,
                                sxml_item item, canonical_copy *copy) {
    sxml_canonical_text canonical_text = {.text = {.byte_count = 0}};
    const sxml_status status = sxml_cast_write_canonical_text(
        fixture->item_heap,
        &(sxml_canonical_text_request){
            .item = item,
            .output = {.bytes = copy->bytes, .byte_count = sizeof(copy->bytes)}},
        &canonical_text);
    sxml_fuzz_check_status(status, "sxml_cast_write_canonical_text");
    copy->is_written = status == SXML_STATUS_OK;
    copy->byte_count = 0;
    if (copy->is_written && canonical_text.text.byte_count != 0) {
        copy->byte_count = canonical_text.text.byte_count;
        /* The text may be borrowed from the item, or already be the
           output. */
        memmove(copy->bytes, canonical_text.text.bytes, copy->byte_count);
    }
}

/* Casts copy back to cast->target_type and checks that it has the same
   canonical text. */
static void check_round_trip(const sxml_fuzz_fixture *fixture,
                             const text_cast *cast, const canonical_copy *copy) {
    sxml_sequence value = sxml_sequence_empty();
    const sxml_status status = sxml_cast_make_value_from_text(
        fixture->item_heap,
        &(sxml_text_cast_request){
            .text = {.bytes = copy->bytes, .byte_count = copy->byte_count},
            .target_type = cast->target_type,
            .namespace_bindings = cast->namespace_bindings,
            .error_record = NULL},
        &value);
    if (status == SXML_STATUS_LIMIT_EXCEEDED ||
        status == SXML_STATUS_OUT_OF_MEMORY) {
        return;
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        sxml_fuzz_check_status(sxml_sequence_get_item(
                                   value,
                                   (sxml_item_request){.item_index = 0,
                                                       .error_record = NULL},
                                   &item),
                               "sxml_sequence_get_item");
    }
    canonical_copy again_storage = {.byte_count = 0, .is_written = false};
    canonical_copy *again = &again_storage;
    if (status == SXML_STATUS_OK) {
        copy_canonical_text(fixture, item, again);
    }
    if (status != SXML_STATUS_OK ||
        (again->is_written &&
         (again->byte_count != copy->byte_count ||
          memcmp(again->bytes, copy->bytes, copy->byte_count) != 0))) {
        (void)fprintf(stderr,
                      "canonical text \"%.*s\" does not round-trip (status "
                      "%d, again \"%.*s\")\n",
                      (int)copy->byte_count, (const char *)copy->bytes,
                      (int)status, (int)again->byte_count,
                      (const char *)again->bytes);
        abort();
    }
    sxml_sequence_release(value);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT(readability-identifier-naming)
    if (size == 0) {
        return 0;
    }
    const cast_target *target = &cast_targets[data[0] % SXML_FUZZ_CAST_TARGET_COUNT];
    const bool is_atomic = target->builtin_type < SXML_TYPE_XS_NMTOKENS;
    const sxml_byte_view text = sxml_fuzz_input_view(data + 1, size - 1);
    sxml_string validated = {.codepoint_count = 0};
    if (sxml_string_from_utf8(text, &validated) != SXML_STATUS_OK) {
        return 0;
    }
    sxml_fuzz_fixture fixture;
    if (sxml_fuzz_fixture_create(
            &(sxml_fuzz_fixture_configuration){.has_function_library = false},
            &fixture) != SXML_STATUS_OK) {
        return 0;
    }
    sxml_namespace_binding binding = {.namespace_prefix = {.index = 0},
                                      .namespace_uri = {.index = 0}};
    if (sxml_name_pool_intern_namespace_prefix(
            fixture.name_pool, SXML_BYTE_VIEW_FROM_LITERAL("p"),
            &binding.namespace_prefix) == SXML_STATUS_OK &&
        sxml_name_pool_intern_namespace_uri(
            fixture.name_pool, SXML_BYTE_VIEW_FROM_LITERAL("urn:p"),
            &binding.namespace_uri) == SXML_STATUS_OK) {
        const text_cast cast = {
            .text = text,
            .target_type = sxml_builtin_type_identifier(target->builtin_type),
            .namespace_bindings = {.namespace_bindings = &binding,
                                   .namespace_binding_count = 1}};
        sxml_error_record error_record = {.value = NULL};
        sxml_sequence value = sxml_sequence_empty();
        const sxml_status status = sxml_cast_make_value_from_text(
            fixture.item_heap,
            &(sxml_text_cast_request){.text = cast.text,
                                      .target_type = cast.target_type,
                                      .namespace_bindings =
                                          cast.namespace_bindings,
                                      .error_record = &error_record},
            &value);
        sxml_fuzz_check_status(status, "sxml_cast_make_value_from_text");
        canonical_copy copy = {.byte_count = 0, .is_written = false};
        sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
        sxml_iteration_step step = {.has_item = status == SXML_STATUS_OK};
        while (step.has_item) {
            const sxml_status next_status =
                sxml_sequence_iterator_next(&iterator, NULL, &step);
            sxml_fuzz_check_status(next_status, "sxml_sequence_iterator_next");
            if (next_status != SXML_STATUS_OK) {
                break;
            }
            if (step.has_item) {
                copy_canonical_text(&fixture, step.item, &copy);
                if (is_atomic && copy.is_written) {
                    check_round_trip(&fixture, &cast, &copy);
                }
            }
        }
        sxml_sequence_release(value);
    }
    sxml_fuzz_fixture_destroy_checking_leaks(&fixture);
    return 0;
}
