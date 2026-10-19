/*
 * The EXSLT common functions (packet 7.4b, D206): exsl:node-set and
 * exsl:object-type of the EXSLT Common module (namespace
 * http://exslt.org/common), which XSLT 1.0 stylesheets such as DocBook's
 * call. One library part, `exslt`, that only XSLT compilation names, as
 * `output` and `xslt` (D101, D149).
 *
 * In XSLT 2.0 and later a temporary tree is a document node, so the
 * result tree fragments EXSLT converts are already nodes: exsl:node-set
 * returns its argument. The other EXSLT modules (math, sets, strings,
 * dates, dynamic, functions) are not supplied; their functions are absent
 * (XPST0017, or XTDE1425 with XSLT 1.0 behaviour, XSLT 4.0, 3.8.1) and
 * function-available reports them false, which DocBook tests first.
 *
 * Golden file: src/function_library_output.c (rows of an XSLT-only part).
 */
#include "function_library_core_internal.h"
#include "function_library_internal.h"
#include "function_library_parts_internal.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* exsl:node-set($object) (EXSLT Common, exsl:node-set): a result tree
   fragment converted to a node-set. A temporary tree is a document node
   (XSLT 4.0, 9.10), so every value is returned as it is. */
static sxml_status call_node_set(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence object = sxml_function_library_get_argument(call, 0);
    sxml_sequence_retain(object);
    *result = object;
    return SXML_STATUS_OK;
}

/* The EXSLT object type of one item: "string", "number", "boolean",
   "node-set" for a node, and "external" for any other item. */
static sxml_byte_view object_type_of_item(sxml_item item) {
    const sxml_item_kind kind = sxml_item_get_kind(item);
    if (kind == SXML_ITEM_KIND_NODE) {
        return SXML_BYTE_VIEW_FROM_LITERAL("node-set");
    }
    if (kind != SXML_ITEM_KIND_ATOMIC) {
        return SXML_BYTE_VIEW_FROM_LITERAL("external");
    }
    switch (sxml_atomic_item_get_primitive_type(item).index) {
    case SXML_TYPE_XS_STRING:
    case SXML_TYPE_XS_UNTYPED_ATOMIC:
    case SXML_TYPE_XS_ANY_URI:
        return SXML_BYTE_VIEW_FROM_LITERAL("string");
    case SXML_TYPE_XS_DECIMAL:
    case SXML_TYPE_XS_FLOAT:
    case SXML_TYPE_XS_DOUBLE:
        return SXML_BYTE_VIEW_FROM_LITERAL("number");
    case SXML_TYPE_XS_BOOLEAN:
        return SXML_BYTE_VIEW_FROM_LITERAL("boolean");
    default:
        return SXML_BYTE_VIEW_FROM_LITERAL("external");
    }
}

/* exsl:object-type($object) (EXSLT Common, exsl:object-type): the XPath
   1.0 type of the value. A single atomic item has the type of its
   primitive type; a sequence of nodes, the empty sequence included, is a
   "node-set"; any other value is "external". */
static sxml_status call_object_type(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    const sxml_sequence object = sxml_function_library_get_argument(call, 0);
    sxml_byte_view type_name = SXML_BYTE_VIEW_FROM_LITERAL("node-set");
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(object);
    sxml_status status = SXML_STATUS_OK;
    for (size_t item_count = 0; status == SXML_STATUS_OK; item_count += 1) {
        sxml_iteration_step next = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &next);
        if (status != SXML_STATUS_OK || !next.has_item) {
            break;
        }
        const sxml_byte_view item_type = object_type_of_item(next.item);
        const bool is_node = sxml_item_get_kind(next.item) == SXML_ITEM_KIND_NODE;
        if (item_count == 0) {
            type_name = item_type;
        } else if (!is_node || !sxml_byte_view_equals(type_name, item_type)) {
            type_name = SXML_BYTE_VIEW_FROM_LITERAL("external");
        }
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = {.utf8 = type_name,
                                                       .codepoint_count = type_name.byte_count,
                                                       .is_ascii = true}}}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* ------------------------------------------------------------------ Rows */

/* A row in the EXSLT common namespace. */
#define SXML_EXSLT_COMMON_ROW(name_text, result_text, body)                                \
    {.function_namespace = SXML_NAMESPACE_EXSLT_COMMON,                                    \
     .properties = 0,                                                                      \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                                  \
     .parameters = {object_parameters, sizeof(object_parameters) / sizeof(object_parameters[0])}, \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                               \
     .call = (body),                                                                       \
     .resumable_body = NULL}

static const sxml_function_parameter object_parameters[] = {
    SXML_REQUIRED_PARAMETER("object", "item()*"),
};

static const sxml_native_function exslt_functions[] = {
    SXML_EXSLT_COMMON_ROW("node-set", "item()*", call_node_set),
    SXML_EXSLT_COMMON_ROW("object-type", "xs:string", call_object_type),
};

const sxml_native_function_view sxml_function_library_exslt = SXML_LIBRARY_PART(exslt_functions);

const sxml_language_set sxml_function_library_exslt_languages = SXML_LANGUAGE_XSLT;
