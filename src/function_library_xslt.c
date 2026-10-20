/*
 * The XSLT 4.0 functions written in C (src/xslt_key_internal.h; XSLT 4.0,
 * sections 20 and 24; docs/DECISIONS.md D101, D139, D149; packet 4.10).
 *
 * One library part, `xslt`, that only XSLT compilation names
 * (sxml_function_library_xslt_languages, D101): the functions of static
 * expressions (system-property, available-system-properties,
 * function-available, element-available, type-available; moved here from
 * src/xslt_static.c, which serves its expressions the first rows,
 * sxml_xslt_static_functions), current, document, unparsed-entity-uri,
 * unparsed-entity-public-id, and regex-groups and regex-group (packet
 * 4.11, D153). Names given as strings are resolved against
 * the namespaces of the unit running (D84, D139).
 *
 * The rows behind fn:key (sxml_xslt_key_functions, in no namespace and in
 * no part): key-name, key-index (the index of one key for one tree, built
 * once per evaluation and kept in the evaluation memo) and key-normalize
 * (key values as map keys). src/xslt_key.c lowers fn:key from XPath texts
 * that call them. The rows behind the accumulator functions
 * (sxml_xslt_accumulator_functions, packet 4.14, D155): accumulator-name,
 * accumulator-scope, accumulator-values, accumulator-link,
 * accumulator-original and accumulator-streamed (D231), for
 * src/xslt_accumulator.c.
 *
 * Golden file: src/function_library_format.c (rows of one part, bodies
 * reading their arguments through function_library_core_internal.h);
 * src/function_library_higher_order.c for the resumable body.
 */
#include "tree_internal.h"
#include "xslt_key_internal.h"

#include "cast_internal.h"
#include "collation_internal.h"
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "function_library_internal.h"
#include "function_library_parts_internal.h"
#include "ir_internal.h"
#include "schema_scope_internal.h"
#include "xdm_internal.h"
#include "xslt_accumulator_internal.h"
#include "xslt_lower_internal.h"
#include "system_property_internal.h"
#include "sxml/version.h"
#include "xslt_static_internal.h"
#include "xml_internal.h"

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

/* The namespace a prefix has in the running unit: its static namespaces,
   then xml (XSLT 4.0, 5.3.1; D261); prefix empty is the
   unit's default element namespace. False when it is unbound. */
typedef struct prefix_request {
    const sxml_function_call *call;
    sxml_byte_view namespace_prefix;
} prefix_request;

/* The namespaces names given as strings are resolved against: the static
   namespaces of the code running, the calling expression's (D163; the
   unit's for a static expression, D139), as casts to xs:QName at run time
   (D84). */
static sxml_namespace_binding_view evaluation_bindings_of(const sxml_function_call *call) {
    if (call->dynamic_context == NULL) {
        return (sxml_namespace_binding_view){.namespace_bindings = NULL,
                                             .namespace_binding_count = 0};
    }
    return sxml_dynamic_context_get_static_namespaces(call->dynamic_context);
}

static bool resolve_prefix(prefix_request request, sxml_namespace_uri_identifier *namespace_uri) {
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
            /* A prefix bound to no namespace is undeclared (D262). */
            return binding->namespace_prefix.index == 0 ||
                   binding->namespace_uri.index != SXML_NAMESPACE_NONE;
        }
    }
    if (request.namespace_prefix.byte_count == 0) {
        *namespace_uri = (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_NONE};
        return true;
    }
    /* Of XPath's predeclared prefixes, XSLT knows only xml (5.3.1; D261). */
    if (sxml_byte_view_equals(SXML_BYTE_VIEW_FROM_LITERAL("xml"), request.namespace_prefix)) {
        *namespace_uri = (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_XML};
        return true;
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
    const sxml_byte_view text = sxml_xml_text_trim_whitespace(datum.datum_details.string.utf8);
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
            if ((has_prefix && !sxml_xml_text_is_ncname(prefix)) ||
                !resolve_prefix((prefix_request){.call = call, .namespace_prefix = prefix},
                                &namespace_uri)) {
                return sxml_function_library_report(call, request.error);
            }
        } else if (request.role == SXML_NAME_ARGUMENT_ROLE_FUNCTION) {
            namespace_uri = (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_FUNCTIONS};
        }
    }
    /* An EQName's local part is an NCName (XPath 4.0, A.1, EQName;
       XTDE1390, XTDE1400, XTDE1440, XTDE1428 otherwise). */
    if (!sxml_xml_text_is_ncname(local_name)) {
        return sxml_function_library_report(call, request.error);
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

/* The system properties of the XSLT namespace (XSLT 4.0, 24.2.1) are those
   of the shared table (src/system_property.c) with an XSLT name. */
#define XSLT_SYSTEM_PROPERTY_CAPACITY 32U

/* A property's value as fn:system-property gives it: an xs:boolean is yes
   or no, a decimal its text (24.2.1). */
static sxml_byte_view system_property_text(const sxml_system_property *property) {
    if (property->value_kind == SXML_SYSTEM_PROPERTY_VALUE_PRODUCT_VERSION) {
        return sxml_byte_view_from_c_string(sxml_version_text());
    }
    if (property->value.datum_kind == SXML_DATUM_KIND_BOOLEAN) {
        return property->value.datum_details.boolean ? SXML_BYTE_VIEW_FROM_LITERAL("yes")
                                                     : SXML_BYTE_VIEW_FROM_LITERAL("no");
    }
    if (property->value.datum_kind == SXML_DATUM_KIND_DECIMAL) {
        return property->value.datum_details.decimal_text;
    }
    return property->value.datum_details.string.utf8;
}

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
    const sxml_system_property_view properties = sxml_system_properties();
    for (size_t property_index = 0; parts.namespace_uri.index == SXML_NAMESPACE_XSLT &&
                                    property_index < properties.property_count;
         property_index += 1) {
        const sxml_system_property *property = &properties.properties[property_index];
        if (property->xslt_name.byte_count > 0 &&
            sxml_byte_view_equals(property->xslt_name, parts.local_name)) {
            value = system_property_text(property);
        }
    }
    return sxml_function_library_make_string(call->item_heap, value, result);
}

