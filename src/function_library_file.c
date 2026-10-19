/*
 * The EXPath File feature (packet 7.9, D200): the file: functions of the
 * EXPath File Module 4.0 (namespace http://expath.org/ns/file) over the
 * host's file system (sxml_file_system, include/sxml/processor.h). An
 * optional feature (glossary `optional_feature`, D189) whose rows are the
 * definition's library_functions (D191): without
 * sxml_expath_file_feature() the functions are absent (XPST0017) and this
 * file is not linked. sxml_all_optional_features() leaves it out (D184).
 *
 * Golden file: src/function_library_binary.c (a feature's rows, errors in
 * the feature's namespace, octet buffers in item heap memory).
 *
 * Paths (EXPath File 4.0, 1.2): a query's path is a host path or a file:
 * URI; it is made absolute against the CURRENT directory and normalized
 * here, lexically ("." and empty segments dropped, ".." removes the
 * segment before it), so the host only ever sees absolute paths. Paths
 * that sxml returns for directories end with the directory separator.
 *
 * No recursion (D71): the recursive operations (file:copy and file:delete
 * of a directory, file:size and file:list with $recursive, file:descendants)
 * walk directories with a work list in item heap memory, and
 * file:descendants is a resumable body that asks the evaluator to call its
 * recurse and filter predicates.
 *
 * The functions that touch files are nondeterministic and context
 * dependent (1.4), so no pass moves, folds or caches them.
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "function_library_internal.h"
#include "optional_feature_internal.h"
#include "processor_internal.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "text_decoding_internal.h"
#include "xdm_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- Errors */

/* An error of EXPath File 4.0, appendix B, in the file namespace. */
typedef struct file_error {
    sxml_byte_view local_name;
    sxml_byte_view description;
} file_error;

#define SXML_FILE_ERROR(name_text, description_text)                          \
    {.local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                       \
     .description = SXML_BYTE_VIEW_INITIALIZER(description_text)}

static const file_error not_found_error =
    SXML_FILE_ERROR("not-found", "The path does not exist (EXPath File 4.0)");
static const file_error exists_error = SXML_FILE_ERROR(
    "exists", "The path, or one of its parent directories, names an existing file (EXPath File 4.0)");
static const file_error no_dir_error = SXML_FILE_ERROR(
    "no-dir", "The path, or its parent, does not name a directory (EXPath File 4.0)");
static const file_error is_dir_error =
    SXML_FILE_ERROR("is-dir", "The path names a directory (EXPath File 4.0)");
static const file_error is_relative_error =
    SXML_FILE_ERROR("is-relative", "The base path is relative (EXPath File 4.0, 5.6)");
static const file_error unknown_encoding_error = SXML_FILE_ERROR(
    "unknown-encoding", "The encoding is not one sxml supports: UTF-8, UTF-16, UTF-16BE, "
                        "UTF-16LE, ISO-8859-1 or US-ASCII (EXPath File 4.0)");
static const file_error out_of_range_error = SXML_FILE_ERROR(
    "out-of-range", "The offset or length is negative or beyond the file (EXPath File 4.0)");
static const file_error cyclic_error = SXML_FILE_ERROR(
    "cyclic", "The target lies inside the source directory (EXPath File 4.0, 3.1, 3.6)");
static const file_error io_error =
    SXML_FILE_ERROR("io-error", "The file system refused the operation (EXPath File 4.0)");
static const file_error decoding_error = SXML_FILE_ERROR(
    "io-error", "The file does not decode in the encoding, or has a character XML does not "
                "allow (EXPath File 4.0, 4.6)");
static const file_error encoding_error = SXML_FILE_ERROR(
    "io-error", "The encoding cannot represent a character of the text (EXPath File 4.0, 4.10)");
static const file_error no_file_system_error = SXML_FILE_ERROR(
    "io-error", "The host gave the processor no file system (include/sxml/processor.h)");

/* Fills call->error_record with error, in the file namespace, and returns
   SXML_STATUS_DYNAMIC_ERROR. */
static sxml_status report(const sxml_function_call *call, const file_error *error) {
    if (call->error_record != NULL) {
        *call->error_record = (sxml_error_record){
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL("http://expath.org/ns/file"),
                     .local_name = error->local_name,
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("file")},
            .value = NULL,
            .diagnostic = {.description = error->description,
                           .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                        .line_number = 0,
                                        .column_number = 0,
                                        .byte_offset = 0}}};
    }
    return SXML_STATUS_DYNAMIC_ERROR;
}

/* ------------------------------------------------------- Growing arrays */

/* Fixed-size elements in item heap memory; elements move when it grows. */
typedef struct heap_array {
    sxml_item_heap *item_heap;
    size_t element_byte_count;
    uint8_t *bytes;
    size_t element_count;
    size_t element_capacity;
} heap_array;

static heap_array heap_array_empty(sxml_item_heap *item_heap, size_t element_byte_count) {
    return (heap_array){.item_heap = item_heap,
                        .element_byte_count = element_byte_count,
                        .bytes = NULL,
                        .element_count = 0,
                        .element_capacity = 0};
}

static void heap_array_release(heap_array *array) {
    if (array->bytes != NULL) {
        sxml_item_heap_release_bytes(
            array->item_heap,
            (sxml_heap_bytes){.memory = array->bytes,
                              .byte_count = array->element_capacity * array->element_byte_count});
    }
    array->bytes = NULL;
    array->element_count = 0;
    array->element_capacity = 0;
}

/* Appends a copy of element. */
static sxml_status heap_array_push(heap_array *array, const void *element) {
    if (array->element_count == array->element_capacity) {
        const size_t capacity = array->element_capacity == 0 ? 8 : array->element_capacity * 2;
        size_t byte_count = 0;
        sxml_status status = sxml_size_multiply(capacity, array->element_byte_count, &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_item_heap_allocate_bytes(
                array->item_heap,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(max_align_t)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (array->bytes != NULL) {
            memcpy(memory, array->bytes, array->element_count * array->element_byte_count);
            sxml_item_heap_release_bytes(
                array->item_heap,
                (sxml_heap_bytes){.memory = array->bytes,
                                  .byte_count =
                                      array->element_capacity * array->element_byte_count});
        }
        array->bytes = memory;
        array->element_capacity = capacity;
    }
    memcpy(array->bytes + array->element_count * array->element_byte_count, element,
           array->element_byte_count);
    array->element_count += 1;
    return SXML_STATUS_OK;
}

static void *heap_array_at(const heap_array *array, size_t element_index) {
    return array->bytes + element_index * array->element_byte_count;
}

/* ------------------------------------------------------------------ Text */

static sxml_library_text text_empty(sxml_item_heap *item_heap) {
    return (sxml_library_text){
        .item_heap = item_heap, .bytes = NULL, .byte_count = 0, .byte_capacity = 0};
}

static sxml_byte_view text_view(const sxml_library_text *text) {
    return sxml_function_library_text_view(text);
}

static bool views_equal(sxml_byte_view first, sxml_byte_view second) {
    return sxml_byte_view_equals(first, second);
}

/* A view of bytes[start, end) of text. */
typedef struct byte_span {
    size_t start;
    size_t end;
} byte_span;

static sxml_byte_view view_span(sxml_byte_view text, byte_span span) {
    if (text.bytes == NULL) {
        return text;
    }
    return (sxml_byte_view){.bytes = text.bytes + span.start, .byte_count = span.end - span.start};
}

/* ------------------------------------------------------- A store of paths */

/* One path of a store: where its text is, and what it names. */
typedef struct stored_path {
    byte_span span;
    sxml_file_kind file_kind;
    /* The directory levels below the walk's start (file:descendants). */
    size_t depth;
} stored_path;

/* Paths kept back to back in one text; views move when it grows, so a
   path added from another of the same store is built elsewhere first. */
typedef struct path_store {
    sxml_library_text text;
    heap_array paths;
} path_store;

static path_store path_store_empty(sxml_item_heap *item_heap) {
    return (path_store){.text = text_empty(item_heap),
                        .paths = heap_array_empty(item_heap, sizeof(stored_path))};
}

static void path_store_release(path_store *store) {
    sxml_function_library_text_release(&store->text);
    heap_array_release(&store->paths);
}

static sxml_status path_store_add(path_store *store, sxml_byte_view path, stored_path details) {
    const size_t start = store->text.byte_count;
    sxml_status status = sxml_function_library_text_append(&store->text, path);
    if (status == SXML_STATUS_OK) {
        details.span = (byte_span){.start = start, .end = store->text.byte_count};
        status = heap_array_push(&store->paths, &details);
    }
    return status;
}

static const stored_path *path_store_at(const path_store *store, size_t path_index) {
    return heap_array_at(&store->paths, path_index);
}

static sxml_byte_view path_store_view(const path_store *store, size_t path_index) {
    return view_span(text_view(&store->text), path_store_at(store, path_index)->span);
}

/* ---------------------------------------------------------------- Access */

/* The host's conventions (EXPath File 4.0, 6), POSIX without a file
   system. */
typedef struct file_conventions {
    uint8_t separator;
    bool is_windows;
    sxml_byte_view directory_separator;
    sxml_byte_view path_separator;
    sxml_byte_view line_separator;
} file_conventions;

static file_conventions conventions_of(sxml_file_system file_system) {
    if (file_system.operations == NULL ||
        file_system.operations->directory_separator.byte_count != 1) {
        return (file_conventions){.separator = '/',
                                  .is_windows = false,
                                  .directory_separator = SXML_BYTE_VIEW_FROM_LITERAL("/"),
                                  .path_separator = SXML_BYTE_VIEW_FROM_LITERAL(":"),
                                  .line_separator = SXML_BYTE_VIEW_FROM_LITERAL("\n")};
    }
    const sxml_file_system_operations *operations = file_system.operations;
    return (file_conventions){.separator = operations->directory_separator.bytes[0],
                              .is_windows = operations->directory_separator.bytes[0] == '\\',
                              .directory_separator = operations->directory_separator,
                              .path_separator = operations->path_separator,
                              .line_separator = operations->line_separator};
}

/* What a call reaches files with. */
typedef struct file_access {
    const sxml_function_call *call;
    sxml_file_system file_system;
    file_conventions conventions;
} file_access;

/* The processor's file system, or the null one without an evaluation
   environment. */
static sxml_file_system file_system_of(const sxml_function_call *call) {
    const sxml_evaluation_environment *environment =
        sxml_processor_get_evaluation_environment(call->dynamic_context);
    if (environment == NULL || environment->processor == NULL) {
        return (sxml_file_system){.state = NULL, .operations = NULL};
    }
    return environment->processor->file_system;
}

static file_access access_of(const sxml_function_call *call) {
    const sxml_file_system file_system = file_system_of(call);
    return (file_access){
        .call = call, .file_system = file_system, .conventions = conventions_of(file_system)};
}

/* file:io-error when there is no file system. */
static sxml_status require_file_system(const file_access *access) {
    return access->file_system.operations == NULL ? report(access->call, &no_file_system_error)
                                                  : SXML_STATUS_OK;
}

/* A host failure is file:io-error; other failures pass. */
static sxml_status host_result(const file_access *access, sxml_status status) {
    return status == SXML_STATUS_INPUT_OUTPUT_FAILED ? report(access->call, &io_error) : status;
}

static sxml_status describe(const file_access *access, sxml_byte_view path,
                            sxml_file_description *description) {
    *description = (sxml_file_description){.file_kind = SXML_FILE_KIND_NONE,
                                           .byte_count = 0,
                                           .modified_seconds = 0,
                                           .modified_nanoseconds = 0};
    return host_result(access, access->file_system.operations->describe(
                                   access->file_system.state, path, description));
}

/* ----------------------------------------------------------------- Paths */

static bool is_separator(file_conventions conventions, uint8_t byte) {
    return byte == '/' || (conventions.is_windows && byte == '\\');
}

static bool is_ascii_letter(uint8_t byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

/* The byte count of the root of an absolute path, 0 for a relative one:
   "/" on POSIX; "C:\" and "\\" (UNC) on Windows. */
static size_t root_length(file_conventions conventions, sxml_byte_view path) {
    if (!conventions.is_windows) {
        return path.byte_count > 0 && path.bytes[0] == '/' ? 1 : 0;
    }
    if (path.byte_count >= 3 && is_ascii_letter(path.bytes[0]) && path.bytes[1] == ':' &&
        is_separator(conventions, path.bytes[2])) {
        return 3;
    }
    if (path.byte_count >= 2 && is_separator(conventions, path.bytes[0]) &&
        is_separator(conventions, path.bytes[1])) {
        return 2;
    }
    return 0;
}

static bool has_file_scheme(sxml_byte_view path) {
    static const char scheme[] = "file:";
    if (path.byte_count < sizeof(scheme) - 1) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < sizeof(scheme) - 1; byte_index += 1) {
        uint8_t byte = path.bytes[byte_index];
        if (byte >= 'A' && byte <= 'Z') {
            byte = (uint8_t)(byte - 'A' + 'a');
        }
        if (byte != (uint8_t)scheme[byte_index]) {
            return false;
        }
    }
    return true;
}

static int hex_digit_value(uint8_t byte) {
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

/* The path of a file: URI (RFC 8089): the authority (empty or localhost)
   dropped, %XX decoded; on Windows "/C:/" becomes "C:/". Appended to
   *path. */
static sxml_status append_uri_path(file_conventions conventions, sxml_byte_view uri,
                                   sxml_library_text *path) {
    size_t position = 5;
    if (uri.byte_count >= position + 2 && uri.bytes[position] == '/' &&
        uri.bytes[position + 1] == '/') {
        position += 2;
        while (position < uri.byte_count && uri.bytes[position] != '/') {
            position += 1;
        }
    }
    if (conventions.is_windows && uri.byte_count >= position + 3 && uri.bytes[position] == '/' &&
        is_ascii_letter(uri.bytes[position + 1]) && uri.bytes[position + 2] == ':') {
        position += 1;
    }
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && position < uri.byte_count) {
        uint8_t byte = uri.bytes[position];
        if (byte == '%' && position + 2 < uri.byte_count &&
            hex_digit_value(uri.bytes[position + 1]) >= 0 &&
            hex_digit_value(uri.bytes[position + 2]) >= 0) {
            byte = (uint8_t)(hex_digit_value(uri.bytes[position + 1]) * 16 +
                             hex_digit_value(uri.bytes[position + 2]));
            position += 2;
        }
        status = sxml_function_library_text_append(
            path, (sxml_byte_view){.bytes = &byte, .byte_count = 1});
        position += 1;
    }
    return status;
}

/* Appends the segments of path (after its root) to *output, whose first
   root_byte_count bytes are the root: "" and "." are dropped, ".."
   removes the last segment (never the root). */
typedef struct segment_source {
    sxml_byte_view path;
    size_t root_byte_count;
} segment_source;

static sxml_status append_segments(file_conventions conventions, segment_source source,
                                   sxml_library_text *output, size_t root_byte_count) {
    const sxml_byte_view path = source.path;
    size_t position = source.root_byte_count;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && position < path.byte_count) {
        size_t end = position;
        while (end < path.byte_count && !is_separator(conventions, path.bytes[end])) {
            end += 1;
        }
        const sxml_byte_view segment = view_span(path, (byte_span){.start = position, .end = end});
        if (segment.byte_count == 2 && segment.bytes[0] == '.' && segment.bytes[1] == '.') {
            size_t kept = output->byte_count;
            while (kept > root_byte_count && output->bytes[kept - 1] != conventions.separator) {
                kept -= 1;
            }
            if (kept > root_byte_count) {
                kept -= 1;
            }
            output->byte_count = kept;
        } else if (segment.byte_count > 0 &&
                   !(segment.byte_count == 1 && segment.bytes[0] == '.')) {
            if (output->byte_count > root_byte_count) {
                status = sxml_function_library_text_append(output, conventions.directory_separator);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_function_library_text_append(output, segment);
            }
        }
        position = end + 1;
    }
    return status;
}

/* Appends the root of an absolute path in the host's form. */
static sxml_status append_root(file_conventions conventions, sxml_byte_view root,
                               sxml_library_text *output) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index < root.byte_count;
         byte_index += 1) {
        const uint8_t byte =
            is_separator(conventions, root.bytes[byte_index]) ? conventions.separator
                                                               : root.bytes[byte_index];
        status = sxml_function_library_text_append(
            output, (sxml_byte_view){.bytes = &byte, .byte_count = 1});
    }
    return status;
}

