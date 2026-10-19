/*
 * Tests for the EXPath File feature (src/function_library_file.c, packet
 * 7.9, D200) through the public API, over an in-memory file system that
 * implements sxml_file_system (include/sxml/processor.h): paths and their
 * normalization, reading and writing, directories (create, list,
 * descendants with predicates, copy, move, delete), the errors in the file
 * namespace, the null file system, the absence of the functions without
 * the feature (XPST0017), and an allocation-failure loop (D23). Every case
 * checks for leaked cells and allocations.
 *
 * Golden file: tests/function_library_binary_test.c.
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* The functions' namespace as a URIQualifiedName prefix. */
#define FILE_NS "Q{http://expath.org/ns/file}"

/* ------------------------------------------------- In-memory file system */

#define MEMORY_ENTRY_CAPACITY 64
#define MEMORY_PATH_CAPACITY 96
#define MEMORY_CONTENT_CAPACITY 256

typedef struct memory_entry {
    bool is_used;
    char path[MEMORY_PATH_CAPACITY];
    sxml_file_kind file_kind;
    uint8_t content[MEMORY_CONTENT_CAPACITY];
    size_t byte_count;
} memory_entry;

typedef struct memory_file_system {
    memory_entry entries[MEMORY_ENTRY_CAPACITY];
    char result[MEMORY_PATH_CAPACITY];
    unsigned temporary_number;
} memory_file_system;

static bool path_is(const char *path, sxml_byte_view view) {
    return strlen(path) == view.byte_count && memcmp(path, view.bytes, view.byte_count) == 0;
}

static memory_entry *find_entry(memory_file_system *files, sxml_byte_view path) {
    for (size_t entry_index = 0; entry_index < MEMORY_ENTRY_CAPACITY; entry_index += 1) {
        if (files->entries[entry_index].is_used && path_is(files->entries[entry_index].path, path)) {
            return &files->entries[entry_index];
        }
    }
    return NULL;
}

static memory_entry *add_entry(memory_file_system *files, sxml_byte_view path,
                               sxml_file_kind kind) {
    if (path.byte_count >= MEMORY_PATH_CAPACITY) {
        return NULL;
    }
    for (size_t entry_index = 0; entry_index < MEMORY_ENTRY_CAPACITY; entry_index += 1) {
        memory_entry *entry = &files->entries[entry_index];
        if (!entry->is_used) {
            memset(entry, 0, sizeof(*entry));
            entry->is_used = true;
            memcpy(entry->path, path.bytes, path.byte_count);
            entry->file_kind = kind;
            return entry;
        }
    }
    return NULL;
}

/* The parent directory of an entry's path: what precedes the last "/". */
static bool is_child_of(const char *path, sxml_byte_view directory) {
    const size_t path_length = strlen(path);
    const size_t prefix = directory.byte_count == 1 ? 0 : directory.byte_count;
    if (path_length <= prefix + 1 || memcmp(path, directory.bytes, prefix) != 0 ||
        path[prefix] != '/') {
        return false;
    }
    return strchr(path + prefix + 1, '/') == NULL;
}

static sxml_status memory_describe(void *state, sxml_byte_view path,
                                   sxml_file_description *description) {
    memory_file_system *files = state;
    const memory_entry *entry = find_entry(files, path);
    *description = (sxml_file_description){
        .file_kind = entry == NULL ? SXML_FILE_KIND_NONE : entry->file_kind,
        .byte_count = entry == NULL ? 0 : entry->byte_count,
        .modified_seconds = 86400,
        .modified_nanoseconds = 0};
    return SXML_STATUS_OK;
}

static sxml_status memory_read(void *state, const sxml_file_read_request *request) {
    memory_file_system *files = state;
    const memory_entry *entry = find_entry(files, request->path);
    if (entry == NULL || request->offset + request->memory.byte_count > entry->byte_count) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    memcpy(request->memory.bytes, entry->content + request->offset, request->memory.byte_count);
    return SXML_STATUS_OK;
}

