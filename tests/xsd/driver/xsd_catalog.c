/*
 * The XSD 1.1 test catalog (xsd_catalog.h). Imitates
 * tests/xslt/driver/xslt_catalog.c; element and attribute names follow
 * third_party/xsdtests/common/xsts.xsd.
 */
#include "xsd_catalog.h"

#include <stdalign.h>
#include <string.h>

#include "suite_path.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xsd_test_empty_text = {.bytes = NULL,
                                                        .byte_count = 0};

/* Arena blocks: the names of a test set's tests take a few hundred
   kilobytes for the largest (NIST, 23,000 tests). */
enum { SXML_XSD_TEST_ARENA_BLOCK_BYTE_COUNT = 64 * 1024 };

/* Suite files are trusted; the bound only stops a broken one. */
enum { SXML_XSD_TEST_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

/* One test of the open test set. */
typedef struct sxml_xsd_test_entry {
    sxml_node_index group;
    sxml_node_index test;
    sxml_xsd_test_kind kind;
    /* "GROUP/TEST", in the test-set arena. */
    sxml_byte_view name;
} sxml_xsd_test_entry;

struct sxml_xsd_test_catalog {
    /* The allocator of create; the null allocator for an initialized
       catalog. */
    sxml_allocator owning_allocator;
    sxml_xsd_test_catalog_configuration configuration;
    /* catalog_path up to and including its last "/". */
    sxml_byte_view catalog_directory;
    /* catalog_path after catalog_directory. */
    sxml_byte_view catalog_suite_path;
    bool has_started_loading;
    sxml_name_pool *name_pool;
    sxml_type_registry *type_registry;
    sxml_item_heap *item_heap;
    sxml_xml_parser *xml_parser;
    /* Lives as long as the catalog: the entries. */
    sxml_arena *catalog_arena;
    /* Reset when a test set is opened. */
    sxml_arena *test_set_arena;
    sxml_qt4_test_set_entry *test_set_entries;
    size_t test_set_entry_count;
    /* The open test set. */
    bool has_open_test_set;
    size_t open_test_set_index;
    sxml_qt4_document test_set_document;
    sxml_byte_view contributor;
    sxml_byte_view test_set_version;
    sxml_xsd_test_entry *tests;
    size_t test_count;
    /* A QT4 catalog that is never loaded, over the same files: what suite
       files are read through, as the QT4 resource table will (made by
       load). */
    sxml_qt4_catalog *file_catalog;
};

/* ------------------------------------------------------------ Lifecycle */

static bool sxml_xsd_test_configuration_is_valid(
    const sxml_xsd_test_catalog_configuration *configuration) {
    return configuration != NULL &&
           !sxml_allocator_is_null(configuration->allocator) &&
           configuration->catalog_path.byte_count != 0;
}

sxml_status sxml_xsd_test_catalog_memory_requirements(
    const sxml_xsd_test_catalog_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (!sxml_xsd_test_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements =
        (sxml_memory_requirements){.byte_count = sizeof(sxml_xsd_test_catalog),
                                   .alignment = alignof(sxml_xsd_test_catalog)};
    return SXML_STATUS_OK;
}

sxml_status sxml_xsd_test_catalog_initialize(
    sxml_memory_view memory,
    const sxml_xsd_test_catalog_configuration *configuration,
    sxml_xsd_test_catalog **catalog) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_xsd_test_catalog_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_xsd_test_catalog *result = placement;
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
static void sxml_xsd_test_forget_test_set(sxml_xsd_test_catalog *catalog) {
    sxml_qt4_document_unload(&catalog->test_set_document);
    catalog->has_open_test_set = false;
    catalog->contributor = sxml_xsd_test_empty_text;
    catalog->test_set_version = sxml_xsd_test_empty_text;
    catalog->tests = NULL;
    catalog->test_count = 0;
}

static void sxml_xsd_test_close_test_set(sxml_xsd_test_catalog *catalog) {
    sxml_xsd_test_forget_test_set(catalog);
    if (catalog->test_set_arena != NULL) {
        sxml_arena_reset(catalog->test_set_arena);
    }
}

void sxml_xsd_test_catalog_finalize(sxml_xsd_test_catalog *catalog) {
    if (catalog == NULL) {
        return;
    }
    sxml_xsd_test_close_test_set(catalog);
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

sxml_status sxml_xsd_test_catalog_create(
    sxml_allocator allocator,
    const sxml_xsd_test_catalog_configuration *configuration,
    sxml_xsd_test_catalog **catalog) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_xsd_test_catalog_memory_requirements(configuration, &requirements);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = requirements.byte_count,
                                      .alignment = requirements.alignment},
            &memory);
    }
    sxml_xsd_test_catalog *result = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_catalog_initialize(
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

void sxml_xsd_test_catalog_destroy(sxml_xsd_test_catalog *catalog) {
    if (catalog == NULL) {
        return;
    }
    const sxml_allocator allocator = catalog->owning_allocator;
    sxml_xsd_test_catalog_finalize(catalog);
    sxml_allocator_release(allocator, catalog);
}

/* -------------------------------------------------------------- Helpers */

/* One suite document being read, and where problems and results go. */
typedef struct sxml_xsd_test_reader {
    const sxml_qt4_document *document;
    /* The suite path of the document: hrefs are relative to it and
       problems name it. */
    sxml_byte_view containing_file;
    sxml_arena *arena;
    /* Filled on failure; may be NULL. */
    sxml_qt4_problem *problem;
} sxml_xsd_test_reader;

static void sxml_xsd_test_report_problem(const sxml_xsd_test_reader *reader,
                                         sxml_byte_view description,
                                         sxml_byte_view subject) {
    if (reader->problem == NULL) {
        return;
    }
    *reader->problem =
        (sxml_qt4_problem){.description = description,
                           .subject = subject,
                           .location = {.source_name = reader->containing_file,
                                        .line_number = 0,
                                        .column_number = 0,
                                        .byte_offset = 0}};
}

static sxml_qt4_document_reader
sxml_xsd_test_document_reader_of(const sxml_xsd_test_catalog *catalog) {
    return (sxml_qt4_document_reader){
        .allocator = catalog->configuration.allocator,
        .file_reader = catalog->configuration.file_reader,
        .xml_parser = catalog->xml_parser,
        .item_heap = catalog->item_heap};
}

/* Reads attribute lookup of the reader's document, empty when absent. */
static sxml_status
sxml_xsd_test_read_attribute(const sxml_xsd_test_reader *reader,
                             sxml_qt4_attribute_lookup lookup,
                             sxml_byte_view *value) {
    sxml_qt4_attribute_value found = {.is_present = false,
                                      .value = sxml_xsd_test_empty_text};
    const sxml_status status =
        sxml_qt4_document_find_attribute(reader->document, lookup, &found);
    if (status == SXML_STATUS_OK) {
        *value = found.value;
    }
    return status;
}

/* The attribute local_name, in no namespace, of element. */
typedef struct sxml_xsd_test_attribute_request {
    sxml_node_index element;
    sxml_byte_view local_name;
} sxml_xsd_test_attribute_request;

static sxml_status
sxml_xsd_test_read_plain_attribute(const sxml_xsd_test_reader *reader,
                                   sxml_xsd_test_attribute_request request,
                                   sxml_byte_view *value) {
    return sxml_xsd_test_read_attribute(
        reader,
        (sxml_qt4_attribute_lookup){.element = request.element,
                                    .local_name = request.local_name,
                                    .namespace_uri = sxml_xsd_test_empty_text},
        value);
}

/*
 * Reads xlink:href of element as a suite path, resolved against the
 * reader's containing file. Returns SXML_STATUS_MALFORMED_INPUT, with the
 * problem filled, when there is none, and the tree's and the arena's
 * failures.
 */
static sxml_status sxml_xsd_test_read_href(const sxml_xsd_test_reader *reader,
                                           sxml_node_index element,
                                           sxml_byte_view *file_path) {
    sxml_byte_view reference = sxml_xsd_test_empty_text;
    sxml_status status = sxml_xsd_test_read_attribute(
        reader,
        (sxml_qt4_attribute_lookup){
            .element = element,
            .local_name = SXML_TEXT("href"),
            .namespace_uri = SXML_TEXT(SXML_XSD_TEST_XLINK_NAMESPACE_URI)},
        &reference);
    if (status == SXML_STATUS_OK && reference.byte_count == 0) {
        sxml_xsd_test_report_problem(reader,
                                     SXML_TEXT("a reference needs xlink:href"),
                                     sxml_xsd_test_empty_text);
        status = SXML_STATUS_MALFORMED_INPUT;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_qt4_suite_path_resolve(
        &(sxml_qt4_suite_path_request){.containing_file =
                                           reader->containing_file,
                                       .reference = reference,
                                       .arena = reader->arena},
        file_path);
}

/* An array of element_count elements of element_byte_count bytes in the
   arena, or NULL for none. */
typedef struct sxml_xsd_test_array_request {
    size_t element_count;
    size_t element_byte_count;
    size_t alignment;
} sxml_xsd_test_array_request;

static sxml_status sxml_xsd_test_allocate_array(
    sxml_arena *arena, sxml_xsd_test_array_request request, void **memory) {
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

/* The children of parent called local_name (empty: every child element
   of the suite's namespace), counted or, with a visit, visited. */
typedef struct sxml_xsd_test_children_walk {
    const sxml_xsd_test_reader *reader;
    sxml_node_index parent;
    sxml_byte_view local_name;
} sxml_xsd_test_children_walk;

/* Finds the first child element of walk.parent called walk.local_name. */
typedef struct sxml_xsd_test_child_lookup {
    bool is_found;
    sxml_node_index element;
} sxml_xsd_test_child_lookup;

static sxml_status
sxml_xsd_test_find_child(sxml_xsd_test_children_walk walk,
                         sxml_xsd_test_child_lookup *lookup) {
    sxml_qt4_element_cursor cursor = {.document = walk.reader->document,
                                      .has_next_node = false,
                                      .next_node = {.index = 0}};
    sxml_status status = sxml_qt4_element_cursor_start(walk.reader->document,
                                                       walk.parent, &cursor);
    lookup->is_found = false;
    while (status == SXML_STATUS_OK) {
        sxml_qt4_element_step step = {.has_element = false,
                                      .element = {.index = 0},
                                      .local_name = sxml_xsd_test_empty_text};
        status = sxml_qt4_element_cursor_next(&cursor, &step);
        if (status != SXML_STATUS_OK || !step.has_element) {
            break;
        }
        if (sxml_byte_view_equals(step.local_name, walk.local_name)) {
            lookup->is_found = true;
            lookup->element = step.element;
            break;
        }
    }
    return status;
}

/* The suite paths of the children of walk.parent called walk.local_name
   (schemaDocument or instanceDocument), read from their hrefs. */
static sxml_status
sxml_xsd_test_collect_documents(sxml_xsd_test_children_walk walk,
                                sxml_xsd_test_document_view *documents) {
    sxml_byte_view *file_paths = NULL;
    size_t file_path_count = 0;
    sxml_status status = SXML_STATUS_OK;
    /* Two passes: count, then fill an array of that size. */
    for (size_t pass_index = 0; status == SXML_STATUS_OK && pass_index < 2;
         pass_index += 1) {
        sxml_qt4_element_cursor cursor = {.document = walk.reader->document,
                                          .has_next_node = false,
                                          .next_node = {.index = 0}};
        status = sxml_qt4_element_cursor_start(walk.reader->document,
                                               walk.parent, &cursor);
        size_t filled_count = 0;
        while (status == SXML_STATUS_OK) {
            sxml_qt4_element_step step = {.has_element = false,
                                          .element = {.index = 0},
                                          .local_name =
                                              sxml_xsd_test_empty_text};
            status = sxml_qt4_element_cursor_next(&cursor, &step);
            if (status != SXML_STATUS_OK || !step.has_element) {
                break;
            }
            if (!sxml_byte_view_equals(step.local_name, walk.local_name)) {
                continue;
            }
            if (file_paths != NULL) {
                status = sxml_xsd_test_read_href(walk.reader, step.element,
                                                 &file_paths[filled_count]);
            }
            filled_count += 1;
        }
        file_path_count = filled_count;
        if (status != SXML_STATUS_OK || pass_index == 1 || filled_count == 0) {
            break;
        }
        void *memory = NULL;
        status = sxml_xsd_test_allocate_array(
            walk.reader->arena,
            (sxml_xsd_test_array_request){.element_count = filled_count,
                                          .element_byte_count =
                                              sizeof(sxml_byte_view),
                                          .alignment = alignof(sxml_byte_view)},
            &memory);
        file_paths = memory;
    }
    if (status == SXML_STATUS_OK) {
        *documents = (sxml_xsd_test_document_view){
            .file_paths = file_paths, .file_path_count = file_path_count};
    }
    return status;
}

/* Finds the document element and checks its local name. */
static sxml_status
sxml_xsd_test_find_document_element(const sxml_xsd_test_reader *reader,
                                    sxml_byte_view local_name,
                                    sxml_node_index *element) {
    sxml_xsd_test_child_lookup lookup = {.is_found = false,
                                         .element = {.index = 0}};
    const sxml_status status = sxml_xsd_test_find_child(
        (sxml_xsd_test_children_walk){
            .reader = reader, .parent = {.index = 0}, .local_name = local_name},
        &lookup);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!lookup.is_found) {
        sxml_xsd_test_report_problem(
            reader,
            SXML_TEXT("the document element is not one of the XSD test suite"),
            local_name);
        return SXML_STATUS_MALFORMED_INPUT;
    }
    *element = lookup.element;
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------------- Load */

static sxml_status sxml_xsd_test_create_parts(sxml_xsd_test_catalog *catalog) {
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
                               SXML_XSD_TEST_MAXIMUM_EXPANSION_BYTE_COUNT}},
            &catalog->xml_parser);
    }
    const sxml_arena_configuration arena_configuration = {
        .block_allocator = allocator,
        .block_byte_count = SXML_XSD_TEST_ARENA_BLOCK_BYTE_COUNT,
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
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_catalog_create(allocator, &catalog->configuration,
                                         &catalog->file_catalog);
    }
    return status;
}