/* The path of a query's path text: a file: URI's path, or the text. */
typedef struct plain_path {
    sxml_byte_view path;
    /* Holds the decoded URI path; release it. */
    sxml_library_text decoded;
} plain_path;

static sxml_status make_plain_path(const file_access *access, sxml_byte_view input,
                                   plain_path *plain) {
    plain->decoded = text_empty(access->call->item_heap);
    plain->path = input;
    if (!has_file_scheme(input)) {
        return SXML_STATUS_OK;
    }
    const sxml_status status = append_uri_path(access->conventions, input, &plain->decoded);
    plain->path = text_view(&plain->decoded);
    return status;
}

/*
 * Writes the absolute, normalized path of input (a path or a file: URI)
 * to *output, emptied first: a relative path is resolved against the
 * CURRENT directory of the file system (EXPath File 4.0, 1.2). No trailing
 * separator but for a root. Needs a file system for a relative path.
 */
static sxml_status make_absolute(const file_access *access, sxml_byte_view input,
                                 sxml_library_text *output) {
    output->byte_count = 0;
    plain_path plain;
    sxml_status status = make_plain_path(access, input, &plain);
    const file_conventions conventions = access->conventions;
    size_t root = root_length(conventions, plain.path);
    if (status == SXML_STATUS_OK && root == 0) {
        status = require_file_system(access);
        sxml_byte_view current = {.bytes = NULL, .byte_count = 0};
        if (status == SXML_STATUS_OK) {
            status = host_result(access, access->file_system.operations->get_directory(
                                             access->file_system.state,
                                             SXML_FILE_DIRECTORY_KIND_CURRENT, &current));
        }
        const size_t current_root = root_length(conventions, current);
        if (status == SXML_STATUS_OK && current_root == 0) {
            status = report(access->call, &io_error);
        }
        if (status == SXML_STATUS_OK) {
            status = append_root(conventions,
                                 view_span(current, (byte_span){.start = 0, .end = current_root}),
                                 output);
        }
        if (status == SXML_STATUS_OK) {
            status = append_segments(
                conventions, (segment_source){.path = current, .root_byte_count = current_root},
                output, current_root);
        }
        root = 0;
    } else if (status == SXML_STATUS_OK) {
        status = append_root(conventions,
                             view_span(plain.path, (byte_span){.start = 0, .end = root}), output);
    }
    if (status == SXML_STATUS_OK) {
        const size_t root_byte_count = root == 0 ? root_length(conventions, text_view(output))
                                                 : root;
        status = append_segments(conventions,
                                 (segment_source){.path = plain.path, .root_byte_count = root},
                                 output, root_byte_count);
    }
    sxml_function_library_text_release(&plain.decoded);
    return status;
}

static bool ends_with_separator(file_conventions conventions, sxml_byte_view path) {
    return path.byte_count > 0 && is_separator(conventions, path.bytes[path.byte_count - 1]);
}

/* Appends a separator unless text ends with one. */
static sxml_status append_separator(file_conventions conventions, sxml_library_text *text) {
    if (ends_with_separator(conventions, text_view(text))) {
        return SXML_STATUS_OK;
    }
    return sxml_function_library_text_append(text, conventions.directory_separator);
}

/* Appends separator and name to text (a separator only when needed). */
static sxml_status append_name(file_conventions conventions, sxml_library_text *text,
                               sxml_byte_view name) {
    sxml_status status = append_separator(conventions, text);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(text, name);
    }
    return status;
}

/* The span of the last segment of a normalized absolute path; empty for a
   root. */
static byte_span last_segment(file_conventions conventions, sxml_byte_view path) {
    const size_t root = root_length(conventions, path);
    size_t start = path.byte_count;
    while (start > root && !is_separator(conventions, path.bytes[start - 1])) {
        start -= 1;
    }
    return (byte_span){.start = start, .end = path.byte_count};
}

/* The byte count of the parent of a normalized absolute path, or 0 for a
   root. */
static size_t parent_length(file_conventions conventions, sxml_byte_view path) {
    const size_t root = root_length(conventions, path);
    if (path.byte_count <= root) {
        return 0;
    }
    const size_t start = last_segment(conventions, path).start;
    return start > root ? start - 1 : root;
}

/* Copies the text into a new text of the same heap. */
static sxml_status copy_text(sxml_byte_view source, sxml_library_text *copy) {
    copy->byte_count = 0;
    return sxml_function_library_text_append(copy, source);
}

/* Whether path lies inside directory (both normalized absolute). */
static bool is_inside(file_conventions conventions, sxml_byte_view path, sxml_byte_view directory) {
    if (path.byte_count <= directory.byte_count ||
        memcmp(path.bytes, directory.bytes, directory.byte_count) != 0) {
        return false;
    }
    return is_separator(conventions, path.bytes[directory.byte_count]) ||
           ends_with_separator(conventions, directory);
}

/* -------------------------------------------------------------- Arguments */

static sxml_status read_string(const sxml_function_call *call, size_t argument_index,
                               sxml_string *string) {
    return sxml_function_library_read_string_argument(call, argument_index, string);
}

/* An optional argument: present when given and not the empty sequence. */
typedef struct optional_string {
    bool has_value;
    sxml_string string;
} optional_string;

static sxml_status read_optional_string(const sxml_function_call *call, size_t argument_index,
                                        optional_string *argument) {
    argument->has_value = false;
    argument->string =
        (sxml_string){.utf8 = {.bytes = NULL, .byte_count = 0}, .codepoint_count = 0, .is_ascii = true};
    sxml_optional_item optional;
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &optional);
    if (status == SXML_STATUS_OK && optional.has_item) {
        argument->has_value = true;
        status = read_string(call, argument_index, &argument->string);
    }
    return status;
}

/* An xs:boolean? argument (default-on-empty false). */
static sxml_status read_boolean(const sxml_function_call *call, size_t argument_index,
                                bool *value) {
    *value = false;
    sxml_optional_item optional;
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &optional);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK && optional.has_item) {
        status = sxml_atomic_item_get_datum(optional.item, &datum);
        if (status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_BOOLEAN) {
            *value = datum.datum_details.boolean;
        }
    }
    return status;
}

/* An xs:integer? argument: absent, an int64_t, or beyond it (negative or
   not), which every offset treats as out of range. */
typedef struct optional_integer {
    bool has_value;
    bool is_inline;
    int64_t value;
    bool is_negative;
} optional_integer;

static sxml_status read_integer(const sxml_function_call *call, size_t argument_index,
                                optional_integer *argument) {
    *argument = (optional_integer){
        .has_value = false, .is_inline = true, .value = 0, .is_negative = false};
    sxml_optional_item optional;
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &optional);
    sxml_atomic_datum datum;
    if (status != SXML_STATUS_OK || !optional.has_item) {
        return status;
    }
    status = sxml_atomic_item_get_datum(optional.item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    argument->has_value = true;
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        argument->value = datum.datum_details.integer;
        argument->is_negative = argument->value < 0;
    } else {
        argument->is_inline = false;
        argument->is_negative = datum.datum_details.decimal_text.byte_count > 0 &&
                                datum.datum_details.decimal_text.bytes[0] == '-';
    }
    return SXML_STATUS_OK;
}

/* Reads a path argument and makes it absolute into *absolute. */
static sxml_status read_absolute(const file_access *access, size_t argument_index,
                                 sxml_library_text *absolute) {
    sxml_string path;
    sxml_status status = read_string(access->call, argument_index, &path);
    if (status == SXML_STATUS_OK) {
        status = make_absolute(access, path.utf8, absolute);
    }
    return status;
}

/* --------------------------------------------------------------- Results */

static sxml_status make_text_result(const sxml_function_call *call, sxml_byte_view text,
                                    sxml_sequence *result) {
    return sxml_function_library_make_string(call->item_heap, text, result);
}

static sxml_sequence boolean_result(bool is_true) {
    return sxml_function_library_make_boolean((sxml_boolean_result){.is_true = is_true});
}

/* Collects string items. */
typedef struct string_results {
    sxml_sequence_builder *builder;
} string_results;

static sxml_status string_results_start(const sxml_function_call *call, string_results *results) {
    results->builder = NULL;
    return sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &results->builder);
}

static sxml_status string_results_add(const sxml_function_call *call, string_results *results,
                                      sxml_byte_view text) {
    sxml_sequence item = sxml_sequence_empty();
    sxml_status status = make_text_result(call, text, &item);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_append_sequence(results->builder, item, NULL);
        sxml_sequence_release(item);
    }
    return status;
}

static sxml_status string_results_finish(string_results *results, sxml_status status,
                                         sxml_sequence *result) {
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(results->builder, result);
    }
    sxml_sequence_builder_destroy(results->builder);
    results->builder = NULL;
    return status;
}

/* ------------------------------------------------------------- Listings */

/* The entries of one directory, names back to back. */
typedef struct entry_list {
    path_store names;
} entry_list;

static sxml_status entry_list_add(void *state, const sxml_file_entry *entry) {
    entry_list *list = state;
    return path_store_add(&list->names, entry->name,
                          (stored_path){.span = {.start = 0, .end = 0},
                                        .file_kind = entry->file_kind,
                                        .depth = 0});
}

/* Lists the directory at path (empty: the roots) into *list, emptied
   first. */
static sxml_status list_directory(const file_access *access, sxml_byte_view path,
                                  entry_list *list) {
    list->names.text.byte_count = 0;
    list->names.paths.element_count = 0;
    return host_result(
        access, access->file_system.operations->list(
                    access->file_system.state,
                    &(sxml_file_list_request){
                        .path = path, .sink = {.state = list, .add = entry_list_add}}));
}

/* Checks that path names a directory: file:not-found or file:no-dir. */
static sxml_status require_directory(const file_access *access, sxml_byte_view path) {
    sxml_file_description description;
    sxml_status status = describe(access, path, &description);
    if (status == SXML_STATUS_OK && description.file_kind == SXML_FILE_KIND_NONE) {
        status = report(access->call, &not_found_error);
    } else if (status == SXML_STATUS_OK && description.file_kind != SXML_FILE_KIND_DIRECTORY) {
        status = report(access->call, &no_dir_error);
    }
    return status;
}

/* A walk over every entry below a directory, breadth first: each entry's
   absolute path, kind and depth (0 for the directory's own entries). */
typedef struct directory_walk {
    path_store found;
    entry_list entries;
    sxml_library_text scratch;
} directory_walk;

static directory_walk directory_walk_empty(sxml_item_heap *item_heap) {
    return (directory_walk){.found = path_store_empty(item_heap),
                            .entries = {.names = path_store_empty(item_heap)},
                            .scratch = text_empty(item_heap)};
}

static void directory_walk_release(directory_walk *walk) {
    path_store_release(&walk->found);
    path_store_release(&walk->entries.names);
    sxml_function_library_text_release(&walk->scratch);
}

/* Lists the entries of directory (an absolute path, at depth) into the
   walk's found paths. */
typedef struct walk_step {
    sxml_byte_view directory;
    size_t depth;
} walk_step;

static sxml_status walk_directory(const file_access *access, directory_walk *walk,
                                  walk_step step) {
    sxml_status status = copy_text(step.directory, &walk->scratch);
    if (status == SXML_STATUS_OK) {
        status = list_directory(access, text_view(&walk->scratch), &walk->entries);
    }
    const size_t directory_byte_count = walk->scratch.byte_count;
    for (size_t entry_index = 0;
         status == SXML_STATUS_OK && entry_index < walk->entries.names.paths.element_count;
         entry_index += 1) {
        walk->scratch.byte_count = directory_byte_count;
        status = append_name(access->conventions, &walk->scratch,
                             path_store_view(&walk->entries.names, entry_index));
        if (status == SXML_STATUS_OK) {
            status = path_store_add(
                &walk->found, text_view(&walk->scratch),
                (stored_path){.span = {.start = 0, .end = 0},
                              .file_kind = path_store_at(&walk->entries.names, entry_index)->file_kind,
                              .depth = step.depth});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_charge_steps(access->call, 1);
        }
    }
    return status;
}

/* Walks everything below directory into walk->found. */
static sxml_status walk_everything(const file_access *access, directory_walk *walk,
                                   sxml_byte_view directory) {
    sxml_status status =
        walk_directory(access, walk, (walk_step){.directory = directory, .depth = 0});
    for (size_t found_index = 0;
         status == SXML_STATUS_OK && found_index < walk->found.paths.element_count;
         found_index += 1) {
        const stored_path *found = path_store_at(&walk->found, found_index);
        if (found->file_kind != SXML_FILE_KIND_DIRECTORY) {
            continue;
        }
        sxml_library_text directory_path = text_empty(access->call->item_heap);
        status = copy_text(path_store_view(&walk->found, found_index), &directory_path);
        const size_t depth = found->depth + 1;
        if (status == SXML_STATUS_OK) {
            status = walk_directory(access, walk,
                                    (walk_step){.directory = text_view(&directory_path),
                                                .depth = depth});
        }
        sxml_function_library_text_release(&directory_path);
    }
    return status;
}

