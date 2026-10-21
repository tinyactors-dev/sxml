/*
 * The collections of the query commands (query_command.h; docs/CLI.md,
 * "Collections"; D298): a collection URI names a local directory, and
 * fn:collection gives the XML documents of its files, fn:uri-collection
 * their URIs (F&O 4.0, 17.1.3, 17.1.4: what a collection URI names is
 * implementation-defined).
 *
 * Imitates the CLDR and file branches of the resolver in query_command.c.
 * Public API only: the directory is listed through the invocation's file
 * system (include/sxml/processor.h, sxml_file_system; describe, list and
 * canonicalize, never a write), its files read through the invocation's
 * file reader, and each document parsed with sxml_processor_parse_document
 * and the processor's entity resolver, as fn:doc parses a file. No C
 * recursion (D71): subdirectories are a work list.
 */
#include "query_command.h"

#include "sxml/memory.h"
#include "sxml/types.h"

#include <stdalign.h>
#include <stdbool.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static bool sxml_collection_starts_with(sxml_byte_view text, sxml_byte_view prefix) {
    return text.byte_count >= prefix.byte_count &&
           (prefix.byte_count == 0 || memcmp(text.bytes, prefix.bytes, prefix.byte_count) == 0);
}

/* ------------------------------------------------------------ The URI */

/* Reads one "name=value" of the query part into *query; false for a name
   or value the CLI does not know. */
static bool sxml_collection_read_parameter(sxml_byte_view parameter,
                                           sxml_query_collection_query *query) {
    const uint8_t *equals =
        parameter.byte_count == 0 ? NULL : memchr(parameter.bytes, '=', parameter.byte_count);
    if (equals == NULL) {
        return false;
    }
    const sxml_byte_view name = {.bytes = parameter.bytes,
                                 .byte_count = (size_t)(equals - parameter.bytes)};
    const sxml_byte_view value = {.bytes = equals + 1,
                                  .byte_count = parameter.byte_count - name.byte_count - 1};
    if (sxml_byte_view_equals(name, SXML_TEXT("select")) && value.byte_count > 0) {
        query->select = value;
        return true;
    }
    const bool is_yes = sxml_byte_view_equals(value, SXML_TEXT("yes"));
    const bool is_no = sxml_byte_view_equals(value, SXML_TEXT("no"));
    if (sxml_byte_view_equals(name, SXML_TEXT("recurse")) && (is_yes || is_no)) {
        query->recurses = is_yes;
        return true;
    }
    const bool is_ignore = sxml_byte_view_equals(value, SXML_TEXT("ignore"));
    if (sxml_byte_view_equals(name, SXML_TEXT("on-error")) &&
        (is_ignore || sxml_byte_view_equals(value, SXML_TEXT("fail")))) {
        query->ignores_errors = is_ignore;
        return true;
    }
    return false;
}

void sxml_query_read_collection_uri(sxml_byte_view uri, sxml_query_collection_query *query) {
    *query = (sxml_query_collection_query){
        .directory_uri = {.bytes = NULL, .byte_count = 0},
        .select = SXML_TEXT("*.xml"),
        .recurses = false,
        .ignores_errors = false,
        .refusal = {.bytes = NULL, .byte_count = 0}};
    size_t query_start = uri.byte_count;
    size_t fragment_start = uri.byte_count;
    for (size_t byte_index = uri.byte_count; byte_index > 0; byte_index -= 1) {
        if (uri.bytes[byte_index - 1] == '#') {
            fragment_start = byte_index - 1;
        }
    }
    for (size_t byte_index = 0; byte_index < fragment_start; byte_index += 1) {
        if (uri.bytes[byte_index] == '?') {
            query_start = byte_index;
            break;
        }
    }
    if (fragment_start < uri.byte_count) {
        query->refusal = SXML_TEXT("A collection URI has no fragment");
        return;
    }
    query->directory_uri = (sxml_byte_view){.bytes = uri.bytes, .byte_count = query_start};
    if (!sxml_collection_starts_with(query->directory_uri, SXML_TEXT("file://"))) {
        query->refusal = SXML_TEXT("Only local directories are collections");
        return;
    }
    /* The parameters, separated by "&" or ";", as other processors write
       them (F&O 4.0, 17.1.3 lets the URI's form be the implementation's). */
    size_t part_start = query_start + 1;
    while (part_start <= uri.byte_count && query_start < uri.byte_count) {
        size_t part_end = part_start;
        while (part_end < uri.byte_count && uri.bytes[part_end] != '&' &&
               uri.bytes[part_end] != ';') {
            part_end += 1;
        }
        const sxml_byte_view part = {.bytes = uri.bytes + part_start,
                                     .byte_count = part_end - part_start};
        if (part.byte_count > 0 && !sxml_collection_read_parameter(part, query)) {
            query->refusal = SXML_TEXT("A collection URI's parameters are select=PATTERN, "
                                       "recurse=yes|no and on-error=fail|ignore");
            return;
        }
        part_start = part_end + 1;
    }
}

