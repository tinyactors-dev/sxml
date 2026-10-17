/*
 * The XSLT 4.0 functions written in C (src/xslt_key_internal.h; XSLT 4.0,
 * sections 20 and 24; docs/DECISIONS.md D101, D139, D149; packet 4.10).
 *
 * One library part, `xslt`, that only XSLT compilation names
 * (sxml_function_library_xslt_languages, D101): the functions of static
 * expressions (system-property, available-system-properties,
 * function-available, element-available, type-available; moved here from
 * src/xslt_static.c, which serves its expressions the first rows,
 * sxml_xslt_static_functions), current, document, unparsed-entity-uri and
 * unparsed-entity-public-id. Names given as strings are resolved against
 * the namespaces of the unit running (D84, D139).
 *
 * The rows behind fn:key (sxml_xslt_key_functions, in no namespace and in
 * no part): key-name, key-index (the index of one key for one tree, built
 * once per evaluation and kept in the evaluation memo) and key-normalize
 * (key values as map keys). src/xslt_key.c lowers fn:key from XPath texts
 * that call them.
 *
 * Golden file: src/function_library_format.c (rows of one part, bodies
 * reading their arguments through function_library_core_internal.h);
 * src/function_library_higher_order.c for the resumable body.
 */
#include "xslt_key_internal.h"

#include "cast_internal.h"
#include "collation_internal.h"
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "function_library_internal.h"
#include "function_library_parts_internal.h"
#include "ir_internal.h"
#include "xdm_internal.h"
#include "xslt_static_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------ Names given as strings */

/* What a name argument of the XSLT functions resolves to. */
typedef enum name_argument_role {
    /* Unprefixed: no namespace (system-property). */
    SXML_NAME_ARGUMENT_ROLE_NO_NAMESPACE,
    /* Unprefixed: the default function namespace (function-available). */
    SXML_NAME_ARGUMENT_ROLE_FUNCTION,
    /* Unprefixed: the default namespace for elements and types
       (element-available, type-available). */
    SXML_NAME_ARGUMENT_ROLE_ELEMENT_OR_TYPE,
} name_argument_role;

typedef struct name_argument_request {
    const sxml_function_call *call;
    name_argument_role role;
    /* Raised for a string that is no EQName or has an unbound prefix. */
    const sxml_static_error *error;
} name_argument_request;

static bool is_space_byte(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

static sxml_byte_view trim_spaces(sxml_byte_view text) {
    while (text.byte_count > 0 && is_space_byte(text.bytes[0])) {
        text = (sxml_byte_view){.bytes = text.bytes + 1, .byte_count = text.byte_count - 1};
    }
    while (text.byte_count > 0 && is_space_byte(text.bytes[text.byte_count - 1])) {
        text.byte_count -= 1;
    }
    return text;
}

/* The namespace a prefix has in the running unit: its static namespaces,
   then the predeclared ones (XPath 4.0, 2.2.1); prefix empty is the
   unit's default element namespace. False when it is unbound. */
typedef struct prefix_request {
    const sxml_function_call *call;
    sxml_byte_view namespace_prefix;
} prefix_request;

/* The namespaces names given as strings are resolved against: the static
   namespaces of the unit running (the expression's for a static
   expression, D139; the template's of the invocation for a stylesheet,
   D142), as casts to xs:QName at run time (D84). */
static sxml_namespace_binding_view evaluation_bindings_of(const sxml_function_call *call) {
    const sxml_ir_unit *unit = call->dynamic_context == NULL
                                   ? NULL
                                   : sxml_dynamic_context_get_running_unit(call->dynamic_context);
    if (unit == NULL) {
        return (sxml_namespace_binding_view){.namespace_bindings = NULL,
                                             .namespace_binding_count = 0};
    }
    return sxml_ir_unit_get_static_namespaces(unit);
}

static bool resolve_prefix(prefix_request request, sxml_namespace_uri_identifier *namespace_uri) {
    static const struct {
        sxml_byte_view prefix;
        sxml_well_known_namespace namespace_uri;
    } predeclared[] = {
        {SXML_BYTE_VIEW_INITIALIZER("xml"), SXML_NAMESPACE_XML},
        {SXML_BYTE_VIEW_INITIALIZER("xs"), SXML_NAMESPACE_XML_SCHEMA},
        {SXML_BYTE_VIEW_INITIALIZER("xsi"), SXML_NAMESPACE_XML_SCHEMA_INSTANCE},
        {SXML_BYTE_VIEW_INITIALIZER("fn"), SXML_NAMESPACE_FUNCTIONS},
        {SXML_BYTE_VIEW_INITIALIZER("map"), SXML_NAMESPACE_MAP_FUNCTIONS},
        {SXML_BYTE_VIEW_INITIALIZER("array"), SXML_NAMESPACE_ARRAY_FUNCTIONS},
        {SXML_BYTE_VIEW_INITIALIZER("math"), SXML_NAMESPACE_MATH_FUNCTIONS},
        {SXML_BYTE_VIEW_INITIALIZER("err"), SXML_NAMESPACE_ERRORS},
    };
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(request.call->item_heap);
    const sxml_namespace_binding_view bindings = evaluation_bindings_of(request.call);
    for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding *binding = &bindings.namespace_bindings[binding_index];
        sxml_byte_view bound_prefix = {.bytes = NULL, .byte_count = 0};
        if (binding->namespace_prefix.index != 0 &&
            sxml_name_pool_get_namespace_prefix(name_pool, binding->namespace_prefix,
                                                &bound_prefix) != SXML_STATUS_OK) {
            continue;
        }
        if (sxml_byte_view_equals(bound_prefix, request.namespace_prefix)) {
            *namespace_uri = binding->namespace_uri;
            return true;
        }
    }
    if (request.namespace_prefix.byte_count == 0) {
        *namespace_uri = (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_NONE};
        return true;
    }
    for (size_t prefix_index = 0; prefix_index < sizeof(predeclared) / sizeof(predeclared[0]);
         prefix_index += 1) {
        if (sxml_byte_view_equals(predeclared[prefix_index].prefix, request.namespace_prefix)) {
            *namespace_uri =
                (sxml_namespace_uri_identifier){.index = predeclared[prefix_index].namespace_uri};
            return true;
        }
    }
    return false;
}