/* --------------------------------------------------------- File properties */

/* file:name($path) as xs:string (2.1): the last segment, lexically. */
static sxml_status call_name(const sxml_function_call *call, sxml_sequence *result) {
    const file_access access = access_of(call);
    sxml_string path;
    sxml_status status = read_string(call, 0, &path);
    plain_path plain = {.path = {.bytes = NULL, .byte_count = 0}, .decoded = text_empty(call->item_heap)};
    if (status == SXML_STATUS_OK) {
        status = make_plain_path(&access, path.utf8, &plain);
    }
    if (status == SXML_STATUS_OK) {
        sxml_byte_view name = plain.path;
        const size_t root = root_length(access.conventions, name);
        size_t end = name.byte_count;
        while (end > root && is_separator(access.conventions, name.bytes[end - 1])) {
            end -= 1;
        }
        size_t start = end;
        while (start > root && !is_separator(access.conventions, name.bytes[start - 1])) {
            start -= 1;
        }
        name = end <= root ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                           : view_span(name, (byte_span){.start = start, .end = end});
        status = make_text_result(call, name, result);
    }
    sxml_function_library_text_release(&plain.decoded);
    return status;
}

/* file:is-absolute($path) as xs:boolean (2.7). */
static sxml_status call_is_absolute(const sxml_function_call *call, sxml_sequence *result) {
    const file_access access = access_of(call);
    sxml_string path;
    const sxml_status status = read_string(call, 0, &path);
    if (status == SXML_STATUS_OK) {
        *result = boolean_result(has_file_scheme(path.utf8) ||
                                 root_length(access.conventions, path.utf8) > 0);
    }
    return status;
}

/* What a path argument names, for file:exists, file:is-dir, file:is-file:
   NONE when it cannot be told (2.4). */
static sxml_status kind_of_argument(const sxml_function_call *call, sxml_file_kind *kind) {
    *kind = SXML_FILE_KIND_NONE;
    const file_access access = access_of(call);
    if (access.file_system.operations == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_library_text absolute = text_empty(call->item_heap);
    sxml_status status = read_absolute(&access, 0, &absolute);
    sxml_file_description description;
    if (status == SXML_STATUS_OK) {
        status = access.file_system.operations->describe(access.file_system.state,
                                                         text_view(&absolute), &description);
        if (status == SXML_STATUS_OK) {
            *kind = description.file_kind;
        } else if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
            status = SXML_STATUS_OK;
        }
    } else if (status == SXML_STATUS_DYNAMIC_ERROR) {
        /* A path that cannot be resolved names nothing that exists. */
        status = SXML_STATUS_OK;
    }
    sxml_function_library_text_release(&absolute);
    return status;
}

static sxml_status call_exists(const sxml_function_call *call, sxml_sequence *result) {
    sxml_file_kind kind;
    const sxml_status status = kind_of_argument(call, &kind);
    if (status == SXML_STATUS_OK) {
        *result = boolean_result(kind != SXML_FILE_KIND_NONE);
    }
    return status;
}

static sxml_status call_is_dir(const sxml_function_call *call, sxml_sequence *result) {
    sxml_file_kind kind;
    const sxml_status status = kind_of_argument(call, &kind);
    if (status == SXML_STATUS_OK) {
        *result = boolean_result(kind == SXML_FILE_KIND_DIRECTORY);
    }
    return status;
}

static sxml_status call_is_file(const sxml_function_call *call, sxml_sequence *result) {
    sxml_file_kind kind;
    const sxml_status status = kind_of_argument(call, &kind);
    if (status == SXML_STATUS_OK) {
        *result = boolean_result(kind == SXML_FILE_KIND_FILE);
    }
    return status;
}

/* Opens access and reads the absolute path of argument 0 and what it
   names; file:not-found when it names nothing. */
typedef struct existing_path {
    sxml_library_text absolute;
    sxml_file_description description;
} existing_path;

static sxml_status read_existing(const file_access *access, existing_path *existing) {
    existing->absolute = text_empty(access->call->item_heap);
    sxml_status status = require_file_system(access);
    if (status == SXML_STATUS_OK) {
        status = read_absolute(access, 0, &existing->absolute);
    }
    if (status == SXML_STATUS_OK) {
        status = describe(access, text_view(&existing->absolute), &existing->description);
    }
    if (status == SXML_STATUS_OK && existing->description.file_kind == SXML_FILE_KIND_NONE) {
        status = report(access->call, &not_found_error);
    }
    return status;
}

/* file:last-modified($path) as xs:dateTime (2.2), in UTC. */
static sxml_status call_last_modified(const sxml_function_call *call, sxml_sequence *result) {
    const file_access access = access_of(call);
    existing_path existing;
    sxml_status status = read_existing(&access, &existing);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DATE_TIME),
                .datum = {.datum_kind = SXML_DATUM_KIND_DATE_TIME,
                          .datum_details = {.date_time = sxml_date_time_from_unix_time(
                                                (sxml_unix_time){
                                                    .seconds = existing.description.modified_seconds,
                                                    .nanoseconds = existing.description
                                                                       .modified_nanoseconds})}}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    sxml_function_library_text_release(&existing.absolute);
    return status;
}

/* file:size($path, $recursive := false()) as xs:integer (2.3). */
static sxml_status call_size(const sxml_function_call *call, sxml_sequence *result) {
    const file_access access = access_of(call);
    existing_path existing;
    sxml_status status = read_existing(&access, &existing);
    bool is_recursive = false;
    if (status == SXML_STATUS_OK) {
        status = read_boolean(call, 1, &is_recursive);
    }
    uint64_t total = 0;
    if (status == SXML_STATUS_OK && existing.description.file_kind == SXML_FILE_KIND_FILE) {
        total = existing.description.byte_count;
    } else if (status == SXML_STATUS_OK &&
               existing.description.file_kind == SXML_FILE_KIND_DIRECTORY && is_recursive) {
        directory_walk walk = directory_walk_empty(call->item_heap);
        status = walk_everything(&access, &walk, text_view(&existing.absolute));
        for (size_t found_index = 0;
             status == SXML_STATUS_OK && found_index < walk.found.paths.element_count;
             found_index += 1) {
            if (path_store_at(&walk.found, found_index)->file_kind != SXML_FILE_KIND_FILE) {
                continue;
            }
            sxml_file_description description;
            status = describe(&access, path_store_view(&walk.found, found_index), &description);
            total += description.byte_count;
        }
        directory_walk_release(&walk);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(sxml_atomic_item_from_integer(
            total > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)total));
    }
    sxml_function_library_text_release(&existing.absolute);
    return status;
}

/* ------------------------------------------ Manipulation of directories */

/* Creates path and its missing parents (3.2): file:exists when one of them
   is not a directory. */
static sxml_status create_directories(const file_access *access, sxml_byte_view path) {
    const size_t root = root_length(access->conventions, path);
    sxml_status status = SXML_STATUS_OK;
    sxml_library_text prefix = text_empty(access->call->item_heap);
    for (size_t end = root; status == SXML_STATUS_OK && end <= path.byte_count; end += 1) {
        if (end < path.byte_count && !is_separator(access->conventions, path.bytes[end])) {
            continue;
        }
        if (end == root) {
            continue;
        }
        status = copy_text(view_span(path, (byte_span){.start = 0, .end = end}), &prefix);
        sxml_file_description description;
        if (status == SXML_STATUS_OK) {
            status = describe(access, text_view(&prefix), &description);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (description.file_kind == SXML_FILE_KIND_NONE) {
            status = host_result(access, access->file_system.operations->create_directory(
                                             access->file_system.state, text_view(&prefix)));
        } else if (description.file_kind != SXML_FILE_KIND_DIRECTORY) {
            status = report(access->call, &exists_error);
        }
    }
    sxml_function_library_text_release(&prefix);
    return status;
}

/* file:create-dir($dir) (3.2). */
static sxml_status call_create_dir(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    const file_access access = access_of(call);
    sxml_library_text absolute = text_empty(call->item_heap);
    sxml_status status = require_file_system(&access);
    if (status == SXML_STATUS_OK) {
        status = read_absolute(&access, 0, &absolute);
    }
    if (status == SXML_STATUS_OK) {
        status = create_directories(&access, text_view(&absolute));
    }
    sxml_function_library_text_release(&absolute);
    return status;
}

/* file:create-temp-file and file:create-temp-dir ($prefix, $suffix, $dir)
   (3.3, 3.4): in $dir, which must be a directory (file:no-dir), or in the
   temporary-file directory. */
static sxml_status create_temporary(const sxml_function_call *call, bool is_directory,
                                    sxml_sequence *result) {
    const file_access access = access_of(call);
    optional_string prefix;
    optional_string suffix;
    optional_string directory;
    sxml_status status = require_file_system(&access);
    if (status == SXML_STATUS_OK) {
        status = read_optional_string(call, 0, &prefix);
    }
    if (status == SXML_STATUS_OK) {
        status = read_optional_string(call, 1, &suffix);
    }
    if (status == SXML_STATUS_OK) {
        status = read_optional_string(call, 2, &directory);
    }
    sxml_library_text absolute = text_empty(call->item_heap);
    if (status == SXML_STATUS_OK && directory.has_value) {
        status = make_absolute(&access, directory.string.utf8, &absolute);
        sxml_file_description description;
        if (status == SXML_STATUS_OK) {
            status = describe(&access, text_view(&absolute), &description);
        }
        if (status == SXML_STATUS_OK && description.file_kind != SXML_FILE_KIND_DIRECTORY) {
            status = report(call, &no_dir_error);
        }
    } else if (status == SXML_STATUS_OK) {
        sxml_byte_view temporary = {.bytes = NULL, .byte_count = 0};
        status = host_result(&access, access.file_system.operations->get_directory(
                                          access.file_system.state,
                                          SXML_FILE_DIRECTORY_KIND_TEMPORARY, &temporary));
        if (status == SXML_STATUS_OK) {
            status = make_absolute(&access, temporary, &absolute);
        }
    }
    sxml_byte_view created = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = host_result(
            &access, access.file_system.operations->create_temporary(
                         access.file_system.state,
                         &(sxml_file_temporary_request){.directory = text_view(&absolute),
                                                        .prefix = prefix.string.utf8,
                                                        .suffix = suffix.string.utf8,
                                                        .is_directory = is_directory},
                         &created));
    }
    if (status == SXML_STATUS_OK) {
        status = copy_text(created, &absolute);
    }
    if (status == SXML_STATUS_OK && is_directory) {
        status = append_separator(access.conventions, &absolute);
    }
    if (status == SXML_STATUS_OK) {
        status = make_text_result(call, text_view(&absolute), result);
    }
    sxml_function_library_text_release(&absolute);
    return status;
}

static sxml_status call_create_temp_dir(const sxml_function_call *call, sxml_sequence *result) {
    return create_temporary(call, true, result);
}

static sxml_status call_create_temp_file(const sxml_function_call *call, sxml_sequence *result) {
    return create_temporary(call, false, result);
}

static sxml_status remove_path(const file_access *access, sxml_byte_view path) {
    return host_result(access, access->file_system.operations->remove(access->file_system.state,
                                                                      path));
}

/* file:delete($path, $recursive := false()) (3.5): nothing for a path
   that does not exist; file:is-dir for a non-empty directory unless
   recursive, which removes what the walk found, deepest first. */
static sxml_status call_delete(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    const file_access access = access_of(call);
    sxml_library_text absolute = text_empty(call->item_heap);
    sxml_status status = require_file_system(&access);
    if (status == SXML_STATUS_OK) {
        status = read_absolute(&access, 0, &absolute);
    }
    bool is_recursive = false;
    if (status == SXML_STATUS_OK) {
        status = read_boolean(call, 1, &is_recursive);
    }
    sxml_file_description description;
    if (status == SXML_STATUS_OK) {
        status = describe(&access, text_view(&absolute), &description);
    }
    if (status == SXML_STATUS_OK && description.file_kind == SXML_FILE_KIND_DIRECTORY) {
        directory_walk walk = directory_walk_empty(call->item_heap);
        status = walk_directory(&access, &walk,
                                (walk_step){.directory = text_view(&absolute), .depth = 0});
        if (status == SXML_STATUS_OK && walk.found.paths.element_count > 0 && !is_recursive) {
            status = report(call, &is_dir_error);
        }
        if (status == SXML_STATUS_OK && is_recursive) {
            directory_walk_release(&walk);
            walk = directory_walk_empty(call->item_heap);
            status = walk_everything(&access, &walk, text_view(&absolute));
        }
        for (size_t found_index = walk.found.paths.element_count;
             status == SXML_STATUS_OK && found_index > 0; found_index -= 1) {
            status = remove_path(&access, path_store_view(&walk.found, found_index - 1));
        }
        directory_walk_release(&walk);
    }
    if (status == SXML_STATUS_OK && description.file_kind != SXML_FILE_KIND_NONE) {
        status = remove_path(&access, text_view(&absolute));
    }
    sxml_function_library_text_release(&absolute);
    return status;
}

/* --------------------------------------------------------- File content */

/* Octets read from a file, in item heap memory. */
typedef struct file_content {
    sxml_item_heap *item_heap;
    uint8_t *bytes;
    size_t byte_count;
} file_content;

static void file_content_release(file_content *content) {
    if (content->bytes != NULL) {
        sxml_item_heap_release_bytes(content->item_heap,
                                     (sxml_heap_bytes){.memory = content->bytes,
                                                       .byte_count = content->byte_count});
    }
    content->bytes = NULL;
    content->byte_count = 0;
}

static sxml_byte_view file_content_view(const file_content *content) {
    return (sxml_byte_view){.bytes = content->bytes, .byte_count = content->byte_count};
}

/* A span of a file to read. */
typedef struct read_span {
    sxml_byte_view path;
    uint64_t offset;
    uint64_t byte_count;
} read_span;

