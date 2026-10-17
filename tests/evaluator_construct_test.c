/*
 * Tests for the node constructors of the evaluator
 * (src/evaluator_construct.c; D40, D67, D98, D101, D110).
 *
 * Golden file: tests/evaluator_machine_test.c (fixture, hand-built IR,
 * allocation-failure loops), with the value rendering of
 * tests/evaluator_test.c. Lowering of constructors comes with packet 3.9,
 * so every case writes its IR by hand: a program is a chain of LET
 * expressions binding slots, ending in a body. Values are rendered as text:
 * nodes as markup (names with their prefixes, "Q{uri}local" for a name in
 * a namespace without one), atomic items in canonical form, items
 * separated by " | ", and an error as "error:CODE". Every case checks that
 * the item heap's cells and the allocator's blocks are all released, and
 * every case is run again failing each allocation in turn.
 */
#include "cast_internal.h"
#include "evaluator_machine_internal.h"
#include "ir_internal.h"
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

static sxml_byte_view text_view(const char *text) {
    return sxml_byte_view_from_c_string(text);
}

/* ------------------------------------------------------------ Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    alignas(16) uint8_t counting_storage[256];
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
    sxml_dynamic_context *dynamic_context;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

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
            &(sxml_type_registry_configuration){.allocator = sxml_default_allocator(),
                                                .name_pool = test_fixture->name_pool,
                                                .maximum_registered_type_count = 0},
            &test_fixture->registry);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_create(
            fixture_allocator(test_fixture),
            &(sxml_item_heap_configuration){.allocator = fixture_allocator(test_fixture),
                                            .name_pool = test_fixture->name_pool,
                                            .type_registry = test_fixture->registry,
                                            .maximum_byte_count = 0},
            &test_fixture->item_heap);
    }
    if (status == SXML_STATUS_OK) {
        sxml_evaluator_configuration configuration;
        memset(&configuration, 0, sizeof(configuration));
        configuration.item_heap = test_fixture->item_heap;
        configuration.allocator = fixture_allocator(test_fixture);
        status = sxml_dynamic_context_create(fixture_allocator(test_fixture), &configuration,
                                             &test_fixture->dynamic_context);
    }
    return status;
}

/* Destroys the fixture; true when every block and cell was released. */
static bool fixture_destroy(fixture *test_fixture) {
    sxml_dynamic_context_destroy(test_fixture->dynamic_context);
    bool released = true;
    if (test_fixture->item_heap != NULL) {
        sxml_item_heap_clear_raised_error(test_fixture->item_heap);
        released = sxml_item_heap_read_counts(test_fixture->item_heap).live_cell_count == 0;
    }
    sxml_item_heap_destroy(test_fixture->item_heap);
    sxml_type_registry_destroy(test_fixture->registry);
    sxml_name_pool_destroy(test_fixture->name_pool);
    if (test_fixture->counting_allocator != NULL) {
        released = released &&
                   sxml_counting_allocator_read_counts(test_fixture->counting_allocator)
                           .live_allocation_count == 0;
        sxml_counting_allocator_finalize(test_fixture->counting_allocator);
    }
    *test_fixture = (fixture){.counting_allocator = NULL};
    if (!released) {
        (void)fprintf(stderr, "leak after a case\n");
    }
    return released;
}

/* ------------------------------------------------------------ Programs */

#define PROGRAM_MAXIMUM_BINDING_COUNT 16U

/* A main function being written: LET bindings, then a body. Every helper
   does nothing once status is not OK. */
typedef struct program {
    const fixture *test_fixture;
    sxml_ir_unit *unit;
    sxml_status status;
    sxml_ir_expression_index bound_expressions[PROGRAM_MAXIMUM_BINDING_COUNT];
    uint32_t binding_count;
} program;

static sxml_ir_atom no_atom(void) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE, .constant = {.value = 0}};
}

/* A constant atom for value, owned: the unit retains it, this releases. */
static sxml_ir_atom program_constant(program *writer, sxml_sequence value) {
    sxml_ir_constant_index constant = {.value = 0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_ir_unit_add_constant(writer->unit, value, &constant);
    }
    sxml_sequence_release(value);
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_CONSTANT, .constant = constant};
}

static sxml_ir_atom program_atomic(program *writer, const sxml_atomic_item_request *request) {
    sxml_item item = {.representation = 0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_atomic_item_make(writer->test_fixture->item_heap, request, &item);
    }
    if (writer->status != SXML_STATUS_OK) {
        return no_atom();
    }
    return program_constant(writer, sxml_sequence_from_item(item));
}

/* A string-like constant of type (xs:string, xs:untypedAtomic). */
typedef struct typed_text {
    const char *text;
    sxml_builtin_type type;
} typed_text;

static sxml_ir_atom program_typed_string(program *writer, typed_text text) {
    sxml_string string = {.utf8 = {.bytes = NULL, .byte_count = 0},
                          .codepoint_count = 0,
                          .is_ascii = true};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_string_from_utf8(text_view(text.text), &string);
    }
    return program_atomic(
        writer, &(sxml_atomic_item_request){
                    .type = sxml_builtin_type_identifier(text.type),
                    .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                              .datum_details = {.string = string}}});
}

static sxml_ir_atom program_string(program *writer, const char *text) {
    return program_typed_string(writer,
                                (typed_text){.text = text, .type = SXML_TYPE_XS_STRING});
}

static sxml_ir_atom program_integer(program *writer, int64_t integer) {
    return program_constant(writer, sxml_sequence_from_item(sxml_atomic_item_from_integer(integer)));
}

/* An xs:QName: namespace URI, prefix (empty for none), local name. */
typedef struct qname_text {
    const char *namespace_uri;
    const char *namespace_prefix;
    const char *local_name;
} qname_text;

static sxml_ir_atom program_qname(program *writer, qname_text qname) {
    sxml_name_pool *name_pool = writer->test_fixture->name_pool;
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_qname_datum datum = {.expanded_name = {.index = 0}, .namespace_prefix = {.index = 0}};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_namespace_uri(
            name_pool, text_view(qname.namespace_uri), &namespace_uri);
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_namespace_prefix(
            name_pool, text_view(qname.namespace_prefix), &datum.namespace_prefix);
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri,
                                 .local_name = text_view(qname.local_name)},
            &datum.expanded_name);
    }
    return program_atomic(writer,
                          &(sxml_atomic_item_request){
                              .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
                              .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                                        .datum_details = {.qname = datum}}});
}

/* A local name in no namespace. */
static sxml_ir_atom program_name(program *writer, const char *local_name) {
    return program_qname(writer, (qname_text){.namespace_uri = "",
                                              .namespace_prefix = "",
                                              .local_name = local_name});
}

static sxml_ir_expression_index program_add(program *writer,
                                            const sxml_ir_expression *expression) {
    sxml_ir_expression_index index = {.value = 0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_ir_unit_add_expression(writer->unit, expression, &index);
    }
    return index;
}

/* The concatenation of atoms. */
static sxml_ir_expression_index program_sequence(program *writer, sxml_ir_atom_view atoms) {
    sxml_ir_atom_list list = {.first_atom_index = 0, .atom_count = 0};
    if (writer->status == SXML_STATUS_OK && atoms.atom_count > 0) {
        writer->status = sxml_ir_unit_add_atoms(writer->unit, atoms, &list);
    }
    sxml_ir_expression expression;
    memset(&expression, 0, sizeof(expression));
    expression.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE;
    expression.sequence = list;
    return program_add(writer, &expression);
}

#define PROGRAM_SEQUENCE(writer, ...)                                                      \
    program_sequence((writer), (sxml_ir_atom_view){                                         \
                                   .atoms = (const sxml_ir_atom[]){__VA_ARGS__},            \
                                   .atom_count = sizeof((const sxml_ir_atom[]){__VA_ARGS__}) / \
                                                 sizeof(sxml_ir_atom)})

static sxml_ir_expression_index program_atom(program *writer, sxml_ir_atom atom) {
    sxml_ir_expression expression;
    memset(&expression, 0, sizeof(expression));
    expression.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM;
    expression.atom = atom;
    return program_add(writer, &expression);
}

/* A constructor expression. */
static sxml_ir_expression_index program_construct(program *writer,
                                                  sxml_ir_expression_kind kind,
                                                  sxml_ir_node_construction construction) {
    sxml_ir_expression expression;
    memset(&expression, 0, sizeof(expression));
    expression.expression_kind = kind;
    expression.node_construction = construction;
    return program_add(writer, &expression);
}

/* Binds the next slot to the value of bound and returns the slot's atom. */
static sxml_ir_atom program_bind(program *writer, sxml_ir_expression_index bound) {
    if (writer->status == SXML_STATUS_OK &&
        writer->binding_count == PROGRAM_MAXIMUM_BINDING_COUNT) {
        writer->status = SXML_STATUS_CAPACITY_EXCEEDED;
    }
    if (writer->status != SXML_STATUS_OK) {
        return no_atom();
    }
    writer->bound_expressions[writer->binding_count] = bound;
    writer->binding_count += 1;
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT,
                          .slot = {.value = writer->binding_count}};
}

