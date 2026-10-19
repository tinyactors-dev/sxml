/*
 * The W3C XML conformance suite through the XML parser (packet 1.14,
 * docs/DECISIONS.md D13, D81): the standalone tests of XML 1.0 Fifth
 * Edition, selected as grammars/xml/test/xml-conformance.py selects them,
 * and apart from them the tests of XML 1.1 and Namespaces in XML 1.1
 * (packet 7.13): the ibm and eduni xml-1.1 catalogs, eduni's namespace 1.1
 * tests and the one 1.1 erratum test, whatever entities they use.
 *
 * Golden file: tests/memory_test.c (layout, naming).
 *
 * The suite is read from the source tree, third_party/xmlconf, found from
 * this file's own path. Where that path cannot be opened (a build moved
 * away from its sources) the test says so and passes. Under WASI,
 * tools/run-wasi.mjs preopens the source tree, so the suite runs (D93).
 *
 * Every valid and invalid test (both well-formed) must build an arena
 * tree; every not-wf test must be refused with MALFORMED_INPUT,
 * INVALID_UTF8 or UNSUPPORTED_FEATURE. Tests marked NAMESPACE="no" are
 * well-formed XML 1.0 but not namespace-well-formed, so a namespace-aware
 * parser rightly refuses some of them; they are counted apart and accepted
 * either way. An "error" test may be refused or not. A 1.1 test that
 * uses external entities (ENTITIES other than "none", or an external DTD
 * that the catalog does not mention) can fail only
 * because the parser does not read them (D81); such failures are counted
 * and printed, and do not fail the run. The catalogs themselves are read with the parser, through a
 * tree builder that collects the attributes of TEST elements.
 */
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"
#include "test_runner.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SXML_TEST_PATH_BYTE_COUNT = 4096,
    SXML_TEST_FIELD_BYTE_COUNT = 256,
};

/* ------------------------------------------------------------ Files */

/* Reads a whole file through the default allocator; NULL bytes when it
   cannot be read. */
static sxml_byte_view read_file(const char *path) {
    sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return contents;
    }
    size_t capacity = 1 << 16;
    size_t byte_count = 0;
    void *memory = NULL;
    if (sxml_allocator_allocate(
            sxml_default_allocator(),
            (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
            &memory) != SXML_STATUS_OK) {
        (void)fclose(file);
        return contents;
    }
    uint8_t *bytes = memory;
    for (;;) {
        if (byte_count == capacity) {
            void *grown = NULL;
            if (sxml_allocator_allocate(
                    sxml_default_allocator(),
                    (sxml_allocation_request){.byte_count = capacity * 2,
                                              .alignment = 1},
                    &grown) != SXML_STATUS_OK) {
                sxml_allocator_release(sxml_default_allocator(), bytes);
                (void)fclose(file);
                return contents;
            }
            memcpy(grown, bytes, byte_count);
            sxml_allocator_release(sxml_default_allocator(), bytes);
            bytes = grown;
            capacity *= 2;
        }
        const size_t read_count =
            fread(bytes + byte_count, 1, capacity - byte_count, file);
        byte_count += read_count;
        if (read_count == 0) {
            break;
        }
    }
    (void)fclose(file);
    contents.bytes = bytes;
    contents.byte_count = byte_count;
    return contents;
}

static void release_file(sxml_byte_view contents) {
    if (contents.bytes != NULL) {
        sxml_allocator_release(sxml_default_allocator(),
                               (void *)contents.bytes);
    }
}

/* Copies view into a NUL-terminated field, cutting it to fit. */
static void copy_field(char *field, sxml_byte_view view) {
    size_t byte_count = view.byte_count;
    if (byte_count >= SXML_TEST_FIELD_BYTE_COUNT) {
        byte_count = SXML_TEST_FIELD_BYTE_COUNT - 1;
    }
    if (byte_count != 0) {
        memcpy(field, view.bytes, byte_count);
    }
    field[byte_count] = '\0';
}

/* The directory part of path, with its final slash. */
static void directory_of(const char *path, char *directory) {
    const char *slash = strrchr(path, '/');
    const size_t byte_count = slash == NULL ? 0 : (size_t)(slash - path) + 1;
    memcpy(directory, path, byte_count);
    directory[byte_count] = '\0';
}

/* Writes first then the first second_byte_count bytes of second into
   path (SXML_TEST_PATH_BYTE_COUNT bytes), cutting what does not fit. */
