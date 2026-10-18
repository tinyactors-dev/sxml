/*
 * The XSLT 4.0 test catalog (xslt_catalog.h). Imitates
 * tests/qt4/driver/catalog.c; element and attribute names follow
 * third_party/xslt40-test/admin/catalog-schema.xsd.
 */
#include "xslt_catalog.h"

#include <stdalign.h>
#include <string.h>

#include "suite_path.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xslt_test_empty_text = {.bytes = NULL,
                                                         .byte_count = 0};

/* Arena blocks: a test-set file resolves to a few hundred kilobytes. */
enum { SXML_XSLT_TEST_ARENA_BLOCK_BYTE_COUNT = 64 * 1024 };

/* Catalog files are trusted; the bound only stops a broken one. */
enum { SXML_XSLT_TEST_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

/* An <environment name> of catalog.xml or of a test-set file. */
typedef struct sxml_xslt_test_named_environment {
    sxml_byte_view name;
    sxml_node_index element;
} sxml_xslt_test_named_environment;

typedef struct sxml_xslt_test_named_environment_view {
    const sxml_xslt_test_named_environment *environments;
    size_t environment_count;
} sxml_xslt_test_named_environment_view;

struct sxml_xslt_test_catalog {
    /* The allocator of create; the null allocator for an initialized
       catalog. */
    sxml_allocator owning_allocator;
    sxml_xslt_test_catalog_configuration configuration;
    /* catalog_path up to and including its last "/". */
    sxml_byte_view catalog_directory;
    /* catalog_path after catalog_directory. */
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
    sxml_xslt_test_named_environment_view global_environments;
    /* The open test set. */
    bool has_open_test_set;
    size_t open_test_set_index;
    sxml_qt4_document test_set_document;
    sxml_xslt_test_named_environment_view local_environments;
    sxml_qt4_dependency_view test_set_dependencies;
    sxml_xslt_test_node_list test_cases;
    /* A QT4 catalog that is never loaded, over the same files: what the
       QT4 resource table and assertions read suite files through (made by
       load). */
    sxml_qt4_catalog *file_catalog;
};

/* ------------------------------------------------------------ Lifecycle */

static bool sxml_xslt_test_configuration_is_valid(
    const sxml_xslt_test_catalog_configuration *configuration) {
    return configuration != NULL &&
           !sxml_allocator_is_null(configuration->allocator) &&
           configuration->catalog_path.byte_count != 0;
}

sxml_status sxml_xslt_test_catalog_memory_requirements(
    const sxml_xslt_test_catalog_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (!sxml_xslt_test_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){
        .byte_count = sizeof(sxml_xslt_test_catalog),
        .alignment = alignof(sxml_xslt_test_catalog)};
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_test_catalog_initialize(
    sxml_memory_view memory,
    const sxml_xslt_test_catalog_configuration *configuration,
    sxml_xslt_test_catalog **catalog) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_xslt_test_catalog_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_xslt_test_catalog *result = placement;
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
static void sxml_xslt_test_forget_test_set(sxml_xslt_test_catalog *catalog) {
    sxml_qt4_document_unload(&catalog->test_set_document);
    catalog->has_open_test_set = false;
    catalog->local_environments = (sxml_xslt_test_named_environment_view){
        .environments = NULL, .environment_count = 0};
    catalog->test_set_dependencies =
        (sxml_qt4_dependency_view){.dependencies = NULL, .dependency_count = 0};
    catalog->test_cases =
        (sxml_xslt_test_node_list){.nodes = NULL, .node_count = 0};
}

static void sxml_xslt_test_close_test_set(sxml_xslt_test_catalog *catalog) {
    sxml_xslt_test_forget_test_set(catalog);
    if (catalog->test_set_arena != NULL) {
        sxml_arena_reset(catalog->test_set_arena);
    }
}

void sxml_xslt_test_catalog_finalize(sxml_xslt_test_catalog *catalog) {
    if (catalog == NULL) {
        return;
    }
    sxml_xslt_test_close_test_set(catalog);
    sxml_qt4_document_unload(&catalog->catalog_document);
    sxml_arena_destroy(catalog->test_set_arena);
    sxml_arena_destroy(catalog->catalog_arena);
    sxml_xml_parser_destroy(catalog->xml_parser);
    sxml_item_heap_destroy(catalog->item_heap);
    sxml_type_registry_destroy(catalog->type_registry);
    sxml_name_pool_destroy(catalog->name_pool);
    sxml_qt4_catalog_destroy(catalog->file_catalog);
    catalog->file_catalog = NULL;
    catalog->test_set_arena = NULL;
    catalog->catalog_arena = NULL;
    catalog->xml_parser = NULL;
    catalog->item_heap = NULL;
    catalog->type_registry = NULL;
    catalog->name_pool = NULL;
    catalog->test_set_entry_count = 0;
}

sxml_status sxml_xslt_test_catalog_create(
    sxml_allocator allocator,
    const sxml_xslt_test_catalog_configuration *configuration,
    sxml_xslt_test_catalog **catalog) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_xslt_test_catalog_memory_requirements(configuration, &requirements);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = requirements.byte_count,
                                      .alignment = requirements.alignment},
            &memory);
    }
    sxml_xslt_test_catalog *result = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_catalog_initialize(
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

void sxml_xslt_test_catalog_destroy(sxml_xslt_test_catalog *catalog) {
    if (catalog == NULL) {
        return;
    }
    const sxml_allocator allocator = catalog->owning_allocator;
    sxml_xslt_test_catalog_finalize(catalog);
    sxml_allocator_release(allocator, catalog);
}

/* -------------------------------------------------------------- Helpers */

static void sxml_xslt_test_set_problem(sxml_qt4_problem *problem,
                                       const sxml_qt4_problem *value) {
    if (problem != NULL) {
        *problem = *value;
    }
}

static sxml_qt4_document_reader
sxml_xslt_test_reader_of(const sxml_xslt_test_catalog *catalog) {
    return (sxml_qt4_document_reader){
        .allocator = catalog->configuration.allocator,
        .file_reader = catalog->configuration.file_reader,
        .xml_parser = catalog->xml_parser,
        .item_heap = catalog->item_heap};
}

/* The <environment> children of a catalog or test-set element. */
static sxml_status sxml_xslt_test_collect_environments(
    const sxml_xslt_test_element_reader *reader, sxml_node_index parent,
    sxml_xslt_test_named_environment_view *environments) {
    sxml_xslt_test_node_list nodes = {.nodes = NULL, .node_count = 0};
    sxml_status status = sxml_xslt_test_collect_children(
        reader,
        (sxml_xslt_test_children_request){.parent = parent,
                                          .local_name =
                                              SXML_TEXT("environment")},
        &nodes);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_allocate_array(
            reader->arena,
            (sxml_xslt_test_array_request){
                .element_count = nodes.node_count,
                .element_byte_count = sizeof(sxml_xslt_test_named_environment),
                .alignment = alignof(sxml_xslt_test_named_environment)},
            &memory);
    }
    sxml_xslt_test_named_environment *named = memory;
    for (size_t node_index = 0;
         status == SXML_STATUS_OK && named != NULL &&
         node_index < nodes.node_count;
         node_index += 1) {
        named[node_index].element = nodes.nodes[node_index];
        status = sxml_xslt_test_read_attribute(
            reader,
            (sxml_qt4_attribute_lookup){.element = nodes.nodes[node_index],
                                        .local_name = SXML_TEXT("name")},
            &named[node_index].name);
    }
    if (status == SXML_STATUS_OK) {
        *environments = (sxml_xslt_test_named_environment_view){
            .environments = named, .environment_count = nodes.node_count};
    }
    return status;
}

