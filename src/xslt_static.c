/*
 * Static expressions of XSLT 4.0 (src/xslt_static_internal.h; XSLT 4.0,
 * sections 9.6, 9.7, 3.11.3, 3.11.4; docs/DECISIONS.md D139).
 *
 * Golden file: src/xpath.c (an expression compiled through the front end
 * and lowering, then evaluated with a dynamic context of its own); the
 * XSLT functions are native rows as in src/function_library_format.c.
 */
#include "xslt_static_internal.h"

#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "function_library_internal.h"
#include "ir_internal.h"
#include "ir_pass_internal.h"
#include "processor_internal.h"
#include "xdm_internal.h"
#include "xpath_ast_internal.h"
#include "xpath_static_internal.h"
#include "xslt_value_template_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------- The XSLT functions */

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

static const sxml_function_table_operations static_function_table_operations;

/* The namespaces of the static expression being evaluated: the scope is
   the state of the dynamic context's function table (it is the table the
   expression was compiled and runs with). */
static sxml_namespace_binding_view evaluation_bindings_of(const sxml_function_call *call);

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
static sxml_status read_name_argument(name_argument_request request,
                                      sxml_name_identifier *name) {
    const sxml_function_call *call = request.call;
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_optional_item argument = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    if (!argument.has_item ||
        sxml_atomic_item_get_datum(argument.item, &datum) != SXML_STATUS_OK) {
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
    {SXML_BYTE_VIEW_INITIALIZER("supports-dynamic-evaluation"),
     SXML_BYTE_VIEW_INITIALIZER("no")},
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
    sxml_status status = read_name_argument(
        (name_argument_request){.call = call,
                                .role = SXML_NAME_ARGUMENT_ROLE_NO_NAMESPACE,
                                .error = &system_property_name_error},
        &name);
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(sxml_item_heap_get_name_pool(call->item_heap),
                                               name, &parts);
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
                &(sxml_atomic_item_request){.type = {.index = SXML_TYPE_XS_QNAME},
                                            .datum = datum},
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
static sxml_status call_function_available(const sxml_function_call *call,
                                           sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_name_identifier name = {.index = 0};
    sxml_status status = read_name_argument(
        (name_argument_request){.call = call,
                                .role = SXML_NAME_ARGUMENT_ROLE_FUNCTION,
                                .error = &function_name_error},
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
    for (size_t arity = first_arity; arity <= last_arity && !is_available && table.operations != NULL;
         arity += 1) {
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
static sxml_status call_element_available(const sxml_function_call *call,
                                          sxml_sequence *result) {
    sxml_name_identifier name = {.index = 0};
    sxml_status status = read_name_argument(
        (name_argument_request){.call = call,
                                .role = SXML_NAME_ARGUMENT_ROLE_ELEMENT_OR_TYPE,
                                .error = &element_name_error},
        &name);
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(sxml_item_heap_get_name_pool(call->item_heap),
                                               name, &parts);
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
    const sxml_status status = read_name_argument(
        (name_argument_request){.call = call,
                                .role = SXML_NAME_ARGUMENT_ROLE_ELEMENT_OR_TYPE,
                                .error = &type_name_error},
        &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_type_identifier type = {.index = 0};
    const bool is_available =
        sxml_type_registry_find_type(sxml_item_heap_get_type_registry(call->item_heap), name, &type);
    *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = is_available});
    return SXML_STATUS_OK;
}

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

/* The functions XSLT 4.0, 9.7 adds to static expressions. They read the
   running unit's namespaces and the function table: context-dependent. */
static const sxml_native_function static_functions[] = {
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
};

static const sxml_native_function_view static_function_part = {
    .native_functions = static_functions,
    .native_function_count = sizeof(static_functions) / sizeof(static_functions[0])};

static const sxml_native_function_view *const static_function_parts[] = {&static_function_part};

/* ---------------------------------------------------------------- Scope */

typedef struct static_variable_entry {
    sxml_name_identifier variable_name;
    /* Owned. */
    sxml_sequence value;
} static_variable_entry;

struct sxml_xslt_static_scope {
    sxml_processor *processor;
    sxml_resource_resolver resource_resolver;
    /* The allocator of create; the null allocator after initialize. */
    sxml_allocator creating_allocator;
    /* The XSLT functions; their table is searched before the
       processor's. */
    sxml_function_library *function_library;
    static_variable_entry *variables;
    size_t variable_count;
    size_t variable_capacity;
    /* The namespaces of the expression being evaluated, for the name
       arguments of the XSLT functions; borrowed for the evaluation. */
    sxml_namespace_binding_view evaluation_bindings;
};

/* The XSLT functions, then the processor's library. */
static sxml_status find_static_function(void *state, const sxml_function_query *query,
                                        sxml_function_entry *entry) {
    const sxml_xslt_static_scope *scope = state;
    sxml_status status = sxml_function_table_find(
        sxml_function_library_as_table(scope->function_library), query, entry);
    if (status != SXML_STATUS_OK || entry->entry_kind != SXML_FUNCTION_ENTRY_KIND_NONE) {
        return status;
    }
    return sxml_function_table_find(sxml_processor_get_function_table(scope->processor), query,
                                    entry);
}

static const sxml_function_table_operations static_function_table_operations = {
    .find_function = find_static_function};

static sxml_namespace_binding_view evaluation_bindings_of(const sxml_function_call *call) {
    if (call->dynamic_context == NULL ||
        call->dynamic_context->function_table.operations != &static_function_table_operations) {
        return (sxml_namespace_binding_view){.namespace_bindings = NULL,
                                             .namespace_binding_count = 0};
    }
    const sxml_xslt_static_scope *scope = call->dynamic_context->function_table.state;
    return scope->evaluation_bindings;
}

static sxml_function_table static_function_table(sxml_xslt_static_scope *scope) {
    return (sxml_function_table){.state = scope, .operations = &static_function_table_operations};
}

sxml_status sxml_xslt_static_scope_memory_requirements(
    const sxml_xslt_static_scope_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration == NULL || configuration->processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_xslt_static_scope),
                                               .alignment = alignof(sxml_xslt_static_scope)};
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_static_scope_initialize(
    sxml_memory_view memory, const sxml_xslt_static_scope_configuration *configuration,
    sxml_xslt_static_scope **scope) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_xslt_static_scope_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_processor *processor = configuration->processor;
    sxml_function_library *library = NULL;
    status = sxml_function_library_create(
        processor->allocator,
        &(sxml_function_library_configuration){
            .allocator = processor->allocator,
            .item_heap = processor->item_heap,
            .parts = {.native_function_views = static_function_parts,
                      .native_function_view_count = 1}},
        &library);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_xslt_static_scope *new_scope = placement;
    *new_scope = (sxml_xslt_static_scope){
        .processor = processor,
        .resource_resolver = configuration->resource_resolver,
        .creating_allocator = {.state = NULL, .operations = NULL},
        .function_library = library,
        .variables = NULL,
        .variable_count = 0,
        .variable_capacity = 0,
        .evaluation_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0}};
    *scope = new_scope;
    return SXML_STATUS_OK;
}

void sxml_xslt_static_scope_finalize(sxml_xslt_static_scope *scope) {
    if (scope == NULL) {
        return;
    }
    for (size_t variable_index = 0; variable_index < scope->variable_count; variable_index += 1) {
        sxml_sequence_release(scope->variables[variable_index].value);
    }
    sxml_allocator_release(scope->processor->allocator, scope->variables);
    sxml_function_library_destroy(scope->function_library);
    scope->variables = NULL;
    scope->variable_count = 0;
    scope->variable_capacity = 0;
    scope->function_library = NULL;
}

sxml_status sxml_xslt_static_scope_create(
    sxml_allocator allocator, const sxml_xslt_static_scope_configuration *configuration,
    sxml_xslt_static_scope **scope) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_xslt_static_scope_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = requirements.byte_count,
                                  .alignment = requirements.alignment},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_static_scope *new_scope = NULL;
    status = sxml_xslt_static_scope_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
        configuration, &new_scope);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_scope->creating_allocator = allocator;
    *scope = new_scope;
    return SXML_STATUS_OK;
}