static sxml_status memory_write(void *state, const sxml_file_write_request *request) {
    memory_file_system *files = state;
    memory_entry *entry = find_entry(files, request->path);
    if (entry == NULL) {
        entry = add_entry(files, request->path, SXML_FILE_KIND_FILE);
    }
    if (entry == NULL || entry->file_kind != SXML_FILE_KIND_FILE) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    size_t start = 0;
    if (request->write_mode == SXML_FILE_WRITE_MODE_APPEND) {
        start = entry->byte_count;
    } else if (request->write_mode == SXML_FILE_WRITE_MODE_AT_OFFSET) {
        start = (size_t)request->offset;
    } else {
        entry->byte_count = 0;
    }
    if (start + request->content.byte_count > MEMORY_CONTENT_CAPACITY) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    if (request->content.byte_count > 0) {
        memcpy(entry->content + start, request->content.bytes, request->content.byte_count);
    }
    if (start + request->content.byte_count > entry->byte_count) {
        entry->byte_count = start + request->content.byte_count;
    }
    return SXML_STATUS_OK;
}

static sxml_status memory_list(void *state, const sxml_file_list_request *request) {
    memory_file_system *files = state;
    if (request->path.byte_count == 0) {
        const sxml_file_entry root = {.name = TEXT("/"), .file_kind = SXML_FILE_KIND_DIRECTORY};
        return request->sink.add(request->sink.state, &root);
    }
    sxml_status status = SXML_STATUS_OK;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < MEMORY_ENTRY_CAPACITY;
         entry_index += 1) {
        const memory_entry *entry = &files->entries[entry_index];
        if (!entry->is_used || !is_child_of(entry->path, request->path)) {
            continue;
        }
        const char *name = strrchr(entry->path, '/') + 1;
        const sxml_file_entry found = {
            .name = {.bytes = (const uint8_t *)name, .byte_count = strlen(name)},
            .file_kind = entry->file_kind};
        status = request->sink.add(request->sink.state, &found);
    }
    return status;
}

static sxml_status memory_create_directory(void *state, sxml_byte_view path) {
    memory_file_system *files = state;
    if (find_entry(files, path) != NULL) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    return add_entry(files, path, SXML_FILE_KIND_DIRECTORY) == NULL
               ? SXML_STATUS_INPUT_OUTPUT_FAILED
               : SXML_STATUS_OK;
}

static sxml_status memory_remove(void *state, sxml_byte_view path) {
    memory_file_system *files = state;
    memory_entry *entry = find_entry(files, path);
    if (entry == NULL) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    for (size_t entry_index = 0; entry_index < MEMORY_ENTRY_CAPACITY; entry_index += 1) {
        if (files->entries[entry_index].is_used && is_child_of(files->entries[entry_index].path, path)) {
            return SXML_STATUS_INPUT_OUTPUT_FAILED;
        }
    }
    entry->is_used = false;
    return SXML_STATUS_OK;
}

static sxml_status memory_move(void *state, const sxml_file_move_request *request) {
    memory_file_system *files = state;
    if (request->target.byte_count >= MEMORY_PATH_CAPACITY ||
        find_entry(files, request->source) == NULL) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    memory_entry *replaced = find_entry(files, request->target);
    if (replaced != NULL) {
        replaced->is_used = false;
    }
    /* Every entry at or below the source moves. */
    const size_t source_length = request->source.byte_count;
    for (size_t entry_index = 0; entry_index < MEMORY_ENTRY_CAPACITY; entry_index += 1) {
        memory_entry *entry = &files->entries[entry_index];
        const size_t length = strlen(entry->path);
        if (!entry->is_used || length < source_length ||
            memcmp(entry->path, request->source.bytes, source_length) != 0 ||
            (length > source_length && entry->path[source_length] != '/')) {
            continue;
        }
        char moved[MEMORY_PATH_CAPACITY];
        const size_t moved_length = request->target.byte_count + length - source_length;
        if (moved_length >= MEMORY_PATH_CAPACITY) {
            return SXML_STATUS_INPUT_OUTPUT_FAILED;
        }
        memcpy(moved, request->target.bytes, request->target.byte_count);
        memcpy(moved + request->target.byte_count, entry->path + source_length,
               length - source_length + 1);
        memcpy(entry->path, moved, moved_length + 1);
    }
    return SXML_STATUS_OK;
}