/* Shorthands for the constructors. */
static sxml_ir_atom bind_construct(program *writer, sxml_ir_expression_kind kind,
                                   sxml_ir_node_construction construction) {
    return program_bind(writer, program_construct(writer, kind, construction));
}

static sxml_ir_node_construction named(sxml_ir_atom name, sxml_ir_atom value) {
    return (sxml_ir_node_construction){.name = name,
                                       .value = value,
                                       .content = {.value = 0},
                                       .namespace_bindings = {.first_namespace_binding_index = 0,
                                                              .namespace_binding_count = 0}};
}

static sxml_ir_node_construction with_content(sxml_ir_atom name,
                                              sxml_ir_expression_index content) {
    sxml_ir_node_construction construction = named(name, no_atom());
    construction.content = content;
    return construction;
}

/* The static namespaces of the unit, also usable as the namespace
   bindings written on an element. */
static sxml_ir_namespace_binding_list program_set_static_namespaces(
    program *writer, const qname_text *bindings, size_t binding_count) {
    sxml_namespace_binding namespace_bindings[8];
    for (size_t binding_index = 0;
         writer->status == SXML_STATUS_OK && binding_index < binding_count;
         binding_index += 1) {
        writer->status = sxml_name_pool_intern_namespace_prefix(
            writer->test_fixture->name_pool,
            text_view(bindings[binding_index].namespace_prefix),
            &namespace_bindings[binding_index].namespace_prefix);
        if (writer->status == SXML_STATUS_OK) {
            writer->status = sxml_name_pool_intern_namespace_uri(
                writer->test_fixture->name_pool,
                text_view(bindings[binding_index].namespace_uri),
                &namespace_bindings[binding_index].namespace_uri);
        }
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_ir_unit_set_static_namespaces(
            writer->unit, (sxml_namespace_binding_view){.namespace_bindings = namespace_bindings,
                                                        .namespace_binding_count = binding_count});
    }
    return writer->status == SXML_STATUS_OK
               ? writer->unit->static_namespaces
               : (sxml_ir_namespace_binding_list){.first_namespace_binding_index = 0,
                                                  .namespace_binding_count = 0};
}

static void program_set_base_uri(program *writer, const char *base_uri) {
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_ir_unit_set_static_base_uri(writer->unit, text_view(base_uri));
    }
}

/* Writes a program: returns its body. */
typedef sxml_ir_expression_index (*program_writer)(program *writer);

/* Writes the unit of writer_function, sealed, or fails. */
static sxml_status program_write(const fixture *test_fixture, program_writer writer_function,
                                 sxml_ir_unit **unit) {
    program writer = {.test_fixture = test_fixture, .unit = NULL, .status = SXML_STATUS_OK};
    writer.status = sxml_ir_unit_create(
        fixture_allocator(test_fixture),
        &(sxml_ir_unit_configuration){.allocator = fixture_allocator(test_fixture),
                                      .item_heap = test_fixture->item_heap,
                                      .source_name = text_view("construct"),
                                      .maximum_expression_count = 0},
        &writer.unit);
    if (writer.status != SXML_STATUS_OK) {
        return writer.status;
    }
    sxml_ir_function_index main_function = {0};
    writer.status = sxml_ir_unit_add_function(writer.unit, &(sxml_ir_function){.slot_count = 0},
                                              &main_function);
    sxml_ir_expression_index body = writer_function(&writer);
    /* Each binding wraps the body, innermost last. */
    for (uint32_t binding_number = writer.binding_count; binding_number > 0;
         binding_number -= 1) {
        sxml_ir_expression expression;
        memset(&expression, 0, sizeof(expression));
        expression.expression_kind = SXML_IR_EXPRESSION_KIND_LET;
        expression.let = (sxml_ir_let){.variable = {.value = binding_number},
                                       .bound_expression =
                                           writer.bound_expressions[binding_number - 1],
                                       .body = body};
        body = program_add(&writer, &expression);
    }
    if (writer.status == SXML_STATUS_OK) {
        writer.status = sxml_ir_unit_replace_function(
            writer.unit,
            &(sxml_ir_function_replacement){
                .index = main_function,
                .function = {.slot_count = writer.binding_count, .body = body}});
    }
    if (writer.status != SXML_STATUS_OK) {
        sxml_ir_unit_release(writer.unit);
        return writer.status;
    }
    sxml_ir_unit_seal(writer.unit);
    *unit = writer.unit;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Rendering */

#define RENDER_CAPACITY 2048U

/* What a rendering shows besides the markup. */
typedef enum render_detail {
    SXML_TEST_RENDER_NONE = 0,
    /* "[p=u ...]" after each element's name: its in-scope namespaces
       except xml, sorted by prefix ("=u" for the default namespace). */
    SXML_TEST_RENDER_NAMESPACES = 1,
    /* "{base}" after each root: its base URI. */
    SXML_TEST_RENDER_BASE_URI = 2,
    /* "#untyped" or "#other" after each element and attribute: whether
       its type annotation is xs:untyped / xs:untypedAtomic. */
    SXML_TEST_RENDER_TYPES = 4,
    /* "#id" after each attribute marked as an ID. */
    SXML_TEST_RENDER_IDS = 8,
} render_detail;

typedef struct rendering {
    const fixture *test_fixture;
    unsigned details;
    char text[RENDER_CAPACITY];
    size_t byte_count;
    bool is_valid;
} rendering;

static void render_bytes(rendering *output, sxml_byte_view bytes) {
    if (output->byte_count + bytes.byte_count + 1 > RENDER_CAPACITY) {
        output->is_valid = false;
        return;
    }
    if (bytes.byte_count > 0) {
        memcpy(output->text + output->byte_count, bytes.bytes, bytes.byte_count);
    }
    output->byte_count += bytes.byte_count;
    output->text[output->byte_count] = '\0';
}

static void render_text(rendering *output, const char *text) {
    render_bytes(output, text_view(text));
}

static void render_name(rendering *output, const sxml_node_description *description) {
    sxml_name_parts parts;
    if (sxml_name_pool_get_name_parts(output->test_fixture->name_pool, description->node_name,
                                      &parts) != SXML_STATUS_OK) {
        output->is_valid = false;
        return;
    }
    sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
    if (description->namespace_prefix.index != 0 &&
        sxml_name_pool_get_namespace_prefix(output->test_fixture->name_pool,
                                            description->namespace_prefix,
                                            &prefix) != SXML_STATUS_OK) {
        output->is_valid = false;
        return;
    }
    if (prefix.byte_count > 0) {
        render_bytes(output, prefix);
        render_text(output, ":");
    } else if (parts.namespace_uri.index != SXML_NAMESPACE_NONE) {
        sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
        if (sxml_name_pool_get_namespace_uri(output->test_fixture->name_pool,
                                               parts.namespace_uri, &namespace_uri) != SXML_STATUS_OK) {
            output->is_valid = false;
        }
        render_text(output, "Q{");
        render_bytes(output, namespace_uri);
        render_text(output, "}");
    }
    render_bytes(output, parts.local_name);
}

static void render_namespaces(rendering *output, sxml_node_reference element) {
    sxml_namespace_binding_view bindings;
    if (sxml_node_get_in_scope_namespaces(element, &bindings) != SXML_STATUS_OK) {
        output->is_valid = false;
        return;
    }
    /* Sorted by prefix text, repeatedly picking the smallest after the
       last one written. */
    const sxml_name_pool *name_pool = output->test_fixture->name_pool;
    render_text(output, "[");
    sxml_byte_view last = {.bytes = NULL, .byte_count = 0};
    bool has_last = false;
    bool is_first = true;
    for (size_t round_index = 0; round_index < bindings.namespace_binding_count;
         round_index += 1) {
        bool has_best = false;
        sxml_byte_view best = {.bytes = NULL, .byte_count = 0};
        sxml_namespace_binding best_binding = {.namespace_prefix = {.index = 0},
                                               .namespace_uri = {.index = 0}};
        for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
             binding_index += 1) {
            const sxml_namespace_binding binding = bindings.namespace_bindings[binding_index];
            sxml_byte_view prefix = {.bytes = (const uint8_t *)"", .byte_count = 0};
            if (binding.namespace_prefix.index != 0) {
                if (sxml_name_pool_get_namespace_prefix(name_pool, binding.namespace_prefix,
                                                          &prefix) != SXML_STATUS_OK) {
            output->is_valid = false;
        }
            }
            if (has_last) {
                const size_t shorter =
                    prefix.byte_count < last.byte_count ? prefix.byte_count : last.byte_count;
                const int order = shorter == 0 ? 0 : memcmp(prefix.bytes, last.bytes, shorter);
                if (order < 0 || (order == 0 && prefix.byte_count <= last.byte_count)) {
                    continue;
                }
            }
            bool is_better = !has_best;
            if (has_best) {
                const size_t shorter =
                    prefix.byte_count < best.byte_count ? prefix.byte_count : best.byte_count;
                const int order = shorter == 0 ? 0 : memcmp(prefix.bytes, best.bytes, shorter);
                is_better = order < 0 || (order == 0 && prefix.byte_count < best.byte_count);
            }
            if (is_better) {
                has_best = true;
                best = prefix;
                best_binding = binding;
            }
        }
        if (!has_best) {
            break;
        }
        has_last = true;
        last = best;
        if (sxml_byte_view_equals(best, text_view("xml"))) {
            continue;
        }
        sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
        if (sxml_name_pool_get_namespace_uri(name_pool, best_binding.namespace_uri,
                                               &namespace_uri) != SXML_STATUS_OK) {
            output->is_valid = false;
        }
        if (!is_first) {
            render_text(output, " ");
        }
        is_first = false;
        render_bytes(output, best);
        render_text(output, "=");
        render_bytes(output, namespace_uri);
    }
    render_text(output, "]");
}

