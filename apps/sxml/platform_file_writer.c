/*
 * The platform layer's file writer (platform.h; cli_command.h,
 * sxml_cli_file_writer): the files --output, --result-directory and
 * --allow-put name (docs/CLI.md, "No hidden side effects"). With stdio
 * (platform_stdio.c) and the EXPath File system (platform_file_system.c)
 * the only code in apps/ that touches files
 * (tools/ast-grep/rules/input-output-outside-platform-layer.yml).
 *
 * Every write goes to a new temporary file beside the target, created
 * exclusively (POSIX open with O_EXCL; C11 fopen mode "x" on Windows), and
 * is moved into place in one step when committed:
 * - replacing (--force): rename, which POSIX.1-2008 makes atomic, and
 *   MoveFileEx with MOVEFILE_REPLACE_EXISTING on Windows;
 * - not replacing: link, which fails with EEXIST rather than replace a
 *   file that appeared since begin, then unlink of the temporary name; where
 *   link is refused (a file system without hard links, a WASI runtime) the
 *   target is looked up once more and the file renamed. Windows' rename
 *   never replaces a file.
 */
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
/* POSIX.1-2008 with the XSI option: fdopen, fsync, link. */
#define _XOPEN_SOURCE 700 /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming) */
#endif

#include "platform.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* One write in progress, in the request's allocator memory. */
typedef struct sxml_platform_file_handle {
    sxml_allocator allocator;
    FILE *file;
    bool replaces_existing;
    bool has_failed;
    sxml_platform_path target_path;
    sxml_platform_path temporary_path;
} sxml_platform_file_handle;

/* The longest decimal of the temporary counter, and the rest of the
   temporary name: "." before the file's name, ".sxml-" and ".tmp" after
   it. */
enum { SXML_PLATFORM_TEMPORARY_EXTRA_BYTE_COUNT = 1 + 6 + 20 + 4 };

/* Writes path as a C string to *text; false for a path with a NUL byte,
   none at all, or one too long. */
static bool sxml_platform_copy_text(sxml_byte_view path, sxml_platform_path *text) {
    if (path.byte_count == 0 || path.byte_count >= SXML_PLATFORM_PATH_CAPACITY ||
        memchr(path.bytes, 0, path.byte_count) != NULL) {
        return false;
    }
    memcpy(text->text, path.bytes, path.byte_count);
    text->text[path.byte_count] = '\0';
    return true;
}

static sxml_cli_file_kind sxml_platform_kind_of(const sxml_platform_path *file) {
    const char *path = file->text;
#if defined(_WIN32)
    struct _stat information;
    if (_stat(path, &information) != 0) {
        return SXML_CLI_FILE_KIND_NONE;
    }
    if ((information.st_mode & _S_IFMT) == _S_IFDIR) {
        return SXML_CLI_FILE_KIND_DIRECTORY;
    }
    return (information.st_mode & _S_IFMT) == _S_IFREG ? SXML_CLI_FILE_KIND_FILE
                                                       : SXML_CLI_FILE_KIND_OTHER;
#else
    struct stat information;
    if (stat(path, &information) != 0) {
        /* A dangling symbolic link still names something. */
        return lstat(path, &information) == 0 ? SXML_CLI_FILE_KIND_OTHER
                                              : SXML_CLI_FILE_KIND_NONE;
    }
    if (S_ISDIR(information.st_mode)) {
        return SXML_CLI_FILE_KIND_DIRECTORY;
    }
    return S_ISREG(information.st_mode) ? SXML_CLI_FILE_KIND_FILE : SXML_CLI_FILE_KIND_OTHER;
#endif
}

static sxml_status sxml_platform_find_kind(void *state, sxml_byte_view path,
                                           sxml_cli_file_kind *kind) {
    sxml_platform_file_writer_state *writer = state;
    *kind = SXML_CLI_FILE_KIND_NONE;
    if (!sxml_platform_copy_text(path, &writer->path)) {
        return SXML_STATUS_OK;
    }
    *kind = sxml_platform_kind_of(&writer->path);
    return SXML_STATUS_OK;
}

