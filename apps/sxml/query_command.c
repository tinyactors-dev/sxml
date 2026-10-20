/*
 * What sxml xpath and sxml xquery share (query_command.h; docs/CLI.md).
 *
 * Moved from command_xpath.c, which it imitates. The library is used
 * through its public API only: one processor (include/sxml/processor.h)
 * whose resource resolver reads local files through the invocation's file
 * reader, so fn:doc sees local files and nothing else (docs/CLI.md, "No
 * hidden side effects"), and the timezone database feature the host's
 * /usr/share/zoneinfo (D209).
 */
#include "query_command.h"

#include "cli_arguments.h"
#include "cldr_data.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/types.h"

#include <stdalign.h>
#include <stdbool.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* As sxml check: entity expansion is bounded, nesting is not. */
enum { SXML_QUERY_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

const sxml_byte_view sxml_query_methods[SXML_QUERY_METHOD_COUNT] = {
    SXML_BYTE_VIEW_INITIALIZER("adaptive"), SXML_BYTE_VIEW_INITIALIZER("xml"),
    SXML_BYTE_VIEW_INITIALIZER("xhtml"),    SXML_BYTE_VIEW_INITIALIZER("html"),
    SXML_BYTE_VIEW_INITIALIZER("text"),     SXML_BYTE_VIEW_INITIALIZER("json"),
    SXML_BYTE_VIEW_INITIALIZER("csv"),
};

static const sxml_serialization_method sxml_query_method_values[SXML_QUERY_METHOD_COUNT] = {
    SXML_SERIALIZATION_METHOD_ADAPTIVE, SXML_SERIALIZATION_METHOD_XML,
    SXML_SERIALIZATION_METHOD_XHTML,    SXML_SERIALIZATION_METHOD_HTML,
    SXML_SERIALIZATION_METHOD_TEXT,     SXML_SERIALIZATION_METHOD_JSON,
    SXML_SERIALIZATION_METHOD_CSV,
};

/* --------------------------------------------------------------- Files */

/* fn:doc reads local files: a URI reference without a scheme is a path
   relative to the working directory, a "file://" URI its path (on
   Windows, file:///C:/work its C:/work), and a "file:" URI without an
   authority (sxml transform's base URI for a relative stylesheet path
   when the working directory is unknown) its relative path. */
static sxml_byte_view sxml_query_local_path(const sxml_resource_request *request) {
    const sxml_byte_view file_scheme = SXML_TEXT("file://");
    const sxml_byte_view relative_scheme = SXML_TEXT("file:");
    sxml_byte_view uri = request->absolute_uri.byte_count > 0 ? request->absolute_uri
                                                              : request->uri_reference;
    if (uri.byte_count > file_scheme.byte_count &&
        memcmp(uri.bytes, file_scheme.bytes, file_scheme.byte_count) == 0) {
        sxml_byte_view path = {.bytes = uri.bytes + file_scheme.byte_count,
                               .byte_count = uri.byte_count - file_scheme.byte_count};
        if (path.byte_count >= 3 && path.bytes[0] == '/' && path.bytes[2] == ':' &&
            ((path.bytes[1] >= 'A' && path.bytes[1] <= 'Z') ||
             (path.bytes[1] >= 'a' && path.bytes[1] <= 'z'))) {
            /* /C:/work: a drive letter (RFC 8089, appendix E.2). */
            path.bytes += 1;
            path.byte_count -= 1;
        }
        return path;
    }
    if (uri.byte_count > relative_scheme.byte_count &&
        memcmp(uri.bytes, relative_scheme.bytes, relative_scheme.byte_count) == 0) {
        return (sxml_byte_view){.bytes = uri.bytes + relative_scheme.byte_count,
                                .byte_count = uri.byte_count - relative_scheme.byte_count};
    }
    for (size_t byte_index = 0; byte_index < uri.byte_count; byte_index += 1) {
        const uint8_t byte = uri.bytes[byte_index];
        if (byte == ':') {
            /* Another scheme: not a local file. */
            return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        }
        if (byte == '/' || byte == '?' || byte == '#') {
            break;
        }
    }
    return uri;
}

/* The timezone database's files (SXML_RESOURCE_KIND_TIMEZONE_DATA,
   D209): urn:sxml:timezone-data:PATH is the host's zoneinfo file PATH. */
#define SXML_QUERY_TIMEZONE_URI_PREFIX "urn:sxml:timezone-data:"
#define SXML_QUERY_ZONEINFO_DIRECTORY "/usr/share/zoneinfo/"

enum { SXML_QUERY_ZONEINFO_PATH_CAPACITY = 320 };

/* Writes the zoneinfo path of a timezone data request into path and
   returns its view; the empty view for a URI that is not one, or a PATH
   with a part "." or ".." (sxml asks for none). */
static sxml_byte_view sxml_query_zoneinfo_path(const sxml_resource_request *request,
                                               uint8_t path[SXML_QUERY_ZONEINFO_PATH_CAPACITY]) {
    const sxml_byte_view prefix = SXML_TEXT(SXML_QUERY_TIMEZONE_URI_PREFIX);
    const sxml_byte_view directory = SXML_TEXT(SXML_QUERY_ZONEINFO_DIRECTORY);
    const sxml_byte_view uri = request->absolute_uri;
    const sxml_byte_view none = {.bytes = NULL, .byte_count = 0};
    if (uri.byte_count <= prefix.byte_count ||
        memcmp(uri.bytes, prefix.bytes, prefix.byte_count) != 0 ||
        uri.byte_count - prefix.byte_count + directory.byte_count >
            SXML_QUERY_ZONEINFO_PATH_CAPACITY) {
        return none;
    }
    const sxml_byte_view name = {.bytes = uri.bytes + prefix.byte_count,
                                 .byte_count = uri.byte_count - prefix.byte_count};
    for (size_t byte_index = 0; byte_index < name.byte_count; byte_index += 1) {
        const bool starts_part = byte_index == 0 || name.bytes[byte_index - 1] == '/';
        if (starts_part && name.bytes[byte_index] == '.') {
            return none;
        }
    }
    memcpy(path, directory.bytes, directory.byte_count);
    memcpy(path + directory.byte_count, name.bytes, name.byte_count);
    return (sxml_byte_view){.bytes = path, .byte_count = directory.byte_count + name.byte_count};
}

/* The CLDR data of the UCA collation feature (SXML_RESOURCE_KIND_COLLATION_
   DATA, D213, D220) and the localization feature (SXML_RESOURCE_KIND_
   LOCALE_DATA, D221): the kind's URI prefix and the path of one of the
   program's CLDR files (cldr_data.h), joined into allocator memory that
   sxml_query_release_file gives back. */
static sxml_status sxml_query_serve_cldr_data(const sxml_query_file_resolver *resolver,
                                              const sxml_resource_request *request,
                                              sxml_resource *resource) {
    const sxml_byte_view prefix = request->resource_kind == SXML_RESOURCE_KIND_COLLATION_DATA
                                      ? SXML_TEXT("urn:sxml:collation-data:")
                                      : SXML_TEXT("urn:sxml:locale-data:");
    const sxml_byte_view uri = request->absolute_uri;
    const sxml_cli_cldr_file_view files = sxml_cli_cldr_data();
    const sxml_cli_cldr_file *found = NULL;
    if (uri.byte_count > prefix.byte_count &&
        memcmp(uri.bytes, prefix.bytes, prefix.byte_count) == 0) {
        const sxml_byte_view path = {.bytes = uri.bytes + prefix.byte_count,
                                     .byte_count = uri.byte_count - prefix.byte_count};
        for (size_t file_index = 0; file_index < files.file_count; file_index += 1) {
            if (sxml_byte_view_equals(path,
                                      sxml_byte_view_from_c_string(files.files[file_index].path))) {
                found = &files.files[file_index];
            }
        }
    }
    if (found == NULL) {
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
        resource->description = SXML_TEXT("sxml has no such CLDR data");
        return SXML_STATUS_OK;
    }
    const sxml_cli_text_chunks data = found->text;
    size_t byte_count = 0;
    for (size_t chunk_index = 0; chunk_index < data.chunk_count; chunk_index += 1) {
        byte_count += strlen(data.chunks[chunk_index]);
    }
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        resolver->invocation->allocator,
        (sxml_allocation_request){.byte_count = byte_count, .alignment = 1}, &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *bytes = memory;
    size_t byte_offset = 0;
    for (size_t chunk_index = 0; chunk_index < data.chunk_count; chunk_index += 1) {
        const size_t chunk_byte_count = strlen(data.chunks[chunk_index]);
        memcpy(bytes + byte_offset, data.chunks[chunk_index], chunk_byte_count);
        byte_offset += chunk_byte_count;
    }
    resource->resource_form = SXML_RESOURCE_FORM_BYTES;
    resource->content = (sxml_byte_view){.bytes = bytes, .byte_count = byte_count};
    return SXML_STATUS_OK;
}

static sxml_status sxml_query_resolve_file(void *state, const sxml_resource_request *request,
                                           sxml_resource *resource) {
    const sxml_query_file_resolver *resolver = state;
    if (request->resource_kind == SXML_RESOURCE_KIND_COLLATION_DATA ||
        request->resource_kind == SXML_RESOURCE_KIND_LOCALE_DATA) {
        return sxml_query_serve_cldr_data(resolver, request, resource);
    }
    if (request->resource_kind == SXML_RESOURCE_KIND_COLLECTION ||
        request->resource_kind == SXML_RESOURCE_KIND_URI_COLLECTION) {
        /* A collection is a SEQUENCE, never a file's bytes
           (include/sxml/processor.h): the program has none, so
           fn:collection and fn:uri-collection raise FODC0002 (F&O 4.0,
           17.1.3, 17.1.4) for every URI, relative or absolute. */
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
        resource->description = SXML_TEXT("The sxml command has no collections");
        return SXML_STATUS_OK;
    }
    uint8_t zoneinfo_path[SXML_QUERY_ZONEINFO_PATH_CAPACITY];
    const sxml_byte_view path = request->resource_kind == SXML_RESOURCE_KIND_TIMEZONE_DATA
                                    ? sxml_query_zoneinfo_path(request, zoneinfo_path)
                                    : sxml_query_local_path(request);
    /* "-" names standard input on the command line only. */
    if (path.byte_count == 0 || sxml_byte_view_equals(path, SXML_TEXT("-"))) {
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
        resource->description = SXML_TEXT("Only local files are available");
        return SXML_STATUS_OK;
    }
    const sxml_cli_file_reader file_reader = resolver->invocation->file_reader;
    if (request->resource_kind == SXML_RESOURCE_KIND_XML_DOCUMENT &&
        sxml_cli_file_reader_streams(file_reader)) {
        /* An XML document as a byte reader (D233): a streamed document is
           parsed as it is read, so sxml transform does not hold its input;
           fn:doc reads it whole, as before. */
        sxml_byte_reader byte_reader = {.state = NULL, .operations = NULL};
        const sxml_status status = sxml_cli_file_reader_open(
            file_reader,
            &(sxml_cli_file_read_request){.path = path,
                                          .allocator = resolver->invocation->allocator},
            &byte_reader);
        if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
            resource->resource_form = SXML_RESOURCE_FORM_NONE;
            resource->description = SXML_TEXT("The file cannot be read");
            return SXML_STATUS_OK;
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        resource->resource_form = SXML_RESOURCE_FORM_BYTE_READER;
        resource->byte_reader = byte_reader;
        return SXML_STATUS_OK;
    }
    sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
    const sxml_status status = sxml_cli_file_reader_read(
        resolver->invocation->file_reader,
        &(sxml_cli_file_read_request){.path = path, .allocator = resolver->invocation->allocator},
        &contents);
    if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
        resource->description = SXML_TEXT("The file cannot be read");
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    resource->resource_form = SXML_RESOURCE_FORM_BYTES;
    resource->content = contents;
    return SXML_STATUS_OK;
}

static void sxml_query_release_file(void *state, const sxml_resource *resource) {
    const sxml_query_file_resolver *resolver = state;
    if (resource->resource_form == SXML_RESOURCE_FORM_BYTES) {
        sxml_allocator_release(resolver->invocation->allocator, (void *)resource->content.bytes);
    } else if (resource->resource_form == SXML_RESOURCE_FORM_BYTE_READER) {
        sxml_cli_file_reader_close(resolver->invocation->file_reader, resource->byte_reader);
    }
}

static const sxml_resource_resolver_operations sxml_query_file_resolver_operations = {
    .resolve = sxml_query_resolve_file,
    .release = sxml_query_release_file,
};

static bool sxml_query_starts_with(sxml_byte_view text, sxml_byte_view prefix) {
    return text.byte_count >= prefix.byte_count &&
           (prefix.byte_count == 0 || memcmp(text.bytes, prefix.bytes, prefix.byte_count) == 0);
}

sxml_status sxml_query_make_file_uri(const sxml_cli_invocation *invocation, sxml_byte_view path,
                                     sxml_byte_view *uri) {
    const sxml_byte_view directory = invocation->working_directory;
    const bool is_absolute = path.byte_count > 0 && path.bytes[0] == '/';
    const bool has_directory = !is_absolute && directory.byte_count > 0;
    sxml_byte_view parts[4] = {
        has_directory || is_absolute ? SXML_TEXT("file://") : SXML_TEXT("file:"),
        {.bytes = NULL, .byte_count = 0},
        {.bytes = NULL, .byte_count = 0},
        path};
    if (has_directory) {
        /* "C:/work" of Windows becomes file:///C:/work. */
        parts[1] = directory.bytes[0] == '/' ? directory : SXML_TEXT("/");
        parts[2] = directory.bytes[0] == '/' ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                                             : directory;
    }
    size_t byte_count = 0;
    for (size_t part_index = 0; part_index < 4; part_index += 1) {
        byte_count += parts[part_index].byte_count;
    }
    const bool needs_separator =
        has_directory && directory.bytes[directory.byte_count - 1] != '/';
    byte_count += needs_separator ? 1 : 0;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        invocation->allocator, (sxml_allocation_request){.byte_count = byte_count, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *bytes = memory;
    size_t written_count = 0;
    for (size_t part_index = 0; part_index < 4; part_index += 1) {
        if (part_index == 3 && needs_separator) {
            bytes[written_count] = '/';
            written_count += 1;
        }
        if (parts[part_index].byte_count > 0) {
            memcpy(bytes + written_count, parts[part_index].bytes, parts[part_index].byte_count);
            written_count += parts[part_index].byte_count;
        }
    }
    *uri = (sxml_byte_view){.bytes = bytes, .byte_count = written_count};
    return SXML_STATUS_OK;
}

sxml_byte_view sxml_query_file_of_uri(const sxml_cli_invocation *invocation, sxml_byte_view uri) {
    sxml_byte_view name = uri;
    const sxml_byte_view directory = invocation->working_directory;
    const sxml_byte_view absolute_scheme = SXML_TEXT("file://");
    const sxml_byte_view relative_scheme = SXML_TEXT("file:");
    if (sxml_query_starts_with(name, absolute_scheme)) {
        name.bytes += absolute_scheme.byte_count;
        name.byte_count -= absolute_scheme.byte_count;
        if (directory.byte_count > 0 && directory.bytes[0] != '/' &&
            sxml_query_starts_with(name, SXML_TEXT("/"))) {
            /* file:///C:/work: a Windows path. */
            name.bytes += 1;
            name.byte_count -= 1;
        }
        if (directory.byte_count > 0 && sxml_query_starts_with(name, directory) &&
            name.byte_count > directory.byte_count &&
            (directory.bytes[directory.byte_count - 1] == '/' ||
             name.bytes[directory.byte_count] == '/')) {
            const size_t skipped_count =
                directory.byte_count + (directory.bytes[directory.byte_count - 1] == '/' ? 0 : 1);
            name.bytes += skipped_count;
            name.byte_count -= skipped_count;
        }
    } else if (sxml_query_starts_with(name, relative_scheme)) {
        name.bytes += relative_scheme.byte_count;
        name.byte_count -= relative_scheme.byte_count;
    }
    return name;
}

/* ------------------------------------------------------- File URIs */

static int sxml_query_hex_value(uint8_t byte) {
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

sxml_status sxml_query_decode_uri_path(const sxml_cli_invocation *invocation,
                                       sxml_byte_view text, sxml_byte_view *path) {
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        invocation->allocator,
        (sxml_allocation_request){.byte_count = text.byte_count + 1, .alignment = 1}, &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *bytes = memory;
    size_t written_count = 0;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        uint8_t byte = text.bytes[byte_index];
        if (byte == '?' || byte == '#' || byte == '\\') {
            sxml_allocator_release(invocation->allocator, memory);
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        if (byte == '%') {
            const int high = byte_index + 2 < text.byte_count
                                 ? sxml_query_hex_value(text.bytes[byte_index + 1])
                                 : -1;
            const int low =
                high >= 0 ? sxml_query_hex_value(text.bytes[byte_index + 2]) : -1;
            /* RFC 3986, 2.1; a decoded "/", "\" or NUL would change the
               path's parts. */
            byte = (uint8_t)(high * 16 + low);
            if (high < 0 || low < 0 || byte == 0 || byte == '/' || byte == '\\') {
                sxml_allocator_release(invocation->allocator, memory);
                return SXML_STATUS_INVALID_ARGUMENT;
            }
            byte_index += 2;
        }
        bytes[written_count] = byte;
        written_count += 1;
    }
    *path = (sxml_byte_view){.bytes = bytes, .byte_count = written_count};
    return SXML_STATUS_OK;
}

sxml_status sxml_query_normalize_directory_uri(const sxml_cli_invocation *invocation,
                                               sxml_byte_view uri, sxml_byte_view *normalized) {
    const sxml_byte_view scheme = SXML_TEXT("file://");
    if (!sxml_query_starts_with(uri, scheme)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        invocation->allocator,
        (sxml_allocation_request){.byte_count = uri.byte_count + 1, .alignment = 1}, &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *bytes = memory;
    memcpy(bytes, scheme.bytes, scheme.byte_count);
    size_t written_count = scheme.byte_count;
    /* The path's parts, as RFC 3986, 5.2.4 removes dot segments; empty
       parts go too, and the result ends with "/". */
    size_t part_start = scheme.byte_count;
    while (part_start <= uri.byte_count) {
        size_t part_end = part_start;
        while (part_end < uri.byte_count && uri.bytes[part_end] != '/') {
            part_end += 1;
        }
        const sxml_byte_view part = {.bytes = uri.bytes + part_start,
                                     .byte_count = part_end - part_start};
        if (sxml_byte_view_equals(part, SXML_TEXT(".."))) {
            /* Back to the "/" before the last part written. */
            while (written_count > scheme.byte_count + 1 && bytes[written_count - 2] != '/') {
                written_count -= 1;
            }
            if (written_count > scheme.byte_count + 1) {
                written_count -= 1;
            }
        } else if (part.byte_count > 0 && !sxml_byte_view_equals(part, SXML_TEXT("."))) {
            if (written_count == scheme.byte_count || bytes[written_count - 1] != '/') {
                bytes[written_count] = '/';
                written_count += 1;
            }
            memcpy(bytes + written_count, part.bytes, part.byte_count);
            written_count += part.byte_count;
            bytes[written_count] = '/';
            written_count += 1;
        }
        part_start = part_end + 1;
    }
    if (written_count == scheme.byte_count) {
        bytes[written_count] = '/';
        written_count += 1;
    }
    *normalized = (sxml_byte_view){.bytes = bytes, .byte_count = written_count};
    return SXML_STATUS_OK;
}

/* Writes bytes to the CLI stream an sxml_query_library_stream wraps. */
static sxml_status sxml_query_write_library_bytes(void *state, sxml_byte_view bytes) {
    const sxml_cli_output_stream *stream = state;
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    return sxml_cli_output_stream_write(*stream, bytes) == SXML_STATUS_OK
               ? SXML_STATUS_OK
               : SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static const sxml_output_stream_operations sxml_query_library_stream_operations = {
    .write = sxml_query_write_library_bytes,
};

sxml_output_stream sxml_query_library_stream(const sxml_cli_output_stream *stream) {
    return (sxml_output_stream){.state = (void *)stream,
                                .operations = &sxml_query_library_stream_operations};
}

/* -------------------------------------------------------------- Output */

static sxml_status sxml_query_write_output(void *state, sxml_byte_view bytes) {
    sxml_cli_writer *writer = state;
    sxml_cli_writer_write(writer, bytes);
    return writer->status == SXML_STATUS_OK ? SXML_STATUS_OK : SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static const sxml_output_stream_operations sxml_query_output_operations = {
    .write = sxml_query_write_output,
};

sxml_status sxml_query_run_write(const sxml_query_run *run, const sxml_query_output *output) {
    sxml_cli_writer *writer = run->result_writer;
    sxml_status status = sxml_serialize_sequence(
        sxml_processor_get_item_heap(run->processor),
        &(sxml_serialization_request){.value = output->result,
                                      .serialization_parameters =
                                          output->serialization_parameters,
                                      .error_record = output->error_record},
        (sxml_output_stream){.state = writer, .operations = &sxml_query_output_operations});
    uint64_t item_count = 0;
    /* The CSV method ends every record with a line feed already
       (Serialization 4.0, CSV output method). */
    const bool ends_lines = output->serialization_parameters.serialization_method ==
                            SXML_SERIALIZATION_METHOD_CSV;
    if (status == SXML_STATUS_OK && !ends_lines &&
        sxml_sequence_count(output->result, NULL, &item_count) == SXML_STATUS_OK &&
        item_count > 0) {
        sxml_cli_writer_write_byte(writer, '\n');
        status =
            writer->status == SXML_STATUS_OK ? SXML_STATUS_OK : SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    return status;
}

bool sxml_query_find_method(const sxml_cli_invocation *invocation,
                            sxml_serialization_method *method) {
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    if (sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = SXML_TEXT("method"), .occurrence_index = 0},
            &match) != SXML_STATUS_OK ||
        match.value.byte_count == 0) {
        return false;
    }
    for (size_t method_index = 0; method_index < SXML_QUERY_METHOD_COUNT; method_index += 1) {
        if (sxml_byte_view_equals(match.value, sxml_query_methods[method_index])) {
            *method = sxml_query_method_values[method_index];
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------- Errors */

sxml_cli_exit_code sxml_query_run_fail(const sxml_query_run *run,
                                       const sxml_query_failure *failure) {
    const sxml_status status = failure->status;
    if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    sxml_cli_writer *error = run->invocation->standard_error;
    const sxml_error_record *error_record = failure->error_record;
    const sxml_diagnostic *diagnostic = &error_record->diagnostic;
    /* An error in an XQuery library module names that module (D148): a
       static error always, a dynamic or type error while its query is
       alive, since the location borrows from the compiled unit (D271). */
    const bool names_its_source = (status == SXML_STATUS_STATIC_ERROR ||
                                   failure->has_live_location) &&
                                  diagnostic->location.line_number != 0 &&
                                  diagnostic->location.source_name.byte_count > 0;
    /* A module's name is its URI, resolved against the static base URI:
       written as the file it names (sxml_query_file_of_uri). */
    sxml_cli_writer_write_escaped(
        error, names_its_source
                   ? sxml_query_file_of_uri(run->invocation, diagnostic->location.source_name)
                   : failure->source_name);
    if (diagnostic->location.line_number != 0) {
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error, diagnostic->location.line_number);
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error, diagnostic->location.column_number);
    }
    sxml_cli_writer_write(error, SXML_TEXT(": "));
    if (sxml_status_has_error_record(status)) {
        const sxml_error_code *code = &error_record->code;
        if (code->namespace_prefix.byte_count > 0) {
            sxml_cli_writer_write_escaped(error, code->namespace_prefix);
            sxml_cli_writer_write_byte(error, ':');
        } else if (code->namespace_uri.byte_count > 0) {
            sxml_cli_writer_write(error, SXML_TEXT("Q{"));
            sxml_cli_writer_write_escaped(error, code->namespace_uri);
            sxml_cli_writer_write_byte(error, '}');
        }
        sxml_cli_writer_write_escaped(error, code->local_name);
        sxml_cli_writer_write(error, SXML_TEXT(": "));
    }
    sxml_cli_writer_write_escaped(error, diagnostic->description.byte_count > 0
                                             ? diagnostic->description
                                             : sxml_status_name(status));
    sxml_cli_writer_write_byte(error, '\n');
    if (sxml_status_has_error_record(status) || status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        return SXML_CLI_EXIT_OPERATION_FAILED;
    }
    return SXML_CLI_EXIT_INTERNAL_ERROR;
}

/* ---------------------------------------------------------- Parameters */

/* True for NAME of --param=NAME=VALUE: an NCName, checked on ASCII (bytes
   from 0x80 are accepted as name characters; XML 1.0 [4], [4a]). */
static bool sxml_query_is_parameter_name(sxml_byte_view name) {
    if (name.byte_count == 0) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < name.byte_count; byte_index += 1) {
        const uint8_t byte = name.bytes[byte_index];
        const bool is_start = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                              byte == '_' || byte >= 0x80;
        const bool is_other = (byte >= '0' && byte <= '9') || byte == '-' || byte == '.';
        if (!is_start && (byte_index == 0 || !is_other)) {
            return false;
        }
    }
    return true;
}

/* One --param split at its first "=". */
typedef struct sxml_query_parameter {
    sxml_byte_view name;
    sxml_byte_view value;
} sxml_query_parameter;

static bool sxml_query_split_parameter(sxml_byte_view text, sxml_query_parameter *parameter) {
    const uint8_t *equals = text.byte_count == 0 ? NULL : memchr(text.bytes, '=', text.byte_count);
    if (equals == NULL) {
        return false;
    }
    const size_t name_byte_count = (size_t)(equals - text.bytes);
    *parameter = (sxml_query_parameter){
        .name = {.bytes = text.bytes, .byte_count = name_byte_count},
        .value = {.bytes = equals + 1, .byte_count = text.byte_count - name_byte_count - 1}};
    return sxml_query_is_parameter_name(parameter->name);
}

/* Reads the --param options into run (declarations and bindings). Returns
   SXML_CLI_EXIT_SUCCESS to go on. */
static sxml_cli_exit_code sxml_query_read_parameters(sxml_query_run *run) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    if (sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = SXML_TEXT("param"), .occurrence_index = 0},
            &match) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    const size_t count = match.occurrence_count;
    if (count == 0) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    void *declaration_memory = NULL;
    void *binding_memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        invocation->allocator,
        (sxml_allocation_request){.byte_count = count * sizeof(sxml_variable_declaration),
                                  .alignment = alignof(sxml_variable_declaration)},
        &declaration_memory);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            invocation->allocator,
            (sxml_allocation_request){.byte_count = count * sizeof(sxml_variable_binding),
                                      .alignment = alignof(sxml_variable_binding)},
            &binding_memory);
        if (status != SXML_STATUS_OK) {
            sxml_allocator_release(invocation->allocator, declaration_memory);
        }
    }
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    run->declarations = declaration_memory;
    run->bindings = binding_memory;
    sxml_item_heap *item_heap = sxml_processor_get_item_heap(run->processor);
    for (size_t parameter_index = 0; parameter_index < count; parameter_index += 1) {
        sxml_cli_option_match occurrence = {.is_present = false, .occurrence_count = 0};
        status = sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = SXML_TEXT("param"),
                                     .occurrence_index = parameter_index},
            &occurrence);
        sxml_query_parameter parameter;
        if (status != SXML_STATUS_OK) {
            return SXML_CLI_EXIT_INTERNAL_ERROR;
        }
        if (!sxml_query_split_parameter(occurrence.value, &parameter)) {
            return sxml_cli_invocation_report_usage_error(
                invocation, SXML_TEXT("--param needs NAME=VALUE with NAME an NCName"));
        }
        sxml_name_identifier name = {.index = 0};
        status = sxml_name_pool_intern_name(
            sxml_processor_get_name_pool(run->processor),
            &(sxml_name_request){.namespace_uri = {.index = 0}, .local_name = parameter.name},
            &name);
        sxml_item value = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_make(
                item_heap,
                &(sxml_atomic_item_request){
                    .type = sxml_builtin_type_identifier(SXML_TYPE_XS_UNTYPED_ATOMIC),
                    .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                              .datum_details = {.string = {.utf8 = parameter.value,
                                                           .codepoint_count = 0,
                                                           .is_ascii = false}}}},
                &value);
        }
        if (status == SXML_STATUS_INVALID_ARGUMENT || status == SXML_STATUS_INVALID_UTF8) {
            return sxml_cli_invocation_report_usage_error(
                invocation, SXML_TEXT("--param VALUE must be text of XML characters"));
        }
        if (status != SXML_STATUS_OK) {
            return SXML_CLI_EXIT_INTERNAL_ERROR;
        }
        for (size_t earlier_index = 0; earlier_index < parameter_index; earlier_index += 1) {
            if (run->declarations[earlier_index].variable_name.index == name.index) {
                sxml_item_release(value);
                return sxml_cli_invocation_report_usage_error(
                    invocation, SXML_TEXT("--param names one variable twice"));
            }
        }
        run->declarations[parameter_index] = (sxml_variable_declaration){
            .variable_name = name,
            .declared_type = {.item_type =
                                  sxml_builtin_type_identifier(SXML_TYPE_XS_UNTYPED_ATOMIC),
                              .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}};
        run->bindings[parameter_index] = (sxml_variable_binding){
            .variable_name = name, .value = sxml_sequence_from_item(value)};
        run->parameter_count = parameter_index + 1;
    }
    return SXML_CLI_EXIT_SUCCESS;
}