/* fn:available-system-properties() (XSLT 4.0, 24.2.2). */
static sxml_status call_available_system_properties(const sxml_function_call *call,
                                                    sxml_sequence *result) {
    sxml_item items[XSLT_SYSTEM_PROPERTY_CAPACITY];
    size_t made_count = 0;
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    sxml_status status = sxml_name_pool_intern_namespace_prefix(
        name_pool, SXML_BYTE_VIEW_FROM_LITERAL("xsl"), &prefix);
    const sxml_system_property_view properties = sxml_system_properties();
    for (size_t property_index = 0; status == SXML_STATUS_OK &&
                                    property_index < properties.property_count &&
                                    made_count < XSLT_SYSTEM_PROPERTY_CAPACITY;
         property_index += 1) {
        const sxml_system_property *property = &properties.properties[property_index];
        if (property->xslt_name.byte_count == 0) {
            continue;
        }
        sxml_name_identifier name = {.index = 0};
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_XSLT},
                                 .local_name = property->xslt_name},
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

/* Whether query names an fn function XSLT lowering implements rather
   than a library row (src/xslt_key.c, src/xslt_accumulator.c, and the
   context functions of grouping and merging, src/xpath_lower.c: a
   static call binds them anywhere, 14.2, 15.5). */
static bool is_lowered_function(const sxml_function_call *call, sxml_function_query query) {
    static const struct {
        const char *local_name;
        size_t first_arity;
        size_t last_arity;
    } lowered[] = {
        {"key", 2, 3},      {"map-for-key", 1, 2},        {"copy-of", 0, 1},
        {"snapshot", 0, 1}, {"accumulator-before", 1, 1}, {"accumulator-after", 1, 1},
        {"current-group", 0, 0}, {"current-grouping-key", 0, 0},
        {"current-merge-group", 0, 1}, {"current-merge-key", 0, 0},
        {"current-merge-key-array", 0, 0},
    };
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (sxml_name_pool_get_name_parts(sxml_item_heap_get_name_pool(call->item_heap),
                                      query.function_name, &parts) != SXML_STATUS_OK ||
        parts.namespace_uri.index != SXML_NAMESPACE_FUNCTIONS) {
        return false;
    }
    for (size_t entry_index = 0; entry_index < sizeof(lowered) / sizeof(lowered[0]);
         entry_index += 1) {
        if (sxml_byte_view_equals(parts.local_name,
                                  sxml_byte_view_from_c_string(lowered[entry_index].local_name)) &&
            query.arity >= lowered[entry_index].first_arity &&
            query.arity <= lowered[entry_index].last_arity) {
            return true;
        }
    }
    return false;
}

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
    for (size_t arity = first_arity; arity <= last_arity && !is_available; arity += 1) {
        is_available = is_lowered_function(call, (sxml_function_query){.function_name = name,
                                                                       .arity = arity});
        /* 24.1: stylesheet functions are available too: the running
           unit's named functions, as fn:function-lookup finds them. */
        sxml_item found = {.representation = 0};
        if (!is_available) {
            status = sxml_evaluator_find_unit_function(
                call->dynamic_context,
                (sxml_unit_function_query){
                    .function_name = name, .arity = arity, .caller_focus = NULL},
                &found);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (found.representation != 0) {
            is_available = true;
            sxml_item_release(found);
        }
    }
    *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = is_available});
    return SXML_STATUS_OK;
}

/* fn:element-available($name) (XSLT 4.0, 24.3.2): XSLT elements sxml
   implements, and exsl:document (D206). */
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
    const bool is_available = sxml_xslt_is_element_available(
        (sxml_xslt_element_query){.namespace_uri = parts.namespace_uri,
                                  .local_name = parts.local_name});
    *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = is_available});
    return SXML_STATUS_OK;
}

/* fn:type-available($name) (XSLT 4.0, 20.4): a built-in type, or a type
   of the in-scope schemas of the call's static context (D179). */
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
    const sxml_ir_static_context *context =
        call->dynamic_context == NULL
            ? NULL
            : sxml_dynamic_context_get_static_context(call->dynamic_context);
    sxml_schema_scope_type found;
    const bool is_available =
        sxml_type_registry_find_type(sxml_item_heap_get_type_registry(call->item_heap), name,
                                     &type) ||
        (context != NULL &&
         sxml_schema_scope_find_type(context->in_scope_schemas, name, &found));
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

/* fn:document's options (XSLT 4.0, 20.1): a map's are fn:doc's
   (sxml_function_library_read_document_options, trust-external false by
   default; base-uri is read with the URI references). A node or no second
   argument, the XSLT 3.0 forms, keeps 3.0's reading of external DTDs and
   entities through the resolver, which access "explicitly enabled" may
   allow (D273); no DTD validation, strip-space "conditional" (the
   stylesheet's xsl:strip-space, which the loader applies). */
static sxml_status read_document_function_options(const sxml_function_call *call,
                                                  sxml_document_options *options) {
    *options = (sxml_document_options){.strips_space = false,
                                       .keeps_space = false,
                                       .validates_dtd = false,
                                       .trusts_external = true,
                                       .xsd_validation = sxml_sequence_empty()};
    sxml_optional_item argument = {.has_item = false};
    sxml_status status = SXML_STATUS_OK;
    if (call->arguments.sequence_count > 1) {
        status = sxml_function_library_read_optional_item(call, 1, &argument);
    }
    if (status != SXML_STATUS_OK || !argument.has_item ||
        sxml_item_get_kind(argument.item) != SXML_ITEM_KIND_MAP) {
        return status;
    }
    return sxml_function_library_read_document_options(call, 1, options);
}

/* fn:document($uri-sequence, $options?) (XSLT 4.0, 20.1; $options a node
   whose base URI, or a map whose base-uri option, relative references are
   resolved against): the documents
   the URI references give, through the document loader as fn:doc (the
   same node for the same URI within an evaluation), in document order
   without duplicates. A fragment identifier is ignored (20.1 leaves its
   meaning to the media type; sxml supports none): the whole document is
   returned. An options map's options are fn:doc's: an external DTD or
   entity is FODC0016 without trust-external, dtd-validation and
   strip-space "all" and xsd-validation act as for fn:doc (F&O 4.0,
   17.1.1, 17.2.1; D179), and strip-space "none" reads the document
   without the stylesheet's xsl:strip-space (D274). */
