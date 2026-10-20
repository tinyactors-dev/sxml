/*
 * The library loader of the sxml program (platform.h; cli_command.h,
 * sxml_cli_library_loader): loads the shared libraries --code-language
 * names (glossary `language_library`, packet 7.23, D288) with dlopen and
 * dlsym on POSIX systems and LoadLibraryW and GetProcAddress on Windows.
 * WASI has no shared libraries: there the loader has no operations.
 * Libraries stay loaded until the program ends: the languages they hold
 * are used until then.
 *
 * Golden file: apps/sxml/platform_file_writer.c (a platform interface
 * over POSIX and Windows calls, with its state in caller memory).
 */
#include "platform.h"

#include <assert.h>
#include <string.h>

#if defined(_WIN32)
#include <stdio.h>
#include <windows.h>
#elif !defined(__wasi__)
#include <dlfcn.h>
#endif

#if !defined(__wasi__)

/* Copies text into the buffer of capacity bytes with a NUL; false when it
   does not fit or holds a NUL. */
typedef struct sxml_platform_text_copy {
    char *buffer;
    size_t capacity;
} sxml_platform_text_copy;

static bool sxml_platform_copy_text(sxml_platform_text_copy copy, sxml_byte_view text) {
    if (text.byte_count + 1 > copy.capacity ||
        (text.byte_count > 0 && memchr(text.bytes, 0, text.byte_count) != NULL)) {
        return false;
    }
    if (text.byte_count > 0) {
        memcpy(copy.buffer, text.bytes, text.byte_count);
    }
    copy.buffer[text.byte_count] = '\0';
    return true;
}

static void sxml_platform_set_reason(sxml_platform_library_loader_state *state,
                                     sxml_cli_library_function_lookup *lookup,
                                     const char *reason) {
    (void)state;
    lookup->function = NULL;
    lookup->reason = reason == NULL ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                                    : sxml_byte_view_from_c_string(reason);
}

static sxml_status sxml_platform_find_function(void *state_pointer,
                                               const sxml_cli_library_function_request *request,
                                               sxml_cli_library_function_lookup *lookup) {
    sxml_platform_library_loader_state *state = state_pointer;
    *lookup = (sxml_cli_library_function_lookup){
        .function = NULL, .reason = {.bytes = NULL, .byte_count = 0}};
    /* A name without a directory is a file in the working directory, not
       one the system's library path finds. */
    const sxml_byte_view path = request->library_path;
    bool has_directory = false;
    for (size_t byte_index = 0; byte_index < path.byte_count; byte_index += 1) {
        has_directory = has_directory || path.bytes[byte_index] == '/' ||
                        path.bytes[byte_index] == '\\';
    }
    const size_t prefix_count = has_directory ? 0 : 2;
    if (!has_directory) {
        memcpy(state->path.text, "./", 2);
    }
    if (!sxml_platform_copy_text(
            (sxml_platform_text_copy){.buffer = state->path.text + prefix_count,
                                      .capacity = sizeof(state->path.text) - prefix_count},
            path) ||
        !sxml_platform_copy_text((sxml_platform_text_copy){.buffer = state->function_name,
                                                           .capacity = sizeof(state->function_name)},
                                 request->function_name)) {
        sxml_platform_set_reason(state, lookup, "the name is too long");
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
#if defined(_WIN32)
    wchar_t wide_path[SXML_PLATFORM_PATH_CAPACITY];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, state->path.text, -1, wide_path,
                            (int)SXML_PLATFORM_PATH_CAPACITY) == 0) {
        sxml_platform_set_reason(state, lookup, "the name is not UTF-8");
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    HMODULE library = LoadLibraryW(wide_path);
    if (library == NULL) {
        (void)snprintf(state->reason, sizeof(state->reason), "LoadLibrary failed with error %lu",
                       (unsigned long)GetLastError());
        sxml_platform_set_reason(state, lookup, state->reason);
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    FARPROC symbol = GetProcAddress(library, state->function_name);
    if (symbol == NULL) {
        (void)snprintf(state->reason, sizeof(state->reason), "no function %s in the library",
                       state->function_name);
        sxml_platform_set_reason(state, lookup, state->reason);
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    static_assert(sizeof(symbol) == sizeof(lookup->function), "function pointers");
    memcpy((void *)&lookup->function, (const void *)&symbol, sizeof(lookup->function));
#else
    void *library = dlopen(state->path.text, RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        sxml_platform_set_reason(state, lookup, dlerror());
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    (void)dlerror();
    void *symbol = dlsym(library, state->function_name);
    if (symbol == NULL) {
        sxml_platform_set_reason(state, lookup, dlerror());
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    /* POSIX (dlsym) lets an object pointer hold a function's address. */
    static_assert(sizeof(symbol) == sizeof(lookup->function), "function pointers");
    memcpy((void *)&lookup->function, (const void *)&symbol, sizeof(lookup->function));
#endif
    return SXML_STATUS_OK;
}

static const sxml_cli_library_loader_operations sxml_platform_library_loader_operations = {
    .find_function = sxml_platform_find_function,
};

sxml_cli_library_loader sxml_platform_library_loader(sxml_platform_library_loader_state *state) {
    return (sxml_cli_library_loader){.state = state,
                                     .operations = &sxml_platform_library_loader_operations};
}

#else

sxml_cli_library_loader sxml_platform_library_loader(sxml_platform_library_loader_state *state) {
    (void)state;
    return (sxml_cli_library_loader){.state = NULL, .operations = NULL};
}

#endif