static void render_type(rendering *output, const sxml_node_description *description,
                        sxml_builtin_type untyped_type) {
    if ((output->details & SXML_TEST_RENDER_TYPES) != 0) {
        render_text(output, sxml_type_identifier_equals(description->type_annotation,
                                                        sxml_builtin_type_identifier(untyped_type))
                                ? "#untyped"
                                : "#other");
    }
}

static void render_content(rendering *output, sxml_node_reference node) {
    sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
    if (sxml_node_get_content(node, &content) != SXML_STATUS_OK) {
        output->is_valid = false;
        return;
    }
    render_bytes(output, content);
}

static void render_attribute(rendering *output, sxml_node_reference attribute) {
    sxml_node_description description;
    if (sxml_node_describe(attribute, &description) != SXML_STATUS_OK) {
        output->is_valid = false;
        return;
    }
    render_name(output, &description);
    render_text(output, "=\"");
    render_content(output, attribute);
    render_text(output, "\"");
    render_type(output, &description, SXML_TYPE_XS_UNTYPED_ATOMIC);
    if ((output->details & SXML_TEST_RENDER_IDS) != 0 && description.is_id) {
        render_text(output, "#id");
    }
}

static void render_base_uri(rendering *output, sxml_node_reference node) {
    if ((output->details & SXML_TEST_RENDER_BASE_URI) == 0) {
        return;
    }
    sxml_sequence base_uri = sxml_sequence_empty();
    if (sxml_node_make_base_uri(output->test_fixture->item_heap,
                                &(sxml_node_value_request){.node = node, .error_record = NULL},
                                &base_uri) != SXML_STATUS_OK) {
        output->is_valid = false;
        return;
    }
    render_text(output, "{");
    sxml_atomic_datum datum;
    if (sxml_sequence_get_representation(base_uri) != SXML_SEQUENCE_REPRESENTATION_EMPTY &&
        sxml_atomic_item_get_datum(base_uri.content, &datum) == SXML_STATUS_OK) {
        render_bytes(output, datum.datum_details.string.utf8);
    }
    render_text(output, "}");
    sxml_sequence_release(base_uri);
}

/* The end of an open document or element. */
static void render_close(rendering *output, sxml_node_reference node) {
    sxml_node_description description;
    if (sxml_node_describe(node, &description) != SXML_STATUS_OK) {
        output->is_valid = false;
        return;
    }
    if (description.node_kind == SXML_NODE_KIND_DOCUMENT) {
        render_text(output, "}");
        return;
    }
    render_text(output, "</");
    render_name(output, &description);
    render_text(output, ">");
}

/* Renders a node and its subtree in node index order, closing elements by
   their last descendants (no recursion, D71). */
static void render_node(rendering *output, sxml_node_reference root) {
    sxml_node_description description;
    if (sxml_node_describe(root, &description) != SXML_STATUS_OK) {
        output->is_valid = false;
        return;
    }
    if (description.node_kind == SXML_NODE_KIND_NAMESPACE) {
        render_text(output, "namespace(");
        if (description.node_name.index != 0) {
            render_name(output, &description);
        }
        render_text(output, "=");
        render_content(output, root);
        render_text(output, ")");
        return;
    }
    if (description.node_kind == SXML_NODE_KIND_ATTRIBUTE) {
        render_text(output, "@");
        render_attribute(output, root);
        return;
    }
    render_base_uri(output, root);
    /* The open documents and elements and their last descendants. */
    sxml_node_reference open_nodes[64];
    uint32_t open_last_descendants[64];
    size_t open_count = 0;
    const uint32_t last_index = description.last_descendant.index;
    for (uint32_t node_index = root.node_index.index; node_index <= last_index;) {
        const sxml_node_reference node = {.tree = root.tree,
                                          .node_index = {.index = node_index},
                                          .namespace_number = 0};
        while (open_count > 0 &&
               open_last_descendants[open_count - 1] < node_index) {
            open_count -= 1;
            render_close(output, open_nodes[open_count]);
        }
        if (sxml_node_describe(node, &description) != SXML_STATUS_OK || open_count == 64) {
            output->is_valid = false;
            return;
        }
        switch (description.node_kind) {
        case SXML_NODE_KIND_DOCUMENT:
            render_text(output, "doc{");
            open_last_descendants[open_count] = description.last_descendant.index;
            open_nodes[open_count] = node;
            open_count += 1;
            break;
        case SXML_NODE_KIND_ELEMENT:
            render_text(output, "<");
            render_name(output, &description);
            render_type(output, &description, SXML_TYPE_XS_UNTYPED);
            if ((output->details & SXML_TEST_RENDER_NAMESPACES) != 0) {
                render_namespaces(output, node);
            }
            for (uint32_t attribute_offset = 1; attribute_offset <= description.attribute_count;
                 attribute_offset += 1) {
                render_text(output, " ");
                render_attribute(output,
                                 (sxml_node_reference){
                                     .tree = root.tree,
                                     .node_index = {.index = node_index + attribute_offset},
                                     .namespace_number = 0});
            }
            render_text(output, ">");
            open_last_descendants[open_count] = description.last_descendant.index;
            open_nodes[open_count] = node;
            open_count += 1;
            node_index += description.attribute_count;
            break;
        case SXML_NODE_KIND_TEXT:
            render_content(output, node);
            break;
        case SXML_NODE_KIND_COMMENT:
            render_text(output, "<!--");
            render_content(output, node);
            render_text(output, "-->");
            break;
        case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
            render_text(output, "<?");
            render_name(output, &description);
            render_text(output, " ");
            render_content(output, node);
            render_text(output, "?>");
            break;
        case SXML_NODE_KIND_ATTRIBUTE:
        case SXML_NODE_KIND_NAMESPACE:
        case SXML_NODE_KIND_ANY:
            output->is_valid = false;
            return;
        }
        node_index += 1;
    }
    while (open_count > 0) {
        open_count -= 1;
        render_close(output, open_nodes[open_count]);
    }
}

static void render_value(rendering *output, sxml_sequence value) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    bool is_first = true;
    for (;;) {
        sxml_iteration_step step;
        if (sxml_sequence_iterator_next(&iterator, NULL, &step) != SXML_STATUS_OK) {
            output->is_valid = false;
            return;
        }
        if (!step.has_item) {
            return;
        }
        if (!is_first) {
            render_text(output, " | ");
        }
        is_first = false;
        sxml_node_reference node;
        if (sxml_item_get_node(step.item, &node) == SXML_STATUS_OK) {
            render_node(output, node);
            continue;
        }
        uint8_t canonical_output[SXML_CAST_CANONICAL_TEXT_CAPACITY];
        sxml_canonical_text canonical;
        if (sxml_cast_write_canonical_text(
                output->test_fixture->item_heap,
                &(sxml_canonical_text_request){
                    .item = step.item,
                    .output = {.bytes = canonical_output,
                               .byte_count = sizeof(canonical_output)}},
                &canonical) != SXML_STATUS_OK) {
            render_text(output, "?");
            continue;
        }
        render_bytes(output, canonical.text);
    }
}

/* ------------------------------------------------------------ Running */

/* One case: a program and what it renders to. */
typedef struct construct_case {
    const char *name;
    program_writer writer_function;
    unsigned details;
    const char *expected;
} construct_case;

/* Evaluates the case's program in test_fixture and renders its value or
   error into output. Returns the status of writing or evaluating. */
static sxml_status run_case(const fixture *test_fixture, const construct_case *test_case,
                            rendering *output) {
    *output = (rendering){.test_fixture = test_fixture,
                          .details = test_case->details,
                          .byte_count = 0,
                          .is_valid = true};
    output->text[0] = '\0';
    sxml_ir_unit *unit = NULL;
    sxml_status status = program_write(test_fixture, test_case->writer_function, &unit);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_sequence value = sxml_sequence_empty();
    status = sxml_evaluator_evaluate(
        test_fixture->dynamic_context,
        &(sxml_evaluation_request){.unit = unit,
                                   .has_context_value = false,
                                   .context_value = sxml_sequence_empty(),
                                   .global_values = {.slots = NULL, .slot_count = 0},
                                   .static_base_uri = {.bytes = NULL, .byte_count = 0},
                                   .error_record = &error_record},
        &value);
    if (status == SXML_STATUS_OK) {
        render_value(output, value);
    } else if (sxml_status_has_error_record(status)) {
        render_text(output, "error:");
        render_bytes(output, error_record.code.local_name);
    }
    sxml_sequence_release(value);
    sxml_ir_unit_release(unit);
    return status;
}