/* ------------------------------------------------------- Context item */

sxml_cli_exit_code sxml_query_run_parse_document(sxml_query_run *run, sxml_byte_view file,
                                                 sxml_item *document) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_cli_file_reader_read(
        invocation->file_reader,
        &(sxml_cli_file_read_request){.path = file, .allocator = invocation->allocator},
        &contents);
    if (status != SXML_STATUS_OK) {
        sxml_cli_writer *error = invocation->standard_error;
        sxml_cli_writer_write(error, invocation->program->name);
        sxml_cli_writer_write_byte(error, ' ');
        sxml_cli_writer_write(error, run->command_name);
        sxml_cli_writer_write(error, SXML_TEXT(": cannot read "));
        sxml_cli_writer_write_quoted(error, file);
        sxml_cli_writer_write_byte(error, '\n');
        return status == SXML_STATUS_INPUT_OUTPUT_FAILED ? SXML_CLI_EXIT_INPUT_OUTPUT_ERROR
                                                         : SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_diagnostic diagnostic = {.description = {.bytes = NULL, .byte_count = 0},
                                  .location = {.source_name = file,
                                               .line_number = 0,
                                               .column_number = 0,
                                               .byte_offset = 0}};
    status = sxml_processor_parse_document(
        run->processor,
        &(sxml_xml_parse_request){.input = contents,
                                  .source_name = file,
                                  .document_uri = {.bytes = NULL, .byte_count = 0},
                                  .base_uri = {.bytes = NULL, .byte_count = 0},
                                  .encoding_name = {.bytes = NULL, .byte_count = 0},
                                  .diagnostic = &diagnostic},
        document);
    sxml_allocator_release(invocation->allocator, (void *)contents.bytes);
    if (status == SXML_STATUS_OK) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    sxml_cli_writer *error = invocation->standard_error;
    sxml_cli_writer_write_escaped(error, file);
    if (diagnostic.location.line_number != 0) {
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error, diagnostic.location.line_number);
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error, diagnostic.location.column_number);
    }
    sxml_cli_writer_write(error, SXML_TEXT(": "));
    sxml_cli_writer_write_line(error, diagnostic.description.byte_count > 0
                                          ? diagnostic.description
                                          : sxml_status_name(status));
    if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8 ||
        status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        return SXML_CLI_EXIT_OPERATION_FAILED;
    }
    return SXML_CLI_EXIT_INTERNAL_ERROR;
}