/* The name of a test set (xsd_catalog.h, "Names"): of
   "saxonMeta/Assert.testSet", "saxon-Assert". */
static sxml_status sxml_xsd_test_name_test_set(sxml_arena *arena,
                                               sxml_byte_view file_path,
                                               sxml_byte_view *name) {
    size_t file_start = file_path.byte_count;
    while (file_start > 0 && file_path.bytes[file_start - 1] != '/') {
        file_start -= 1;
    }
    size_t file_end = file_path.byte_count;
    while (file_end > file_start && file_path.bytes[file_end - 1] != '.') {
        file_end -= 1;
    }
    if (file_end == file_start) {
        /* No extension. */
        file_end = file_path.byte_count;
    } else {
        file_end -= 1;
    }
    const sxml_byte_view file_name = {.bytes = file_path.bytes + file_start,
                                      .byte_count = file_end - file_start};
    if (file_start == 0) {
        *name = file_name;
        return SXML_STATUS_OK;
    }
    size_t directory_end = file_start - 1;
    size_t directory_start = directory_end;
    while (directory_start > 0 && file_path.bytes[directory_start - 1] != '/') {
        directory_start -= 1;
    }
    sxml_byte_view directory = {.bytes = file_path.bytes + directory_start,
                                .byte_count = directory_end - directory_start};
    const sxml_byte_view suffix = SXML_TEXT("Meta");
    if (directory.byte_count > suffix.byte_count &&
        memcmp(directory.bytes + directory.byte_count - suffix.byte_count,
               suffix.bytes, suffix.byte_count) == 0) {
        directory.byte_count -= suffix.byte_count;
    }
    sxml_byte_view prefix = sxml_xsd_test_empty_text;
    sxml_status status = sxml_qt4_concatenate(
        arena,
        (sxml_qt4_concatenation){.first = directory, .second = SXML_TEXT("-")},
        &prefix);
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_concatenate(
            arena,
            (sxml_qt4_concatenation){.first = prefix, .second = file_name},
            name);
    }
    return status;
}