static void expect_cases(sxml_test_state *state, const construct_case *cases,
                         size_t case_count) {
    for (size_t case_index = 0; case_index < case_count; case_index += 1) {
        const construct_case *test_case = &cases[case_index];
        fixture test_fixture;
        SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        rendering output;
        const sxml_status status = run_case(&test_fixture, test_case, &output);
        const bool is_expected =
            (status == SXML_STATUS_OK || sxml_status_has_error_record(status)) &&
            output.is_valid && strcmp(output.text, test_case->expected) == 0;
        if (!is_expected) {
            (void)fprintf(stderr, "case %s: status %s, got \"%s\", expected \"%s\"\n",
                          test_case->name, (const char *)sxml_status_name(status).bytes,
                          output.text, test_case->expected);
        }
        SXML_TEST_EXPECT(state, is_expected);
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
}

/* Runs every case failing each allocation in turn until one run gets as
   far as the expected text; every failure must be OUT_OF_MEMORY and
   release everything. */
static void expect_cases_survive_allocation_failures(sxml_test_state *state,
                                                     const construct_case *cases,
                                                     size_t case_count) {
    for (size_t case_index = 0; case_index < case_count; case_index += 1) {
        const construct_case *test_case = &cases[case_index];
        bool succeeded = false;
        for (uint64_t failing_allocation_number = 1;
             !succeeded && failing_allocation_number < 5000; failing_allocation_number += 1) {
            fixture test_fixture;
            sxml_status status = fixture_create(failing_allocation_number, &test_fixture);
            rendering output;
            output.text[0] = '\0';
            if (status == SXML_STATUS_OK) {
                status = run_case(&test_fixture, test_case, &output);
            }
            if (status == SXML_STATUS_OK && !output.is_valid) {
                /* Rendering itself ran out of memory (base URIs). */
                status = SXML_STATUS_OUT_OF_MEMORY;
            }
            succeeded = (status == SXML_STATUS_OK || sxml_status_has_error_record(status)) &&
                        strcmp(output.text, test_case->expected) == 0;
            if (!succeeded && status != SXML_STATUS_OUT_OF_MEMORY) {
                (void)fprintf(stderr, "case %s, failing allocation %llu: status %s, \"%s\"\n",
                              test_case->name, (unsigned long long)failing_allocation_number,
                              (const char *)sxml_status_name(status).bytes, output.text);
            }
            SXML_TEST_EXPECT(state, succeeded || status == SXML_STATUS_OUT_OF_MEMORY);
            SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
        }
        SXML_TEST_EXPECT(state, succeeded);
    }
}

/* ----------------------------------------------- Element content */

/* element a { 1, 2, "x" }: adjacent atomic items of one enclosed
   expression, one space between. */
static sxml_ir_expression_index write_atomic_content(program *writer) {
    const sxml_ir_atom values = program_bind(
        writer, PROGRAM_SEQUENCE(writer, program_integer(writer, 1), program_integer(writer, 2),
                                 program_string(writer, "x")));
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
                             with_content(program_name(writer, "a"), program_atom(writer, values)));
}

/* <a>{1}{2}{"x"}</a>: a SEQUENCE content lists enclosed expressions, whose
   atomic items are not joined by spaces (D110). */
static sxml_ir_expression_index write_enclosed_expressions(program *writer) {
    const sxml_ir_atom values = program_bind(
        writer, PROGRAM_SEQUENCE(writer, program_integer(writer, 2), program_integer(writer, 3)));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"),
                     PROGRAM_SEQUENCE(writer, program_integer(writer, 1), values,
                                      program_string(writer, "x"))));
}

/* <a>{text {"a"}}{1}{text {""}}{""}{("", "")}</a>: text nodes merge
   with the text of atomic items; empty strings of one enclosed expression
   are joined by a space. */
static sxml_ir_expression_index write_merged_text(program *writer) {
    const sxml_ir_atom first = bind_construct(writer, SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                                              named(no_atom(), program_string(writer, "a")));
    const sxml_ir_atom empty = bind_construct(writer, SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                                              named(no_atom(), program_string(writer, "")));
    const sxml_ir_atom one = program_integer(writer, 1);
    const sxml_ir_atom blank = program_string(writer, "");
    const sxml_ir_atom blanks = program_bind(writer, PROGRAM_SEQUENCE(writer, blank, blank));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"),
                     PROGRAM_SEQUENCE(writer, first, one, empty, blank, blanks)));
}

/* let $b := <b x="1"/> return (<a>{$b, $b}</a>, $b): copies, the original
   unchanged. */
static sxml_ir_expression_index write_copied_children(program *writer) {
    const sxml_ir_atom attribute =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_name(writer, "x"), program_integer(writer, 1)));
    const sxml_ir_atom inner = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "b"), PROGRAM_SEQUENCE(writer, attribute)));
    const sxml_ir_atom outer = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"), PROGRAM_SEQUENCE(writer, inner, inner)));
    return PROGRAM_SEQUENCE(writer, outer, inner);
}

/* element a { attribute x {"1"}, "t", <!--c-->, <?p d?> }. */
static sxml_ir_expression_index write_mixed_content(program *writer) {
    const sxml_ir_atom attribute =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_name(writer, "x"), program_string(writer, "1")));
    const sxml_ir_atom comment =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR,
                       named(no_atom(), program_string(writer, "c")));
    const sxml_ir_atom instruction =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR,
                       named(program_string(writer, "p"), program_string(writer, "d")));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"),
                     PROGRAM_SEQUENCE(writer, attribute, program_string(writer, "t"), comment,
                                      instruction)));
}

/* element a { "t", attribute x {} }: XQTY0024. */
static sxml_ir_expression_index write_late_attribute(program *writer) {
    const sxml_ir_atom attribute = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
        named(program_name(writer, "x"), (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE}));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"),
                     PROGRAM_SEQUENCE(writer, program_string(writer, "t"), attribute)));
}

/* element a { "", attribute x {} }: empty text is deleted before the
   check (XQuery 4.0, section 4.12.1.3, steps 3 and 4). */
static sxml_ir_expression_index write_attribute_after_empty_text(program *writer) {
    const sxml_ir_atom attribute =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_name(writer, "x"), no_atom()));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"),
                     PROGRAM_SEQUENCE(writer, program_string(writer, ""), attribute)));
}

/* element a { namespace p {"u"} , <b/> , namespace q {"v"} }: XQTY0024
   for a namespace node too. */
static sxml_ir_expression_index write_late_namespace(program *writer) {
    const sxml_ir_atom first = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
        named(program_string(writer, "p"), program_string(writer, "u")));
    const sxml_ir_atom child = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "b"), (sxml_ir_expression_index){.value = 0}));
    const sxml_ir_atom second = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
        named(program_string(writer, "q"), program_string(writer, "v")));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"), PROGRAM_SEQUENCE(writer, first, child, second)));
}

/* element a { attribute x {1}, attribute x {2} }: XQDY0025. */
static sxml_ir_expression_index write_duplicate_attributes(program *writer) {
    const sxml_ir_atom first =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_name(writer, "x"), program_integer(writer, 1)));
    const sxml_ir_atom second =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_name(writer, "x"), program_integer(writer, 2)));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"), PROGRAM_SEQUENCE(writer, first, second)));
}

/* element a { document { <b/>, "t" }, "u" }: a document node is replaced
   by its children. */
static sxml_ir_expression_index write_document_in_content(program *writer) {
    const sxml_ir_atom child = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "b"), (sxml_ir_expression_index){.value = 0}));
    const sxml_ir_atom document = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
        with_content(no_atom(), PROGRAM_SEQUENCE(writer, child, program_string(writer, "t"))));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"),
                     PROGRAM_SEQUENCE(writer, document, program_string(writer, "u"))));
}

/* element a { [1, [2, 3]], 4, [] }: arrays are flattened, and the atomic
   items stay adjacent across them. */
static sxml_ir_expression_index write_flattened_arrays(program *writer) {
    sxml_item inner = {.representation = 0};
    sxml_item outer = {.representation = 0};
    sxml_item empty = {.representation = 0};
    if (writer->status == SXML_STATUS_OK) {
        const sxml_sequence members[] = {
            sxml_sequence_from_item(sxml_atomic_item_from_integer(2)),
            sxml_sequence_from_item(sxml_atomic_item_from_integer(3))};
        writer->status = sxml_array_make(
            writer->test_fixture->item_heap,
            (sxml_sequence_view){.sequences = members, .sequence_count = 2}, &inner);
    }
    if (writer->status == SXML_STATUS_OK) {
        const sxml_sequence members[] = {
            sxml_sequence_from_item(sxml_atomic_item_from_integer(1)),
            sxml_sequence_from_item(inner)};
        writer->status = sxml_array_make(
            writer->test_fixture->item_heap,
            (sxml_sequence_view){.sequences = members, .sequence_count = 2}, &outer);
        sxml_item_release(inner);
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_array_make(writer->test_fixture->item_heap,
                                         (sxml_sequence_view){.sequences = NULL,
                                                              .sequence_count = 0},
                                         &empty);
        if (writer->status != SXML_STATUS_OK) {
            sxml_item_release(outer);
        }
    }
    if (writer->status != SXML_STATUS_OK) {
        return (sxml_ir_expression_index){.value = 0};
    }
    const sxml_ir_atom arrays = program_constant(writer, sxml_sequence_from_item(outer));
    const sxml_ir_atom nothing = program_constant(writer, sxml_sequence_from_item(empty));
    const sxml_ir_atom values = program_bind(
        writer, PROGRAM_SEQUENCE(writer, arrays, program_integer(writer, 4), nothing));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"), program_atom(writer, values)));
}