/* Parses the --context-item document. Returns SXML_CLI_EXIT_SUCCESS to go
   on. */
static sxml_cli_exit_code sxml_query_read_context_item(sxml_query_run *run) {
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    const sxml_status status = sxml_cli_arguments_find_option(
        run->invocation->arguments,
        (sxml_cli_option_lookup){.long_name = SXML_TEXT("context-item"), .occurrence_index = 0},
        &match);
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        /* The command has no --context-item (sxml transform: INPUT). */
        return SXML_CLI_EXIT_SUCCESS;
    }
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (!match.is_present) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    const sxml_cli_exit_code exit_code =
        sxml_query_run_parse_document(run, match.value, &run->context_item);
    run->has_context_item = exit_code == SXML_CLI_EXIT_SUCCESS;
    return exit_code;
}

/* ------------------------------------------------------- Written files */

void sxml_query_run_write_prefix(const sxml_query_run *run) {
    sxml_cli_writer *error = run->invocation->standard_error;
    sxml_cli_writer_write(error, run->invocation->program->name);
    sxml_cli_writer_write_byte(error, ' ');
    sxml_cli_writer_write(error, run->command_name);
    sxml_cli_writer_write(error, SXML_TEXT(": "));
}

/* A file that cannot be written: it exists (and --force is not given) or
   the platform refused. */
