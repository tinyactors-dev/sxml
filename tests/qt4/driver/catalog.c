/*
 * The QT4 catalog (catalog.h). Imitates src/memory.c for the lifecycle;
 * element and attribute names follow third_party/qt4tests/
 * catalog-schema.xsd.
 */
#include "catalog.h"

#include <stdalign.h>
#include <string.h>

#include "suite_path.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_qt4_empty_text = {.bytes = NULL,
                                                   .byte_count = 0};

/* Arena blocks: a test-set file resolves to a few hundred kilobytes of
   arrays and paths at most. */
enum { SXML_QT4_ARENA_BLOCK_BYTE_COUNT = 64 * 1024 };

/* Catalog files are trusted; the bound only stops a broken one. */
enum { SXML_QT4_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

/* An <environment name> of catalog.xml or of a test-set file. */
typedef struct sxml_qt4_named_environment {
    sxml_byte_view name;
    sxml_node_index element;
} sxml_qt4_named_environment;

typedef struct sxml_qt4_named_environment_view {
    const sxml_qt4_named_environment *environments;
    size_t environment_count;
} sxml_qt4_named_environment_view;

/* Node indices of elements, in document order. */
typedef struct sxml_qt4_node_list {
    sxml_node_index *nodes;
    size_t node_count;
} sxml_qt4_node_list;

struct sxml_qt4_catalog {
    /* The allocator of sxml_qt4_catalog_create; the null allocator for an
       initialized catalog. */
    sxml_allocator owning_allocator;
    sxml_qt4_catalog_configuration configuration;
    /* catalog_path up to and including its last "/"; the prefix of every
       path the file reader is given. */
    sxml_byte_view catalog_directory;
    /* catalog_path after catalog_directory: the suite path of
       catalog.xml. */
    sxml_byte_view catalog_suite_path;
    bool has_started_loading;
    sxml_name_pool *name_pool;
    sxml_type_registry *type_registry;
    sxml_item_heap *item_heap;
    sxml_xml_parser *xml_parser;
    /* Lives as long as the catalog: entries and global environments. */
    sxml_arena *catalog_arena;
    /* Reset when a test set is opened. */
    sxml_arena *test_set_arena;
    sxml_qt4_document catalog_document;
    sxml_qt4_test_set_entry *test_set_entries;
    size_t test_set_entry_count;
    sxml_qt4_named_environment_view global_environments;
    /* The open test set. */
    bool has_open_test_set;
    size_t open_test_set_index;
    sxml_qt4_document test_set_document;
    sxml_qt4_named_environment_view local_environments;
    sxml_qt4_dependency_view test_set_dependencies;
    sxml_qt4_node_list test_cases;
};

/* ------------------------------------------------------------ Lifecycle */

static bool sxml_qt4_configuration_is_valid(
    const sxml_qt4_catalog_configuration *configuration) {
    return configuration != NULL &&
           !sxml_allocator_is_null(configuration->allocator) &&
           configuration->catalog_path.byte_count != 0;
}

sxml_status sxml_qt4_catalog_memory_requirements(
    const sxml_qt4_catalog_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (!sxml_qt4_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements =
        (sxml_memory_requirements){.byte_count = sizeof(sxml_qt4_catalog),
                                   .alignment = alignof(sxml_qt4_catalog)};
    return SXML_STATUS_OK;
}

sxml_status
sxml_qt4_catalog_initialize(sxml_memory_view memory,
                            const sxml_qt4_catalog_configuration *configuration,
                            sxml_qt4_catalog **catalog) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_qt4_catalog_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_qt4_catalog *result = placement;
    memset(result, 0, sizeof *result);
    result->owning_allocator =
        (sxml_allocator){.state = NULL, .operations = NULL};
    result->configuration = *configuration;
    const sxml_byte_view path = configuration->catalog_path;
    size_t directory_byte_count = path.byte_count;
    while (directory_byte_count > 0 &&
           path.bytes[directory_byte_count - 1] != '/') {
        directory_byte_count -= 1;
    }
    result->catalog_directory = (sxml_byte_view){
        .bytes = path.bytes, .byte_count = directory_byte_count};
    result->catalog_suite_path =
        (sxml_byte_view){.bytes = path.bytes + directory_byte_count,
                         .byte_count = path.byte_count - directory_byte_count};
    *catalog = result;
    return SXML_STATUS_OK;
}

/* Forgets the open test set but keeps the arena's contents, which a
   problem being reported may point into. */
static void sxml_qt4_forget_test_set(sxml_qt4_catalog *catalog) {
    sxml_qt4_document_unload(&catalog->test_set_document);
    catalog->has_open_test_set = false;
    catalog->local_environments = (sxml_qt4_named_environment_view){
        .environments = NULL, .environment_count = 0};
    catalog->test_set_dependencies =
        (sxml_qt4_dependency_view){.dependencies = NULL, .dependency_count = 0};
    catalog->test_cases = (sxml_qt4_node_list){.nodes = NULL, .node_count = 0};
}

static void sxml_qt4_close_test_set(sxml_qt4_catalog *catalog) {
    sxml_qt4_forget_test_set(catalog);
    if (catalog->test_set_arena != NULL) {
        sxml_arena_reset(catalog->test_set_arena);
    }
}

void sxml_qt4_catalog_finalize(sxml_qt4_catalog *catalog) {
    if (catalog == NULL) {
        return;
    }
    sxml_qt4_close_test_set(catalog);
    sxml_qt4_document_unload(&catalog->catalog_document);
    sxml_arena_destroy(catalog->test_set_arena);
    sxml_arena_destroy(catalog->catalog_arena);
    sxml_xml_parser_destroy(catalog->xml_parser);
    sxml_item_heap_destroy(catalog->item_heap);
    sxml_type_registry_destroy(catalog->type_registry);
    sxml_name_pool_destroy(catalog->name_pool);
    catalog->test_set_arena = NULL;
    catalog->catalog_arena = NULL;
    catalog->xml_parser = NULL;
    catalog->item_heap = NULL;
    catalog->type_registry = NULL;
    catalog->name_pool = NULL;
    catalog->test_set_entry_count = 0;
}

sxml_status
sxml_qt4_catalog_create(sxml_allocator allocator,
                        const sxml_qt4_catalog_configuration *configuration,
                        sxml_qt4_catalog **catalog) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_qt4_catalog_memory_requirements(configuration, &requirements);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = requirements.byte_count,
                                      .alignment = requirements.alignment},
            &memory);
    }
    sxml_qt4_catalog *result = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_catalog_initialize(
            (sxml_memory_view){.bytes = memory,
                               .byte_count = requirements.byte_count},
            configuration, &result);
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    result->owning_allocator = allocator;
    *catalog = result;
    return SXML_STATUS_OK;
}