/* element a { map {} }: XQTY0105. */
static sxml_ir_expression_index write_function_in_content(program *writer) {
    sxml_item map = {.representation = 0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_map_make(
            writer->test_fixture->item_heap,
            &(sxml_map_request){.map_entries = {.map_entries = NULL, .map_entry_count = 0},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                                .error_record = NULL},
            &map);
    }
    if (writer->status != SXML_STATUS_OK) {
        return (sxml_ir_expression_index){.value = 0};
    }
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"),
                     PROGRAM_SEQUENCE(writer,
                                      program_constant(writer, sxml_sequence_from_item(map)))));
}

/* element a {} and document {}: no content expression at all. */
static sxml_ir_expression_index write_empty_nodes(program *writer) {
    const sxml_ir_atom element = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"), (sxml_ir_expression_index){.value = 0}));
    const sxml_ir_atom document =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                       with_content(no_atom(), (sxml_ir_expression_index){.value = 0}));
    return PROGRAM_SEQUENCE(writer, element, document);
}

static const construct_case element_content_cases[] = {
    {"atomic_content", write_atomic_content, SXML_TEST_RENDER_NONE, "<a>1 2 x</a>"},
    {"enclosed_expressions", write_enclosed_expressions, SXML_TEST_RENDER_NONE, "<a>12 3x</a>"},
    {"merged_text", write_merged_text, SXML_TEST_RENDER_NONE, "<a>a1 </a>"},
    {"copied_children", write_copied_children, SXML_TEST_RENDER_NONE,
     "<a><b x=\"1\"></b><b x=\"1\"></b></a> | <b x=\"1\"></b>"},
    {"mixed_content", write_mixed_content, SXML_TEST_RENDER_NONE,
     "<a x=\"1\">t<!--c--><?p d?></a>"},
    {"late_attribute", write_late_attribute, SXML_TEST_RENDER_NONE, "error:XQTY0024"},
    {"attribute_after_empty_text", write_attribute_after_empty_text, SXML_TEST_RENDER_NONE,
     "<a x=\"\"></a>"},
    {"late_namespace", write_late_namespace, SXML_TEST_RENDER_NONE, "error:XQTY0024"},
    {"duplicate_attributes", write_duplicate_attributes, SXML_TEST_RENDER_NONE, "error:XQDY0025"},
    {"document_in_content", write_document_in_content, SXML_TEST_RENDER_NONE,
     "<a><b></b>tu</a>"},
    {"flattened_arrays", write_flattened_arrays, SXML_TEST_RENDER_NONE, "<a>1 2 3 4</a>"},
    {"function_in_content", write_function_in_content, SXML_TEST_RENDER_NONE, "error:XQTY0105"},
    {"empty_nodes", write_empty_nodes, SXML_TEST_RENDER_NONE, "<a></a> | doc{}"},
};

static void test_element_content(sxml_test_state *state) {
    expect_cases(state, element_content_cases, SXML_TEST_COUNT(element_content_cases));
}

/* ------------------------------------------------------ Documents */

/* document { attribute x {} }: XPTY0004. */
static sxml_ir_expression_index write_document_attribute(program *writer) {
    const sxml_ir_atom attribute =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_name(writer, "x"), no_atom()));
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                             with_content(no_atom(), PROGRAM_SEQUENCE(writer, attribute)));
}

/* document { namespace p {"u"} }: XPTY0004. */
static sxml_ir_expression_index write_document_namespace(program *writer) {
    const sxml_ir_atom namespace_node = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
        named(program_string(writer, "p"), program_string(writer, "u")));
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                             with_content(no_atom(), PROGRAM_SEQUENCE(writer, namespace_node)));
}

/* <document>{1}{2}{<a/>}{document { "x" }}: atomic items of different
   enclosed expressions are not joined. */
static sxml_ir_expression_index write_document_content(program *writer) {
    const sxml_ir_atom element = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"), (sxml_ir_expression_index){.value = 0}));
    const sxml_ir_atom inner =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                       with_content(no_atom(), PROGRAM_SEQUENCE(writer, program_string(writer, "x"))));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
        with_content(no_atom(), PROGRAM_SEQUENCE(writer, program_integer(writer, 1),
                                                 program_integer(writer, 2), element, inner)));
}

static const construct_case document_cases[] = {
    {"document_attribute", write_document_attribute, SXML_TEST_RENDER_NONE, "error:XPTY0004"},
    {"document_namespace", write_document_namespace, SXML_TEST_RENDER_NONE, "error:XPTY0004"},
    {"document_content", write_document_content, SXML_TEST_RENDER_NONE, "doc{12<a></a>x}"},
};

static void test_documents(sxml_test_state *state) {
    expect_cases(state, document_cases, SXML_TEST_COUNT(document_cases));
}

/* ---------------------------------------------------------- Names */

static const qname_text static_namespaces[] = {
    {.namespace_uri = "http://p.example/", .namespace_prefix = "p", .local_name = ""},
    {.namespace_uri = "http://default.example/", .namespace_prefix = "", .local_name = ""},
};

/* A computed element and attribute named by text; with the static
   namespaces of the unit (p and a default element namespace). */
static sxml_ir_expression_index write_named(program *writer, typed_text element_name,
                                            typed_text attribute_name) {
    (void)program_set_static_namespaces(writer, static_namespaces,
                                        SXML_TEST_COUNT(static_namespaces));
    const sxml_ir_atom attribute =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_typed_string(writer, attribute_name), no_atom()));
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
                             with_content(program_typed_string(writer, element_name),
                                          PROGRAM_SEQUENCE(writer, attribute)));
}

static sxml_ir_expression_index write_lexical_names(program *writer) {
    return write_named(writer, (typed_text){" p:e ", SXML_TYPE_XS_STRING},
                       (typed_text){"p:x", SXML_TYPE_XS_UNTYPED_ATOMIC});
}

/* Unprefixed: the element takes the default element namespace, the
   attribute none. */
static sxml_ir_expression_index write_unprefixed_names(program *writer) {
    return write_named(writer, (typed_text){"e", SXML_TYPE_XS_STRING},
                       (typed_text){"x", SXML_TYPE_XS_STRING});
}

/* Q{uri}local and Q{uri}prefix:local; the attribute in a namespace without
   a prefix gets one. */
static sxml_ir_expression_index write_uri_qualified_names(program *writer) {
    return write_named(writer, (typed_text){"Q{http://q.example/}q:e", SXML_TYPE_XS_STRING},
                       (typed_text){"Q{http://r.example/}x", SXML_TYPE_XS_STRING});
}

static sxml_ir_expression_index write_unbound_prefix(program *writer) {
    return write_named(writer, (typed_text){"z:e", SXML_TYPE_XS_STRING},
                       (typed_text){"x", SXML_TYPE_XS_STRING});
}

static sxml_ir_expression_index write_invalid_name(program *writer) {
    return write_named(writer, (typed_text){"e f", SXML_TYPE_XS_STRING},
                       (typed_text){"x", SXML_TYPE_XS_STRING});
}

static sxml_ir_expression_index write_invalid_eqname(program *writer) {
    return write_named(writer, (typed_text){"Q{{}x", SXML_TYPE_XS_STRING},
                       (typed_text){"x", SXML_TYPE_XS_STRING});
}

static sxml_ir_expression_index write_invalid_attribute_name(program *writer) {
    return write_named(writer, (typed_text){"e", SXML_TYPE_XS_STRING},
                       (typed_text){"p:", SXML_TYPE_XS_STRING});
}

/* element { 1 } {}: XPTY0004. */
static sxml_ir_expression_index write_integer_name(program *writer) {
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
                             with_content(program_integer(writer, 1),
                                          (sxml_ir_expression_index){.value = 0}));
}

/* attribute { () } {}: XPTY0004. */
static sxml_ir_expression_index write_empty_name(program *writer) {
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                             named(no_atom(), no_atom()));
}

/* element { ("a", "b") } {}: XPTY0004. */
static sxml_ir_expression_index write_two_names(program *writer) {
    const sxml_ir_atom names = program_bind(
        writer, PROGRAM_SEQUENCE(writer, program_string(writer, "a"), program_string(writer, "b")));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(names, (sxml_ir_expression_index){.value = 0}));
}

