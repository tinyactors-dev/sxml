/*
 * Tests for the function library (src/function_library.c and the core parts
 * src/function_library_{numeric,string,sequence,node,context,error}.c,
 * src/function_library_deep_equal.c, src/function_library_case.c; D55,
 * D78).
 *
 * Golden file: tests/memory_test.c (fixture, lifecycle, allocation-failure
 * loops), with the compilation and evaluation of tests/evaluator_test.c.
 * Expressions are compiled with sxml_xpath_compile against a function
 * library of the six core parts, evaluated, and their values written as
 * text: strings in double quotes, other atomic items in canonical form,
 * nodes by local name, separated by spaces. Errors are checked by code.
 * Every case checks that the item heap's cells and the allocator's blocks
 * are all released. Slice queries are named by their QT4 test case.
 */
#include "cast_internal.h"
#include "evaluator_internal.h"
#include "function_library_internal.h"
#include "sxml/name.h"
#include "sxml/processor.h"
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
    sxml_function_library *library;
} fixture;

static const sxml_native_function_view *const core_parts[] = {
    &sxml_function_library_numeric, &sxml_function_library_string,
    &sxml_function_library_sequence, &sxml_function_library_node,
    &sxml_function_library_context, &sxml_function_library_error,
};

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_function_library_configuration library_configuration(
    const fixture *test_fixture) {
    return (sxml_function_library_configuration){
        .allocator = fixture_allocator(test_fixture),
        .item_heap = test_fixture->item_heap,
        .parts = {.native_function_views = core_parts,
                  .native_function_view_count = SXML_TEST_COUNT(core_parts)}};
}

/* How to make a fixture: the counting allocator's failing allocation and
   whether the library is made too. */
typedef struct fixture_request {
    uint64_t failing_allocation_number;
    bool is_library_wanted;
} fixture_request;

static const fixture_request with_library = {.failing_allocation_number = 0,
                                            .is_library_wanted = true};
static const fixture_request without_library = {.failing_allocation_number = 0,
                                               .is_library_wanted = false};

static sxml_status fixture_create(fixture_request request, fixture *test_fixture) {
    const uint64_t failing_allocation_number = request.failing_allocation_number;
    const bool is_library_wanted = request.is_library_wanted;
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
    if (status == SXML_STATUS_OK && is_library_wanted) {
        const sxml_function_library_configuration configuration =
            library_configuration(test_fixture);
        status = sxml_function_library_create(fixture_allocator(test_fixture), &configuration,
                                              &test_fixture->library);
    }
    return status;
}

/* Destroys the fixture; true when every block of the counting allocator
   was released. */
static bool fixture_destroy(fixture *test_fixture) {
    sxml_function_library_destroy(test_fixture->library);
    test_fixture->library = NULL;
    sxml_item_heap_destroy(test_fixture->item_heap);
    sxml_type_registry_destroy(test_fixture->registry);
    sxml_name_pool_destroy(test_fixture->name_pool);
    bool is_clean = true;
    if (test_fixture->counting_allocator != NULL) {
        const uint64_t live_allocation_count =
            sxml_counting_allocator_read_counts(test_fixture->counting_allocator)
                .live_allocation_count;
        if (live_allocation_count != 0) {
            (void)fprintf(stderr, "leak: %llu blocks\n",
                          (unsigned long long)live_allocation_count);
        }
        is_clean = live_allocation_count == 0;
    }
    sxml_counting_allocator_finalize(test_fixture->counting_allocator);
    return is_clean;
}

/* True when every cell of the heap is released. */
static bool fixture_released_cells(const fixture *test_fixture) {
    sxml_item_heap_clear_raised_error(test_fixture->item_heap);
    const uint64_t live_cell_count =
        sxml_item_heap_read_counts(test_fixture->item_heap).live_cell_count;
    if (live_cell_count != 0) {
        (void)fprintf(stderr, "leak: %llu cells\n", (unsigned long long)live_cell_count);
    }
    return live_cell_count == 0;
}

/* Blocks the counting allocator holds now. */
static uint64_t live_blocks(const fixture *test_fixture) {
    return sxml_counting_allocator_read_counts(test_fixture->counting_allocator)
        .live_allocation_count;
}

static sxml_byte_view text_view(const char *text) {
    return sxml_byte_view_from_c_string(text);
}

/* ----------------------------------------------------- Mini documents */

static const sxml_source_location no_location = {
    .source_name = {.bytes = NULL, .byte_count = 0},
    .line_number = 0,
    .column_number = 0,
    .byte_offset = 0};

static sxml_status intern_local_name(const fixture *test_fixture, sxml_byte_view local_name,
                                     sxml_name_identifier *name) {
    return sxml_name_pool_intern_name(
        test_fixture->name_pool,
        &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                             .local_name = local_name},
        name);
}

static sxml_byte_view read_name(const char *text, size_t *position) {
    const size_t start = *position;
    while ((text[*position] >= 'a' && text[*position] <= 'z') ||
           (text[*position] >= 'A' && text[*position] <= 'Z') ||
           (text[*position] >= '0' && text[*position] <= '9')) {
        *position += 1;
    }
    return (sxml_byte_view){.bytes = (const uint8_t *)text + start,
                            .byte_count = *position - start};
}

/* Sends the events of a tiny XML subset (elements, attributes in double
   quotes, text, comments "<!--x-->"; no entities) to builder. */
static sxml_status send_mini_xml(const fixture *test_fixture, sxml_tree_builder builder,
                                 const char *text) {
    sxml_status status = builder.operations->start_document(
        builder.state, &(sxml_document_start_event){.document_uri = text_view(""),
                                                    .base_uri = text_view(""),
                                                    .location = no_location});
    size_t position = 0;
    while (status == SXML_STATUS_OK && text[position] != '\0') {
        if (text[position] != '<') {
            const size_t start = position;
            while (text[position] != '\0' && text[position] != '<') {
                position += 1;
            }
            status = builder.operations->add_text(
                builder.state,
                &(sxml_text_event){.text = {.bytes = (const uint8_t *)text + start,
                                            .byte_count = position - start},
                                   .location = no_location});
            continue;
        }
        position += 1;
        if (text[position] == '!') {
            position += 3;
            const size_t start = position;
            while (strncmp(text + position, "-->", 3) != 0) {
                position += 1;
            }
            status = builder.operations->add_comment(
                builder.state,
                &(sxml_comment_event){.comment = {.bytes = (const uint8_t *)text + start,
                                                  .byte_count = position - start},
                                      .location = no_location});
            position += 3;
            continue;
        }
        if (text[position] == '/') {
            while (text[position] != '>') {
                position += 1;
            }
            position += 1;
            status = builder.operations->end_element(builder.state);
            continue;
        }
        sxml_name_identifier element_name = {.index = 0};
        status = intern_local_name(test_fixture, read_name(text, &position), &element_name);
        if (status == SXML_STATUS_OK) {
            status = builder.operations->start_element(
                builder.state, &(sxml_element_start_event){
                                   .element_name = element_name,
                                   .namespace_prefix = {.index = 0},
                                   .type_annotation = {.index = SXML_TYPE_NONE},
                                   .location = no_location});
        }
        while (status == SXML_STATUS_OK && text[position] == ' ') {
            position += 1;
            sxml_name_identifier attribute_name = {.index = 0};
            status = intern_local_name(test_fixture, read_name(text, &position),
                                       &attribute_name);
            position += 2;
            const size_t value_start = position;
            while (text[position] != '"') {
                position += 1;
            }
            if (status == SXML_STATUS_OK) {
                status = builder.operations->add_attribute(
                    builder.state,
                    &(sxml_attribute_event){
                        .attribute_name = attribute_name,
                        .namespace_prefix = {.index = 0},
                        .attribute_value = {.bytes = (const uint8_t *)text + value_start,
                                            .byte_count = position - value_start},
                        .type_annotation = {.index = SXML_TYPE_NONE},
                        .location = no_location});
            }
            position += 1;
        }
        if (status == SXML_STATUS_OK && text[position] == '/') {
            position += 1;
            status = builder.operations->end_element(builder.state);
        }
        position += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_document(builder.state);
    }
    return status;
}