/* The expanded name a name argument gives: an xs:QName as it is, or a
   string holding an EQName (XSLT 4.0, 24.1, 24.3.2, 20.4, 24.2.1). */
static sxml_status read_name_argument(name_argument_request request, sxml_name_identifier *name) {
    const sxml_function_call *call = request.call;
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_optional_item argument = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    if (!argument.has_item || sxml_atomic_item_get_datum(argument.item, &datum) != SXML_STATUS_OK) {
        return sxml_function_library_report(call, request.error);
    }
    if (datum.datum_kind == SXML_DATUM_KIND_QNAME) {
        *name = datum.datum_details.qname.expanded_name;
        return SXML_STATUS_OK;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return sxml_function_library_report(call, request.error);
    }
    const sxml_byte_view text = trim_spaces(datum.datum_details.string.utf8);
    sxml_byte_view local_name = text;
    sxml_namespace_uri_identifier namespace_uri = {.index = SXML_NAMESPACE_NONE};
    if (text.byte_count > 2 && text.bytes[0] == 'Q' && text.bytes[1] == '{') {
        size_t close_index = 2;
        while (close_index < text.byte_count && text.bytes[close_index] != '}') {
            close_index += 1;
        }
        if (close_index >= text.byte_count) {
            return sxml_function_library_report(call, request.error);
        }
        status = sxml_name_pool_intern_namespace_uri(
            name_pool, (sxml_byte_view){.bytes = text.bytes + 2, .byte_count = close_index - 2},
            &namespace_uri);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        local_name = (sxml_byte_view){.bytes = text.bytes + close_index + 1,
                                      .byte_count = text.byte_count - close_index - 1};
    } else {
        size_t colon_index = 0;
        while (colon_index < text.byte_count && text.bytes[colon_index] != ':') {
            colon_index += 1;
        }
        const bool has_prefix = colon_index < text.byte_count;
        if (has_prefix) {
            local_name = (sxml_byte_view){.bytes = text.bytes + colon_index + 1,
                                          .byte_count = text.byte_count - colon_index - 1};
        }
        if (has_prefix || request.role == SXML_NAME_ARGUMENT_ROLE_ELEMENT_OR_TYPE) {
            const sxml_byte_view prefix = {.bytes = text.bytes,
                                           .byte_count = has_prefix ? colon_index : 0};
            if (!resolve_prefix((prefix_request){.call = call, .namespace_prefix = prefix},
                                &namespace_uri)) {
                return sxml_function_library_report(call, request.error);
            }
        } else if (request.role == SXML_NAME_ARGUMENT_ROLE_FUNCTION) {
            namespace_uri = (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_FUNCTIONS};
        }
    }
    if (local_name.byte_count == 0) {
        return sxml_function_library_report(call, request.error);
    }
    for (size_t byte_index = 0; byte_index < local_name.byte_count; byte_index += 1) {
        const uint8_t byte = local_name.bytes[byte_index];
        if (byte == ':' || byte == '{' || byte == '}' || is_space_byte(byte)) {
            return sxml_function_library_report(call, request.error);
        }
    }
    return sxml_name_pool_intern_name(
        name_pool, &(sxml_name_request){.namespace_uri = namespace_uri, .local_name = local_name},
        name);
}

static const sxml_static_error system_property_name_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE1390"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The argument of system-property is not a valid EQName (XSLT 4.0, 24.2.1)")};
static const sxml_static_error function_name_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE1400"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The argument of function-available is not a valid EQName (XSLT 4.0, 24.1)")};
static const sxml_static_error element_name_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE1440"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The argument of element-available is not a valid EQName (XSLT 4.0, 24.3.2)")};
static const sxml_static_error type_name_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE1428"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The argument of type-available is not a valid EQName (XSLT 4.0, 20.4)")};

/* The system properties of the XSLT namespace (XSLT 4.0, 24.2.1). */
typedef struct system_property {
    sxml_byte_view local_name;
    sxml_byte_view value;
} system_property;