typedef struct sxml_query_unwritable {
    sxml_byte_view path;
    bool refused_existing;
} sxml_query_unwritable;

static void sxml_query_report_unwritable(const sxml_query_run *run,
                                         sxml_query_unwritable unwritable) {
    sxml_cli_writer *error = run->invocation->standard_error;
    const sxml_byte_view path = unwritable.path;
    sxml_query_run_write_prefix(run);
    if (unwritable.refused_existing) {
        sxml_cli_writer_write_quoted(error, path);
        sxml_cli_writer_write_line(error, SXML_TEXT(" exists; give --force to replace it"));
        return;
    }
    sxml_cli_writer_write(error, SXML_TEXT("cannot write "));
    sxml_cli_writer_write_quoted(error, path);
    sxml_cli_writer_write_byte(error, '\n');
}

/* Makes room for one more file in run->files. */
static sxml_status sxml_query_grow_files(sxml_query_run *run) {
    if (run->file_count < run->file_capacity) {
        return SXML_STATUS_OK;
    }
    const size_t capacity = run->file_capacity == 0 ? 4 : run->file_capacity * 2;
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(capacity, sizeof(sxml_query_file), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            run->invocation->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_query_file)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (run->file_count > 0) {
        memcpy(memory, run->files, run->file_count * sizeof(sxml_query_file));
    }
    sxml_allocator_release(run->invocation->allocator, run->files);
    run->files = memory;
    run->file_capacity = capacity;
    return SXML_STATUS_OK;
}