/* Whether name matches pattern: "*" any characters, "?" one character
   (of UTF-8), anything else itself. Iterative, with one backtracking
   point for the last "*" (D71). */
static bool sxml_collection_matches(sxml_byte_view pattern, sxml_byte_view name) {
    size_t pattern_index = 0;
    size_t name_index = 0;
    bool has_star = false;
    size_t star_pattern_index = 0;
    size_t star_name_index = 0;
    while (name_index < name.byte_count) {
        if (pattern_index < pattern.byte_count && pattern.bytes[pattern_index] == '*') {
            has_star = true;
            pattern_index += 1;
            star_pattern_index = pattern_index;
            star_name_index = name_index;
            continue;
        }
        if (pattern_index < pattern.byte_count &&
            (pattern.bytes[pattern_index] == '?' ||
             pattern.bytes[pattern_index] == name.bytes[name_index])) {
            const bool is_any = pattern.bytes[pattern_index] == '?';
            pattern_index += 1;
            name_index += 1;
            while (is_any && name_index < name.byte_count &&
                   (name.bytes[name_index] & 0xC0U) == 0x80U) {
                name_index += 1;
            }
            continue;
        }
        if (!has_star) {
            return false;
        }
        star_name_index += 1;
        name_index = star_name_index;
        pattern_index = star_pattern_index;
    }
    while (pattern_index < pattern.byte_count && pattern.bytes[pattern_index] == '*') {
        pattern_index += 1;
    }
    return pattern_index == pattern.byte_count;
}

/* ------------------------------------------------------------ Path lists */

/* Paths in allocator memory, each owned. */
typedef struct sxml_collection_path_list {
    sxml_byte_view *paths;
    size_t path_count;
    size_t path_capacity;
} sxml_collection_path_list;

/* A path to add: first, then second, with "/" between them when both are
   not empty and first does not end with one. */
typedef struct sxml_collection_joined_path {
    sxml_byte_view first;
    sxml_byte_view second;
} sxml_collection_joined_path;