static const system_property system_properties[] = {
    {SXML_BYTE_VIEW_INITIALIZER("version"), SXML_BYTE_VIEW_INITIALIZER("4.0")},
    {SXML_BYTE_VIEW_INITIALIZER("vendor"), SXML_BYTE_VIEW_INITIALIZER("sxml")},
    {SXML_BYTE_VIEW_INITIALIZER("vendor-url"), SXML_BYTE_VIEW_INITIALIZER("")},
    {SXML_BYTE_VIEW_INITIALIZER("product-name"), SXML_BYTE_VIEW_INITIALIZER("sxml")},
    {SXML_BYTE_VIEW_INITIALIZER("product-version"), SXML_BYTE_VIEW_INITIALIZER("0.2.0")},
    {SXML_BYTE_VIEW_INITIALIZER("is-schema-aware"), SXML_BYTE_VIEW_INITIALIZER("no")},
    {SXML_BYTE_VIEW_INITIALIZER("supports-serialization"), SXML_BYTE_VIEW_INITIALIZER("yes")},
    {SXML_BYTE_VIEW_INITIALIZER("supports-backwards-compatibility"),
     SXML_BYTE_VIEW_INITIALIZER("no")},
    {SXML_BYTE_VIEW_INITIALIZER("supports-namespace-axis"), SXML_BYTE_VIEW_INITIALIZER("yes")},
    {SXML_BYTE_VIEW_INITIALIZER("supports-streaming"), SXML_BYTE_VIEW_INITIALIZER("no")},
    {SXML_BYTE_VIEW_INITIALIZER("supports-dynamic-evaluation"), SXML_BYTE_VIEW_INITIALIZER("no")},
    {SXML_BYTE_VIEW_INITIALIZER("supports-higher-order-functions"),
     SXML_BYTE_VIEW_INITIALIZER("yes")},
    {SXML_BYTE_VIEW_INITIALIZER("xpath-version"), SXML_BYTE_VIEW_INITIALIZER("4.0")},
    {SXML_BYTE_VIEW_INITIALIZER("xsd-version"), SXML_BYTE_VIEW_INITIALIZER("1.1")},
};

#define STATIC_SYSTEM_PROPERTY_COUNT (sizeof(system_properties) / sizeof(system_properties[0]))

/* fn:system-property($name) (XSLT 4.0, 24.2.1): the zero-length string
   for a property sxml does not have. */
static sxml_status call_system_property(const sxml_function_call *call, sxml_sequence *result) {
    sxml_name_identifier name = {.index = 0};
    sxml_status status =
        read_name_argument((name_argument_request){.call = call,
                                                   .role = SXML_NAME_ARGUMENT_ROLE_NO_NAMESPACE,
                                                   .error = &system_property_name_error},
                           &name);
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(sxml_item_heap_get_name_pool(call->item_heap), name,
                                               &parts);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
    if (parts.namespace_uri.index == SXML_NAMESPACE_XSLT) {
        for (size_t property_index = 0;
             property_index < sizeof(system_properties) / sizeof(system_properties[0]);
             property_index += 1) {
            if (sxml_byte_view_equals(system_properties[property_index].local_name,
                                      parts.local_name)) {
                value = system_properties[property_index].value;
            }
        }
    }
    return sxml_function_library_make_string(call->item_heap, value, result);
}

/* fn:available-system-properties() (XSLT 4.0, 24.2.2). */
static sxml_status call_available_system_properties(const sxml_function_call *call,
                                                    sxml_sequence *result) {
    sxml_item items[STATIC_SYSTEM_PROPERTY_COUNT];
    size_t made_count = 0;
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    sxml_status status = sxml_name_pool_intern_namespace_prefix(
        name_pool, SXML_BYTE_VIEW_FROM_LITERAL("xsl"), &prefix);
    while (status == SXML_STATUS_OK && made_count < STATIC_SYSTEM_PROPERTY_COUNT) {
        sxml_name_identifier name = {.index = 0};
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_XSLT},
                                 .local_name = system_properties[made_count].local_name},
            &name);
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof(datum));
        datum.datum_kind = SXML_DATUM_KIND_QNAME;
        datum.datum_details.qname =
            (sxml_qname_datum){.expanded_name = name, .namespace_prefix = prefix};
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_make(
                call->item_heap,
                &(sxml_atomic_item_request){.type = {.index = SXML_TYPE_XS_QNAME}, .datum = datum},
                &items[made_count]);
        }
        if (status == SXML_STATUS_OK) {
            made_count += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            call->item_heap, (sxml_item_view){.items = items, .item_count = made_count}, result);
    }
    for (size_t item_index = 0; item_index < made_count; item_index += 1) {
        sxml_item_release(items[item_index]);
    }
    return status;
}

/* The largest arity function-available tries for a call without $arity:
   no function of the library takes more parameters except variadic ones,
   which have every arity from their required count. */
#define STATIC_FUNCTION_MAXIMUM_ARITY 32U

/* fn:function-available($name, $arity?) (XSLT 4.0, 24.1): through the
   dynamic context's function table, the one static analysis used, so a
   function is available exactly when a static call could bind it. */