/* Reads the testSetRef entries of suite.xml. */
static sxml_status
sxml_xsd_test_read_test_set_entries(sxml_xsd_test_catalog *catalog,
                                    const sxml_xsd_test_reader *reader,
                                    sxml_node_index suite_element) {
    sxml_xsd_test_document_view file_paths = {.file_paths = NULL,
                                              .file_path_count = 0};
    sxml_status status = sxml_xsd_test_collect_documents(
        (sxml_xsd_test_children_walk){.reader = reader,
                                      .parent = suite_element,
                                      .local_name = SXML_TEXT("testSetRef")},
        &file_paths);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_allocate_array(
            catalog->catalog_arena,
            (sxml_xsd_test_array_request){
                .element_count = file_paths.file_path_count,
                .element_byte_count = sizeof(sxml_qt4_test_set_entry),
                .alignment = alignof(sxml_qt4_test_set_entry)},
            &memory);
    }
    sxml_qt4_test_set_entry *entries = memory;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entries != NULL &&
                                 entry_index < file_paths.file_path_count;
         entry_index += 1) {
        entries[entry_index].file_path = file_paths.file_paths[entry_index];
        status = sxml_xsd_test_name_test_set(catalog->catalog_arena,
                                             file_paths.file_paths[entry_index],
                                             &entries[entry_index].name);
    }
    if (status == SXML_STATUS_OK) {
        catalog->test_set_entries = entries;
        catalog->test_set_entry_count = file_paths.file_path_count;
    }
    return status;
}