sxml_cli_exit_code sxml_query_run_begin_file(sxml_query_run *run, sxml_byte_view path,
                                             sxml_cli_output_stream *stream) {
    const sxml_cli_invocation *invocation = run->invocation;
    void *path_memory = NULL;
    sxml_status status = sxml_query_grow_files(run);
    if (status == SXML_STATUS_OK && path.byte_count > 0) {
        status = sxml_allocator_allocate(
            invocation->allocator,
            (sxml_allocation_request){.byte_count = path.byte_count, .alignment = 1},
            &path_memory);
    }
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (path.byte_count > 0) {
        memcpy(path_memory, path.bytes, path.byte_count);
    }
    sxml_query_file *file = &run->files[run->file_count];
    *file = (sxml_query_file){
        .write = {.stream = {.state = NULL, .operations = NULL},
                  .handle = NULL,
                  .refused_existing = false},
        .path = {.bytes = path_memory, .byte_count = path.byte_count}};
    const sxml_cli_file_writer writer = invocation->file_writer;
    status = writer.operations == NULL
                 ? SXML_STATUS_INPUT_OUTPUT_FAILED
                 : writer.operations->begin(
                       writer.state,
                       &(sxml_cli_file_write_request){.path = file->path,
                                                      .replaces_existing = run->replaces_existing,
                                                      .allocator = invocation->allocator},
                       &file->write);
    if (status != SXML_STATUS_OK) {
        sxml_query_report_unwritable(
            run, (sxml_query_unwritable){.path = path,
                                         .refused_existing = file->write.refused_existing});
        sxml_allocator_release(invocation->allocator, path_memory);
        return status == SXML_STATUS_INPUT_OUTPUT_FAILED ? SXML_CLI_EXIT_INPUT_OUTPUT_ERROR
                                                         : SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    run->file_count += 1;
    *stream = file->write.stream;
    return SXML_CLI_EXIT_SUCCESS;
}

/* Commits (on success) or abandons the run's files and releases them. */
static sxml_cli_exit_code sxml_query_run_end_files(sxml_query_run *run,
                                                   sxml_cli_exit_code exit_code) {
    const sxml_cli_invocation *invocation = run->invocation;
    const sxml_cli_file_writer writer = invocation->file_writer;
    if (exit_code != SXML_CLI_EXIT_SUCCESS && run->output_path.byte_count > 0 &&
        run->output_writer.status != SXML_STATUS_OK) {
        /* The result could not be written to the --output file. */
        sxml_query_report_unwritable(
            run, (sxml_query_unwritable){.path = run->output_path, .refused_existing = false});
    }
    for (size_t file_index = 0; file_index < run->file_count; file_index += 1) {
        sxml_query_file *file = &run->files[file_index];
        if (file->write.handle != NULL) {
            if (exit_code == SXML_CLI_EXIT_SUCCESS) {
                if (writer.operations->commit(writer.state, &file->write) != SXML_STATUS_OK) {
                    sxml_query_report_unwritable(
                        run, (sxml_query_unwritable){
                                 .path = file->path,
                                 .refused_existing = file->write.refused_existing});
                    exit_code = SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
                }
            } else {
                writer.operations->abandon(writer.state, &file->write);
            }
        }
        sxml_allocator_release(invocation->allocator, (void *)file->path.bytes);
    }
    sxml_allocator_release(invocation->allocator, run->files);
    run->files = NULL;
    run->file_count = 0;
    run->file_capacity = 0;
    return exit_code;
}

/* The value of the option long_name, and whether it is given; false for
   an option the command does not have. */
static bool sxml_query_find_text_option(const sxml_cli_invocation *invocation,
                                        sxml_byte_view long_name, sxml_byte_view *value) {
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    if (sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = long_name, .occurrence_index = 0},
            &match) != SXML_STATUS_OK ||
        !match.is_present) {
        return false;
    }
    *value = match.value;
    return true;
}