/* Reads one dependency element: its local name is the dependency type
   (catalog-schema.xsd, the substitution group "dependency"). */
static sxml_status
sxml_xslt_test_read_dependency(const sxml_xslt_test_element_reader *reader,
                               sxml_qt4_element_step step,
                               sxml_qt4_dependency *dependency) {
    sxml_byte_view value = sxml_xslt_test_empty_text;
    sxml_status status = sxml_xslt_test_read_attribute(
        reader,
        (sxml_qt4_attribute_lookup){.element = step.element,
                                    .local_name = SXML_TEXT("value")},
        &value);
    bool is_satisfied = true;
    if (status == SXML_STATUS_OK) {
        /* satisfied defaults to true (catalog-schema.xsd,
           "dependencyType"). */
        status = sxml_xslt_test_read_flag(
            reader,
            (sxml_xslt_test_flag_lookup){.element = step.element,
                                         .local_name = SXML_TEXT("satisfied"),
                                         .default_value = true,
                                         .value = &is_satisfied});
    }
    *dependency = (sxml_qt4_dependency){.dependency_type = step.local_name,
                                        .value = value,
                                        .requires_satisfaction = is_satisfied};
    return status;
}

/* The children of every <dependencies> child of parent, in document
   order: counted, then read. */
static sxml_status
sxml_xslt_test_collect_dependencies(const sxml_xslt_test_element_reader *reader,
                                    sxml_node_index parent,
                                    sxml_qt4_dependency_view *dependencies) {
    sxml_xslt_test_node_list wrappers = {.nodes = NULL, .node_count = 0};
    sxml_status status = sxml_xslt_test_collect_children(
        reader,
        (sxml_xslt_test_children_request){.parent = parent,
                                          .local_name =
                                              SXML_TEXT("dependencies")},
        &wrappers);
    sxml_qt4_dependency *read = NULL;
    size_t dependency_count = 0;
    for (size_t pass_index = 0; status == SXML_STATUS_OK && pass_index < 2;
         pass_index += 1) {
        size_t filled_count = 0;
        for (size_t wrapper_index = 0;
             status == SXML_STATUS_OK && wrapper_index < wrappers.node_count;
             wrapper_index += 1) {
            sxml_qt4_element_cursor cursor = {.document = reader->document,
                                              .has_next_node = false,
                                              .next_node = {.index = 0}};
            status = sxml_qt4_element_cursor_start(
                reader->document, wrappers.nodes[wrapper_index], &cursor);
            while (status == SXML_STATUS_OK) {
                sxml_qt4_element_step step = {
                    .has_element = false,
                    .element = {.index = 0},
                    .local_name = sxml_xslt_test_empty_text};
                status = sxml_qt4_element_cursor_next(&cursor, &step);
                if (status != SXML_STATUS_OK || !step.has_element) {
                    break;
                }
                if (read != NULL) {
                    status = sxml_xslt_test_read_dependency(
                        reader, step, &read[filled_count]);
                }
                filled_count += 1;
            }
        }
        dependency_count = filled_count;
        if (status != SXML_STATUS_OK || pass_index == 1 || filled_count == 0) {
            break;
        }
        void *memory = NULL;
        status = sxml_xslt_test_allocate_array(
            reader->arena,
            (sxml_xslt_test_array_request){
                .element_count = filled_count,
                .element_byte_count = sizeof(sxml_qt4_dependency),
                .alignment = alignof(sxml_qt4_dependency)},
            &memory);
        read = memory;
    }
    if (status == SXML_STATUS_OK) {
        *dependencies = (sxml_qt4_dependency_view){
            .dependencies = read, .dependency_count = dependency_count};
    }
    return status;
}