static sxml_status read_file(const file_access *access, read_span span, file_content *content) {
    *content = (file_content){.item_heap = access->call->item_heap, .bytes = NULL, .byte_count = 0};
    if (span.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    if (span.byte_count > (uint64_t)SIZE_MAX) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_status status =
        sxml_function_library_charge_steps(access->call, span.byte_count / 64U + 1U);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_bytes(
            access->call->item_heap,
            (sxml_allocation_request){.byte_count = (size_t)span.byte_count, .alignment = 1},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    content->bytes = memory;
    content->byte_count = (size_t)span.byte_count;
    status = host_result(
        access, access->file_system.operations->read(
                    access->file_system.state,
                    &(sxml_file_read_request){
                        .path = span.path,
                        .offset = span.offset,
                        .memory = {.bytes = memory, .byte_count = content->byte_count}}));
    if (status != SXML_STATUS_OK) {
        file_content_release(content);
    }
    return status;
}

static sxml_status write_file(const file_access *access, const sxml_file_write_request *request) {
    sxml_status status = sxml_function_library_charge_steps(
        access->call, (uint64_t)request->content.byte_count / 64U + 1U);
    if (status == SXML_STATUS_OK) {
        status = host_result(access, access->file_system.operations->write(
                                         access->file_system.state, request));
    }
    return status;
}

/* Copies the file at source to target (replacing it). */
typedef struct copy_request {
    sxml_byte_view source;
    sxml_byte_view target;
    uint64_t byte_count;
} copy_request;

static sxml_status copy_file(const file_access *access, copy_request request) {
    file_content content;
    sxml_status status = read_file(
        access, (read_span){.path = request.source, .offset = 0, .byte_count = request.byte_count},
        &content);
    if (status == SXML_STATUS_OK) {
        status = write_file(access, &(sxml_file_write_request){
                                        .path = request.target,
                                        .content = file_content_view(&content),
                                        .write_mode = SXML_FILE_WRITE_MODE_REPLACE,
                                        .offset = 0});
    }
    file_content_release(&content);
    return status;
}

/* The two paths of file:copy and file:move, absolute, and what they
   name. */
typedef struct transfer {
    sxml_library_text source;
    sxml_library_text target;
    sxml_file_description source_description;
    sxml_file_description target_description;
    /* target/name(source) when the target is a directory. */
    sxml_library_text destination;
} transfer;

static void transfer_release(transfer *paths) {
    sxml_function_library_text_release(&paths->source);
    sxml_function_library_text_release(&paths->target);
    sxml_function_library_text_release(&paths->destination);
}

/* Reads and checks $source and $target (3.1, 3.6): file:not-found for no
   source, file:exists for a directory onto a file, file:cyclic for a
   target inside the source directory. *is_same is true when both name the
   same path (no effect). */
static sxml_status read_transfer(const file_access *access, transfer *paths, bool *is_same) {
    *is_same = false;
    paths->source = text_empty(access->call->item_heap);
    paths->target = text_empty(access->call->item_heap);
    paths->destination = text_empty(access->call->item_heap);
    sxml_status status = require_file_system(access);
    if (status == SXML_STATUS_OK) {
        status = read_absolute(access, 0, &paths->source);
    }
    if (status == SXML_STATUS_OK) {
        status = read_absolute(access, 1, &paths->target);
    }
    if (status == SXML_STATUS_OK) {
        status = describe(access, text_view(&paths->source), &paths->source_description);
    }
    if (status == SXML_STATUS_OK) {
        status = describe(access, text_view(&paths->target), &paths->target_description);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view source = text_view(&paths->source);
    const sxml_byte_view target = text_view(&paths->target);
    if (paths->source_description.file_kind == SXML_FILE_KIND_NONE) {
        return report(access->call, &not_found_error);
    }
    if (views_equal(source, target)) {
        *is_same = true;
        return SXML_STATUS_OK;
    }
    const bool is_directory = paths->source_description.file_kind == SXML_FILE_KIND_DIRECTORY;
    if (is_directory && paths->target_description.file_kind == SXML_FILE_KIND_FILE) {
        return report(access->call, &exists_error);
    }
    if (is_directory && is_inside(access->conventions, target, source)) {
        return report(access->call, &cyclic_error);
    }
    status = copy_text(target, &paths->destination);
    if (status == SXML_STATUS_OK &&
        paths->target_description.file_kind == SXML_FILE_KIND_DIRECTORY) {
        status = append_name(access->conventions, &paths->destination,
                             view_span(source, last_segment(access->conventions, source)));
    }
    return status;
}

/* Copies the contents of the directory source into the existing directory
   target, merging (3.1): a work list of directory pairs. */
static sxml_status copy_directory_contents(const file_access *access, const transfer *paths) {
    path_store sources = path_store_empty(access->call->item_heap);
    path_store targets = path_store_empty(access->call->item_heap);
    entry_list entries = {.names = path_store_empty(access->call->item_heap)};
    sxml_library_text source = text_empty(access->call->item_heap);
    sxml_library_text target = text_empty(access->call->item_heap);
    const stored_path directory = {.span = {.start = 0, .end = 0},
                                   .file_kind = SXML_FILE_KIND_DIRECTORY,
                                   .depth = 0};
    sxml_status status = path_store_add(&sources, text_view(&paths->source), directory);
    if (status == SXML_STATUS_OK) {
        status = path_store_add(&targets, text_view(&paths->destination), directory);
    }
    for (size_t pair_index = 0; status == SXML_STATUS_OK && pair_index < sources.paths.element_count;
         pair_index += 1) {
        status = copy_text(path_store_view(&sources, pair_index), &source);
        if (status == SXML_STATUS_OK) {
            status = copy_text(path_store_view(&targets, pair_index), &target);
        }
        if (status == SXML_STATUS_OK) {
            status = list_directory(access, text_view(&source), &entries);
        }
        const size_t source_byte_count = source.byte_count;
        const size_t target_byte_count = target.byte_count;
        for (size_t entry_index = 0;
             status == SXML_STATUS_OK && entry_index < entries.names.paths.element_count;
             entry_index += 1) {
            const sxml_byte_view name = path_store_view(&entries.names, entry_index);
            const sxml_file_kind kind = path_store_at(&entries.names, entry_index)->file_kind;
            source.byte_count = source_byte_count;
            target.byte_count = target_byte_count;
            status = append_name(access->conventions, &source, name);
            if (status == SXML_STATUS_OK) {
                status = append_name(access->conventions, &target, name);
            }
            sxml_file_description existing;
            if (status == SXML_STATUS_OK) {
                status = describe(access, text_view(&target), &existing);
            }
            if (status != SXML_STATUS_OK) {
                break;
            }
            if (kind == SXML_FILE_KIND_DIRECTORY) {
                if (existing.file_kind == SXML_FILE_KIND_NONE) {
                    status = host_result(access, access->file_system.operations->create_directory(
                                                     access->file_system.state, text_view(&target)));
                } else if (existing.file_kind != SXML_FILE_KIND_DIRECTORY) {
                    status = report(access->call, &exists_error);
                }
                if (status == SXML_STATUS_OK) {
                    status = path_store_add(&sources, text_view(&source), directory);
                }
                if (status == SXML_STATUS_OK) {
                    status = path_store_add(&targets, text_view(&target), directory);
                }
            } else if (existing.file_kind == SXML_FILE_KIND_DIRECTORY) {
                status = report(access->call, &is_dir_error);
            } else {
                sxml_file_description source_description;
                status = describe(access, text_view(&source), &source_description);
                if (status == SXML_STATUS_OK) {
                    status = copy_file(access,
                                       (copy_request){.source = text_view(&source),
                                                      .target = text_view(&target),
                                                      .byte_count = source_description.byte_count});
                }
            }
        }
    }
    sxml_function_library_text_release(&source);
    sxml_function_library_text_release(&target);
    path_store_release(&entries.names);
    path_store_release(&sources);
    path_store_release(&targets);
    return status;
}

/* file:copy($source, $target) (3.1). */
static sxml_status call_copy(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    const file_access access = access_of(call);
    transfer paths;
    bool is_same = false;
    sxml_status status = read_transfer(&access, &paths, &is_same);
    if (status == SXML_STATUS_OK && !is_same) {
        sxml_file_description destination;
        status = describe(&access, text_view(&paths.destination), &destination);
        if (status == SXML_STATUS_OK &&
            paths.source_description.file_kind == SXML_FILE_KIND_DIRECTORY) {
            if (destination.file_kind == SXML_FILE_KIND_NONE) {
                status = create_directories(&access, text_view(&paths.destination));
            } else if (destination.file_kind != SXML_FILE_KIND_DIRECTORY) {
                status = report(call, &exists_error);
            }
            if (status == SXML_STATUS_OK) {
                status = copy_directory_contents(&access, &paths);
            }
        } else if (status == SXML_STATUS_OK) {
            if (destination.file_kind == SXML_FILE_KIND_DIRECTORY) {
                status = report(call, &is_dir_error);
            } else {
                /* "If $target does not exist, it will be created" (3.1):
                   its directory too. */
                const size_t parent =
                    parent_length(access.conventions, text_view(&paths.destination));
                sxml_library_text parent_path = text_empty(call->item_heap);
                status = copy_text(view_span(text_view(&paths.destination),
                                             (byte_span){.start = 0, .end = parent}),
                                   &parent_path);
                if (status == SXML_STATUS_OK) {
                    status = create_directories(&access, text_view(&parent_path));
                }
                sxml_function_library_text_release(&parent_path);
            }
            if (status == SXML_STATUS_OK) {
                status = copy_file(&access,
                                   (copy_request){.source = text_view(&paths.source),
                                                  .target = text_view(&paths.destination),
                                                  .byte_count = paths.source_description.byte_count});
            }
        }
    }
    transfer_release(&paths);
    return status;
}

/* file:move($source, $target) (3.6). */
static sxml_status call_move(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    const file_access access = access_of(call);
    transfer paths;
    bool is_same = false;
    sxml_status status = read_transfer(&access, &paths, &is_same);
    sxml_file_description destination;
    if (status == SXML_STATUS_OK && !is_same) {
        status = describe(&access, text_view(&paths.destination), &destination);
        if (status == SXML_STATUS_OK && destination.file_kind == SXML_FILE_KIND_DIRECTORY) {
            status = report(call, &is_dir_error);
        } else if (status == SXML_STATUS_OK &&
                   paths.source_description.file_kind == SXML_FILE_KIND_DIRECTORY &&
                   destination.file_kind != SXML_FILE_KIND_NONE) {
            status = report(call, &exists_error);
        }
        if (status == SXML_STATUS_OK) {
            status = host_result(
                &access, access.file_system.operations->move(
                             access.file_system.state,
                             &(sxml_file_move_request){.source = text_view(&paths.source),
                                                       .target = text_view(&paths.destination)}));
        }
    }
    transfer_release(&paths);
    return status;
}

/* -------------------------------------------------------------- Writing */

/* Checks a file to write (4.1 to 4.11): file:is-dir for a directory,
   file:no-dir when its parent is not a directory. */
static sxml_status check_writable(const file_access *access, sxml_byte_view path,
                                  sxml_file_description *description) {
    sxml_status status = describe(access, path, description);
    if (status == SXML_STATUS_OK && description->file_kind == SXML_FILE_KIND_DIRECTORY) {
        return report(access->call, &is_dir_error);
    }
    const size_t parent = parent_length(access->conventions, path);
    sxml_file_description parent_description;
    sxml_library_text parent_path = text_empty(access->call->item_heap);
    if (status == SXML_STATUS_OK) {
        status = copy_text(view_span(path, (byte_span){.start = 0, .end = parent}), &parent_path);
    }
    if (status == SXML_STATUS_OK) {
        status = describe(access, text_view(&parent_path), &parent_description);
    }
    if (status == SXML_STATUS_OK && parent_description.file_kind != SXML_FILE_KIND_DIRECTORY) {
        status = report(access->call, &no_dir_error);
    }
    sxml_function_library_text_release(&parent_path);
    return status;
}

/* A write of octets to the file named by argument 0. */
typedef struct octet_write {
    sxml_byte_view content;
    sxml_file_write_mode write_mode;
} octet_write;

static sxml_status write_octets_to_argument(const file_access *access, octet_write write) {
    sxml_library_text absolute = text_empty(access->call->item_heap);
    sxml_status status = require_file_system(access);
    if (status == SXML_STATUS_OK) {
        status = read_absolute(access, 0, &absolute);
    }
    sxml_file_description description;
    if (status == SXML_STATUS_OK) {
        status = check_writable(access, text_view(&absolute), &description);
    }
    if (status == SXML_STATUS_OK) {
        status = write_file(access, &(sxml_file_write_request){.path = text_view(&absolute),
                                                               .content = write.content,
                                                               .write_mode = write.write_mode,
                                                               .offset = 0});
    }
    sxml_function_library_text_release(&absolute);
    return status;
}

/* file:write and file:append ($file, $value, $options) (4.1, 4.8): the
   value serialized as fn:serialize does, the encoding stage kept. */
static sxml_status write_serialized(const sxml_function_call *call, sxml_file_write_mode mode) {
    const file_access access = access_of(call);
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    sxml_status status = require_file_system(&access);
    sxml_serialization_parameter_reader *reader = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_serialization_parameter_reader_create(
            allocator,
            &(sxml_serialization_parameter_reader_configuration){.allocator = allocator,
                                                                 .item_heap = call->item_heap},
            &reader);
    }
    sxml_serialization_parameters parameters = {.serialization_method =
                                                    SXML_SERIALIZATION_METHOD_XML};
    if (status == SXML_STATUS_OK) {
        status = sxml_serialization_parameter_reader_read(
            reader,
            &(sxml_serialization_parameter_read_request){
                .options = sxml_function_library_get_argument(call, 2),
                .error_record = call->error_record},
            &parameters);
    }
    sxml_output_buffer *output = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            allocator,
            &(sxml_output_buffer_configuration){.allocator = allocator, .maximum_byte_count = 0},
            &output);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            call->item_heap,
            &(sxml_serialization_request){.value = sxml_function_library_get_argument(call, 1),
                                          .serialization_parameters = parameters,
                                          .error_record = call->error_record},
            sxml_output_buffer_as_output_stream(output));
    }
    if (status == SXML_STATUS_OK) {
        status = write_octets_to_argument(
            &access,
            (octet_write){.content = sxml_output_buffer_get_bytes(output), .write_mode = mode});
    }
    sxml_output_buffer_destroy(output);
    sxml_serialization_parameter_reader_destroy(reader);
    return status;
}

static sxml_status call_write(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    return write_serialized(call, SXML_FILE_WRITE_MODE_REPLACE);
}

static sxml_status call_append(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    return write_serialized(call, SXML_FILE_WRITE_MODE_APPEND);
}

/* The encoding named by an optional argument, UTF-8 by default:
   file:unknown-encoding for a name sxml does not support. */
static sxml_status read_encoding(const sxml_function_call *call, size_t argument_index,
                                 sxml_xml_encoding_family *family) {
    optional_string encoding;
    sxml_status status = read_optional_string(call, argument_index, &encoding);
    *family = SXML_XML_ENCODING_FAMILY_UTF8;
    if (status == SXML_STATUS_OK && encoding.has_value) {
        *family = sxml_xml_classify_encoding_name(encoding.string.utf8);
        if (*family == SXML_XML_ENCODING_FAMILY_UNSUPPORTED) {
            status = report(call, &unknown_encoding_error);
        }
    }
    return status;
}