bool sxml_query_has_option(const sxml_cli_invocation *invocation, sxml_byte_view long_name) {
    sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
    return sxml_query_find_text_option(invocation, long_name, &value);
}

/* Whether the command has the option long_name. */
static bool sxml_query_command_declares(const sxml_cli_invocation *invocation,
                                        sxml_byte_view long_name) {
    const sxml_cli_command *command = invocation->command;
    for (size_t option_index = 0; option_index < command->option_count; option_index += 1) {
        if (sxml_byte_view_equals(command->options[option_index].long_name, long_name)) {
            return true;
        }
    }
    return false;
}

/* Reads --output and --force and begins writing the --output file. */
static sxml_cli_exit_code sxml_query_read_output(sxml_query_run *run) {
    const sxml_cli_invocation *invocation = run->invocation;
    run->replaces_existing = sxml_query_has_option(invocation, SXML_TEXT("force"));
    sxml_byte_view path = {.bytes = NULL, .byte_count = 0};
    const bool has_output = sxml_query_find_text_option(invocation, SXML_TEXT("output"), &path);
    if (run->replaces_existing && !has_output &&
        !sxml_query_has_option(invocation, SXML_TEXT("result-directory")) &&
        !sxml_query_has_option(invocation, SXML_TEXT("allow-put"))) {
        sxml_byte_view description =
            SXML_TEXT("--force replaces only files the command writes; give --output");
        if (sxml_query_command_declares(invocation, SXML_TEXT("result-directory"))) {
            description = SXML_TEXT("--force replaces only files the command writes; give "
                                    "--output or --result-directory");
        } else if (sxml_query_command_declares(invocation, SXML_TEXT("allow-put"))) {
            description = SXML_TEXT(
                "--force replaces only files the command writes; give --output or --allow-put");
        }
        return sxml_cli_invocation_report_usage_error(invocation, description);
    }
    if (!has_output) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    if (path.byte_count == 0 || sxml_byte_view_equals(path, SXML_TEXT("-"))) {
        return sxml_cli_invocation_report_usage_error(
            invocation,
            SXML_TEXT("--output needs a file name; without it the result goes to standard output"));
    }
    sxml_cli_output_stream stream = {.state = NULL, .operations = NULL};
    const sxml_cli_exit_code exit_code = sxml_query_run_begin_file(run, path, &stream);
    if (exit_code != SXML_CLI_EXIT_SUCCESS) {
        return exit_code;
    }
    run->output_path = path;
    run->output_writer = sxml_cli_writer_from_stream(stream);
    run->result_writer = &run->output_writer;
    return SXML_CLI_EXIT_SUCCESS;
}