void sxml_qt4_catalog_destroy(sxml_qt4_catalog *catalog) {
    if (catalog == NULL) {
        return;
    }
    const sxml_allocator allocator = catalog->owning_allocator;
    sxml_qt4_catalog_finalize(catalog);
    sxml_allocator_release(allocator, catalog);
}

/* -------------------------------------------------------------- Helpers */

static void sxml_qt4_report(sxml_qt4_problem *problem,
                            const sxml_qt4_problem *value) {
    if (problem != NULL) {
        *problem = *value;
    }
}

static sxml_qt4_document_reader
sxml_qt4_reader_of(const sxml_qt4_catalog *catalog) {
    return (sxml_qt4_document_reader){
        .allocator = catalog->configuration.allocator,
        .file_reader = catalog->configuration.file_reader,
        .xml_parser = catalog->xml_parser,
        .item_heap = catalog->item_heap};
}

/* Children of one element with one local name. */
typedef struct sxml_qt4_children_request {
    const sxml_qt4_document *document;
    sxml_node_index parent;
    sxml_byte_view local_name;
    sxml_arena *arena;
} sxml_qt4_children_request;

/* Collects the matching children in two passes: count, then fill. */
static sxml_status
sxml_qt4_collect_children(const sxml_qt4_children_request *request,
                          sxml_qt4_node_list *list) {
    size_t node_count = 0;
    sxml_node_index *nodes = NULL;
    for (size_t pass_index = 0; pass_index < 2; pass_index += 1) {
        sxml_qt4_element_cursor cursor = {.document = request->document,
                                          .has_next_node = false,
                                          .next_node = {.index = 0}};
        sxml_status status = sxml_qt4_element_cursor_start(
            request->document, request->parent, &cursor);
        size_t filled_count = 0;
        while (status == SXML_STATUS_OK) {
            sxml_qt4_element_step step = {.has_element = false,
                                          .element = {.index = 0},
                                          .local_name = sxml_qt4_empty_text};
            status = sxml_qt4_element_cursor_next(&cursor, &step);
            if (status != SXML_STATUS_OK || !step.has_element) {
                break;
            }
            if (sxml_byte_view_equals(step.local_name, request->local_name)) {
                if (nodes != NULL) {
                    nodes[filled_count] = step.element;
                }
                filled_count += 1;
            }
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (pass_index == 1 || filled_count == 0) {
            node_count = filled_count;
            break;
        }
        size_t byte_count = 0;
        status = sxml_size_multiply(filled_count, sizeof(sxml_node_index),
                                    &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status =
                sxml_arena_allocate(request->arena,
                                    (sxml_allocation_request){
                                        .byte_count = byte_count,
                                        .alignment = alignof(sxml_node_index)},
                                    &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        nodes = memory;
    }
    *list = (sxml_qt4_node_list){.nodes = nodes, .node_count = node_count};
    return SXML_STATUS_OK;
}

/* Allocates count elements of element_byte_count bytes from arena. */
typedef struct sxml_qt4_array_request {
    size_t element_count;
    size_t element_byte_count;
    size_t alignment;
} sxml_qt4_array_request;

static sxml_status sxml_qt4_allocate_array(sxml_arena *arena,
                                           sxml_qt4_array_request request,
                                           void **memory) {
    if (request.element_count == 0) {
        *memory = NULL;
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    const sxml_status status = sxml_size_multiply(
        request.element_count, request.element_byte_count, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_arena_allocate(
        arena,
        (sxml_allocation_request){.byte_count = byte_count,
                                  .alignment = request.alignment},
        memory);
}

/* One attribute of one element of a document. */
typedef struct sxml_qt4_attribute_read {
    const sxml_qt4_document *document;
    sxml_node_index element;
    sxml_byte_view local_name;
} sxml_qt4_attribute_read;

static sxml_status sxml_qt4_read_attribute(sxml_qt4_attribute_read read,
                                           sxml_qt4_attribute_value *value) {
    return sxml_qt4_document_find_attribute(
        read.document,
        (sxml_qt4_attribute_lookup){.element = read.element,
                                    .local_name = read.local_name},
        value);
}

/* The environments of a catalog or test-set element. */
typedef struct sxml_qt4_environments_request {
    const sxml_qt4_document *document;
    sxml_node_index parent;
    sxml_arena *arena;
} sxml_qt4_environments_request;

static sxml_status
sxml_qt4_collect_environments(const sxml_qt4_environments_request *request,
                              sxml_qt4_named_environment_view *environments) {
    sxml_qt4_node_list nodes = {.nodes = NULL, .node_count = 0};
    sxml_status status = sxml_qt4_collect_children(
        &(sxml_qt4_children_request){.document = request->document,
                                     .parent = request->parent,
                                     .local_name = SXML_TEXT("environment"),
                                     .arena = request->arena},
        &nodes);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_allocate_array(
            request->arena,
            (sxml_qt4_array_request){
                .element_count = nodes.node_count,
                .element_byte_count = sizeof(sxml_qt4_named_environment),
                .alignment = alignof(sxml_qt4_named_environment)},
            &memory);
    }
    sxml_qt4_named_environment *named = memory;
    for (size_t node_index = 0;
         status == SXML_STATUS_OK && nodes.nodes != NULL &&
         node_index < nodes.node_count;
         node_index += 1) {
        sxml_qt4_attribute_value name = {.is_present = false,
                                         .value = sxml_qt4_empty_text};
        status = sxml_qt4_read_attribute(
            (sxml_qt4_attribute_read){.document = request->document,
                                      .element = nodes.nodes[node_index],
                                      .local_name = SXML_TEXT("name")},
            &name);
        named[node_index] = (sxml_qt4_named_environment){
            .name = name.value, .element = nodes.nodes[node_index]};
    }
    if (status == SXML_STATUS_OK) {
        *environments = (sxml_qt4_named_environment_view){
            .environments = named, .environment_count = nodes.node_count};
    }
    return status;
}

/* The <dependency> children of a test-set or test-case element. */
static sxml_status
sxml_qt4_collect_dependencies(const sxml_qt4_environments_request *request,
                              sxml_qt4_dependency_view *dependencies) {
    sxml_qt4_node_list nodes = {.nodes = NULL, .node_count = 0};
    sxml_status status = sxml_qt4_collect_children(
        &(sxml_qt4_children_request){.document = request->document,
                                     .parent = request->parent,
                                     .local_name = SXML_TEXT("dependency"),
                                     .arena = request->arena},
        &nodes);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_allocate_array(
            request->arena,
            (sxml_qt4_array_request){.element_count = nodes.node_count,
                                     .element_byte_count =
                                         sizeof(sxml_qt4_dependency),
                                     .alignment = alignof(sxml_qt4_dependency)},
            &memory);
    }
    sxml_qt4_dependency *read = memory;
    for (size_t node_index = 0;
         status == SXML_STATUS_OK && nodes.nodes != NULL &&
         node_index < nodes.node_count;
         node_index += 1) {
        const sxml_node_index element = nodes.nodes[node_index];
        sxml_qt4_attribute_value dependency_type = {
            .is_present = false, .value = sxml_qt4_empty_text};
        sxml_qt4_attribute_value value = {.is_present = false,
                                          .value = sxml_qt4_empty_text};
        sxml_qt4_attribute_value satisfied = {.is_present = false,
                                              .value = sxml_qt4_empty_text};
        status = sxml_qt4_read_attribute(
            (sxml_qt4_attribute_read){.document = request->document,
                                      .element = element,
                                      .local_name = SXML_TEXT("type")},
            &dependency_type);
        if (status == SXML_STATUS_OK) {
            status = sxml_qt4_read_attribute(
                (sxml_qt4_attribute_read){.document = request->document,
                                          .element = element,
                                          .local_name = SXML_TEXT("value")},
                &value);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_qt4_read_attribute(
                (sxml_qt4_attribute_read){.document = request->document,
                                          .element = element,
                                          .local_name = SXML_TEXT("satisfied")},
                &satisfied);
        }
        /* satisfied is an xs:boolean defaulting to true
           (catalog-schema.xsd, "dependency"). */
        read[node_index] = (sxml_qt4_dependency){
            .dependency_type = dependency_type.value,
            .value = value.value,
            .requires_satisfaction =
                !(sxml_byte_view_equals(satisfied.value, SXML_TEXT("false")) ||
                  sxml_byte_view_equals(satisfied.value, SXML_TEXT("0")))};
    }
    if (status == SXML_STATUS_OK) {
        *dependencies = (sxml_qt4_dependency_view){
            .dependencies = read, .dependency_count = nodes.node_count};
    }
    return status;
}

/* Checks that the document element has the expected local name. */
typedef struct sxml_qt4_document_element_request {
    const sxml_qt4_document *document;
    sxml_byte_view local_name;
    sxml_byte_view suite_path;
    sxml_qt4_problem *problem;
} sxml_qt4_document_element_request;

static sxml_status
sxml_qt4_find_document_element(const sxml_qt4_document_element_request *request,
                               sxml_node_index *element) {
    sxml_qt4_element_cursor cursor = {.document = request->document,
                                      .has_next_node = false,
                                      .next_node = {.index = 0}};
    sxml_qt4_element_step step = {.has_element = false,
                                  .element = {.index = 0},
                                  .local_name = sxml_qt4_empty_text};
    sxml_status status = sxml_qt4_element_cursor_start(
        request->document, (sxml_node_index){.index = 0}, &cursor);
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_element_cursor_next(&cursor, &step);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!step.has_element ||
        !sxml_byte_view_equals(step.local_name, request->local_name)) {
        sxml_qt4_report(
            request->problem,
            &(sxml_qt4_problem){.description =
                                    SXML_TEXT("the document element is not"),
                                .subject = request->local_name,
                                .location = {.source_name = request->suite_path,
                                             .line_number = 0,
                                             .column_number = 0,
                                             .byte_offset = 0}});
        return SXML_STATUS_MALFORMED_INPUT;
    }
    *element = step.element;
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------------- Load */

static sxml_status sxml_qt4_create_parts(sxml_qt4_catalog *catalog) {
    const sxml_allocator allocator = catalog->configuration.allocator;
    sxml_status status = sxml_name_pool_create(
        allocator,
        &(sxml_name_pool_configuration){.allocator = allocator,
                                        .maximum_byte_count = 0},
        &catalog->name_pool);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_type_registry_create(allocator,
                                      &(sxml_type_registry_configuration){
                                          .allocator = allocator,
                                          .name_pool = catalog->name_pool,
                                          .maximum_registered_type_count = 0},
                                      &catalog->type_registry);
    }
    if (status == SXML_STATUS_OK) {
        status =
            sxml_item_heap_create(allocator,
                                  &(sxml_item_heap_configuration){
                                      .allocator = allocator,
                                      .name_pool = catalog->name_pool,
                                      .type_registry = catalog->type_registry,
                                      .maximum_byte_count = 0},
                                  &catalog->item_heap);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_create(
            allocator,
            &(sxml_xml_parser_configuration){
                .allocator = allocator,
                .name_pool = catalog->name_pool,
                .limits = {.maximum_depth = 0,
                           .maximum_entity_expansion_byte_count =
                               SXML_QT4_MAXIMUM_EXPANSION_BYTE_COUNT}},
            &catalog->xml_parser);
    }
    const sxml_arena_configuration arena_configuration = {
        .block_allocator = allocator,
        .block_byte_count = SXML_QT4_ARENA_BLOCK_BYTE_COUNT,
        .maximum_block_byte_count = 0,
        .initial_memory = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_create(allocator, &arena_configuration,
                                   &catalog->catalog_arena);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_create(allocator, &arena_configuration,
                                   &catalog->test_set_arena);
    }
    return status;
}

/* Reads the <test-set name file> entries of catalog.xml. */
static sxml_status
sxml_qt4_read_test_set_entries(sxml_qt4_catalog *catalog,
                               sxml_node_index catalog_element,
                               sxml_qt4_problem *problem) {
    const sxml_qt4_document *document = &catalog->catalog_document;
    sxml_qt4_node_list nodes = {.nodes = NULL, .node_count = 0};
    sxml_status status = sxml_qt4_collect_children(
        &(sxml_qt4_children_request){.document = document,
                                     .parent = catalog_element,
                                     .local_name = SXML_TEXT("test-set"),
                                     .arena = catalog->catalog_arena},
        &nodes);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_allocate_array(
            catalog->catalog_arena,
            (sxml_qt4_array_request){
                .element_count = nodes.node_count,
                .element_byte_count = sizeof(sxml_qt4_test_set_entry),
                .alignment = alignof(sxml_qt4_test_set_entry)},
            &memory);
    }
    sxml_qt4_test_set_entry *entries = memory;
    for (size_t node_index = 0;
         status == SXML_STATUS_OK && nodes.nodes != NULL &&
         node_index < nodes.node_count;
         node_index += 1) {
        const sxml_node_index element = nodes.nodes[node_index];
        sxml_qt4_attribute_value name = {.is_present = false,
                                         .value = sxml_qt4_empty_text};
        sxml_qt4_attribute_value file = {.is_present = false,
                                         .value = sxml_qt4_empty_text};
        status = sxml_qt4_read_attribute(
            (sxml_qt4_attribute_read){.document = document,
                                      .element = element,
                                      .local_name = SXML_TEXT("name")},
            &name);
        if (status == SXML_STATUS_OK) {
            status = sxml_qt4_read_attribute(
                (sxml_qt4_attribute_read){.document = document,
                                          .element = element,
                                          .local_name = SXML_TEXT("file")},
                &file);
        }
        if (status == SXML_STATUS_OK &&
            (name.value.byte_count == 0 || file.value.byte_count == 0)) {
            sxml_qt4_report(
                problem,
                &(sxml_qt4_problem){
                    .description =
                        SXML_TEXT("a test-set entry needs a name and a file"),
                    .subject = name.value,
                    .location = {.source_name = catalog->catalog_suite_path,
                                 .line_number = 0,
                                 .column_number = 0,
                                 .byte_offset = 0}});
            status = SXML_STATUS_MALFORMED_INPUT;
        }
        if (status == SXML_STATUS_OK) {
            entries[node_index].name = name.value;
            status = sxml_qt4_suite_path_resolve(
                &(sxml_qt4_suite_path_request){.containing_file =
                                                   catalog->catalog_suite_path,
                                               .reference = file.value,
                                               .arena = catalog->catalog_arena},
                &entries[node_index].file_path);
        }
    }
    if (status == SXML_STATUS_OK) {
        catalog->test_set_entries = entries;
        catalog->test_set_entry_count = nodes.node_count;
    }
    return status;
}

sxml_status sxml_qt4_catalog_load(sxml_qt4_catalog *catalog,
                                  sxml_qt4_problem *problem) {
    if (catalog->has_started_loading) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    catalog->has_started_loading = true;
    sxml_status status = sxml_qt4_create_parts(catalog);
    if (status == SXML_STATUS_OK) {
        const sxml_qt4_document_reader reader = sxml_qt4_reader_of(catalog);
        status = sxml_qt4_document_load(
            &reader,
            &(sxml_qt4_document_request){
                .file_path = catalog->configuration.catalog_path,
                .suite_path = catalog->catalog_suite_path,
                .problem = problem},
            &catalog->catalog_document);
    }
    sxml_node_index catalog_element = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_find_document_element(
            &(sxml_qt4_document_element_request){
                .document = &catalog->catalog_document,
                .local_name = SXML_TEXT("catalog"),
                .suite_path = catalog->catalog_suite_path,
                .problem = problem},
            &catalog_element);
    }
    if (status == SXML_STATUS_OK) {
        status =
            sxml_qt4_read_test_set_entries(catalog, catalog_element, problem);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_collect_environments(
            &(sxml_qt4_environments_request){.document =
                                                 &catalog->catalog_document,
                                             .parent = catalog_element,
                                             .arena = catalog->catalog_arena},
            &catalog->global_environments);
    }
    if (status != SXML_STATUS_OK) {
        catalog->test_set_entry_count = 0;
    }
    return status;
}

