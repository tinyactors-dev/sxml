/*
 * The platform layer's file system (platform.h; packet 7.9, D200): the
 * sxml_file_system interface (include/sxml/processor.h) over POSIX, for
 * the EXPath File feature of `sxml --allow-file-access` and the QT4
 * driver's sandpits. With stdio (platform_stdio.c) the only code in apps/
 * that touches files (tools/ast-grep/rules/input-output-outside-platform-
 * layer.yml).
 *
 * POSIX.1-2008: stat, lstat, opendir, readdir, mkdir, rmdir, unlink,
 * rename, open with O_EXCL, realpath; WASI's libc has them but realpath,
 * so there the canonical path is the normalized absolute path sxml gives.
 * Windows is not implemented yet: initialize returns
 * SXML_STATUS_UNSUPPORTED_FEATURE there (D200).
 */
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
/* POSIX.1-2008 with the XSI option: lstat, realpath, PATH_MAX. */
#define _XOPEN_SOURCE 700 /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming) */
#endif

#include "platform.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)

/* Copies view into buffer as a C string; false when it does not fit or
   has a NUL. */
static bool sxml_platform_copy_path(sxml_byte_view path, sxml_platform_path *buffer) {
    if (path.byte_count >= SXML_PLATFORM_PATH_CAPACITY ||
        (path.byte_count > 0 && memchr(path.bytes, 0, path.byte_count) != NULL)) {
        return false;
    }
    if (path.byte_count > 0) {
        memcpy(buffer->text, path.bytes, path.byte_count);
    }
    buffer->text[path.byte_count] = '\0';
    return true;
}

static sxml_status sxml_platform_describe(void *state, sxml_byte_view path,
                                          sxml_file_description *description) {
    sxml_platform_file_system_state *file_system = state;
    *description = (sxml_file_description){.file_kind = SXML_FILE_KIND_NONE,
                                           .byte_count = 0,
                                           .modified_seconds = 0,
                                           .modified_nanoseconds = 0};
    if (!sxml_platform_copy_path(path, &file_system->first_path)) {
        return SXML_STATUS_OK;
    }
    struct stat information;
    if (stat(file_system->first_path.text, &information) != 0) {
        /* Missing, or not visible: nothing sxml may see (EXPath File 4.0,
           2.4). */
        return SXML_STATUS_OK;
    }
    if (S_ISREG(information.st_mode)) {
        description->file_kind = SXML_FILE_KIND_FILE;
        description->byte_count =
            information.st_size > 0 ? (uint64_t)information.st_size : 0U;
    } else if (S_ISDIR(information.st_mode)) {
        description->file_kind = SXML_FILE_KIND_DIRECTORY;
    } else {
        description->file_kind = SXML_FILE_KIND_OTHER;
    }
    description->modified_seconds = (int64_t)information.st_mtime;
    return SXML_STATUS_OK;
}

