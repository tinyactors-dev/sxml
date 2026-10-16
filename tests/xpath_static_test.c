/*
 * Tests for static analysis and lowering of XPath 4.0
 * (src/xpath_static.c, src/xpath_lower.c, src/xpath_static_internal.h;
 * D46, D55, D63).
 *
 * Golden file: tests/memory_test.c. Expressions are compiled with
 * sxml_xpath_compile against a test function table (a few dozen rows
 * whose types are F&O text, resolved through sxml_xpath_resolve_sequence_
 * type as the function library does) and the unit is compared with golden
 * text in the form of sxml_ir_unit_write_text. Static errors are checked
 * by code and location. Every case checks that the item heap's cells and
 * the allocator's blocks are all released.
 */
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "xpath_static_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    alignas(16) uint8_t counting_storage[256];
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
} fixture;

static sxml_status fixture_create(uint64_t failing_allocation_number,
                                  fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL};
    sxml_status status = sxml_counting_allocator_initialize(
        (sxml_memory_view){.bytes = test_fixture->counting_storage,
                           .byte_count = sizeof(test_fixture->counting_storage)},
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator(),
            .failing_allocation_number = failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_create(
            sxml_default_allocator(),
            &(sxml_name_pool_configuration){.allocator = sxml_default_allocator(),
                                            .maximum_byte_count = 0},
            &test_fixture->name_pool);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_create(
            sxml_default_allocator(),
            &(sxml_type_registry_configuration){
                .allocator = sxml_default_allocator(),
                .name_pool = test_fixture->name_pool,
                .maximum_registered_type_count = 0},
            &test_fixture->registry);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_create(
            sxml_default_allocator(),
            &(sxml_item_heap_configuration){
                .allocator = sxml_default_allocator(),
                .name_pool = test_fixture->name_pool,
                .type_registry = test_fixture->registry,
                .maximum_byte_count = 0},
            &test_fixture->item_heap);
    }
    return status;
}

static void fixture_destroy(fixture *test_fixture) {
    sxml_item_heap_destroy(test_fixture->item_heap);
    sxml_type_registry_destroy(test_fixture->registry);
    sxml_name_pool_destroy(test_fixture->name_pool);
    sxml_counting_allocator_finalize(test_fixture->counting_allocator);
}

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static bool fixture_released_everything(const fixture *test_fixture) {
    return sxml_counting_allocator_read_counts(test_fixture->counting_allocator)
                   .live_allocation_count == 0 &&
           sxml_item_heap_read_counts(test_fixture->item_heap).live_cell_count ==
               0;
}

/* --------------------------------------------------- Function table */

#define PARAMETER(name_text, type_text, default_kind)                         \
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                 \
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER(type_text),                 \
     .parameter_default = (default_kind)}

#define REQUIRED SXML_PARAMETER_DEFAULT_REQUIRED
#define CONTEXT SXML_PARAMETER_DEFAULT_CONTEXT_VALUE
#define BODY SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY

static const sxml_function_parameter input_parameters[] = {
    PARAMETER("input", "item()*", REQUIRED)};
static const sxml_function_parameter two_input_parameters[] = {
    PARAMETER("input1", "item()*", REQUIRED),
    PARAMETER("input2", "item()*", REQUIRED),
    PARAMETER("options", "map(*)?", BODY)};
static const sxml_function_parameter numeric_parameters[] = {
    PARAMETER("value", "xs:numeric?", REQUIRED)};
static const sxml_function_parameter round_parameters[] = {
    PARAMETER("value", "xs:numeric?", REQUIRED),
    PARAMETER("precision", "xs:integer?", BODY),
    PARAMETER("mode", "enum('floor', 'ceiling')?", BODY)};
static const sxml_function_parameter node_parameters[] = {
    PARAMETER("node", "node()?", CONTEXT)};
static const sxml_function_parameter string_parameters[] = {
    PARAMETER("value", "item()?", CONTEXT)};
static const sxml_function_parameter number_parameters[] = {
    PARAMETER("value", "xs:anyAtomicType?", CONTEXT)};
static const sxml_function_parameter data_parameters[] = {
    PARAMETER("input", "item()*", CONTEXT)};
static const sxml_function_parameter filter_parameters[] = {
    PARAMETER("input", "item()*", REQUIRED),
    PARAMETER("predicate", "fn(item(), xs:integer) as xs:boolean?", REQUIRED)};
static const sxml_function_parameter for_each_parameters[] = {
    PARAMETER("input", "item()*", REQUIRED),
    PARAMETER("action", "fn(item(), xs:integer) as item()*", REQUIRED)};
static const sxml_function_parameter concat_parameters[] = {
    PARAMETER("values", "xs:anyAtomicType*", BODY)};
static const sxml_function_parameter partial_apply_parameters[] = {
    PARAMETER("function", "function(*)", REQUIRED),
    PARAMETER("arguments", "map(xs:positiveInteger, item()*)", REQUIRED)};
static const sxml_function_parameter string_join_parameters[] = {
    PARAMETER("values", "xs:anyAtomicType*", REQUIRED),
    PARAMETER("separator", "xs:string?", BODY)};
static const sxml_function_parameter substring_parameters[] = {
    PARAMETER("value", "xs:string?", REQUIRED),
    PARAMETER("start", "xs:double", REQUIRED),
    PARAMETER("length", "xs:double?", BODY)};
static const sxml_function_parameter starts_with_parameters[] = {
    PARAMETER("value", "xs:string?", REQUIRED),
    PARAMETER("substring", "xs:string?", REQUIRED),
    PARAMETER("collation", "xs:string?", BODY)};
static const sxml_function_parameter sum_parameters[] = {
    PARAMETER("values", "xs:anyAtomicType*", REQUIRED),
    PARAMETER("zero", "xs:anyAtomicType?", BODY)};
static const sxml_function_parameter subsequence_parameters[] = {
    PARAMETER("input", "item()*", REQUIRED),
    PARAMETER("start", "xs:double", REQUIRED),
    PARAMETER("length", "xs:double?", BODY)};
static const sxml_function_parameter qname_parameters[] = {
    PARAMETER("uri", "xs:string?", REQUIRED),
    PARAMETER("qname", "xs:string", REQUIRED)};
static const sxml_function_parameter function_parameters[] = {
    PARAMETER("function", "function(*)", REQUIRED)};
static const sxml_function_parameter operator_parameters[] = {
    PARAMETER("operator", "xs:string", REQUIRED)};
static const sxml_function_parameter jtree_parameters[] = {
    PARAMETER("input", "item()?", REQUIRED)};
static const sxml_function_parameter map_parameters[] = {
    PARAMETER("map", "map(*)", REQUIRED)};
static const sxml_function_parameter map_get_parameters[] = {
    PARAMETER("map", "map(*)", REQUIRED),
    PARAMETER("key", "xs:anyAtomicType", REQUIRED),
    PARAMETER("default", "item()*", BODY)};
static const sxml_function_parameter map_merge_parameters[] = {
    PARAMETER("maps", "map(*)*", REQUIRED),
    PARAMETER("options", "map(*)?", BODY)};
static const sxml_function_parameter array_parameters[] = {
    PARAMETER("array", "array(*)", REQUIRED)};

#define ROW(namespace_constant, name_text, parameter_array, result_text,     \
            property_bits)                                                    \
    {.function_namespace = (namespace_constant),                              \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                     \
     .parameters = {(parameter_array),                                        \
                    sizeof(parameter_array) / sizeof((parameter_array)[0])},  \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                  \
     .properties = (property_bits),                                           \
     .call = NULL}

#define FN SXML_NAMESPACE_FUNCTIONS
#define MAP SXML_NAMESPACE_MAP_FUNCTIONS
#define ARRAY SXML_NAMESPACE_ARRAY_FUNCTIONS

#define NULLARY_ROW(name_text, result_text, property_bits)                    \
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,                          \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                     \
     .parameters = {NULL, 0},                                                 \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                  \
     .properties = (property_bits),                                           \
     .call = NULL}