static sxml_status sxml_collection_add_path(sxml_allocator allocator,
                                            sxml_collection_path_list *list,
                                            const sxml_collection_joined_path *joined) {
    if (list->path_count == list->path_capacity) {
        const size_t capacity = list->path_capacity == 0 ? 16 : list->path_capacity * 2;
        size_t byte_count = 0;
        void *memory = NULL;
        sxml_status status = sxml_size_multiply(capacity, sizeof(sxml_byte_view), &byte_count);
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(sxml_byte_view)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (list->path_count > 0) {
            memcpy(memory, list->paths, list->path_count * sizeof(sxml_byte_view));
        }
        sxml_allocator_release(allocator, list->paths);
        list->paths = memory;
        list->path_capacity = capacity;
    }
    const bool has_separator = joined->first.byte_count > 0 && joined->second.byte_count > 0 &&
                               joined->first.bytes[joined->first.byte_count - 1] != '/';
    const size_t byte_count =
        joined->first.byte_count + joined->second.byte_count + (has_separator ? 1 : 0);
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        allocator, (sxml_allocation_request){.byte_count = byte_count + 1, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *bytes = memory;
    size_t written_count = 0;
    if (joined->first.byte_count > 0) {
        memcpy(bytes, joined->first.bytes, joined->first.byte_count);
        written_count = joined->first.byte_count;
    }
    if (has_separator) {
        bytes[written_count] = '/';
        written_count += 1;
    }
    if (joined->second.byte_count > 0) {
        memcpy(bytes + written_count, joined->second.bytes, joined->second.byte_count);
    }
    list->paths[list->path_count] = (sxml_byte_view){.bytes = bytes, .byte_count = byte_count};
    list->path_count += 1;
    return SXML_STATUS_OK;
}

static void sxml_collection_release_paths(sxml_allocator allocator,
                                          sxml_collection_path_list *list) {
    for (size_t path_index = 0; path_index < list->path_count; path_index += 1) {
        sxml_allocator_release(allocator, (void *)list->paths[path_index].bytes);
    }
    sxml_allocator_release(allocator, list->paths);
    *list = (sxml_collection_path_list){.paths = NULL, .path_count = 0, .path_capacity = 0};
}

/* Byte order, which is code point order for UTF-8. */
static bool sxml_collection_precedes(sxml_byte_view left, sxml_byte_view right) {
    const size_t common_count =
        left.byte_count < right.byte_count ? left.byte_count : right.byte_count;
    const int comparison = common_count == 0 ? 0 : memcmp(left.bytes, right.bytes, common_count);
    return comparison < 0 || (comparison == 0 && left.byte_count < right.byte_count);
}

/* Moves the entry at root_index down the heap of the first heap_count
   paths. */
typedef struct sxml_collection_heap_step {
    size_t root_index;
    size_t heap_count;
} sxml_collection_heap_step;

static void sxml_collection_sift_down(sxml_collection_path_list *list,
                                      sxml_collection_heap_step step) {
    size_t parent_index = step.root_index;
    for (;;) {
        size_t largest_index = parent_index;
        const size_t left_index = 2 * parent_index + 1;
        const size_t right_index = left_index + 1;
        if (left_index < step.heap_count &&
            sxml_collection_precedes(list->paths[largest_index], list->paths[left_index])) {
            largest_index = left_index;
        }
        if (right_index < step.heap_count &&
            sxml_collection_precedes(list->paths[largest_index], list->paths[right_index])) {
            largest_index = right_index;
        }
        if (largest_index == parent_index) {
            return;
        }
        const sxml_byte_view moved = list->paths[parent_index];
        list->paths[parent_index] = list->paths[largest_index];
        list->paths[largest_index] = moved;
        parent_index = largest_index;
    }
}

/* Sorts the paths by name (heap sort: no recursion, no allocation). */
static void sxml_collection_sort_paths(sxml_collection_path_list *list) {
    const size_t count = list->path_count;
    for (size_t root_index = count / 2; root_index > 0; root_index -= 1) {
        sxml_collection_sift_down(
            list, (sxml_collection_heap_step){.root_index = root_index - 1, .heap_count = count});
    }
    for (size_t heap_count = count; heap_count > 1; heap_count -= 1) {
        const sxml_byte_view largest = list->paths[0];
        list->paths[0] = list->paths[heap_count - 1];
        list->paths[heap_count - 1] = largest;
        sxml_collection_sift_down(
            list, (sxml_collection_heap_step){.root_index = 0, .heap_count = heap_count - 1});
    }
}

/* ------------------------------------------------------------ The walk */

/* Lists a directory and its subdirectories (recurse=yes). */
typedef struct sxml_collection_walk {
    sxml_allocator allocator;
    sxml_file_system file_system;
    const sxml_query_collection_query *query;
    /* The files found, relative to the directory. */
    sxml_collection_path_list files;
    /* The subdirectories still to list, relative to the directory. */
    sxml_collection_path_list pending_directories;
    /* The canonical paths of the directories listed, so that a symbolic
       link back up is listed once. */
    sxml_collection_path_list listed_directories;
    /* The directory being listed, relative. */
    sxml_byte_view current_directory;
} sxml_collection_walk;

static sxml_status sxml_collection_add_entry(void *state, const sxml_file_entry *entry) {
    sxml_collection_walk *walk = state;
    const sxml_collection_joined_path joined = {.first = walk->current_directory,
                                                .second = entry->name};
    if (entry->file_kind == SXML_FILE_KIND_FILE &&
        sxml_collection_matches(walk->query->select, entry->name)) {
        return sxml_collection_add_path(walk->allocator, &walk->files, &joined);
    }
    if (entry->file_kind == SXML_FILE_KIND_DIRECTORY && walk->query->recurses) {
        return sxml_collection_add_path(walk->allocator, &walk->pending_directories, &joined);
    }
    return SXML_STATUS_OK;
}

/* Whether the canonical path of directory was listed already; adds it
   when not. */
static sxml_status sxml_collection_note_listed(sxml_collection_walk *walk,
                                               sxml_byte_view directory, bool *was_listed) {
    *was_listed = false;
    sxml_byte_view canonical_path = {.bytes = NULL, .byte_count = 0};
    if (walk->file_system.operations->canonicalize(walk->file_system.state, directory,
                                                   &canonical_path) != SXML_STATUS_OK) {
        /* Not to be told apart: listed by its own path. */
        canonical_path = directory;
    }
    for (size_t path_index = 0; path_index < walk->listed_directories.path_count;
         path_index += 1) {
        if (sxml_byte_view_equals(walk->listed_directories.paths[path_index], canonical_path)) {
            *was_listed = true;
            return SXML_STATUS_OK;
        }
    }
    return sxml_collection_add_path(
        walk->allocator, &walk->listed_directories,
        &(sxml_collection_joined_path){.first = canonical_path,
                                       .second = {.bytes = NULL, .byte_count = 0}});
}

/* The absolute path of a file or directory relative to the collection's
   directory, in allocator memory added to list (its last path). */
static sxml_status sxml_collection_absolute_path(const sxml_collection_walk *walk,
                                                 sxml_byte_view relative_path,
                                                 sxml_collection_path_list *list) {
    return sxml_collection_add_path(
        walk->allocator, list,
        &(sxml_collection_joined_path){.first = walk->query->directory_path,
                                       .second = relative_path});
}

/* Lists the directory (and with recurse=yes its subdirectories) into
   walk->files, sorted. */
static sxml_status sxml_collection_list(sxml_collection_walk *walk) {
    sxml_status status = sxml_collection_add_path(
        walk->allocator, &walk->pending_directories,
        &(sxml_collection_joined_path){.first = {.bytes = NULL, .byte_count = 0},
                                       .second = {.bytes = NULL, .byte_count = 0}});
    sxml_collection_path_list absolute_paths = {.paths = NULL, .path_count = 0,
                                                .path_capacity = 0};
    while (status == SXML_STATUS_OK && walk->pending_directories.path_count > 0) {
        walk->pending_directories.path_count -= 1;
        const sxml_byte_view relative_path =
            walk->pending_directories.paths[walk->pending_directories.path_count];
        status = sxml_collection_absolute_path(walk, relative_path, &absolute_paths);
        bool was_listed = false;
        if (status == SXML_STATUS_OK) {
            status = sxml_collection_note_listed(
                walk, absolute_paths.paths[absolute_paths.path_count - 1], &was_listed);
        }
        if (status == SXML_STATUS_OK && !was_listed) {
            walk->current_directory = relative_path;
            status = walk->file_system.operations->list(
                walk->file_system.state,
                &(sxml_file_list_request){
                    .path = absolute_paths.paths[absolute_paths.path_count - 1],
                    .sink = {.state = walk, .add = sxml_collection_add_entry}});
        }
        sxml_allocator_release(walk->allocator, (void *)relative_path.bytes);
    }
    sxml_collection_release_paths(walk->allocator, &absolute_paths);
    if (status == SXML_STATUS_OK) {
        sxml_collection_sort_paths(&walk->files);
    }
    return status;
}

/* ------------------------------------------------------------ The items */

/* Writes "FILE: description" or "FILE:LINE:COLUMN: description" into
   the resolver's description memory, cut at its end, for the FODC0002
   the collection raises, and makes it the resource's description. */
typedef struct sxml_collection_failure {
    sxml_byte_view file;
    const sxml_diagnostic *diagnostic;
    sxml_byte_view description;
} sxml_collection_failure;

/* Text written into fixed memory, cut at its end. */
typedef struct sxml_collection_text {
    uint8_t *bytes;
    size_t capacity;
    size_t byte_count;
} sxml_collection_text;

static void sxml_collection_append(sxml_collection_text *text, sxml_byte_view part) {
    const size_t room = text->capacity - text->byte_count;
    const size_t copied_count = part.byte_count < room ? part.byte_count : room;
    if (copied_count > 0) {
        memcpy(text->bytes + text->byte_count, part.bytes, copied_count);
        text->byte_count += copied_count;
    }
}

static void sxml_collection_append_decimal(sxml_collection_text *text, size_t value) {
    uint8_t digits[24];
    size_t digit_count = 0;
    do {
        digits[sizeof(digits) - 1 - digit_count] = (uint8_t)('0' + value % 10);
        digit_count += 1;
        value /= 10;
    } while (value > 0);
    sxml_collection_append(text, (sxml_byte_view){.bytes = digits + sizeof(digits) - digit_count,
                                                  .byte_count = digit_count});
}

static void sxml_collection_describe_failure(sxml_query_file_resolver *resolver,
                                             const sxml_collection_failure *failure,
                                             sxml_resource *resource) {
    sxml_collection_text text = {.bytes = resolver->description,
                                 .capacity = sizeof(resolver->description),
                                 .byte_count = 0};
    sxml_collection_append(&text, failure->file);
    if (failure->diagnostic != NULL && failure->diagnostic->location.line_number != 0) {
        sxml_collection_append(&text, SXML_TEXT(":"));
        sxml_collection_append_decimal(&text, failure->diagnostic->location.line_number);
        sxml_collection_append(&text, SXML_TEXT(":"));
        sxml_collection_append_decimal(&text, failure->diagnostic->location.column_number);
    }
    sxml_collection_append(&text, SXML_TEXT(": "));
    sxml_collection_append(&text, failure->description);
    resource->resource_form = SXML_RESOURCE_FORM_NONE;
    resource->description = (sxml_byte_view){.bytes = text.bytes, .byte_count = text.byte_count};
}

/* What one file of the collection gives. */
typedef struct sxml_collection_member {
    sxml_byte_view uri;
    sxml_byte_view path;
} sxml_collection_member;

/* What making the items needs. */
typedef struct sxml_collection_items {
    sxml_query_file_resolver *resolver;
    const sxml_resource_request *request;
    sxml_item *items;
    size_t item_count;
} sxml_collection_items;

/* Appends the xs:anyURI of member (fn:uri-collection) or its document
   (fn:collection) to items; a document that cannot be read or parsed
   ends the collection with resource's NONE unless on-error=ignore. */
static sxml_status sxml_collection_make_item(sxml_collection_items *items,
                                             const sxml_collection_member *member,
                                             sxml_resource *resource) {
    const sxml_cli_invocation *invocation = items->resolver->invocation;
    sxml_processor *processor = items->request->processor;
    sxml_item *item = &items->items[items->item_count];
    if (items->request->resource_kind == SXML_RESOURCE_KIND_URI_COLLECTION) {
        const sxml_status status = sxml_atomic_item_make(
            sxml_processor_get_item_heap(processor),
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_URI),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = {.utf8 = member->uri,
                                                       .codepoint_count = 0,
                                                       .is_ascii = false}}}},
            item);
        if (status == SXML_STATUS_INVALID_UTF8) {
            /* A file name that is not UTF-8 is not in the collection. */
            return SXML_STATUS_OK;
        }
        items->item_count += status == SXML_STATUS_OK ? 1 : 0;
        return status;
    }
    const sxml_byte_view file = sxml_query_file_of_uri(invocation, member->uri);
    const bool ignores_errors = items->resolver->collection_query.ignores_errors;
    sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_cli_file_reader_read(
        invocation->file_reader,
        &(sxml_cli_file_read_request){.path = member->path, .allocator = invocation->allocator},
        &contents);
    if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        if (!ignores_errors) {
            sxml_collection_describe_failure(
                items->resolver,
                &(sxml_collection_failure){.file = file,
                                           .diagnostic = NULL,
                                           .description = SXML_TEXT("The file cannot be read")},
                resource);
        }
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* As fn:doc parses a file: its URI is the document URI and base URI
       (F&O 4.0, 17.1.3: a collection's documents are available documents,
       so fn:doc of the document URI gives the same node, D286), and
       external entities come through the resolver. */
    sxml_diagnostic diagnostic = {.description = {.bytes = NULL, .byte_count = 0},
                                  .location = {.source_name = file,
                                               .line_number = 0,
                                               .column_number = 0,
                                               .byte_offset = 0}};
    status = sxml_processor_parse_document(
        processor,
        &(sxml_xml_parse_request){.input = contents,
                                  .source_name = file,
                                  .document_uri = member->uri,
                                  .base_uri = member->uri,
                                  .encoding_name = {.bytes = NULL, .byte_count = 0},
                                  .diagnostic = &diagnostic,
                                  .entity_resolver = sxml_processor_entity_resolver(processor)},
        item);
    if (status == SXML_STATUS_OK) {
        items->item_count += 1;
    } else if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8 ||
               status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        if (!ignores_errors) {
            sxml_collection_describe_failure(
                items->resolver,
                &(sxml_collection_failure){.file = file,
                                           .diagnostic = &diagnostic,
                                           .description = diagnostic.description.byte_count > 0
                                                              ? diagnostic.description
                                                              : sxml_status_name(status)},
                resource);
        }
        status = SXML_STATUS_OK;
    }
    sxml_allocator_release(invocation->allocator, (void *)contents.bytes);
    return status;
}