sxml_status sxml_xsd_test_catalog_load(sxml_xsd_test_catalog *catalog,
                                       sxml_qt4_problem *problem) {
    if (catalog->has_started_loading) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    catalog->has_started_loading = true;
    sxml_status status = sxml_xsd_test_create_parts(catalog);
    sxml_qt4_document suite_document = {.arena_tree = NULL,
                                        .tree = NULL,
                                        .name_pool = NULL,
                                        .catalog_namespace = {.index = 0}};
    if (status == SXML_STATUS_OK) {
        const sxml_qt4_document_reader document_reader =
            sxml_xsd_test_document_reader_of(catalog);
        status = sxml_qt4_document_load(
            &document_reader,
            &(sxml_qt4_document_request){
                .file_path = catalog->configuration.catalog_path,
                .suite_path = catalog->catalog_suite_path,
                .problem = problem,
                .catalog_namespace_uri =
                    SXML_TEXT(SXML_XSD_TEST_SUITE_NAMESPACE_URI)},
            &suite_document);
    }
    const sxml_xsd_test_reader reader = {.document = &suite_document,
                                         .containing_file =
                                             catalog->catalog_suite_path,
                                         .arena = catalog->catalog_arena,
                                         .problem = problem};
    sxml_node_index suite_element = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_find_document_element(
            &reader, SXML_TEXT("testSuite"), &suite_element);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_read_test_set_entries(catalog, &reader,
                                                     suite_element);
    }
    /* The entries hold only arena memory: suite.xml is not needed again. */
    sxml_qt4_document_unload(&suite_document);
    if (status != SXML_STATUS_OK) {
        catalog->test_set_entry_count = 0;
    }
    return status;
}

/* ------------------------------------------------------------ Test sets */

size_t
sxml_xsd_test_catalog_test_set_count(const sxml_xsd_test_catalog *catalog) {
    return catalog->test_set_entry_count;
}