/* -------------------------------------------------------------- The run */

/* The most optional features a run names: every feature of
   sxml_all_optional_features and EXPath File. */
#define SXML_QUERY_FEATURE_CAPACITY ((size_t)16)

/* Whether the command was given --allow-file-access (false for a command
   without the option). */
static bool sxml_query_allows_file_access(const sxml_cli_invocation *invocation) {
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    return sxml_cli_arguments_find_option(
               invocation->arguments,
               (sxml_cli_option_lookup){.long_name = SXML_TEXT("allow-file-access"),
                                        .occurrence_index = 0},
               &match) == SXML_STATUS_OK &&
           match.is_present;
}

/* Binds the conventional prefixes of the optional features' namespaces
   (EXPath Binary 4.0, 1.5; EXPath File 4.0, 1.7): bin and file; without
   --allow-file-access the file: functions are absent (XPST0017). */
static sxml_status sxml_query_bind_feature_namespaces(sxml_query_run *run) {
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(run->processor);
    run->feature_namespace_count = 0;
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    sxml_status status = sxml_name_pool_intern_namespace_prefix(name_pool, SXML_TEXT("bin"),
                                                                &prefix);
    if (status == SXML_STATUS_OK) {
        run->feature_namespaces[0] = (sxml_namespace_binding){
            .namespace_prefix = prefix,
            .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_EXPATH_BINARY)};
        run->feature_namespace_count = 1;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(name_pool, SXML_TEXT("file"), &prefix);
        if (status == SXML_STATUS_OK) {
            run->feature_namespaces[1] = (sxml_namespace_binding){
                .namespace_prefix = prefix,
                .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_EXPATH_FILE)};
            run->feature_namespace_count = 2;
        }
    }
    return status;
}