/* Appends text encoded in family to *octets; file:io-error for a character
   the encoding cannot represent. */
typedef struct text_encoding_request {
    sxml_string text;
    sxml_xml_encoding_family family;
} text_encoding_request;

static sxml_status encode_text(const sxml_function_call *call, text_encoding_request request,
                               sxml_library_text *octets) {
    sxml_codepoint_iterator iterator = {.string = request.text, .byte_offset = 0};
    uint32_t codepoint = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        const sxml_text_encoded_codepoint encoded =
            sxml_text_encode_codepoint(request.family, codepoint);
        if (encoded.byte_count == 0) {
            return report(call, &encoding_error);
        }
        status = sxml_function_library_text_append(
            octets, (sxml_byte_view){.bytes = encoded.bytes, .byte_count = encoded.byte_count});
    }
    return status;
}

/* The byte order mark UTF-16 is written with (an implementation-defined
   choice, EXPath File 4.0, 4.3): big-endian, as bin:encode-string's
   UTF-16. */
static sxml_status append_byte_order_mark(sxml_xml_encoding_family family,
                                          sxml_library_text *octets) {
    if (family != SXML_XML_ENCODING_FAMILY_UTF16) {
        return SXML_STATUS_OK;
    }
    return sxml_function_library_text_append(octets, SXML_BYTE_VIEW_FROM_LITERAL("\xFE\xFF"));
}

/* file:write-text, file:append-text ($file, $value, $encoding) (4.3,
   4.10), and with lines the -text-lines functions (4.4, 4.11): each line
   followed by the line separator. */
typedef struct text_write {
    sxml_file_write_mode write_mode;
    bool writes_lines;
} text_write;

static sxml_status write_text(const sxml_function_call *call, text_write write) {
    const file_access access = access_of(call);
    sxml_xml_encoding_family family = SXML_XML_ENCODING_FAMILY_UTF8;
    sxml_status status = require_file_system(&access);
    if (status == SXML_STATUS_OK) {
        status = read_encoding(call, 2, &family);
    }
    sxml_library_text octets = text_empty(call->item_heap);
    if (status == SXML_STATUS_OK) {
        status = append_byte_order_mark(family, &octets);
    }
    if (status == SXML_STATUS_OK && !write.writes_lines) {
        sxml_string text;
        status = read_string(call, 1, &text);
        if (status == SXML_STATUS_OK) {
            status = encode_text(call, (text_encoding_request){.text = text, .family = family},
                                 &octets);
        }
    }
    if (status == SXML_STATUS_OK && write.writes_lines) {
        sxml_string separator;
        status = sxml_string_from_utf8(access.conventions.line_separator, &separator);
        sxml_sequence_iterator iterator =
            sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 1));
        while (status == SXML_STATUS_OK) {
            sxml_iteration_step step = {.has_item = false, .item = {.representation = 0}};
            status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
            if (status != SXML_STATUS_OK || !step.has_item) {
                break;
            }
            sxml_atomic_datum datum;
            status = sxml_atomic_item_get_datum(step.item, &datum);
            if (status == SXML_STATUS_OK) {
                status = encode_text(call,
                                     (text_encoding_request){.text = datum.datum_details.string,
                                                             .family = family},
                                     &octets);
            }
            if (status == SXML_STATUS_OK) {
                status = encode_text(
                    call, (text_encoding_request){.text = separator, .family = family}, &octets);
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        status = write_octets_to_argument(
            &access, (octet_write){.content = text_view(&octets), .write_mode = write.write_mode});
    }
    sxml_function_library_text_release(&octets);
    return status;
}

static sxml_status call_write_text(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    return write_text(call,
                      (text_write){.write_mode = SXML_FILE_WRITE_MODE_REPLACE, .writes_lines = false});
}

static sxml_status call_append_text(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    return write_text(call,
                      (text_write){.write_mode = SXML_FILE_WRITE_MODE_APPEND, .writes_lines = false});
}

static sxml_status call_write_text_lines(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    return write_text(call,
                      (text_write){.write_mode = SXML_FILE_WRITE_MODE_REPLACE, .writes_lines = true});
}

static sxml_status call_append_text_lines(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    return write_text(call,
                      (text_write){.write_mode = SXML_FILE_WRITE_MODE_APPEND, .writes_lines = true});
}

static sxml_status read_binary_argument(const sxml_function_call *call, sxml_byte_view *octets) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 1, &value);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(value.item, &datum);
    }
    if (status == SXML_STATUS_OK) {
        *octets = datum.datum_details.binary;
    }
    return status;
}

/* file:append-binary($file, $value) (4.2). */
static sxml_status call_append_binary(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    const file_access access = access_of(call);
    sxml_byte_view octets = {.bytes = NULL, .byte_count = 0};
    sxml_status status = read_binary_argument(call, &octets);
    if (status == SXML_STATUS_OK) {
        status = write_octets_to_argument(
            &access, (octet_write){.content = octets, .write_mode = SXML_FILE_WRITE_MODE_APPEND});
    }
    return status;
}

/* file:write-binary($file, $value, $offset := 0) (4.9): from $offset when
   it is given (at most the file's size), else the whole file. */
static sxml_status call_write_binary(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    const file_access access = access_of(call);
    sxml_byte_view octets = {.bytes = NULL, .byte_count = 0};
    optional_integer offset;
    sxml_status status = require_file_system(&access);
    if (status == SXML_STATUS_OK) {
        status = read_binary_argument(call, &octets);
    }
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 2, &offset);
    }
    sxml_library_text absolute = text_empty(call->item_heap);
    if (status == SXML_STATUS_OK) {
        status = read_absolute(&access, 0, &absolute);
    }
    sxml_file_description description;
    if (status == SXML_STATUS_OK) {
        status = check_writable(&access, text_view(&absolute), &description);
    }
    if (status == SXML_STATUS_OK && offset.has_value &&
        (offset.is_negative || !offset.is_inline ||
         (uint64_t)offset.value > (description.file_kind == SXML_FILE_KIND_NONE
                                       ? 0U
                                       : description.byte_count))) {
        status = report(call, &out_of_range_error);
    }
    if (status == SXML_STATUS_OK) {
        const bool writes_at_offset = offset.has_value && description.file_kind != SXML_FILE_KIND_NONE;
        status = write_file(&access, &(sxml_file_write_request){
                                         .path = text_view(&absolute),
                                         .content = octets,
                                         .write_mode = writes_at_offset
                                                           ? SXML_FILE_WRITE_MODE_AT_OFFSET
                                                           : SXML_FILE_WRITE_MODE_REPLACE,
                                         .offset = writes_at_offset ? (uint64_t)offset.value : 0U});
    }
    sxml_function_library_text_release(&absolute);
    return status;
}

/* -------------------------------------------------------------- Reading */

/* Reads argument 0 as an existing file (file:not-found, file:is-dir). */
static sxml_status read_existing_file(const file_access *access, existing_path *existing) {
    sxml_status status = read_existing(access, existing);
    if (status == SXML_STATUS_OK && existing->description.file_kind == SXML_FILE_KIND_DIRECTORY) {
        status = report(access->call, &is_dir_error);
    }
    return status;
}

/* file:read-binary($file, $offset := 0, $length := ()) (4.5):
   file:out-of-range for a negative offset or length, or one beyond the
   file. */
static sxml_status call_read_binary(const sxml_function_call *call, sxml_sequence *result) {
    const file_access access = access_of(call);
    existing_path existing;
    sxml_status status = read_existing_file(&access, &existing);
    optional_integer offset;
    optional_integer length;
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 1, &offset);
    }
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 2, &length);
    }
    const uint64_t size = existing.description.byte_count;
    uint64_t start = 0;
    uint64_t byte_count = size;
    if (status == SXML_STATUS_OK) {
        if (offset.has_value &&
            (offset.is_negative || !offset.is_inline || (uint64_t)offset.value > size)) {
            status = report(call, &out_of_range_error);
        } else {
            start = offset.has_value ? (uint64_t)offset.value : 0U;
            byte_count = size - start;
        }
    }
    if (status == SXML_STATUS_OK && length.has_value) {
        if (length.is_negative || !length.is_inline || (uint64_t)length.value > byte_count) {
            status = report(call, &out_of_range_error);
        } else {
            byte_count = (uint64_t)length.value;
        }
    }
    file_content content = {.item_heap = call->item_heap, .bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = read_file(&access,
                           (read_span){.path = text_view(&existing.absolute),
                                       .offset = start,
                                       .byte_count = byte_count},
                           &content);
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_BASE64_BINARY),
                .datum = {.datum_kind = SXML_DATUM_KIND_BINARY,
                          .datum_details = {.binary = file_content_view(&content)}}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    file_content_release(&content);
    sxml_function_library_text_release(&existing.absolute);
    return status;
}

/* The options of file:read-text and file:read-text-lines (4.6, 4.7), by
   position. */
enum {
    SXML_FILE_TEXT_OPTION_ENCODING,
    SXML_FILE_TEXT_OPTION_NORMALIZE_NEWLINES,
    SXML_FILE_TEXT_OPTION_FALLBACK,
};

static const sxml_option_definition text_option_definitions[] = {
    SXML_FUNCTION_OPTION("encoding", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_ZERO_OR_ONE),
    SXML_FUNCTION_OPTION("normalize-newlines", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("fallback", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
};

/* What read-text's options ask for. */
typedef struct text_options {
    sxml_byte_view encoding_name;
    bool normalizes_newlines;
    bool has_fallback;
    /* Owned; the encoding name borrows from it. */
    sxml_option_values option_values;
} text_options;

/* Reads $options (argument 1): a string $S is {"encoding": $S}, a map is
   read by the option parameter conventions; newlines are normalized by
   default (4.6), always for the -lines function (4.7). Release
   options->option_values. */
static sxml_status read_text_options(const sxml_function_call *call, bool is_lines,
                                     text_options *options) {
    memset(options, 0, sizeof(*options));
    options->normalizes_newlines = true;
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 1, &argument);
    if (status != SXML_STATUS_OK || !argument.has_item) {
        return status;
    }
    if (sxml_item_get_kind(argument.item) == SXML_ITEM_KIND_ATOMIC) {
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(argument.item, &datum);
        if (status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_STRING) {
            options->encoding_name = datum.datum_details.string.utf8;
        }
        return status;
    }
    sxml_option_values *values = &options->option_values;
    status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){.argument_index = 1,
                               .definitions = {text_option_definitions,
                                               sizeof(text_option_definitions) /
                                                   sizeof(text_option_definitions[0])}},
        values);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    options->encoding_name =
        sxml_function_library_option_string(
            (sxml_option_lookup){.values = values, .option_index = SXML_FILE_TEXT_OPTION_ENCODING})
            .utf8;
    options->normalizes_newlines =
        is_lines || !sxml_function_library_option_is_false(
                        (sxml_option_lookup){.values = values,
                                             .option_index = SXML_FILE_TEXT_OPTION_NORMALIZE_NEWLINES});
    options->has_fallback = sxml_function_library_option_is_true(
        (sxml_option_lookup){.values = values, .option_index = SXML_FILE_TEXT_OPTION_FALLBACK});
    return SXML_STATUS_OK;
}

/* Reads and decodes the file of argument 0 into *text (4.6): the encoding
   bin:infer-encoding infers, file:unknown-encoding, file:io-error for
   octets or characters that do not decode without fallback. */
static sxml_status read_text_file(const sxml_function_call *call, bool is_lines,
                                  sxml_library_text *text) {
    const file_access access = access_of(call);
    text_options options;
    existing_path existing;
    sxml_status status = read_existing_file(&access, &existing);
    if (status == SXML_STATUS_OK) {
        status = read_text_options(call, is_lines, &options);
    } else {
        memset(&options, 0, sizeof(options));
    }
    if (status == SXML_STATUS_OK && options.encoding_name.byte_count > 0 &&
        sxml_xml_classify_encoding_name(options.encoding_name) ==
            SXML_XML_ENCODING_FAMILY_UNSUPPORTED) {
        status = report(call, &unknown_encoding_error);
    }
    file_content content = {.item_heap = call->item_heap, .bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = read_file(&access,
                           (read_span){.path = text_view(&existing.absolute),
                                       .offset = 0,
                                       .byte_count = existing.description.byte_count},
                           &content);
    }
    sxml_text_decoding decoding;
    if (status == SXML_STATUS_OK &&
        !sxml_text_decoding_choose((sxml_text_decoding_choice){.content = file_content_view(&content),
                                                               .candidate = options.encoding_name},
                                   &decoding)) {
        status = report(call, &unknown_encoding_error);
    }
    sxml_text_decoding_failure failure = SXML_TEXT_DECODING_FAILURE_NONE;
    if (status == SXML_STATUS_OK) {
        status = sxml_text_decode(
            &(sxml_text_decoding_request){.content = file_content_view(&content),
                                          .decoding = decoding,
                                          .normalizes_newlines = options.normalizes_newlines,
                                          .has_fallback = options.has_fallback,
                                          .permits_any_character = false},
            text, &failure);
    }
    if (status == SXML_STATUS_OK && failure != SXML_TEXT_DECODING_FAILURE_NONE) {
        status = report(call, &decoding_error);
    }
    file_content_release(&content);
    sxml_function_library_release_options(&options.option_values);
    sxml_function_library_text_release(&existing.absolute);
    return status;
}