static sxml_status call_function_available(const sxml_function_call *call, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_name_identifier name = {.index = 0};
    sxml_status status = read_name_argument(
        (name_argument_request){
            .call = call, .role = SXML_NAME_ARGUMENT_ROLE_FUNCTION, .error = &function_name_error},
        &name);
    sxml_optional_item arity_argument = {.has_item = false};
    if (status == SXML_STATUS_OK && call->arguments.sequence_count > 1) {
        status = sxml_function_library_read_optional_item(call, 1, &arity_argument);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t first_arity = 0;
    size_t last_arity = STATIC_FUNCTION_MAXIMUM_ARITY;
    if (arity_argument.has_item) {
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof(datum));
        status = sxml_atomic_item_get_datum(arity_argument.item, &datum);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (datum.datum_kind != SXML_DATUM_KIND_INTEGER || datum.datum_details.integer < 0 ||
            datum.datum_details.integer > (int64_t)STATIC_FUNCTION_MAXIMUM_ARITY * 1024) {
            *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = false});
            return SXML_STATUS_OK;
        }
        first_arity = (size_t)datum.datum_details.integer;
        last_arity = first_arity;
    }
    const sxml_function_table table = call->dynamic_context->function_table;
    bool is_available = false;
    for (size_t arity = first_arity;
         arity <= last_arity && !is_available && table.operations != NULL; arity += 1) {
        sxml_function_entry entry;
        memset(&entry, 0, sizeof(entry));
        entry.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE;
        status = sxml_function_table_find(
            table, &(sxml_function_query){.function_name = name, .arity = arity}, &entry);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        is_available = entry.entry_kind != SXML_FUNCTION_ENTRY_KIND_NONE;
    }
    *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = is_available});
    return SXML_STATUS_OK;
}

/* fn:element-available($name) (XSLT 4.0, 24.3.2): XSLT elements sxml
   implements; sxml knows no extension instructions. */
static sxml_status call_element_available(const sxml_function_call *call, sxml_sequence *result) {
    sxml_name_identifier name = {.index = 0};
    sxml_status status =
        read_name_argument((name_argument_request){.call = call,
                                                   .role = SXML_NAME_ARGUMENT_ROLE_ELEMENT_OR_TYPE,
                                                   .error = &element_name_error},
                           &name);
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(sxml_item_heap_get_name_pool(call->item_heap), name,
                                               &parts);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_available =
        parts.namespace_uri.index == SXML_NAMESPACE_XSLT &&
        sxml_xslt_is_element_available((sxml_xslt_element_query){.local_name = parts.local_name});
    *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = is_available});
    return SXML_STATUS_OK;
}

/* fn:type-available($name) (XSLT 4.0, 20.4): the built-in types (no
   imported schemas, D97). */
static sxml_status call_type_available(const sxml_function_call *call, sxml_sequence *result) {
    sxml_name_identifier name = {.index = 0};
    const sxml_status status =
        read_name_argument((name_argument_request){.call = call,
                                                   .role = SXML_NAME_ARGUMENT_ROLE_ELEMENT_OR_TYPE,
                                                   .error = &type_name_error},
                           &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_type_identifier type = {.index = 0};
    const bool is_available = sxml_type_registry_find_type(
        sxml_item_heap_get_type_registry(call->item_heap), name, &type);
    *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = is_available});
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------- current, document */

/* fn:current() (XSLT 4.0, 20.3.1): the current item, which lowering
   passes as the argument (SXML_PARAMETER_DEFAULT_CURRENT_ITEM: the focus
   the outermost expression started with; in a pattern, the item being
   matched, D118). */
static const sxml_static_error current_absent_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE1360"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "fn:current is called dynamically, where the current item is absent (XSLT 4.0, "
        "20.3.1)")};

static sxml_status call_current(const sxml_function_call *call, sxml_sequence *result) {
    if (call->arguments.sequence_count == 0) {
        /* current#0 called as a function item: no expression supplies
           the current item. */
        return sxml_function_library_report(call, &current_absent_error);
    }
    const sxml_sequence item = sxml_function_library_get_argument(call, 0);
    sxml_sequence_retain(item);
    *result = item;
    return SXML_STATUS_OK;
}

static const sxml_static_error document_base_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE1162"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "fn:document is given a relative URI and no base URI to resolve it against "
        "(XSLT 4.0, 20.1)")};

/* The URI reference an item of $uri-sequence gives (its string value)
   and the base URI to resolve it against: the base node's, else the
   node's own, else the static base URI (XSLT 4.0, 20.1). */
typedef struct document_reference {
    /* Owned: the string value and the base URI item, released by
       release_document_reference. */
    sxml_sequence text;
    sxml_sequence base;
} document_reference;

static void release_document_reference(document_reference *reference) {
    sxml_sequence_release(reference->text);
    sxml_sequence_release(reference->base);
    *reference = (document_reference){.text = sxml_sequence_empty(), .base = sxml_sequence_empty()};
}