/* Finds the document element and checks its local name. */
typedef struct sxml_xslt_test_document_element_request {
    const sxml_xslt_test_element_reader *reader;
    sxml_byte_view local_name;
} sxml_xslt_test_document_element_request;

static sxml_status sxml_xslt_test_find_document_element(
    sxml_xslt_test_document_element_request request, sxml_node_index *element) {
    const sxml_xslt_test_element_reader *reader = request.reader;
    sxml_qt4_element_cursor cursor = {.document = reader->document,
                                      .has_next_node = false,
                                      .next_node = {.index = 0}};
    sxml_qt4_element_step step = {.has_element = false,
                                  .element = {.index = 0},
                                  .local_name = sxml_xslt_test_empty_text};
    sxml_status status = sxml_qt4_element_cursor_start(
        reader->document, (sxml_node_index){.index = 0}, &cursor);
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_element_cursor_next(&cursor, &step);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!step.has_element ||
        !sxml_byte_view_equals(step.local_name, request.local_name)) {
        sxml_xslt_test_report_problem(
            reader, (sxml_xslt_test_problem_text){
                        .description = SXML_TEXT(
                            "the document element is not an XSLT catalog"),
                        .subject = request.local_name});
        return SXML_STATUS_MALFORMED_INPUT;
    }
    *element = step.element;
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------------- Load */