static const sxml_native_function test_rows[] = {
    NULLARY_ROW("true", "xs:boolean", 0),
    NULLARY_ROW("false", "xs:boolean", 0),
    NULLARY_ROW("current-date", "xs:date",
                SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT),
    NULLARY_ROW("position", "xs:integer",
                SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION),
    NULLARY_ROW("last", "xs:integer", SXML_NATIVE_FUNCTION_READS_CONTEXT_SIZE),
    ROW(FN, "boolean", input_parameters, "xs:boolean", 0),
    ROW(FN, "not", input_parameters, "xs:boolean", 0),
    ROW(FN, "count", input_parameters, "xs:integer", 0),
    ROW(FN, "reverse", input_parameters, "item()*", 0),
    ROW(FN, "deep-equal", two_input_parameters, "xs:boolean", 0),
    ROW(FN, "abs", numeric_parameters, "xs:numeric?", 0),
    ROW(FN, "floor", numeric_parameters, "xs:numeric?", 0),
    ROW(FN, "ceiling", numeric_parameters, "xs:numeric?", 0),
    ROW(FN, "round", round_parameters, "xs:numeric?", 0),
    ROW(FN, "root", node_parameters, "node()?", 0),
    ROW(FN, "local-name", node_parameters, "xs:string", 0),
    ROW(FN, "string", string_parameters, "xs:string", 0),
    ROW(FN, "number", number_parameters, "xs:double", 0),
    ROW(FN, "data", data_parameters, "xs:anyAtomicType*", 0),
    ROW(FN, "filter", filter_parameters, "item()*",
        SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS),
    ROW(FN, "for-each", for_each_parameters, "item()*",
        SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS),
    ROW(FN, "concat", concat_parameters, "xs:string",
        SXML_NATIVE_FUNCTION_IS_VARIADIC),
    ROW(FN, "partial-apply", partial_apply_parameters, "function(*)", 0),
    ROW(FN, "string-join", string_join_parameters, "xs:string", 0),
    ROW(FN, "substring", substring_parameters, "xs:string", 0),
    ROW(FN, "starts-with", starts_with_parameters, "xs:boolean", 0),
    ROW(FN, "sum", sum_parameters, "xs:anyAtomicType?", 0),
    ROW(FN, "subsequence", subsequence_parameters, "item()*", 0),
    ROW(FN, "QName", qname_parameters, "xs:QName", 0),
    ROW(FN, "function-name", function_parameters, "xs:QName?", 0),
    ROW(FN, "op", operator_parameters, "fn(item()*, item()*) as item()*", 0),
    ROW(FN, "jtree", jtree_parameters, "jnode()?", 0),
    ROW(MAP, "size", map_parameters, "xs:integer", 0),
    ROW(MAP, "get", map_get_parameters, "item()*", 0),
    ROW(MAP, "merge", map_merge_parameters, "map(*)", 0),
    ROW(MAP, "keys", map_parameters, "xs:anyAtomicType*", 0),
    ROW(ARRAY, "size", array_parameters, "xs:integer", 0),
};

#define TEST_ROW_COUNT (sizeof(test_rows) / sizeof(test_rows[0]))

static size_t row_parameter_count(size_t row_index) {
    return test_rows[row_index].parameters.function_parameter_count;
}

#define TEST_PARAMETER_LIMIT 12

typedef struct test_table {
    sxml_item_heap *item_heap;
    sxml_allocator allocator;
    sxml_name_identifier row_names[TEST_ROW_COUNT];
    bool is_resolved[TEST_ROW_COUNT];
    sxml_sequence_type parameter_types[TEST_ROW_COUNT][TEST_PARAMETER_LIMIT];
    sxml_sequence_type result_types[TEST_ROW_COUNT];
    /* For constructor functions: xs:anyAtomicType?. */
    sxml_sequence_type constructor_parameter_type;
} test_table;

/* Resolves type text the way the function library does (D55). */
typedef struct type_text_request {
    const test_table *table;
    sxml_byte_view type_text;
} type_text_request;

static sxml_status resolve_type_text(type_text_request request,
                                     sxml_sequence_type *sequence_type) {
    sxml_ast *ast = NULL;
    sxml_status status = sxml_xpath_build_sequence_type_ast(
        &(sxml_ast_build_request){.expression_text = request.type_text,
                                  .allocator = request.table->allocator},
        &ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_xpath_static_context static_context = {
        .item_heap = request.table->item_heap,
        .allocator = request.table->allocator};
    status = sxml_xpath_resolve_sequence_type(
        &(sxml_xpath_type_request){.ast = ast,
                                   .type_node = ast->root,
                                   .static_context = &static_context,
                                   .error_record = NULL},
        sequence_type);
    sxml_ast_destroy(ast);
    return status;
}

static sxml_status resolve_row(test_table *table, size_t row_index) {
    if (table->is_resolved[row_index]) {
        return SXML_STATUS_OK;
    }
    const sxml_native_function *row = &test_rows[row_index];
    const size_t parameter_count = row_parameter_count(row_index);
    sxml_status status = SXML_STATUS_OK;
    for (size_t parameter_index = 0;
         status == SXML_STATUS_OK && parameter_index < TEST_PARAMETER_LIMIT;
         parameter_index += 1) {
        if (parameter_count == 0) {
            break;
        }
        const size_t source_index = parameter_index < parameter_count
                                        ? parameter_index
                                        : parameter_count - 1;
        status = resolve_type_text(
            (type_text_request){
                .table = table,
                .type_text = row->parameters.function_parameters[source_index]
                                 .parameter_type},
            &table->parameter_types[row_index][parameter_index]);
    }
    if (status == SXML_STATUS_OK) {
        status = resolve_type_text(
            (type_text_request){.table = table, .type_text = row->result_type},
            &table->result_types[row_index]);
    }
    if (status == SXML_STATUS_OK) {
        table->is_resolved[row_index] = true;
    }
    return status;
}

static bool row_accepts(size_t row_index, size_t arity) {
    const sxml_native_function *row = &test_rows[row_index];
    const size_t parameter_count = row_parameter_count(row_index);
    size_t required_count = 0;
    for (size_t parameter_index = 0; parameter_index < parameter_count;
         parameter_index += 1) {
        if (row->parameters.function_parameters[parameter_index]
                .parameter_default == SXML_PARAMETER_DEFAULT_REQUIRED) {
            required_count += 1;
        }
    }
    if (arity < required_count || arity > TEST_PARAMETER_LIMIT) {
        return false;
    }
    return (row->properties & SXML_NATIVE_FUNCTION_IS_VARIADIC) != 0 ||
           arity <= parameter_count;
}

typedef struct signature_request {
    sxml_type_registry *registry;
    sxml_sequence_type_view parameter_types;
    sxml_sequence_type result_type;
} signature_request;

static sxml_status register_signature(signature_request request,
                                      sxml_type_identifier *signature) {
    return sxml_type_registry_register_item_type(
        request.registry,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_FUNCTION,
            .variety_details = {.function_type = {.is_any_function = false,
                                                  .parameter_types =
                                                      request.parameter_types,
                                                  .result_type =
                                                      request.result_type}}},
        signature);
}

static sxml_status find_constructor(test_table *table,
                                    const sxml_function_query *query,
                                    sxml_function_entry *entry) {
    sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(table->item_heap);
    sxml_type_identifier type = {0};
    sxml_type_description description = {0};
    if (query->arity != 1 ||
        !sxml_type_registry_find_type(registry, query->function_name, &type) ||
        sxml_type_registry_describe_type(registry, type, &description) !=
            SXML_STATUS_OK ||
        (description.variety != SXML_TYPE_VARIETY_ATOMIC &&
         description.variety != SXML_TYPE_VARIETY_LIST) ||
        type.index == SXML_TYPE_XS_ANY_ATOMIC_TYPE ||
        type.index == SXML_TYPE_XS_NOTATION) {
        return SXML_STATUS_OK;
    }
    sxml_type_identifier signature = {0};
    sxml_status status = register_signature(
        (signature_request){
            .registry = registry,
            .parameter_types = {.sequence_types = &table->constructor_parameter_type,
                                .sequence_type_count = 1},
            .result_type = {.item_type = type,
                            .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE}},
        &signature);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *entry = (sxml_function_entry){
        .entry_kind = SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR,
        .constructor_type = type,
        .signature = signature,
        .parameter_types = {.sequence_types = &table->constructor_parameter_type,
                            .sequence_type_count = 1},
        .result_type = {.item_type = type,
                        .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE}};
    return SXML_STATUS_OK;
}