sxml_status
sxml_xsd_test_catalog_get_test_set_entry(const sxml_xsd_test_catalog *catalog,
                                         size_t test_set_index,
                                         sxml_qt4_test_set_entry *entry) {
    if (test_set_index >= catalog->test_set_entry_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    *entry = catalog->test_set_entries[test_set_index];
    return SXML_STATUS_OK;
}

bool sxml_xsd_test_catalog_find_test_set(const sxml_xsd_test_catalog *catalog,
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

/* Reads one test of a group into *entry. */
typedef struct sxml_xsd_test_entry_request {
    const sxml_xsd_test_reader *reader;
    sxml_node_index group;
    sxml_byte_view group_name;
    sxml_qt4_element_step step;
} sxml_xsd_test_entry_request;

static sxml_status
sxml_xsd_test_read_entry(const sxml_xsd_test_entry_request *request,
                         sxml_xsd_test_entry *entry) {
    sxml_byte_view test_name = sxml_xsd_test_empty_text;
    sxml_status status = sxml_xsd_test_read_plain_attribute(
        request->reader,
        (sxml_xsd_test_attribute_request){.element = request->step.element,
                                          .local_name = SXML_TEXT("name")},
        &test_name);
    if (status == SXML_STATUS_OK && test_name.byte_count == 0) {
        sxml_xsd_test_report_problem(request->reader,
                                     SXML_TEXT("a test needs a name"),
                                     request->group_name);
        status = SXML_STATUS_MALFORMED_INPUT;
    }
    sxml_byte_view prefix = sxml_xsd_test_empty_text;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_concatenate(
            request->reader->arena,
            (sxml_qt4_concatenation){.first = request->group_name,
                                     .second = SXML_TEXT("/")},
            &prefix);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_concatenate(
            request->reader->arena,
            (sxml_qt4_concatenation){.first = prefix, .second = test_name},
            &entry->name);
    }
    entry->group = request->group;
    entry->test = request->step.element;
    entry->kind =
        sxml_byte_view_equals(request->step.local_name, SXML_TEXT("schemaTest"))
            ? SXML_XSD_TEST_KIND_SCHEMA
            : SXML_XSD_TEST_KIND_INSTANCE;
    return status;
}

/* Counts, or with entries fills, the tests of one test group. */
typedef struct sxml_xsd_test_group_walk {
    const sxml_xsd_test_reader *reader;
    sxml_node_index group;
    /* NULL when counting. */
    sxml_xsd_test_entry *entries;
    /* In: the entries filled so far. Out: with this group's. */
    size_t *filled_count;
} sxml_xsd_test_group_walk;

static sxml_status
sxml_xsd_test_walk_group(const sxml_xsd_test_group_walk *walk) {
    sxml_byte_view group_name = sxml_xsd_test_empty_text;
    sxml_status status = sxml_xsd_test_read_plain_attribute(
        walk->reader,
        (sxml_xsd_test_attribute_request){.element = walk->group,
                                          .local_name = SXML_TEXT("name")},
        &group_name);
    if (status == SXML_STATUS_OK && group_name.byte_count == 0) {
        sxml_xsd_test_report_problem(walk->reader,
                                     SXML_TEXT("a test group needs a name"),
                                     sxml_xsd_test_empty_text);
        status = SXML_STATUS_MALFORMED_INPUT;
    }
    sxml_qt4_element_cursor cursor = {.document = walk->reader->document,
                                      .has_next_node = false,
                                      .next_node = {.index = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_element_cursor_start(walk->reader->document,
                                               walk->group, &cursor);
    }
    while (status == SXML_STATUS_OK) {
        sxml_qt4_element_step step = {.has_element = false,
                                      .element = {.index = 0},
                                      .local_name = sxml_xsd_test_empty_text};
        status = sxml_qt4_element_cursor_next(&cursor, &step);
        if (status != SXML_STATUS_OK || !step.has_element) {
            break;
        }
        if (!sxml_byte_view_equals(step.local_name, SXML_TEXT("schemaTest")) &&
            !sxml_byte_view_equals(step.local_name,
                                   SXML_TEXT("instanceTest"))) {
            continue;
        }
        if (walk->entries != NULL) {
            status = sxml_xsd_test_read_entry(
                &(sxml_xsd_test_entry_request){.reader = walk->reader,
                                               .group = walk->group,
                                               .group_name = group_name,
                                               .step = step},
                &walk->entries[*walk->filled_count]);
        }
        *walk->filled_count += 1;
    }
    return status;
}

/* Collects every test of the test set, group by group: counted, then
   read. */
static sxml_status
sxml_xsd_test_collect_tests(sxml_xsd_test_catalog *catalog,
                            const sxml_xsd_test_reader *reader,
                            sxml_node_index test_set_element) {
    sxml_xsd_test_entry *entries = NULL;
    size_t entry_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t pass_index = 0; status == SXML_STATUS_OK && pass_index < 2;
         pass_index += 1) {
        sxml_qt4_element_cursor cursor = {.document = reader->document,
                                          .has_next_node = false,
                                          .next_node = {.index = 0}};
        status = sxml_qt4_element_cursor_start(reader->document,
                                               test_set_element, &cursor);
        size_t filled_count = 0;
        while (status == SXML_STATUS_OK) {
            sxml_qt4_element_step step = {.has_element = false,
                                          .element = {.index = 0},
                                          .local_name =
                                              sxml_xsd_test_empty_text};
            status = sxml_qt4_element_cursor_next(&cursor, &step);
            if (status != SXML_STATUS_OK || !step.has_element) {
                break;
            }
            if (sxml_byte_view_equals(step.local_name,
                                      SXML_TEXT("testGroup"))) {
                status = sxml_xsd_test_walk_group(
                    &(sxml_xsd_test_group_walk){.reader = reader,
                                                .group = step.element,
                                                .entries = entries,
                                                .filled_count = &filled_count});
            }
        }
        entry_count = filled_count;
        if (status != SXML_STATUS_OK || pass_index == 1 || filled_count == 0) {
            break;
        }
        void *memory = NULL;
        status = sxml_xsd_test_allocate_array(
            reader->arena,
            (sxml_xsd_test_array_request){
                .element_count = filled_count,
                .element_byte_count = sizeof(sxml_xsd_test_entry),
                .alignment = alignof(sxml_xsd_test_entry)},
            &memory);
        entries = memory;
    }
    if (status == SXML_STATUS_OK) {
        catalog->tests = entries;
        catalog->test_count = entry_count;
    }
    return status;
}

static sxml_xsd_test_reader
sxml_xsd_test_reader_of_test_set(sxml_xsd_test_catalog *catalog,
                                 sxml_qt4_problem *problem) {
    return (sxml_xsd_test_reader){
        .document = &catalog->test_set_document,
        .containing_file =
            catalog->test_set_entries[catalog->open_test_set_index].file_path,
        .arena = catalog->test_set_arena,
        .problem = problem};
}

sxml_status
sxml_xsd_test_catalog_open_test_set(sxml_xsd_test_catalog *catalog,
                                    const sxml_qt4_test_set_request *request) {
    sxml_xsd_test_close_test_set(catalog);
    if (request->test_set_index >= catalog->test_set_entry_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    const sxml_qt4_test_set_entry entry =
        catalog->test_set_entries[request->test_set_index];
    sxml_byte_view file_path = sxml_xsd_test_empty_text;
    sxml_status status = sxml_qt4_concatenate(
        catalog->test_set_arena,
        (sxml_qt4_concatenation){.first = catalog->catalog_directory,
                                 .second = entry.file_path},
        &file_path);
    if (status == SXML_STATUS_OK) {
        const sxml_qt4_document_reader document_reader =
            sxml_xsd_test_document_reader_of(catalog);
        status =
            sxml_qt4_document_load(&document_reader,
                                   &(sxml_qt4_document_request){
                                       .file_path = file_path,
                                       .suite_path = entry.file_path,
                                       .problem = request->problem,
                                       .catalog_namespace_uri = SXML_TEXT(
                                           SXML_XSD_TEST_SUITE_NAMESPACE_URI)},
                                   &catalog->test_set_document);
    }
    catalog->open_test_set_index = request->test_set_index;
    const sxml_xsd_test_reader reader =
        sxml_xsd_test_reader_of_test_set(catalog, request->problem);
    sxml_node_index test_set_element = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_find_document_element(
            &reader, SXML_TEXT("testSet"), &test_set_element);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_read_plain_attribute(
            &reader,
            (sxml_xsd_test_attribute_request){.element = test_set_element,
                                              .local_name =
                                                  SXML_TEXT("contributor")},
            &catalog->contributor);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_read_plain_attribute(
            &reader,
            (sxml_xsd_test_attribute_request){.element = test_set_element,
                                              .local_name =
                                                  SXML_TEXT("version")},
            &catalog->test_set_version);
    }
    if (status == SXML_STATUS_OK) {
        status =
            sxml_xsd_test_collect_tests(catalog, &reader, test_set_element);
    }
    if (status != SXML_STATUS_OK) {
        sxml_xsd_test_forget_test_set(catalog);
        return status;
    }
    catalog->has_open_test_set = true;
    return SXML_STATUS_OK;
}

bool sxml_xsd_test_catalog_get_open_test_set(
    const sxml_xsd_test_catalog *catalog, size_t *test_set_index) {
    if (!catalog->has_open_test_set) {
        return false;
    }
    *test_set_index = catalog->open_test_set_index;
    return true;
}

size_t
sxml_xsd_test_catalog_test_case_count(const sxml_xsd_test_catalog *catalog) {
    return catalog->test_count;
}

bool sxml_xsd_test_catalog_find_test_case(const sxml_xsd_test_catalog *catalog,
                                          sxml_byte_view name,
                                          size_t *test_case_index) {
    for (size_t test_index = 0; test_index < catalog->test_count;
         test_index += 1) {
        if (sxml_byte_view_equals(catalog->tests[test_index].name, name)) {
            *test_case_index = test_index;
            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------- Test cases */

/* The expected elements of a test, in document order: counted, then
   read. */
static sxml_status
sxml_xsd_test_collect_expectations(const sxml_xsd_test_reader *reader,
                                   sxml_node_index test,
                                   sxml_xsd_test_expectation_view *view) {
    sxml_xsd_test_expectation *expectations = NULL;
    size_t expectation_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t pass_index = 0; status == SXML_STATUS_OK && pass_index < 2;
         pass_index += 1) {
        sxml_qt4_element_cursor cursor = {.document = reader->document,
                                          .has_next_node = false,
                                          .next_node = {.index = 0}};
        status = sxml_qt4_element_cursor_start(reader->document, test, &cursor);
        size_t filled_count = 0;
        while (status == SXML_STATUS_OK) {
            sxml_qt4_element_step step = {.has_element = false,
                                          .element = {.index = 0},
                                          .local_name =
                                              sxml_xsd_test_empty_text};
            status = sxml_qt4_element_cursor_next(&cursor, &step);
            if (status != SXML_STATUS_OK || !step.has_element) {
                break;
            }
            if (!sxml_byte_view_equals(step.local_name,
                                       SXML_TEXT("expected"))) {
                continue;
            }
            if (expectations != NULL) {
                sxml_xsd_test_expectation *expectation =
                    &expectations[filled_count];
                status = sxml_xsd_test_read_plain_attribute(
                    reader,
                    (sxml_xsd_test_attribute_request){
                        .element = step.element,
                        .local_name = SXML_TEXT("validity")},
                    &expectation->validity);
                if (status == SXML_STATUS_OK) {
                    status = sxml_xsd_test_read_plain_attribute(
                        reader,
                        (sxml_xsd_test_attribute_request){
                            .element = step.element,
                            .local_name = SXML_TEXT("version")},
                        &expectation->version);
                }
            }
            filled_count += 1;
        }
        expectation_count = filled_count;
        if (status != SXML_STATUS_OK || pass_index == 1 || filled_count == 0) {
            break;
        }
        void *memory = NULL;
        status = sxml_xsd_test_allocate_array(
            reader->arena,
            (sxml_xsd_test_array_request){
                .element_count = filled_count,
                .element_byte_count = sizeof(sxml_xsd_test_expectation),
                .alignment = alignof(sxml_xsd_test_expectation)},
            &memory);
        expectations = memory;
    }
    if (status == SXML_STATUS_OK) {
        *view = (sxml_xsd_test_expectation_view){.expectations = expectations,
                                                 .expectation_count =
                                                     expectation_count};
    }
    return status;
}

/* current/@status of a test, empty without <current>. */
static sxml_status sxml_xsd_test_read_status(const sxml_xsd_test_reader *reader,
                                             sxml_node_index test,
                                             sxml_byte_view *status_text) {
    sxml_xsd_test_child_lookup lookup = {.is_found = false,
                                         .element = {.index = 0}};
    const sxml_status status = sxml_xsd_test_find_child(
        (sxml_xsd_test_children_walk){.reader = reader,
                                      .parent = test,
                                      .local_name = SXML_TEXT("current")},
        &lookup);
    if (status != SXML_STATUS_OK || !lookup.is_found) {
        return status;
    }
    return sxml_xsd_test_read_plain_attribute(
        reader,
        (sxml_xsd_test_attribute_request){.element = lookup.element,
                                          .local_name = SXML_TEXT("status")},
        status_text);
}

/* The schema an instance test is validated against: the documents of its
   group's schema test, if any. */
static sxml_status
sxml_xsd_test_read_group_schema(const sxml_xsd_test_reader *reader,
                                sxml_node_index group,
                                sxml_xsd_test_case *test_case) {
    sxml_xsd_test_child_lookup lookup = {.is_found = false,
                                         .element = {.index = 0}};
    const sxml_status status = sxml_xsd_test_find_child(
        (sxml_xsd_test_children_walk){.reader = reader,
                                      .parent = group,
                                      .local_name = SXML_TEXT("schemaTest")},
        &lookup);
    test_case->has_schema_test = lookup.is_found;
    if (status != SXML_STATUS_OK || !lookup.is_found) {
        return status;
    }
    return sxml_xsd_test_collect_documents(
        (sxml_xsd_test_children_walk){.reader = reader,
                                      .parent = lookup.element,
                                      .local_name =
                                          SXML_TEXT("schemaDocument")},
        &test_case->schema_documents);
}

sxml_status
sxml_xsd_test_catalog_read_test_case(sxml_xsd_test_catalog *catalog,
                                     const sxml_qt4_test_case_request *request,
                                     sxml_xsd_test_case *test_case) {
    if (!catalog->has_open_test_set) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (request->test_case_index >= catalog->test_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    const sxml_xsd_test_entry *entry =
        &catalog->tests[request->test_case_index];
    const sxml_qt4_test_set_entry *test_set =
        &catalog->test_set_entries[catalog->open_test_set_index];
    const sxml_xsd_test_reader reader =
        sxml_xsd_test_reader_of_test_set(catalog, request->problem);
    const sxml_xsd_test_document_view no_documents = {.file_paths = NULL,
                                                      .file_path_count = 0};
    sxml_xsd_test_case result = {
        .test_set_name = test_set->name,
        .test_set_file_path = test_set->file_path,
        .contributor = catalog->contributor,
        .name = entry->name,
        .group_name = sxml_xsd_test_empty_text,
        .kind = entry->kind,
        .test_set_version = catalog->test_set_version,
        .group_version = sxml_xsd_test_empty_text,
        .test_version = sxml_xsd_test_empty_text,
        .status = sxml_xsd_test_empty_text,
        .expectations = {.expectations = NULL, .expectation_count = 0},
        .documents = no_documents,
        .has_schema_test = entry->kind == SXML_XSD_TEST_KIND_SCHEMA,
        .schema_documents = no_documents};
    sxml_status status = sxml_xsd_test_read_plain_attribute(
        &reader,
        (sxml_xsd_test_attribute_request){.element = entry->group,
                                          .local_name = SXML_TEXT("name")},
        &result.group_name);
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_read_plain_attribute(
            &reader,
            (sxml_xsd_test_attribute_request){
                .element = entry->group, .local_name = SXML_TEXT("version")},
            &result.group_version);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_read_plain_attribute(
            &reader,
            (sxml_xsd_test_attribute_request){
                .element = entry->test, .local_name = SXML_TEXT("version")},
            &result.test_version);
    }
    if (status == SXML_STATUS_OK) {
        status =
            sxml_xsd_test_read_status(&reader, entry->test, &result.status);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_collect_expectations(&reader, entry->test,
                                                    &result.expectations);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_collect_documents(
            (sxml_xsd_test_children_walk){
                .reader = &reader,
                .parent = entry->test,
                .local_name = entry->kind == SXML_XSD_TEST_KIND_SCHEMA
                                  ? SXML_TEXT("schemaDocument")
                                  : SXML_TEXT("instanceDocument")},
            &result.documents);
    }
    if (status == SXML_STATUS_OK && result.documents.file_path_count == 0) {
        sxml_xsd_test_report_problem(
            &reader, SXML_TEXT("a test needs a document"), entry->name);
        status = SXML_STATUS_MALFORMED_INPUT;
    }
    if (status == SXML_STATUS_OK) {
        if (entry->kind == SXML_XSD_TEST_KIND_SCHEMA) {
            result.schema_documents = result.documents;
        } else {
            status =
                sxml_xsd_test_read_group_schema(&reader, entry->group, &result);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *test_case = result;
    return SXML_STATUS_OK;
}

sxml_status
sxml_xsd_test_catalog_read_file(sxml_xsd_test_catalog *catalog,
                                const sxml_qt4_file_request *request,
                                sxml_byte_view *contents) {
    if (catalog->file_catalog == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_qt4_catalog_read_file(catalog->file_catalog, request, contents);
}

/* ------------------------------------------------------- Test catalog */

static size_t sxml_xsd_test_get_test_set_count(const void *state) {
    return sxml_xsd_test_catalog_test_set_count(state);
}

static sxml_byte_view sxml_xsd_test_get_test_set_name(const void *state,
                                                      size_t test_set_index) {
    sxml_qt4_test_set_entry entry = {.name = sxml_xsd_test_empty_text,
                                     .file_path = sxml_xsd_test_empty_text};
    if (sxml_xsd_test_catalog_get_test_set_entry(state, test_set_index,
                                                 &entry) != SXML_STATUS_OK) {
        return sxml_xsd_test_empty_text;
    }
    return entry.name;
}

static bool sxml_xsd_test_find_test_set(const void *state, sxml_byte_view name,
                                        size_t *test_set_index) {
    return sxml_xsd_test_catalog_find_test_set(state, name, test_set_index);
}

static sxml_status
sxml_xsd_test_open_test_set(void *state,
                            const sxml_qt4_test_set_request *request) {
    return sxml_xsd_test_catalog_open_test_set(state, request);
}

static bool sxml_xsd_test_get_open_test_set(const void *state,
                                            size_t *test_set_index) {
    return sxml_xsd_test_catalog_get_open_test_set(state, test_set_index);
}

static size_t sxml_xsd_test_get_test_case_count(const void *state) {
    return sxml_xsd_test_catalog_test_case_count(state);
}

static bool sxml_xsd_test_find_test_case(const void *state, sxml_byte_view name,
                                         size_t *test_case_index) {
    return sxml_xsd_test_catalog_find_test_case(state, name, test_case_index);
}

static const sxml_qt4_test_catalog_operations
    sxml_xsd_test_test_catalog_operations = {
        .get_test_set_count = sxml_xsd_test_get_test_set_count,
        .get_test_set_name = sxml_xsd_test_get_test_set_name,
        .find_test_set = sxml_xsd_test_find_test_set,
        .open_test_set = sxml_xsd_test_open_test_set,
        .get_open_test_set = sxml_xsd_test_get_open_test_set,
        .get_test_case_count = sxml_xsd_test_get_test_case_count,
        .find_test_case = sxml_xsd_test_find_test_case,
};

sxml_qt4_test_catalog
sxml_xsd_test_catalog_as_test_catalog(sxml_xsd_test_catalog *catalog) {
    return (sxml_qt4_test_catalog){
        .state = catalog, .operations = &sxml_xsd_test_test_catalog_operations};
}

sxml_xsd_test_catalog *
sxml_xsd_test_selection_catalog(const sxml_qt4_selection *selection) {
    return selection->catalog.state;
}