static sxml_status make_base_uri_of(const sxml_function_call *call, sxml_item node_item,
                                    sxml_sequence *base) {
    sxml_node_reference node;
    const sxml_status status = sxml_item_get_node(node_item, &node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_node_make_base_uri(
        call->item_heap,
        &(sxml_node_value_request){.node = node, .error_record = call->error_record}, base);
}

static sxml_byte_view text_of(sxml_sequence value) {
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
        sxml_atomic_item_get_datum(value.content, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return datum.datum_details.string.utf8;
}

static sxml_status read_document_reference(const sxml_function_call *call, sxml_item item,
                                           document_reference *reference) {
    sxml_status status = SXML_STATUS_OK;
    if (sxml_item_get_kind(item) == SXML_ITEM_KIND_NODE) {
        sxml_node_reference node;
        sxml_item text = {.representation = 0};
        status = sxml_item_get_node(item, &node);
        if (status == SXML_STATUS_OK) {
            status = sxml_node_make_string_value(call->item_heap, node, &text);
        }
        if (status == SXML_STATUS_OK) {
            reference->text = sxml_sequence_from_item(text);
        }
    } else {
        sxml_library_text text = {
            .item_heap = call->item_heap, .bytes = NULL, .byte_count = 0, .byte_capacity = 0};
        status = sxml_function_library_text_append_atomic(&text, item);
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_make_string(
                call->item_heap, sxml_function_library_text_view(&text), &reference->text);
        }
        sxml_function_library_text_release(&text);
    }
    sxml_optional_item base_node = {.has_item = false};
    if (status == SXML_STATUS_OK && call->arguments.sequence_count > 1) {
        status = sxml_function_library_read_optional_item(call, 1, &base_node);
    }
    if (status == SXML_STATUS_OK && base_node.has_item &&
        sxml_item_get_kind(base_node.item) == SXML_ITEM_KIND_MAP) {
        /* 4.0: an options map; its base-uri option is the base URI. */
        sxml_sequence key = sxml_sequence_empty();
        status = sxml_function_library_make_string(
            call->item_heap, SXML_BYTE_VIEW_FROM_LITERAL("base-uri"), &key);
        sxml_map_lookup lookup = {.is_found = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_map_find(base_node.item, key.content, &lookup);
        }
        sxml_sequence_release(key);
        if (status == SXML_STATUS_OK && lookup.is_found) {
            sxml_sequence_retain(lookup.entry.value);
            reference->base = lookup.entry.value;
            return SXML_STATUS_OK;
        }
        base_node.has_item = false;
    }
    if (status == SXML_STATUS_OK && base_node.has_item) {
        status = make_base_uri_of(call, base_node.item, &reference->base);
    } else if (status == SXML_STATUS_OK && sxml_item_get_kind(item) == SXML_ITEM_KIND_NODE) {
        status = make_base_uri_of(call, item, &reference->base);
    }
    return status;
}

/* fn:document($uri-sequence, $options?) (XSLT 4.0, 20.1; $options a node
   whose base URI, or a map whose base-uri option, relative references are
   resolved against): the documents
   the URI references give, through the document loader as fn:doc (the
   same node for the same URI within an evaluation), in document order
   without duplicates. A fragment identifier is ignored (20.1 leaves its
   meaning to the media type; sxml supports none): the whole document is
   returned. */
static sxml_status call_document(const sxml_function_call *call, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_sequence_builder *builder = NULL;
    sxml_status status = sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    for (;;) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        document_reference reference = {.text = sxml_sequence_empty(),
                                        .base = sxml_sequence_empty()};
        status = read_document_reference(call, step.item, &reference);
        sxml_byte_view uri_reference = text_of(reference.text);
        for (size_t byte_index = 0; byte_index < uri_reference.byte_count; byte_index += 1) {
            if (uri_reference.bytes[byte_index] == '#') {
                uri_reference.byte_count = byte_index;
            }
        }
        const sxml_byte_view base_uri =
            sxml_sequence_get_representation(reference.base) == SXML_SEQUENCE_REPRESENTATION_EMPTY
                ? sxml_dynamic_context_get_static_base_uri(call->dynamic_context)
                : text_of(reference.base);
        if (status == SXML_STATUS_OK && uri_reference.byte_count == 0 && base_uri.byte_count == 0) {
            status = sxml_function_library_report(call, &document_base_error);
        }
        sxml_item document_node = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_dynamic_context_load_document(
                call->dynamic_context,
                &(sxml_document_request){.uri_reference = uri_reference,
                                         .base_uri = base_uri,
                                         .error_record = call->error_record},
                &document_node);
        }
        release_document_reference(&reference);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, document_node);
            sxml_item_release(document_node);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    sxml_sequence documents = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, &documents);
    }
    sxml_sequence_builder_destroy(builder);
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_make_document_order_value(
            call->dynamic_context,
            &(sxml_document_order_request){.input = documents, .error_record = call->error_record},
            result);
    }
    sxml_sequence_release(documents);
    return status;
}

static const sxml_static_error unparsed_entity_document_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE1370"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The node given to unparsed-entity-uri or unparsed-entity-public-id is not in a tree "
        "whose root is a document node (XSLT 4.0, 20.2.3, 20.2.4)")};

/* Raises XTDE1370 unless the node of argument 1 is in a document. */
static sxml_status check_unparsed_entity_document(const sxml_function_call *call) {
    sxml_optional_item node_item = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 1, &node_item);
    sxml_node_reference node;
    if (status == SXML_STATUS_OK && node_item.has_item) {
        status = sxml_item_get_node(node_item.item, &node);
    }
    if (status != SXML_STATUS_OK || !node_item.has_item) {
        return status == SXML_STATUS_OK
                   ? sxml_function_library_report(call, &unparsed_entity_document_error)
                   : status;
    }
    sxml_node_description description;
    status = sxml_node_describe(
        (sxml_node_reference){.tree = node.tree, .node_index = {.index = 0}, .namespace_number = 0},
        &description);
    if (status != SXML_STATUS_OK || description.node_kind != SXML_NODE_KIND_DOCUMENT) {
        return sxml_function_library_report(call, &unparsed_entity_document_error);
    }
    return SXML_STATUS_OK;
}