/* element { QName("http://www.w3.org/2000/xmlns/", "e") } {}: XQDY0096. */
static sxml_ir_expression_index write_xmlns_element(program *writer) {
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_qname(writer, (qname_text){"http://www.w3.org/2000/xmlns/", "", "e"}),
                     (sxml_ir_expression_index){.value = 0}));
}

/* element { QName("http://other.example/", "xml:e") } {}: XQDY0096. */
static sxml_ir_expression_index write_misused_xml_element(program *writer) {
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_qname(writer, (qname_text){"http://other.example/", "xml", "e"}),
                     (sxml_ir_expression_index){.value = 0}));
}

/* attribute xmlns {}: XQDY0044. */
static sxml_ir_expression_index write_xmlns_attribute(program *writer) {
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                             named(program_string(writer, "xmlns"), no_atom()));
}

/* attribute { QName("http://www.w3.org/XML/1998/namespace", "space") }
   {"preserve"}: the xml prefix is supplied. */
static sxml_ir_expression_index write_xml_attribute(program *writer) {
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
        named(program_qname(writer,
                            (qname_text){"http://www.w3.org/XML/1998/namespace", "", "space"}),
              program_string(writer, "preserve")));
}

/* attribute { QName("http://www.w3.org/XML/1998/namespace", "p:x") } {}:
   XQDY0044. */
static sxml_ir_expression_index write_misused_xml_attribute(program *writer) {
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
        named(program_qname(writer, (qname_text){"http://www.w3.org/XML/1998/namespace", "p", "x"}),
              no_atom()));
}

static const construct_case name_cases[] = {
    {"lexical_names", write_lexical_names, SXML_TEST_RENDER_NAMESPACES,
     "<p:e[p=http://p.example/] p:x=\"\"></p:e>"},
    {"unprefixed_names", write_unprefixed_names, SXML_TEST_RENDER_NAMESPACES,
     "<Q{http://default.example/}e[=http://default.example/] x=\"\"></Q{http://default.example/}e>"},
    {"uri_qualified_names", write_uri_qualified_names, SXML_TEST_RENDER_NAMESPACES,
     "<q:e[ns0=http://r.example/ q=http://q.example/] ns0:x=\"\"></q:e>"},
    {"unbound_prefix", write_unbound_prefix, SXML_TEST_RENDER_NONE, "error:XQDY0074"},
    {"invalid_name", write_invalid_name, SXML_TEST_RENDER_NONE, "error:XQDY0074"},
    {"invalid_eqname", write_invalid_eqname, SXML_TEST_RENDER_NONE, "error:XQDY0074"},
    {"invalid_attribute_name", write_invalid_attribute_name, SXML_TEST_RENDER_NONE,
     "error:XQDY0074"},
    {"integer_name", write_integer_name, SXML_TEST_RENDER_NONE, "error:XPTY0004"},
    {"empty_name", write_empty_name, SXML_TEST_RENDER_NONE, "error:XPTY0004"},
    {"two_names", write_two_names, SXML_TEST_RENDER_NONE, "error:XPTY0004"},
    {"xmlns_element", write_xmlns_element, SXML_TEST_RENDER_NONE, "error:XQDY0096"},
    {"misused_xml_element", write_misused_xml_element, SXML_TEST_RENDER_NONE, "error:XQDY0096"},
    {"xmlns_attribute", write_xmlns_attribute, SXML_TEST_RENDER_NONE, "error:XQDY0044"},
    {"xml_attribute", write_xml_attribute, SXML_TEST_RENDER_NONE, "@xml:space=\"preserve\""},
    {"misused_xml_attribute", write_misused_xml_attribute, SXML_TEST_RENDER_NONE,
     "error:XQDY0044"},
};

static void test_names(sxml_test_state *state) {
    expect_cases(state, name_cases, SXML_TEST_COUNT(name_cases));
}

/* ----------------------------------------------------- Namespaces */

/* The bindings written on a direct element constructor, here the unit's
   static namespaces: <p:e xmlns:p="..." xmlns="...">. */
static sxml_ir_expression_index write_written_bindings(program *writer) {
    const sxml_ir_namespace_binding_list bindings = program_set_static_namespaces(
        writer, static_namespaces, SXML_TEST_COUNT(static_namespaces));
    sxml_ir_node_construction construction =
        with_content(program_qname(writer, (qname_text){"http://p.example/", "p", "e"}),
                     (sxml_ir_expression_index){.value = 0});
    construction.namespace_bindings = bindings;
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR, construction);
}

/* element e { namespace r {"http://r.example/"}, namespace {""}
   {"http://d.example/"} }: namespace nodes add bindings; the element
   name is in no namespace, so a default namespace node conflicts
   (XQDY0102), unless the element is in a namespace. */
static sxml_ir_expression_index write_namespace_nodes(program *writer) {
    const sxml_ir_atom prefixed = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
        named(program_string(writer, "r"), program_string(writer, "http://r.example/")));
    const sxml_ir_atom default_namespace = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
        named(program_string(writer, ""), program_string(writer, "http://d.example/")));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_qname(writer, (qname_text){"http://d.example/", "", "e"}),
                     PROGRAM_SEQUENCE(writer, prefixed, default_namespace)));
}

static sxml_ir_expression_index write_default_namespace_conflict(program *writer) {
    const sxml_ir_atom default_namespace = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
        named(no_atom(), program_string(writer, "http://d.example/")));
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
                             with_content(program_name(writer, "e"),
                                          PROGRAM_SEQUENCE(writer, default_namespace)));
}

/* element e { namespace p {"u"}, namespace p {"v"} }: XQDY0102. */
static sxml_ir_expression_index write_conflicting_namespaces(program *writer) {
    const sxml_ir_atom first = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
        named(program_string(writer, "p"), program_string(writer, "http://u.example/")));
    const sxml_ir_atom second = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
        named(program_string(writer, "p"), program_string(writer, "http://v.example/")));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "e"), PROGRAM_SEQUENCE(writer, first, second)));
}

/* namespace p {" http://p.example/ "}: a parentless namespace node. */
static sxml_ir_expression_index write_parentless_namespace(program *writer) {
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
        named(program_typed_string(writer, (typed_text){"p", SXML_TYPE_XS_UNTYPED_ATOMIC}),
              program_string(writer, " http://p.example/ ")));
}

static sxml_ir_expression_index write_empty_namespace_uri(program *writer) {
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
                             named(program_string(writer, "p"), program_string(writer, "")));
}

static sxml_ir_expression_index write_xmlns_namespace(program *writer) {
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
                             named(program_string(writer, "xmlns"),
                                   program_string(writer, "http://u.example/")));
}

static sxml_ir_expression_index write_misused_xml_namespace(program *writer) {
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
                             named(program_string(writer, "p"),
                                   program_string(writer, "http://www.w3.org/XML/1998/namespace")));
}

static sxml_ir_expression_index write_invalid_namespace_prefix(program *writer) {
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
                             named(program_string(writer, "1p"),
                                   program_string(writer, "http://u.example/")));
}

/* namespace p { 1 }: XPTY0004 (not a string). */
static sxml_ir_expression_index write_integer_namespace_uri(program *writer) {
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
                             named(program_string(writer, "p"), program_integer(writer, 1)));
}

static const construct_case namespace_cases[] = {
    {"written_bindings", write_written_bindings, SXML_TEST_RENDER_NAMESPACES,
     "<p:e[=http://default.example/ p=http://p.example/]></p:e>"},
    {"namespace_nodes", write_namespace_nodes, SXML_TEST_RENDER_NAMESPACES,
     "<Q{http://d.example/}e[=http://d.example/ r=http://r.example/]></Q{http://d.example/}e>"},
    {"default_namespace_conflict", write_default_namespace_conflict, SXML_TEST_RENDER_NONE,
     "error:XQDY0102"},
    {"conflicting_namespaces", write_conflicting_namespaces, SXML_TEST_RENDER_NONE,
     "error:XQDY0102"},
    {"parentless_namespace", write_parentless_namespace, SXML_TEST_RENDER_NONE,
     "namespace(p=http://p.example/)"},
    {"empty_namespace_uri", write_empty_namespace_uri, SXML_TEST_RENDER_NONE, "error:XQDY0101"},
    {"xmlns_namespace", write_xmlns_namespace, SXML_TEST_RENDER_NONE, "error:XQDY0101"},
    {"misused_xml_namespace", write_misused_xml_namespace, SXML_TEST_RENDER_NONE,
     "error:XQDY0101"},
    {"invalid_namespace_prefix", write_invalid_namespace_prefix, SXML_TEST_RENDER_NONE,
     "error:XQDY0074"},
    {"integer_namespace_uri", write_integer_namespace_uri, SXML_TEST_RENDER_NONE,
     "error:XPTY0004"},
};

static void test_namespaces(sxml_test_state *state) {
    expect_cases(state, namespace_cases, SXML_TEST_COUNT(namespace_cases));
}

/* ------------------------------------------------- Copy namespaces */

/* let $b := <q:b xmlns:q="..."/> return <a xmlns:p="...">{$b}</a>: the
   copy keeps its own bindings (preserve) and takes the new parent's
   (inherit); a binding the copy does not use is kept too. */