/* file:read-text($file, $options := ()) as xs:string (4.6). */
static sxml_status call_read_text(const sxml_function_call *call, sxml_sequence *result) {
    sxml_library_text text = text_empty(call->item_heap);
    sxml_status status = read_text_file(call, false, &text);
    if (status == SXML_STATUS_OK) {
        status = make_text_result(call, text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* file:read-text-lines($file, $options := ()) as xs:string* (4.7): the
   text split at LF, a final empty line left out. */
static sxml_status call_read_text_lines(const sxml_function_call *call, sxml_sequence *result) {
    sxml_library_text text = text_empty(call->item_heap);
    sxml_status status = read_text_file(call, true, &text);
    string_results results = {.builder = NULL};
    if (status == SXML_STATUS_OK) {
        status = string_results_start(call, &results);
    }
    const sxml_byte_view content = text_view(&text);
    size_t start = 0;
    for (size_t position = 0; status == SXML_STATUS_OK && position <= content.byte_count;
         position += 1) {
        const bool is_end = position == content.byte_count;
        if (!is_end && content.bytes[position] != '\n') {
            continue;
        }
        if (is_end && start == position) {
            break;
        }
        status = string_results_add(call, &results,
                                    view_span(content, (byte_span){.start = start, .end = position}));
        start = position + 1;
    }
    status = string_results_finish(&results, status, result);
    sxml_function_library_text_release(&text);
    return status;
}

/* ---------------------------------------------------------------- Paths */

/* Appends name to *path and, for a directory, a separator. */
typedef struct named_entry {
    sxml_byte_view name;
    sxml_file_kind file_kind;
} named_entry;

static sxml_status append_entry(file_conventions conventions, sxml_library_text *path,
                                named_entry entry) {
    sxml_status status = append_name(conventions, path, entry.name);
    if (status == SXML_STATUS_OK && entry.file_kind == SXML_FILE_KIND_DIRECTORY) {
        status = sxml_function_library_text_append(path, conventions.directory_separator);
    }
    return status;
}

/* file:parent($path) as xs:string? (5.1). */
static sxml_status call_parent(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    const file_access access = access_of(call);
    sxml_library_text absolute = text_empty(call->item_heap);
    sxml_status status = read_absolute(&access, 0, &absolute);
    const size_t parent = parent_length(access.conventions, text_view(&absolute));
    if (status == SXML_STATUS_OK && parent > 0) {
        absolute.byte_count = parent;
        status = append_separator(access.conventions, &absolute);
        if (status == SXML_STATUS_OK) {
            status = make_text_result(call, text_view(&absolute), result);
        }
    }
    sxml_function_library_text_release(&absolute);
    return status;
}

/* file:children($path) as xs:string* (5.2): the supplied path joined with
   each name. */
static sxml_status call_children(const sxml_function_call *call, sxml_sequence *result) {
    const file_access access = access_of(call);
    sxml_string supplied;
    sxml_library_text absolute = text_empty(call->item_heap);
    sxml_status status = require_file_system(&access);
    if (status == SXML_STATUS_OK) {
        status = read_string(call, 0, &supplied);
    }
    if (status == SXML_STATUS_OK) {
        status = make_absolute(&access, supplied.utf8, &absolute);
    }
    if (status == SXML_STATUS_OK) {
        status = require_directory(&access, text_view(&absolute));
    }
    entry_list entries = {.names = path_store_empty(call->item_heap)};
    if (status == SXML_STATUS_OK) {
        status = list_directory(&access, text_view(&absolute), &entries);
    }
    string_results results = {.builder = NULL};
    if (status == SXML_STATUS_OK) {
        status = string_results_start(call, &results);
    }
    sxml_library_text child = text_empty(call->item_heap);
    for (size_t entry_index = 0;
         status == SXML_STATUS_OK && entry_index < entries.names.paths.element_count;
         entry_index += 1) {
        status = copy_text(supplied.utf8, &child);
        if (status == SXML_STATUS_OK) {
            status = append_entry(
                access.conventions, &child,
                (named_entry){.name = path_store_view(&entries.names, entry_index),
                              .file_kind = path_store_at(&entries.names, entry_index)->file_kind});
        }
        if (status == SXML_STATUS_OK) {
            status = string_results_add(call, &results, text_view(&child));
        }
    }
    if (results.builder != NULL) {
        status = string_results_finish(&results, status, result);
    }
    sxml_function_library_text_release(&child);
    path_store_release(&entries.names);
    sxml_function_library_text_release(&absolute);
    return status;
}

/* Whether name matches a glob pattern: "*" any run of characters, "?"
   one character (5.4); case-sensitive. A loop with one backtrack point. */
static bool glob_matches(sxml_string pattern, sxml_string name) {
    sxml_codepoint_iterator pattern_at = {.string = pattern, .byte_offset = 0};
    sxml_codepoint_iterator name_at = {.string = name, .byte_offset = 0};
    bool has_star = false;
    sxml_codepoint_iterator star_pattern = pattern_at;
    sxml_codepoint_iterator star_name = name_at;
    for (;;) {
        sxml_codepoint_iterator next_name = name_at;
        uint32_t name_codepoint = 0;
        const bool has_name = sxml_codepoint_iterator_next(&next_name, &name_codepoint);
        sxml_codepoint_iterator next_pattern = pattern_at;
        uint32_t pattern_codepoint = 0;
        const bool has_pattern = sxml_codepoint_iterator_next(&next_pattern, &pattern_codepoint);
        if (has_pattern && pattern_codepoint == '*') {
            has_star = true;
            star_pattern = next_pattern;
            star_name = name_at;
            pattern_at = next_pattern;
            continue;
        }
        if (!has_name) {
            if (!has_pattern) {
                return true;
            }
        } else if (has_pattern && (pattern_codepoint == '?' || pattern_codepoint == name_codepoint)) {
            pattern_at = next_pattern;
            name_at = next_name;
            continue;
        }
        if (!has_star) {
            return false;
        }
        uint32_t skipped = 0;
        if (!sxml_codepoint_iterator_next(&star_name, &skipped)) {
            return false;
        }
        pattern_at = star_pattern;
        name_at = star_name;
    }
}

/* file:list($dir, $recursive := false(), $pattern := ()) (5.4): paths
   relative to $dir, directories with a trailing separator; the pattern is
   matched against names only. */
static sxml_status call_list(const sxml_function_call *call, sxml_sequence *result) {
    const file_access access = access_of(call);
    sxml_library_text absolute = text_empty(call->item_heap);
    bool is_recursive = false;
    optional_string pattern;
    sxml_status status = require_file_system(&access);
    if (status == SXML_STATUS_OK) {
        status = read_absolute(&access, 0, &absolute);
    }
    if (status == SXML_STATUS_OK) {
        status = read_boolean(call, 1, &is_recursive);
    }
    if (status == SXML_STATUS_OK) {
        status = read_optional_string(call, 2, &pattern);
    }
    if (status == SXML_STATUS_OK) {
        status = require_directory(&access, text_view(&absolute));
    }
    /* Directories to list, absolute, and the relative prefix of each. */
    path_store directories = path_store_empty(call->item_heap);
    path_store prefixes = path_store_empty(call->item_heap);
    entry_list entries = {.names = path_store_empty(call->item_heap)};
    sxml_library_text directory = text_empty(call->item_heap);
    sxml_library_text relative = text_empty(call->item_heap);
    string_results results = {.builder = NULL};
    const stored_path no_details = {.span = {.start = 0, .end = 0},
                                    .file_kind = SXML_FILE_KIND_DIRECTORY,
                                    .depth = 0};
    if (status == SXML_STATUS_OK) {
        status = path_store_add(&directories, text_view(&absolute), no_details);
    }
    if (status == SXML_STATUS_OK) {
        status = path_store_add(&prefixes, (sxml_byte_view){.bytes = NULL, .byte_count = 0},
                                no_details);
    }
    if (status == SXML_STATUS_OK) {
        status = string_results_start(call, &results);
    }
    for (size_t directory_index = 0;
         status == SXML_STATUS_OK && directory_index < directories.paths.element_count;
         directory_index += 1) {
        status = copy_text(path_store_view(&directories, directory_index), &directory);
        if (status == SXML_STATUS_OK) {
            status = copy_text(path_store_view(&prefixes, directory_index), &relative);
        }
        if (status == SXML_STATUS_OK) {
            status = list_directory(&access, text_view(&directory), &entries);
        }
        const size_t directory_byte_count = directory.byte_count;
        const size_t relative_byte_count = relative.byte_count;
        for (size_t entry_index = 0;
             status == SXML_STATUS_OK && entry_index < entries.names.paths.element_count;
             entry_index += 1) {
            const sxml_byte_view name = path_store_view(&entries.names, entry_index);
            const sxml_file_kind kind = path_store_at(&entries.names, entry_index)->file_kind;
            relative.byte_count = relative_byte_count;
            directory.byte_count = directory_byte_count;
            status = sxml_function_library_text_append(&relative, name);
            if (status == SXML_STATUS_OK && kind == SXML_FILE_KIND_DIRECTORY) {
                status = sxml_function_library_text_append(&relative,
                                                           access.conventions.directory_separator);
            }
            sxml_string name_string;
            if (status == SXML_STATUS_OK) {
                status = sxml_string_from_utf8(name, &name_string);
            }
            if (status == SXML_STATUS_OK &&
                (!pattern.has_value || glob_matches(pattern.string, name_string))) {
                status = string_results_add(call, &results, text_view(&relative));
            }
            if (status == SXML_STATUS_OK && is_recursive && kind == SXML_FILE_KIND_DIRECTORY) {
                status = append_name(access.conventions, &directory, name);
                if (status == SXML_STATUS_OK) {
                    status = path_store_add(&directories, text_view(&directory), no_details);
                }
                if (status == SXML_STATUS_OK) {
                    status = path_store_add(&prefixes, text_view(&relative), no_details);
                }
            }
        }
    }
    if (results.builder != NULL) {
        status = string_results_finish(&results, status, result);
    }
    sxml_function_library_text_release(&directory);
    sxml_function_library_text_release(&relative);
    path_store_release(&entries.names);
    path_store_release(&directories);
    path_store_release(&prefixes);
    sxml_function_library_text_release(&absolute);
    return status;
}

/* file:list-roots() as xs:string* (5.5). */
static sxml_status call_list_roots(const sxml_function_call *call, sxml_sequence *result) {
    const file_access access = access_of(call);
    entry_list entries = {.names = path_store_empty(call->item_heap)};
    sxml_status status = require_file_system(&access);
    if (status == SXML_STATUS_OK) {
        status = list_directory(&access, (sxml_byte_view){.bytes = NULL, .byte_count = 0},
                                &entries);
    }
    string_results results = {.builder = NULL};
    if (status == SXML_STATUS_OK) {
        status = string_results_start(call, &results);
    }
    for (size_t entry_index = 0;
         status == SXML_STATUS_OK && entry_index < entries.names.paths.element_count;
         entry_index += 1) {
        status = string_results_add(call, &results, path_store_view(&entries.names, entry_index));
    }
    if (results.builder != NULL) {
        status = string_results_finish(&results, status, result);
    }
    path_store_release(&entries.names);
    return status;
}

/* Appends a separator to a normalized absolute path that names a
   directory. */
static sxml_status mark_directory(const file_access *access, sxml_library_text *path) {
    if (access->file_system.operations == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_file_description description;
    sxml_status status = describe(access, text_view(path), &description);
    if (status == SXML_STATUS_OK && description.file_kind == SXML_FILE_KIND_DIRECTORY) {
        status = append_separator(access->conventions, path);
    }
    return status;
}

/* file:resolve-path($path, $base := ()) as xs:string (5.6): against the
   current directory, or against $base (absolute: file:is-relative), or
   its parent when it does not end with a separator. */
static sxml_status call_resolve_path(const sxml_function_call *call, sxml_sequence *result) {
    const file_access access = access_of(call);
    sxml_string path;
    optional_string base;
    sxml_status status = read_string(call, 0, &path);
    if (status == SXML_STATUS_OK) {
        status = read_optional_string(call, 1, &base);
    }
    sxml_library_text resolved = text_empty(call->item_heap);
    const bool is_absolute =
        has_file_scheme(path.utf8) || root_length(access.conventions, path.utf8) > 0;
    if (status == SXML_STATUS_OK && (is_absolute || !base.has_value)) {
        status = make_absolute(&access, path.utf8, &resolved);
    } else if (status == SXML_STATUS_OK) {
        const sxml_byte_view base_path = base.string.utf8;
        if (!has_file_scheme(base_path) && root_length(access.conventions, base_path) == 0) {
            status = report(call, &is_relative_error);
        }
        if (status == SXML_STATUS_OK) {
            status = make_absolute(&access, base_path, &resolved);
        }
        if (status == SXML_STATUS_OK && !ends_with_separator(access.conventions, base_path)) {
            const size_t parent = parent_length(access.conventions, text_view(&resolved));
            resolved.byte_count = parent > 0 ? parent : resolved.byte_count;
        }
        if (status == SXML_STATUS_OK) {
            status = append_separator(access.conventions, &resolved);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(&resolved, path.utf8);
        }
        sxml_library_text combined = text_empty(call->item_heap);
        if (status == SXML_STATUS_OK) {
            status = copy_text(text_view(&resolved), &combined);
        }
        if (status == SXML_STATUS_OK) {
            status = make_absolute(&access, text_view(&combined), &resolved);
        }
        sxml_function_library_text_release(&combined);
    }
    if (status == SXML_STATUS_OK) {
        status = mark_directory(&access, &resolved);
    }
    if (status == SXML_STATUS_OK) {
        status = make_text_result(call, text_view(&resolved), result);
    }
    sxml_function_library_text_release(&resolved);
    return status;
}

/* file:path-to-native($path) as xs:string (5.7). */
static sxml_status call_path_to_native(const sxml_function_call *call, sxml_sequence *result) {
    const file_access access = access_of(call);
    existing_path existing;
    sxml_status status = read_existing(&access, &existing);
    sxml_byte_view canonical = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = host_result(&access, access.file_system.operations->canonicalize(
                                          access.file_system.state,
                                          text_view(&existing.absolute), &canonical));
    }
    if (status == SXML_STATUS_OK) {
        status = copy_text(canonical, &existing.absolute);
    }
    if (status == SXML_STATUS_OK &&
        existing.description.file_kind == SXML_FILE_KIND_DIRECTORY) {
        status = append_separator(access.conventions, &existing.absolute);
    }
    if (status == SXML_STATUS_OK) {
        status = make_text_result(call, text_view(&existing.absolute), result);
    }
    sxml_function_library_text_release(&existing.absolute);
    return status;
}

/* Whether a byte stays as it is in a file: URI path (RFC 3986, 2.3
   unreserved, and "/"). */
static bool is_uri_path_byte(uint8_t byte) {
    return is_ascii_letter(byte) || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
           byte == '_' || byte == '~' || byte == '/' || byte == ':';
}

/* file:path-to-uri($path) as xs:anyURI (5.8): the absolute path as a file:
   URI, other bytes percent-encoded; not looked up. */
static sxml_status call_path_to_uri(const sxml_function_call *call, sxml_sequence *result) {
    const file_access access = access_of(call);
    sxml_string path;
    sxml_status status = read_string(call, 0, &path);
    sxml_library_text absolute = text_empty(call->item_heap);
    if (status == SXML_STATUS_OK) {
        status = make_absolute(&access, path.utf8, &absolute);
    }
    if (status == SXML_STATUS_OK && ends_with_separator(access.conventions, path.utf8)) {
        status = append_separator(access.conventions, &absolute);
    }
    sxml_library_text uri = text_empty(call->item_heap);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(
            &uri, access.conventions.is_windows ? SXML_BYTE_VIEW_FROM_LITERAL("file:///")
                                                : SXML_BYTE_VIEW_FROM_LITERAL("file://"));
    }
    const sxml_byte_view bytes = text_view(&absolute);
    static const char hex_digits[] = "0123456789ABCDEF";
    for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index < bytes.byte_count;
         byte_index += 1) {
        uint8_t byte = bytes.bytes[byte_index];
        if (is_separator(access.conventions, byte)) {
            byte = '/';
        }
        if (is_uri_path_byte(byte)) {
            status = sxml_function_library_text_append(
                &uri, (sxml_byte_view){.bytes = &byte, .byte_count = 1});
        } else {
            const uint8_t escaped[3] = {'%', (uint8_t)hex_digits[byte >> 4U],
                                        (uint8_t)hex_digits[byte & 0x0FU]};
            status = sxml_function_library_text_append(
                &uri, (sxml_byte_view){.bytes = escaped, .byte_count = 3});
        }
    }
    sxml_string uri_string;
    if (status == SXML_STATUS_OK) {
        status = sxml_string_from_utf8(text_view(&uri), &uri_string);
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){.type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_URI),
                                        .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                                                  .datum_details = {.string = uri_string}}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    sxml_function_library_text_release(&uri);
    sxml_function_library_text_release(&absolute);
    return status;
}