/* fn:unparsed-entity-uri($entity-name, $doc := .) (XSLT 4.0, 20.2.3):
   sxml's trees keep no unparsed entities of the DTD, so every name gives
   the zero-length URI, as for an entity the DTD does not declare. */
static sxml_status call_unparsed_entity_uri(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_status status = check_unparsed_entity_document(call);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item item = {.representation = 0};
    const sxml_status make_status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_URI),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = {.bytes = NULL, .byte_count = 0},
                                                   .codepoint_count = 0,
                                                   .is_ascii = true}}}},
        &item);
    if (make_status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return make_status;
}

/* fn:unparsed-entity-public-id($entity-name, $doc := .) (XSLT 4.0,
   20.2.4): the zero-length string, as above. */
static sxml_status call_unparsed_entity_public_id(const sxml_function_call *call,
                                                  sxml_sequence *result) {
    const sxml_status status = check_unparsed_entity_document(call);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_function_library_make_string(
        call->item_heap, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, result);
}

static const sxml_function_parameter current_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("current-item"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("item()"),
     .parameter_default = SXML_PARAMETER_DEFAULT_CURRENT_ITEM},
};

static const sxml_function_parameter document_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("uri-sequence"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("options"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("(node() | map(*))?"),
     .parameter_default = SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY},
};

static const sxml_function_parameter unparsed_entity_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("entity-name"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:string"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("doc"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("node()"),
     .parameter_default = SXML_PARAMETER_DEFAULT_CONTEXT_VALUE},
};

static const sxml_function_parameter name_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("name"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:anyAtomicType"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
};

static const sxml_function_parameter function_available_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("name"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:anyAtomicType"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("arity"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:integer?"),
     .parameter_default = SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY},
};

/* The XSLT functions. The first five are those XSLT 4.0, 9.7 adds to
   static expressions (sxml_xslt_static_functions); they read the running
   unit's namespaces and the function table: context-dependent. */
static const sxml_native_function xslt_functions[] = {
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("system-property"),
     .parameters = {name_parameters, 1},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:string"),
     .call = call_system_property,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("available-system-properties"),
     .parameters = {NULL, 0},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:QName*"),
     .call = call_available_system_properties,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("function-available"),
     .parameters = {function_available_parameters, 2},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean"),
     .call = call_function_available,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("element-available"),
     .parameters = {name_parameters, 1},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean"),
     .call = call_element_available,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("type-available"),
     .parameters = {name_parameters, 1},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean"),
     .call = call_type_available,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = 0,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("current"),
     .parameters = {current_parameters, 1},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("item()"),
     .call = call_current,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("document"),
     .parameters = {document_parameters, 2},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("node()*"),
     .call = call_document,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = 0,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("unparsed-entity-uri"),
     .parameters = {unparsed_entity_parameters, 2},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:anyURI"),
     .call = call_unparsed_entity_uri,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = 0,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("unparsed-entity-public-id"),
     .parameters = {unparsed_entity_parameters, 2},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:string"),
     .call = call_unparsed_entity_public_id,
     .resumable_body = NULL},
};

/* The rows of static expressions come first. */
#define XSLT_STATIC_FUNCTION_COUNT 5U

const sxml_native_function_view sxml_function_library_xslt = SXML_LIBRARY_PART(xslt_functions);

const sxml_language_set sxml_function_library_xslt_languages = SXML_LANGUAGE_XSLT;

const sxml_native_function_view sxml_xslt_static_functions = {
    .native_functions = xslt_functions, .native_function_count = XSLT_STATIC_FUNCTION_COUNT};

/* ------------------------------------------------- Rows behind fn:key */

static const sxml_static_error key_name_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE1260"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The key name given to fn:key is not a valid EQName (XSLT 4.0, 20.2.2)")};

/* key-name($key-name): the expanded name an xs:QName or a string holding
   an EQName gives (XSLT 4.0, 20.2.2: unprefixed, no namespace). */
static sxml_status call_key_name(const sxml_function_call *call, sxml_sequence *result) {
    sxml_name_identifier name = {.index = 0};
    sxml_status status = read_name_argument(
        (name_argument_request){
            .call = call, .role = SXML_NAME_ARGUMENT_ROLE_NO_NAMESPACE, .error = &key_name_error},
        &name);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof(datum));
        datum.datum_kind = SXML_DATUM_KIND_QNAME;
        datum.datum_details.qname =
            (sxml_qname_datum){.expanded_name = name, .namespace_prefix = {.index = 0}};
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){.type = {.index = SXML_TYPE_XS_QNAME}, .datum = datum},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* key-index($number, $root, $build): the index of key $number for the tree
   of $root, built by calling $build($root) the first time an evaluation
   asks for it and kept in the evaluation memo (D149) for the rest of it
   (XSLT 4.0, 20.2.2). */
typedef struct key_index_state {
    /* Owned while the build call runs. */
    sxml_sequence call_argument;
} key_index_state;