typedef struct path_parts {
    const char *first;
    const char *second;
    size_t second_byte_count;
} path_parts;

static void join_path(char *path, path_parts parts) {
    size_t first_byte_count = strlen(parts.first);
    if (first_byte_count >= SXML_TEST_PATH_BYTE_COUNT) {
        first_byte_count = SXML_TEST_PATH_BYTE_COUNT - 1;
    }
    size_t second_byte_count = parts.second_byte_count;
    if (second_byte_count > SXML_TEST_PATH_BYTE_COUNT - 1 - first_byte_count) {
        second_byte_count = SXML_TEST_PATH_BYTE_COUNT - 1 - first_byte_count;
    }
    memmove(path, parts.first, first_byte_count);
    memcpy(path + first_byte_count, parts.second, second_byte_count);
    path[first_byte_count + second_byte_count] = '\0';
}

static path_parts parts_of(const char *first, const char *second) {
    return (path_parts){
        .first = first, .second = second, .second_byte_count = strlen(second)};
}

/* --------------------------------------------------------- Test cases */

/* The attributes of one TEST element; empty fields are absent. */
typedef struct conformance_case {
    char identifier[SXML_TEST_FIELD_BYTE_COUNT];
    char type[SXML_TEST_FIELD_BYTE_COUNT];
    char entities[SXML_TEST_FIELD_BYTE_COUNT];
    char recommendation[SXML_TEST_FIELD_BYTE_COUNT];
    char version[SXML_TEST_FIELD_BYTE_COUNT];
    char edition[SXML_TEST_FIELD_BYTE_COUNT];
    char namespace_flag[SXML_TEST_FIELD_BYTE_COUNT];
    char uri[SXML_TEST_FIELD_BYTE_COUNT];
} conformance_case;

/* Outcome counts by test type. */
typedef struct conformance_counts {
    uint32_t valid_passed;
    uint32_t valid_total;
    uint32_t invalid_passed;
    uint32_t invalid_total;
    uint32_t not_well_formed_passed;
    uint32_t not_well_formed_total;
    uint32_t namespace_free_total;
    uint32_t namespace_free_refused;
    uint32_t error_total;
    /* Failures of tests that use external entities (XML 1.1 only). */
    uint32_t external_entity_failure_count;
    uint32_t failure_count;
} conformance_counts;

/* What the suite run shares: the objects and the counts. */
typedef struct conformance_run {
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
    sxml_xml_parser *document_parser;
    sxml_xml_parser *catalog_parser;
    char catalog_directory[SXML_TEST_PATH_BYTE_COUNT];
    conformance_case current_case;
    bool is_in_case;
    uint32_t element_depth;
    uint32_t case_depth;
    conformance_counts counts;
    conformance_counts version_1_1_counts;
} conformance_run;

static bool field_is(const char *field, const char *expected) {
    return strcmp(field, expected) == 0;
}

/* The selection of grammars/xml/test/xml-conformance.py. */
static bool case_is_selected(const conformance_case *test_case) {
    if (test_case->entities[0] != '\0' &&
        !field_is(test_case->entities, "none")) {
        return false;
    }
    const char *recommendation = test_case->recommendation[0] == '\0'
                                     ? "XML1.0"
                                     : test_case->recommendation;
    if (!field_is(recommendation, "XML1.0") &&
        !field_is(recommendation, "NS1.0") &&
        !field_is(recommendation, "XML1.0-errata2e") &&
        !field_is(recommendation, "XML1.0-errata3e") &&
        !field_is(recommendation, "XML1.0-errata4e")) {
        return false;
    }
    if (test_case->version[0] != '\0' && !field_is(test_case->version, "1.0") &&
        strstr(test_case->version, "1.0") == NULL) {
        return false;
    }
    if (test_case->edition[0] != '\0' &&
        strchr(test_case->edition, '5') == NULL) {
        return false;
    }
    return true;
}

/* The tests of XML 1.1 and Namespaces in XML 1.1 (packet 7.13), of the
   Fifth Edition of XML 1.0 when it matters. */
static bool case_is_version_1_1(const conformance_case *test_case) {
    if (test_case->edition[0] != '\0' &&
        strchr(test_case->edition, '5') == NULL) {
        return false;
    }
    return field_is(test_case->recommendation, "XML1.1") ||
           field_is(test_case->recommendation, "NS1.1") ||
           strstr(test_case->version, "1.1") != NULL;
}

/* True when document names an external entity or DTD, which the parser
   does not read (D81); some catalogs say ENTITIES="none" for them. */