/* ------------------------------------------------------ System properties */

static sxml_status call_dir_separator(const sxml_function_call *call, sxml_sequence *result) {
    return make_text_result(call, access_of(call).conventions.directory_separator, result);
}

static sxml_status call_line_separator(const sxml_function_call *call, sxml_sequence *result) {
    return make_text_result(call, access_of(call).conventions.line_separator, result);
}

static sxml_status call_path_separator(const sxml_function_call *call, sxml_sequence *result) {
    return make_text_result(call, access_of(call).conventions.path_separator, result);
}

/* file:temp-dir and file:current-dir (6.4, 6.6): with a trailing
   separator. */
static sxml_status call_directory(const sxml_function_call *call,
                                  sxml_file_directory_kind directory_kind, sxml_sequence *result) {
    const file_access access = access_of(call);
    sxml_status status = require_file_system(&access);
    sxml_byte_view path = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = host_result(&access, access.file_system.operations->get_directory(
                                          access.file_system.state, directory_kind, &path));
    }
    sxml_library_text directory = text_empty(call->item_heap);
    if (status == SXML_STATUS_OK) {
        status = make_absolute(&access, path, &directory);
    }
    if (status == SXML_STATUS_OK) {
        status = append_separator(access.conventions, &directory);
    }
    if (status == SXML_STATUS_OK) {
        status = make_text_result(call, text_view(&directory), result);
    }
    sxml_function_library_text_release(&directory);
    return status;
}

static sxml_status call_temp_dir(const sxml_function_call *call, sxml_sequence *result) {
    return call_directory(call, SXML_FILE_DIRECTORY_KIND_TEMPORARY, result);
}

static sxml_status call_current_dir(const sxml_function_call *call, sxml_sequence *result) {
    return call_directory(call, SXML_FILE_DIRECTORY_KIND_CURRENT, result);
}

/* file:base-dir() as xs:string? (6.5): the directory of a file: static
   base URI, else (). */
static sxml_status call_base_dir(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view base_uri = sxml_dynamic_context_get_static_base_uri(call->dynamic_context);
    if (!has_file_scheme(base_uri)) {
        return SXML_STATUS_OK;
    }
    const file_access access = access_of(call);
    sxml_library_text directory = text_empty(call->item_heap);
    sxml_status status = make_absolute(&access, base_uri, &directory);
    if (status == SXML_STATUS_OK && !ends_with_separator(access.conventions, base_uri)) {
        const size_t parent = parent_length(access.conventions, text_view(&directory));
        directory.byte_count = parent > 0 ? parent : directory.byte_count;
    }
    if (status == SXML_STATUS_OK) {
        status = append_separator(access.conventions, &directory);
    }
    if (status == SXML_STATUS_OK) {
        status = make_text_result(call, text_view(&directory), result);
    }
    sxml_function_library_text_release(&directory);
    return status;
}

/* ------------------------------------------------------ file:descendants */

/* The options of file:descendants (5.3), by position. */
enum {
    SXML_FILE_DESCENDANTS_OPTION_RECURSE,
    SXML_FILE_DESCENDANTS_OPTION_FILTER,
    SXML_FILE_DESCENDANTS_OPTION_DEPTH,
};