static sxml_status call_document(const sxml_function_call *call, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_document_options options;
    sxml_status status = read_document_function_options(call, &options);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view xsd_validation = text_of(options.xsd_validation);
    sxml_sequence_builder *builder = NULL;
    status = sxml_sequence_builder_create(
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
                                         .error_record = call->error_record,
                                         .blocks_external_resources = !options.trusts_external,
                                         .validates_dtd = options.validates_dtd,
                                         .skips_preparation = options.keeps_space},
                &document_node);
        }
        release_document_reference(&reference);
        if (status == SXML_STATUS_OK && options.strips_space) {
            sxml_sequence stripped = sxml_sequence_empty();
            status = sxml_function_library_strip_document(call, document_node, &stripped);
            sxml_item_release(document_node);
            document_node =
                status == SXML_STATUS_OK ? stripped.content : (sxml_item){.representation = 0};
        }
        if (status == SXML_STATUS_OK && xsd_validation.byte_count > 0) {
            status = sxml_schema_scope_validate_document(
                call,
                &(sxml_schema_scope_document_validation){
                    .xsd_validation = xsd_validation,
                    .uses_schema_location_hints = options.uses_schema_location_hints},
                &document_node);
        }
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
    sxml_sequence_release(options.xsd_validation);
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_make_document_order_value(
            call->dynamic_context,
            &(sxml_document_order_request){.input = documents, .error_record = call->error_record},
            result);
    }
    sxml_sequence_release(documents);
    return status;
}

/* XSLT 4.0, 20.5.2 and 20.5.3: each function has its own code. An
   absent context item is XPDY0002, raised before the call, which both
   sections name for the omitted $doc. */
static const sxml_static_error unparsed_entity_uri_document_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE1370"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The node given to unparsed-entity-uri is not in a tree whose root is a document node "
        "(XSLT 4.0, 20.5.2)")};

static const sxml_static_error unparsed_entity_public_id_document_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE1380"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The node given to unparsed-entity-public-id is not in a tree whose root is a document "
        "node (XSLT 4.0, 20.5.3)")};

/* Raises document_error unless the node of argument 1 is in a document. */
static sxml_status check_unparsed_entity_document(const sxml_function_call *call,
                                                  const sxml_static_error *document_error) {
    sxml_optional_item node_item = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 1, &node_item);
    sxml_node_reference node;
    if (status == SXML_STATUS_OK && node_item.has_item) {
        status = sxml_item_get_node(node_item.item, &node);
    }
    if (status != SXML_STATUS_OK || !node_item.has_item) {
        return status == SXML_STATUS_OK ? sxml_function_library_report(call, document_error)
                                        : status;
    }
    sxml_node_description description;
    status = sxml_node_describe(
        (sxml_node_reference){.tree = node.tree, .node_index = {.index = 0}, .namespace_number = 0},
        &description);
    if (status != SXML_STATUS_OK || description.node_kind != SXML_NODE_KIND_DOCUMENT) {
        return sxml_function_library_report(call, document_error);
    }
    return SXML_STATUS_OK;
}

/* The unparsed entity of argument 0's name in the document of argument
   1's node, after document_error (in); found is false when the DTD
   declares none (XDM 4.0, 6.1.3, dm:unparsed-entity-system-id). */
typedef struct unparsed_entity_lookup {
    const sxml_static_error *document_error;
    bool is_found;
    sxml_unparsed_entity entity;
} unparsed_entity_lookup;

static sxml_status find_unparsed_entity(const sxml_function_call *call,
                                        unparsed_entity_lookup *lookup) {
    lookup->is_found = false;
    sxml_status status = check_unparsed_entity_document(call, lookup->document_error);
    sxml_string name = {.utf8 = {.bytes = NULL, .byte_count = 0},
                        .codepoint_count = 0,
                        .is_ascii = true};
    sxml_optional_item node_item = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 0, &name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 1, &node_item);
    }
    sxml_node_reference node;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(node_item.item, &node);
    }
    if (status != SXML_STATUS_OK || node.tree->operations->find_unparsed_entity == NULL) {
        return status;
    }
    lookup->entity = (sxml_unparsed_entity){
        .entity_name = name.utf8,
        .system_identifier = {.bytes = NULL, .byte_count = 0},
        .public_identifier = {.bytes = NULL, .byte_count = 0}};
    status = node.tree->operations->find_unparsed_entity(node.tree->state, &lookup->entity);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        return SXML_STATUS_OK;
    }
    lookup->is_found = status == SXML_STATUS_OK;
    return status;
}

/* fn:unparsed-entity-uri($entity-name, $doc := .) (XSLT 4.0, 20.2.3): the
   absolute system identifier of the unparsed entity, the zero-length URI
   when there is none. */
static sxml_status call_unparsed_entity_uri(const sxml_function_call *call, sxml_sequence *result) {
    unparsed_entity_lookup lookup = {.document_error = &unparsed_entity_uri_document_error,
                                     .is_found = false};
    const sxml_status status = find_unparsed_entity(call, &lookup);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view uri = lookup.is_found
                                   ? lookup.entity.system_identifier
                                   : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    sxml_item item = {.representation = 0};
    const sxml_status make_status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_URI),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = uri,
                                                   .codepoint_count = 0,
                                                   .is_ascii = false}}}},
        &item);
    if (make_status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return make_status;
}

/* fn:unparsed-entity-public-id($entity-name, $doc := .) (XSLT 4.0,
   20.2.4): the public identifier, the zero-length string when there is
   none. */
static sxml_status call_unparsed_entity_public_id(const sxml_function_call *call,
                                                  sxml_sequence *result) {
    unparsed_entity_lookup lookup = {.document_error = &unparsed_entity_public_id_document_error,
                                     .is_found = false};
    const sxml_status status = find_unparsed_entity(call, &lookup);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_function_library_make_string(
        call->item_heap,
        lookup.is_found ? lookup.entity.public_identifier
                        : (sxml_byte_view){.bytes = NULL, .byte_count = 0},
        result);
}

/* ------------------------------------------------------ Captured groups */

/* The current captured groups (XSLT 4.0, 17.2): the context variable
   Q{XSLT}regex-groups that xsl:analyze-string puts in scope
   (src/xslt_lower_number.c, D153): a map from group numbers and names to
   fn:captured-group-record values, group 0 the match; () outside
   xsl:matching-substring. */
static sxml_status read_captured_groups(const sxml_function_call *call, size_t argument_index,
                                        sxml_optional_item *groups) {
    return sxml_function_library_read_optional_item(call, argument_index, groups);
}

/* fn:regex-groups() as map((xs:integer | xs:string), fn:captured-group-record)
   (17.2.1): the current captured groups, the empty map without. */
static sxml_status call_regex_groups(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item groups = {.has_item = false};
    const sxml_status status = read_captured_groups(call, 0, &groups);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (groups.has_item) {
        sxml_item_retain(groups.item);
        *result = sxml_sequence_from_item(groups.item);
        return SXML_STATUS_OK;
    }
    sxml_item empty_map = {.representation = 0};
    const sxml_status making_status = sxml_map_make(
        call->item_heap,
        &(sxml_map_request){.map_entries = {.map_entries = NULL, .map_entry_count = 0},
                            .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                            .error_record = call->error_record},
        &empty_map);
    if (making_status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(empty_map);
    }
    return making_status;
}