/* Builds a document from mini XML; *document is its document node,
   owned. */
static sxml_status build_document(const fixture *test_fixture, const char *text,
                                  sxml_item *document) {
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        sxml_default_allocator(),
        &(sxml_arena_tree_configuration){
            .allocator = sxml_default_allocator(),
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = test_fixture->item_heap,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = true,
            .error_record = NULL},
        &arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = send_mini_xml(test_fixture, sxml_arena_tree_as_tree_builder(arena_tree), text);
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document = sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ Document loader */

/* A loader double: "a.xml" and "b.xml" are two documents, made once per
   evaluation; any other URI is FODC0002. */
typedef struct test_loader {
    const fixture *test_fixture;
    sxml_item documents[2];
} test_loader;

static const sxml_static_error test_unavailable_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER("No such document in the test loader"),
};

static sxml_status load_test_document(void *state, const sxml_document_request *request,
                                      sxml_item *document_node) {
    test_loader *loader = state;
    size_t document_index = 2;
    if (sxml_byte_view_equals(request->uri_reference, text_view("a.xml"))) {
        document_index = 0;
    } else if (sxml_byte_view_equals(request->uri_reference, text_view("b.xml"))) {
        document_index = 1;
    }
    if (document_index == 2) {
        return sxml_error_record_report(request->error_record, &test_unavailable_error);
    }
    if (loader->documents[document_index].representation == 0) {
        sxml_status status = build_document(
            loader->test_fixture, "<a><b/>x</a>",
            &loader->documents[document_index]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_item_retain(loader->documents[document_index]);
    *document_node = loader->documents[document_index];
    return SXML_STATUS_OK;
}

static sxml_status parse_test_document(void *state,
                                       const sxml_document_parse_request *request,
                                       sxml_item *document_node) {
    (void)state;
    (void)request;
    (void)document_node;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

static const sxml_document_loader_operations test_loader_operations = {
    .load_document = load_test_document,
    .parse_document = parse_test_document,
};

/* ------------------------------------------------------- Evaluating */

/* 2026-10-16T12:30:45.5-05:00 with implicit timezone +02:00. */
static const sxml_clock_reading test_clock_reading = {
    .current_date_time = {.year = 2026,
                          .month = 10,
                          .day = 16,
                          .hour = 12,
                          .minute = 30,
                          .second = 45,
                          .nanosecond = 500000000,
                          .has_timezone = true,
                          .timezone_offset_minutes = -300},
    .implicit_timezone_offset_minutes = 120};

/* A clock double: reads test_clock_reading (sxml_fixed_clock arrives with
   the processor, packet 1.23). */
static sxml_status read_test_clock(void *state, sxml_clock_reading *reading) {
    *reading = *(const sxml_clock_reading *)state;
    return SXML_STATUS_OK;
}

static const sxml_clock_operations test_clock_operations = {.read = read_test_clock};

/* An expression with an optional context value. */
typedef struct evaluation_case {
    const char *expression_text;
    bool has_context_value;
    sxml_sequence context_value;
} evaluation_case;

typedef struct evaluation_outcome {
    sxml_status status;
    char code[32];
    char text[4096];
} evaluation_outcome;

static void outcome_append(evaluation_outcome *outcome, sxml_byte_view text) {
    const size_t used = strlen(outcome->text);
    size_t byte_count = text.byte_count;
    if (used + byte_count + 1 > sizeof(outcome->text)) {
        byte_count = sizeof(outcome->text) - used - 1;
    }
    if (byte_count > 0) {
        memcpy(outcome->text + used, text.bytes, byte_count);
    }
    outcome->text[used + byte_count] = '\0';
}

static sxml_status write_atomic(const fixture *test_fixture, sxml_item item,
                                evaluation_outcome *outcome) {
    uint8_t output[256];
    sxml_canonical_text canonical;
    sxml_status status = sxml_cast_write_canonical_text(
        test_fixture->item_heap,
        &(sxml_canonical_text_request){
            .item = item, .output = {.bytes = output, .byte_count = sizeof(output)}},
        &canonical);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_string =
        sxml_atomic_item_get_primitive_type(item).index == SXML_TYPE_XS_STRING;
    if (is_string) {
        outcome_append(outcome, text_view("\""));
    }
    outcome_append(outcome, canonical.text);
    if (is_string) {
        outcome_append(outcome, text_view("\""));
    }
    return SXML_STATUS_OK;
}

static sxml_status write_value(const fixture *test_fixture, sxml_sequence value,
                               evaluation_outcome *outcome) {
    outcome->text[0] = '\0';
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    bool is_first = true;
    for (;;) {
        sxml_iteration_step step;
        sxml_status status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!step.has_item) {
            return SXML_STATUS_OK;
        }
        if (!is_first) {
            outcome_append(outcome, text_view(" "));
        }
        is_first = false;
        switch (sxml_item_get_kind(step.item)) {
        case SXML_ITEM_KIND_ATOMIC:
            status = write_atomic(test_fixture, step.item, outcome);
            break;
        case SXML_ITEM_KIND_NODE: {
            sxml_node_reference node;
            sxml_node_description description;
            sxml_name_parts parts = {.local_name = {.bytes = NULL, .byte_count = 0}};
            status = sxml_item_get_node(step.item, &node);
            if (status == SXML_STATUS_OK) {
                status = sxml_node_describe(node, &description);
            }
            if (status == SXML_STATUS_OK && description.node_name.index != 0) {
                status = sxml_name_pool_get_name_parts(test_fixture->name_pool,
                                                       description.node_name, &parts);
            }
            outcome_append(outcome,
                           parts.local_name.byte_count == 0 ? text_view("#node") : parts.local_name);
            break;
        }
        case SXML_ITEM_KIND_JSON_NODE:
            outcome_append(outcome, text_view("#jnode"));
            break;
        case SXML_ITEM_KIND_FUNCTION:
            outcome_append(outcome, text_view("#function"));
            break;
        case SXML_ITEM_KIND_MAP:
            outcome_append(outcome, text_view("#map"));
            break;
        case SXML_ITEM_KIND_ARRAY:
            outcome_append(outcome, text_view("#array"));
            break;
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
}

/* Compiles and evaluates test_case against the library of test_fixture. */
static void evaluate_with(fixture *test_fixture, const evaluation_case *test_case,
                          evaluation_outcome *outcome) {
    *outcome = (evaluation_outcome){.status = SXML_STATUS_OK};
    const sxml_xpath_static_context static_context = {
        .item_heap = test_fixture->item_heap,
        .allocator = fixture_allocator(test_fixture),
        .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
        .variable_declarations = {.variable_declarations = NULL,
                                  .variable_declaration_count = 0},
        .function_table = sxml_function_library_as_table(test_fixture->library),
    };
    sxml_error_record error_record = {.value = NULL};
    sxml_ir_unit *unit = NULL;
    outcome->status = sxml_xpath_compile(
        &(sxml_xpath_compile_request){.expression_text = text_view(test_case->expression_text),
                                      .source_name = SXML_BYTE_VIEW_FROM_LITERAL("test.xpath"),
                                      .static_context = &static_context,
                                      .error_record = &error_record},
        &unit);
    test_loader loader = {.test_fixture = test_fixture};
    sxml_dynamic_context *dynamic_context = NULL;
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_dynamic_context_create(
            fixture_allocator(test_fixture),
            &(sxml_evaluator_configuration){
                .item_heap = test_fixture->item_heap,
                .allocator = fixture_allocator(test_fixture),
                .document_loader = {.state = &loader, .operations = &test_loader_operations},
                .clock = {.state = (void *)&test_clock_reading,
                          .operations = &test_clock_operations},
                .function_table = static_context.function_table,
                .maximum_evaluation_depth = 0},
            &dynamic_context);
    }
    sxml_sequence value = sxml_sequence_empty();
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){.unit = unit,
                                       .has_context_value = test_case->has_context_value,
                                       .context_value = test_case->context_value,
                                       .global_values = {.slots = NULL, .slot_count = 0},
                                       .error_record = &error_record},
            &value);
    }
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = write_value(test_fixture, value, outcome);
    } else if (sxml_status_has_error_record(outcome->status)) {
        const size_t byte_count =
            error_record.code.local_name.byte_count < sizeof(outcome->code) - 1
                ? error_record.code.local_name.byte_count
                : sizeof(outcome->code) - 1;
        memcpy(outcome->code, error_record.code.local_name.bytes, byte_count);
        outcome->code[byte_count] = '\0';
    }
    sxml_sequence_release(value);
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_ir_unit_release(unit);
    sxml_item_release(loader.documents[0]);
    sxml_item_release(loader.documents[1]);
}