/* Appends the items of the files walk found to items; stops at a file
   that makes the collection unavailable (resource's NONE). */
static sxml_status sxml_collection_make_items(sxml_collection_items *items,
                                              const sxml_collection_walk *walk,
                                              sxml_resource *resource) {
    const sxml_allocator allocator = walk->allocator;
    const sxml_query_collection_query *query = walk->query;
    sxml_collection_path_list uris = {.paths = NULL, .path_count = 0, .path_capacity = 0};
    sxml_collection_path_list paths = {.paths = NULL, .path_count = 0, .path_capacity = 0};
    sxml_status status = SXML_STATUS_OK;
    for (size_t file_index = 0; file_index < walk->files.path_count &&
                                status == SXML_STATUS_OK &&
                                resource->resource_form != SXML_RESOURCE_FORM_NONE;
         file_index += 1) {
        const sxml_byte_view relative_path = walk->files.paths[file_index];
        /* The member's URI: the directory's, which ends with "/", and the
           relative path as it is, the way fn:doc maps a file URI to a file
           (query_command.c). */
        status = sxml_collection_add_path(
            allocator, &uris,
            &(sxml_collection_joined_path){.first = query->directory_uri,
                                           .second = relative_path});
        if (status == SXML_STATUS_OK) {
            status = sxml_collection_absolute_path(walk, relative_path, &paths);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_collection_make_item(
                items,
                &(sxml_collection_member){.uri = uris.paths[uris.path_count - 1],
                                          .path = paths.paths[paths.path_count - 1]},
                resource);
        }
    }
    sxml_collection_release_paths(allocator, &uris);
    sxml_collection_release_paths(allocator, &paths);
    return status;
}