/* The entry key of map, or (is_found false) none. */
typedef struct map_key_lookup {
    sxml_item map;
    sxml_item key;
} map_key_lookup;

static sxml_status look_up_key(map_key_lookup request, sxml_map_lookup *lookup) {
    lookup->is_found = false;
    return sxml_map_find(request.map, request.key, lookup);
}

/* The field of a record (a map) whose key is the string field_name. */
typedef struct record_field_read {
    const sxml_function_call *call;
    sxml_item record;
    const char *field_name;
} record_field_read;

static sxml_status read_record_field(record_field_read request, sxml_map_lookup *field) {
    sxml_sequence key = sxml_sequence_empty();
    sxml_status status = sxml_function_library_make_string(
        request.call->item_heap, sxml_byte_view_from_c_string(request.field_name), &key);
    sxml_item key_item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            key, (sxml_item_request){.item_index = 0, .error_record = NULL}, &key_item);
    }
    if (status == SXML_STATUS_OK) {
        status = look_up_key((map_key_lookup){.map = request.record, .key = key_item}, field);
    }
    sxml_sequence_release(key);
    return status;
}

/* The first entry of a groups map whose record's nr equals an integer
   key; none for other keys. */
static sxml_status find_group_by_number(map_key_lookup request, const sxml_function_call *call,
                                        sxml_map_lookup *found) {
    sxml_atomic_datum wanted;
    sxml_status status = sxml_atomic_item_get_datum(request.key, &wanted);
    if (status != SXML_STATUS_OK || wanted.datum_kind != SXML_DATUM_KIND_INTEGER) {
        return status;
    }
    size_t entry_count = 0;
    status = sxml_map_get_size(request.map, &entry_count);
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(request.map, entry_index, &entry);
        sxml_item record = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(
                entry.value, (sxml_item_request){.item_index = 0, .error_record = NULL}, &record);
        }
        sxml_map_lookup number = {.is_found = false};
        if (status == SXML_STATUS_OK) {
            status = read_record_field(
                (record_field_read){.call = call, .record = record, .field_name = "nr"}, &number);
        }
        sxml_item number_item = {.representation = 0};
        if (status != SXML_STATUS_OK || !number.is_found ||
            sxml_sequence_get_item(number.entry.value,
                                   (sxml_item_request){.item_index = 0, .error_record = NULL},
                                   &number_item) != SXML_STATUS_OK) {
            continue;
        }
        sxml_atomic_datum datum;
        if (sxml_atomic_item_get_datum(number_item, &datum) == SXML_STATUS_OK &&
            datum.datum_kind == SXML_DATUM_KIND_INTEGER &&
            datum.datum_details.integer == wanted.datum_details.integer) {
            *found = (sxml_map_lookup){.is_found = true, .entry_index = entry_index, .entry = entry};
            return SXML_STATUS_OK;
        }
    }
    return status;
}

/* fn:regex-group($group as (xs:integer | xs:string)) as xs:string
   (17.2.2): regex-groups()?$group?value, else "". */
static sxml_status call_regex_group(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item group = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &group);
    sxml_optional_item groups = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status = read_captured_groups(call, 1, &groups);
    }
    sxml_map_lookup entry = {.is_found = false};
    if (status == SXML_STATUS_OK && group.has_item && groups.has_item) {
        status = look_up_key((map_key_lookup){.map = groups.item, .key = group.item}, &entry);
    }
    if (status == SXML_STATUS_OK && !entry.is_found && group.has_item && groups.has_item) {
        /* A named group by its number too, as XSLT 3.0 had it (QT4
           analyze-string-406): the entry whose record has that nr. */
        status = find_group_by_number(
            (map_key_lookup){.map = groups.item, .key = group.item}, call, &entry);
    }
    sxml_map_lookup value = {.is_found = false};
    if (status == SXML_STATUS_OK && entry.is_found) {
        /* The fn:captured-group-record's value. */
        sxml_item record = {.representation = 0};
        status = sxml_sequence_get_item(
            entry.entry.value, (sxml_item_request){.item_index = 0, .error_record = NULL},
            &record);
        if (status == SXML_STATUS_OK) {
            status = read_record_field(
                (record_field_read){.call = call, .record = record, .field_name = "value"},
                &value);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (value.is_found) {
        sxml_sequence_retain(value.entry.value);
        *result = value.entry.value;
        return SXML_STATUS_OK;
    }
    return sxml_function_library_make_string(
        call->item_heap, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, result);
}

static const sxml_function_parameter regex_groups_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("regex-groups"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("map(*)?"),
     .parameter_default = SXML_PARAMETER_DEFAULT_CONTEXT_VARIABLE},
};