/* Static: the 64 KiB stack of the WebAssembly tests is not for buffers. */
typedef struct evaluation_fixture {
    fixture test_fixture;
    evaluation_outcome outcome;
} evaluation_fixture;

static evaluation_fixture shared_fixture;

/* The outcome of one expression in a fresh fixture; false on a leak. */
typedef struct case_check {
    const evaluation_case *test_case;
    /* Builds the context document from this mini XML first, when set. */
    const char *context_document;
} case_check;

static bool run_case(case_check check, evaluation_outcome **outcome) {
    evaluation_fixture *evaluation = &shared_fixture;
    if (fixture_create(with_library, &evaluation->test_fixture) != SXML_STATUS_OK) {
        (void)fixture_destroy(&evaluation->test_fixture);
        return false;
    }
    evaluation_case test_case = *check.test_case;
    sxml_item document = {.representation = 0};
    if (check.context_document != NULL) {
        if (build_document(&evaluation->test_fixture, check.context_document, &document) !=
            SXML_STATUS_OK) {
            (void)fixture_destroy(&evaluation->test_fixture);
            return false;
        }
        test_case.has_context_value = true;
        test_case.context_value = sxml_sequence_from_item(document);
    }
    evaluate_with(&evaluation->test_fixture, &test_case, &evaluation->outcome);
    sxml_item_release(document);
    const bool is_clean = fixture_released_cells(&evaluation->test_fixture);
    const bool has_no_blocks = fixture_destroy(&evaluation->test_fixture);
    *outcome = &evaluation->outcome;
    return is_clean && has_no_blocks;
}

/* True when expression_text gives expected_text (or, for an expected text
   "!CODE", fails with that error code) and nothing leaks. */
static bool check_expression(case_check check, const char *expected_text) {
    evaluation_outcome *outcome = NULL;
    const bool is_clean = run_case(check, &outcome);
    bool is_expected = false;
    if (outcome != NULL && expected_text[0] == '!') {
        is_expected = sxml_status_has_error_record(outcome->status) &&
                      strcmp(outcome->code, expected_text + 1) == 0;
    } else if (outcome != NULL) {
        is_expected = outcome->status == SXML_STATUS_OK &&
                      strcmp(outcome->text, expected_text) == 0;
    }
    if (!is_expected && outcome != NULL) {
        (void)fprintf(stderr, "for %s\n  expected %s, got status %d %s \"%s\"\n",
                      check.test_case->expression_text, expected_text, (int)outcome->status,
                      outcome->code, outcome->text);
    }
    return is_expected && is_clean;
}

static bool evaluates_to(const char *expression_text, const char *expected_text) {
    return check_expression(
        (case_check){.test_case = &(evaluation_case){.expression_text = expression_text},
                     .context_document = NULL},
        expected_text);
}

static bool evaluates_on_document(const char *document_text, const char *expression_text,
                                  const char *expected_text) {
    return check_expression(
        (case_check){.test_case = &(evaluation_case){.expression_text = expression_text},
                     .context_document = document_text},
        expected_text);
}

/* An expression and what it gives. */
typedef struct expression_case {
    const char *expression_text;
    const char *expected_text;
} expression_case;

static void expect_all(sxml_test_state *state, const expression_case *cases,
                       size_t case_count) {
    for (size_t case_index = 0; case_index < case_count; case_index += 1) {
        const bool passed =
            evaluates_to(cases[case_index].expression_text, cases[case_index].expected_text);
        (void)sxml_test_record_check(
            state, (sxml_test_check){.passed = passed,
                                     .expression = cases[case_index].expression_text,
                                     .file_name = __FILE__,
                                     .line_number = (uint32_t)__LINE__});
    }
}

/* ---------------------------------------------------------- Lifecycle */