static sxml_ir_expression_index write_copy_namespaces(program *writer) {
    static const qname_text bindings[] = {
        {.namespace_uri = "http://p.example/", .namespace_prefix = "p", .local_name = ""},
    };
    const sxml_ir_namespace_binding_list written =
        program_set_static_namespaces(writer, bindings, SXML_TEST_COUNT(bindings));
    const sxml_ir_atom unused = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
        named(program_string(writer, "s"), program_string(writer, "http://s.example/")));
    const sxml_ir_atom inner = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_qname(writer, (qname_text){"http://q.example/", "q", "b"}),
                     PROGRAM_SEQUENCE(writer, unused)));
    sxml_ir_node_construction outer =
        with_content(program_name(writer, "a"), PROGRAM_SEQUENCE(writer, inner));
    outer.namespace_bindings = written;
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR, outer);
}

/* Construction strip: constructed and copied nodes are untyped. */
static sxml_ir_expression_index write_untyped_nodes(program *writer) {
    const sxml_ir_atom attribute =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_name(writer, "x"), program_integer(writer, 1)));
    const sxml_ir_atom inner = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "b"), PROGRAM_SEQUENCE(writer, attribute)));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"), PROGRAM_SEQUENCE(writer, attribute, inner)));
}

static const construct_case copy_cases[] = {
    {"copy_namespaces", write_copy_namespaces, SXML_TEST_RENDER_NAMESPACES,
     "<a[p=http://p.example/]><q:b[p=http://p.example/ q=http://q.example/ "
     "s=http://s.example/]></q:b></a>"},
    {"untyped_nodes", write_untyped_nodes, SXML_TEST_RENDER_TYPES,
     "<a#untyped x=\"1\"#untyped><b#untyped x=\"1\"#untyped></b></a>"},
};

static void test_copies(sxml_test_state *state) {
    expect_cases(state, copy_cases, SXML_TEST_COUNT(copy_cases));
}

/* ------------------------------------------------------ Base URIs */

/* With the static base URI http://example.com/base/: elements and
   documents take it, xml:base resolves against it, a comment, text,
   attribute or processing instruction has none; a copied element takes
   its new parent's. */
static sxml_ir_expression_index write_base_uris(program *writer) {
    program_set_base_uri(writer, "http://example.com/base/");
    const sxml_ir_atom base_attribute = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
        named(program_qname(writer,
                            (qname_text){"http://www.w3.org/XML/1998/namespace", "xml", "base"}),
              program_string(writer, "sub/")));
    const sxml_ir_atom based = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "b"), PROGRAM_SEQUENCE(writer, base_attribute)));
    const sxml_ir_atom plain = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"), (sxml_ir_expression_index){.value = 0}));
    const sxml_ir_atom document =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                       with_content(no_atom(), (sxml_ir_expression_index){.value = 0}));
    const sxml_ir_atom comment =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR,
                       named(no_atom(), program_string(writer, "c")));
    const sxml_ir_atom instruction =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR,
                       named(program_string(writer, "p"), no_atom()));
    return PROGRAM_SEQUENCE(writer, plain, based, document, comment, instruction);
}

static const construct_case base_uri_cases[] = {
    {"base_uris", write_base_uris, SXML_TEST_RENDER_BASE_URI,
     "{http://example.com/base/}<a></a> | {http://example.com/base/sub/}<b "
     "xml:base=\"sub/\"></b> | {http://example.com/base/}doc{} | {}<!--c--> | {}<?p ?>"},
};

static void test_base_uris(sxml_test_state *state) {
    expect_cases(state, base_uri_cases, SXML_TEST_COUNT(base_uri_cases));
}

/* ------------------------------------------------- Leaf kinds */

static sxml_ir_expression_index write_texts(program *writer) {
    const sxml_ir_atom values = program_bind(
        writer, PROGRAM_SEQUENCE(writer, program_integer(writer, 1), program_string(writer, "b")));
    const sxml_ir_atom joined = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR, named(no_atom(), values));
    const sxml_ir_atom empty = bind_construct(writer, SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                                              named(no_atom(), no_atom()));
    const sxml_ir_atom blank = bind_construct(writer, SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                                              named(no_atom(), program_string(writer, "")));
    return PROGRAM_SEQUENCE(writer, joined, empty, blank, program_string(writer, "end"));
}

static sxml_ir_expression_index write_attribute_value(program *writer) {
    const sxml_ir_atom values = program_bind(
        writer, PROGRAM_SEQUENCE(writer, program_integer(writer, 1), program_string(writer, "b")));
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                             named(program_name(writer, "x"), values));
}

/* attribute xml:id {"  a   b "}: whitespace collapsed, an ID. */
static sxml_ir_expression_index write_xml_id(program *writer) {
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
        named(program_qname(writer, (qname_text){"http://www.w3.org/XML/1998/namespace", "xml", "id"}),
              program_string(writer, "  a   b ")));
}

static sxml_ir_expression_index write_comment(program *writer) {
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR,
                             named(no_atom(), program_string(writer, "a - b")));
}

static sxml_ir_expression_index write_double_hyphen_comment(program *writer) {
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR,
                             named(no_atom(), program_string(writer, "a--b")));
}

static sxml_ir_expression_index write_trailing_hyphen_comment(program *writer) {
    return program_construct(writer, SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR,
                             named(no_atom(), program_string(writer, "a-")));
}

/* processing-instruction { " t " } { "  d ?", ">" }: target trimmed,
   leading whitespace of the content removed. */
static sxml_ir_expression_index write_instruction(program *writer) {
    const sxml_ir_atom values = program_bind(
        writer, PROGRAM_SEQUENCE(writer, program_string(writer, "  d ?"), program_string(writer, ">")));
    return program_construct(writer,
                             SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR,
                             named(program_string(writer, " t "), values));
}

static sxml_ir_expression_index write_instruction_end(program *writer) {
    return program_construct(writer,
                             SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR,
                             named(program_string(writer, "t"), program_string(writer, "a?>b")));
}

static sxml_ir_expression_index write_xml_target(program *writer) {
    return program_construct(writer,
                             SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR,
                             named(program_string(writer, "XmL"), no_atom()));
}

static sxml_ir_expression_index write_invalid_target(program *writer) {
    return program_construct(writer,
                             SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR,
                             named(program_string(writer, "p:i"), no_atom()));
}

static sxml_ir_expression_index write_qname_target(program *writer) {
    return program_construct(writer,
                             SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR,
                             named(program_name(writer, "t"), no_atom()));
}

static const construct_case leaf_cases[] = {
    {"texts", write_texts, SXML_TEST_RENDER_NONE, "1 b | end"},
    {"attribute_value", write_attribute_value, SXML_TEST_RENDER_TYPES, "@x=\"1 b\"#untyped"},
    {"xml_id", write_xml_id, SXML_TEST_RENDER_IDS, "@xml:id=\"a b\"#id"},
    {"comment", write_comment, SXML_TEST_RENDER_NONE, "<!--a - b-->"},
    {"double_hyphen_comment", write_double_hyphen_comment, SXML_TEST_RENDER_NONE,
     "error:XQDY0072"},
    {"trailing_hyphen_comment", write_trailing_hyphen_comment, SXML_TEST_RENDER_NONE,
     "error:XQDY0072"},
    {"instruction", write_instruction, SXML_TEST_RENDER_NONE, "<?t d ? >?>"},
    {"instruction_end", write_instruction_end, SXML_TEST_RENDER_NONE, "error:XQDY0026"},
    {"xml_target", write_xml_target, SXML_TEST_RENDER_NONE, "error:XQDY0064"},
    {"invalid_target", write_invalid_target, SXML_TEST_RENDER_NONE, "error:XQDY0041"},
    {"qname_target", write_qname_target, SXML_TEST_RENDER_NONE, "error:XPTY0004"},
};

static void test_leaf_kinds(sxml_test_state *state) {
    expect_cases(state, leaf_cases, SXML_TEST_COUNT(leaf_cases));
}

/* ------------------------------------------------------ Shallow copy */

/* let $e := <p:e xmlns:p="..." x="1">t</p:e> return
   <xsl:copy select="$e"> attribute x {2}, attribute y {3}, "u" </xsl:copy>:
   name and namespaces kept, children not, a later attribute replaces. */
static sxml_ir_expression_index write_shallow_element(program *writer) {
    const sxml_ir_atom first =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_name(writer, "x"), program_integer(writer, 1)));
    const sxml_ir_atom unused = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
        named(program_string(writer, "s"), program_string(writer, "http://s.example/")));
    const sxml_ir_atom element = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_qname(writer, (qname_text){"http://p.example/", "p", "e"}),
                     PROGRAM_SEQUENCE(writer, unused, first, program_string(writer, "t"))));
    const sxml_ir_atom second =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_name(writer, "x"), program_integer(writer, 2)));
    const sxml_ir_atom third =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_name(writer, "y"), program_integer(writer, 3)));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_SHALLOW_COPY,
        (sxml_ir_node_construction){
            .name = no_atom(),
            .value = element,
            .content = PROGRAM_SEQUENCE(writer, second, third, program_string(writer, "u")),
            .namespace_bindings = {.first_namespace_binding_index = 0,
                                   .namespace_binding_count = 0}});
}