sxml_cli_exit_code sxml_query_run_start(sxml_query_run *run) {
    const sxml_cli_invocation *invocation = run->invocation;
    run->resolver = (sxml_query_file_resolver){.invocation = invocation};
    run->result_writer = invocation->standard_output;
    const sxml_cli_exit_code output_exit_code = sxml_query_read_output(run);
    if (output_exit_code != SXML_CLI_EXIT_SUCCESS) {
        return output_exit_code;
    }
    /* Every feature; EXPath File, and the platform's files, only with
       --allow-file-access (D184, D200). */
    const sxml_optional_feature_view all_features = sxml_all_optional_features();
    sxml_optional_feature features[SXML_QUERY_FEATURE_CAPACITY];
    size_t feature_count = 0;
    for (size_t feature_index = 0; feature_index < all_features.optional_feature_count &&
                                   feature_count + 1 < SXML_QUERY_FEATURE_CAPACITY;
         feature_index += 1) {
        features[feature_count] = all_features.optional_features[feature_index];
        feature_count += 1;
    }
    const bool allows_file_access = sxml_query_allows_file_access(invocation);
    if (allows_file_access) {
        features[feature_count] = sxml_expath_file_feature();
        feature_count += 1;
    }
    /* F&O 4.0, fn:trace and fn:message: the output goes where the
       implementation decides; like other processors, to standard error. */
    run->trace_stream = (sxml_output_stream){.state = invocation->standard_error,
                                             .operations = &sxml_query_output_operations};
    const sxml_status status = sxml_processor_create(
        invocation->allocator,
        &(sxml_processor_configuration){
            .allocator = invocation->allocator,
            .limits = {.xml_limits = {.maximum_depth = 0,
                                      .maximum_entity_expansion_byte_count =
                                          SXML_QUERY_MAXIMUM_EXPANSION_BYTE_COUNT}},
            .resource_resolver = {.state = &run->resolver,
                                  .operations = &sxml_query_file_resolver_operations},
            .clock = sxml_system_clock(),
            .trace_sink = sxml_query_run_trace_sink(run),
            .optional_features = {.optional_features = features,
                                  .optional_feature_count = feature_count},
            .file_system = allows_file_access
                               ? invocation->file_system
                               : (sxml_file_system){.state = NULL, .operations = NULL}},
        &run->processor);
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (sxml_query_bind_feature_namespaces(run) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    /* XPath 4.0 and XQuery 4.0, C.1: the static base URI is
       implementation-defined; the working directory's makes a relative
       name a file there, for fn:html-doc and fn:unparsed-text too, whose
       relative names need an absolute base (F&O 4.0, 17.1.5). */
    if (invocation->working_directory.byte_count > 0 &&
        sxml_query_make_file_uri(invocation, (sxml_byte_view){.bytes = NULL, .byte_count = 0},
                                 &run->static_base_uri) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (run->parameter_binding == SXML_PARAMETER_BINDING_NONE) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    sxml_cli_exit_code exit_code = sxml_query_read_parameters(run);
    if (exit_code == SXML_CLI_EXIT_SUCCESS) {
        exit_code = sxml_query_read_context_item(run);
    }
    return exit_code;
}

sxml_cli_exit_code sxml_query_run_finish(sxml_query_run *run, sxml_cli_exit_code exit_code) {
    const sxml_allocator allocator = run->invocation->allocator;
    exit_code = sxml_query_run_end_files(run, exit_code);
    for (size_t parameter_index = 0; parameter_index < run->parameter_count;
         parameter_index += 1) {
        sxml_sequence_release(run->bindings[parameter_index].value);
    }
    sxml_allocator_release(allocator, run->declarations);
    sxml_allocator_release(allocator, run->bindings);
    sxml_allocator_release(allocator, (void *)run->static_base_uri.bytes);
    if (run->has_context_item) {
        sxml_item_release(run->context_item);
    }
    if (run->processor != NULL) {
        sxml_item_heap_clear_raised_error(sxml_processor_get_item_heap(run->processor));
        sxml_processor_destroy(run->processor);
    }
    return exit_code;
}

sxml_trace_sink sxml_query_run_trace_sink(const sxml_query_run *run) {
    return sxml_stream_trace_sink(&run->trace_stream);
}

sxml_static_context_configuration sxml_query_run_static_context(const sxml_query_run *run) {
    const bool declares = run->parameter_binding == SXML_PARAMETER_BINDING_DECLARE;
    return (sxml_static_context_configuration){
        .namespace_bindings = {.namespace_bindings = run->feature_namespaces,
                               .namespace_binding_count = run->feature_namespace_count},
        .static_base_uri = run->static_base_uri,
        .variable_declarations = {.variable_declarations = declares ? run->declarations : NULL,
                                  .variable_declaration_count =
                                      declares ? run->parameter_count : 0}};
}

sxml_dynamic_context_configuration sxml_query_run_dynamic_context(const sxml_query_run *run) {
    return (sxml_dynamic_context_configuration){
        .has_context_value = run->has_context_item,
        .context_value = run->has_context_item ? sxml_sequence_from_item(run->context_item)
                                               : sxml_sequence_empty(),
        .variable_bindings = {.variable_bindings = run->bindings,
                              .variable_binding_count = run->parameter_count}};
}