static void test_library_lifecycle(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(without_library, &test_fixture) == SXML_STATUS_OK);
    const uint64_t blocks_before = live_blocks(&test_fixture);
    const sxml_function_library_configuration configuration =
        library_configuration(&test_fixture);
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_EXPECT(state, sxml_function_library_memory_requirements(
                                &configuration, &requirements) == SXML_STATUS_OK);
    alignas(64) static uint8_t storage[512];
    SXML_TEST_REQUIRE(state, requirements.byte_count <= sizeof(storage) &&
                                 requirements.alignment <= 64);
    sxml_function_library *library = NULL;
    SXML_TEST_EXPECT(state, sxml_function_library_initialize(
                                (sxml_memory_view){.bytes = storage, .byte_count = 1},
                                &configuration, &library) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state, sxml_function_library_initialize(
                                (sxml_memory_view){.bytes = storage + 1,
                                                   .byte_count = sizeof(storage) - 1},
                                &configuration,
                                &library) == SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_REQUIRE(state, sxml_function_library_initialize(
                                 (sxml_memory_view){.bytes = storage,
                                                    .byte_count = sizeof(storage)},
                                 &configuration, &library) == SXML_STATUS_OK);
    sxml_function_library_finalize(library);
    sxml_function_library_finalize(NULL);
    sxml_function_library_destroy(NULL);
    SXML_TEST_EXPECT(state, live_blocks(&test_fixture) == blocks_before);
    /* Two parts serving fn:abs#1 twice. */
    static const sxml_native_function_view *const doubled_parts[] = {
        &sxml_function_library_numeric, &sxml_function_library_numeric};
    sxml_function_library_configuration doubled = configuration;
    doubled.parts = (sxml_native_function_view_list){
        .native_function_views = doubled_parts, .native_function_view_count = 2};
    SXML_TEST_EXPECT(state, sxml_function_library_create(fixture_allocator(&test_fixture),
                                                         &doubled,
                                                         &library) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_function_library_configuration without_heap = configuration;
    without_heap.item_heap = NULL;
    SXML_TEST_EXPECT(state, sxml_function_library_create(fixture_allocator(&test_fixture),
                                                         &without_heap,
                                                         &library) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, live_blocks(&test_fixture) == blocks_before);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Making the library and finding a variadic row (which allocates its
   types) fail cleanly at every allocation. */
static void test_library_survives_every_allocation_failure(sxml_test_state *state) {
    bool has_succeeded = false;
    for (uint64_t failing_number = 1; !has_succeeded && failing_number < 1000;
         failing_number += 1) {
        fixture test_fixture;
        sxml_status status = fixture_create(
            (fixture_request){.failing_allocation_number = failing_number,
                              .is_library_wanted = true}, &test_fixture);
        sxml_function_entry entry = {.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
        sxml_name_identifier name = {.index = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_name(
                test_fixture.name_pool,
                &(sxml_name_request){.namespace_uri = sxml_well_known_namespace_identifier(
                                         SXML_NAMESPACE_FUNCTIONS),
                                     .local_name = text_view("concat")},
                &name);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_table_find(
                sxml_function_library_as_table(test_fixture.library),
                &(sxml_function_query){.function_name = name, .arity = 9}, &entry);
        }
        has_succeeded = status == SXML_STATUS_OK &&
                        sxml_counting_allocator_read_counts(test_fixture.counting_allocator)
                                .failed_allocation_count == 0;
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, has_succeeded);
}

/* ------------------------------------------------------- Function table */

typedef struct name_query {
    sxml_well_known_namespace function_namespace;
    const char *local_name;
    size_t arity;
} name_query;

static sxml_status find_named(const fixture *test_fixture, name_query query,
                              sxml_function_entry *entry) {
    sxml_name_identifier name = {.index = 0};
    sxml_status status = sxml_name_pool_intern_name(
        test_fixture->name_pool,
        &(sxml_name_request){
            .namespace_uri = sxml_well_known_namespace_identifier(query.function_namespace),
            .local_name = text_view(query.local_name)},
        &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_function_table_find(sxml_function_library_as_table(test_fixture->library),
                                    &(sxml_function_query){.function_name = name,
                                                           .arity = query.arity},
                                    entry);
}

static void test_table_finds_rows_and_constructors(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(with_library, &test_fixture) == SXML_STATUS_OK);
    sxml_function_entry entry = {.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    SXML_TEST_EXPECT(state, find_named(&test_fixture,
                                       (name_query){SXML_NAMESPACE_FUNCTIONS, "round", 2},
                                       &entry) == SXML_STATUS_OK &&
                                entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NATIVE &&
                                entry.parameter_types.sequence_type_count == 2 &&
                                entry.parameters.function_parameter_count == 3 &&
                                entry.signature.index != SXML_TYPE_NONE);
    SXML_TEST_EXPECT(state, find_named(&test_fixture,
                                       (name_query){SXML_NAMESPACE_FUNCTIONS, "round", 4},
                                       &entry) == SXML_STATUS_OK &&
                                entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE);
    SXML_TEST_EXPECT(state, find_named(&test_fixture,
                                       (name_query){SXML_NAMESPACE_FUNCTIONS, "concat", 12},
                                       &entry) == SXML_STATUS_OK &&
                                entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NATIVE &&
                                entry.parameter_types.sequence_type_count == 12);
    SXML_TEST_EXPECT(state, find_named(&test_fixture,
                                       (name_query){SXML_NAMESPACE_FUNCTIONS, "concat", 40},
                                       &entry) == SXML_STATUS_OK &&
                                entry.parameter_types.sequence_type_count == 40);
    SXML_TEST_EXPECT(state, find_named(&test_fixture,
                                       (name_query){SXML_NAMESPACE_XML_SCHEMA, "integer", 1},
                                       &entry) == SXML_STATUS_OK &&
                                entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR &&
                                entry.constructor_type.index == SXML_TYPE_XS_INTEGER);
    SXML_TEST_EXPECT(state, find_named(&test_fixture,
                                       (name_query){SXML_NAMESPACE_XML_SCHEMA, "NMTOKENS", 1},
                                       &entry) == SXML_STATUS_OK &&
                                entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR &&
                                entry.result_type.occurrence == SXML_OCCURRENCE_ZERO_OR_MORE);
    SXML_TEST_EXPECT(state, find_named(&test_fixture,
                                       (name_query){SXML_NAMESPACE_XML_SCHEMA,
                                                    "anyAtomicType", 1},
                                       &entry) == SXML_STATUS_OK &&
                                entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE);
    SXML_TEST_EXPECT(state, find_named(&test_fixture,
                                       (name_query){SXML_NAMESPACE_FUNCTIONS, "no-such", 1},
                                       &entry) == SXML_STATUS_OK &&
                                entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE);
    SXML_TEST_EXPECT(state, sxml_function_table_find(
                                (sxml_function_table){.state = NULL, .operations = NULL},
                                &(sxml_function_query){.function_name = {.index = 1},
                                                       .arity = 0},
                                &entry) == SXML_STATUS_OK &&
                                entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Every row's type text parses and resolves. */
static void test_every_row_resolves(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(with_library, &test_fixture) == SXML_STATUS_OK);
    for (size_t part_index = 0; part_index < SXML_TEST_COUNT(core_parts); part_index += 1) {
        const sxml_native_function_view *part = core_parts[part_index];
        for (size_t row_index = 0; row_index < part->native_function_count; row_index += 1) {
            const sxml_native_function *row = &part->native_functions[row_index];
            sxml_name_identifier name = {.index = 0};
            sxml_function_entry entry = {.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
            sxml_status status = sxml_name_pool_intern_name(
                test_fixture.name_pool,
                &(sxml_name_request){.namespace_uri = sxml_well_known_namespace_identifier(
                                         row->function_namespace),
                                     .local_name = row->local_name},
                &name);
            if (status == SXML_STATUS_OK) {
                status = sxml_function_table_find(
                    sxml_function_library_as_table(test_fixture.library),
                    &(sxml_function_query){.function_name = name,
                                           .arity = row->parameters.function_parameter_count},
                    &entry);
            }
            const bool passed =
                status == SXML_STATUS_OK && entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NATIVE;
            if (!passed) {
                (void)fprintf(stderr, "row %.*s: status %d\n", (int)row->local_name.byte_count,
                              (const char *)row->local_name.bytes, (int)status);
            }
            SXML_TEST_EXPECT(state, passed);
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ---------------------------------------------------------------- Numbers */

static void test_numeric_functions(sxml_test_state *state) {
    static const expression_case cases[] = {
        /* treat-as-14 */
        {"let $var := -100 return fn:abs($var cast as xs:float treat as xs:float)", "100"},
        {"abs(-2147483648) instance of xs:integer", "true"},
        {"abs(xs:int(-3)) instance of xs:int", "false"},
        {"abs(-9223372036854775808)", "9223372036854775808"},
        {"abs(-1.5)", "1.5"},
        {"abs(-0e0)", "0"},
        {"abs(())", ""},
        /* function-literal-038, ArrowPostfix-001, DynamicFunctionCall-143
           (written with "!": the evaluator does not call a sequence of
           functions yet) */
        {"Q{http://www.w3.org/2005/xpath-functions}ceiling#1(0.9)", "1"},
        {"4.54=>ceiling()", "5"},
        {"(abs#1, round#1, floor#1, ceiling#1) ! .(23.7)", "23.7 24 23 24"},
        {"ceiling(-0.5e0)", "-0"},
        {"floor(-0.5)", "-1"},
        {"floor(xs:float(2.5)) instance of xs:float", "true"},
        /* function-literal-044 */
        {"Q{http://www.w3.org/2005/xpath-functions}round#2(1.1, 0)", "1"},
        {"round(2.5)", "3"},
        {"round(-2.5)", "-2"},
        {"round(2.5e0)", "3"},
        {"round(-0.4e0)", "-0"},
        {"round(1.15e0, 1)", "1.1"},
        {"round(1e-2, 2, 'away-from-zero')", "0.02"},
        {"round-half-to-even(xs:float('0.05'), 1)", "0.1"},
        {"round(1234.5678, -2)", "1200"},
        {"round(35, -1)", "40"},
        {"round(2.5, 0, 'half-to-even')", "2"},
        {"round(-2.5, 0, 'half-away-from-zero')", "-3"},
        {"round(2.4, 0, 'ceiling')", "3"},
        {"round(1, 0, 'sideways')", "!XPTY0004"},
        {"round(xs:double('INF'), -2, 'half-to-even')", "INF"},
        {"round-half-to-even(2.5)", "2"},
        {"round-half-to-even(3.567812e0, 2)", "3.57"},
        {"number('12.5')", "12.5"},
        {"number('x')", "NaN"},
        {"number(())", "NaN"},
        {"number(true())", "1"},
        /* inline-fn-004 */
        {"sum(for $i in 1 to 6 return let $add := function($x){$x + $i} return $add(4))", "45"},
        {"sum(())", "0"},
        {"sum((), ())", ""},
        {"sum((1, 2.5, 3e0))", "6.5"},
        {"sum((xs:dayTimeDuration('PT1H'), xs:dayTimeDuration('PT30M')))", "PT1H30M"},
        {"sum(('a', 'b'))", "!FORG0006"},
        {"sum((1, xs:dayTimeDuration('PT1H')))", "!FORG0006"},
        /* DynamicFunctionCall-009 */
        {"let $f := fn($in as xs:decimal*) as xs:decimal {sum($in, 0.0)} return "
         "$f(xs:NMTOKENS('1 1.2 1.3 1.4')!xs:untypedAtomic(.))",
         "4.9"},
        /* fn-function-lookup-336 without function-lookup (1.21) */
        {"avg((1, 3))", "2"},
        {"avg((1, 2)) instance of xs:decimal", "true"},
        {"avg(())", ""},
        {"avg((xs:yearMonthDuration('P1Y'), xs:yearMonthDuration('P2Y')))", "P1Y6M"},
        {"max((1, 3, 2))", "3"},
        {"max((1, 2.5e0)) instance of xs:double", "true"},
        {"max((3, 2.5))", "3"},
        {"max((3, 2.5)) instance of xs:integer", "true"},
        {"min((1, number('NaN')))", "NaN"},
        {"min(('b', 'a', 'c'))", "\"a\""},
        {"max((xs:anyURI('b'), 'a')) instance of xs:anyURI", "true"},
        {"max((5, 3.0e0)) instance of xs:integer", "true"},
        {"max((xs:float('NaN'), 1, 'a string'))", "NaN"},
        {"max((xs:float('NaN'), xs:untypedAtomic('one')))", "NaN"},
        {"max((xs:untypedAtomic('one'), xs:float('NaN')))", "!FORG0001"},
        {"max(('a string', 1, xs:float('NaN')))", "!FORG0006"},
        {"max((xs:yearMonthDuration('P1Y'), xs:dayTimeDuration('P1D')))", "P1Y"},
        {"max((1, true()))", "!FORG0006"},
        {"is-NaN(number('x')), is-NaN(1)", "true false"},
        {"max((1, 'a'))", "!FORG0006"},
        {"max(())", ""},
        {"max(('a', 'b'), 'http://example.com/collation')", "!FOCH0002"},
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

static void test_random_number_generator(sxml_test_state *state) {
    static const expression_case cases[] = {
        {"let $r := random-number-generator(42) return $r?number ge 0 and $r?number lt 1",
         "true"},
        {"random-number-generator(42)?number = random-number-generator(42)?number", "true"},
        {"random-number-generator(42)?number = random-number-generator(42)?next()?number",
         "false"},
        {"sort(random-number-generator(1)?permute(1 to 10))", "1 2 3 4 5 6 7 8 9 10"},
        {"count(random-number-generator()?permute(1 to 5))", "5"},
        {"random-number-generator()?permute(())", ""},
        {"let $r := random-number-generator(1) return deep-equal($r?take(3), ($r?number, "
         "$r?next()?number, $r?next()?next()?number))",
         "true"},
        {"count(random-number-generator()?take(100)), count(random-number-generator()?take(-1))",
         "100 0"},
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ---------------------------------------------------------------- Strings */

static void test_string_functions(sxml_test_state *state) {
    static const expression_case cases[] = {
        /* fn-concatint2args-1, hof-005, ArrowPostfix-108 */
        {"fn:concat(xs:int(\"-2147483648\"),xs:int(\"-2147483648\"))",
         "\"-2147483648-2147483648\""},
        {"let $f := concat#8 return $f('a','b','c','d','e','f','g','h')", "\"abcdefgh\""},
        {"let $usd := \"$\" => concat(?) return $usd(3)", "\"$3\""},
        {"concat()", "\"\""},
        {"concat(('a', 'b'), (), 1.5)", "\"ab1.5\""},
        /* fn-substring-1, fn-substring-25, substring-40-001 */
        {"fn:substring(\"motor car\", 6)", "\" car\""},
        {"substring(\"\xF0\x90\x80\x81\", 1, 2)", "\"\xF0\x90\x80\x81\""},
        {"substring(\"abcde\", 3, ())", "\"cde\""},
        {"substring('12345', 1.5, 2.6)", "\"234\""},
        {"substring('12345', 0, 3)", "\"12\""},
        {"substring('12345', 5, -3)", "\"\""},
        {"substring('12345', -3, 5)", "\"1\""},
        {"substring('12345', 0 div 0e0, 3)", "\"\""},
        {"substring('12345', -42, 1 div 0e0)", "\"12345\""},
        {"substring((), 1)", "\"\""},
        /* fn-string-join-16 */
        {"fn:string-join((\"a\"),\"A\")", "\"a\""},
        {"string-join((1, 2, 3), ', ')", "\"1, 2, 3\""},
        {"string-join(())", "\"\""},
        {"string-length('h\xC3\xA9llo')", "5"},
        {"string-length(())", "0"},
        {"string-length(111), normalize-space(xs:date('2002-01-01'))", "3 \"2002-01-01\""},
        {"substring('abcde', 2.4999999999999999999)", "\"bcde\""},
        {"substring('XY', -9223372036854775805, 9223372036854775807)", "\"X\""},
        {"normalize-space('  a  b \t c ')", "\"a b c\""},
        {"codepoints-to-string((72, 105))", "\"Hi\""},
        {"codepoints-to-string(0)", "!FOCH0001"},
        {"string-to-codepoints(codepoints-to-string((1, 31)))", "1 31"},
        {"codepoints-to-string(65 to 1114112)", "!FOCH0001"},
        /* Coercing a range to xs:integer* does not walk it. */
        {"codepoints-to-string(65536 to 1114112)", "!FOCH0001"},  /* cbcl-codepoints-to-string-005 */
        {"let $y := 65536*65536 return for $x in $y to $y+10 return "
         "codepoints-to-string(65 to $x)",
         "!FOCH0001"},  /* cbcl-codepoints-to-string-021 */
        {"tokenize(' a  b ', flags := 'q')", "\"a\" \"b\""},  /* keyword after a left-out $pattern */
        {"string-to-codepoints('Hi')", "72 105"},
        {"string(12.50)", "\"12.5\""},
        {"string(())", "\"\""},
        {"string(abs#1)", "!FOTY0014"},
        {"string(xs:anyURI('u')) instance of xs:string", "true"},
        /* for-each-001 without for-each (1.21) */
        {"upper-case('aa')", "\"AA\""},
        {"upper-case('stra\xC3\x9F" "e')", "\"STRASSE\""},
        {"lower-case('\xC3\x84" "BC')", "\"\xC3\xA4" "bc\""},
        {"upper-case(())", "\"\""},
        {"contains('abc', 'b')", "true"},
        {"contains('abc', '')", "true"},
        {"contains((), 'a')", "false"},
        {"starts-with('apple', 'a')", "true"},
        {"starts-with('apple', 'p')", "false"},
        {"ends-with('apple', 'le')", "true"},
        {"ends-with('apple', 'apples')", "false"},
        {"starts-with('a', 'a', 'http://www.w3.org/2005/xpath-functions/collation/codepoint')",
         "true"},
        {"starts-with('a', 'a', 'http://example.com/collation')", "!FOCH0002"},
        {"substring-before('tattoo', 'attoo')", "\"t\""},
        {"substring-after('tattoo', 'tat')", "\"too\""},
        {"substring-after('abc', 'x')", "\"\""},
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

static void test_regular_expression_functions(sxml_test_state *state) {
    static const expression_case cases[] = {
        /* fn-tokenize-1a, fn-tokenize-35, ArrowPostfix-004 */
        {"fn:tokenize(\"abba\", \".?\")", "\"\" \"\" \"\" \"\" \"\""},
        {"fn:tokenize(concat('Mary', codepoints-to-string(13), 'Jones'), 'y.J', 's')",
         "\"Mar\" \"ones\""},
        {"let $f:= tokenize#2 return \"the cat sat on the mat\"=>$f(\"\\s+\")=>count()", "6"},
        {"tokenize(' a  b ')", "\"a\" \"b\""},
        {"tokenize('')", ""},
        {"tokenize('a,b,,c', ',')", "\"a\" \"b\" \"\" \"c\""},
        {"tokenize(',a,', ',')", "\"\" \"a\" \"\""},
        {"tokenize('abc', '')", "\"a\" \"b\" \"c\""},
        {"tokenize('a b', '\\s', 't')", "!FORX0001"},
        {"tokenize('a b', '(')", "!FORX0002"},
        {"matches('abracadabra', 'bra')", "true"},
        {"matches('abracadabra', '^a.*a$')", "true"},
        {"matches('abracadabra', '^bra')", "false"},
        {"matches('ABC', 'abc', 'i')", "true"},
        {"replace('abracadabra', 'bra', '*')", "\"a*cada*\""},
        {"replace('abracadabra', 'a(.)', 'a$1$1')", "\"abbraccaddabbra\""},
        {"replace('abracadabra', 'a', '\\$')", "\"$br$c$d$br$\""},
        {"replace('abc', 'b', '$')", "!FORX0004"},
        {"replace('a.b', '.', '!', 'q')", "\"a!b\""},
        {"replace('abc', '', '-')", "\"-a-b-c-\""},
        {"replace('ab', 'x', '$')", "!FORX0004"},
        {"replace('Chapter 9', '[0-9]+', function($k, $g) { string(number($k) + 1) })",
         "\"Chapter 10\""},
        {"replace('A1B2', '([A-Z])([0-9])', fn($s, $g) { $g?2 || $g?1 })", "\"1A2B\""},
        {"replace('abc', 'b', fn{upper-case(.)})", "\"aBc\""},
        /* $replacement has its F&O 4.0 choice type: the function is
           coerced by the call, and receives untyped values. */
        {"replace('abc', '(b)', fn($m, $g) { $m instance of xs:untypedAtomic and "
         "$g?1 instance of xs:untypedAtomic })",
         "\"atruec\""},
        {"replace('abc', 'b', fn() { 'Z' }), replace('abc', 'b', xs:untypedAtomic('U'))",
         "\"aZc\" \"aUc\""},
        /* Neither alternative takes it; the error is the first attempt's,
           atomizing a function (XPath 4.0, section 3.4.2). */
        {"replace('abc', 'b', fn($a, $b, $c) { 'Z' })", "!FOTY0013"},
        {"replace('abc', 'x', fn{'y'})", "\"abc\""},
        {"replace((), 'x', fn{'y'})", "\"\""},
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* -------------------------------------------------------------- Sequences */

static void test_sequence_functions(sxml_test_state *state) {
    static const expression_case cases[] = {
        {"true(), false()", "true false"},
        {"boolean(()), boolean(0), boolean('a'), not(1)", "false false true false"},
        {"boolean((1, 2))", "!FORG0006"},
        /* inline-fn-030 */
        {"data(function($a, $b) {$a + $b})", "!FOTY0013"},
        {"data((1, [2, [3]]))", "1 2 3"},
        {"count(()), count(1 to 10)", "0 10"},
        /* RangeExpr-408a */
        {"count(100000000000 to 100000000003)", "4"},
        {"empty(()), empty(1), exists(()), exists((1, 2))", "true false false true"},
        {"head((3, 4)), head(())", "3"},
        {"tail((1, 2, 3)), tail(())", "2 3"},
        /* K-RangeExpr-17 */
        {"subsequence(reverse(1 to 4), 3, 1) eq 2", "true"},
        {"reverse(('a', 'b'))", "\"b\" \"a\""},
        {"subsequence((1, 2, 3, 4, 5), 2, 3)", "2 3 4"},
        {"subsequence(1 to 1000000000000, 999999999999)", "999999999999 1000000000000"},
        {"subsequence((1, 2, 3), 0 div 0e0)", ""},
        {"subsequence((1, 2, 3), -1)", "1 2 3"},
        {"subsequence((1, 2, 3), 1.5, 1.4)", "2"},
        {"subsequence((1, 2, 3), 1.5e0, 1.4e0)", "2"},
        {"subsequence(('a', 'b', 'c'), -9223372036854775805, 9223372036854775807)", "\"a\""},
        {"subsequence(('a', 'b', 'c'), -18446744073709551615, 18446744073709551617)",
         "!FOAR0002"},
        {"subsequence(('a', 'b', 'c'), 18446744073709551617e0)", ""},
        {"distinct-values((1, 1.0, 1e0, 'a', xs:untypedAtomic('a'), number('NaN'), "
         "number('NaN')))",
         "1 \"a\" NaN"},
        {"distinct-values(())", ""},
        {"zero-or-one(()), one-or-more(1), exactly-one('a')", "1 \"a\""},
        {"zero-or-one((1, 2))", "!FORG0003"},
        {"one-or-more(())", "!FORG0004"},
        {"exactly-one((1, 2))", "!FORG0005"},
        {"index-of((10, 20, 30, 20), 20), index-of(('a', 1), 'a')", "2 4 1"},
        {"index-of(xs:double('NaN'), xs:float('NaN'))", "1"},
        {"insert-before(('a', 'b'), 2, 'x'), insert-before((), 5, 1)", "\"a\" \"x\" \"b\" 1"},
        {"remove(('a', 'b', 'c'), (1, 3))", "\"b\""},
        /* fn-sort-10, fn-sort-19, fn-sort-error-1 */
        {"let $r := fn:sort( (17, 5, 14) ) return (count($r) eq 3 and $r[1] eq 5 and "
         "$r[2] eq 14 and$r[3] eq 17)",
         "true"},
        {"fn:sort( ([1, 2], 1) ) ! (if (. instance of array(*)) then 'array' else .)",
         "1 \"array\""},
        {"fn:sort( (1, \"a\") )", "!XPTY0004"},
        {"sort(('b', 'c', 'a'))", "\"a\" \"b\" \"c\""},
        {"sort((3, 1, 2), (), function($x){-$x})", "3 2 1"},
        {"sort((1, number('NaN'), -1))", "NaN -1 1"},
        {"sort((), (), abs#1)", ""},
        {"sort((2, 1), 'http://example.com/collation')", "!FOCH0002"},
        {"sort(('aa', 'b', 'ccc'), (), string-length#1)", "\"b\" \"aa\" \"ccc\""},
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

static void test_deep_equal(sxml_test_state *state) {
    static const expression_case cases[] = {
        /* fn-deep-equalint2args-1, fn-deep-equal-arrays-11, K-FilterExpr-64,
           map-put-019 without map functions */
        {"fn:deep-equal((xs:int(\"-2147483648\")),(xs:int(\"-2147483648\")))", "true"},
        {"fn:deep-equal([['a', 'b', 'c']], [['a', 'b', 'c']])", "true"},
        {"deep-equal((0, 1, 2), (0, 1, 2)[position() eq position()])", "true"},
        {"deep-equal(map{}, map{})", "true"},
        {"deep-equal((), ())", "true"},
        {"deep-equal((1, 2), (1, 2, 3))", "false"},
        {"deep-equal(1, 1e0)", "true"},
        {"deep-equal(1, '1')", "false"},
        {"deep-equal('a', xs:untypedAtomic('a'))", "true"},
        {"deep-equal(number('NaN'), number('NaN'))", "true"},
        {"deep-equal([1, [2, [3]]], [1, [2, [4]]])", "false"},
        {"deep-equal([1, (2, 3)], [1, (2, 3)])", "true"},
        {"deep-equal(map{1: 'a', 2: 'b'}, map{2: 'b', 1: 'a'})", "true"},
        {"deep-equal(map{1: 'a', 2: 'b'}, map{2: 'b', 1: 'a'}, map{'map-order': true()})",
         "false"},
        {"deep-equal(map{1: [1]}, map{1: [2]})", "false"},
        {"deep-equal(map{1: 'a'}, map{2: 'a'})", "false"},
        {"deep-equal(abs#1, abs#1)", "true"},
        {"deep-equal(abs#1, floor#1)", "false"},
        {"deep-equal([1], map{1: 1})", "false"},
        {"deep-equal('bedtime', ' bedtime ', { 'whitespace': 'normalize' })", "true"},
        {"deep-equal('bedtime', ' bed-time ', { 'whitespace': 'normalize' })", "false"},
        {"deep-equal('a', 'a', 'http://example.com/collation')", "!FOCH0002"},
        {"deep-equal('a', 'a', 'http://www.w3.org/2005/xpath-functions/collation/codepoint')",
         "true"},
        {"deep-equal(1, 1, map{'comments': 1})", "!XPTY0004"},
        {"deep-equal(1, 1, map{'unknown-option': 1})", "!XPTY0004"},
        {"deep-equal(1, 1, map{QName('http://example.com/', 'x'): 1})", "true"},
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

static void test_deep_equal_unsupported_options(sxml_test_state *state) {
    evaluation_outcome *outcome = NULL;
    SXML_TEST_EXPECT(state,
                     run_case((case_check){.test_case = &(evaluation_case){
                                               .expression_text =
                                                   "deep-equal((1, 2), (2, 1), map{'ordered': false()})"},
                                           .context_document = NULL},
                              &outcome) &&
                         outcome->status == SXML_STATUS_UNSUPPORTED_FEATURE);
}

static void test_deep_equal_of_nodes(sxml_test_state *state) {
    /* deep-equal-40-comments-001 with mini documents. */
    SXML_TEST_EXPECT(state, evaluates_on_document("<r><a><!--foo--></a><a><!--bar--></a></r>",
                                                  "deep-equal(/r/a[1], /r/a[2])", "true"));
    SXML_TEST_EXPECT(state,
                     evaluates_on_document("<r><a><!--foo--></a><a><!--bar--></a></r>",
                                           "deep-equal(/r/a[1], /r/a[2], map{'comments': true()})",
                                           "false"));
    SXML_TEST_EXPECT(state, evaluates_on_document("<r><a x=\"1\" y=\"2\">t</a><a y=\"2\" "
                                                  "x=\"1\">t</a></r>",
                                                  "deep-equal(/r/a[1], /r/a[2])", "true"));
    SXML_TEST_EXPECT(state, evaluates_on_document("<r><a x=\"1\">t</a><a x=\"2\">t</a></r>",
                                                  "deep-equal(/r/a[1], /r/a[2])", "false"));
    SXML_TEST_EXPECT(state, evaluates_on_document("<r><a>t<!--c-->u</a><a>tu</a></r>",
                                                  "deep-equal(/r/a[1], /r/a[2])", "true"));
    SXML_TEST_EXPECT(state, evaluates_on_document("<r><a><b/> <c/></a><a><b/><c/></a></r>",
                                                  "deep-equal(/r/a[1], /r/a[2])", "false"));
    SXML_TEST_EXPECT(state,
                     evaluates_on_document("<r><a><b/> <c/></a><a><b/><c/></a></r>",
                                           "deep-equal(/r/a[1], /r/a[2], map{'whitespace': "
                                           "'strip'})",
                                           "true"));
    SXML_TEST_EXPECT(state, evaluates_on_document("<r><a><b>1</b></a><a><b>2</b></a></r>",
                                                  "deep-equal(/r/a[1], /r/a[2])", "false"));
    SXML_TEST_EXPECT(state, evaluates_on_document("<r><a/><b/></r>",
                                                  "deep-equal(/r/a, /r/b)", "false"));
    SXML_TEST_EXPECT(state, evaluates_on_document("<r><a/></r>",
                                                  "deep-equal(/r/a, 'a')", "false"));
    SXML_TEST_EXPECT(state, evaluates_on_document("<r>x</r>", "deep-equal(/, /)", "true"));
}

/* ------------------------------------------------------------------ Nodes */

static void test_node_functions(sxml_test_state *state) {
    const char *document = "<works><employee name=\"Jane\"><hours>20</hours></employee>"
                           "<employee name=\"John\"><hours>40</hours></employee></works>";
    SXML_TEST_EXPECT(state, evaluates_on_document(document, "local-name(/*), name((//@name)[1])",
                                                  "\"works\" \"name\""));
    SXML_TEST_EXPECT(state, evaluates_on_document(document, "/works/employee ! local-name()",
                                                  "\"employee\" \"employee\""));
    SXML_TEST_EXPECT(state, evaluates_on_document(document, "local-name(/), name(())",
                                                  "\"\" \"\""));
    SXML_TEST_EXPECT(state, evaluates_on_document(document, "namespace-uri(/*) eq ''", "true"));
    SXML_TEST_EXPECT(state, evaluates_on_document(document, "node-name(/*) eq QName('', 'works')",
                                                  "true"));
    /* bang-7, bang-13 */
    SXML_TEST_EXPECT(state, evaluates_on_document(
                                document, "/ works ! employee[2] ! hours[1] ! number()", "40"));
    SXML_TEST_EXPECT(state, evaluates_on_document(
                                document,
                                "/ works ! employee[2] ! preceding-sibling::*[1] ! string(@name)",
                                "\"Jane\""));
    SXML_TEST_EXPECT(state, evaluates_on_document(document, "string(/works/employee[1])",
                                                  "\"20\""));
    SXML_TEST_EXPECT(state, evaluates_on_document(document, "data(//@name)", "Jane John"));
    SXML_TEST_EXPECT(state, evaluates_on_document(document, "string-length((//hours)[1])", "2"));
    SXML_TEST_EXPECT(state, evaluates_on_document(document, "//hours/string-length()", "2 2"));
    SXML_TEST_EXPECT(state, evaluates_on_document(document, "root((//hours)[2]) is /", "true"));
    /* d-o-n-004, d-o-n-005, d-o-n-007 with mini documents */
    SXML_TEST_EXPECT(state, evaluates_on_document(
                                "<x><a/><b/><c/><d/></x>",
                                "distinct-ordered-nodes((//c, //b, //d, //a, //b)) ! local-name()",
                                "\"a\" \"b\" \"c\" \"d\""));
    SXML_TEST_EXPECT(state, evaluates_on_document(
                                "<p><a q=\"1\"/><b r=\"2\"/></p>",
                                "let $y := random-number-generator()?permute(//* | //@*) "
                                "return distinct-ordered-nodes(($y, //b)) ! local-name()",
                                "\"p\" \"a\" \"q\" \"b\" \"r\""));
    SXML_TEST_EXPECT(state, evaluates_to("distinct-ordered-nodes(())", ""));
    SXML_TEST_EXPECT(state, evaluates_to("root(())", ""));
}

static void test_doc_and_qname(sxml_test_state *state) {
    static const expression_case cases[] = {
        /* fn-doc-6 with the loader double */
        {"fn:doc('a.xml') is fn:doc('b.xml')", "false"},
        {"fn:doc('a.xml') is fn:doc('a.xml')", "true"},
        {"doc('a.xml')/a/b ! local-name()", "\"b\""},
        {"doc(())", ""},
        {"doc('missing.xml')", "!FODC0002"},
        {"QName('http://example.com/', 'p:local') eq QName('http://example.com/', 'local')",
         "true"},
        {"string(QName('http://example.com/', 'p:local'))", "\"p:local\""},
        {"QName('', 'p:local')", "!FOCA0002"},
        {"QName('u', '1a')", "!FOCA0002"},
        {"QName('u', 'a:')", "!FOCA0002"},
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ----------------------------------------------------- Context and errors */

static void test_context_functions(sxml_test_state *state) {
    static const expression_case cases[] = {
        /* the clock reads 12:30:45.5-05:00, the implicit timezone is +02:00 */
        {"current-dateTime()", "2026-10-16T19:30:45.5+02:00"},
        {"current-date()", "2026-10-16+02:00"},
        {"current-time()", "19:30:45.5+02:00"},
        {"current-dateTime() instance of xs:dateTimeStamp", "true"},
        {"implicit-timezone()", "PT2H"},
        {"current-dateTime() eq current-dateTime()", "true"},
        /* K-CondExpr-3, MapConstructor-023's year-from-date */
        {"(if((1, current-time())[1] treat as xs:integer) then true() else false()) eq true()",
         "true"},
        {"year-from-date(current-date())", "2026"},
        {"year-from-date(xs:date('1999-05-31')), month-from-date(xs:date('1999-05-31')), "
         "day-from-date(xs:date('1999-05-31'))",
         "1999 5 31"},
        {"year-from-date(())", ""},
        {"hours-from-dateTime(xs:dateTime('1999-05-31T13:20:00-05:00')), "
         "minutes-from-time(xs:time('13:20:10')), seconds-from-time(xs:time('13:20:10.5'))",
         "13 20 10.5"},
        {"seconds-from-dateTime(xs:dateTime('1999-05-31T13:20:07Z')) instance of xs:decimal",
         "true"},
        {"timezone-from-dateTime(current-dateTime()), timezone-from-date(xs:date('2000-01-01')), "
         "timezone-from-time(xs:time('10:00:00-05:30'))",
         "PT2H -PT5H30M"},
        {"(1 to 5)[position() = last()]", "5"},
        {"(1 to 5)[last()]", "5"},
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

static void test_error_functions(sxml_test_state *state) {
    static const expression_case cases[] = {
        /* fn-error-1, fn-error-20, function-literal-028, K-ValCompTypeChecking-40,
           cbcl-error-017 */
        {"fn:error()", "!FOER0000"},
        {"fn:error(fn:QName('http://www.w3.org/2005/xqt-errors', 'err:SERE0014'))",
         "!SERE0014"},
        {"Q{http://www.w3.org/2005/xpath-functions}error#1(fn:QName("
         "'http://www.w3.org/2005/xqt-errors', 'foo:XXXX0000'))",
         "!XXXX0000"},
        {"(error(), 3) eq 3", "!FOER0000"},
        {"if (fn:error()) then true() else true()", "!FOER0000"},
        {"error((), 'described')", "!FOER0000"},
        {"error(QName('http://example.com/', 'e:MINE0001'), 'described', (1, 2))",
         "!MINE0001"},
        {"trace((1, 2), 'label')", "1 2"},
        {"fn:error#0 instance of fn() as xs:integer", "true"},
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* An error raised by fn:error carries its description and value. */
static void test_error_has_description_and_value(sxml_test_state *state) {
    evaluation_fixture *evaluation = &shared_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(with_library, &evaluation->test_fixture) ==
                                 SXML_STATUS_OK);
    fixture *test_fixture = &evaluation->test_fixture;
    const sxml_xpath_static_context static_context = {
        .item_heap = test_fixture->item_heap,
        .allocator = fixture_allocator(test_fixture),
        .function_table = sxml_function_library_as_table(test_fixture->library)};
    sxml_error_record error_record = {.value = NULL};
    sxml_ir_unit *unit = NULL;
    sxml_status status = sxml_xpath_compile(
        &(sxml_xpath_compile_request){
            .expression_text = text_view("error(QName('http://example.com/', 'e:MINE0001'), "
                                         "'described', (1, 2))"),
            .static_context = &static_context,
            .error_record = &error_record},
        &unit);
    sxml_dynamic_context *dynamic_context = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_create(
            fixture_allocator(test_fixture),
            &(sxml_evaluator_configuration){.item_heap = test_fixture->item_heap,
                                            .allocator = fixture_allocator(test_fixture),
                                            .function_table = static_context.function_table},
            &dynamic_context);
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){.unit = unit, .error_record = &error_record}, &value);
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.namespace_uri,
                                                  text_view("http://example.com/")));
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(error_record.code.namespace_prefix, text_view("e")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.diagnostic.description,
                                                  text_view("described")));
    uint64_t value_count = 0;
    SXML_TEST_EXPECT(state, error_record.value != NULL &&
                                sxml_sequence_count(*error_record.value, NULL, &value_count) ==
                                    SXML_STATUS_OK &&
                                value_count == 2);
    sxml_sequence_release(value);
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_ir_unit_release(unit);
    SXML_TEST_EXPECT(state, fixture_released_cells(test_fixture));
    SXML_TEST_EXPECT(state, fixture_destroy(test_fixture));
}

/* ---------------------------------------------- Allocation failures */

/* Every allocation of the library's counting allocator fails in turn
   while an expression that exercises several parts runs; each run fails
   cleanly or succeeds with the right value. */
static void test_evaluation_survives_every_allocation_failure(sxml_test_state *state) {
    const char *expression =
        "string-join((upper-case('ab'), tokenize('a b', ' '), string(sort((3, 1, 2), (), "
        "function($x){-$x})[1]), string(deep-equal([1, [2]], [1, [2]])), "
        "substring('xyz', 2), string(sum((1, 2.5))), string(round(2.45, 1))), ',')";
    const char *expected = "\"AB,a,b,3,true,yz,3.5,2.5\"";
    bool has_succeeded = false;
    for (uint64_t failing_number = 1; !has_succeeded && failing_number < 20000;
         failing_number += 1) {
        evaluation_fixture *evaluation = &shared_fixture;
        sxml_status status = fixture_create(
            (fixture_request){.failing_allocation_number = failing_number,
                              .is_library_wanted = true}, &evaluation->test_fixture);
        if (status == SXML_STATUS_OK) {
            evaluate_with(&evaluation->test_fixture,
                          &(evaluation_case){.expression_text = expression},
                          &evaluation->outcome);
            status = evaluation->outcome.status;
            SXML_TEST_EXPECT(state, fixture_released_cells(&evaluation->test_fixture));
        }
        const bool has_failed_allocation =
            sxml_counting_allocator_read_counts(evaluation->test_fixture.counting_allocator)
                .failed_allocation_count > 0;
        if (!has_failed_allocation) {
            has_succeeded = true;
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OK &&
                                        strcmp(evaluation->outcome.text, expected) == 0);
        } else if (status != SXML_STATUS_OUT_OF_MEMORY) {
            (void)fprintf(stderr, "allocation %llu: status %d\n",
                          (unsigned long long)failing_number, (int)status);
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        }
        SXML_TEST_EXPECT(state, fixture_destroy(&evaluation->test_fixture));
    }
    SXML_TEST_EXPECT(state, has_succeeded);
}

static const sxml_test_case function_library_test_cases[] = {
    {.name = "library_lifecycle", .function = test_library_lifecycle},
    {.name = "library_survives_every_allocation_failure",
     .function = test_library_survives_every_allocation_failure},
    {.name = "table_finds_rows_and_constructors",
     .function = test_table_finds_rows_and_constructors},
    {.name = "every_row_resolves", .function = test_every_row_resolves},
    {.name = "numeric_functions", .function = test_numeric_functions},
    {.name = "random_number_generator", .function = test_random_number_generator},
    {.name = "string_functions", .function = test_string_functions},
    {.name = "regular_expression_functions", .function = test_regular_expression_functions},
    {.name = "sequence_functions", .function = test_sequence_functions},
    {.name = "deep_equal", .function = test_deep_equal},
    {.name = "deep_equal_unsupported_options", .function = test_deep_equal_unsupported_options},
    {.name = "deep_equal_of_nodes", .function = test_deep_equal_of_nodes},
    {.name = "node_functions", .function = test_node_functions},
    {.name = "doc_and_qname", .function = test_doc_and_qname},
    {.name = "context_functions", .function = test_context_functions},
    {.name = "error_functions", .function = test_error_functions},
    {.name = "error_has_description_and_value",
     .function = test_error_has_description_and_value},
    {.name = "evaluation_survives_every_allocation_failure",
     .function = test_evaluation_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "function_library",
            .cases = function_library_test_cases,
            .case_count = SXML_TEST_COUNT(function_library_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