/* Copies of a document, an attribute, an atomic item and the empty
   sequence; content of the leaf kinds is not evaluated. */
static sxml_ir_expression_index write_shallow_others(program *writer) {
    const sxml_ir_atom child = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "c"), (sxml_ir_expression_index){.value = 0}));
    const sxml_ir_atom document = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
        with_content(no_atom(), PROGRAM_SEQUENCE(writer, child)));
    const sxml_ir_atom attribute =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_name(writer, "x"), program_integer(writer, 1)));
    const sxml_ir_expression_index new_content =
        PROGRAM_SEQUENCE(writer, program_string(writer, "new"));
    const sxml_ir_atom inputs[] = {document, attribute, program_integer(writer, 7), no_atom()};
    sxml_ir_atom copies[4];
    for (size_t input_index = 0; input_index < SXML_TEST_COUNT(inputs); input_index += 1) {
        copies[input_index] = bind_construct(
            writer, SXML_IR_EXPRESSION_KIND_SHALLOW_COPY,
            (sxml_ir_node_construction){.name = no_atom(),
                                        .value = inputs[input_index],
                                        .content = new_content,
                                        .namespace_bindings = {.first_namespace_binding_index = 0,
                                                               .namespace_binding_count = 0}});
    }
    return PROGRAM_SEQUENCE(writer, copies[0], copies[1], copies[2], copies[3]);
}

/* xsl:copy of a document with an attribute in its content: XTDE0420. */
static sxml_ir_expression_index write_shallow_document_attribute(program *writer) {
    const sxml_ir_atom document =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                       with_content(no_atom(), (sxml_ir_expression_index){.value = 0}));
    const sxml_ir_atom attribute =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_name(writer, "x"), program_integer(writer, 1)));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_SHALLOW_COPY,
        (sxml_ir_node_construction){.name = no_atom(),
                                    .value = document,
                                    .content = PROGRAM_SEQUENCE(writer, attribute),
                                    .namespace_bindings = {.first_namespace_binding_index = 0,
                                                           .namespace_binding_count = 0}});
}

/* xsl:copy of an element with an attribute after text: XTDE0410. */
static sxml_ir_expression_index write_shallow_late_attribute(program *writer) {
    const sxml_ir_atom element = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "e"), (sxml_ir_expression_index){.value = 0}));
    const sxml_ir_atom attribute =
        bind_construct(writer, SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                       named(program_name(writer, "x"), program_integer(writer, 1)));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_SHALLOW_COPY,
        (sxml_ir_node_construction){
            .name = no_atom(),
            .value = element,
            .content = PROGRAM_SEQUENCE(writer, program_string(writer, "t"), attribute),
            .namespace_bindings = {.first_namespace_binding_index = 0,
                                   .namespace_binding_count = 0}});
}

/* xsl:copy of two items: XTTE3180. */
static sxml_ir_expression_index write_shallow_two_items(program *writer) {
    const sxml_ir_atom items = program_bind(
        writer, PROGRAM_SEQUENCE(writer, program_integer(writer, 1), program_integer(writer, 2)));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_SHALLOW_COPY,
        (sxml_ir_node_construction){.name = no_atom(),
                                    .value = items,
                                    .content = {.value = 0},
                                    .namespace_bindings = {.first_namespace_binding_index = 0,
                                                           .namespace_binding_count = 0}});
}

/* xsl:copy of an element with a map in its content: XTDE0450. */
static sxml_ir_expression_index write_shallow_function(program *writer) {
    sxml_item map = {.representation = 0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_map_make(
            writer->test_fixture->item_heap,
            &(sxml_map_request){.map_entries = {.map_entries = NULL, .map_entry_count = 0},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                                .error_record = NULL},
            &map);
    }
    if (writer->status != SXML_STATUS_OK) {
        return (sxml_ir_expression_index){.value = 0};
    }
    const sxml_ir_atom map_atom = program_constant(writer, sxml_sequence_from_item(map));
    const sxml_ir_atom element = bind_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "e"), (sxml_ir_expression_index){.value = 0}));
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_SHALLOW_COPY,
        (sxml_ir_node_construction){.name = no_atom(),
                                    .value = element,
                                    .content = PROGRAM_SEQUENCE(writer, map_atom),
                                    .namespace_bindings = {.first_namespace_binding_index = 0,
                                                           .namespace_binding_count = 0}});
}

static const construct_case shallow_copy_cases[] = {
    {"shallow_element", write_shallow_element, SXML_TEST_RENDER_NAMESPACES,
     "<p:e[p=http://p.example/ s=http://s.example/] x=\"2\" y=\"3\">u</p:e>"},
    {"shallow_others", write_shallow_others, SXML_TEST_RENDER_NONE, "doc{new} | @x=\"1\" | 7"},
    {"shallow_document_attribute", write_shallow_document_attribute, SXML_TEST_RENDER_NONE,
     "error:XTDE0420"},
    {"shallow_late_attribute", write_shallow_late_attribute, SXML_TEST_RENDER_NONE,
     "error:XTDE0410"},
    {"shallow_two_items", write_shallow_two_items, SXML_TEST_RENDER_NONE, "error:XTTE3180"},
    {"shallow_function", write_shallow_function, SXML_TEST_RENDER_NONE, "error:XTDE0450"},
};

static void test_shallow_copies(sxml_test_state *state) {
    expect_cases(state, shallow_copy_cases, SXML_TEST_COUNT(shallow_copy_cases));
}

/* -------------------------------------------------------- No recursion */

#define NESTED_ARRAY_DEPTH 20000U

/* element a { [[[ ... [1] ... ]]] } 20,000 arrays deep: flattened with
   the explicit stack of content frames (D71). */
static sxml_ir_expression_index write_deep_arrays(program *writer) {
    sxml_item array = {.representation = 0};
    sxml_sequence member = sxml_sequence_from_item(sxml_atomic_item_from_integer(1));
    for (uint32_t depth = 0; writer->status == SXML_STATUS_OK && depth < NESTED_ARRAY_DEPTH;
         depth += 1) {
        writer->status = sxml_array_make(
            writer->test_fixture->item_heap,
            (sxml_sequence_view){.sequences = &member, .sequence_count = 1}, &array);
        sxml_sequence_release(member);
        member = writer->status == SXML_STATUS_OK ? sxml_sequence_from_item(array)
                                                  : sxml_sequence_empty();
    }
    if (writer->status != SXML_STATUS_OK) {
        sxml_sequence_release(member);
        return (sxml_ir_expression_index){.value = 0};
    }
    return program_construct(
        writer, SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
        with_content(program_name(writer, "a"),
                     PROGRAM_SEQUENCE(writer, program_constant(writer, member))));
}

static const construct_case deep_cases[] = {
    {"deep_arrays", write_deep_arrays, SXML_TEST_RENDER_NONE, "<a>1</a>"},
};

static void test_deep_arrays(sxml_test_state *state) {
    expect_cases(state, deep_cases, SXML_TEST_COUNT(deep_cases));
}

/* ------------------------------------------------ Allocation failures */

static void test_cases_survive_allocation_failures(sxml_test_state *state) {
    expect_cases_survive_allocation_failures(state, element_content_cases,
                                             SXML_TEST_COUNT(element_content_cases));
    expect_cases_survive_allocation_failures(state, document_cases,
                                             SXML_TEST_COUNT(document_cases));
    expect_cases_survive_allocation_failures(state, name_cases, SXML_TEST_COUNT(name_cases));
    expect_cases_survive_allocation_failures(state, namespace_cases,
                                             SXML_TEST_COUNT(namespace_cases));
    expect_cases_survive_allocation_failures(state, copy_cases, SXML_TEST_COUNT(copy_cases));
    expect_cases_survive_allocation_failures(state, base_uri_cases,
                                             SXML_TEST_COUNT(base_uri_cases));
    expect_cases_survive_allocation_failures(state, leaf_cases, SXML_TEST_COUNT(leaf_cases));
    expect_cases_survive_allocation_failures(state, shallow_copy_cases,
                                             SXML_TEST_COUNT(shallow_copy_cases));
}

static const sxml_test_case evaluator_construct_test_cases[] = {
    {.name = "element_content", .function = test_element_content},
    {.name = "documents", .function = test_documents},
    {.name = "names", .function = test_names},
    {.name = "namespaces", .function = test_namespaces},
    {.name = "copies", .function = test_copies},
    {.name = "base_uris", .function = test_base_uris},
    {.name = "leaf_kinds", .function = test_leaf_kinds},
    {.name = "shallow_copies", .function = test_shallow_copies},
    {.name = "deep_arrays", .function = test_deep_arrays},
    {.name = "cases_survive_allocation_failures",
     .function = test_cases_survive_allocation_failures},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "evaluator_construct",
            .cases = evaluator_construct_test_cases,
            .case_count = SXML_TEST_COUNT(evaluator_construct_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