static const sxml_function_parameter regex_group_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("group"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("(xs:integer | xs:string)"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("regex-groups"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("map(*)?"),
     .parameter_default = SXML_PARAMETER_DEFAULT_CONTEXT_VARIABLE},
};

/* fn:stream-available($uri as xs:string?) as xs:boolean (XSLT 4.0,
   18.2): true when the document at $uri can be retrieved and its prolog
   and the start tag of its document element are well-formed, as a
   streamed open reads them (xsl:source-document streamable="yes"); the
   rest is not read ("it is not necessary to check that the entire
   document is well-formed"). Without a processor's document loader, the
   answer is whether fn:doc reads it. A relative URI is resolved against
   the static base URI; () gives false. */
static sxml_status call_stream_available(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item uri;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &uri);
    bool is_available = false;
    if (status == SXML_STATUS_OK && uri.has_item && call->dynamic_context != NULL) {
        sxml_string uri_reference;
        status = sxml_function_library_read_string_argument(call, 0, &uri_reference);
        sxml_stream_availability availability = SXML_STREAM_AVAILABILITY_UNKNOWN;
        if (status == SXML_STATUS_OK) {
            status = sxml_evaluator_stream_check_available(call->dynamic_context,
                                                           uri_reference.utf8, &availability);
        }
        is_available = availability == SXML_STREAM_AVAILABILITY_AVAILABLE;
        if (status == SXML_STATUS_OK && availability == SXML_STREAM_AVAILABILITY_UNKNOWN) {
            sxml_item document_node = {.representation = 0};
            status = sxml_dynamic_context_load_document(
                call->dynamic_context,
                &(sxml_document_request){
                    .uri_reference = uri_reference.utf8,
                    .base_uri = sxml_dynamic_context_get_static_base_uri(call->dynamic_context),
                    .error_record = NULL},
                &document_node);
            if (status == SXML_STATUS_OK) {
                is_available = true;
                sxml_item_release(document_node);
            } else if (status == SXML_STATUS_DYNAMIC_ERROR) {
                /* What fn:doc would raise is the answer false. */
                sxml_item_heap_clear_raised_error(call->item_heap);
                status = SXML_STATUS_OK;
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean(
            (sxml_boolean_result){.is_true = is_available});
    }
    return status;
}

static const sxml_function_parameter stream_available_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("uri"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:string?"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
};

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
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = (uint32_t)SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT |
                   (uint32_t)SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("stream-available"),
     .parameters = {stream_available_parameters, 1},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean"),
     .call = call_stream_available,
     .resumable_body = NULL},
    /* Context functions (glossary `context_function`): their values come
       from the variable xsl:analyze-string puts in scope (D153). */
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = 0,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("regex-groups"),
     .parameters = {regex_groups_parameters, 1},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("map(*)"),
     .call = call_regex_groups,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = 0,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("regex-group"),
     .parameters = {regex_group_parameters, 2},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:string"),
     .call = call_regex_group,
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

/* The expanded name an xs:QName or a string holding an EQName gives
   (unprefixed, no namespace) as an xs:QName; error for anything else. */
static sxml_status make_name_result(const sxml_function_call *call, const sxml_static_error *error,
                                    sxml_sequence *result) {
    sxml_name_identifier name = {.index = 0};
    sxml_status status = read_name_argument(
        (name_argument_request){
            .call = call, .role = SXML_NAME_ARGUMENT_ROLE_NO_NAMESPACE, .error = error},
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

/* key-name($key-name): the expanded name of the key (XSLT 4.0, 20.2.2:
   unprefixed, no namespace). */
static sxml_status call_key_name(const sxml_function_call *call, sxml_sequence *result) {
    return make_name_result(call, &key_name_error, result);
}

/* ------------------------------------------- Builds kept per evaluation */

/* The non-negative xs:integer item, or UINT64_MAX for anything else. */
static uint64_t read_number(sxml_item item) {
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    if (sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_INTEGER || datum.datum_details.integer < 0) {
        return UINT64_MAX;
    }
    return (uint64_t)datum.datum_details.integer;
}

/*
 * A build kept per tree: the rows key-index($number, $root, $build) and
 * accumulator-values($number, $root, $build) give the map $build($root)
 * returns, computed the first time an evaluation asks for it and kept in
 * the evaluation memo (D149) for the rest of it (XSLT 4.0, 20.2.2: a key's
 * index; 19.3: accumulator values, lazily, per tree), under the
 * computation (the row's part) and $number. While the build runs, the
 * entry is the empty sequence, so a build that asks for itself raises the
 * circularity error instead of running forever (XTDE0640 for keys, 9.11;
 * XTDE3400 for accumulators, 19.6); a build that fails leaves xs:boolean
 * false, and the next call builds again and raises its error again (19.5).
 */
typedef struct kept_build {
    const sxml_native_function_view *computation;
    const sxml_static_error *cycle_error;
} kept_build;

typedef struct kept_build_state {
    /* Owned while the build call runs. */
    sxml_sequence call_argument;
    /* Set while the build runs: release_state then marks the entry for a
       new build (the build failed). */
    sxml_dynamic_context *dynamic_context;
    sxml_evaluation_memo_key key;
    bool is_building;
} kept_build_state;

static sxml_evaluation_memo_key kept_build_key(const sxml_function_call *call, kept_build build) {
    return (sxml_evaluation_memo_key){
        .computation = build.computation,
        .computation_number = read_number(sxml_function_library_get_argument(call, 0).content),
        .subject = sxml_function_library_get_argument(call, 1).content};
}

static sxml_status step_kept_build(const sxml_native_step_request *request, kept_build build,
                                   sxml_native_step *step) {
    kept_build_state *state = request->state;
    const sxml_function_call *call = request->call;
    const sxml_evaluation_memo_key key = kept_build_key(call, build);
    if (request->step_number == 0) {
        sxml_sequence values = sxml_sequence_empty();
        if (sxml_dynamic_context_find_memo(call->dynamic_context, &key, &values)) {
            sxml_item item = {.representation = 0};
            if (sxml_sequence_get_item(values,
                                       (sxml_item_request){.item_index = 0, .error_record = NULL},
                                       &item) != SXML_STATUS_OK) {
                return sxml_function_library_report(call, build.cycle_error);
            }
            if (sxml_item_get_kind(item) == SXML_ITEM_KIND_MAP) {
                sxml_sequence_retain(values);
                *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                                           .result = values};
                return SXML_STATUS_OK;
            }
        }
        const sxml_status status = sxml_dynamic_context_store_memo(
            call->dynamic_context,
            &(sxml_evaluation_memo_entry){.key = key, .value = sxml_sequence_empty()});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        state->dynamic_context = call->dynamic_context;
        state->key = key;
        state->is_building = true;
        state->call_argument = sxml_function_library_get_argument(call, 1);
        sxml_sequence_retain(state->call_argument);
        *step = (sxml_native_step){
            .step_kind = SXML_NATIVE_STEP_KIND_CALL,
            .result = sxml_sequence_empty(),
            .function_item = sxml_function_library_get_argument(call, 2).content,
            .arguments = {.sequences = &state->call_argument, .sequence_count = 1}};
        return SXML_STATUS_OK;
    }
    state->is_building = false;
    const sxml_sequence values = request->call_result;
    const sxml_status status = sxml_dynamic_context_store_memo(
        call->dynamic_context, &(sxml_evaluation_memo_entry){.key = key, .value = values});
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(values);
        return status;
    }
    *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH, .result = values};
    return SXML_STATUS_OK;
}

static void release_kept_build_state(void *state) {
    kept_build_state *build_state = state;
    if (build_state->is_building) {
        /* The entry exists, so replacing it allocates nothing and cannot
           fail. */
        const sxml_status status = sxml_dynamic_context_store_memo(
            build_state->dynamic_context,
            &(sxml_evaluation_memo_entry){
                .key = build_state->key,
                .value = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = false})});
        (void)status;
        build_state->is_building = false;
    }
    sxml_sequence_release(build_state->call_argument);
    build_state->call_argument = sxml_sequence_empty();
}

static const sxml_static_error key_cycle_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE0640"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The index of a key depends on itself: its match pattern or use expression calls "
        "fn:key for the same key (XSLT 4.0, 9.11, 20.2.1)")};