static bool document_names_external_entity(sxml_byte_view document) {
    static const char *const keywords[] = {"SYSTEM", "PUBLIC"};
    for (size_t keyword_index = 0; keyword_index < 2; keyword_index++) {
        const size_t keyword_byte_count = strlen(keywords[keyword_index]);
        for (size_t byte_index = 0;
             byte_index + keyword_byte_count <= document.byte_count;
             byte_index++) {
            if (memcmp(document.bytes + byte_index, keywords[keyword_index],
                       keyword_byte_count) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* Parses document into a new arena tree and returns the parser's status. */
static sxml_status build_document(const conformance_run *run,
                                  sxml_byte_view document) {
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        sxml_default_allocator(),
        &(sxml_arena_tree_configuration){
            .allocator = sxml_default_allocator(),
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = run->item_heap,
            .maximum_node_count = 0,
            .maximum_depth = 0,
            .maximum_text_byte_count = 0,
            .keeps_locations = true,
            .duplicate_attribute_policy =
                SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = false,
            .error_record = NULL},
        &arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_diagnostic diagnostic = {
        .description = {.bytes = NULL, .byte_count = 0},
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0}};
    status = sxml_xml_parser_parse(
        run->document_parser,
        &(sxml_xml_parse_request){
            .input = document,
            .source_name = SXML_BYTE_VIEW_FROM_LITERAL("test.xml"),
            .document_uri = {.bytes = NULL, .byte_count = 0},
            .base_uri = {.bytes = NULL, .byte_count = 0},
            .encoding_name = {.bytes = NULL, .byte_count = 0},
            .diagnostic = &diagnostic},
        sxml_arena_tree_as_tree_builder(arena_tree));
    if (status == SXML_STATUS_OK) {
        sxml_tree *tree = NULL;
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK &&
        getenv("SXML_XML_CONFORMANCE_VERBOSE") != NULL) {
        printf("    %u:%u %.*s\n", diagnostic.location.line_number,
               diagnostic.location.column_number,
               (int)diagnostic.description.byte_count,
               (const char *)diagnostic.description.bytes);
    }
    sxml_arena_tree_destroy(arena_tree);
    return status;
}

static bool status_refuses(sxml_status status) {
    return status == SXML_STATUS_MALFORMED_INPUT ||
           status == SXML_STATUS_INVALID_UTF8 ||
           status == SXML_STATUS_UNSUPPORTED_FEATURE;
}

/* Runs one selected test case. */
static void run_case(conformance_run *run, const conformance_case *test_case) {
    char path[SXML_TEST_PATH_BYTE_COUNT];
    join_path(path, parts_of(run->catalog_directory, test_case->uri));
    const sxml_byte_view document = read_file(path);
    if (document.bytes == NULL) {
        printf("cannot read %s\n", path);
        run->counts.failure_count += 1;
        return;
    }
    const sxml_status status = build_document(run, document);
    const bool uses_external_entities =
        (test_case->entities[0] != '\0' &&
         !field_is(test_case->entities, "none")) ||
        document_names_external_entity(document);
    release_file(document);
    const bool is_namespace_free = field_is(test_case->namespace_flag, "no");
    const bool is_version_1_1 = !case_is_selected(test_case);
    conformance_counts *counts =
        is_version_1_1 ? &run->version_1_1_counts : &run->counts;
    bool passed = false;
    if (field_is(test_case->type, "valid") ||
        field_is(test_case->type, "invalid")) {
        const bool is_valid = field_is(test_case->type, "valid");
        passed = status == SXML_STATUS_OK ||
                 (is_namespace_free && status_refuses(status));
        counts->valid_total += is_valid ? 1 : 0;
        counts->invalid_total += is_valid ? 0 : 1;
        counts->valid_passed += is_valid && passed ? 1 : 0;
        counts->invalid_passed += !is_valid && passed ? 1 : 0;
        if (is_namespace_free) {
            counts->namespace_free_total += 1;
            counts->namespace_free_refused += status_refuses(status) ? 1 : 0;
        }
    } else if (field_is(test_case->type, "not-wf")) {
        passed = status_refuses(status);
        counts->not_well_formed_total += 1;
        counts->not_well_formed_passed += passed ? 1 : 0;
    } else if (field_is(test_case->type, "error")) {
        counts->error_total += 1;
        passed = true;
    } else {
        return;
    }
    if (!passed && is_version_1_1 && uses_external_entities) {
        counts->external_entity_failure_count += 1;
        printf("FAILED (external entities not read) %s (%s, %s): %s\n",
               test_case->identifier, test_case->type, path,
               (const char *)sxml_status_name(status).bytes);
    } else if (!passed) {
        counts->failure_count += 1;
        printf("FAILED %s (%s, %s): %s\n", test_case->identifier,
               test_case->type, path,
               (const char *)sxml_status_name(status).bytes);
    }
}

/* --------------------------------------------------- Catalog builder */

static conformance_run *run_from_state(void *state) { return state; }

static sxml_status
catalog_start_document(void *state, const sxml_document_start_event *event) {
    (void)event;
    run_from_state(state)->element_depth = 0;
    return SXML_STATUS_OK;
}

static sxml_status catalog_ignore(void *state) {
    (void)state;
    return SXML_STATUS_OK;
}

static sxml_status
catalog_start_element(void *state, const sxml_element_start_event *event) {
    conformance_run *run = run_from_state(state);
    run->element_depth += 1;
    if (run->is_in_case) {
        return SXML_STATUS_OK;
    }
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = sxml_name_pool_get_name_parts(
        run->name_pool, event->element_name, &parts);
    if (status == SXML_STATUS_OK &&
        sxml_byte_view_equals(parts.local_name,
                              SXML_BYTE_VIEW_FROM_LITERAL("TEST"))) {
        memset(&run->current_case, 0, sizeof run->current_case);
        run->is_in_case = true;
        run->case_depth = run->element_depth;
    }
    return status;
}

static sxml_status
catalog_add_namespace_binding(void *state,
                              const sxml_namespace_binding *binding) {
    (void)state;
    (void)binding;
    return SXML_STATUS_OK;
}

static sxml_status catalog_add_attribute(void *state,
                                         const sxml_attribute_event *event) {
    conformance_run *run = run_from_state(state);
    if (!run->is_in_case || run->element_depth != run->case_depth) {
        return SXML_STATUS_OK;
    }
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = sxml_name_pool_get_name_parts(
        run->name_pool, event->attribute_name, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    static const struct {
        const char *name;
        size_t offset;
    } fields[] = {
        {"ID", offsetof(conformance_case, identifier)},
        {"TYPE", offsetof(conformance_case, type)},
        {"ENTITIES", offsetof(conformance_case, entities)},
        {"RECOMMENDATION", offsetof(conformance_case, recommendation)},
        {"VERSION", offsetof(conformance_case, version)},
        {"EDITION", offsetof(conformance_case, edition)},
        {"NAMESPACE", offsetof(conformance_case, namespace_flag)},
        {"URI", offsetof(conformance_case, uri)},
    };
    for (size_t field_index = 0; field_index < sizeof fields / sizeof fields[0];
         field_index++) {
        if (sxml_byte_view_equals(
                parts.local_name,
                sxml_byte_view_from_c_string(fields[field_index].name))) {
            copy_field((char *)&run->current_case + fields[field_index].offset,
                       event->attribute_value);
        }
    }
    return SXML_STATUS_OK;
}

static sxml_status catalog_end_element(void *state) {
    conformance_run *run = run_from_state(state);
    if (run->is_in_case && run->element_depth == run->case_depth) {
        run->is_in_case = false;
        if (case_is_selected(&run->current_case) ||
            case_is_version_1_1(&run->current_case)) {
            run_case(run, &run->current_case);
        }
    }
    run->element_depth -= 1;
    return SXML_STATUS_OK;
}

static sxml_status catalog_add_text(void *state, const sxml_text_event *event) {
    (void)state;
    (void)event;
    return SXML_STATUS_OK;
}

static sxml_status catalog_add_comment(void *state,
                                       const sxml_comment_event *event) {
    (void)state;
    (void)event;
    return SXML_STATUS_OK;
}

static sxml_status catalog_add_processing_instruction(
    void *state, const sxml_processing_instruction_event *event) {
    (void)state;
    (void)event;
    return SXML_STATUS_OK;
}

static sxml_status
catalog_add_unparsed_entity(void *state, const sxml_unparsed_entity *entity) {
    (void)state;
    (void)entity;
    return SXML_STATUS_OK;
}

static const sxml_tree_builder_operations catalog_builder_operations = {
    .start_document = catalog_start_document,
    .end_document = catalog_ignore,
    .start_element = catalog_start_element,
    .add_namespace_binding = catalog_add_namespace_binding,
    .add_attribute = catalog_add_attribute,
    .end_element = catalog_end_element,
    .add_text = catalog_add_text,
    .add_comment = catalog_add_comment,
    .add_processing_instruction = catalog_add_processing_instruction,
    .add_unparsed_entity = catalog_add_unparsed_entity,
};

/* Parses one catalog file, running its selected cases. */
static sxml_status run_catalog(conformance_run *run, const char *catalog_path) {
    const sxml_byte_view catalog = read_file(catalog_path);
    if (catalog.bytes == NULL) {
        printf("cannot read catalog %s\n", catalog_path);
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    directory_of(catalog_path, run->catalog_directory);
    /* A catalog is an external entity with several TEST elements: wrap it
       in one element, without its XML declaration. */
    sxml_byte_view content = catalog;
    if (content.byte_count > 5 && memcmp(content.bytes, "<?xml", 5) == 0) {
        const uint8_t *end = memchr(content.bytes, '>', content.byte_count);
        if (end != NULL) {
            content.byte_count -= (size_t)(end + 1 - content.bytes);
            content.bytes = end + 1;
        }
    }
    static const char wrapper_start[] = "<wrap>";
    static const char wrapper_end[] = "</wrap>";
    const size_t wrapped_byte_count =
        sizeof wrapper_start - 1 + content.byte_count + sizeof wrapper_end - 1;
    void *memory = NULL;
    if (sxml_allocator_allocate(
            sxml_default_allocator(),
            (sxml_allocation_request){.byte_count = wrapped_byte_count,
                                      .alignment = 1},
            &memory) != SXML_STATUS_OK) {
        release_file(catalog);
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    uint8_t *wrapped = memory;
    memcpy(wrapped, wrapper_start, sizeof wrapper_start - 1);
    memcpy(wrapped + sizeof wrapper_start - 1, content.bytes,
           content.byte_count);
    memcpy(wrapped + sizeof wrapper_start - 1 + content.byte_count, wrapper_end,
           sizeof wrapper_end - 1);
    sxml_diagnostic diagnostic = {
        .description = {.bytes = NULL, .byte_count = 0},
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0}};
    const sxml_status status = sxml_xml_parser_parse(
        run->catalog_parser,
        &(sxml_xml_parse_request){
            .input = {.bytes = wrapped, .byte_count = wrapped_byte_count},
            .source_name = sxml_byte_view_from_c_string(catalog_path),
            .document_uri = {.bytes = NULL, .byte_count = 0},
            .base_uri = {.bytes = NULL, .byte_count = 0},
            .encoding_name = {.bytes = NULL, .byte_count = 0},
            .diagnostic = &diagnostic},
        (sxml_tree_builder){.state = run,
                            .operations = &catalog_builder_operations});
    if (status != SXML_STATUS_OK) {
        printf("catalog %s: %s at %u:%u: %.*s\n", catalog_path,
               (const char *)sxml_status_name(status).bytes,
               diagnostic.location.line_number,
               diagnostic.location.column_number,
               (int)diagnostic.description.byte_count,
               (const char *)diagnostic.description.bytes);
    }
    sxml_allocator_release(sxml_default_allocator(), wrapped);
    release_file(catalog);
    return status;
}

/*
 * Runs every catalog that suite/xmlconf.xml declares as an external
 * entity (<!ENTITY name SYSTEM "path">). The declarations are found by
 * scanning, because external entities are not read by the parser.
 */
static sxml_status run_suite(conformance_run *run,
                             const char *suite_directory) {
    char path[SXML_TEST_PATH_BYTE_COUNT];
    join_path(path, parts_of(suite_directory, "xmlconf.xml"));
    const sxml_byte_view top = read_file(path);
    if (top.bytes == NULL) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    sxml_status status = SXML_STATUS_OK;
    const char *text = (const char *)top.bytes;
    size_t offset = 0;
    while (status == SXML_STATUS_OK && offset < top.byte_count) {
        const char *declaration = NULL;
        for (size_t scan_offset = offset; scan_offset + 8 <= top.byte_count;
             scan_offset++) {
            if (memcmp(text + scan_offset, "<!ENTITY", 8) == 0) {
                declaration = text + scan_offset;
                break;
            }
        }
        if (declaration == NULL) {
            break;
        }
        const size_t start = (size_t)(declaration - text);
        size_t end = start;
        while (end < top.byte_count && text[end] != '>') {
            end += 1;
        }
        offset = end + 1;
        const char *quote = memchr(declaration, '"', end - start);
        if (quote == NULL) {
            continue;
        }
        const char *closing =
            memchr(quote + 1, '"', (size_t)(text + end - quote - 1));
        if (closing == NULL) {
            continue;
        }
        join_path(path, (path_parts){.first = suite_directory,
                                     .second = quote + 1,
                                     .second_byte_count =
                                         (size_t)(closing - quote - 1)});
        status = run_catalog(run, path);
    }
    release_file(top);
    return status;
}

/* ------------------------------------------------------------- Case */

static void test_conformance_suite(sxml_test_state *state) {
    char suite_directory[SXML_TEST_PATH_BYTE_COUNT];
    directory_of(__FILE__, suite_directory);
    join_path(suite_directory,
              parts_of(suite_directory, "../third_party/xmlconf/"));
    char probe[SXML_TEST_PATH_BYTE_COUNT];
    join_path(probe, parts_of(suite_directory, "xmlconf.xml"));
    FILE *file = fopen(probe, "rb");
    if (file == NULL) {
        printf("skipped: the W3C XML suite is not reachable at %s\n", probe);
        return;
    }
    (void)fclose(file);
    conformance_run run;
    memset(&run, 0, sizeof run);
    const sxml_allocator allocator = sxml_default_allocator();
    SXML_TEST_REQUIRE(
        state, sxml_name_pool_create(
                   allocator,
                   &(sxml_name_pool_configuration){.allocator = allocator,
                                                   .maximum_byte_count = 0},
                   &run.name_pool) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_type_registry_create(
                                 allocator,
                                 &(sxml_type_registry_configuration){
                                     .allocator = allocator,
                                     .name_pool = run.name_pool,
                                     .maximum_registered_type_count = 0},
                                 &run.registry) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      sxml_item_heap_create(allocator,
                                            &(sxml_item_heap_configuration){
                                                .allocator = allocator,
                                                .name_pool = run.name_pool,
                                                .type_registry = run.registry,
                                                .maximum_byte_count = 0},
                                            &run.item_heap) == SXML_STATUS_OK);
    const sxml_xml_parser_configuration configuration = {
        .allocator = allocator,
        .name_pool = run.name_pool,
        .limits = {.maximum_depth = 0,
                   .maximum_entity_expansion_byte_count = 1 << 20}};
    SXML_TEST_REQUIRE(state, sxml_xml_parser_create(allocator, &configuration,
                                                    &run.document_parser) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_xml_parser_create(allocator, &configuration,
                                                    &run.catalog_parser) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run_suite(&run, suite_directory) == SXML_STATUS_OK);
    const conformance_counts *counts = &run.counts;
    printf("valid %u/%u, invalid %u/%u, not-wf %u/%u; NAMESPACE=\"no\": %u "
           "of %u refused as not namespace-well-formed\n",
           counts->valid_passed, counts->valid_total, counts->invalid_passed,
           counts->invalid_total, counts->not_well_formed_passed,
           counts->not_well_formed_total, counts->namespace_free_refused,
           counts->namespace_free_total);
    SXML_TEST_EXPECT(state, counts->failure_count == 0);
    /* The selection of D28 and grammars/xml/README.md. */
    SXML_TEST_EXPECT(state, counts->valid_total == 601);
    SXML_TEST_EXPECT(state, counts->invalid_total == 175);
    SXML_TEST_EXPECT(state, counts->not_well_formed_total == 948);
    const conformance_counts *version_1_1 = &run.version_1_1_counts;
    printf("XML 1.1: valid %u/%u, invalid %u/%u, not-wf %u/%u, error %u; "
           "%u failed only because external entities are not read\n",
           version_1_1->valid_passed, version_1_1->valid_total,
           version_1_1->invalid_passed, version_1_1->invalid_total,
           version_1_1->not_well_formed_passed,
           version_1_1->not_well_formed_total, version_1_1->error_total,
           version_1_1->external_entity_failure_count);
    SXML_TEST_EXPECT(state, version_1_1->failure_count == 0);
    sxml_xml_parser_destroy(run.catalog_parser);
    sxml_xml_parser_destroy(run.document_parser);
    sxml_item_heap_destroy(run.item_heap);
    sxml_type_registry_destroy(run.registry);
    sxml_name_pool_destroy(run.name_pool);
}

static const sxml_test_case xml_conformance_test_cases[] = {
    {.name = "conformance_suite", .function = test_conformance_suite},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xml_conformance",
            .cases = xml_conformance_test_cases,
            .case_count = SXML_TEST_COUNT(xml_conformance_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
