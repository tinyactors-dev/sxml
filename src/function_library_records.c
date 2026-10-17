/*
 * The built-in record types (glossary `builtin_record_type`, D126): the
 * nominative record types of F&O 4.0, appendix C "Built-in nominal record
 * types", which "are available in the static context of every
 * application". The function library declares them in the type registry of
 * its item heap when it is initialized, so a sequence type of any
 * expression, and the type text of any row, can name them
 * (`$r instance of fn:dateTime-record`), and the bodies that return such a
 * record annotate it with sxml_function_library_make_record.
 *
 * Golden file: src/function_library.c (comments, status handling).
 *
 * Each type is one table row: its local name in the fn namespace and its
 * fields as the text of a structural record type, in the F&O 4.0 order and
 * spelling, so a row reads like the specification's table. A field whose
 * type allows the empty sequence is optional (D7). All names are declared
 * before any fields are resolved, so the fields may name the types of the
 * table, their own included (fn:random-number-generator-record's next).
 *
 * This file is not a library part: it defines no row view.
 */
#include "function_library_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"

#include <stddef.h>

typedef struct builtin_record_row {
    sxml_byte_view local_name;
    sxml_byte_view record_type_text;
} builtin_record_row;

#define SXML_BUILTIN_RECORD(local_name, record_type_text)                   \
    {SXML_BYTE_VIEW_INITIALIZER(local_name),                                \
     SXML_BYTE_VIEW_INITIALIZER(record_type_text)}

/* F&O 4.0, appendix C, with the section of each definition. */
static const builtin_record_row builtin_records[] = {
    /* 15.2.27 */
    SXML_BUILTIN_RECORD("array-sort-key-record",
                        "record(key as (fn(item()*) as xs:anyAtomicType*)?, "
                        "collation as xs:string?, "
                        "order as enum('ascending', 'descending')?)"),
    /* 14.6.5.2 */
    SXML_BUILTIN_RECORD("attribute-conversion-plan-record",
                        "record(type as enum('integer', 'decimal', 'double', 'boolean', "
                        "'string', 'skip')?)"),
    /* 6.3.7 */
    SXML_BUILTIN_RECORD("captured-group-record",
                        "record(value as xs:string, position as xs:integer, "
                        "nr as xs:integer, name as xs:string?)"),
    /* 6.3.9 */
    SXML_BUILTIN_RECORD(
        "compiled-regex-record",
        "record(pattern as xs:string, flags as xs:string, "
        "matches as fn(xs:string) as xs:boolean, "
        "tokenize as fn(xs:string) as xs:string*, "
        "replace as fn(xs:string, (xs:string | fn(xs:untypedAtomic, "
        "map((xs:integer | xs:string), xs:untypedAtomic)) as item()?)?) as xs:string, "
        "analyze-string as fn(xs:string) as element(fn:analyze-string-result), "
        "matching-segments as fn(xs:string) as fn:matching-segment-record*)"),
    /* 9.3 */
    SXML_BUILTIN_RECORD("dateTime-record",
                        "record(year as xs:integer?, month as xs:integer?, "
                        "day as xs:integer?, hours as xs:integer?, "
                        "minutes as xs:integer?, seconds as xs:decimal?, "
                        "timezone as xs:dayTimeDuration?)"),
    /* 4.3.2 */
    SXML_BUILTIN_RECORD("division-record",
                        "record(quotient as xs:decimal, remainder as xs:decimal)"),
    /* 14.6.5.1 */
    SXML_BUILTIN_RECORD("element-conversion-plan-record",
                        "record(layout as enum('empty', 'empty-plus', 'simple', "
                        "'simple-plus', 'list', 'list-plus', 'record', 'sequence', "
                        "'mixed', 'xml', 'error', 'deep-skip'), "
                        "child as xs:string?, "
                        "type as enum('integer', 'decimal', 'double', 'boolean', "
                        "'string')?)"),
    /* 18.1 */
    SXML_BUILTIN_RECORD("load-xquery-module-record",
                        "record(variables as map(xs:QName, item()*), "
                        "functions as map(xs:QName, map(xs:integer, fn(*))))"),
    /* 12.1.8; the specification's "xs:positive-integer" is
       xs:positiveInteger. */
    SXML_BUILTIN_RECORD("location-record",
                        "record(system-id as xs:anyURI?, public-id as xs:string?, "
                        "line-number as xs:positiveInteger?, "
                        "column-number as xs:positiveInteger?)"),
    /* 6.3.6 */
    SXML_BUILTIN_RECORD("matching-segment-record",
                        "record(substring as xs:string, position as xs:integer, "
                        "groups as map((xs:integer | xs:string), "
                        "fn:captured-group-record))"),
    /* 17.5.6 */
    SXML_BUILTIN_RECORD("parsed-csv-structure-record",
                        "record(columns as xs:string*, "
                        "column-index as map(xs:string, xs:integer)?, "
                        "rows as array(xs:string)*, "
                        "get as fn(xs:positiveInteger, (xs:positiveInteger | xs:string)) "
                        "as xs:string)"),
    /* 4.11.1 */
    SXML_BUILTIN_RECORD("random-number-generator-record",
                        "record(number as xs:double, "
                        "next as fn() as fn:random-number-generator-record, "
                        "permute as fn(item()*) as item()*, "
                        "take as fn(xs:nonNegativeInteger) as xs:double*)"),
    /* 19.1.1 */
    SXML_BUILTIN_RECORD(
        "schema-type-record",
        "record(name as xs:QName?, is-simple as xs:boolean, "
        "base-type as fn() as fn:schema-type-record?, "
        "primitive-type as (fn() as fn:schema-type-record)?, "
        "variety as enum('atomic', 'list', 'union', 'empty', 'simple', "
        "'element-only', 'mixed')?, "
        "members as (fn() as fn:schema-type-record*)?, "
        "simple-content-type as (fn() as fn:schema-type-record)?, "
        "matches as (fn(xs:anyAtomicType) as xs:boolean)?, "
        "constructor as (fn(xs:anyAtomicType?) as xs:anyAtomicType*)?)"),
    /* 2.5.19 */
    SXML_BUILTIN_RECORD("sort-key-record",
                        "record(key as (fn(item()) as xs:anyAtomicType*)?, "
                        "collation as xs:string?, "
                        "order as enum('ascending', 'descending')?)"),
    /* 7.6.1 */
    SXML_BUILTIN_RECORD("uri-structure-record",
                        "record(uri as xs:string?, scheme as xs:string?, "
                        "absolute as xs:boolean?, hierarchical as xs:boolean?, "
                        "authority as xs:string?, userinfo as xs:string?, "
                        "host as xs:string?, port as xs:integer?, "
                        "path as xs:string?, query as xs:string?, "
                        "fragment as xs:string?, path-segments as xs:string*, "
                        "query-parameters as map(xs:string, xs:string*)?, "
                        "filepath as xs:string?)"),
    /* 17.2.6 */
    SXML_BUILTIN_RECORD("validation-result-record",
                        "record(is-valid as xs:boolean, typed-node as xnode()?, "
                        "error-details as map(*)*)"),
};