/* key-index($number, $root, $build): the index of key $number for the
   tree of $root, a kept build (XSLT 4.0, 20.2.2). */
static sxml_status step_key_index(const sxml_native_step_request *request, sxml_native_step *step) {
    if (request->call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return step_kept_build(
        request,
        (kept_build){.computation = &sxml_xslt_key_functions, .cycle_error = &key_cycle_error},
        step);
}

static const sxml_resumable_native_body key_index_body = {
    .step = step_key_index,
    .release_state = release_kept_build_state,
    .state_byte_count = sizeof(kept_build_state),
    .state_alignment = alignof(kept_build_state),
};

/*
 * cached-call($number, $arguments, $body) (src/xslt_lower_internal.h): the
 * value of $body called with the members of the array $arguments, kept in
 * the evaluation memo under $number and the arguments, so a second call
 * with the same arguments gives the first call's value (XSLT 4.0, 10.3.7,
 * memoization; D163). A call that fails keeps nothing.
 */
typedef struct cached_call_state {
    sxml_allocator allocator;
    /* The members of $arguments, borrowed from it, while $body runs. */
    sxml_sequence *arguments;
} cached_call_state;

static sxml_status step_cached_call(const sxml_native_step_request *request,
                                    sxml_native_step *step) {
    cached_call_state *state = request->state;
    const sxml_function_call *call = request->call;
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_evaluation_memo_key key = {
        .computation = &sxml_xslt_cached_call_function,
        .computation_number = read_number(sxml_function_library_get_argument(call, 0).content),
        .subject = sxml_function_library_get_argument(call, 1).content};
    if (request->step_number > 0) {
        const sxml_sequence value = request->call_result;
        const sxml_status status = sxml_dynamic_context_store_memo(
            call->dynamic_context, &(sxml_evaluation_memo_entry){.key = key, .value = value});
        if (status != SXML_STATUS_OK) {
            sxml_sequence_release(value);
            return status;
        }
        *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH, .result = value};
        return SXML_STATUS_OK;
    }
    sxml_sequence value = sxml_sequence_empty();
    if (sxml_dynamic_context_find_memo(call->dynamic_context, &key, &value)) {
        sxml_sequence_retain(value);
        *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH, .result = value};
        return SXML_STATUS_OK;
    }
    size_t member_count = 0;
    sxml_status status = sxml_array_get_size(key.subject, &member_count);
    state->allocator = sxml_item_heap_as_allocator(call->item_heap);
    if (status == SXML_STATUS_OK && member_count > 0) {
        size_t byte_count = 0;
        status = sxml_size_multiply(member_count, sizeof(sxml_sequence), &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                state->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(sxml_sequence)},
                &memory);
        }
        state->arguments = memory;
    }
    for (size_t member_index = 0; status == SXML_STATUS_OK && member_index < member_count;
         member_index += 1) {
        status = sxml_array_get_member(key.subject, member_index, &state->arguments[member_index]);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *step = (sxml_native_step){
        .step_kind = SXML_NATIVE_STEP_KIND_CALL,
        .result = sxml_sequence_empty(),
        .function_item = sxml_function_library_get_argument(call, 2).content,
        .arguments = {.sequences = state->arguments, .sequence_count = member_count}};
    return SXML_STATUS_OK;
}

static void release_cached_call_state(void *state) {
    cached_call_state *call_state = state;
    if (call_state->arguments != NULL) {
        sxml_allocator_release(call_state->allocator, call_state->arguments);
        call_state->arguments = NULL;
    }
}

static const sxml_resumable_native_body cached_call_body = {
    .step = step_cached_call,
    .release_state = release_cached_call_state,
    .state_byte_count = sizeof(cached_call_state),
    .state_alignment = alignof(cached_call_state),
};

static const sxml_function_parameter cached_call_parameters[] = {
    SXML_REQUIRED_PARAMETER("number", "xs:integer"),
    SXML_REQUIRED_PARAMETER("arguments", "array(*)"),
    SXML_REQUIRED_PARAMETER("body", "function(*)"),
};

const sxml_native_function sxml_xslt_cached_call_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties =
        SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT | SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("cached-call"),
    .parameters = {cached_call_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = NULL,
    .resumable_body = &cached_call_body};

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

/* ----------------------------------- Rows behind the accumulator functions */

static const sxml_static_error accumulator_name_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE3340"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The accumulator name given to fn:accumulator-before or fn:accumulator-after is not a "
        "valid EQName (XSLT 4.0, 19.6)")};

static const sxml_static_error accumulator_applicability_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE3362"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The accumulator is not applicable to the tree of the context node: use-accumulators of "
        "the initial mode does not name it (XSLT 4.0, 19.2, 19.6)")};

static const sxml_static_error accumulator_cycle_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE3400"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The values of an accumulator depend on themselves: a rule asks for the accumulator "
        "it computes (XSLT 4.0, 19.6)")};

/* accumulator-name($name): the expanded name of the accumulator (XSLT 4.0,
   19.6: an xs:QName or an EQName, unprefixed meaning no namespace). */
static sxml_status call_accumulator_name(const sxml_function_call *call, sxml_sequence *result) {
    return make_name_result(call, &accumulator_name_error, result);
}

/* The computation number of the memo entry that holds the accumulators
   applicable to a tree (src/xslt_accumulator_internal.h); accumulators
   are numbered from 1. */
#define ACCUMULATOR_SCOPE_NUMBER 0U

/* accumulator-scope($root, $numbers): records that only the accumulators
   numbered $numbers are applicable to the tree of $root (XSLT 4.0, 19.2:
   the initial match selection and use-accumulators of the initial mode),
   for the rest of the evaluation; returns the empty sequence. */
static sxml_status call_accumulator_scope(const sxml_function_call *call, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_status status = sxml_dynamic_context_store_memo(
        call->dynamic_context,
        &(sxml_evaluation_memo_entry){
            .key = {.computation = &sxml_xslt_accumulator_functions,
                    .computation_number = ACCUMULATOR_SCOPE_NUMBER,
                    .subject = sxml_function_library_get_argument(call, 0).content},
            .value = sxml_function_library_get_argument(call, 1)});
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_empty();
    }
    return status;
}

/* accumulator-values($number, $root, $build): the values of accumulator
   $number for the tree of $root, a kept build (XSLT 4.0, 19.3); XTDE3362
   when a scope of the tree leaves the accumulator out. */