static sxml_status sxml_xslt_test_create_parts(sxml_xslt_test_catalog *catalog) {
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
                               SXML_XSLT_TEST_MAXIMUM_EXPANSION_BYTE_COUNT}},
            &catalog->xml_parser);
    }
    const sxml_arena_configuration arena_configuration = {
        .block_allocator = allocator,
        .block_byte_count = SXML_XSLT_TEST_ARENA_BLOCK_BYTE_COUNT,
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
sxml_xslt_test_read_test_set_entries(sxml_xslt_test_catalog *catalog,
                                     const sxml_xslt_test_element_reader *reader,
                                     sxml_node_index catalog_element) {
    sxml_xslt_test_node_list nodes = {.nodes = NULL, .node_count = 0};
    sxml_status status = sxml_xslt_test_collect_children(
        reader,
        (sxml_xslt_test_children_request){.parent = catalog_element,
                                          .local_name = SXML_TEXT("test-set")},
        &nodes);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_allocate_array(
            catalog->catalog_arena,
            (sxml_xslt_test_array_request){
                .element_count = nodes.node_count,
                .element_byte_count = sizeof(sxml_qt4_test_set_entry),
                .alignment = alignof(sxml_qt4_test_set_entry)},
            &memory);
    }
    sxml_qt4_test_set_entry *entries = memory;
    for (size_t node_index = 0;
         status == SXML_STATUS_OK && entries != NULL &&
         node_index < nodes.node_count;
         node_index += 1) {
        const sxml_node_index element = nodes.nodes[node_index];
        sxml_byte_view name = sxml_xslt_test_empty_text;
        sxml_byte_view file = sxml_xslt_test_empty_text;
        status = sxml_xslt_test_read_attribute(
            reader,
            (sxml_qt4_attribute_lookup){.element = element,
                                        .local_name = SXML_TEXT("name")},
            &name);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_test_read_attribute(
                reader,
                (sxml_qt4_attribute_lookup){.element = element,
                                            .local_name = SXML_TEXT("file")},
                &file);
        }
        if (status == SXML_STATUS_OK &&
            (name.byte_count == 0 || file.byte_count == 0)) {
            sxml_xslt_test_report_problem(
                reader,
                (sxml_xslt_test_problem_text){
                    .description =
                        SXML_TEXT("a test-set entry needs a name and a file"),
                    .subject = name});
            status = SXML_STATUS_MALFORMED_INPUT;
        }
        if (status == SXML_STATUS_OK) {
            entries[node_index].name = name;
            status = sxml_xslt_test_read_file_attribute(
                reader, element, &entries[node_index].file_path);
        }
    }
    if (status == SXML_STATUS_OK) {
        catalog->test_set_entries = entries;
        catalog->test_set_entry_count = nodes.node_count;
    }
    return status;
}

sxml_status sxml_xslt_test_catalog_load(sxml_xslt_test_catalog *catalog,
                                        sxml_qt4_problem *problem) {
    if (catalog->has_started_loading) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    catalog->has_started_loading = true;
    sxml_status status = sxml_xslt_test_create_parts(catalog);
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_catalog_create(catalog->configuration.allocator,
                                         &catalog->configuration,
                                         &catalog->file_catalog);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_qt4_document_reader reader =
            sxml_xslt_test_reader_of(catalog);
        status = sxml_xslt_test_document_load(
            &reader,
            &(sxml_qt4_document_request){
                .file_path = catalog->configuration.catalog_path,
                .suite_path = catalog->catalog_suite_path,
                .problem = problem},
            &catalog->catalog_document);
    }
    const sxml_xslt_test_element_reader reader = {
        .document = &catalog->catalog_document,
        .containing_file = catalog->catalog_suite_path,
        .arena = catalog->catalog_arena,
        .problem = problem};
    sxml_node_index catalog_element = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_find_document_element(
            (sxml_xslt_test_document_element_request){
                .reader = &reader, .local_name = SXML_TEXT("catalog")},
            &catalog_element);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_test_set_entries(catalog, &reader,
                                                      catalog_element);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_collect_environments(
            &reader, catalog_element, &catalog->global_environments);
    }
    if (status != SXML_STATUS_OK) {
        catalog->test_set_entry_count = 0;
    }
    return status;
}

/* ------------------------------------------------------------ Test sets */

size_t sxml_xslt_test_catalog_test_set_count(
    const sxml_xslt_test_catalog *catalog) {
    return catalog->test_set_entry_count;
}