static sxml_status memory_create_temporary(void *state, const sxml_file_temporary_request *request,
                                           sxml_byte_view *created_path) {
    memory_file_system *files = state;
    files->temporary_number += 1;
    const int length = snprintf(files->result, sizeof(files->result), "%.*s/%.*s%u%.*s",
                                (int)request->directory.byte_count, request->directory.bytes,
                                (int)request->prefix.byte_count, request->prefix.bytes,
                                files->temporary_number, (int)request->suffix.byte_count,
                                request->suffix.bytes);
    if (length <= 0 || (size_t)length >= sizeof(files->result)) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    *created_path = (sxml_byte_view){.bytes = (const uint8_t *)files->result,
                                     .byte_count = (size_t)length};
    return add_entry(files, *created_path,
                     request->is_directory ? SXML_FILE_KIND_DIRECTORY : SXML_FILE_KIND_FILE) == NULL
               ? SXML_STATUS_INPUT_OUTPUT_FAILED
               : SXML_STATUS_OK;
}

static sxml_status memory_get_directory(void *state, sxml_file_directory_kind directory_kind,
                                        sxml_byte_view *path) {
    (void)state;
    *path = directory_kind == SXML_FILE_DIRECTORY_KIND_CURRENT ? TEXT("/work") : TEXT("/tmp");
    return SXML_STATUS_OK;
}

static sxml_status memory_canonicalize(void *state, sxml_byte_view path,
                                       sxml_byte_view *canonical_path) {
    (void)state;
    *canonical_path = path;
    return SXML_STATUS_OK;
}

static const sxml_file_system_operations memory_operations = {
    .directory_separator = SXML_BYTE_VIEW_INITIALIZER("/"),
    .path_separator = SXML_BYTE_VIEW_INITIALIZER(":"),
    .line_separator = SXML_BYTE_VIEW_INITIALIZER("\n"),
    .describe = memory_describe,
    .read = memory_read,
    .write = memory_write,
    .list = memory_list,
    .create_directory = memory_create_directory,
    .remove = memory_remove,
    .move = memory_move,
    .create_temporary = memory_create_temporary,
    .get_directory = memory_get_directory,
    .canonicalize = memory_canonicalize,
};

/* A file system with /, /work, /tmp and /work/data.txt ("one\ntwo\n"). */
static void memory_files_start(memory_file_system *files) {
    memset(files, 0, sizeof(*files));
    (void)add_entry(files, TEXT("/"), SXML_FILE_KIND_DIRECTORY);
    (void)add_entry(files, TEXT("/work"), SXML_FILE_KIND_DIRECTORY);
    (void)add_entry(files, TEXT("/tmp"), SXML_FILE_KIND_DIRECTORY);
    memory_entry *data = add_entry(files, TEXT("/work/data.txt"), SXML_FILE_KIND_FILE);
    if (data != NULL) {
        memcpy(data->content, "one\ntwo\n", 8);
        data->byte_count = 8;
    }
}

/* ---------------------------------------------------------------- Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
} fixture;

typedef struct fixture_options {
    uint64_t failing_allocation_number;
    bool names_feature;
    /* NULL for the null file system. */
    memory_file_system *files;
} fixture_options;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(const fixture_options *options, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL, .output_buffer = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     options->failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    const sxml_optional_feature feature = sxml_expath_file_feature();
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .optional_features = {.optional_features = &feature,
                                  .optional_feature_count = options->names_feature ? 1 : 0},
            .file_system = {.state = options->files,
                            .operations = options->files == NULL ? NULL : &memory_operations}},
        &test_fixture->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            sxml_default_allocator(),
            &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                                .maximum_byte_count = 0},
            &test_fixture->output_buffer);
    }
    return status;
}

