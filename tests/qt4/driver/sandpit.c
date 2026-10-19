/*
 * Sandpits of the QT4 driver (sandpit.h; D200).
 *
 * Golden file: tests/qt4/driver/resource_table.c (a host interface the
 * driver gives the test's processor, over memory the driver owns).
 */
#include "sandpit.h"

#include <stdalign.h>
#include <stdbool.h>
#include <string.h>

/* ------------------------------------------------------------ Path lists */

/* Paths back to back in allocator memory, by offset. */
typedef struct sxml_qt4_path_list {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
    size_t *ends;
    size_t path_count;
    size_t path_capacity;
} sxml_qt4_path_list;

static void sxml_qt4_path_list_release(sxml_qt4_path_list *list) {
    sxml_allocator_release(list->allocator, list->bytes);
    sxml_allocator_release(list->allocator, list->ends);
    list->bytes = NULL;
    list->ends = NULL;
    list->byte_count = 0;
    list->path_count = 0;
}

/* Grows *memory (of capacity elements of element_byte_count) to hold
   needed elements. */
typedef struct sxml_qt4_growth {
    void **memory;
    size_t *capacity;
    size_t needed;
    size_t element_byte_count;
    size_t alignment;
} sxml_qt4_growth;

static sxml_status sxml_qt4_grow(sxml_allocator allocator, sxml_qt4_growth growth) {
    if (growth.needed <= *growth.capacity) {
        return SXML_STATUS_OK;
    }
    size_t capacity = *growth.capacity == 0 ? 64 : *growth.capacity;
    while (capacity < growth.needed) {
        capacity *= 2;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(capacity, growth.element_byte_count, &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = growth.alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (*growth.memory != NULL) {
        memcpy(memory, *growth.memory, *growth.capacity * growth.element_byte_count);
        sxml_allocator_release(allocator, *growth.memory);
    }
    *growth.memory = memory;
    *growth.capacity = capacity;
    return SXML_STATUS_OK;
}

static sxml_status sxml_qt4_path_list_add(sxml_qt4_path_list *list, sxml_byte_view path) {
    void *bytes = list->bytes;
    sxml_status status =
        sxml_qt4_grow(list->allocator, (sxml_qt4_growth){.memory = &bytes,
                                                         .capacity = &list->byte_capacity,
                                                         .needed = list->byte_count +
                                                                   path.byte_count,
                                                         .element_byte_count = 1,
                                                         .alignment = 1});
    list->bytes = bytes;
    void *ends = list->ends;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_grow(list->allocator,
                               (sxml_qt4_growth){.memory = &ends,
                                                 .capacity = &list->path_capacity,
                                                 .needed = list->path_count + 1,
                                                 .element_byte_count = sizeof(size_t),
                                                 .alignment = alignof(size_t)});
        list->ends = ends;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (path.byte_count > 0) {
        memcpy(list->bytes + list->byte_count, path.bytes, path.byte_count);
    }
    list->byte_count += path.byte_count;
    list->ends[list->path_count] = list->byte_count;
    list->path_count += 1;
    return SXML_STATUS_OK;
}

static sxml_byte_view sxml_qt4_path_list_at(const sxml_qt4_path_list *list, size_t path_index) {
    const size_t start = path_index == 0 ? 0 : list->ends[path_index - 1];
    return (sxml_byte_view){.bytes = list->bytes == NULL ? NULL : list->bytes + start,
                            .byte_count = list->ends[path_index] - start};
}

/* ----------------------------------------------------------------- Paths */

/* A path in a fixed buffer. */
typedef struct sxml_qt4_path_buffer {
    uint8_t bytes[SXML_QT4_SANDPIT_PATH_CAPACITY];
    size_t byte_count;
} sxml_qt4_path_buffer;

/* Writes first, "/" when both are given and first does not end with one,
   and second to *path; false when it does not fit. */
typedef struct sxml_qt4_path_join {
    sxml_byte_view first;
    sxml_byte_view second;
} sxml_qt4_path_join;

static bool sxml_qt4_join(sxml_qt4_path_join join, sxml_qt4_path_buffer *path) {
    const bool needs_separator = join.first.byte_count > 0 && join.second.byte_count > 0 &&
                                 join.first.bytes[join.first.byte_count - 1] != '/';
    const size_t byte_count =
        join.first.byte_count + (needs_separator ? 1U : 0U) + join.second.byte_count;
    if (byte_count >= SXML_QT4_SANDPIT_PATH_CAPACITY) {
        return false;
    }
    size_t written = 0;
    if (join.first.byte_count > 0 && join.first.bytes != NULL) {
        memmove(path->bytes, join.first.bytes, join.first.byte_count);
        written = join.first.byte_count;
    }
    if (needs_separator) {
        path->bytes[written] = '/';
        written += 1;
    }
    if (join.second.byte_count > 0 && join.second.bytes != NULL) {
        memcpy(path->bytes + written, join.second.bytes, join.second.byte_count);
    }
    path->byte_count = byte_count;
    return true;
}

static sxml_byte_view sxml_qt4_path_view(const sxml_qt4_path_buffer *path) {
    return (sxml_byte_view){.bytes = path->bytes, .byte_count = path->byte_count};
}

/* Copies view into a sandpit's fixed directory buffer. */
typedef struct sxml_qt4_directory_slot {
    uint8_t *bytes;
    size_t *byte_count;
} sxml_qt4_directory_slot;

static bool sxml_qt4_keep_directory(sxml_byte_view view, sxml_qt4_directory_slot slot) {
    if (view.byte_count >= SXML_QT4_SANDPIT_PATH_CAPACITY) {
        return false;
    }
    if (view.byte_count > 0) {
        memcpy(slot.bytes, view.bytes, view.byte_count);
    }
    *slot.byte_count = view.byte_count;
    return true;
}

/* ------------------------------------------------------ Directory walks */

/* Collects the entries of one listing. */
typedef struct sxml_qt4_listing {
    sxml_qt4_path_list names;
    /* One kind per name. */
    sxml_file_kind *kinds;
    size_t kind_capacity;
} sxml_qt4_listing;

static sxml_status sxml_qt4_listing_add(void *state, const sxml_file_entry *entry) {
    sxml_qt4_listing *listing = state;
    void *kinds = listing->kinds;
    sxml_status status = sxml_qt4_grow(
        listing->names.allocator,
        (sxml_qt4_growth){.memory = &kinds,
                          .capacity = &listing->kind_capacity,
                          .needed = listing->names.path_count + 1,
                          .element_byte_count = sizeof(sxml_file_kind),
                          .alignment = alignof(sxml_file_kind)});
    listing->kinds = kinds;
    if (status == SXML_STATUS_OK) {
        listing->kinds[listing->names.path_count] = entry->file_kind;
        status = sxml_qt4_path_list_add(&listing->names, entry->name);
    }
    return status;
}

static void sxml_qt4_listing_release(sxml_qt4_listing *listing) {
    sxml_qt4_path_list_release(&listing->names);
    sxml_allocator_release(listing->names.allocator, listing->kinds);
    listing->kinds = NULL;
    listing->kind_capacity = 0;
}

/* Lists directory into *listing, emptied first. */
typedef struct sxml_qt4_list_request {
    sxml_file_system host;
    sxml_byte_view directory;
} sxml_qt4_list_request;

static sxml_status sxml_qt4_list(sxml_qt4_list_request request, sxml_qt4_listing *listing) {
    listing->names.byte_count = 0;
    listing->names.path_count = 0;
    return request.host.operations->list(
        request.host.state,
        &(sxml_file_list_request){.path = request.directory,
                                  .sink = {.state = listing, .add = sxml_qt4_listing_add}});
}

/* Deletes directory and everything below it: every path found, breadth
   first, removed deepest first. Best effort. */
typedef struct sxml_qt4_deletion {
    sxml_file_system host;
    sxml_allocator allocator;
    sxml_byte_view directory;
} sxml_qt4_deletion;

static void sxml_qt4_delete_tree(sxml_qt4_deletion deletion) {
    sxml_qt4_path_list found = {.allocator = deletion.allocator};
    sxml_qt4_listing listing = {.names = {.allocator = deletion.allocator}};
    sxml_qt4_path_buffer path;
    sxml_status status = sxml_qt4_path_list_add(&found, deletion.directory);
    for (size_t found_index = 0; status == SXML_STATUS_OK && found_index < found.path_count;
         found_index += 1) {
        sxml_qt4_path_buffer directory;
        if (!sxml_qt4_join((sxml_qt4_path_join){.first = sxml_qt4_path_list_at(&found, found_index),
                                                .second = {.bytes = NULL, .byte_count = 0}},
                           &directory)) {
            break;
        }
        sxml_file_description description;
        if (deletion.host.operations->describe(deletion.host.state, sxml_qt4_path_view(&directory),
                                               &description) != SXML_STATUS_OK ||
            description.file_kind != SXML_FILE_KIND_DIRECTORY ||
            sxml_qt4_list((sxml_qt4_list_request){.host = deletion.host,
                                                  .directory = sxml_qt4_path_view(&directory)},
                          &listing) != SXML_STATUS_OK) {
            continue;
        }
        for (size_t name_index = 0; status == SXML_STATUS_OK && name_index < listing.names.path_count;
             name_index += 1) {
            if (sxml_qt4_join(
                    (sxml_qt4_path_join){.first = sxml_qt4_path_view(&directory),
                                         .second = sxml_qt4_path_list_at(&listing.names,
                                                                         name_index)},
                    &path)) {
                status = sxml_qt4_path_list_add(&found, sxml_qt4_path_view(&path));
            }
        }
    }
    for (size_t found_index = found.path_count; found_index > 0; found_index -= 1) {
        (void)deletion.host.operations->remove(deletion.host.state,
                                               sxml_qt4_path_list_at(&found, found_index - 1));
    }
    sxml_qt4_listing_release(&listing);
    sxml_qt4_path_list_release(&found);
}

/* Copies the files of source below target (an existing directory). */
typedef struct sxml_qt4_copy_request {
    sxml_file_system host;
    sxml_allocator allocator;
    sxml_byte_view source;
    sxml_byte_view target;
} sxml_qt4_copy_request;

/* Copies one file. */
static sxml_status sxml_qt4_copy_file(const sxml_qt4_copy_request *request,
                                      const sxml_qt4_path_buffer *source,
                                      const sxml_qt4_path_buffer *target) {
    sxml_file_description description;
    sxml_status status = request->host.operations->describe(
        request->host.state, sxml_qt4_path_view(source), &description);
    if (status != SXML_STATUS_OK || description.byte_count > (uint64_t)SIZE_MAX) {
        return status == SXML_STATUS_OK ? SXML_STATUS_LIMIT_EXCEEDED : status;
    }
    void *memory = NULL;
    const size_t byte_count = (size_t)description.byte_count;
    if (byte_count > 0) {
        status = sxml_allocator_allocate(
            request->allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = 1}, &memory);
    }
    if (status == SXML_STATUS_OK && byte_count > 0) {
        status = request->host.operations->read(
            request->host.state,
            &(sxml_file_read_request){.path = sxml_qt4_path_view(source),
                                      .offset = 0,
                                      .memory = {.bytes = memory, .byte_count = byte_count}});
    }
    if (status == SXML_STATUS_OK) {
        status = request->host.operations->write(
            request->host.state,
            &(sxml_file_write_request){.path = sxml_qt4_path_view(target),
                                       .content = {.bytes = memory, .byte_count = byte_count},
                                       .write_mode = SXML_FILE_WRITE_MODE_REPLACE,
                                       .offset = 0});
    }
    sxml_allocator_release(request->allocator, memory);
    return status;
}

static sxml_status sxml_qt4_copy_tree(const sxml_qt4_copy_request *request) {
    sxml_qt4_path_list relatives = {.allocator = request->allocator};
    sxml_qt4_listing listing = {.names = {.allocator = request->allocator}};
    sxml_status status =
        sxml_qt4_path_list_add(&relatives, (sxml_byte_view){.bytes = NULL, .byte_count = 0});
    for (size_t relative_index = 0;
         status == SXML_STATUS_OK && relative_index < relatives.path_count; relative_index += 1) {
        sxml_qt4_path_buffer relative;
        sxml_qt4_path_buffer source_directory;
        if (!sxml_qt4_join(
                (sxml_qt4_path_join){.first = sxml_qt4_path_list_at(&relatives, relative_index),
                                     .second = {.bytes = NULL, .byte_count = 0}},
                &relative) ||
            !sxml_qt4_join((sxml_qt4_path_join){.first = request->source,
                                                .second = sxml_qt4_path_view(&relative)},
                           &source_directory)) {
            status = SXML_STATUS_INPUT_OUTPUT_FAILED;
            break;
        }
        status = sxml_qt4_list((sxml_qt4_list_request){.host = request->host,
                                                       .directory =
                                                           sxml_qt4_path_view(&source_directory)},
                               &listing);
        for (size_t name_index = 0; status == SXML_STATUS_OK && name_index < listing.names.path_count;
             name_index += 1) {
            const sxml_byte_view name = sxml_qt4_path_list_at(&listing.names, name_index);
            sxml_qt4_path_buffer child_relative;
            sxml_qt4_path_buffer source;
            sxml_qt4_path_buffer target;
            if (!sxml_qt4_join((sxml_qt4_path_join){.first = sxml_qt4_path_view(&relative),
                                                    .second = name},
                               &child_relative) ||
                !sxml_qt4_join((sxml_qt4_path_join){.first = request->source,
                                                    .second = sxml_qt4_path_view(&child_relative)},
                               &source) ||
                !sxml_qt4_join((sxml_qt4_path_join){.first = request->target,
                                                    .second = sxml_qt4_path_view(&child_relative)},
                               &target)) {
                status = SXML_STATUS_INPUT_OUTPUT_FAILED;
                break;
            }
            if (listing.kinds[name_index] == SXML_FILE_KIND_DIRECTORY) {
                status = request->host.operations->create_directory(request->host.state,
                                                                    sxml_qt4_path_view(&target));
                if (status == SXML_STATUS_OK) {
                    status = sxml_qt4_path_list_add(&relatives,
                                                    sxml_qt4_path_view(&child_relative));
                }
            } else if (listing.kinds[name_index] == SXML_FILE_KIND_FILE) {
                status = sxml_qt4_copy_file(request, &source, &target);
            }
        }
    }
    sxml_qt4_listing_release(&listing);
    sxml_qt4_path_list_release(&relatives);
    return status;
}

/* ------------------------------------------------- The test's file system */

static sxml_status sxml_qt4_sandpit_describe(void *state, sxml_byte_view path,
                                             sxml_file_description *description) {
    const sxml_qt4_sandpit *sandpit = state;
    return sandpit->host.operations->describe(sandpit->host.state, path, description);
}

static sxml_status sxml_qt4_sandpit_read(void *state, const sxml_file_read_request *request) {
    const sxml_qt4_sandpit *sandpit = state;
    return sandpit->host.operations->read(sandpit->host.state, request);
}

static sxml_status sxml_qt4_sandpit_write(void *state, const sxml_file_write_request *request) {
    const sxml_qt4_sandpit *sandpit = state;
    return sandpit->host.operations->write(sandpit->host.state, request);
}

static sxml_status sxml_qt4_sandpit_list(void *state, const sxml_file_list_request *request) {
    const sxml_qt4_sandpit *sandpit = state;
    return sandpit->host.operations->list(sandpit->host.state, request);
}

static sxml_status sxml_qt4_sandpit_create_directory(void *state, sxml_byte_view path) {
    const sxml_qt4_sandpit *sandpit = state;
    return sandpit->host.operations->create_directory(sandpit->host.state, path);
}

static sxml_status sxml_qt4_sandpit_remove(void *state, sxml_byte_view path) {
    const sxml_qt4_sandpit *sandpit = state;
    return sandpit->host.operations->remove(sandpit->host.state, path);
}

static sxml_status sxml_qt4_sandpit_move(void *state, const sxml_file_move_request *request) {
    const sxml_qt4_sandpit *sandpit = state;
    return sandpit->host.operations->move(sandpit->host.state, request);
}

static sxml_status sxml_qt4_sandpit_create_temporary(void *state,
                                                     const sxml_file_temporary_request *request,
                                                     sxml_byte_view *created_path) {
    const sxml_qt4_sandpit *sandpit = state;
    return sandpit->host.operations->create_temporary(sandpit->host.state, request, created_path);
}

static sxml_status sxml_qt4_sandpit_get_directory(void *state,
                                                  sxml_file_directory_kind directory_kind,
                                                  sxml_byte_view *path) {
    const sxml_qt4_sandpit *sandpit = state;
    *path = directory_kind == SXML_FILE_DIRECTORY_KIND_CURRENT
                ? (sxml_byte_view){.bytes = sandpit->current,
                                   .byte_count = sandpit->current_byte_count}
                : (sxml_byte_view){.bytes = sandpit->temporary,
                                   .byte_count = sandpit->temporary_byte_count};
    return SXML_STATUS_OK;
}

static sxml_status sxml_qt4_sandpit_canonicalize(void *state, sxml_byte_view path,
                                                 sxml_byte_view *canonical_path) {
    const sxml_qt4_sandpit *sandpit = state;
    return sandpit->host.operations->canonicalize(sandpit->host.state, path, canonical_path);
}

/* The table is per host conventions; the driver's host is POSIX (the
   platform layer), whose conventions these are. */
static const sxml_file_system_operations sxml_qt4_sandpit_operations = {
    .directory_separator = SXML_BYTE_VIEW_INITIALIZER("/"),
    .path_separator = SXML_BYTE_VIEW_INITIALIZER(":"),
    .line_separator = SXML_BYTE_VIEW_INITIALIZER("\n"),
    .describe = sxml_qt4_sandpit_describe,
    .read = sxml_qt4_sandpit_read,
    .write = sxml_qt4_sandpit_write,
    .list = sxml_qt4_sandpit_list,
    .create_directory = sxml_qt4_sandpit_create_directory,
    .remove = sxml_qt4_sandpit_remove,
    .move = sxml_qt4_sandpit_move,
    .create_temporary = sxml_qt4_sandpit_create_temporary,
    .get_directory = sxml_qt4_sandpit_get_directory,
    .canonicalize = sxml_qt4_sandpit_canonicalize,
};

/* ------------------------------------------------------------- Lifecycle */

sxml_status sxml_qt4_sandpit_create(const sxml_qt4_sandpit_request *request,
                                    sxml_qt4_sandpit *sandpit) {
    memset(sandpit, 0, sizeof(*sandpit));
    sandpit->host = request->host;
    sandpit->allocator = request->allocator;
    const sxml_file_system_operations *host = request->host.operations;
    if (host == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_byte_view directory = {.bytes = NULL, .byte_count = 0};
    sxml_status status =
        host->get_directory(request->host.state, SXML_FILE_DIRECTORY_KIND_TEMPORARY, &directory);
    sxml_qt4_path_buffer temporary_root;
    if (status == SXML_STATUS_OK &&
        !sxml_qt4_join((sxml_qt4_path_join){.first = directory,
                                            .second = {.bytes = NULL, .byte_count = 0}},
                       &temporary_root)) {
        status = SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    sxml_byte_view created = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = host->create_temporary(
            request->host.state,
            &(sxml_file_temporary_request){.directory = sxml_qt4_path_view(&temporary_root),
                                           .prefix = SXML_BYTE_VIEW_FROM_LITERAL("sxml-qt4-"),
                                           .suffix = {.bytes = NULL, .byte_count = 0},
                                           .is_directory = true},
            &created);
    }
    if (status == SXML_STATUS_OK &&
        !sxml_qt4_keep_directory(created, (sxml_qt4_directory_slot){
                                              .bytes = sandpit->root,
                                              .byte_count = &sandpit->root_byte_count})) {
        status = SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    const sxml_byte_view root = {.bytes = sandpit->root, .byte_count = sandpit->root_byte_count};
    sxml_qt4_path_buffer path;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_join((sxml_qt4_path_join){.first = root,
                                                    .second = SXML_BYTE_VIEW_FROM_LITERAL("sandpit")},
                               &path) &&
                         sxml_qt4_keep_directory(sxml_qt4_path_view(&path),
                                                 (sxml_qt4_directory_slot){
                                                     .bytes = sandpit->current,
                                                     .byte_count = &sandpit->current_byte_count})
                     ? host->create_directory(request->host.state, sxml_qt4_path_view(&path))
                     : SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_join((sxml_qt4_path_join){.first = root,
                                                    .second = SXML_BYTE_VIEW_FROM_LITERAL("temp")},
                               &path) &&
                         sxml_qt4_keep_directory(sxml_qt4_path_view(&path),
                                                 (sxml_qt4_directory_slot){
                                                     .bytes = sandpit->temporary,
                                                     .byte_count = &sandpit->temporary_byte_count})
                     ? host->create_directory(request->host.state, sxml_qt4_path_view(&path))
                     : SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    sxml_qt4_path_buffer source;
    if (status == SXML_STATUS_OK) {
        sxml_byte_view current = {.bytes = NULL, .byte_count = 0};
        const bool is_absolute = request->source_directory.byte_count > 0 &&
                                 request->source_directory.bytes[0] == '/';
        if (!is_absolute) {
            status = host->get_directory(request->host.state, SXML_FILE_DIRECTORY_KIND_CURRENT,
                                         &current);
        }
        if (status == SXML_STATUS_OK &&
            !sxml_qt4_join((sxml_qt4_path_join){.first = current,
                                                .second = request->source_directory},
                           &source)) {
            status = SXML_STATUS_INPUT_OUTPUT_FAILED;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_copy_tree(&(sxml_qt4_copy_request){
            .host = request->host,
            .allocator = request->allocator,
            .source = sxml_qt4_path_view(&source),
            .target = {.bytes = sandpit->current, .byte_count = sandpit->current_byte_count}});
    }
    if (status != SXML_STATUS_OK) {
        sxml_qt4_sandpit_destroy(sandpit);
        return status;
    }
    sandpit->file_system =
        (sxml_file_system){.state = sandpit, .operations = &sxml_qt4_sandpit_operations};
    return SXML_STATUS_OK;
}

void sxml_qt4_sandpit_destroy(sxml_qt4_sandpit *sandpit) {
    if (sandpit->root_byte_count > 0 && sandpit->host.operations != NULL) {
        sxml_qt4_delete_tree((sxml_qt4_deletion){
            .host = sandpit->host,
            .allocator = sandpit->allocator,
            .directory = {.bytes = sandpit->root, .byte_count = sandpit->root_byte_count}});
    }
    sandpit->root_byte_count = 0;
    sandpit->file_system = (sxml_file_system){.state = NULL, .operations = NULL};
}