void sxml_xslt_static_scope_destroy(sxml_xslt_static_scope *scope) {
    if (scope == NULL) {
        return;
    }
    const sxml_allocator allocator = scope->creating_allocator;
    sxml_xslt_static_scope_finalize(scope);
    sxml_allocator_release(allocator, scope);
}

sxml_status sxml_xslt_static_scope_add_variable(sxml_xslt_static_scope *scope,
                                                const sxml_xslt_static_variable *variable) {
    if (scope->variable_count == scope->variable_capacity) {
        const size_t new_capacity = scope->variable_capacity < 8 ? 8 : scope->variable_capacity * 2;
        size_t byte_count = 0;
        sxml_status status =
            sxml_size_multiply(new_capacity, sizeof(static_variable_entry), &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                scope->processor->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(static_variable_entry)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (scope->variable_count > 0) {
            memcpy(memory, scope->variables, scope->variable_count * sizeof(static_variable_entry));
        }
        sxml_allocator_release(scope->processor->allocator, scope->variables);
        scope->variables = memory;
        scope->variable_capacity = new_capacity;
    }
    sxml_sequence_retain(variable->value);
    scope->variables[scope->variable_count] =
        (static_variable_entry){.variable_name = variable->variable_name, .value = variable->value};
    scope->variable_count += 1;
    return SXML_STATUS_OK;
}

sxml_xslt_static_lookup sxml_xslt_static_scope_find_variable(const sxml_xslt_static_scope *scope,
                                                             sxml_name_identifier variable_name) {
    for (size_t entry_number = scope->variable_count; entry_number > 0; entry_number -= 1) {
        const static_variable_entry *entry = &scope->variables[entry_number - 1];
        if (entry->variable_name.index == variable_name.index) {
            return (sxml_xslt_static_lookup){.is_found = true, .value = entry->value};
        }
    }
    return (sxml_xslt_static_lookup){.is_found = false, .value = sxml_sequence_empty()};
}

/* ------------------------------------------------------------ Evaluation */

/* The variables in scope, each name once (the latest declaration), as
   declarations of type item()* and their bindings; both arrays owned. */
typedef struct scope_variables {
    sxml_variable_declaration *declarations;
    sxml_variable_binding *bindings;
    size_t variable_count;
} scope_variables;

static sxml_status collect_scope_variables(const sxml_xslt_static_scope *scope,
                                           scope_variables *variables) {
    *variables = (scope_variables){.declarations = NULL, .bindings = NULL, .variable_count = 0};
    if (scope->variable_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_allocator allocator = scope->processor->allocator;
    size_t declaration_bytes = 0;
    size_t binding_bytes = 0;
    sxml_status status = sxml_size_multiply(scope->variable_count,
                                            sizeof(sxml_variable_declaration), &declaration_bytes);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(scope->variable_count, sizeof(sxml_variable_binding),
                                    &binding_bytes);
    }
    void *declaration_memory = NULL;
    void *binding_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = declaration_bytes,
                                      .alignment = alignof(sxml_variable_declaration)},
            &declaration_memory);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = binding_bytes,
                                      .alignment = alignof(sxml_variable_binding)},
            &binding_memory);
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, declaration_memory);
        return status;
    }
    sxml_variable_declaration *declarations = declaration_memory;
    sxml_variable_binding *bindings = binding_memory;
    size_t count = 0;
    /* From the latest: a name already taken is hidden (9.6). */
    for (size_t entry_number = scope->variable_count; entry_number > 0; entry_number -= 1) {
        const static_variable_entry *entry = &scope->variables[entry_number - 1];
        bool is_hidden = false;
        for (size_t taken_index = 0; taken_index < count; taken_index += 1) {
            if (declarations[taken_index].variable_name.index == entry->variable_name.index) {
                is_hidden = true;
                break;
            }
        }
        if (is_hidden) {
            continue;
        }
        declarations[count] = (sxml_variable_declaration){
            .variable_name = entry->variable_name,
            .declared_type = {.item_type = {.index = SXML_TYPE_ITEM},
                              .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
        bindings[count] =
            (sxml_variable_binding){.variable_name = entry->variable_name, .value = entry->value};
        count += 1;
    }
    *variables = (scope_variables){
        .declarations = declarations, .bindings = bindings, .variable_count = count};
    return SXML_STATUS_OK;
}

static void release_scope_variables(const sxml_xslt_static_scope *scope,
                                    scope_variables *variables) {
    sxml_allocator_release(scope->processor->allocator, variables->declarations);
    sxml_allocator_release(scope->processor->allocator, variables->bindings);
    *variables = (scope_variables){.declarations = NULL, .bindings = NULL, .variable_count = 0};
}

/* An AST to lower and run in the scope. */
typedef struct static_run_request {
    /* Root XPATH; borrowed. */
    const sxml_ast *ast;
    sxml_namespace_binding_view namespace_bindings;
    sxml_byte_view static_base_uri;
    /* Variables in scope (declarations and their values). */
    sxml_variable_declaration_view declarations;
    sxml_variable_binding_view bindings;
    sxml_error_record *error_record;
} static_run_request;

/* Lowers and seals request->ast as sxml_xpath_compile does, then
   evaluates it as sxml_xpath_expression_evaluate does, with the scope's
   function table at both. */
static sxml_status run_static_ast(sxml_xslt_static_scope *scope, const static_run_request *request,
                                  sxml_sequence *value) {
    sxml_processor *processor = scope->processor;
    const sxml_xpath_static_context static_context = {
        .item_heap = processor->item_heap,
        .allocator = processor->allocator,
        .namespace_bindings = request->namespace_bindings,
        .variable_declarations = request->declarations,
        .function_table = static_function_table(scope),
        .static_base_uri = request->static_base_uri};
    sxml_ir_unit *unit = NULL;
    sxml_status status = sxml_xpath_lower(
        &(sxml_xpath_lowering_request){
            .ast = request->ast,
            .static_context = &static_context,
            .maximum_expression_count = 0,
            .maximum_expression_depth = sxml_processor_get_maximum_expression_depth(processor),
            .error_record = request->error_record},
        &unit);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_diagnostic diagnostic;
    memset(&diagnostic, 0, sizeof(diagnostic));
    status = sxml_ir_run_passes(&(sxml_ir_pass_list_request){
        .pass_request = {.unit = unit,
                         .type_registry = processor->type_registry,
                         .error_record = request->error_record,
                         .diagnostic = &diagnostic},
        .passes = sxml_ir_standard_passes,
        .verifies_after_each_pass = false});
    if (status != SXML_STATUS_OK) {
        sxml_ir_unit_release(unit);
        return status;
    }
    sxml_ir_unit_seal(unit);
    /* Global n of the unit is declaration n (sxml_xpath_lower). */
    const size_t global_count = request->declarations.variable_declaration_count;
    void *slot_memory = NULL;
    if (global_count > 0) {
        size_t byte_count = 0;
        status = sxml_size_multiply(global_count, sizeof(sxml_evaluator_slot), &byte_count);
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                processor->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(sxml_evaluator_slot)},
                &slot_memory);
        }
    }
    sxml_evaluator_slot *slots = slot_memory;
    for (size_t global_index = 0; status == SXML_STATUS_OK && global_index < global_count;
         global_index += 1) {
        slots[global_index] =
            (sxml_evaluator_slot){.value = request->bindings.variable_bindings[global_index].value,
                                  .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
    }
    sxml_dynamic_context *dynamic_context = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_create(
            processor->allocator,
            &(sxml_evaluator_configuration){
                .item_heap = processor->item_heap,
                .allocator = processor->allocator,
                /* 9.7 leaves available documents to the implementation:
                   static expressions read none (fn:doc raises FODC0002). */
                .document_loader = {.state = NULL, .operations = NULL},
                .clock = processor->clock,
                .function_table = static_function_table(scope),
                .maximum_evaluation_depth = sxml_processor_get_maximum_evaluation_depth(processor),
                .maximum_frame_slot_count = 0,
                .maximum_evaluation_step_count =
                    sxml_processor_get_maximum_evaluation_step_count(processor)},
            &dynamic_context);
    }
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        /* 9.7: the focus is absent. */
        status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){
                .unit = unit,
                .has_context_value = false,
                .context_value = sxml_sequence_empty(),
                .global_values = {.slots = slots, .slot_count = global_count},
                .static_base_uri = request->static_base_uri,
                .error_record = request->error_record},
            &result);
    }
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_allocator_release(processor->allocator, slot_memory);
    sxml_ir_unit_release(unit);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *value = result;
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_static_scope_evaluate(sxml_xslt_static_scope *scope,
                                            const sxml_xslt_static_expression *expression,
                                            sxml_sequence *value) {
    if (scope == NULL || expression == NULL || value == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_processor *processor = scope->processor;
    const sxml_ast_build_request build_request = {
        .expression_text = expression->expression_text,
        .source_name = expression->source_name,
        .allocator = processor->allocator,
        .maximum_expression_depth = sxml_processor_get_maximum_expression_depth(processor),
        .error_record = expression->error_record};
    sxml_ast *ast = NULL;
    /* A shadow attribute is a value template whose value is its string
       (3.11.4, 5.6): the AST is that of a string template. */
    sxml_status status = expression->is_value_template
                             ? sxml_xslt_build_value_template_ast(&build_request, &ast)
                             : sxml_xpath_build_ast(&build_request, &ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    scope_variables variables = {.declarations = NULL, .bindings = NULL, .variable_count = 0};
    status = collect_scope_variables(scope, &variables);
    scope->evaluation_bindings = expression->namespace_bindings;
    if (status == SXML_STATUS_OK) {
        status = run_static_ast(
            scope,
            &(static_run_request){
                .ast = ast,
                .namespace_bindings = expression->namespace_bindings,
                .static_base_uri = expression->static_base_uri,
                .declarations = {.variable_declarations = variables.declarations,
                                 .variable_declaration_count = variables.variable_count},
                .bindings = {.variable_bindings = variables.bindings,
                             .variable_binding_count = variables.variable_count},
                .error_record = expression->error_record},
            value);
    }
    scope->evaluation_bindings =
        (sxml_namespace_binding_view){.namespace_bindings = NULL, .namespace_binding_count = 0};
    release_scope_variables(scope, &variables);
    sxml_ast_destroy(ast);
    return status;
}

/* The private name of the value being coerced. */
#define STATIC_COERCION_NAMESPACE_URI "urn:x-sxml:static-coercion"

sxml_status sxml_xslt_static_scope_coerce(sxml_xslt_static_scope *scope,
                                          const sxml_xslt_static_coercion *coercion,
                                          sxml_sequence *value) {
    if (scope == NULL || coercion == NULL || value == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_processor *processor = scope->processor;
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_name_identifier name = {.index = 0};
    sxml_status status = sxml_name_pool_intern_namespace_uri(
        processor->name_pool, SXML_BYTE_VIEW_FROM_LITERAL(STATIC_COERCION_NAMESPACE_URI),
        &namespace_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            processor->name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri,
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("value")},
            &name);
    }
    sxml_ast *ast = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_build_ast(
            &(sxml_ast_build_request){
                .expression_text =
                    SXML_BYTE_VIEW_FROM_LITERAL("$Q{" STATIC_COERCION_NAMESPACE_URI "}value"),
                .source_name = {.bytes = NULL, .byte_count = 0},
                .allocator = processor->allocator,
                .maximum_expression_depth = 0,
                .error_record = NULL},
            &ast);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The binding is coerced to the declared type when the evaluation
       starts (sxml_evaluation_request.global_values). */
    const sxml_variable_declaration declaration = {.variable_name = name,
                                                   .declared_type = coercion->declared_type};
    const sxml_variable_binding binding = {.variable_name = name, .value = coercion->value};
    sxml_error_record record;
    memset(&record, 0, sizeof(record));
    status = run_static_ast(
        scope,
        &(static_run_request){
            .ast = ast,
            .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
            .static_base_uri = {.bytes = NULL, .byte_count = 0},
            .declarations = {.variable_declarations = &declaration,
                             .variable_declaration_count = 1},
            .bindings = {.variable_bindings = &binding, .variable_binding_count = 1},
            .error_record = &record},
        value);
    sxml_ast_destroy(ast);
    if (status == SXML_STATUS_TYPE_ERROR) {
        /* XPTY0004 from the binding is the variable's own code (9.3). */
        if (coercion->error_record != NULL) {
            *coercion->error_record = (sxml_error_record){
                .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                         .local_name = coercion->error_code,
                         .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
                .value = NULL,
                .diagnostic = {.description = SXML_BYTE_VIEW_FROM_LITERAL(
                                   "The value of a static variable or parameter does not "
                                   "match its declared type (XSLT 4.0, 9.3)"),
                               .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                            .line_number = 0,
                                            .column_number = 0,
                                            .byte_offset = 0}}};
        }
        return SXML_STATUS_TYPE_ERROR;
    }
    if (status != SXML_STATUS_OK && coercion->error_record != NULL) {
        *coercion->error_record = record;
    }
    return status;
}