/* Destroys the fixture; true when nothing leaked. */
static bool fixture_destroy(fixture *test_fixture) {
    bool has_no_leak = true;
    if (test_fixture->processor != NULL) {
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(test_fixture->processor);
        sxml_item_heap_clear_raised_error(item_heap);
        has_no_leak = sxml_item_heap_read_counts(item_heap).live_cell_count == 0;
    }
    sxml_processor_destroy(test_fixture->processor);
    sxml_output_buffer_destroy(test_fixture->output_buffer);
    if (test_fixture->counting_allocator != NULL) {
        has_no_leak = has_no_leak && sxml_counting_allocator_read_counts(
                                         test_fixture->counting_allocator)
                                             .live_allocation_count == 0;
        sxml_counting_allocator_destroy(test_fixture->counting_allocator);
    }
    return has_no_leak;
}

/* Compiles and evaluates expression_text and writes the result by the
   adaptive method into the fixture's output buffer (cleared first). */
static sxml_status run_query(const fixture *test_fixture, sxml_byte_view expression_text,
                             sxml_error_record *error_record) {
    memset(error_record, 0, sizeof(*error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        fixture_allocator(test_fixture),
        &(sxml_xpath_expression_configuration){
            .processor = test_fixture->processor,
            .expression_text = expression_text,
            .source_name = TEXT("test.xpath"),
            .static_context = {.static_base_uri = TEXT("file:///work/query.xq")},
            .error_record = error_record},
        &expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence result = sxml_sequence_empty();
    status = sxml_xpath_expression_evaluate(
        expression, &(sxml_xpath_evaluation_request){.error_record = error_record}, &result);
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(test_fixture->processor),
            &(sxml_serialization_request){
                .value = result,
                .serialization_parameters = {.serialization_method =
                                                 SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                             .has_item_separator = true,
                                             .item_separator = TEXT(" ")},
                .error_record = error_record},
            sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
    }
    sxml_sequence_release(result);
    sxml_xpath_expression_destroy(expression);
    return status;
}

static sxml_byte_view text_of(const char *text) {
    return (sxml_byte_view){.bytes = (const uint8_t *)text, .byte_count = strlen(text)};
}

/* Runs pair[0] and checks its adaptive output pair[1], or the error code
   it raises ("!CODE"; "!file:CODE" also checks the file namespace). */
static void expect_query(sxml_test_state *state, const fixture *test_fixture,
                         const char *const pair[2]) {
    sxml_error_record error_record;
    const sxml_status status = run_query(test_fixture, text_of(pair[0]), &error_record);
    const char *expected = pair[1];
    if (expected[0] != '!') {
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                    sxml_output_buffer_get_bytes(test_fixture->output_buffer),
                                    text_of(expected)));
        return;
    }
    SXML_TEST_EXPECT(state, sxml_status_has_error_record(status));
    const char *code = expected + 1;
    if (strncmp(code, "file:", 5) == 0) {
        code += 5;
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.namespace_uri,
                                                      TEXT("http://expath.org/ns/file")));
    }
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.local_name, text_of(code)));
}

/* Runs the cases in order on one processor over one in-memory file
   system. */