static sxml_status sxml_platform_create_directory(void *state, sxml_byte_view path) {
    sxml_platform_file_writer_state *writer = state;
    if (!sxml_platform_copy_text(path, &writer->path)) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
#if defined(_WIN32)
    const bool is_created = _mkdir(writer->path.text) == 0;
#else
    const bool is_created = mkdir(writer->path.text, 0777) == 0;
#endif
    if (is_created || sxml_platform_kind_of(&writer->path) == SXML_CLI_FILE_KIND_DIRECTORY) {
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static sxml_status sxml_platform_write_bytes(void *state, sxml_byte_view bytes) {
    sxml_platform_file_handle *handle = state;
    if (handle->has_failed ||
        fwrite(bytes.bytes, 1, bytes.byte_count, handle->file) != bytes.byte_count) {
        handle->has_failed = true;
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    return SXML_STATUS_OK;
}

static const sxml_cli_output_stream_operations sxml_platform_file_stream_operations = {
    .write = sxml_platform_write_bytes,
};

/* Bytes to append to a text. */
typedef struct sxml_platform_text_part {
    const char *bytes;
    size_t byte_count;
} sxml_platform_text_part;

/* Appends part to text at *written_count (the caller checked the room). */
static void sxml_platform_append(sxml_platform_path *text, size_t *written_count,
                                 sxml_platform_text_part part) {
    for (size_t byte_index = 0; byte_index < part.byte_count; byte_index += 1) {
        text->text[*written_count] = part.bytes[byte_index];
        *written_count += 1;
    }
}

/* Writes the temporary name of handle's target with counter into
   handle->temporary_path: the target's directory, then
   ".NAME.sxml-COUNTER.tmp". */
static void sxml_platform_name_temporary(sxml_platform_file_handle *handle, uint64_t counter) {
    const char *target = handle->target_path.text;
    const char *slash = strrchr(target, '/');
#if defined(_WIN32)
    const char *backslash = strrchr(target, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) {
        slash = backslash;
    }
#endif
    const size_t directory_byte_count = slash == NULL ? 0 : (size_t)(slash - target) + 1;
    sxml_platform_path *temporary = &handle->temporary_path;
    size_t written_count = 0;
    sxml_platform_append(temporary, &written_count,
                         (sxml_platform_text_part){.bytes = target,
                                                   .byte_count = directory_byte_count});
    sxml_platform_append(temporary, &written_count,
                         (sxml_platform_text_part){.bytes = ".", .byte_count = 1});
    sxml_platform_append(
        temporary, &written_count,
        (sxml_platform_text_part){.bytes = target + directory_byte_count,
                                  .byte_count = strlen(target) - directory_byte_count});
    sxml_platform_append(temporary, &written_count,
                         (sxml_platform_text_part){.bytes = ".sxml-", .byte_count = 6});
    char digits[20];
    size_t digit_count = 0;
    do {
        digits[sizeof digits - 1 - digit_count] = (char)('0' + (counter % 10));
        digit_count += 1;
        counter /= 10;
    } while (counter > 0 && digit_count < sizeof digits);
    sxml_platform_append(temporary, &written_count,
                         (sxml_platform_text_part){.bytes = digits + sizeof digits - digit_count,
                                                   .byte_count = digit_count});
    sxml_platform_append(temporary, &written_count,
                         (sxml_platform_text_part){.bytes = ".tmp", .byte_count = 4});
    temporary->text[written_count] = '\0';
}

/* Creates the temporary file under a name no file has; NULL when it
   cannot. */
static FILE *sxml_platform_open_temporary(sxml_platform_file_writer_state *writer,
                                          sxml_platform_file_handle *handle) {
    /* A name taken by another process is skipped; a hundred in a row
       means something else is wrong. */
    for (size_t attempt_count = 0; attempt_count < 100; attempt_count += 1) {
        sxml_platform_name_temporary(handle, writer->temporary_counter);
        writer->temporary_counter += 1;
#if defined(_WIN32)
        FILE *file = fopen(handle->temporary_path.text, "wbx");
        if (file != NULL) {
            return file;
        }
        if (sxml_platform_kind_of(&handle->temporary_path) == SXML_CLI_FILE_KIND_NONE) {
            return NULL;
        }
#else
        const int descriptor =
            open(handle->temporary_path.text, O_WRONLY | O_CREAT | O_EXCL, (mode_t)0666);
        if (descriptor >= 0) {
            FILE *file = fdopen(descriptor, "wb");
            if (file == NULL) {
                (void)close(descriptor);
                (void)unlink(handle->temporary_path.text);
            }
            return file;
        }
        if (errno != EEXIST) {
            return NULL;
        }
#endif
    }
    return NULL;
}

static sxml_status sxml_platform_begin(void *state, const sxml_cli_file_write_request *request,
                                       sxml_cli_file_write *write) {
    sxml_platform_file_writer_state *writer = state;
    write->handle = NULL;
    write->refused_existing = false;
    write->stream = (sxml_cli_output_stream){.state = NULL, .operations = NULL};
    if (request->path.byte_count + SXML_PLATFORM_TEMPORARY_EXTRA_BYTE_COUNT >=
        SXML_PLATFORM_PATH_CAPACITY) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        request->allocator,
        (sxml_allocation_request){.byte_count = sizeof(sxml_platform_file_handle),
                                  .alignment = alignof(sxml_platform_file_handle)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_platform_file_handle *handle = memory;
    handle->allocator = request->allocator;
    handle->file = NULL;
    handle->replaces_existing = request->replaces_existing;
    handle->has_failed = false;
    if (!sxml_platform_copy_text(request->path, &handle->target_path)) {
        sxml_allocator_release(request->allocator, memory);
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    const sxml_cli_file_kind kind = sxml_platform_kind_of(&handle->target_path);
    if (kind == SXML_CLI_FILE_KIND_DIRECTORY ||
        (kind != SXML_CLI_FILE_KIND_NONE && !request->replaces_existing)) {
        write->refused_existing = kind != SXML_CLI_FILE_KIND_DIRECTORY;
        sxml_allocator_release(request->allocator, memory);
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    handle->file = sxml_platform_open_temporary(writer, handle);
    if (handle->file == NULL) {
        sxml_allocator_release(request->allocator, memory);
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    write->handle = handle;
    write->stream = (sxml_cli_output_stream){.state = handle,
                                             .operations = &sxml_platform_file_stream_operations};
    return SXML_STATUS_OK;
}

/* Releases the handle of write and empties write. */
static void sxml_platform_end(sxml_cli_file_write *write) {
    sxml_platform_file_handle *handle = write->handle;
    sxml_allocator_release(handle->allocator, handle);
    write->handle = NULL;
    write->stream = (sxml_cli_output_stream){.state = NULL, .operations = NULL};
}

/* What moving a written file into place came to. */
typedef enum sxml_platform_move_outcome {
    SXML_PLATFORM_MOVE_OUTCOME_MOVED,
    /* A file at the target stopped it. */
    SXML_PLATFORM_MOVE_OUTCOME_EXISTS,
    SXML_PLATFORM_MOVE_OUTCOME_FAILED,
} sxml_platform_move_outcome;

/* Moves the closed temporary file to the target. */
static sxml_platform_move_outcome sxml_platform_move_into_place(
    const sxml_platform_file_handle *handle) {
    const char *temporary = handle->temporary_path.text;
    const char *target = handle->target_path.text;
#if defined(_WIN32)
    if (handle->replaces_existing) {
        return MoveFileExA(temporary, target, MOVEFILE_REPLACE_EXISTING) != 0
                   ? SXML_PLATFORM_MOVE_OUTCOME_MOVED
                   : SXML_PLATFORM_MOVE_OUTCOME_FAILED;
    }
    /* The C runtime's rename fails when the target exists. */
    if (rename(temporary, target) == 0) {
        return SXML_PLATFORM_MOVE_OUTCOME_MOVED;
    }
    return sxml_platform_kind_of(&handle->target_path) != SXML_CLI_FILE_KIND_NONE
               ? SXML_PLATFORM_MOVE_OUTCOME_EXISTS
               : SXML_PLATFORM_MOVE_OUTCOME_FAILED;
#else
    if (handle->replaces_existing) {
        return rename(temporary, target) == 0 ? SXML_PLATFORM_MOVE_OUTCOME_MOVED
                                              : SXML_PLATFORM_MOVE_OUTCOME_FAILED;
    }
    if (link(temporary, target) == 0) {
        (void)unlink(temporary);
        return SXML_PLATFORM_MOVE_OUTCOME_MOVED;
    }
    if (errno == EEXIST || sxml_platform_kind_of(&handle->target_path) != SXML_CLI_FILE_KIND_NONE) {
        return SXML_PLATFORM_MOVE_OUTCOME_EXISTS;
    }
    return rename(temporary, target) == 0 ? SXML_PLATFORM_MOVE_OUTCOME_MOVED
                                          : SXML_PLATFORM_MOVE_OUTCOME_FAILED;
#endif
}

static sxml_status sxml_platform_commit(void *state, sxml_cli_file_write *write) {
    (void)state;
    sxml_platform_file_handle *handle = write->handle;
    if (handle == NULL) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    bool is_written = !handle->has_failed && fflush(handle->file) == 0;
#if !defined(_WIN32)
    /* The bytes reach the disk before the name does, so a crash cannot
       leave an empty file at the target. */
    is_written = is_written && fsync(fileno(handle->file)) == 0;
#endif
    is_written = fclose(handle->file) == 0 && is_written;
    handle->file = NULL;
    sxml_platform_move_outcome outcome = SXML_PLATFORM_MOVE_OUTCOME_FAILED;
    if (is_written) {
        outcome = sxml_platform_move_into_place(handle);
    }
    write->refused_existing = outcome == SXML_PLATFORM_MOVE_OUTCOME_EXISTS;
    const bool is_moved = outcome == SXML_PLATFORM_MOVE_OUTCOME_MOVED;
    if (!is_moved) {
        (void)remove(handle->temporary_path.text);
    }
    sxml_platform_end(write);
    return is_moved ? SXML_STATUS_OK : SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static void sxml_platform_abandon(void *state, sxml_cli_file_write *write) {
    (void)state;
    sxml_platform_file_handle *handle = write->handle;
    if (handle == NULL) {
        return;
    }
    if (handle->file != NULL) {
        (void)fclose(handle->file);
    }
    (void)remove(handle->temporary_path.text);
    sxml_platform_end(write);
}

static const sxml_cli_file_writer_operations sxml_platform_file_writer_operations = {
    .find_kind = sxml_platform_find_kind,
    .create_directory = sxml_platform_create_directory,
    .begin = sxml_platform_begin,
    .commit = sxml_platform_commit,
    .abandon = sxml_platform_abandon,
};

sxml_cli_file_writer sxml_platform_file_writer(sxml_platform_file_writer_state *state) {
    state->temporary_counter = 0;
    return (sxml_cli_file_writer){.state = state,
                                  .operations = &sxml_platform_file_writer_operations};
}