#define SXML_BUILTIN_RECORD_COUNT (sizeof(builtin_records) / sizeof(builtin_records[0]))

static sxml_status intern_record_name(sxml_name_pool *name_pool, sxml_byte_view local_name,
                                      sxml_name_identifier *name) {
    return sxml_name_pool_intern_name(
        name_pool,
        &(sxml_name_request){
            .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_FUNCTIONS),
            .local_name = local_name},
        name);
}

/* Gives a declared type the fields of its row's structural record type. */
static sxml_status define_builtin_record(sxml_function_library *library,
                                         const builtin_record_row *row,
                                         sxml_type_identifier record_type) {
    sxml_sequence_type structural = {.item_type = {.index = SXML_TYPE_NONE},
                                     .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
    sxml_status status =
        sxml_function_library_resolve_type_text(library, row->record_type_text, &structural);
    sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(sxml_function_library_get_item_heap(library));
    sxml_type_description description = {.variety = SXML_TYPE_VARIETY_ANY_ITEM};
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_describe_type(registry, structural.item_type, &description);
    }
    if (status == SXML_STATUS_OK && description.variety != SXML_TYPE_VARIETY_RECORD) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_type_registry_define_record_fields(
        registry, &(sxml_record_definition){
                      .record_type = record_type,
                      .record_fields = description.variety_details.record_type.record_fields});
}

sxml_status sxml_function_library_declare_record_types(sxml_function_library *library) {
    sxml_item_heap *item_heap = sxml_function_library_get_item_heap(library);
    sxml_type_registry *registry = sxml_item_heap_get_type_registry(item_heap);
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(item_heap);
    /* SXML_TYPE_NONE for a type the registry knew already (another library
       on the same heap declared it). */
    sxml_type_identifier declared[SXML_BUILTIN_RECORD_COUNT];
    for (size_t record_index = 0; record_index < SXML_BUILTIN_RECORD_COUNT; record_index += 1) {
        declared[record_index] = (sxml_type_identifier){.index = SXML_TYPE_NONE};
    }
    sxml_status status = SXML_STATUS_OK;
    for (size_t record_index = 0; status == SXML_STATUS_OK && record_index < SXML_BUILTIN_RECORD_COUNT;
         record_index += 1) {
        sxml_name_identifier name = {.index = 0};
        sxml_type_identifier known = {.index = SXML_TYPE_NONE};
        status = intern_record_name(name_pool, builtin_records[record_index].local_name, &name);
        if (status == SXML_STATUS_OK && !sxml_type_registry_find_type(registry, name, &known)) {
            status = sxml_type_registry_declare_nominative_record_type(registry, name,
                                                                       &declared[record_index]);
        }
    }
    for (size_t record_index = 0; status == SXML_STATUS_OK && record_index < SXML_BUILTIN_RECORD_COUNT;
         record_index += 1) {
        if (declared[record_index].index != SXML_TYPE_NONE) {
            status = define_builtin_record(library, &builtin_records[record_index],
                                           declared[record_index]);
        }
    }
    return status;
}

sxml_status sxml_function_library_find_record_type(sxml_item_heap *item_heap,
                                                   sxml_byte_view local_name,
                                                   sxml_type_identifier *record_type) {
    sxml_name_identifier name = {.index = 0};
    if (!sxml_name_pool_find_name(
            sxml_item_heap_get_name_pool(item_heap),
            &(sxml_name_request){
                .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_FUNCTIONS),
                .local_name = local_name},
            &name) ||
        !sxml_type_registry_find_type(sxml_item_heap_get_type_registry(item_heap), name,
                                      record_type)) {
        /* No function library declared the built-in record types on this
           heap: an error in sxml itself. */
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_function_library_make_record(const sxml_function_call *call,
                                              const sxml_builtin_record_request *request,
                                              sxml_sequence *record) {
    sxml_type_identifier record_type = {.index = SXML_TYPE_NONE};
    sxml_status status =
        sxml_function_library_find_record_type(call->item_heap, request->local_name, &record_type);
    sxml_item made = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_record_make(call->item_heap,
                                  &(sxml_record_request){.record_type = record_type,
                                                         .field_values = request->field_values},
                                  &made);
    }
    if (status == SXML_STATUS_OK) {
        *record = sxml_sequence_from_item(made);
    }
    return status;
}