static sxml_status check_applicable(const sxml_function_call *call, sxml_item root,
                                    uint64_t number) {
    sxml_sequence numbers = sxml_sequence_empty();
    if (!sxml_dynamic_context_find_memo(
            call->dynamic_context,
            &(sxml_evaluation_memo_key){.computation = &sxml_xslt_accumulator_functions,
                                        .computation_number = ACCUMULATOR_SCOPE_NUMBER,
                                        .subject = root},
            &numbers)) {
        return SXML_STATUS_OK;
    }
    uint64_t count = 0;
    const sxml_status status = sxml_sequence_count(numbers, NULL, &count);
    for (uint64_t item_index = 0; status == SXML_STATUS_OK && item_index < count;
         item_index += 1) {
        sxml_item item = {.representation = 0};
        if (sxml_sequence_get_item(numbers,
                                   (sxml_item_request){.item_index = item_index,
                                                       .error_record = NULL},
                                   &item) == SXML_STATUS_OK &&
            read_number(item) == number) {
            return SXML_STATUS_OK;
        }
    }
    return sxml_function_library_report(call, &accumulator_applicability_error);
}

static sxml_status step_accumulator_values(const sxml_native_step_request *request,
                                           sxml_native_step *step) {
    const sxml_function_call *call = request->call;
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (request->step_number == 0) {
        const sxml_status status = check_applicable(
            call, sxml_function_library_get_argument(call, 1).content,
            read_number(sxml_function_library_get_argument(call, 0).content));
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return step_kept_build(request,
                           (kept_build){.computation = &sxml_xslt_accumulator_functions,
                                        .cycle_error = &accumulator_cycle_error},
                           step);
}

static const sxml_resumable_native_body accumulator_values_body = {
    .step = step_accumulator_values,
    .release_state = release_kept_build_state,
    .state_byte_count = sizeof(kept_build_state),
    .state_alignment = alignof(kept_build_state),
};

/* The computation number of the memo entries that link a copy to the node
   it copies (19.10). */
#define ACCUMULATOR_ORIGINAL_NUMBER UINT64_MAX

/* accumulator-link($copies, $originals): records that each node of
   $copies is a copy of the item at the same position of $originals, whose
   accumulator values it has (XSLT 4.0, 19.10: fn:copy-of, xsl:copy-of
   with copy-accumulators="yes"); returns $copies. */
static sxml_status call_accumulator_link(const sxml_function_call *call, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_sequence copies = sxml_function_library_get_argument(call, 0);
    const sxml_sequence originals = sxml_function_library_get_argument(call, 1);
    uint64_t copy_count = 0;
    uint64_t original_count = 0;
    sxml_status status = sxml_sequence_count(copies, NULL, &copy_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(originals, NULL, &original_count);
    }
    for (uint64_t item_index = 0;
         status == SXML_STATUS_OK && item_index < copy_count && item_index < original_count;
         item_index += 1) {
        const sxml_item_request request = {.item_index = item_index, .error_record = NULL};
        sxml_item copy = {.representation = 0};
        sxml_item original = {.representation = 0};
        status = sxml_sequence_get_item(copies, request, &copy);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(originals, request, &original);
        }
        if (status == SXML_STATUS_OK && sxml_item_get_kind(copy) == SXML_ITEM_KIND_NODE &&
            sxml_item_get_kind(original) == SXML_ITEM_KIND_NODE) {
            status = sxml_dynamic_context_store_memo(
                call->dynamic_context,
                &(sxml_evaluation_memo_entry){
                    .key = {.computation = &sxml_xslt_accumulator_functions,
                            .computation_number = ACCUMULATOR_ORIGINAL_NUMBER,
                            .subject = copy},
                    .value = sxml_sequence_from_item(original)});
        }
    }
    if (status == SXML_STATUS_OK) {
        sxml_sequence_retain(copies);
        *result = copies;
    }
    return status;
}

/* accumulator-original($node): the node $node is a copy of by
   accumulator-link, or the empty sequence. */
static sxml_status call_accumulator_original(const sxml_function_call *call,
                                             sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_sequence original = sxml_sequence_empty();
    if (sxml_dynamic_context_find_memo(
            call->dynamic_context,
            &(sxml_evaluation_memo_key){.computation = &sxml_xslt_accumulator_functions,
                                        .computation_number = ACCUMULATOR_ORIGINAL_NUMBER,
                                        .subject =
                                            sxml_function_library_get_argument(call, 0).content},
            &original)) {
        sxml_sequence_retain(original);
        *result = original;
        return SXML_STATUS_OK;
    }
    *result = sxml_sequence_empty();
    return SXML_STATUS_OK;
}

/* --------------------------------------- Accumulators of streamed nodes */

static const sxml_static_error accumulator_end_unread_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTSE3430"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "fn:accumulator-after is evaluated for a streamed node whose end has not been read: "
        "the construct is not guaranteed-streamable (XSLT 4.0, 19.8.9.1)"),
};

/* The entry of map under key (borrowed); found false when there is none or
   map is no map. */
typedef struct entry_lookup {
    sxml_item map;
    sxml_item key;
} entry_lookup;

static sxml_status find_entry(entry_lookup lookup, sxml_map_lookup *found) {
    found->is_found = false;
    if (lookup.map.representation == 0 || sxml_item_get_kind(lookup.map) != SXML_ITEM_KIND_MAP) {
        return SXML_STATUS_OK;
    }
    return sxml_map_find(lookup.map, lookup.key, found);
}

/* The map that is the single item of value, or no item. */
static sxml_item single_map(sxml_sequence value) {
    sxml_item item = {.representation = 0};
    if (sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
        sxml_item_get_kind(value.content) == SXML_ITEM_KIND_MAP) {
        item = value.content;
    }
    return item;
}

/* Raises what a value of number the node's maps leave out raises: XTDE3362
   when the accumulator is not applicable to the streamed document (19.2);
   the error of the rule that failed to compute it (19.5); else, while the
   document computes the node's values, XTDE3400 after asking for the
   accumulator to be computed after the others (it then runs again), and
   XTDE3400 when it is computed last and still reads itself (19.6). */
typedef struct missing_value {
    const sxml_function_call *call;
    const sxml_streamed_accumulator_values *values;
    sxml_item number;
} missing_value;