static const sxml_option_definition descendants_option_definitions[] = {
    SXML_FUNCTION_OPTION("recurse", SXML_TYPE_FUNCTION, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("filter", SXML_TYPE_FUNCTION, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("depth", SXML_TYPE_XS_INTEGER, SXML_OCCURRENCE_ZERO_OR_ONE),
};

/* What a descendants walk is doing with its current entry. */
typedef enum descendants_phase {
    /* Take the next entry, or the next directory to list. */
    SXML_DESCENDANTS_PHASE_NEXT_ENTRY,
    /* The filter predicate was asked about the current entry. */
    SXML_DESCENDANTS_PHASE_FILTERED,
    /* The recurse predicate was asked about the current entry. */
    SXML_DESCENDANTS_PHASE_RECURSED,
} descendants_phase;

/* The state of one file:descendants call, zeroed by the evaluator. */
typedef struct descendants_state {
    bool is_started;
    descendants_phase phase;
    sxml_option_values option_values;
    bool has_recurse;
    bool has_filter;
    bool has_depth_limit;
    int64_t depth_limit;
    /* Directories to list: absolute paths, and the paths reported for
       them (the supplied path joined with the relative path), by index. */
    path_store directories;
    path_store reported_directories;
    size_t next_directory;
    /* The entries of the directory being processed, and its depth. */
    entry_list entries;
    size_t next_entry;
    size_t entry_depth;
    /* The current entry's absolute and reported paths. */
    sxml_library_text entry_path;
    sxml_library_text entry_reported;
    sxml_file_kind entry_kind;
    /* The reported paths kept. */
    path_store results;
    /* The argument of a requested call (owned). */
    sxml_sequence call_argument;
    sxml_library_text scratch;
} descendants_state;

static void release_descendants_state(void *state) {
    descendants_state *walk = state;
    if (!walk->is_started) {
        return;
    }
    sxml_function_library_release_options(&walk->option_values);
    path_store_release(&walk->directories);
    path_store_release(&walk->reported_directories);
    path_store_release(&walk->entries.names);
    sxml_function_library_text_release(&walk->entry_path);
    sxml_function_library_text_release(&walk->entry_reported);
    path_store_release(&walk->results);
    sxml_sequence_release(walk->call_argument);
    walk->call_argument = sxml_sequence_empty();
    sxml_function_library_text_release(&walk->scratch);
}

static const stored_path descendants_directory = {.span = {.start = 0, .end = 0},
                                                  .file_kind = SXML_FILE_KIND_DIRECTORY,
                                                  .depth = 0};

/* Reads the arguments and lists nothing yet: the walk starts at $path. */
static sxml_status start_descendants(const file_access *access, descendants_state *walk) {
    sxml_item_heap *item_heap = access->call->item_heap;
    walk->is_started = true;
    walk->directories = path_store_empty(item_heap);
    walk->reported_directories = path_store_empty(item_heap);
    walk->entries = (entry_list){.names = path_store_empty(item_heap)};
    walk->entry_path = text_empty(item_heap);
    walk->entry_reported = text_empty(item_heap);
    walk->results = path_store_empty(item_heap);
    walk->call_argument = sxml_sequence_empty();
    walk->scratch = text_empty(item_heap);
    walk->phase = SXML_DESCENDANTS_PHASE_NEXT_ENTRY;
    sxml_status status = require_file_system(access);
    sxml_string supplied;
    if (status == SXML_STATUS_OK) {
        status = read_string(access->call, 0, &supplied);
    }
    if (status == SXML_STATUS_OK) {
        status = make_absolute(access, supplied.utf8, &walk->scratch);
    }
    if (status == SXML_STATUS_OK) {
        status = require_directory(access, text_view(&walk->scratch));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_options(
            access->call,
            &(sxml_option_request){
                .argument_index = 1,
                .definitions = {descendants_option_definitions,
                                sizeof(descendants_option_definitions) /
                                    sizeof(descendants_option_definitions[0])}},
            &walk->option_values);
    }
    if (status == SXML_STATUS_OK) {
        walk->has_recurse = walk->option_values.is_present[SXML_FILE_DESCENDANTS_OPTION_RECURSE];
        walk->has_filter = walk->option_values.is_present[SXML_FILE_DESCENDANTS_OPTION_FILTER];
        const sxml_sequence depth =
            walk->option_values.values[SXML_FILE_DESCENDANTS_OPTION_DEPTH];
        sxml_atomic_datum datum;
        if (walk->option_values.is_present[SXML_FILE_DESCENDANTS_OPTION_DEPTH] &&
            sxml_sequence_get_representation(depth) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
            sxml_atomic_item_get_datum(depth.content, &datum) == SXML_STATUS_OK) {
            walk->has_depth_limit = true;
            walk->depth_limit =
                datum.datum_kind == SXML_DATUM_KIND_INTEGER ? datum.datum_details.integer : INT64_MAX;
            if (datum.datum_kind != SXML_DATUM_KIND_INTEGER &&
                datum.datum_details.decimal_text.byte_count > 0 &&
                datum.datum_details.decimal_text.bytes[0] == '-') {
                walk->depth_limit = 0;
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        status = path_store_add(&walk->directories, text_view(&walk->scratch), descendants_directory);
    }
    if (status == SXML_STATUS_OK) {
        status = path_store_add(&walk->reported_directories, supplied.utf8, descendants_directory);
    }
    return status;
}

/* Whether the walk may descend below an entry at depth. */
static bool may_descend(const descendants_state *walk, size_t depth) {
    if (!walk->has_depth_limit) {
        return true;
    }
    return walk->depth_limit > 0 && (uint64_t)depth < (uint64_t)walk->depth_limit;
}

/* Lists the next directory into walk->entries; false when none is left. */
static sxml_status list_next_directory(const file_access *access, descendants_state *walk,
                                       bool *has_directory) {
    *has_directory = walk->next_directory < walk->directories.paths.element_count;
    if (!*has_directory) {
        return SXML_STATUS_OK;
    }
    const size_t directory_index = walk->next_directory;
    walk->next_directory += 1;
    walk->entry_depth = path_store_at(&walk->directories, directory_index)->depth;
    walk->next_entry = 0;
    sxml_status status = copy_text(path_store_view(&walk->directories, directory_index),
                                   &walk->scratch);
    if (status == SXML_STATUS_OK) {
        status = list_directory(access, text_view(&walk->scratch), &walk->entries);
    }
    return status;
}

/* Makes the current entry's paths from entry entry_index of the directory
   last listed (walk->next_directory - 1). */
static sxml_status take_entry(const file_access *access, descendants_state *walk,
                              size_t entry_index) {
    const size_t directory_index = walk->next_directory - 1;
    const sxml_byte_view name = path_store_view(&walk->entries.names, entry_index);
    walk->entry_kind = path_store_at(&walk->entries.names, entry_index)->file_kind;
    sxml_status status =
        copy_text(path_store_view(&walk->directories, directory_index), &walk->entry_path);
    if (status == SXML_STATUS_OK) {
        status = append_name(access->conventions, &walk->entry_path, name);
    }
    if (status == SXML_STATUS_OK) {
        status = copy_text(path_store_view(&walk->reported_directories, directory_index),
                           &walk->entry_reported);
    }
    if (status == SXML_STATUS_OK) {
        status = append_entry(access->conventions, &walk->entry_reported,
                              (named_entry){.name = name, .file_kind = walk->entry_kind});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_steps(access->call, 1);
    }
    return status;
}

/* Asks the evaluator to call the option's predicate with the current
   entry's reported path. */
static sxml_status request_predicate(const file_access *access, descendants_state *walk,
                                     size_t option_index, sxml_native_step *step) {
    sxml_sequence_release(walk->call_argument);
    walk->call_argument = sxml_sequence_empty();
    sxml_status status =
        make_text_result(access->call, text_view(&walk->entry_reported), &walk->call_argument);
    if (status == SXML_STATUS_OK) {
        *step = (sxml_native_step){
            .step_kind = SXML_NATIVE_STEP_KIND_CALL,
            .result = sxml_sequence_empty(),
            .function_item = walk->option_values.values[option_index].content,
            .arguments = {.sequences = &walk->call_argument, .sequence_count = 1}};
    }
    return status;
}

static const sxml_static_error predicate_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A recurse or filter predicate must return xs:boolean? (EXPath File 4.0, 5.3)")};

/* The truth of a predicate's value (taken over), xs:boolean?: the empty
   sequence is false (5.3); anything else is XPTY0004. */
static sxml_status read_truth(const sxml_native_step_request *request, bool *is_true) {
    *is_true = false;
    const sxml_sequence value = request->call_result;
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(value, request->call->error_record, &item_count);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK && item_count == 1) {
        status = sxml_sequence_get_item(
            value, (sxml_item_request){.item_index = 0, .error_record = request->call->error_record},
            &item);
    }
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK && item_count == 1) {
        if (sxml_item_get_kind(item) == SXML_ITEM_KIND_ATOMIC &&
            sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK &&
            datum.datum_kind == SXML_DATUM_KIND_BOOLEAN) {
            *is_true = datum.datum_details.boolean;
        } else {
            status = sxml_function_library_report(request->call, &predicate_type_error);
        }
    } else if (status == SXML_STATUS_OK && item_count > 1) {
        status = sxml_function_library_report(request->call, &predicate_type_error);
    }
    sxml_sequence_release(value);
    return status;
}

/* Keeps the current entry when it passed the filter, then decides whether
   to descend: asks the recurse predicate, or descends by the depth. */
static sxml_status after_filter(const file_access *access, descendants_state *walk,
                                bool is_kept, sxml_native_step *step) {
    sxml_status status = SXML_STATUS_OK;
    if (is_kept) {
        status = path_store_add(&walk->results, text_view(&walk->entry_reported),
                                descendants_directory);
    }
    walk->phase = SXML_DESCENDANTS_PHASE_NEXT_ENTRY;
    if (status != SXML_STATUS_OK || walk->entry_kind != SXML_FILE_KIND_DIRECTORY ||
        !may_descend(walk, walk->entry_depth)) {
        return status;
    }
    if (walk->has_recurse) {
        walk->phase = SXML_DESCENDANTS_PHASE_RECURSED;
        return request_predicate(access, walk, SXML_FILE_DESCENDANTS_OPTION_RECURSE, step);
    }
    status = path_store_add(&walk->directories, text_view(&walk->entry_path),
                            (stored_path){.span = {.start = 0, .end = 0},
                                          .file_kind = SXML_FILE_KIND_DIRECTORY,
                                          .depth = walk->entry_depth + 1});
    if (status == SXML_STATUS_OK) {
        status = path_store_add(&walk->reported_directories, text_view(&walk->entry_reported),
                                descendants_directory);
    }
    return status;
}

/* Finishes with the kept paths. */
static sxml_status finish_descendants(const file_access *access, descendants_state *walk,
                                      sxml_native_step *step) {
    string_results results = {.builder = NULL};
    sxml_status status = string_results_start(access->call, &results);
    for (size_t result_index = 0;
         status == SXML_STATUS_OK && result_index < walk->results.paths.element_count;
         result_index += 1) {
        status = string_results_add(access->call, &results,
                                    path_store_view(&walk->results, result_index));
    }
    sxml_sequence value = sxml_sequence_empty();
    status = string_results_finish(&results, status, &value);
    if (status == SXML_STATUS_OK) {
        *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                                   .result = value,
                                   .function_item = {.representation = 0},
                                   .arguments = {.sequences = NULL, .sequence_count = 0}};
    }
    return status;
}

/*
 * file:descendants($path, $options := {}) as xs:string* (5.3): breadth
 * first; for each entry the filter predicate decides whether it is kept,
 * then, for a directory within the depth, the recurse predicate whether
 * its entries are walked. The predicates are called by the evaluator
 * (D71, D75): each step finishes, or asks for one call.
 */
static sxml_status step_descendants(const sxml_native_step_request *request,
                                    sxml_native_step *step) {
    descendants_state *walk = request->state;
    const file_access access = access_of(request->call);
    sxml_status status = SXML_STATUS_OK;
    if (!walk->is_started) {
        status = start_descendants(&access, walk);
    } else if (walk->phase == SXML_DESCENDANTS_PHASE_FILTERED) {
        bool is_kept = false;
        status = read_truth(request, &is_kept);
        if (status == SXML_STATUS_OK) {
            step->step_kind = SXML_NATIVE_STEP_KIND_FINISH;
            status = after_filter(&access, walk, is_kept, step);
            if (status == SXML_STATUS_OK && walk->phase == SXML_DESCENDANTS_PHASE_RECURSED) {
                return SXML_STATUS_OK;
            }
        }
    } else if (walk->phase == SXML_DESCENDANTS_PHASE_RECURSED) {
        bool descends = false;
        status = read_truth(request, &descends);
        walk->phase = SXML_DESCENDANTS_PHASE_NEXT_ENTRY;
        if (status == SXML_STATUS_OK && descends) {
            status = path_store_add(&walk->directories, text_view(&walk->entry_path),
                                    (stored_path){.span = {.start = 0, .end = 0},
                                                  .file_kind = SXML_FILE_KIND_DIRECTORY,
                                                  .depth = walk->entry_depth + 1});
            if (status == SXML_STATUS_OK) {
                status = path_store_add(&walk->reported_directories,
                                        text_view(&walk->entry_reported), descendants_directory);
            }
        }
    }
    while (status == SXML_STATUS_OK) {
        if (walk->next_directory == 0 ||
            walk->next_entry >= walk->entries.names.paths.element_count) {
            bool has_directory = false;
            status = list_next_directory(&access, walk, &has_directory);
            if (status != SXML_STATUS_OK) {
                break;
            }
            if (!has_directory) {
                return finish_descendants(&access, walk, step);
            }
            continue;
        }
        const size_t entry_index = walk->next_entry;
        walk->next_entry += 1;
        status = take_entry(&access, walk, entry_index);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (walk->has_filter) {
            walk->phase = SXML_DESCENDANTS_PHASE_FILTERED;
            return request_predicate(&access, walk, SXML_FILE_DESCENDANTS_OPTION_FILTER, step);
        }
        step->step_kind = SXML_NATIVE_STEP_KIND_FINISH;
        status = after_filter(&access, walk, true, step);
        if (status == SXML_STATUS_OK && walk->phase == SXML_DESCENDANTS_PHASE_RECURSED) {
            return SXML_STATUS_OK;
        }
    }
    return status;
}

static const sxml_resumable_native_body descendants_body = {
    .step = step_descendants,
    .release_state = release_descendants_state,
    .state_byte_count = sizeof(descendants_state),
    .state_alignment = alignof(descendants_state),
};

/* ------------------------------------------------------------------ Rows */

/* Touches files: neither moved, folded nor cached (1.4). */
#define SXML_FILE_EFFECTS                                                      \
    ((uint32_t)SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT |                     \
     (uint32_t)SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC)

/* A row in the file namespace (EXPath File 4.0, section 1.7). */
#define SXML_FILE_ROW(name_text, parameter_array, result_text, property_bits, body)            \
    {.function_namespace = SXML_NAMESPACE_EXPATH_FILE,                                          \
     .properties = (property_bits),                                                             \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                                       \
     .parameters = {(parameter_array), sizeof(parameter_array) / sizeof((parameter_array)[0])}, \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                                    \
     .call = (body),                                                                            \
     .resumable_body = NULL}

#define SXML_FILE_NULLARY_ROW(name_text, result_text, property_bits, body)                     \
    {.function_namespace = SXML_NAMESPACE_EXPATH_FILE,                                          \
     .properties = (property_bits),                                                             \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                                       \
     .parameters = {NULL, 0},                                                                   \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                                    \
     .call = (body),                                                                            \
     .resumable_body = NULL}

static const sxml_function_parameter path_parameters[] = {
    SXML_REQUIRED_PARAMETER("path", "xs:string"),
};
static const sxml_function_parameter path_recursive_parameters[] = {
    SXML_REQUIRED_PARAMETER("path", "xs:string"),
    SXML_OPTIONAL_PARAMETER("recursive", "xs:boolean?"),
};
static const sxml_function_parameter dir_parameters[] = {
    SXML_REQUIRED_PARAMETER("dir", "xs:string"),
};
static const sxml_function_parameter temporary_parameters[] = {
    SXML_OPTIONAL_PARAMETER("prefix", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("suffix", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("dir", "xs:string?"),
};
static const sxml_function_parameter transfer_parameters[] = {
    SXML_REQUIRED_PARAMETER("source", "xs:string"),
    SXML_REQUIRED_PARAMETER("target", "xs:string"),
};
static const sxml_function_parameter serialized_parameters[] = {
    SXML_REQUIRED_PARAMETER("file", "xs:string"),
    SXML_REQUIRED_PARAMETER("value", "item()*"),
    /* (element(output:serialization-parameters) | map(*))?, checked by
       the parameter reader as fn:serialize does (D78). */
    SXML_OPTIONAL_PARAMETER("options", "item()?"),
};
static const sxml_function_parameter binary_parameters[] = {
    SXML_REQUIRED_PARAMETER("file", "xs:string"),
    SXML_REQUIRED_PARAMETER("value", "xs:base64Binary"),
};
static const sxml_function_parameter write_binary_parameters[] = {
    SXML_REQUIRED_PARAMETER("file", "xs:string"),
    SXML_REQUIRED_PARAMETER("value", "xs:base64Binary"),
    SXML_OPTIONAL_PARAMETER("offset", "xs:integer?"),
};
static const sxml_function_parameter text_parameters[] = {
    SXML_REQUIRED_PARAMETER("file", "xs:string"),
    SXML_REQUIRED_PARAMETER("value", "xs:string"),
    SXML_OPTIONAL_PARAMETER("encoding", "xs:string?"),
};
static const sxml_function_parameter text_lines_parameters[] = {
    SXML_REQUIRED_PARAMETER("file", "xs:string"),
    SXML_REQUIRED_PARAMETER("lines", "xs:string*"),
    SXML_OPTIONAL_PARAMETER("encoding", "xs:string?"),
};
static const sxml_function_parameter read_binary_parameters[] = {
    SXML_REQUIRED_PARAMETER("file", "xs:string"),
    SXML_OPTIONAL_PARAMETER("offset", "xs:integer?"),
    SXML_OPTIONAL_PARAMETER("length", "xs:integer?"),
};
static const sxml_function_parameter read_text_parameters[] = {
    SXML_REQUIRED_PARAMETER("file", "xs:string"),
    SXML_OPTIONAL_PARAMETER("options", "(xs:string | map(*))?"),
};
static const sxml_function_parameter descendants_parameters[] = {
    SXML_REQUIRED_PARAMETER("path", "xs:string"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?"),
};
static const sxml_function_parameter list_parameters[] = {
    SXML_REQUIRED_PARAMETER("dir", "xs:string"),
    SXML_OPTIONAL_PARAMETER("recursive", "xs:boolean?"),
    SXML_OPTIONAL_PARAMETER("pattern", "xs:string?"),
};
static const sxml_function_parameter resolve_path_parameters[] = {
    SXML_REQUIRED_PARAMETER("path", "xs:string"),
    SXML_OPTIONAL_PARAMETER("base", "xs:string?"),
};

static const sxml_native_function file_functions[] = {
    SXML_FILE_ROW("name", path_parameters, "xs:string", 0, call_name),
    SXML_FILE_ROW("last-modified", path_parameters, "xs:dateTime", SXML_FILE_EFFECTS,
                  call_last_modified),
    SXML_FILE_ROW("size", path_recursive_parameters, "xs:integer", SXML_FILE_EFFECTS, call_size),
    SXML_FILE_ROW("exists", path_parameters, "xs:boolean", SXML_FILE_EFFECTS, call_exists),
    SXML_FILE_ROW("is-dir", path_parameters, "xs:boolean", SXML_FILE_EFFECTS, call_is_dir),
    SXML_FILE_ROW("is-file", path_parameters, "xs:boolean", SXML_FILE_EFFECTS, call_is_file),
    SXML_FILE_ROW("is-absolute", path_parameters, "xs:boolean",
                  SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_is_absolute),
    SXML_FILE_ROW("copy", transfer_parameters, "empty-sequence()", SXML_FILE_EFFECTS, call_copy),
    SXML_FILE_ROW("create-dir", dir_parameters, "empty-sequence()", SXML_FILE_EFFECTS,
                  call_create_dir),
    SXML_FILE_ROW("create-temp-dir", temporary_parameters, "xs:string", SXML_FILE_EFFECTS,
                  call_create_temp_dir),
    SXML_FILE_ROW("create-temp-file", temporary_parameters, "xs:string", SXML_FILE_EFFECTS,
                  call_create_temp_file),
    SXML_FILE_ROW("delete", path_recursive_parameters, "empty-sequence()", SXML_FILE_EFFECTS,
                  call_delete),
    SXML_FILE_ROW("move", transfer_parameters, "empty-sequence()", SXML_FILE_EFFECTS, call_move),
    SXML_FILE_ROW("append", serialized_parameters, "empty-sequence()", SXML_FILE_EFFECTS,
                  call_append),
    SXML_FILE_ROW("append-binary", binary_parameters, "empty-sequence()", SXML_FILE_EFFECTS,
                  call_append_binary),
    SXML_FILE_ROW("append-text", text_parameters, "empty-sequence()", SXML_FILE_EFFECTS,
                  call_append_text),
    SXML_FILE_ROW("append-text-lines", text_lines_parameters, "empty-sequence()",
                  SXML_FILE_EFFECTS, call_append_text_lines),
    SXML_FILE_ROW("read-binary", read_binary_parameters, "xs:base64Binary", SXML_FILE_EFFECTS,
                  call_read_binary),
    SXML_FILE_ROW("read-text", read_text_parameters, "xs:string", SXML_FILE_EFFECTS,
                  call_read_text),
    SXML_FILE_ROW("read-text-lines", read_text_parameters, "xs:string*", SXML_FILE_EFFECTS,
                  call_read_text_lines),
    SXML_FILE_ROW("write", serialized_parameters, "empty-sequence()", SXML_FILE_EFFECTS,
                  call_write),
    SXML_FILE_ROW("write-binary", write_binary_parameters, "empty-sequence()", SXML_FILE_EFFECTS,
                  call_write_binary),
    SXML_FILE_ROW("write-text", text_parameters, "empty-sequence()", SXML_FILE_EFFECTS,
                  call_write_text),
    SXML_FILE_ROW("write-text-lines", text_lines_parameters, "empty-sequence()",
                  SXML_FILE_EFFECTS, call_write_text_lines),
    SXML_FILE_ROW("parent", path_parameters, "xs:string?", SXML_FILE_EFFECTS, call_parent),
    SXML_FILE_ROW("children", path_parameters, "xs:string*", SXML_FILE_EFFECTS, call_children),
    {.function_namespace = SXML_NAMESPACE_EXPATH_FILE,
     .properties = SXML_FILE_EFFECTS | (uint32_t)SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("descendants"),
     .parameters = {descendants_parameters,
                    sizeof(descendants_parameters) / sizeof(descendants_parameters[0])},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:string*"),
     .call = NULL,
     .resumable_body = &descendants_body},
    SXML_FILE_ROW("list", list_parameters, "xs:string*", SXML_FILE_EFFECTS, call_list),
    SXML_FILE_NULLARY_ROW("list-roots", "xs:string*", SXML_FILE_EFFECTS, call_list_roots),
    SXML_FILE_ROW("resolve-path", resolve_path_parameters, "xs:string", SXML_FILE_EFFECTS,
                  call_resolve_path),
    SXML_FILE_ROW("path-to-native", path_parameters, "xs:string", SXML_FILE_EFFECTS,
                  call_path_to_native),
    SXML_FILE_ROW("path-to-uri", path_parameters, "xs:anyURI", SXML_FILE_EFFECTS,
                  call_path_to_uri),
    SXML_FILE_NULLARY_ROW("dir-separator", "xs:string", SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
                          call_dir_separator),
    SXML_FILE_NULLARY_ROW("line-separator", "xs:string",
                          SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_line_separator),
    SXML_FILE_NULLARY_ROW("path-separator", "xs:string",
                          SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_path_separator),
    SXML_FILE_NULLARY_ROW("temp-dir", "xs:string", SXML_FILE_EFFECTS, call_temp_dir),
    SXML_FILE_NULLARY_ROW("base-dir", "xs:string?", SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
                          call_base_dir),
    SXML_FILE_NULLARY_ROW("current-dir", "xs:string", SXML_FILE_EFFECTS, call_current_dir),
};

/* Not named sxml_function_library_<part>: not a registered part (D191). */
static const sxml_native_function_view file_library_functions = SXML_LIBRARY_PART(file_functions);

/* --------------------------------------------------------------- Feature */

static const sxml_optional_feature_definition sxml_expath_file_feature_definition = {
    .feature_kind = SXML_OPTIONAL_FEATURE_KIND_EXPATH_FILE,
    .feature_name = SXML_BYTE_VIEW_INITIALIZER("expath-file"),
    .operations = &file_library_functions,
    .finalize_state = NULL,
    .library_functions = &file_library_functions,
};

sxml_optional_feature sxml_expath_file_feature(void) {
    return (sxml_optional_feature){.definition = &sxml_expath_file_feature_definition};
}