static sxml_status find_test_function(void *state,
                                      const sxml_function_query *query,
                                      sxml_function_entry *entry) {
    test_table *table = state;
    *entry = (sxml_function_entry){.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    sxml_name_parts parts = {0};
    if (sxml_name_pool_get_name_parts(
            sxml_item_heap_get_name_pool(table->item_heap), query->function_name,
            &parts) == SXML_STATUS_OK &&
        parts.namespace_uri.index == SXML_NAMESPACE_XML_SCHEMA) {
        return find_constructor(table, query, entry);
    }
    for (size_t row_index = 0; row_index < TEST_ROW_COUNT; row_index += 1) {
        if (table->row_names[row_index].index != query->function_name.index ||
            !row_accepts(row_index, query->arity)) {
            continue;
        }
        sxml_status status = resolve_row(table, row_index);
        sxml_type_identifier signature = {0};
        const sxml_sequence_type_view parameter_types = {
            .sequence_types = table->parameter_types[row_index],
            .sequence_type_count = query->arity};
        if (status == SXML_STATUS_OK) {
            status = register_signature(
                (signature_request){
                    .registry = sxml_item_heap_get_type_registry(table->item_heap),
                    .parameter_types = parameter_types,
                    .result_type = table->result_types[row_index]},
                &signature);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        *entry = (sxml_function_entry){
            .entry_kind = SXML_FUNCTION_ENTRY_KIND_NATIVE,
            .native_function = &test_rows[row_index],
            .parameters = {.function_parameters =
                               test_rows[row_index].parameters.function_parameters,
                           .function_parameter_count =
                               row_parameter_count(row_index)},
            .signature = signature,
            .parameter_types = parameter_types,
            .result_type = table->result_types[row_index]};
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_OK;
}

static const sxml_function_table_operations test_table_operations = {
    .find_function = find_test_function,
};

static sxml_status test_table_initialize(const fixture *test_fixture,
                                         test_table *table) {
    memset(table, 0, sizeof(*table));
    table->item_heap = test_fixture->item_heap;
    table->allocator = sxml_default_allocator();
    table->constructor_parameter_type = (sxml_sequence_type){
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_ATOMIC_TYPE),
        .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE};
    for (size_t row_index = 0; row_index < TEST_ROW_COUNT; row_index += 1) {
        sxml_status status = sxml_name_pool_intern_name(
            test_fixture->name_pool,
            &(sxml_name_request){
                .namespace_uri = sxml_well_known_namespace_identifier(
                    test_rows[row_index].function_namespace),
                .local_name = test_rows[row_index].local_name},
            &table->row_names[row_index]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Compiling */

#define CASE_NAME_LIMIT 4

/* An expression to compile, with external variables (in no namespace) and
   namespace bindings given as text; prefix "" binds the default element
   namespace. */
typedef struct compile_case {
    const char *expression_text;
    const char *variable_names[CASE_NAME_LIMIT];
    const char *namespace_prefixes[CASE_NAME_LIMIT];
    const char *namespace_uris[CASE_NAME_LIMIT];
    uint32_t maximum_expression_depth;
} compile_case;

/* What compiling one expression gave: the status, the error record and,
   on success, the unit's text form. */
typedef struct compile_outcome {
    sxml_status status;
    sxml_error_record error_record;
    char text[6144];
    size_t text_byte_count;
} compile_outcome;

static sxml_byte_view text_view(const char *text) {
    return sxml_byte_view_from_c_string(text);
}

/* The declarations and bindings of a case, interned in the fixture's pool. */
typedef struct case_context {
    sxml_variable_declaration declarations[CASE_NAME_LIMIT];
    size_t declaration_count;
    sxml_namespace_binding bindings[CASE_NAME_LIMIT];
    size_t binding_count;
} case_context;

static sxml_status prepare_case_context(const fixture *test_fixture,
                                        const compile_case *test_case,
                                        case_context *context) {
    memset(context, 0, sizeof(*context));
    sxml_status status = SXML_STATUS_OK;
    for (size_t name_index = 0;
         status == SXML_STATUS_OK && name_index < CASE_NAME_LIMIT &&
         test_case->variable_names[name_index] != NULL;
         name_index += 1) {
        sxml_variable_declaration *declaration =
            &context->declarations[context->declaration_count];
        status = sxml_name_pool_intern_name(
            test_fixture->name_pool,
            &(sxml_name_request){
                .namespace_uri = {.index = SXML_NAMESPACE_NONE},
                .local_name = text_view(test_case->variable_names[name_index])},
            &declaration->variable_name);
        declaration->declared_type = (sxml_sequence_type){
            .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
        context->declaration_count += 1;
    }
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < CASE_NAME_LIMIT &&
         test_case->namespace_prefixes[binding_index] != NULL;
         binding_index += 1) {
        sxml_namespace_binding *binding =
            &context->bindings[context->binding_count];
        status = sxml_name_pool_intern_namespace_prefix(
            test_fixture->name_pool,
            text_view(test_case->namespace_prefixes[binding_index]),
            &binding->namespace_prefix);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_namespace_uri(
                test_fixture->name_pool,
                text_view(test_case->namespace_uris[binding_index]),
                &binding->namespace_uri);
        }
        context->binding_count += 1;
    }
    return status;
}

/* The fixture and table a case is compiled with. */
typedef struct compile_environment {
    fixture *test_fixture;
    test_table *table;
} compile_environment;

static void compile_with(compile_environment environment,
                         const compile_case *test_case,
                         compile_outcome *outcome) {
    fixture *test_fixture = environment.test_fixture;
    test_table *table = environment.table;
    outcome->error_record = (sxml_error_record){.value = NULL};
    outcome->text[0] = '\0';
    outcome->text_byte_count = 0;
    case_context context;
    outcome->status = prepare_case_context(test_fixture, test_case, &context);
    if (outcome->status != SXML_STATUS_OK) {
        return;
    }
    const sxml_xpath_static_context static_context = {
        .item_heap = test_fixture->item_heap,
        .allocator = fixture_allocator(test_fixture),
        .namespace_bindings = {.namespace_bindings = context.bindings,
                               .namespace_binding_count = context.binding_count},
        .variable_declarations = {.variable_declarations = context.declarations,
                                  .variable_declaration_count =
                                      context.declaration_count},
        .function_table = {.state = table, .operations = &test_table_operations},
    };
    sxml_ir_unit *unit = NULL;
    outcome->status = sxml_xpath_compile(
        &(sxml_xpath_compile_request){
            .expression_text = text_view(test_case->expression_text),
            .source_name = SXML_BYTE_VIEW_FROM_LITERAL("test.xpath"),
            .static_context = &static_context,
            .maximum_expression_depth = test_case->maximum_expression_depth,
            .error_record = &outcome->error_record},
        &unit);
    if (outcome->status != SXML_STATUS_OK) {
        return;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_ir_unit_write_text(
        unit,
        (sxml_memory_view){.bytes = (uint8_t *)outcome->text,
                           .byte_count = sizeof(outcome->text) - 1},
        &byte_count);
    if (status == SXML_STATUS_OK) {
        outcome->text[byte_count] = '\0';
        outcome->text_byte_count = byte_count;
    } else {
        outcome->status = status;
    }
    sxml_ir_unit_release(unit);
}

/* Compiles test_case and compares its text form with expected_text. */
static bool compiles_to(const compile_case *test_case,
                        const char *expected_text) {
    /* Static: the 64 KiB stack of the WebAssembly tests is for lowering. */
    static fixture test_fixture;
    static test_table table;
    if (fixture_create(0, &test_fixture) != SXML_STATUS_OK ||
        test_table_initialize(&test_fixture, &table) != SXML_STATUS_OK) {
        fixture_destroy(&test_fixture);
        return false;
    }
    static compile_outcome outcome;
    compile_with(
        (compile_environment){.test_fixture = &test_fixture, .table = &table},
        test_case, &outcome);
    const bool is_equal = outcome.status == SXML_STATUS_OK &&
                          strcmp(outcome.text, expected_text) == 0;
    if (!is_equal) {
        (void)fprintf(stderr, "for %s\nstatus %d, %.*s\nexpected:\n%s\ngot:\n%s\n",
                      test_case->expression_text, (int)outcome.status,
                      (int)outcome.error_record.diagnostic.description.byte_count,
                      (const char *)outcome.error_record.diagnostic.description.bytes,
                      expected_text, outcome.text);
    }
    const bool is_clean = fixture_released_everything(&test_fixture);
    fixture_destroy(&test_fixture);
    return is_equal && is_clean;
}

/* A static error expected at a line and column. */
typedef struct expected_error {
    const char *code;
    uint32_t line_number;
    uint32_t column_number;
} expected_error;

static bool fails_with(const compile_case *test_case, expected_error expected) {
    /* Static: the 64 KiB stack of the WebAssembly tests is for lowering. */
    static fixture test_fixture;
    static test_table table;
    if (fixture_create(0, &test_fixture) != SXML_STATUS_OK ||
        test_table_initialize(&test_fixture, &table) != SXML_STATUS_OK) {
        fixture_destroy(&test_fixture);
        return false;
    }
    static compile_outcome outcome;
    compile_with(
        (compile_environment){.test_fixture = &test_fixture, .table = &table},
        test_case, &outcome);
    const sxml_error_record *record = &outcome.error_record;
    const bool is_expected =
        outcome.status == SXML_STATUS_STATIC_ERROR &&
        sxml_byte_view_equals(record->code.local_name,
                              sxml_byte_view_from_c_string(expected.code)) &&
        sxml_byte_view_equals(record->code.namespace_uri,
                              SXML_BYTE_VIEW_FROM_LITERAL(
                                  SXML_ERROR_NAMESPACE_URI)) &&
        record->diagnostic.location.line_number == expected.line_number &&
        record->diagnostic.location.column_number == expected.column_number &&
        sxml_byte_view_equals(record->diagnostic.location.source_name,
                              SXML_BYTE_VIEW_FROM_LITERAL("test.xpath"));
    if (!is_expected) {
        (void)fprintf(stderr, "for %s: status %d, code %.*s at %u:%u\n%s\n",
                      test_case->expression_text, (int)outcome.status,
                      (int)record->code.local_name.byte_count,
                      (const char *)record->code.local_name.bytes,
                      record->diagnostic.location.line_number,
                      record->diagnostic.location.column_number, outcome.text);
    }
    const bool is_clean = fixture_released_everything(&test_fixture);
    fixture_destroy(&test_fixture);
    return is_expected && is_clean;
}

static compile_case plain(const char *expression_text) {
    return (compile_case){.expression_text = expression_text};
}

/* ------------------------------------------------------- Golden dumps */

/* The IR of src/ir_internal.h, Example 2. */
static void test_absolute_path_lowers_to_root_treat_and_maps(
    sxml_test_state *state) {
    const compile_case test_case = plain("/a/b[2]");
    SXML_TEST_EXPECT(
        state,
        compiles_to(
            &test_case,
            "(function 1 main (parameters 0) (captures) (slots 10) (focus $1 _ _)\n"
            "  (let $2 (step $1 self node())\n"
            "    (let $3 (static-call fn:root#1 $2)\n"
            "      (let $4 (treat $3 document-node())\n"
            "        (let $10 (map $4 (focus $5 _ _)\n"
            "                   (let $6 (step $5 child element(a))\n"
            "                     (let $9 (map $6 (focus $7 _ _)\n"
            "                               (let $8 (step $7 child element(b))\n"
            "                                 (filter $8 (focus _ _ _)\n"
            "                                   2)))\n"
            "                       (document-order $9))))\n"
            "          (document-order $10))))))\n"));
}

/* A slice query and the text its unit must have. */
typedef struct golden_case {
    const char *test_case_name;
    const char *expression_text;
    const char *expected_text;
} golden_case;

static const golden_case golden_cases[] = {
    {"K-NumericAdd-29",
     "1+1 eq 2",
     "(function 1 main (parameters 0) (captures) (slots 1)\n"
     "  (let $1 (binary add 1 1)\n"
     "    (binary value-equal $1 2)))\n"},
    {"op-numeric-addint2args-1",
     "xs:int(\"-1873914410\") + xs:int(\"-273569238\")",
     "(function 1 main (parameters 0) (captures) (slots 2)\n"
     "  (let $1 (cast \"-1873914410\" xs:int?)\n"
     "    (let $2 (cast \"-273569238\" xs:int?)\n"
     "      (binary add $1 $2))))\n"},
    {"K-ValCompTypeChecking-1",
     "(1, 2, 3) eq 3",
     "(function 1 main (parameters 0) (captures) (slots 1)\n"
     "  (let $1 (sequence 1 2 3)\n"
     "    (binary value-equal $1 3)))\n"},
    {"value-comp-ne-string-15",
     "not(/works/@iddddd ne \"\")",
     "(function 1 main (parameters 0) (captures) (slots 11) (focus $1 _ _)\n"
     "  (let $2 (step $1 self node())\n"
     "    (let $3 (static-call fn:root#1 $2)\n"
     "      (let $4 (treat $3 document-node())\n"
     "        (let $9 (map $4 (focus $5 _ _)\n"
     "                  (let $6 (step $5 child element(works))\n"
     "                    (let $8 (map $6 (focus $7 _ _)\n"
     "                              (step $7 attribute attribute(iddddd)))\n"
     "                      (document-order $8))))\n"
     "          (let $10 (document-order $9)\n"
     "            (let $11 (binary value-not-equal $10 \"\")\n"
     "              (static-call fn:not#1 $11))))))))\n"},
    {"PathExpr-21",
     "/count(bid)",
     "(function 1 main (parameters 0) (captures) (slots 7) (focus $1 _ _)\n"
     "  (let $2 (step $1 self node())\n"
     "    (let $3 (static-call fn:root#1 $2)\n"
     "      (let $4 (treat $3 document-node())\n"
     "        (let $7 (map $4 (focus $5 _ _)\n"
     "                  (let $6 (step $5 child element(bid))\n"
     "                    (static-call fn:count#1 $6)))\n"
     "          (document-order $7))))))\n"},
    {"Axes001-1",
     "fn:count(//center/child::*)",
     "(function 1 main (parameters 0) (captures) (slots 13) (focus $1 _ _)\n"
     "  (let $2 (step $1 self node())\n"
     "    (let $3 (static-call fn:root#1 $2)\n"
     "      (let $4 (treat $3 document-node())\n"
     "        (let $6 (map $4 (focus $5 _ _)\n"
     "                  (step $5 descendant-or-self node()))\n"
     "          (let $7 (document-order $6)\n"
     "            (let $12 (map $7 (focus $8 _ _)\n"
     "                       (let $9 (step $8 child element(center))\n"
     "                         (let $11 (map $9 (focus $10 _ _)\n"
     "                                    (step $10 child element()))\n"
     "                           (document-order $11))))\n"
     "              (let $13 (document-order $12)\n"
     "                (static-call fn:count#1 $13)))))))))\n"},
    {"filterexpressionhc2",
     "((1 to 25)[. mod 2 eq 0])",
     "(function 1 main (parameters 0) (captures) (slots 3)\n"
     "  (let $1 (binary range 1 25)\n"
     "    (filter $1 (focus $2 _ _)\n"
     "      (let $3 (binary modulo $2 2)\n"
     "        (binary value-equal $3 0)))))\n"},
    {"K-FilterExpr-64",
     "deep-equal((0, 1, 2), (0, 1, 2)[position() eq position()])",
     "(function 1 main (parameters 0) (captures) (slots 4)\n"
     "  (let $1 (sequence 0 1 2)\n"
     "    (let $2 (sequence 0 1 2)\n"
     "      (let $4 (filter $2 (focus _ $3 _)\n"
     "                (binary value-equal $3 $3))\n"
     "        (static-call fn:deep-equal#2 $1 $4)))))\n"},
    {"bang-13",
     "/ works ! employee[4] ! preceding-sibling::*[1] ! string(@name)",
     "(function 1 main (parameters 0) (captures) (slots 17) (focus $1 _ _)\n"
     "  (let $2 (step $1 self node())\n"
     "    (let $3 (static-call fn:root#1 $2)\n"
     "      (let $4 (treat $3 document-node())\n"
     "        (let $6 (map $4 (focus $5 _ _)\n"
     "                  (step $5 child element(works)))\n"
     "          (let $7 (document-order $6)\n"
     "            (let $10 (map $7 (focus $8 _ _)\n"
     "                       (let $9 (step $8 child element(employee))\n"
     "                         (filter $9 (focus _ _ _)\n"
     "                           4)))\n"
     "              (let $14 (map $10 (focus $11 _ _)\n"
     "                         (let $12 (step $11 preceding-sibling element())\n"
     "                           (let $13 (filter $12 (focus _ _ _)\n"
     "                                      1)\n"
     "                             (document-order $13))))\n"
     "                (map $14 (focus $15 _ _)\n"
     "                  (let $16 (step $15 attribute attribute(name))\n"
     "                    (let $17 (coerce $16 item()? function-argument)\n"
     "                      (static-call fn:string#1 $17))))))))))))\n"},
    {"quantExpr-1",
     "some $x in (1, 2) satisfies $x + $x = 3",
     "(function 1 main (parameters 0) (captures) (slots 3)\n"
     "  (let $1 (sequence 1 2)\n"
     "    (quantified some $2 $1\n"
     "      (let $3 (binary add $2 $2)\n"
     "        (binary general-equal $3 3)))))\n"},
    {"treat-as-1",
     "let $var := (100+200) div 2 return fn:concat($var treat as xs:string,\"a string\")",
     "(function 1 main (parameters 0) (captures) (slots 4)\n"
     "  (let $1 (binary add 100 200)\n"
     "    (let $2 (binary divide $1 2)\n"
     "      (let $3 (treat $2 xs:string)\n"
     "        (let $4 (coerce $3 xs:anyAtomicType* function-argument)\n"
     "          (static-call fn:concat#2 $4 \"a string\"))))))\n"},
    {"inline-fn-004",
     "sum(for $i in 1 to 6 return let $add := function($x){$x + $i} return $add(4))",
     "(function 1 main (parameters 0) (captures) (slots 5)\n"
     "  (let $1 (binary range 1 6)\n"
     "    (let $4 (map $1 (focus $2 _ _)\n"
     "              (let $3 (function-definition 2)\n"
     "                (dynamic-call $3 4)))\n"
     "      (let $5 (coerce $4 xs:anyAtomicType* function-argument)\n"
     "        (static-call fn:sum#1 $5)))))\n"
     "(function 2 anonymous (parameters 1) (captures $2) (slots 2)\n"
     "  (binary add $1 $2))\n"},
    {"inline-fn-005",
     "(1 to 4) ! (let $add := function($x){$x + .} return $add(4))",
     "(function 1 main (parameters 0) (captures) (slots 2)\n"
     "  (let $1 (binary range 1 4)\n"
     "    (map $1 (focus _ _ _)\n"
     "      (let $2 (function-definition 2)\n"
     "        (dynamic-call $2 4)))))\n"
     "(function 2 anonymous (parameters 1) (captures) (slots 2)\n"
     "  (let $2 (raise #err:XPDY0002 \"the focus is absent in a function body\")\n"
     "    (binary add $1 $2)))\n"},
    {"inline-fn-008",
     "let $outer := function($x) { let $inner := function($y) {$y + $x} return $inner(5) } return $outer(3)",
     "(function 1 main (parameters 0) (captures) (slots 1)\n"
     "  (let $1 (function-definition 2)\n"
     "    (dynamic-call $1 3)))\n"
     "(function 2 anonymous (parameters 1) (captures) (slots 2)\n"
     "  (let $2 (function-definition 3)\n"
     "    (dynamic-call $2 5)))\n"
     "(function 3 anonymous (parameters 1) (captures $1) (slots 2)\n"
     "  (binary add $1 $2))\n"},
    {"inline-fn-013",
     "let $add := function($x, $y) as xs:double {$x + $y} return $add(3, 4)",
     "(function 1 main (parameters 0) (captures) (slots 1)\n"
     "  (let $1 (function-definition 2)\n"
     "    (dynamic-call $1 3 4)))\n"
     "(function 2 anonymous (parameters 2) (captures) (slots 3)\n"
     "  (let $3 (binary add $1 $2)\n"
     "    (coerce $3 xs:double function-result)))\n"},
    {"DynamicFunctionCall-143",
     "let $f := (abs#1, round#1, floor#1, ceiling#1) return $f(23.7)",
     "(function 1 main (parameters 0) (captures) (slots 5)\n"
     "  (let $1 (function-reference fn:abs#1)\n"
     "    (let $2 (function-reference fn:round#1)\n"
     "      (let $3 (function-reference fn:floor#1)\n"
     "        (let $4 (function-reference fn:ceiling#1)\n"
     "          (let $5 (sequence $1 $2 $3 $4)\n"
     "            (dynamic-call $5 23.7)))))))\n"},
    {"ArrowPostfix-108",
     "let $usd := \"$\" => concat(?) return $usd(3)",
     "(function 1 main (parameters 0) (captures) (slots 1)\n"
     "  (let $1 (function-definition 2)\n"
     "    (dynamic-call $1 3)))\n"
     "(function 2 anonymous (parameters 1) (captures) (slots 1)\n"
     "  (static-call fn:concat#2 \"$\" $1))\n"},
    {"hof-920",
     "let $m := map { 'a': 1 } return $m(?) instance of map(*)",
     "(function 1 main (parameters 0) (captures) (slots 3)\n"
     "  (let $1 (map-constructor (\"a\" 1))\n"
     "    (let $2 (map-constructor)\n"
     "      (let $3 (static-call fn:partial-apply#2 $1 $2)\n"
     "        (instance-of $3 map(*))))))\n"},
    {"filter-001",
     "filter((\"apple\", \"pear\", \"apricot\", \"advocado\", \"orange\"),starts-with(?, \"a\"))",
     "(function 1 main (parameters 0) (captures) (slots 3)\n"
     "  (let $1 (sequence \"apple\" \"pear\" \"apricot\" \"advocado\" \"orange\")\n"
     "    (let $2 (function-definition 2)\n"
     "      (let $3 (coerce $2 function(item(), xs:integer) as xs:boolean? function-argument)\n"
     "        (static-call fn:filter#2 $1 $3)))))\n"
     "(function 2 anonymous (parameters 1) (captures) (slots 1)\n"
     "  (static-call fn:starts-with#2 $1 \"a\"))\n"},
    {"Lookup-159",
     "map{'decimal':true(), 'integer':true(), 'polygon':[11,22,33]}?polygon?2",
     "(function 1 main (parameters 0) (captures) (slots 5)\n"
     "  (let $1 (static-call fn:true#0)\n"
     "    (let $2 (static-call fn:true#0)\n"
     "      (let $3 (array-constructor square 11 22 33)\n"
     "        (let $4 (map-constructor (\"decimal\" $1) (\"integer\" $2) (\"polygon\" $3))\n"
     "          (let $5 (lookup $4 \"polygon\")\n"
     "            (lookup $5 2)))))))\n"},
    {"UnaryLookup-023",
     "[['a', 'b', 'c'], ['b', 'c', 'd'], ['e', 'f', 'b']]!?*!?1",
     "(function 1 main (parameters 0) (captures) (slots 7)\n"
     "  (let $1 (array-constructor square \"a\" \"b\" \"c\")\n"
     "    (let $2 (array-constructor square \"b\" \"c\" \"d\")\n"
     "      (let $3 (array-constructor square \"e\" \"f\" \"b\")\n"
     "        (let $4 (array-constructor square $1 $2 $3)\n"
     "          (let $6 (map $4 (focus $5 _ _)\n"
     "                    (lookup $5 *))\n"
     "            (map $6 (focus $7 _ _)\n"
     "              (lookup $7 1))))))))\n"},
    {"function-item-9",
     "function-name(let $a := 92, $b := true() return function($c) { $a, $b, $c })",
     "(function 1 main (parameters 0) (captures) (slots 3)\n"
     "  (let $1 (static-call fn:true#0)\n"
     "    (let $2 (function-definition 2)\n"
     "      (let $3 (coerce $2 function(*) function-argument)\n"
     "        (static-call fn:function-name#1 $3)))))\n"
     "(function 2 anonymous (parameters 1) (captures $1) (slots 2)\n"
     "  (sequence 92 $2 $1))\n"},
    {"casthc31",
     "xs:dateTime(\"1999-05-31T13:20:00-05:00\") cast as xs:date",
     "(function 1 main (parameters 0) (captures) (slots 1)\n"
     "  (let $1 (cast \"1999-05-31T13:20:00-05:00\" xs:dateTime?)\n"
     "    (cast $1 xs:date)))\n"},
    {"rangeExpr-31",
     "((28446744073709551616 to 28446744073709551620)!last())[1]",
     "(function 1 main (parameters 0) (captures) (slots 3)\n"
     "  (let $1 (binary range 28446744073709551616 28446744073709551620)\n"
     "    (let $3 (map $1 (focus _ _ $2)\n"
     "              $2)\n"
     "      (filter $3 (focus _ _ _)\n"
     "        1))))\n"},
    {"map-merge-401",
     "map:merge(({17:0}, {29:0}, {'a':0}, {-234:0}, {86:0}, {445:0}, {101:0}, {'e':0})) => map:keys()",
     "(function 1 main (parameters 0) (captures) (slots 13)\n"
     "  (let $1 (map-constructor (17 0))\n"
     "    (let $2 (map-constructor (29 0))\n"
     "      (let $3 (map-constructor (\"a\" 0))\n"
     "        (let $4 (unary minus 234)\n"
     "          (let $5 (map-constructor ($4 0))\n"
     "            (let $6 (map-constructor (86 0))\n"
     "              (let $7 (map-constructor (445 0))\n"
     "                (let $8 (map-constructor (101 0))\n"
     "                  (let $9 (map-constructor (\"e\" 0))\n"
     "                    (let $10 (sequence $1 $2 $3 $5 $6 $7 $8 $9)\n"
     "                      (let $11 (coerce $10 map(*)* function-argument)\n"
     "                        (let $12 (static-call map:merge#1 $11)\n"
     "                          (let $13 (coerce $12 map(*) function-argument)\n"
     "                            (static-call map:keys#1 $13)))))))))))))))\n"},
    {"CurlyArray-004",
     "array:size(array{(), ('A', 'B', 'C'), 'D'})",
     "(function 1 main (parameters 0) (captures) (slots 5)\n"
     "  (let $1 (sequence)\n"
     "    (let $2 (sequence \"A\" \"B\" \"C\")\n"
     "      (let $3 (sequence $1 $2 \"D\")\n"
     "        (let $4 (array-constructor curly $3)\n"
     "          (let $5 (coerce $4 array(*) function-argument)\n"
     "            (static-call array:size#1 $5)))))))\n"},
    {"fn-root-29",
     "let $j := jtree([1,2,3]) return $j is root($j/*[3])",
     "(function 1 main (parameters 0) (captures) (slots 9)\n"
     "  (let $1 (array-constructor square 1 2 3)\n"
     "    (let $2 (coerce $1 item()? function-argument)\n"
     "      (let $3 (static-call fn:jtree#1 $2)\n"
     "        (let $6 (map $3 (focus $4 _ _)\n"
     "                  (let $5 (step $4 child element())\n"
     "                    (filter $5 (focus _ _ _)\n"
     "                      3)))\n"
     "          (let $7 (document-order $6)\n"
     "            (let $8 (coerce $7 node()? function-argument)\n"
     "              (let $9 (static-call fn:root#1 $8)\n"
     "                (binary is $3 $9)))))))))\n"},
    {"casthc13",
     "xs:double(1267.43233E12) cast as xs:string",
     "(function 1 main (parameters 0) (captures) (slots 1)\n"
     "  (let $1 (cast 1267432330000000e0 xs:double?)\n"
     "    (cast $1 xs:string)))\n"},
};

static void test_slice_queries_match_golden_text(sxml_test_state *state) {
    for (size_t case_index = 0;
         case_index < sizeof(golden_cases) / sizeof(golden_cases[0]);
         case_index += 1) {
        const compile_case test_case = {
            .expression_text = golden_cases[case_index].expression_text};
        SXML_TEST_EXPECT(state,
                         compiles_to(&test_case,
                                     golden_cases[case_index].expected_text));
    }
}

/* Constructs beyond the slice queries: a case and its text. */
typedef struct feature_case {
    compile_case compile;
    const char *expected_text;
} feature_case;

static const feature_case feature_cases[] = {
    {.compile = {.expression_text = "$x + 1",
                 .variable_names = {"x"}},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 0)\n"
         "  (binary add $global:1 1))\n"},
    {.compile = {.expression_text = "let $f := function($y) { $y + $x } return $f(1)",
                 .variable_names = {"x"}},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (function-definition 2)\n"
         "    (dynamic-call $1 1)))\n"
         "(function 2 anonymous (parameters 1) (captures $global:1) (slots 2)\n"
         "  (binary add $1 $2))\n"},
    {.compile = {.expression_text = "string()"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 2) (focus $1 _ _)\n"
         "  (let $2 (coerce $1 item()? function-argument)\n"
         "    (static-call fn:string#1 $2)))\n"},
    {.compile = {.expression_text = "string#0"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 2) (focus $1 _ _)\n"
         "  (let $2 (coerce $1 item()? function-argument)\n"
         "    (function-definition 2)))\n"
         "(function 2 anonymous (parameters 0) (captures $2) (slots 1)\n"
         "  (static-call fn:string#1 $1))\n"},
    {.compile = {.expression_text = "position#0"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1) (focus _ $1 _)\n"
         "  (function-definition 2))\n"
         "(function 2 anonymous (parameters 0) (captures $1) (slots 1)\n"
         "  $1)\n"},
    {.compile = {.expression_text = "function() { position() }"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 0)\n"
         "  (function-definition 2))\n"
         "(function 2 anonymous (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (raise #err:XPDY0002 \"the focus is absent in a function body\")\n"
         "    $1))\n"},
    {.compile = {.expression_text = "fn { . + 1 }"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 0)\n"
         "  (function-definition 2))\n"
         "(function 2 anonymous (parameters 1) (captures) (slots 1)\n"
         "  (binary add $1 1))\n"},
    {.compile = {.expression_text = "substring(value := 'abc', start := 2)"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (coerce 2 xs:double function-argument)\n"
         "    (static-call fn:substring#2 \"abc\" $1)))\n"},
    {.compile = {.expression_text = "substring('abc', length := 1, start := 2)"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 2)\n"
         "  (let $1 (coerce 2 xs:double function-argument)\n"
         "    (let $2 (coerce 1 xs:double? function-argument)\n"
         "      (static-call fn:substring#3 \"abc\" $1 $2))))\n"},
    {.compile = {.expression_text = "1 or 2"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 0)\n"
         "  (conditional 1\n"
         "    true()\n"
         "    (conditional 2\n"
         "      true()\n"
         "      false())))\n"},
    {.compile = {.expression_text = "1 and 2"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 0)\n"
         "  (conditional 1\n"
         "    (conditional 2\n"
         "      true()\n"
         "      false())\n"
         "    false()))\n"},
    {.compile = {.expression_text = "() otherwise 3"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 2)\n"
         "  (let $1 (sequence)\n"
         "    (let $2 (instance-of $1 item()+)\n"
         "      (conditional $2\n"
         "        $1\n"
         "        3))))\n"},
    {.compile = {.expression_text = "if (1) { 2 }"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 0)\n"
         "  (conditional 1\n"
         "    2\n"
         "    (sequence)))\n"},
    {.compile = {.expression_text = "5 -> (. + 1)"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 0)\n"
         "  (binary add 5 1))\n"},
    {.compile = {.expression_text = "(1, 2) =!> abs()"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 3)\n"
         "  (let $1 (sequence 1 2)\n"
         "    (map $1 (focus $2 _ _)\n"
         "      (let $3 (coerce $2 xs:numeric? function-argument)\n"
         "        (static-call fn:abs#1 $3)))))\n"},
    {.compile = {.expression_text = "(1, 2) => count()"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (sequence 1 2)\n"
         "    (static-call fn:count#1 $1)))\n"},
    {.compile = {.expression_text = "`a{1}b`"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 3)\n"
         "  (let $1 (static-call fn:string-join#2 1 \" \")\n"
         "    (let $2 (binary concatenate \"a\" $1)\n"
         "      (let $3 (binary concatenate $2 \"b\")\n"
         "        $3))))\n"},
    {.compile = {.expression_text = "#p:local",
                 .namespace_prefixes = {"p"},
                 .namespace_uris = {"urn:p"}},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 0)\n"
         "  #Q{urn:p}local)\n"},
    {.compile = {.expression_text = "0x1F + 0b101 + 12345678901234567890"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (binary add 31 5)\n"
         "    (binary add $1 12345678901234567890)))\n"},
    {.compile = {.expression_text = "1.5e-3 + 2.50 + 0.000"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (binary add 0.0015e0 2.5)\n"
         "    (binary add $1 0.0)))\n"},
    {.compile = {.expression_text = "?(1 + 1)"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 2) (focus $1 _ _)\n"
         "  (let $2 (binary add 1 1)\n"
         "    (lookup $1 $2)))\n"},
    {.compile = {.expression_text = "for $x at $i in (1, 2) return $x * $i"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 3)\n"
         "  (let $1 (sequence 1 2)\n"
         "    (map $1 (focus $2 $3 _)\n"
         "      (binary multiply $2 $3))))\n"},
    {.compile = {.expression_text = "for $x as xs:integer in (1, 2), $y in $x return $y"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 4)\n"
         "  (let $1 (sequence 1 2)\n"
         "    (map $1 (focus $2 _ _)\n"
         "      (let $3 (coerce $2 xs:integer variable)\n"
         "        (map $3 (focus $4 _ _)\n"
         "          $4)))))\n"},
    {.compile = {.expression_text = "every $a in (1, 2), $b in (3, 4) satisfies $a lt $b"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 4)\n"
         "  (let $1 (sequence 1 2)\n"
         "    (quantified every $2 $1\n"
         "      (let $3 (sequence 3 4)\n"
         "        (quantified every $4 $3\n"
         "          (binary value-less $2 $4))))))\n"},
    {.compile = {.expression_text = "() instance of function(xs:string, item()*) as map(xs:string, array(xs:integer+))"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (sequence)\n"
         "    (instance-of $1 function(xs:string, item()*) as map(xs:string, array(xs:integer+)))))\n"},
    {.compile = {.expression_text = "() instance of record(a, b as xs:string?)"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (sequence)\n"
         "    (instance-of $1 record(\"a\" as item()*, \"b\" as xs:string?))))\n"},
    {.compile = {.expression_text = "() instance of enum('x', 'y', 'x')"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (sequence)\n"
         "    (instance-of $1 enum(\"x\", \"y\"))))\n"},
    {.compile = {.expression_text = "() instance of (xs:string | xs:integer)*"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (sequence)\n"
         "    (instance-of $1 (xs:string | xs:integer)*)))\n"},
    {.compile = {.expression_text = "() instance of element(a|p:b, xs:untyped?)",
                 .namespace_prefixes = {"p"},
                 .namespace_uris = {"urn:p"}},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (sequence)\n"
         "    (instance-of $1 (element(a, xs:untyped?) | element(Q{urn:p}b, xs:untyped?)))))\n"},
    {.compile = {.expression_text = "() instance of document-node(element(a))"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (sequence)\n"
         "    (instance-of $1 document-node(element(a)))))\n"},
    {.compile = {.expression_text = "() instance of processing-instruction('  t ')"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (sequence)\n"
         "    (instance-of $1 processing-instruction(t))))\n"},
    {.compile = {.expression_text = "() instance of empty-sequence()"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (sequence)\n"
         "    (instance-of $1 empty-sequence())))\n"},
    {.compile = {.expression_text = "1 treat as xs:integer+"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 0)\n"
         "  (treat 1 xs:integer+))\n"},
    {.compile = {.expression_text = "'1' cast as xs:integer?"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 0)\n"
         "  (cast \"1\" xs:integer?))\n"},
    {.compile = {.expression_text = "'1' castable as xs:NMTOKENS"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 0)\n"
         "  (castable \"1\" xs:NMTOKENS))\n"},
    {.compile = {.expression_text = "a/@b/text()/..",
                 .namespace_prefixes = {""},
                 .namespace_uris = {"urn:default"}},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 11) (focus $1 _ _)\n"
         "  (let $2 (step $1 child element(Q{urn:default}a))\n"
         "    (let $4 (map $2 (focus $3 _ _)\n"
         "              (step $3 attribute attribute(b)))\n"
         "      (let $5 (document-order $4)\n"
         "        (let $7 (map $5 (focus $6 _ _)\n"
         "                  (step $6 child text()))\n"
         "          (let $8 (document-order $7)\n"
         "            (let $11 (map $8 (focus $9 _ _)\n"
         "                       (let $10 (step $9 parent node())\n"
         "                         (document-order $10)))\n"
         "              (document-order $11))))))))\n"},
    {.compile = {.expression_text = "p:*/*:local/Q{urn:q}*/namespace::*",
                 .namespace_prefixes = {"p"},
                 .namespace_uris = {"urn:p"}},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 10) (focus $1 _ _)\n"
         "  (let $2 (step $1 child element(Q{urn:p}*))\n"
         "    (let $4 (map $2 (focus $3 _ _)\n"
         "              (step $3 child element(*:local)))\n"
         "      (let $5 (document-order $4)\n"
         "        (let $7 (map $5 (focus $6 _ _)\n"
         "                  (step $6 child element(Q{urn:q}*)))\n"
         "          (let $8 (document-order $7)\n"
         "            (let $10 (map $8 (focus $9 _ _)\n"
         "                       (step $9 namespace namespace-node()))\n"
         "              (document-order $10))))))))\n"},
    {.compile = {.expression_text = "(a | b) intersect c except d"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 7) (focus $1 _ _)\n"
         "  (let $2 (step $1 child element(a))\n"
         "    (let $3 (step $1 child element(b))\n"
         "      (let $4 (binary union $2 $3)\n"
         "        (let $5 (step $1 child element(c))\n"
         "          (let $6 (binary intersect $4 $5)\n"
         "            (let $7 (step $1 child element(d))\n"
         "              (binary except $6 $7))))))))\n"},
    {.compile = {.expression_text = "(a, b) ! (. is ., . << .)"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 7) (focus $1 _ _)\n"
         "  (let $2 (step $1 child element(a))\n"
         "    (let $3 (step $1 child element(b))\n"
         "      (let $4 (sequence $2 $3)\n"
         "        (map $4 (focus $5 _ _)\n"
         "          (let $6 (binary is $5 $5)\n"
         "            (let $7 (binary precedes $5 $5)\n"
         "              (sequence $6 $7))))))))\n"},
    {.compile = {.expression_text = "ancestor::node()[1]"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 3) (focus $1 _ _)\n"
         "  (let $2 (step $1 ancestor node())\n"
         "    (let $3 (filter $2 (focus _ _ _)\n"
         "              1)\n"
         "      (document-order $3))))\n"},
    {.compile = {.expression_text = "child::(a | text())"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1) (focus $1 _ _)\n"
         "  (step $1 child (element(a) | text())))\n"},
    {.compile = {.expression_text = "-(+1)"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (unary plus 1)\n"
         "    (unary minus $1)))\n"},
    {.compile = {.expression_text = "xs:int(?)"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 0)\n"
         "  (function-definition 2))\n"
         "(function 2 anonymous (parameters 1) (captures) (slots 1)\n"
         "  (cast $1 xs:int?))\n"},
    {.compile = {.expression_text = "concat(?, 'b', ?)"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 0)\n"
         "  (function-definition 2))\n"
         "(function 2 anonymous (parameters 2) (captures) (slots 2)\n"
         "  (static-call fn:concat#3 $1 \"b\" $2))\n"},
    {.compile = {.expression_text = "map { 1: 2, 'k': (3, 4) }"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 1)\n"
         "  (let $1 (sequence 3 4)\n"
         "    (map-constructor (1 2) (\"k\" $1))))\n"},
    {.compile = {.expression_text = "[1, (2, 3)]?*"},
     .expected_text =
         "(function 1 main (parameters 0) (captures) (slots 2)\n"
         "  (let $1 (sequence 2 3)\n"
         "    (let $2 (array-constructor square 1 $1)\n"
         "      (lookup $2 *))))\n"},
};

static void test_constructs_lower_as_documented(sxml_test_state *state) {
    for (size_t case_index = 0;
         case_index < sizeof(feature_cases) / sizeof(feature_cases[0]);
         case_index += 1) {
        SXML_TEST_EXPECT(state,
                         compiles_to(&feature_cases[case_index].compile,
                                     feature_cases[case_index].expected_text));
    }
}

/* ------------------------------------------------------------- Errors */

static void test_variable_in_its_own_binding_is_undeclared(
    sxml_test_state *state) {
    /* QT4 K-QuantExprWithout-79. */
    const compile_case test_case = plain("some $foo in $foo satisfies 1");
    SXML_TEST_EXPECT(state,
                     fails_with(&test_case,
                                (expected_error){.code = "XPST0008",
                                                 .line_number = 1,
                                                 .column_number = 14}));
}

static void test_function_with_wrong_arity_is_unknown(sxml_test_state *state) {
    /* QT4 fn-filter-003 and map-merge-001. */
    const compile_case filter_case = plain("fn:filter( fn:boolean#1 )");
    SXML_TEST_EXPECT(state,
                     fails_with(&filter_case,
                                (expected_error){.code = "XPST0017",
                                                 .line_number = 1,
                                                 .column_number = 1}));
    const compile_case merge_case = plain("map:merge()");
    SXML_TEST_EXPECT(state,
                     fails_with(&merge_case,
                                (expected_error){.code = "XPST0017",
                                                 .line_number = 1,
                                                 .column_number = 1}));
}

/* An error case: the expression and where its static error is. */
typedef struct error_case {
    const char *expression_text;
    expected_error expected;
} error_case;

static const error_case error_cases[] = {
    {"p:x", {"XPST0081", 1, 1}},
    {"#q:x", {"XPST0081", 1, 2}},
    {"$y", {"XPST0008", 1, 1}},
    {"function() { $z }", {"XPST0008", 1, 14}},
    {"let $a := $a return 1", {"XPST0008", 1, 11}},
    {"1 instance of xs:nonexistent", {"XPST0051", 1, 15}},
    {"() instance of xs:NMTOKENS", {"XPST0051", 1, 16}},
    {"1 cast as xs:nothing", {"XPST0051", 1, 11}},
    {"1 cast as xs:NOTATION", {"XPST0080", 1, 11}},
    {"function($a, $a) { 1 }", {"XPST0039", 1, 14}},
    {"() instance of map(element(), item())", {"XPST0152", 1, 20}},
    {"() instance of record(a, a)", {"XPST0021", 1, 26}},
    {"() instance of schema-element(x)", {"XPST0008", 1, 16}},
    {"() instance of element(a, xs:nonexistent)", {"XPST0008", 1, 27}},
    {"nope()", {"XPST0017", 1, 1}},
    {"abs#2", {"XPST0017", 1, 1}},
    {"xs:int('1', 2)", {"XPST0017", 1, 1}},
    {"substring('a', nope := 1)", {"XPST0017", 1, 16}},
    {"substring(start := 1)", {"XPST0017", 1, 1}},
    {"1 +\n  $missing", {"XPST0008", 2, 3}},
    {"1 +", {"XPST0003", 1, 3}},
};

static void test_static_errors_have_code_and_location(sxml_test_state *state) {
    for (size_t case_index = 0;
         case_index < sizeof(error_cases) / sizeof(error_cases[0]);
         case_index += 1) {
        const compile_case test_case =
            plain(error_cases[case_index].expression_text);
        SXML_TEST_EXPECT(state,
                         fails_with(&test_case, error_cases[case_index].expected));
    }
}

/* Compiles test_case and writes only the status. */
static sxml_status compile_status(const compile_case *test_case,
                                  sxml_error_record *error_record) {
    static fixture test_fixture;
    static test_table table;
    sxml_status status = fixture_create(0, &test_fixture);
    if (status == SXML_STATUS_OK) {
        status = test_table_initialize(&test_fixture, &table);
    }
    case_context context;
    if (status == SXML_STATUS_OK) {
        status = prepare_case_context(&test_fixture, test_case, &context);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_xpath_static_context static_context = {
            .item_heap = test_fixture.item_heap,
            .allocator = fixture_allocator(&test_fixture),
            .variable_declarations = {.variable_declarations =
                                          context.declarations,
                                      .variable_declaration_count =
                                          context.declaration_count},
            .function_table = {.state = &table,
                               .operations = &test_table_operations},
        };
        sxml_ir_unit *unit = NULL;
        status = sxml_xpath_compile(
            &(sxml_xpath_compile_request){
                .expression_text = text_view(test_case->expression_text),
                .static_context = &static_context,
                .maximum_expression_depth = test_case->maximum_expression_depth,
                .error_record = error_record},
            &unit);
        sxml_ir_unit_release(unit);
        if (!fixture_released_everything(&test_fixture)) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    fixture_destroy(&test_fixture);
    return status;
}

static void test_constructs_beyond_phase_1_are_unsupported(
    sxml_test_state *state) {
    static const char *const expressions[] = {
        "for member $m in [1] return $m",
        "let $( $a, $b ) := (1, 2) return $a",
        "namespace::x",
    };
    for (size_t expression_index = 0;
         expression_index < sizeof(expressions) / sizeof(expressions[0]);
         expression_index += 1) {
        sxml_error_record error_record = {.value = NULL};
        const compile_case test_case = plain(expressions[expression_index]);
        const sxml_status status = compile_status(&test_case, &error_record);
        if (status != SXML_STATUS_UNSUPPORTED_FEATURE) {
            (void)fprintf(stderr, "%s: status %d\n",
                          expressions[expression_index], (int)status);
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_UNSUPPORTED_FEATURE);
        SXML_TEST_EXPECT(state,
                         error_record.diagnostic.description.byte_count > 0);
        SXML_TEST_EXPECT(state, error_record.diagnostic.location.line_number == 1);
    }
}

static void test_duplicate_variable_declarations_are_invalid(
    sxml_test_state *state) {
    const compile_case test_case = {.expression_text = "1",
                                    .variable_names = {"x", "x"}};
    SXML_TEST_EXPECT(state, compile_status(&test_case, NULL) ==
                                SXML_STATUS_INVALID_ARGUMENT);
}

/* Text of "1 + 1 + ... + 1" with term_count terms, or of term_count
   nested parentheses around 1. */
typedef struct generated_text {
    char text[24000];
} generated_text;

static void test_long_operator_chains_cost_no_stack(sxml_test_state *state) {
    static generated_text generated;
    const size_t term_count = 5000;
    size_t length = 0;
    for (size_t term_index = 0; term_index < term_count; term_index += 1) {
        memcpy(generated.text + length, term_index == 0 ? "1" : "+1",
               term_index == 0 ? 1 : 2);
        length += term_index == 0 ? 1 : 2;
    }
    generated.text[length] = '\0';
    const compile_case test_case = plain(generated.text);
    SXML_TEST_EXPECT(state, compile_status(&test_case, NULL) == SXML_STATUS_OK);
}

static void test_deep_nesting_reaches_the_depth_limit(sxml_test_state *state) {
    static generated_text generated;
    const size_t nesting = 200;
    const size_t shallow_nesting = 24;
    for (size_t index = 0; index < nesting; index += 1) {
        generated.text[index] = '(';
        generated.text[nesting + 1 + index] = ')';
    }
    generated.text[nesting] = '1';
    generated.text[2 * nesting + 1] = '\0';
    sxml_error_record error_record = {.value = NULL};
    const compile_case test_case = plain(generated.text);
    SXML_TEST_EXPECT(state, compile_status(&test_case, &error_record) ==
                                SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, error_record.diagnostic.description.byte_count > 0);
    /* Within a raised limit, nesting deeper than the default compiles. */
    static generated_text shallow;
    for (size_t index = 0; index < shallow_nesting; index += 1) {
        shallow.text[index] = '(';
        shallow.text[shallow_nesting + 1 + index] = ')';
    }
    shallow.text[shallow_nesting] = '1';
    shallow.text[2 * shallow_nesting + 1] = '\0';
    const compile_case limited_case = {.expression_text = shallow.text,
                                       .maximum_expression_depth = 20};
    SXML_TEST_EXPECT(state, compile_status(&limited_case, NULL) ==
                                SXML_STATUS_LIMIT_EXCEEDED);
    const compile_case shallow_case = {.expression_text = shallow.text,
                                       .maximum_expression_depth = 1000};
    SXML_TEST_EXPECT(state,
                     compile_status(&shallow_case, NULL) == SXML_STATUS_OK);
}

/* The costliest nesting per level, arrows into static calls and dynamic
   calls, up to just under the default depth: lowering's frames fit the
   64 KiB stack of the WebAssembly tests. */
static void test_nesting_at_the_default_limit_fits_the_stack(
    sxml_test_state *state) {
    static const char *const openings[] = {"1 => concat(", "$g("};
    for (size_t opening_index = 0; opening_index < 2; opening_index += 1) {
        static generated_text generated;
        const char *opening = openings[opening_index];
        const size_t opening_length = strlen(opening);
        const size_t nesting = 30;
        size_t length = 0;
        for (size_t level = 0; level < nesting; level += 1) {
            memcpy(generated.text + length, opening, opening_length);
            length += opening_length;
        }
        generated.text[length] = '1';
        length += 1;
        memset(generated.text + length, ')', nesting);
        generated.text[length + nesting] = '\0';
        const compile_case test_case = {.expression_text = generated.text,
                                        .variable_names = {"g"}};
        SXML_TEST_EXPECT(state, compile_status(&test_case, NULL) == SXML_STATUS_OK);
    }
}

/* Every allocation of compiling (the AST, the unit, scratch arrays) can
   fail; each failure releases everything and reports
   SXML_STATUS_OUT_OF_MEMORY (D23). */
static void test_compile_survives_every_allocation_failure(
    sxml_test_state *state) {
    const compile_case test_case = {
        .expression_text =
            "let $f := function($y as xs:integer) as item()* "
            "{ $y + $x, substring(?, 2)('abc'), position#0 } "
            "return (/a//b[@c = 1.5e0] ! $f(.), map { 'k': [1, 2] }, "
            "`t{$x}`)",
        .variable_names = {"x"}};
    uint64_t failing_allocation_number = 1;
    for (;;) {
        static fixture test_fixture;
        static test_table table;
        SXML_TEST_REQUIRE(state, fixture_create(failing_allocation_number,
                                                &test_fixture) == SXML_STATUS_OK);
        SXML_TEST_REQUIRE(state, test_table_initialize(&test_fixture, &table) ==
                                     SXML_STATUS_OK);
        static compile_outcome outcome;
        compile_with((compile_environment){.test_fixture = &test_fixture,
                                           .table = &table},
                     &test_case, &outcome);
        const sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(test_fixture.counting_allocator);
        SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
        fixture_destroy(&test_fixture);
        if (outcome.status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state, counts.failed_allocation_count == 1);
        if (outcome.status != SXML_STATUS_OUT_OF_MEMORY) {
            (void)fprintf(stderr, "allocation %llu: status %d %.*s\n",
                          (unsigned long long)failing_allocation_number,
                          (int)outcome.status,
                          (int)outcome.error_record.diagnostic.description
                              .byte_count,
                          (const char *)outcome.error_record.diagnostic
                              .description.bytes);
            break;
        }
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 10000);
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 20);
}

static const sxml_test_case xpath_static_test_cases[] = {
    {.name = "absolute_path_lowers_to_root_treat_and_maps",
     .function = test_absolute_path_lowers_to_root_treat_and_maps},
    {.name = "slice_queries_match_golden_text",
     .function = test_slice_queries_match_golden_text},
    {.name = "constructs_lower_as_documented",
     .function = test_constructs_lower_as_documented},
    {.name = "variable_in_its_own_binding_is_undeclared",
     .function = test_variable_in_its_own_binding_is_undeclared},
    {.name = "function_with_wrong_arity_is_unknown",
     .function = test_function_with_wrong_arity_is_unknown},
    {.name = "static_errors_have_code_and_location",
     .function = test_static_errors_have_code_and_location},
    {.name = "constructs_beyond_phase_1_are_unsupported",
     .function = test_constructs_beyond_phase_1_are_unsupported},
    {.name = "duplicate_variable_declarations_are_invalid",
     .function = test_duplicate_variable_declarations_are_invalid},
    {.name = "long_operator_chains_cost_no_stack",
     .function = test_long_operator_chains_cost_no_stack},
    {.name = "deep_nesting_reaches_the_depth_limit",
     .function = test_deep_nesting_reaches_the_depth_limit},
    {.name = "nesting_at_the_default_limit_fits_the_stack",
     .function = test_nesting_at_the_default_limit_fits_the_stack},
    {.name = "compile_survives_every_allocation_failure",
     .function = test_compile_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xpath_static",
            .cases = xpath_static_test_cases,
            .case_count = SXML_TEST_COUNT(xpath_static_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