static sxml_status raise_missing_value(missing_value missing) {
    const sxml_function_call *call = missing.call;
    sxml_map_lookup applicable = {.is_found = false};
    sxml_status status = find_entry(
        (entry_lookup){.map = missing.values->applicable, .key = missing.number}, &applicable);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!applicable.is_found && missing.values->applicable.representation != 0) {
        return sxml_function_library_report(call, &accumulator_applicability_error);
    }
    if (missing.values->waiting != NULL) {
        *missing.values->waiting = true;
        return sxml_function_library_report(call, &accumulator_cycle_error);
    }
    if (missing.values->failure != NULL) {
        return sxml_item_heap_raise_error(call->item_heap, missing.values->failure,
                                          call->error_record);
    }
    return sxml_function_library_report(call, &accumulator_cycle_error);
}

/* The value of accumulator number for a node of a full stream window: in
   descended, the map from numbers to {"b": map, "a": map} by
   fn:generate-id (src/xslt_accumulator.c, descend-K). */
typedef struct descended_query {
    const sxml_function_call *call;
    const sxml_streamed_accumulator_values *values;
    sxml_item number;
    bool is_after;
} descended_query;

static sxml_status find_descended_value(descended_query query, sxml_sequence *value) {
    *value = sxml_sequence_empty();
    sxml_map_lookup found = {.is_found = false};
    sxml_status status = find_entry(
        (entry_lookup){.map = query.values->descended, .key = query.number}, &found);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!found.is_found) {
        return raise_missing_value(
            (missing_value){.call = query.call, .values = query.values, .number = query.number});
    }
    sxml_sequence phase_key = sxml_sequence_empty();
    status = sxml_function_library_make_string(query.call->item_heap,
                                               query.is_after ? SXML_BYTE_VIEW_FROM_LITERAL("a")
                                                              : SXML_BYTE_VIEW_FROM_LITERAL("b"),
                                               &phase_key);
    sxml_map_lookup phase = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = find_entry(
            (entry_lookup){.map = single_map(found.entry.value), .key = phase_key.content},
            &phase);
    }
    sxml_sequence_release(phase_key);
    sxml_map_lookup node = {.is_found = false};
    if (status == SXML_STATUS_OK && phase.is_found) {
        status = find_entry(
            (entry_lookup){.map = single_map(phase.entry.value),
                           .key = sxml_function_library_get_argument(query.call, 2).content},
            &node);
    }
    if (status == SXML_STATUS_OK && node.is_found) {
        *value = node.entry.value;
    }
    return status;
}

/* Whether the call of accumulator-streamed asks for the value after the
   node ($after). */
static bool is_after_requested(const sxml_function_call *call) {
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    return sxml_atomic_item_get_datum(sxml_function_library_get_argument(call, 3).content,
                                      &datum) == SXML_STATUS_OK &&
           datum.datum_kind == SXML_DATUM_KIND_BOOLEAN && datum.datum_details.boolean;
}

/* accumulator-streamed($number, $node, $id, $after): for a node of a
   streamed document whose accumulators are computed as its events pass
   (D231), {"v": the value of accumulator $number before or after the node},
   $id being the node's fn:generate-id; the empty sequence for any other
   node, whose values accumulator-values builds. */
static sxml_status call_accumulator_streamed(const sxml_function_call *call,
                                             sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_streamed_accumulator_values values;
    sxml_status status = sxml_evaluator_stream_find_accumulator_values(
        call->dynamic_context, sxml_function_library_get_argument(call, 1).content, &values);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!values.is_streamed) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    const bool is_after = is_after_requested(call);
    const sxml_item number = sxml_function_library_get_argument(call, 0).content;
    sxml_sequence value = sxml_sequence_empty();
    if (values.descended.representation != 0) {
        status = find_descended_value(
            (descended_query){
                .call = call, .values = &values, .number = number, .is_after = is_after},
            &value);
    } else {
        const sxml_item phase_values = is_after ? values.after : values.before;
        if (phase_values.representation == 0 && is_after && values.waiting == NULL &&
            values.failure == NULL) {
            return sxml_function_library_report(call, &accumulator_end_unread_error);
        }
        sxml_map_lookup found = {.is_found = false};
        status = find_entry((entry_lookup){.map = phase_values, .key = number}, &found);
        if (status == SXML_STATUS_OK && !found.is_found) {
            return raise_missing_value(
                (missing_value){.call = call, .values = &values, .number = number});
        }
        value = found.entry.value;
    }
    sxml_sequence value_key = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(call->item_heap, SXML_BYTE_VIEW_FROM_LITERAL("v"),
                                                   &value_key);
    }
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        const sxml_map_entry entry = {.key = value_key.content, .value = value};
        status = sxml_map_make(
            call->item_heap,
            &(sxml_map_request){.map_entries = {.map_entries = &entry, .map_entry_count = 1},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                                .error_record = NULL},
            &map);
    }
    sxml_sequence_release(value_key);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(map);
    }
    return status;
}

static const sxml_function_parameter accumulator_streamed_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("number"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:integer"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("node"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("node()"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("id"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:string"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("after"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
};

static const sxml_function_parameter accumulator_link_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("copies"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("originals"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
};

static const sxml_function_parameter accumulator_original_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("node"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("node()"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
};

static const sxml_function_parameter accumulator_name_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("name"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:anyAtomicType"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
};

static const sxml_function_parameter accumulator_scope_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("root"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("node()"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("numbers"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:integer*"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
};

static const sxml_function_parameter accumulator_values_parameters[] = {
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

/* In no namespace: only the texts of src/xslt_accumulator.c name them,
   through a table no stylesheet expression sees. */
static const sxml_native_function accumulator_functions[] = {
    {.function_namespace = SXML_NAMESPACE_NONE,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("accumulator-name"),
     .parameters = {accumulator_name_parameters, 1},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:QName"),
     .call = call_accumulator_name,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_NONE,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("accumulator-scope"),
     .parameters = {accumulator_scope_parameters, 2},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("empty-sequence()"),
     .call = call_accumulator_scope,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_NONE,
     .properties =
         SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT | SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("accumulator-values"),
     .parameters = {accumulator_values_parameters, 3},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("map(*)"),
     .call = NULL,
     .resumable_body = &accumulator_values_body},
    {.function_namespace = SXML_NAMESPACE_NONE,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("accumulator-link"),
     .parameters = {accumulator_link_parameters, 2},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
     .call = call_accumulator_link,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_NONE,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("accumulator-original"),
     .parameters = {accumulator_original_parameters, 1},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("node()?"),
     .call = call_accumulator_original,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_NONE,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("accumulator-streamed"),
     .parameters = {accumulator_streamed_parameters, 4},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("map(*)?"),
     .call = call_accumulator_streamed,
     .resumable_body = NULL},
};

const sxml_native_function_view sxml_xslt_accumulator_functions =
    SXML_LIBRARY_PART(accumulator_functions);