static sxml_evaluation_memo_key key_index_memo_key(const sxml_function_call *call) {
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    const sxml_status status =
        sxml_atomic_item_get_datum(sxml_function_library_get_argument(call, 0).content, &datum);
    const uint64_t number = status == SXML_STATUS_OK &&
                                    datum.datum_kind == SXML_DATUM_KIND_INTEGER &&
                                    datum.datum_details.integer >= 0
                                ? (uint64_t)datum.datum_details.integer
                                : 0;
    return (sxml_evaluation_memo_key){.computation = &sxml_xslt_key_functions,
                                      .computation_number = number,
                                      .subject =
                                          sxml_function_library_get_argument(call, 1).content};
}

static sxml_status step_key_index(const sxml_native_step_request *request, sxml_native_step *step) {
    key_index_state *state = request->state;
    const sxml_function_call *call = request->call;
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_evaluation_memo_key key = key_index_memo_key(call);
    if (request->step_number == 0) {
        sxml_sequence index = sxml_sequence_empty();
        if (sxml_dynamic_context_find_memo(call->dynamic_context, &key, &index)) {
            sxml_sequence_retain(index);
            *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH, .result = index};
            return SXML_STATUS_OK;
        }
        state->call_argument = sxml_function_library_get_argument(call, 1);
        sxml_sequence_retain(state->call_argument);
        *step = (sxml_native_step){
            .step_kind = SXML_NATIVE_STEP_KIND_CALL,
            .result = sxml_sequence_empty(),
            .function_item = sxml_function_library_get_argument(call, 2).content,
            .arguments = {.sequences = &state->call_argument, .sequence_count = 1}};
        return SXML_STATUS_OK;
    }
    const sxml_sequence index = request->call_result;
    const sxml_status status = sxml_dynamic_context_store_memo(
        call->dynamic_context, &(sxml_evaluation_memo_entry){.key = key, .value = index});
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(index);
        return status;
    }
    *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH, .result = index};
    return SXML_STATUS_OK;
}

static void release_key_index_state(void *state) {
    key_index_state *index_state = state;
    sxml_sequence_release(index_state->call_argument);
    index_state->call_argument = sxml_sequence_empty();
}

static const sxml_resumable_native_body key_index_body = {
    .step = step_key_index,
    .release_state = release_key_index_state,
    .state_byte_count = sizeof(key_index_state),
    .state_alignment = alignof(key_index_state),
};

/* The separator of the parts of a composite key value: U+E000, which no
   tag uses. */
#define KEY_PART_SEPARATOR "\xEE\x80\x80"

static bool is_string_like(sxml_type_identifier type, const sxml_type_registry *registry) {
    static const sxml_builtin_type string_types[] = {SXML_TYPE_XS_STRING, SXML_TYPE_XS_ANY_URI,
                                                     SXML_TYPE_XS_UNTYPED_ATOMIC};
    for (size_t type_index = 0; type_index < sizeof(string_types) / sizeof(string_types[0]);
         type_index += 1) {
        if (sxml_type_registry_is_derived_from(
                registry, (sxml_type_pair){.derived_type = type,
                                           .base_type = sxml_builtin_type_identifier(
                                               string_types[type_index])})) {
            return true;
        }
    }
    return false;
}

/* How key-normalize turns key values into map keys. */
typedef struct key_normalization {
    const sxml_function_call *call;
    /* NULL for the codepoint collation. */
    const sxml_collation *collation;
} key_normalization;

/* One key value as a map key (XSLT 4.0, 20.2.2: compared with eq after
   xs:untypedAtomic becomes xs:string, under the key's collation): a
   string-like value as xs:string, or as its collation key under another
   collation than the codepoint one; other values as they are, since map
   keys are compared as eq compares them (op:same-key). */
static sxml_status normalize_key_value(key_normalization normalization, sxml_item value,
                                       sxml_item *key) {
    const sxml_function_call *call = normalization.call;
    const sxml_type_registry *registry = sxml_item_heap_get_type_registry(call->item_heap);
    const sxml_type_identifier type = sxml_item_get_type_annotation(value);
    if (sxml_type_registry_is_derived_from(
            registry,
            (sxml_type_pair){.derived_type = type,
                             .base_type = sxml_builtin_type_identifier(SXML_TYPE_XS_NUMERIC)})) {
        /* eq promotes a number compared with an xs:double (XPath 4.0,
           B.2), so 3.7 matches number('3.7'): numbers are keyed as
           xs:double. */
        sxml_sequence cast = sxml_sequence_empty();
        const sxml_status status = sxml_cast_make_value(
            call->item_heap,
            &(sxml_cast_request){
                .value = sxml_sequence_from_item(value),
                .target_type = sxml_builtin_type_identifier(SXML_TYPE_XS_DOUBLE),
                .allows_empty = false,
                .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
                .error_record = call->error_record},
            &cast);
        if (status == SXML_STATUS_OK) {
            *key = cast.content;
        }
        return status;
    }
    if (!is_string_like(type, registry)) {
        sxml_item_retain(value);
        *key = value;
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    sxml_status status = sxml_atomic_item_get_datum(value, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (normalization.collation == NULL) {
        return sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){.type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                                        .datum = datum},
            key);
    }
    sxml_collation_key collation_key = {.allocator = sxml_item_heap_as_allocator(call->item_heap),
                                        .bytes = NULL};
    status =
        sxml_collation_make_key(&(sxml_collation_key_request){.collation = normalization.collation,
                                                              .input = datum.datum_details.string},
                                &collation_key);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_BASE64_BINARY),
                .datum = {.datum_kind = SXML_DATUM_KIND_BINARY,
                          .datum_details = {.binary = sxml_collation_key_view(&collation_key)}}},
            key);
    }
    sxml_collation_key_release(&collation_key);
    return status;
}