/* Makes the items of the files walk found into resource's SEQUENCE. */
static sxml_status sxml_collection_make_sequence(sxml_collection_items *items,
                                                 const sxml_collection_walk *walk,
                                                 sxml_resource *resource) {
    const sxml_allocator allocator = walk->allocator;
    size_t byte_count = 0;
    void *memory = NULL;
    sxml_status status =
        sxml_size_multiply(walk->files.path_count + 1, sizeof(sxml_item), &byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(sxml_item)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    items->items = memory;
    items->item_count = 0;
    resource->resource_form = SXML_RESOURCE_FORM_SEQUENCE;
    status = sxml_collection_make_items(items, walk, resource);
    if (status == SXML_STATUS_OK && resource->resource_form == SXML_RESOURCE_FORM_SEQUENCE) {
        status = sxml_sequence_make_vector(
            sxml_processor_get_item_heap(items->request->processor),
            (sxml_item_view){.items = items->items, .item_count = items->item_count},
            &resource->items);
    }
    if (status != SXML_STATUS_OK) {
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
    }
    for (size_t item_index = 0; item_index < items->item_count; item_index += 1) {
        sxml_item_release(items->items[item_index]);
    }
    sxml_allocator_release(allocator, memory);
    items->items = NULL;
    return status;
}

/* ------------------------------------------------------------ Resolving */

/* The path of a file:// URI of a directory, borrowed from uri: on Windows
   file:///C:/work/ is C:/work/ (RFC 8089, appendix E.2). */
static sxml_byte_view sxml_collection_directory_path(sxml_byte_view uri) {
    const size_t scheme_byte_count = SXML_TEXT("file://").byte_count;
    sxml_byte_view path = {.bytes = uri.bytes + scheme_byte_count,
                           .byte_count = uri.byte_count - scheme_byte_count};
    if (path.byte_count >= 3 && path.bytes[0] == '/' && path.bytes[2] == ':' &&
        ((path.bytes[1] >= 'A' && path.bytes[1] <= 'Z') ||
         (path.bytes[1] >= 'a' && path.bytes[1] <= 'z'))) {
        path.bytes += 1;
        path.byte_count -= 1;
    }
    return path;
}

sxml_status sxml_query_resolve_collection(sxml_query_file_resolver *resolver,
                                          const sxml_resource_request *request,
                                          sxml_resource *resource) {
    const sxml_cli_invocation *invocation = resolver->invocation;
    resource->resource_form = SXML_RESOURCE_FORM_NONE;
    const sxml_byte_view uri = request->is_default_collection
                                   ? resolver->default_collection_uri
                                   : request->absolute_uri;
    if (uri.byte_count == 0) {
        /* XPath 4.0, 2.2.2: the default collection is the host's; without
           --collection there is none (F&O 4.0, 17.1.3: FODC0002). */
        resource->description = SXML_TEXT("There is no default collection; give --collection");
        return SXML_STATUS_OK;
    }
    sxml_query_collection_query *query = &resolver->collection_query;
    sxml_query_read_collection_uri(uri, query);
    if (query->refusal.byte_count > 0) {
        resource->description = query->refusal;
        return SXML_STATUS_OK;
    }
    const sxml_file_system file_system = invocation->file_system;
    if (file_system.operations == NULL) {
        resource->description = SXML_TEXT("This platform's sxml cannot list directories");
        return SXML_STATUS_OK;
    }
    sxml_byte_view directory_uri = {.bytes = NULL, .byte_count = 0};
    sxml_status status =
        sxml_query_normalize_directory_uri(invocation, query->directory_uri, &directory_uri);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    query->directory_uri = directory_uri;
    query->directory_path = sxml_collection_directory_path(directory_uri);
    sxml_file_description description;
    status = file_system.operations->describe(file_system.state, query->directory_path,
                                              &description);
    if (status == SXML_STATUS_OK && description.file_kind != SXML_FILE_KIND_DIRECTORY) {
        resource->description = SXML_TEXT("No such directory");
    } else if (status == SXML_STATUS_OK) {
        sxml_collection_walk walk = {
            .allocator = invocation->allocator,
            .file_system = file_system,
            .query = query,
            .files = {.paths = NULL, .path_count = 0, .path_capacity = 0},
            .pending_directories = {.paths = NULL, .path_count = 0, .path_capacity = 0},
            .listed_directories = {.paths = NULL, .path_count = 0, .path_capacity = 0},
            .current_directory = {.bytes = NULL, .byte_count = 0}};
        status = sxml_collection_list(&walk);
        if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
            resource->description = SXML_TEXT("The directory cannot be listed");
            status = SXML_STATUS_OK;
        } else if (status == SXML_STATUS_OK) {
            sxml_collection_items items = {
                .resolver = resolver, .request = request, .items = NULL, .item_count = 0};
            status = sxml_collection_make_sequence(&items, &walk, resource);
        }
        sxml_collection_release_paths(walk.allocator, &walk.files);
        sxml_collection_release_paths(walk.allocator, &walk.pending_directories);
        sxml_collection_release_paths(walk.allocator, &walk.listed_directories);
    }
    sxml_allocator_release(invocation->allocator, (void *)directory_uri.bytes);
    query->directory_uri = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    query->directory_path = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    return status;
}