/* ------------------------------------------------------------ Test sets */

size_t sxml_qt4_catalog_test_set_count(const sxml_qt4_catalog *catalog) {
    return catalog->test_set_entry_count;
}

sxml_status
sxml_qt4_catalog_get_test_set_entry(const sxml_qt4_catalog *catalog,
                                    size_t test_set_index,
                                    sxml_qt4_test_set_entry *entry) {
    if (test_set_index >= catalog->test_set_entry_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    *entry = catalog->test_set_entries[test_set_index];
    return SXML_STATUS_OK;
}

bool sxml_qt4_catalog_find_test_set(const sxml_qt4_catalog *catalog,
                                    sxml_byte_view name,
                                    size_t *test_set_index) {
    for (size_t entry_index = 0; entry_index < catalog->test_set_entry_count;
         entry_index += 1) {
        if (sxml_byte_view_equals(catalog->test_set_entries[entry_index].name,
                                  name)) {
            *test_set_index = entry_index;
            return true;
        }
    }
    return false;
}

sxml_status
sxml_qt4_catalog_open_test_set(sxml_qt4_catalog *catalog,
                               const sxml_qt4_test_set_request *request) {
    sxml_qt4_close_test_set(catalog);
    if (request->test_set_index >= catalog->test_set_entry_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    const sxml_qt4_test_set_entry entry =
        catalog->test_set_entries[request->test_set_index];
    sxml_byte_view file_path = sxml_qt4_empty_text;
    sxml_status status = sxml_qt4_concatenate(
        catalog->test_set_arena,
        (sxml_qt4_concatenation){.first = catalog->catalog_directory,
                                 .second = entry.file_path},
        &file_path);
    if (status == SXML_STATUS_OK) {
        const sxml_qt4_document_reader reader = sxml_qt4_reader_of(catalog);
        status = sxml_qt4_document_load(
            &reader,
            &(sxml_qt4_document_request){.file_path = file_path,
                                         .suite_path = entry.file_path,
                                         .problem = request->problem},
            &catalog->test_set_document);
    }
    sxml_node_index test_set_element = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_find_document_element(
            &(sxml_qt4_document_element_request){
                .document = &catalog->test_set_document,
                .local_name = SXML_TEXT("test-set"),
                .suite_path = entry.file_path,
                .problem = request->problem},
            &test_set_element);
    }
    const sxml_qt4_environments_request children = {
        .document = &catalog->test_set_document,
        .parent = test_set_element,
        .arena = catalog->test_set_arena};
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_collect_environments(&children,
                                               &catalog->local_environments);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_collect_dependencies(&children,
                                               &catalog->test_set_dependencies);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_collect_children(
            &(sxml_qt4_children_request){.document =
                                             &catalog->test_set_document,
                                         .parent = test_set_element,
                                         .local_name = SXML_TEXT("test-case"),
                                         .arena = catalog->test_set_arena},
            &catalog->test_cases);
    }
    if (status != SXML_STATUS_OK) {
        sxml_qt4_forget_test_set(catalog);
        return status;
    }
    catalog->has_open_test_set = true;
    catalog->open_test_set_index = request->test_set_index;
    return SXML_STATUS_OK;
}