static void expect_queries(sxml_test_state *state, const char *const (*cases)[2],
                           size_t case_count) {
    static memory_file_system files;
    memory_files_start(&files);
    fixture test_fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&(fixture_options){.names_feature = true, .files = &files},
                                     &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < case_count; case_index += 1) {
        expect_query(state, &test_fixture, cases[case_index]);
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------------ Cases */

/* Paths (EXPath File 4.0, 1.2, 2.1, 2.7, 5): lexical, resolved against the
   current directory, directories with a trailing separator. */
static void test_paths(sxml_test_state *state) {
    static const char *const cases[][2] = {
        {FILE_NS "name('dir/file.txt'), " FILE_NS "name('dir/'), " FILE_NS "name('/'), " FILE_NS
                 "name('file:///tmp/001.bin'), " FILE_NS "name('')",
         "\"file.txt\" \"dir\" \"\" \"001.bin\" \"\""},
        {FILE_NS "is-absolute('/a'), " FILE_NS "is-absolute('a/b')", "true() false()"},
        {FILE_NS "resolve-path('data.txt'), " FILE_NS "resolve-path('.'), " FILE_NS
                 "resolve-path('x/../y/./z')",
         "\"/work/data.txt\" \"/work/\" \"/work/y/z\""},
        {FILE_NS "resolve-path('b.txt', '/work/data.txt'), " FILE_NS
                 "resolve-path('b.txt', '/tmp/')",
         "\"/work/b.txt\" \"/tmp/b.txt\""},
        {FILE_NS "resolve-path('a', 'relative')", "!file:is-relative"},
        {FILE_NS "parent('data.txt'), " FILE_NS "parent('/'), " FILE_NS "parent('/work/')",
         "\"/work/\" \"/\""},
        {FILE_NS "path-to-uri('my file.txt')", "\"file:///work/my%20file.txt\""},
        {FILE_NS "path-to-native('file:///work/../work/data.txt')", "\"/work/data.txt\""},
        {FILE_NS "path-to-native('missing')", "!file:not-found"},
        {FILE_NS "current-dir(), " FILE_NS "temp-dir(), " FILE_NS "base-dir()",
         "\"/work/\" \"/tmp/\" \"/work/\""},
        {FILE_NS "dir-separator(), " FILE_NS "path-separator(), " FILE_NS "list-roots()",
         "\"/\" \":\" \"/\""},
    };
    expect_queries(state, cases, SXML_TEST_COUNT(cases));
}

/* Reading and writing (4). */
static void test_reading_and_writing(sxml_test_state *state) {
    static const char *const cases[][2] = {
        {FILE_NS "exists('data.txt'), " FILE_NS "is-file('data.txt'), " FILE_NS
                 "is-dir('/work'), " FILE_NS "exists('nothing'), " FILE_NS "size('data.txt')",
         "true() true() true() false() 8"},
        {FILE_NS "read-text('data.txt'), " FILE_NS "read-text-lines('data.txt')",
         "\"one\ntwo\n\" \"one\" \"two\""},
        {"string(xs:hexBinary(" FILE_NS "read-binary('data.txt', 4, 3)))", "\"74776F\""},
        {FILE_NS "read-binary('data.txt', 7, 2)", "!file:out-of-range"},
        {FILE_NS "read-binary('data.txt', -1)", "!file:out-of-range"},
        {FILE_NS "read-text('nothing')", "!file:not-found"},
        {FILE_NS "read-text('/work')", "!file:is-dir"},
        {FILE_NS "write-text('t.txt', 'caf' || codepoints-to-string(233)), " FILE_NS
                 "size('t.txt'), " FILE_NS "read-text('t.txt')",
         "5 \"caf\xC3\xA9\""},
        {FILE_NS "append-text('t.txt', '!', 'ISO-8859-1'), " FILE_NS "size('t.txt')", "6"},
        {FILE_NS "write-text('u.txt', 'A', 'UTF-16'), string(xs:hexBinary(" FILE_NS
                 "read-binary('u.txt')))",
         "\"FEFF0041\""},
        {FILE_NS "write-text('t.txt', 'x', 'no-such')", "!file:unknown-encoding"},
        {FILE_NS "write-text('missing/t.txt', 'x')", "!file:no-dir"},
        {FILE_NS "write-text('/work', 'x')", "!file:is-dir"},
        {FILE_NS "write-text-lines('l.txt', ('a', 'b')), " FILE_NS
                 "append-text-lines('l.txt', 'c'), " FILE_NS "read-text-lines('l.txt')",
         "\"a\" \"b\" \"c\""},
        {FILE_NS "write-binary('b.bin', xs:hexBinary('0102')), " FILE_NS
                 "write-binary('b.bin', xs:hexBinary('0304'), 1), " FILE_NS
                 "append-binary('b.bin', xs:hexBinary('05')), string(xs:hexBinary(" FILE_NS
                 "read-binary('b.bin')))",
         "\"01030405\""},
        {FILE_NS "write-binary('b.bin', xs:hexBinary('00'), 9)", "!file:out-of-range"},
        {FILE_NS "write('s.json', map { 'a': 1 }, map { 'method': 'json' }), " FILE_NS
                 "read-text('s.json')",
         "\"{\"\"a\"\":1}\""},
        {FILE_NS "append('s.json', (1, 2)), " FILE_NS "read-text('s.json')",
         "\"{\"\"a\"\":1}1 2\""},
        {FILE_NS "write-binary('bad.txt', xs:hexBinary('FF')), " FILE_NS "read-text('bad.txt')",
         "!file:io-error"},
        {"string-to-codepoints(" FILE_NS "read-text('bad.txt', { 'fallback': true() }))",
         "65533"},
        {"string-to-codepoints(" FILE_NS "read-text('bad.txt', 'ISO-8859-1'))", "255"},
        {FILE_NS "read-text('bad.txt', 'no-such')", "!file:unknown-encoding"},
        {"exists(" FILE_NS "last-modified('data.txt'))", "true()"},
        {"string(" FILE_NS "last-modified('data.txt'))", "\"1970-01-02T00:00:00Z\""},
    };
    expect_queries(state, cases, SXML_TEST_COUNT(cases));
}

/* Directories (3, 5.2 to 5.4): no recursion in C, work lists instead. */
static void test_directories(sxml_test_state *state) {
    static const char *const cases[][2] = {
        {FILE_NS "create-dir('d/e/f'), " FILE_NS "is-dir('d/e/f'), " FILE_NS "create-dir('d')",
         "true()"},
        {FILE_NS "create-dir('data.txt/x')", "!file:exists"},
        {FILE_NS "write-text('d/a.txt', 'a'), " FILE_NS "write-text('d/e/b.txt', 'bb'), "
                 "sort(" FILE_NS "list('d')), sort(" FILE_NS "list('d', true())), " FILE_NS
                 "list('d', true(), '*.txt') => sort()",
         "\"a.txt\" \"e/\" \"a.txt\" \"e/\" \"e/b.txt\" \"e/f/\" \"a.txt\" \"e/b.txt\""},
        {FILE_NS "size('d', true()), " FILE_NS "size('d')", "3 0"},
        {"sort(" FILE_NS "children('d'))", "\"d/a.txt\" \"d/e/\""},
        {FILE_NS "children('data.txt')", "!file:no-dir"},
        {FILE_NS "children('nothing')", "!file:not-found"},
        {"sort(" FILE_NS "descendants('d'))", "\"d/a.txt\" \"d/e/\" \"d/e/b.txt\" \"d/e/f/\""},
        {"sort(" FILE_NS "descendants('d', { 'depth': 0 }))", "\"d/a.txt\" \"d/e/\""},
        {"sort(" FILE_NS "descendants('d', { 'filter': fn($p) { ends-with($p, '.txt') } }))",
         "\"d/a.txt\" \"d/e/b.txt\""},
        {"sort(" FILE_NS "descendants('d', { 'recurse': fn($p) { () } }))",
         "\"d/a.txt\" \"d/e/\""},
        {FILE_NS "descendants('d', { 'filter': fn($p) { 1 } })", "!XPTY0004"},
        {FILE_NS "copy('d', 'c'), sort(" FILE_NS "list('c', true()))",
         "\"a.txt\" \"e/\" \"e/b.txt\" \"e/f/\""},
        {FILE_NS "copy('d', 'c'), sort(" FILE_NS "list('c', false()))", "\"a.txt\" \"d/\" \"e/\""},
        {FILE_NS "copy('d', 'd/e')", "!file:cyclic"},
        {FILE_NS "copy('data.txt', 'c'), " FILE_NS "read-text('c/data.txt') => string-length()",
         "8"},
        {FILE_NS "copy('nothing', 'c')", "!file:not-found"},
        {FILE_NS "move('c', 'm'), " FILE_NS "exists('c'), " FILE_NS "read-text('m/e/b.txt')",
         "false() \"bb\""},
        {FILE_NS "move('m/a.txt', 'd'), " FILE_NS "exists('m/a.txt')", "false()"},
        {FILE_NS "delete('m')", "!file:is-dir"},
        {FILE_NS "delete('m', true()), " FILE_NS "exists('m'), " FILE_NS "delete('m')", "false()"},
        {FILE_NS "create-temp-file('p', '.s') => " FILE_NS "name(), " FILE_NS
                 "create-temp-dir((), (), 'd') => ends-with('/')",
         "\"p1.s\" true()"},
        {FILE_NS "create-temp-dir((), (), 'data.txt')", "!file:no-dir"},
    };
    expect_queries(state, cases, SXML_TEST_COUNT(cases));
}

/* Without a file system the functions that need files raise
   file:io-error; without the feature they are absent. */
static void test_without_files_or_feature(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.names_feature = true, .files = NULL},
                                            &test_fixture) == SXML_STATUS_OK);
    static const char *const no_files[][2] = {
        {FILE_NS "read-text('a')", "!file:io-error"},
        {FILE_NS "exists('a'), " FILE_NS "name('a/b'), " FILE_NS "dir-separator()",
         "false() \"b\" \"/\""},
        {FILE_NS "current-dir()", "!file:io-error"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(no_files); case_index += 1) {
        expect_query(state, &test_fixture, no_files[case_index]);
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    static memory_file_system files;
    memory_files_start(&files);
    SXML_TEST_REQUIRE(state,
                      fixture_create(&(fixture_options){.names_feature = false, .files = &files},
                                     &test_fixture) == SXML_STATUS_OK);
    static const char *const absent[2] = {FILE_NS "exists('data.txt')", "!XPST0017"};
    expect_query(state, &test_fixture, absent);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Every allocation may fail: the evaluation then fails cleanly. */
static void test_allocation_failures_leak_nothing(sxml_test_state *state) {
    static const sxml_byte_view expression = SXML_BYTE_VIEW_INITIALIZER(
        FILE_NS "create-dir('w/x'), " FILE_NS "write-text-lines('w/x/a.txt', ('1', '2')), " FILE_NS
                "copy('w', 'v'), count(" FILE_NS "descendants('v', { 'filter': fn($p) { true() } })), "
                FILE_NS "read-text-lines('v/x/a.txt'), " FILE_NS "delete('v', true()), " FILE_NS
                "exists('v')");
    static memory_file_system files;
    bool has_succeeded = false;
    for (uint64_t failing_number = 1; failing_number < 8000 && !has_succeeded;
         failing_number += 1) {
        memory_files_start(&files);
        fixture test_fixture;
        sxml_status status = fixture_create(
            &(fixture_options){.failing_allocation_number = failing_number,
                               .names_feature = true,
                               .files = &files},
            &test_fixture);
        if (status == SXML_STATUS_OK) {
            sxml_error_record error_record;
            status = run_query(&test_fixture, expression, &error_record);
            has_succeeded = status == SXML_STATUS_OK;
            if (has_succeeded) {
                SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                            sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                            TEXT("2 \"1\" \"2\" false()")));
            }
        }
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, has_succeeded);
}

static const sxml_test_case file_test_cases[] = {
    {.name = "paths", .function = test_paths},
    {.name = "reading_and_writing", .function = test_reading_and_writing},
    {.name = "directories", .function = test_directories},
    {.name = "without_files_or_feature", .function = test_without_files_or_feature},
    {.name = "allocation_failures_leak_nothing", .function = test_allocation_failures_leak_nothing},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "function_library_file",
            .cases = file_test_cases,
            .case_count = SXML_TEST_COUNT(file_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