sxml_status sxml_xslt_test_catalog_get_test_set_entry(
    const sxml_xslt_test_catalog *catalog, size_t test_set_index,
    sxml_qt4_test_set_entry *entry) {
    if (test_set_index >= catalog->test_set_entry_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    *entry = catalog->test_set_entries[test_set_index];
    return SXML_STATUS_OK;
}

bool sxml_xslt_test_catalog_find_test_set(
    const sxml_xslt_test_catalog *catalog, sxml_byte_view name,
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

sxml_status sxml_xslt_test_catalog_open_test_set(
    sxml_xslt_test_catalog *catalog, const sxml_qt4_test_set_request *request) {
    sxml_xslt_test_close_test_set(catalog);
    if (request->test_set_index >= catalog->test_set_entry_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    const sxml_qt4_test_set_entry entry =
        catalog->test_set_entries[request->test_set_index];
    sxml_byte_view file_path = sxml_xslt_test_empty_text;
    sxml_status status = sxml_qt4_concatenate(
        catalog->test_set_arena,
        (sxml_qt4_concatenation){.first = catalog->catalog_directory,
                                 .second = entry.file_path},
        &file_path);
    if (status == SXML_STATUS_OK) {
        const sxml_qt4_document_reader reader =
            sxml_xslt_test_reader_of(catalog);
        status = sxml_xslt_test_document_load(
            &reader,
            &(sxml_qt4_document_request){.file_path = file_path,
                                         .suite_path = entry.file_path,
                                         .problem = request->problem},
            &catalog->test_set_document);
    }
    const sxml_xslt_test_element_reader reader = {
        .document = &catalog->test_set_document,
        .containing_file = entry.file_path,
        .arena = catalog->test_set_arena,
        .problem = request->problem};
    sxml_node_index test_set_element = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_find_document_element(
            (sxml_xslt_test_document_element_request){
                .reader = &reader, .local_name = SXML_TEXT("test-set")},
            &test_set_element);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_collect_environments(
            &reader, test_set_element, &catalog->local_environments);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_collect_dependencies(
            &reader, test_set_element, &catalog->test_set_dependencies);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_collect_children(
            &reader,
            (sxml_xslt_test_children_request){.parent = test_set_element,
                                              .local_name =
                                                  SXML_TEXT("test-case")},
            &catalog->test_cases);
    }
    if (status != SXML_STATUS_OK) {
        sxml_xslt_test_forget_test_set(catalog);
        return status;
    }
    catalog->has_open_test_set = true;
    catalog->open_test_set_index = request->test_set_index;
    return SXML_STATUS_OK;
}

bool sxml_xslt_test_catalog_get_open_test_set(
    const sxml_xslt_test_catalog *catalog, size_t *test_set_index) {
    if (!catalog->has_open_test_set) {
        return false;
    }
    *test_set_index = catalog->open_test_set_index;
    return true;
}

const sxml_qt4_document *sxml_xslt_test_catalog_get_test_set_document(
    const sxml_xslt_test_catalog *catalog) {
    return catalog->has_open_test_set ? &catalog->test_set_document : NULL;
}

size_t sxml_xslt_test_catalog_test_case_count(
    const sxml_xslt_test_catalog *catalog) {
    return catalog->test_cases.node_count;
}

bool sxml_xslt_test_catalog_find_test_case(
    const sxml_xslt_test_catalog *catalog, sxml_byte_view name,
    size_t *test_case_index) {
    for (size_t node_index = 0; node_index < catalog->test_cases.node_count;
         node_index += 1) {
        sxml_qt4_attribute_value found = {.is_present = false,
                                          .value = sxml_xslt_test_empty_text};
        if (sxml_qt4_document_find_attribute(
                &catalog->test_set_document,
                (sxml_qt4_attribute_lookup){
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
typedef struct sxml_xslt_test_case_parts {
    bool has_environment;
    sxml_node_index environment;
    bool has_test;
    sxml_node_index test;
    bool has_result;
    sxml_node_index result;
} sxml_xslt_test_case_parts;

static sxml_status
sxml_xslt_test_find_case_parts(const sxml_qt4_document *document,
                               sxml_node_index test_case,
                               sxml_xslt_test_case_parts *parts) {
    sxml_qt4_element_cursor cursor = {.document = document,
                                      .has_next_node = false,
                                      .next_node = {.index = 0}};
    sxml_status status =
        sxml_qt4_element_cursor_start(document, test_case, &cursor);
    while (status == SXML_STATUS_OK) {
        sxml_qt4_element_step step = {.has_element = false,
                                      .element = {.index = 0},
                                      .local_name = sxml_xslt_test_empty_text};
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

static bool sxml_xslt_test_find_named_environment(
    sxml_xslt_test_named_environment_view environments, sxml_byte_view name,
    sxml_node_index *element) {
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
typedef struct sxml_xslt_test_environment_resolution {
    const sxml_xslt_test_catalog *catalog;
    const sxml_xslt_test_case_parts *parts;
    sxml_byte_view test_set_file_path;
    sxml_qt4_problem *problem;
} sxml_xslt_test_environment_resolution;

static sxml_status sxml_xslt_test_resolve_environment(
    const sxml_xslt_test_environment_resolution *resolution,
    sxml_xslt_test_environment *environment) {
    const sxml_xslt_test_catalog *catalog = resolution->catalog;
    if (!resolution->parts->has_environment) {
        *environment = sxml_xslt_test_environment_absent();
        return SXML_STATUS_OK;
    }
    const sxml_node_index element = resolution->parts->environment;
    sxml_qt4_attribute_value reference = {.is_present = false,
                                          .value = sxml_xslt_test_empty_text};
    sxml_status status = sxml_qt4_document_find_attribute(
        &catalog->test_set_document,
        (sxml_qt4_attribute_lookup){.element = element,
                                    .local_name = SXML_TEXT("ref")},
        &reference);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_qt4_environment_request request = {
        .document = &catalog->test_set_document,
        .element = element,
        .containing_file = resolution->test_set_file_path,
        .origin = SXML_QT4_ENVIRONMENT_ORIGIN_INLINE,
        .name = sxml_xslt_test_empty_text,
        .arena = catalog->test_set_arena};
    sxml_node_index found = {.index = 0};
    if (!reference.is_present) {
        /* The test case's own environment (catalog-schema.xsd: the name
           attribute is prohibited there). */
    } else if (sxml_xslt_test_find_named_environment(
                   catalog->local_environments, reference.value, &found)) {
        request.element = found;
        request.origin = SXML_QT4_ENVIRONMENT_ORIGIN_TEST_SET;
        request.name = reference.value;
    } else if (sxml_xslt_test_find_named_environment(
                   catalog->global_environments, reference.value, &found)) {
        request.document = &catalog->catalog_document;
        request.element = found;
        request.containing_file = catalog->catalog_suite_path;
        request.origin = SXML_QT4_ENVIRONMENT_ORIGIN_CATALOG;
        request.name = reference.value;
    } else {
        sxml_xslt_test_set_problem(
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
    return sxml_xslt_test_environment_read(&request, resolution->problem,
                                           environment);
}

/* The group of a test-set file: the segment after a leading "tests/",
   else the first segment. */
static sxml_byte_view sxml_xslt_test_group_of(sxml_byte_view suite_path) {
    sxml_byte_view rest = suite_path;
    const sxml_byte_view tests_prefix = SXML_TEXT("tests/");
    if (rest.byte_count > tests_prefix.byte_count &&
        memcmp(rest.bytes, tests_prefix.bytes, tests_prefix.byte_count) == 0) {
        rest.bytes += tests_prefix.byte_count;
        rest.byte_count -= tests_prefix.byte_count;
    }
    size_t segment_byte_count = 0;
    while (segment_byte_count < rest.byte_count &&
           rest.bytes[segment_byte_count] != '/') {
        segment_byte_count += 1;
    }
    if (segment_byte_count == rest.byte_count) {
        return sxml_xslt_test_empty_text;
    }
    return (sxml_byte_view){.bytes = rest.bytes,
                            .byte_count = segment_byte_count};
}

/* Reads one initial-template, initial-mode or initial-function child. */
typedef struct sxml_xslt_test_initial_element {
    sxml_byte_view local_name;
    sxml_xslt_test_name *name;
    bool *is_present;
    sxml_node_index *element;
} sxml_xslt_test_initial_element;

static sxml_status
sxml_xslt_test_read_initial(const sxml_xslt_test_element_reader *reader,
                            sxml_node_index test,
                            sxml_xslt_test_initial_element initial) {
    sxml_xslt_test_child_lookup lookup = {.element = {.index = 0},
                                          .is_found = false};
    sxml_status status = sxml_xslt_test_find_child(
        reader,
        (sxml_xslt_test_children_request){.parent = test,
                                          .local_name = initial.local_name},
        &lookup);
    *initial.is_present = lookup.is_found;
    if (status == SXML_STATUS_OK && lookup.is_found) {
        *initial.element = lookup.element;
        status = sxml_xslt_test_read_name(
            reader,
            (sxml_qt4_attribute_lookup){.element = lookup.element,
                                        .local_name = SXML_TEXT("name")},
            initial.name);
    }
    return status;
}

/* Reads the invocation of <test> (xslt_catalog.h). */
static sxml_status
sxml_xslt_test_read_invocation(const sxml_xslt_test_element_reader *reader,
                               sxml_node_index test,
                               sxml_xslt_test_invocation *invocation) {
    *invocation = (sxml_xslt_test_invocation){
        .initial_template = sxml_xslt_test_no_name(),
        .initial_mode = sxml_xslt_test_no_name(),
        .initial_match_selection = sxml_xslt_test_empty_text,
        .initial_function = sxml_xslt_test_no_name(),
        .parameters = {.parameters = NULL, .parameter_count = 0},
        .invocation_kind = SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES,
        .has_initial_template = false,
        .has_initial_mode = false,
        .has_ignored_initial_mode = false};
    sxml_node_index template_element = {.index = 0};
    sxml_node_index mode_element = {.index = 0};
    sxml_node_index function_element = {.index = 0};
    bool has_initial_function = false;
    sxml_status status = sxml_xslt_test_read_initial(
        reader, test,
        (sxml_xslt_test_initial_element){
            .local_name = SXML_TEXT("initial-template"),
            .name = &invocation->initial_template,
            .is_present = &invocation->has_initial_template,
            .element = &template_element});
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_initial(
            reader, test,
            (sxml_xslt_test_initial_element){
                .local_name = SXML_TEXT("initial-mode"),
                .name = &invocation->initial_mode,
                .is_present = &invocation->has_initial_mode,
                .element = &mode_element});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_initial(
            reader, test,
            (sxml_xslt_test_initial_element){
                .local_name = SXML_TEXT("initial-function"),
                .name = &invocation->initial_function,
                .is_present = &has_initial_function,
                .element = &function_element});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_node_index parameter_parent = {.index = 0};
    bool has_parameter_parent = true;
    if (has_initial_function) {
        invocation->invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION;
        parameter_parent = function_element;
    } else if (invocation->has_initial_template) {
        invocation->invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE;
        invocation->has_ignored_initial_mode = invocation->has_initial_mode;
        parameter_parent = template_element;
    } else if (invocation->has_initial_mode) {
        parameter_parent = mode_element;
        status = sxml_xslt_test_read_attribute(
            reader,
            (sxml_qt4_attribute_lookup){.element = mode_element,
                                        .local_name = SXML_TEXT("select")},
            &invocation->initial_match_selection);
    } else {
        has_parameter_parent = false;
    }
    if (status == SXML_STATUS_OK && has_parameter_parent) {
        status = sxml_xslt_test_read_parameters(reader, parameter_parent,
                                                &invocation->parameters);
    }
    return status;
}

/* Reads the parts of <test>: modules, params, invocation, output. */
static sxml_status
sxml_xslt_test_read_test_element(const sxml_xslt_test_element_reader *reader,
                                 sxml_node_index test,
                                 sxml_xslt_test_case *test_case) {
    sxml_status status =
        sxml_xslt_test_read_modules(reader, test, &test_case->modules);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_xslt_test_read_parameters(reader, test, &test_case->parameters);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_invocation(reader, test,
                                                &test_case->invocation);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_output(reader, test, &test_case->output);
    }
    sxml_xslt_test_child_lookup posture = {.element = {.index = 0},
                                           .is_found = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_find_child(
            reader,
            (sxml_xslt_test_children_request){
                .parent = test, .local_name = SXML_TEXT("posture-and-sweep")},
            &posture);
        test_case->has_posture_and_sweep = posture.is_found;
    }
    return status;
}

sxml_status sxml_xslt_test_catalog_read_test_case(
    sxml_xslt_test_catalog *catalog, const sxml_qt4_test_case_request *request,
    sxml_xslt_test_case *test_case) {
    if (!catalog->has_open_test_set) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (request->test_case_index >= catalog->test_cases.node_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    const sxml_node_index element =
        catalog->test_cases.nodes[request->test_case_index];
    const sxml_qt4_test_set_entry entry =
        catalog->test_set_entries[catalog->open_test_set_index];
    const sxml_xslt_test_element_reader reader = {
        .document = &catalog->test_set_document,
        .containing_file = entry.file_path,
        .arena = catalog->test_set_arena,
        .problem = request->problem};
    sxml_xslt_test_case result = {
        .test_set_name = entry.name,
        .test_set_file_path = entry.file_path,
        .group_name = sxml_xslt_test_group_of(entry.file_path),
        .name = sxml_xslt_test_empty_text,
        .environment = sxml_xslt_test_environment_absent(),
        .modules = {.modules = NULL, .module_count = 0},
        .parameters = {.parameters = NULL, .parameter_count = 0},
        .output = sxml_xslt_test_output_absent(),
        .has_posture_and_sweep = false,
        .test_set_dependencies = catalog->test_set_dependencies,
        .dependencies = {.dependencies = NULL, .dependency_count = 0},
        .result = {.index = 0}};
    sxml_status status = sxml_xslt_test_read_attribute(
        &reader,
        (sxml_qt4_attribute_lookup){.element = element,
                                    .local_name = SXML_TEXT("name")},
        &result.name);
    sxml_xslt_test_case_parts parts = {.has_environment = false,
                                       .environment = {.index = 0},
                                       .has_test = false,
                                       .test = {.index = 0},
                                       .has_result = false,
                                       .result = {.index = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_find_case_parts(&catalog->test_set_document,
                                                element, &parts);
    }
    if (status == SXML_STATUS_OK && (!parts.has_test || !parts.has_result)) {
        sxml_xslt_test_report_problem(
            &reader, (sxml_xslt_test_problem_text){
                         .description = parts.has_test
                                            ? SXML_TEXT("no <result> in")
                                            : SXML_TEXT("no <test> in"),
                         .subject = result.name});
        status = SXML_STATUS_MALFORMED_INPUT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_resolve_environment(
            &(sxml_xslt_test_environment_resolution){
                .catalog = catalog,
                .parts = &parts,
                .test_set_file_path = entry.file_path,
                .problem = request->problem},
            &result.environment);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_collect_dependencies(&reader, element,
                                                     &result.dependencies);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_test_element(&reader, parts.test, &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!result.output.is_present) {
        result.output = result.environment.output;
    }
    result.result = parts.result;
    *test_case = result;
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_test_catalog_read_file(
    sxml_xslt_test_catalog *catalog, const sxml_qt4_file_request *request,
    sxml_byte_view *contents) {
    if (request->suite_path.byte_count == 0) {
        /* An empty path names no file. */
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    if (catalog->test_set_arena == NULL) {
        /* Not loaded. */
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_byte_view path = sxml_xslt_test_empty_text;
    sxml_status status = sxml_qt4_concatenate(
        catalog->test_set_arena,
        (sxml_qt4_concatenation){.first = catalog->catalog_directory,
                                 .second = request->suite_path},
        &path);
    if (status == SXML_STATUS_OK) {
        status = sxml_cli_file_reader_read(
            catalog->configuration.file_reader,
            &(sxml_cli_file_read_request){.path = path,
                                          .allocator = request->allocator},
            contents);
    }
    return status;
}

sxml_qt4_catalog *
sxml_xslt_test_catalog_get_file_catalog(const sxml_xslt_test_catalog *catalog) {
    return catalog->file_catalog;
}

/* ------------------------------------------------------- Test catalog */

static size_t sxml_xslt_test_get_test_set_count(const void *state) {
    return sxml_xslt_test_catalog_test_set_count(state);
}

static sxml_byte_view sxml_xslt_test_get_test_set_name(const void *state,
                                                       size_t test_set_index) {
    sxml_qt4_test_set_entry entry = {.name = sxml_xslt_test_empty_text,
                                     .file_path = sxml_xslt_test_empty_text};
    if (sxml_xslt_test_catalog_get_test_set_entry(state, test_set_index,
                                                  &entry) != SXML_STATUS_OK) {
        return sxml_xslt_test_empty_text;
    }
    return entry.name;
}

static bool sxml_xslt_test_find_test_set(const void *state,
                                         sxml_byte_view name,
                                         size_t *test_set_index) {
    return sxml_xslt_test_catalog_find_test_set(state, name, test_set_index);
}

static sxml_status
sxml_xslt_test_open_test_set(void *state,
                             const sxml_qt4_test_set_request *request) {
    return sxml_xslt_test_catalog_open_test_set(state, request);
}

static bool sxml_xslt_test_get_open_test_set(const void *state,
                                             size_t *test_set_index) {
    return sxml_xslt_test_catalog_get_open_test_set(state, test_set_index);
}

static size_t sxml_xslt_test_get_test_case_count(const void *state) {
    return sxml_xslt_test_catalog_test_case_count(state);
}

static bool sxml_xslt_test_find_test_case(const void *state,
                                          sxml_byte_view name,
                                          size_t *test_case_index) {
    return sxml_xslt_test_catalog_find_test_case(state, name, test_case_index);
}

static const sxml_qt4_test_catalog_operations
    sxml_xslt_test_test_catalog_operations = {
        .get_test_set_count = sxml_xslt_test_get_test_set_count,
        .get_test_set_name = sxml_xslt_test_get_test_set_name,
        .find_test_set = sxml_xslt_test_find_test_set,
        .open_test_set = sxml_xslt_test_open_test_set,
        .get_open_test_set = sxml_xslt_test_get_open_test_set,
        .get_test_case_count = sxml_xslt_test_get_test_case_count,
        .find_test_case = sxml_xslt_test_find_test_case,
};

sxml_qt4_test_catalog
sxml_xslt_test_catalog_as_test_catalog(sxml_xslt_test_catalog *catalog) {
    return (sxml_qt4_test_catalog){
        .state = catalog, .operations = &sxml_xslt_test_test_catalog_operations};
}

sxml_xslt_test_catalog *
sxml_xslt_test_selection_catalog(const sxml_qt4_selection *selection) {
    return selection->catalog.state;
}