/* Appends one part of a composite key: a tag ("s" string-like, "n"
   numeric, else "t" and the type), then the text of the normalized
   value, so values eq compares equal give equal text (numbers by their
   canonical xs:string text, 1 and 1.0 alike). */
static sxml_status append_composite_part(key_normalization normalization, sxml_item key,
                                         sxml_library_text *text) {
    const sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(normalization.call->item_heap);
    const sxml_type_identifier type = sxml_item_get_type_annotation(key);
    sxml_status status = SXML_STATUS_OK;
    if (sxml_type_registry_is_derived_from(
            registry,
            (sxml_type_pair){.derived_type = type,
                             .base_type = sxml_builtin_type_identifier(SXML_TYPE_XS_NUMERIC)})) {
        status = sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("n"));
    } else if (is_string_like(type, registry) ||
               type.index == (uint32_t)SXML_TYPE_XS_BASE64_BINARY) {
        status = sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("s"));
    } else {
        status = sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("t"));
        for (uint32_t digits = type.index; status == SXML_STATUS_OK; digits /= 10U) {
            status = sxml_function_library_text_append_codepoint(text, '0' + digits % 10U);
            if (digits < 10U) {
                break;
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append_atomic(text, key);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(text,
                                                   SXML_BYTE_VIEW_FROM_LITERAL(KEY_PART_SEPARATOR));
    }
    return status;
}

/* key-normalize($values, $collation, $composite): the map keys of key
   values (normalize_key_value), one per value, or for a composite key
   (composite="yes", XSLT 4.0, 20.2.1) one xs:string standing for the
   whole sequence. $collation is a collation URI, "" for the codepoint
   collation. */
static sxml_status call_key_normalize(const sxml_function_call *call, sxml_sequence *result) {
    sxml_string collation_uri;
    sxml_status status = sxml_function_library_read_string_argument(call, 1, &collation_uri);
    sxml_optional_item composite = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 2, &composite);
    }
    sxml_collation collation;
    memset(&collation, 0, sizeof(collation));
    bool has_collation = false;
    if (status == SXML_STATUS_OK && collation_uri.utf8.byte_count > 0 &&
        !sxml_byte_view_equals(collation_uri.utf8,
                               sxml_function_library_codepoint_collation_uri())) {
        status = sxml_function_library_find_collation(call, collation_uri.utf8, &collation);
        has_collation = true;
    }
    bool is_composite = false;
    if (status == SXML_STATUS_OK && composite.has_item) {
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof(datum));
        status = sxml_atomic_item_get_datum(composite.item, &datum);
        is_composite = status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_BOOLEAN &&
                       datum.datum_details.boolean;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const key_normalization normalization = {.call = call,
                                             .collation = has_collation ? &collation : NULL};
    sxml_sequence_builder *builder = NULL;
    status = sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_library_text text = {
        .item_heap = call->item_heap, .bytes = NULL, .byte_count = 0, .byte_capacity = 0};
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    for (;;) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_item key = {.representation = 0};
        status = normalize_key_value(normalization, step.item, &key);
        if (status == SXML_STATUS_OK) {
            status = is_composite ? append_composite_part(normalization, key, &text)
                                  : sxml_sequence_builder_append_item(builder, key);
            sxml_item_release(key);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    sxml_sequence keys = sxml_sequence_empty();
    if (status == SXML_STATUS_OK && is_composite) {
        status = sxml_function_library_make_string(call->item_heap,
                                                   sxml_function_library_text_view(&text), &keys);
    } else if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, &keys);
    }
    sxml_function_library_text_release(&text);
    sxml_sequence_builder_destroy(builder);
    if (status == SXML_STATUS_OK) {
        *result = keys;
    }
    return status;
}

static const sxml_function_parameter key_name_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("key-name"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:anyAtomicType"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
};

static const sxml_function_parameter key_index_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("number"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:integer"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("root"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("node()"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("build"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("fn(node()) as map(*)"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
};

static const sxml_function_parameter key_normalize_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("values"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:anyAtomicType*"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("collation"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:string"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("composite"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
};

/* In no namespace: only the snippets of src/xslt_key.c name them, through
   a table no stylesheet expression sees. */
static const sxml_native_function key_functions[] = {
    {.function_namespace = SXML_NAMESPACE_NONE,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("key-name"),
     .parameters = {key_name_parameters, 1},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:QName"),
     .call = call_key_name,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_NONE,
     .properties =
         SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT | SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("key-index"),
     .parameters = {key_index_parameters, 3},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("map(*)"),
     .call = NULL,
     .resumable_body = &key_index_body},
    {.function_namespace = SXML_NAMESPACE_NONE,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("key-normalize"),
     .parameters = {key_normalize_parameters, 3},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:anyAtomicType*"),
     .call = call_key_normalize,
     .resumable_body = NULL},
};

const sxml_native_function_view sxml_xslt_key_functions = SXML_LIBRARY_PART(key_functions);