bool sxml_qt4_catalog_get_open_test_set(const sxml_qt4_catalog *catalog,
                                        size_t *test_set_index) {
    if (!catalog->has_open_test_set) {
        return false;
    }
    *test_set_index = catalog->open_test_set_index;
    return true;
}

const sxml_qt4_document *
sxml_qt4_catalog_get_test_set_document(const sxml_qt4_catalog *catalog) {
    return catalog->has_open_test_set ? &catalog->test_set_document : NULL;
}

size_t sxml_qt4_catalog_test_case_count(const sxml_qt4_catalog *catalog) {
    return catalog->test_cases.node_count;
}

bool sxml_qt4_catalog_find_test_case(const sxml_qt4_catalog *catalog,
                                     sxml_byte_view name,
                                     size_t *test_case_index) {
    for (size_t node_index = 0; node_index < catalog->test_cases.node_count;
         node_index += 1) {
        sxml_qt4_attribute_value found = {.is_present = false,
                                          .value = sxml_qt4_empty_text};
        if (sxml_qt4_read_attribute(
                (sxml_qt4_attribute_read){
                    .document = &catalog->test_set_document,
                    .element = catalog->test_cases.nodes[node_index],
                    .local_name = SXML_TEXT("name")},
                &found) == SXML_STATUS_OK &&
            sxml_byte_view_equals(found.value, name)) {
            *test_case_index = node_index;
            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------- Test cases */

/* The children of a <test-case> the driver needs. */
typedef struct sxml_qt4_test_case_parts {
    bool has_environment;
    sxml_node_index environment;
    bool has_test;
    sxml_node_index test;
    bool has_result;
    sxml_node_index result;
} sxml_qt4_test_case_parts;

static sxml_status
sxml_qt4_find_test_case_parts(const sxml_qt4_document *document,
                              sxml_node_index test_case,
                              sxml_qt4_test_case_parts *parts) {
    sxml_qt4_element_cursor cursor = {.document = document,
                                      .has_next_node = false,
                                      .next_node = {.index = 0}};
    sxml_status status =
        sxml_qt4_element_cursor_start(document, test_case, &cursor);
    while (status == SXML_STATUS_OK) {
        sxml_qt4_element_step step = {.has_element = false,
                                      .element = {.index = 0},
                                      .local_name = sxml_qt4_empty_text};
        status = sxml_qt4_element_cursor_next(&cursor, &step);
        if (status != SXML_STATUS_OK || !step.has_element) {
            break;
        }
        if (!parts->has_environment &&
            sxml_byte_view_equals(step.local_name, SXML_TEXT("environment"))) {
            parts->has_environment = true;
            parts->environment = step.element;
        } else if (!parts->has_test &&
                   sxml_byte_view_equals(step.local_name, SXML_TEXT("test"))) {
            parts->has_test = true;
            parts->test = step.element;
        } else if (!parts->has_result &&
                   sxml_byte_view_equals(step.local_name,
                                         SXML_TEXT("result"))) {
            parts->has_result = true;
            parts->result = step.element;
        }
    }
    return status;
}

static bool
sxml_qt4_find_named_environment(sxml_qt4_named_environment_view environments,
                                sxml_byte_view name, sxml_node_index *element) {
    for (size_t environment_index = 0;
         environment_index < environments.environment_count;
         environment_index += 1) {
        if (sxml_byte_view_equals(
                environments.environments[environment_index].name, name)) {
            *element = environments.environments[environment_index].element;
            return true;
        }
    }
    return false;
}

/* What resolving a test case's environment needs. */
typedef struct sxml_qt4_environment_resolution {
    const sxml_qt4_catalog *catalog;
    const sxml_qt4_test_case_parts *parts;
    sxml_byte_view test_set_file_path;
    sxml_qt4_problem *problem;
} sxml_qt4_environment_resolution;

static sxml_status
sxml_qt4_resolve_environment(const sxml_qt4_environment_resolution *resolution,
                             sxml_qt4_environment *environment) {
    const sxml_qt4_catalog *catalog = resolution->catalog;
    if (!resolution->parts->has_environment) {
        *environment = sxml_qt4_environment_absent();
        return SXML_STATUS_OK;
    }
    const sxml_node_index element = resolution->parts->environment;
    sxml_qt4_attribute_value reference = {.is_present = false,
                                          .value = sxml_qt4_empty_text};
    sxml_status status = sxml_qt4_read_attribute(
        (sxml_qt4_attribute_read){.document = &catalog->test_set_document,
                                  .element = element,
                                  .local_name = SXML_TEXT("ref")},
        &reference);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!reference.is_present) {
        /* An environment of the test case itself; a name on it names it
           without making it shared (catalog-schema.xsd). */
        return sxml_qt4_environment_read(
            &(sxml_qt4_environment_request){
                .document = &catalog->test_set_document,
                .element = element,
                .containing_file = resolution->test_set_file_path,
                .origin = SXML_QT4_ENVIRONMENT_ORIGIN_INLINE,
                .name = sxml_qt4_empty_text,
                .arena = catalog->test_set_arena},
            environment);
    }
    sxml_node_index found = {.index = 0};
    if (sxml_qt4_find_named_environment(catalog->local_environments,
                                        reference.value, &found)) {
        return sxml_qt4_environment_read(
            &(sxml_qt4_environment_request){
                .document = &catalog->test_set_document,
                .element = found,
                .containing_file = resolution->test_set_file_path,
                .origin = SXML_QT4_ENVIRONMENT_ORIGIN_TEST_SET,
                .name = reference.value,
                .arena = catalog->test_set_arena},
            environment);
    }
    if (sxml_qt4_find_named_environment(catalog->global_environments,
                                        reference.value, &found)) {
        return sxml_qt4_environment_read(
            &(sxml_qt4_environment_request){
                .document = &catalog->catalog_document,
                .element = found,
                .containing_file = catalog->catalog_suite_path,
                .origin = SXML_QT4_ENVIRONMENT_ORIGIN_CATALOG,
                .name = reference.value,
                .arena = catalog->test_set_arena},
            environment);
    }
    sxml_qt4_report(
        resolution->problem,
        &(sxml_qt4_problem){
            .description = SXML_TEXT("unknown environment"),
            .subject = reference.value,
            .location = {.source_name = resolution->test_set_file_path,
                         .line_number = 0,
                         .column_number = 0,
                         .byte_offset = 0}});
    return SXML_STATUS_MALFORMED_INPUT;
}

/* The <module> children of a test case. */
typedef struct sxml_qt4_module_request {
    const sxml_qt4_catalog *catalog;
    sxml_node_index test_case;
    sxml_byte_view test_set_file_path;
} sxml_qt4_module_request;

static sxml_status sxml_qt4_read_modules(const sxml_qt4_module_request *request,
                                         sxml_qt4_module_view *modules) {
    const sxml_qt4_document *document = &request->catalog->test_set_document;
    sxml_arena *arena = request->catalog->test_set_arena;
    sxml_qt4_node_list nodes = {.nodes = NULL, .node_count = 0};
    sxml_status status = sxml_qt4_collect_children(
        &(sxml_qt4_children_request){.document = document,
                                     .parent = request->test_case,
                                     .local_name = SXML_TEXT("module"),
                                     .arena = arena},
        &nodes);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_allocate_array(
            arena,
            (sxml_qt4_array_request){.element_count = nodes.node_count,
                                     .element_byte_count =
                                         sizeof(sxml_qt4_module),
                                     .alignment = alignof(sxml_qt4_module)},
            &memory);
    }
    sxml_qt4_module *read = memory;
    for (size_t node_index = 0;
         status == SXML_STATUS_OK && nodes.nodes != NULL &&
         node_index < nodes.node_count;
         node_index += 1) {
        const sxml_node_index element = nodes.nodes[node_index];
        sxml_qt4_attribute_value uri = {.is_present = false,
                                        .value = sxml_qt4_empty_text};
        sxml_qt4_attribute_value file = {.is_present = false,
                                         .value = sxml_qt4_empty_text};
        sxml_qt4_attribute_value location = {.is_present = false,
                                             .value = sxml_qt4_empty_text};
        status = sxml_qt4_read_attribute(
            (sxml_qt4_attribute_read){.document = document,
                                      .element = element,
                                      .local_name = SXML_TEXT("uri")},
            &uri);
        if (status == SXML_STATUS_OK) {
            status = sxml_qt4_read_attribute(
                (sxml_qt4_attribute_read){.document = document,
                                          .element = element,
                                          .local_name = SXML_TEXT("file")},
                &file);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_qt4_read_attribute(
                (sxml_qt4_attribute_read){.document = document,
                                          .element = element,
                                          .local_name = SXML_TEXT("location")},
                &location);
        }
        sxml_byte_view file_path = sxml_qt4_empty_text;
        if (status == SXML_STATUS_OK && file.value.byte_count != 0) {
            status = sxml_qt4_suite_path_resolve(
                &(sxml_qt4_suite_path_request){.containing_file =
                                                   request->test_set_file_path,
                                               .reference = file.value,
                                               .arena = arena},
                &file_path);
        }
        read[node_index] = (sxml_qt4_module){.uri = uri.value,
                                             .file_path = file_path,
                                             .location = location.value};
    }
    if (status == SXML_STATUS_OK) {
        *modules = (sxml_qt4_module_view){.modules = read,
                                          .module_count = nodes.node_count};
    }
    return status;
}

/* Reads <test>: inline text, or a file. */
typedef struct sxml_qt4_query_request {
    const sxml_qt4_catalog *catalog;
    sxml_node_index test;
    sxml_byte_view test_set_file_path;
} sxml_qt4_query_request;

static sxml_status sxml_qt4_read_query(const sxml_qt4_query_request *request,
                                       sxml_qt4_test_case *test_case) {
    const sxml_qt4_document *document = &request->catalog->test_set_document;
    sxml_qt4_attribute_value file = {.is_present = false,
                                     .value = sxml_qt4_empty_text};
    sxml_status status = sxml_qt4_read_attribute(
        (sxml_qt4_attribute_read){.document = document,
                                  .element = request->test,
                                  .local_name = SXML_TEXT("file")},
        &file);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (file.is_present) {
        test_case->query_text = sxml_qt4_empty_text;
        return sxml_qt4_suite_path_resolve(
            &(sxml_qt4_suite_path_request){
                .containing_file = request->test_set_file_path,
                .reference = file.value,
                .arena = request->catalog->test_set_arena},
            &test_case->query_file_path);
    }
    test_case->query_file_path = sxml_qt4_empty_text;
    return sxml_qt4_document_get_text(
        document,
        &(sxml_qt4_text_request){.element = request->test,
                                 .arena = request->catalog->test_set_arena},
        &test_case->query_text);
}

sxml_status
sxml_qt4_catalog_read_test_case(sxml_qt4_catalog *catalog,
                                const sxml_qt4_test_case_request *request,
                                sxml_qt4_test_case *test_case) {
    if (!catalog->has_open_test_set) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (request->test_case_index >= catalog->test_cases.node_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    const sxml_qt4_test_set_entry entry =
        catalog->test_set_entries[catalog->open_test_set_index];
    const sxml_qt4_document *document = &catalog->test_set_document;
    const sxml_node_index element =
        catalog->test_cases.nodes[request->test_case_index];
    sxml_qt4_test_case result = {
        .test_set_name = entry.name,
        .test_set_file_path = entry.file_path,
        .name = sxml_qt4_empty_text,
        .query_text = sxml_qt4_empty_text,
        .query_file_path = sxml_qt4_empty_text,
        .environment = sxml_qt4_environment_absent(),
        .test_set_dependencies = catalog->test_set_dependencies,
        .dependencies = {.dependencies = NULL, .dependency_count = 0},
        .modules = {.modules = NULL, .module_count = 0},
        .has_result = false,
        .result = {.index = 0}};
    sxml_qt4_attribute_value name = {.is_present = false,
                                     .value = sxml_qt4_empty_text};
    sxml_status status = sxml_qt4_read_attribute(
        (sxml_qt4_attribute_read){.document = document,
                                  .element = element,
                                  .local_name = SXML_TEXT("name")},
        &name);
    result.name = name.value;
    sxml_qt4_test_case_parts parts = {.has_environment = false,
                                      .environment = {.index = 0},
                                      .has_test = false,
                                      .test = {.index = 0},
                                      .has_result = false,
                                      .result = {.index = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_find_test_case_parts(document, element, &parts);
    }
    if (status == SXML_STATUS_OK && !parts.has_test) {
        sxml_qt4_report(
            request->problem,
            &(sxml_qt4_problem){.description =
                                    SXML_TEXT("a test case without <test>"),
                                .subject = result.name,
                                .location = {.source_name = entry.file_path,
                                             .line_number = 0,
                                             .column_number = 0,
                                             .byte_offset = 0}});
        status = SXML_STATUS_MALFORMED_INPUT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_read_query(
            &(sxml_qt4_query_request){.catalog = catalog,
                                      .test = parts.test,
                                      .test_set_file_path = entry.file_path},
            &result);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_collect_dependencies(
            &(sxml_qt4_environments_request){.document = document,
                                             .parent = element,
                                             .arena = catalog->test_set_arena},
            &result.dependencies);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_read_modules(
            &(sxml_qt4_module_request){.catalog = catalog,
                                       .test_case = element,
                                       .test_set_file_path = entry.file_path},
            &result.modules);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_resolve_environment(
            &(sxml_qt4_environment_resolution){.catalog = catalog,
                                               .parts = &parts,
                                               .test_set_file_path =
                                                   entry.file_path,
                                               .problem = request->problem},
            &result.environment);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    result.has_result = parts.has_result;
    result.result = parts.result;
    *test_case = result;
    return SXML_STATUS_OK;
}

void sxml_qt4_test_case_decide_applicability(
    sxml_qt4_test_case_target request, sxml_qt4_applicability *applicability) {
    sxml_qt4_decide_applicability(
        &(sxml_qt4_applicability_request){
            .target = request.target,
            .test_set_dependencies = request.test_case->test_set_dependencies,
            .test_case_dependencies = request.test_case->dependencies,
            .environment = &request.test_case->environment,
            .test_set_name = request.test_case->test_set_name},
        applicability);
}

sxml_status sxml_qt4_catalog_read_file(sxml_qt4_catalog *catalog,
                                       const sxml_qt4_file_request *request,
                                       sxml_byte_view *contents) {
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_add(catalog->catalog_directory.byte_count,
                      request->suite_path.byte_count, &byte_count);
    if (status == SXML_STATUS_OK && byte_count == 0) {
        /* An empty path names no file. */
        status = SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            request->allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = 1},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *path = memory;
    if (catalog->catalog_directory.byte_count != 0) {
        memcpy(path, catalog->catalog_directory.bytes,
               catalog->catalog_directory.byte_count);
    }
    if (request->suite_path.byte_count != 0) {
        memcpy(path + catalog->catalog_directory.byte_count,
               request->suite_path.bytes, request->suite_path.byte_count);
    }
    status = sxml_cli_file_reader_read(
        catalog->configuration.file_reader,
        &(sxml_cli_file_read_request){
            .path = {.bytes = path, .byte_count = byte_count},
            .allocator = request->allocator},
        contents);
    sxml_allocator_release(request->allocator, memory);
    return status;
}