static sxml_status sxml_platform_read(void *state, const sxml_file_read_request *request) {
    sxml_platform_file_system_state *file_system = state;
    if (!sxml_platform_copy_path(request->path, &file_system->first_path) ||
        request->offset > (uint64_t)LONG_MAX) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    FILE *file = fopen(file_system->first_path.text, "rb");
    if (file == NULL) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    bool is_read = fseek(file, (long)request->offset, SEEK_SET) == 0;
    if (is_read && request->memory.byte_count > 0) {
        is_read = fread(request->memory.bytes, 1, request->memory.byte_count, file) ==
                  request->memory.byte_count;
    }
    const bool is_closed = fclose(file) == 0;
    return is_read && is_closed ? SXML_STATUS_OK : SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static sxml_status sxml_platform_write(void *state, const sxml_file_write_request *request) {
    sxml_platform_file_system_state *file_system = state;
    if (!sxml_platform_copy_path(request->path, &file_system->first_path) ||
        request->offset > (uint64_t)LONG_MAX) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    const char *mode = "wb";
    if (request->write_mode == SXML_FILE_WRITE_MODE_APPEND) {
        mode = "ab";
    } else if (request->write_mode == SXML_FILE_WRITE_MODE_AT_OFFSET) {
        mode = "r+b";
    }
    FILE *file = fopen(file_system->first_path.text, mode);
    if (file == NULL) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    bool is_written = true;
    if (request->write_mode == SXML_FILE_WRITE_MODE_AT_OFFSET) {
        is_written = fseek(file, (long)request->offset, SEEK_SET) == 0;
    }
    if (is_written && request->content.byte_count > 0) {
        is_written = fwrite(request->content.bytes, 1, request->content.byte_count, file) ==
                     request->content.byte_count;
    }
    const bool is_closed = fclose(file) == 0;
    return is_written && is_closed ? SXML_STATUS_OK : SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static sxml_status sxml_platform_list(void *state, const sxml_file_list_request *request) {
    sxml_platform_file_system_state *file_system = state;
    if (request->path.byte_count == 0) {
        const sxml_file_entry root = {.name = {.bytes = (const uint8_t *)"/", .byte_count = 1},
                                      .file_kind = SXML_FILE_KIND_DIRECTORY};
        return request->sink.add(request->sink.state, &root);
    }
    if (!sxml_platform_copy_path(request->path, &file_system->second_path)) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    DIR *directory = opendir(file_system->second_path.text);
    if (directory == NULL) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    sxml_status status = SXML_STATUS_OK;
    const size_t directory_length = strlen(file_system->second_path.text);
    for (struct dirent *entry = readdir(directory); status == SXML_STATUS_OK && entry != NULL;
         entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        const size_t name_length = strlen(entry->d_name);
        /* The entry's path, to tell what it is. */
        if (directory_length + 1 + name_length >= SXML_PLATFORM_PATH_CAPACITY) {
            status = SXML_STATUS_INPUT_OUTPUT_FAILED;
            break;
        }
        memcpy(file_system->first_path.text, file_system->second_path.text, directory_length);
        size_t length = directory_length;
        if (length == 0 || file_system->first_path.text[length - 1] != '/') {
            file_system->first_path.text[length] = '/';
            length += 1;
        }
        memcpy(file_system->first_path.text + length, entry->d_name, name_length + 1);
        struct stat information;
        sxml_file_kind kind = SXML_FILE_KIND_OTHER;
        if (stat(file_system->first_path.text, &information) == 0) {
            kind = S_ISDIR(information.st_mode)   ? SXML_FILE_KIND_DIRECTORY
                   : S_ISREG(information.st_mode) ? SXML_FILE_KIND_FILE
                                                  : SXML_FILE_KIND_OTHER;
        }
        const sxml_file_entry found = {.name = sxml_byte_view_from_c_string(entry->d_name),
                                       .file_kind = kind};
        status = request->sink.add(request->sink.state, &found);
    }
    if (closedir(directory) != 0 && status == SXML_STATUS_OK) {
        status = SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    return status;
}

static sxml_status sxml_platform_create_directory(void *state, sxml_byte_view path) {
    sxml_platform_file_system_state *file_system = state;
    if (!sxml_platform_copy_path(path, &file_system->first_path) ||
        mkdir(file_system->first_path.text, 0777) != 0) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_platform_remove(void *state, sxml_byte_view path) {
    sxml_platform_file_system_state *file_system = state;
    struct stat information;
    if (!sxml_platform_copy_path(path, &file_system->first_path) ||
        lstat(file_system->first_path.text, &information) != 0) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    const int outcome = S_ISDIR(information.st_mode) ? rmdir(file_system->first_path.text)
                                                     : unlink(file_system->first_path.text);
    return outcome == 0 ? SXML_STATUS_OK : SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static sxml_status sxml_platform_move(void *state, const sxml_file_move_request *request) {
    sxml_platform_file_system_state *file_system = state;
    if (!sxml_platform_copy_path(request->source, &file_system->first_path) ||
        !sxml_platform_copy_path(request->target, &file_system->second_path)) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    /* POSIX rename: the stdio name the platform rule reserves for
       platform_stdio.c is the same function. */
    return rename(file_system->first_path.text, file_system->second_path.text) == 0
               ? SXML_STATUS_OK
               : SXML_STATUS_INPUT_OUTPUT_FAILED;
}

/* The decimal digits of a number, most significant first. */
typedef struct sxml_platform_decimal {
    char digits[24];
    size_t digit_count;
} sxml_platform_decimal;

static sxml_platform_decimal sxml_platform_decimal_of(uint64_t number) {
    char reversed[24];
    size_t digit_count = 0;
    do {
        reversed[digit_count] = (char)('0' + number % 10U);
        digit_count += 1;
        number /= 10U;
    } while (number > 0);
    sxml_platform_decimal decimal = {.digit_count = digit_count};
    for (size_t digit_index = 0; digit_index < digit_count; digit_index += 1) {
        decimal.digits[digit_index] = reversed[digit_count - 1 - digit_index];
    }
    return decimal;
}

static sxml_status sxml_platform_create_temporary(void *state,
                                                  const sxml_file_temporary_request *request,
                                                  sxml_byte_view *created_path) {
    sxml_platform_file_system_state *file_system = state;
    const size_t fixed_length = request->directory.byte_count + 1 + request->prefix.byte_count +
                                request->suffix.byte_count;
    if (fixed_length + 24 >= SXML_PLATFORM_PATH_CAPACITY) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    for (int attempt_number = 0; attempt_number < 1000; attempt_number += 1) {
        file_system->temporary_counter =
            file_system->temporary_counter * 6364136223846793005ULL + 1442695040888963407ULL;
        char *text = file_system->result_path.text;
        size_t length = 0;
        memcpy(text, request->directory.bytes, request->directory.byte_count);
        length += request->directory.byte_count;
        if (length == 0 || text[length - 1] != '/') {
            text[length] = '/';
            length += 1;
        }
        if (request->prefix.byte_count > 0) {
            memcpy(text + length, request->prefix.bytes, request->prefix.byte_count);
            length += request->prefix.byte_count;
        }
        const sxml_platform_decimal decimal =
            sxml_platform_decimal_of(file_system->temporary_counter >> 20U);
        memcpy(text + length, decimal.digits, decimal.digit_count);
        length += decimal.digit_count;
        if (request->suffix.byte_count > 0) {
            memcpy(text + length, request->suffix.bytes, request->suffix.byte_count);
            length += request->suffix.byte_count;
        }
        text[length] = '\0';
        if (memchr(text, 0, length) != NULL) {
            return SXML_STATUS_INPUT_OUTPUT_FAILED;
        }
        int outcome = 0;
        if (request->is_directory) {
            outcome = mkdir(text, 0700);
        } else {
            const int descriptor = open(text, O_WRONLY | O_CREAT | O_EXCL, 0600);
            outcome = descriptor < 0 ? -1 : close(descriptor);
        }
        if (outcome == 0) {
            *created_path = (sxml_byte_view){.bytes = (const uint8_t *)text, .byte_count = length};
            return SXML_STATUS_OK;
        }
        if (errno != EEXIST) {
            return SXML_STATUS_INPUT_OUTPUT_FAILED;
        }
    }
    return SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static sxml_status sxml_platform_get_directory(void *state,
                                               sxml_file_directory_kind directory_kind,
                                               sxml_byte_view *path) {
    sxml_platform_file_system_state *file_system = state;
    *path = sxml_byte_view_from_c_string(directory_kind == SXML_FILE_DIRECTORY_KIND_CURRENT
                                      ? file_system->current_directory.text
                                      : file_system->temporary_directory.text);
    return SXML_STATUS_OK;
}

static sxml_status sxml_platform_canonicalize(void *state, sxml_byte_view path,
                                              sxml_byte_view *canonical_path) {
    sxml_platform_file_system_state *file_system = state;
    if (!sxml_platform_copy_path(path, &file_system->first_path)) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
#if defined(__wasi__)
    memcpy(file_system->result_path.text, file_system->first_path.text, path.byte_count + 1);
#else
    char resolved[PATH_MAX];
    if (realpath(file_system->first_path.text, resolved) == NULL ||
        strlen(resolved) >= SXML_PLATFORM_PATH_CAPACITY) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    memcpy(file_system->result_path.text, resolved, strlen(resolved) + 1);
#endif
    *canonical_path = sxml_byte_view_from_c_string(file_system->result_path.text);
    return SXML_STATUS_OK;
}

static const sxml_file_system_operations sxml_platform_file_system_operations = {
    .directory_separator = {.bytes = (const uint8_t *)"/", .byte_count = 1},
    .path_separator = {.bytes = (const uint8_t *)":", .byte_count = 1},
    .line_separator = {.bytes = (const uint8_t *)"\n", .byte_count = 1},
    .describe = sxml_platform_describe,
    .read = sxml_platform_read,
    .write = sxml_platform_write,
    .list = sxml_platform_list,
    .create_directory = sxml_platform_create_directory,
    .remove = sxml_platform_remove,
    .move = sxml_platform_move,
    .create_temporary = sxml_platform_create_temporary,
    .get_directory = sxml_platform_get_directory,
    .canonicalize = sxml_platform_canonicalize,
};

/* Copies an absolute directory into buffer: the given one, or fallback. */
typedef struct sxml_platform_directory_choice {
    sxml_byte_view given;
    const char *fallback;
} sxml_platform_directory_choice;

static bool sxml_platform_choose_directory(sxml_platform_directory_choice choice,
                                           sxml_platform_path *buffer) {
    if (choice.given.byte_count > 0) {
        return sxml_platform_copy_path(choice.given, buffer) && buffer->text[0] == '/';
    }
    if (choice.fallback == NULL || choice.fallback[0] != '/') {
        return false;
    }
    return sxml_platform_copy_path(sxml_byte_view_from_c_string(choice.fallback), buffer);
}

sxml_status sxml_platform_file_system_initialize(
    sxml_platform_file_system_state *state,
    const sxml_platform_file_system_configuration *configuration,
    sxml_file_system *file_system) {
    *file_system = (sxml_file_system){.state = NULL, .operations = NULL};
    memset(state, 0, sizeof(*state));
    const char *working_directory = NULL;
    if (configuration->current_directory.byte_count == 0) {
        working_directory = getcwd(state->first_path.text, SXML_PLATFORM_PATH_CAPACITY);
    }
    const char *temporary = getenv("TMPDIR");
    if (temporary == NULL || temporary[0] != '/') {
#if defined(__wasi__)
        /* A WASI program sees only the directories its runtime grants:
           temporary files go to the current directory, which the call
           below fills first. */
        temporary = state->current_directory.text;
#else
        temporary = "/tmp";
#endif
    }
    if (!sxml_platform_choose_directory(
            (sxml_platform_directory_choice){.given = configuration->current_directory,
                                             .fallback = working_directory},
            &state->current_directory) ||
        !sxml_platform_choose_directory(
            (sxml_platform_directory_choice){.given = configuration->temporary_directory,
                                             .fallback = temporary},
            &state->temporary_directory)) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    state->temporary_counter = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)state;
    *file_system = (sxml_file_system){.state = state,
                                      .operations = &sxml_platform_file_system_operations};
    return SXML_STATUS_OK;
}

#else

sxml_status sxml_platform_file_system_initialize(
    sxml_platform_file_system_state *state,
    const sxml_platform_file_system_configuration *configuration,
    sxml_file_system *file_system) {
    (void)state;
    (void)configuration;
    *file_system = (sxml_file_system){.state = NULL, .operations = NULL};
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

#endif
