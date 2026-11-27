/*
 * The DEFLATE vector test (packet 8.7; D333): sxml's inflate and deflate
 * (src/deflate_internal.h) against the vectors of tests/deflate/, which
 * zlib wrote (tools/deflate/make-vectors.py), and the malformed streams
 * written by hand.
 *
 *   deflate_vectors ROOT
 *
 * Checks, each deterministic:
 * - every valid vector inflates to its .raw bytes, consuming the whole
 *   stream (all but the 5 trailer bytes of a -trailer vector);
 * - every malformed vector is refused with the status the manifest names;
 * - every noise vector is refused or inflated, never read out of bounds
 *   (the sanitizers watch), and never consumes more than its bytes;
 * - inflating a valid vector with a limit one byte below its size fails
 *   with SXML_STATUS_LIMIT_EXCEEDED;
 * - every .raw input deflates to a stream that inflates back to it, the
 *   same stream twice, and no larger than the stored-block bound;
 * - for the compressible inputs (text, runs, lengths, unicode), sxml's
 *   stream is at most 1.3 times the size of zlib's level-6 stream;
 * - with an allocator that fails its Nth allocation, for every N until a
 *   call succeeds, inflate and deflate fail with SXML_STATUS_OUT_OF_MEMORY
 *   and leave nothing allocated.
 */
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "deflate_internal.h"
#include "sxml/memory.h"
#include "sxml/status.h"

enum {
    SXML_DEFLATE_VECTORS_MAXIMUM_PATH = 4096,
    SXML_DEFLATE_VECTORS_TRAILER_BYTE_COUNT = 5,
};

typedef struct sxml_deflate_vectors_bytes {
    uint8_t *bytes;
    size_t byte_count;
} sxml_deflate_vectors_bytes;

static void *sxml_deflate_vectors_allocate(size_t byte_count) {
    void *memory = NULL;
    if (sxml_allocator_allocate(sxml_default_allocator(),
                                (sxml_allocation_request){.byte_count = byte_count == 0 ? 1 : byte_count,
                                                          .alignment = alignof(max_align_t)},
                                &memory) != SXML_STATUS_OK) {
        return NULL;
    }
    return memory;
}

static void sxml_deflate_vectors_release(void *memory) {
    sxml_allocator_release(sxml_default_allocator(), memory);
}

/* Reads path whole; false when it cannot. */
static bool sxml_deflate_vectors_read(const char *path, sxml_deflate_vectors_bytes *file) {
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        return false;
    }
    bool is_read = fseek(stream, 0, SEEK_END) == 0;
    long size = is_read ? ftell(stream) : -1;
    is_read = size >= 0 && fseek(stream, 0, SEEK_SET) == 0;
    uint8_t *bytes = is_read ? sxml_deflate_vectors_allocate((size_t)size) : NULL;
    if (bytes != NULL && fread(bytes, 1, (size_t)size, stream) != (size_t)size) {
        sxml_deflate_vectors_release(bytes);
        bytes = NULL;
    }
    (void)fclose(stream);
    if (bytes == NULL) {
        return false;
    }
    *file = (sxml_deflate_vectors_bytes){.bytes = bytes, .byte_count = (size_t)size};
    return true;
}

static sxml_byte_view sxml_deflate_vectors_view(const sxml_deflate_vectors_bytes *file) {
    return (sxml_byte_view){.bytes = file->bytes, .byte_count = file->byte_count};
}

static bool sxml_deflate_vectors_equal(sxml_memory_view output, const sxml_deflate_vectors_bytes *expected) {
    return output.byte_count == expected->byte_count &&
           (output.byte_count == 0 || memcmp(output.bytes, expected->bytes, output.byte_count) == 0);
}

typedef struct sxml_deflate_vectors_run {
    const char *root;
    size_t check_count;
    size_t failure_count;
} sxml_deflate_vectors_run;

typedef struct sxml_deflate_vectors_check {
    const char *name;
    const char *what;
    bool passed;
} sxml_deflate_vectors_check;

static void sxml_deflate_vectors_record(sxml_deflate_vectors_run *run, sxml_deflate_vectors_check check) {
    run->check_count++;
    if (!check.passed) {
        run->failure_count++;
        (void)fprintf(stderr, "FAIL %s: %s\n", check.name, check.what);
    }
}

/* Inflates input with allocator and a limit; releases the output. */
typedef struct sxml_deflate_vectors_inflation {
    sxml_byte_view input;
    sxml_allocator allocator;
    size_t maximum_output_byte_count;
} sxml_deflate_vectors_inflation;

static sxml_status sxml_deflate_vectors_inflate(const sxml_deflate_vectors_inflation *inflation,
                                                sxml_inflate_result *result) {
    return sxml_inflate(&(sxml_inflate_request){.input = inflation->input,
                                                .allocator = inflation->allocator,
                                                .maximum_output_byte_count = inflation->maximum_output_byte_count},
                        result);
}

/* ------------------------------------------------------------ Vectors */

typedef struct sxml_deflate_vectors_vector {
    const char *name;
    const char *kind;
    const char *status;
} sxml_deflate_vectors_vector;

static void sxml_deflate_vectors_check_valid(sxml_deflate_vectors_run *run, const sxml_deflate_vectors_vector *vector) {
    char path[SXML_DEFLATE_VECTORS_MAXIMUM_PATH];
    sxml_deflate_vectors_bytes stream = {0};
    sxml_deflate_vectors_bytes expected = {0};
    (void)snprintf(path, sizeof path, "%s/tests/deflate/%s.deflate", run->root, vector->name);
    bool is_read = sxml_deflate_vectors_read(path, &stream);
    (void)snprintf(path, sizeof path, "%s/tests/deflate/%s.raw", run->root, vector->name);
    is_read = sxml_deflate_vectors_read(path, &expected) && is_read;
    sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){vector->name, "the vector reads", is_read});
    if (!is_read) {
        sxml_deflate_vectors_release(stream.bytes);
        sxml_deflate_vectors_release(expected.bytes);
        return;
    }
    size_t trailer = strstr(vector->name, "-trailer") != NULL ? SXML_DEFLATE_VECTORS_TRAILER_BYTE_COUNT : 0;
    sxml_inflate_result result = {0};
    sxml_status status = sxml_deflate_vectors_inflate(
        &(sxml_deflate_vectors_inflation){.input = sxml_deflate_vectors_view(&stream),
                                          .allocator = sxml_default_allocator(),
                                          .maximum_output_byte_count = SIZE_MAX},
        &result);
    sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){vector->name, "inflates", status == SXML_STATUS_OK});
    if (status == SXML_STATUS_OK) {
        sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){vector->name, "inflates to its .raw bytes",
                                                                      sxml_deflate_vectors_equal(result.output, &expected)});
        sxml_deflate_vectors_record(
            run, (sxml_deflate_vectors_check){vector->name, "consumes the stream and no more",
                                              result.consumed_byte_count == stream.byte_count - trailer});
        sxml_allocator_release(sxml_default_allocator(), result.output.bytes);
    }
    if (expected.byte_count > 0) {
        sxml_inflate_result limited = {0};
        status = sxml_deflate_vectors_inflate(
            &(sxml_deflate_vectors_inflation){.input = sxml_deflate_vectors_view(&stream),
                                              .allocator = sxml_default_allocator(),
                                              .maximum_output_byte_count = expected.byte_count - 1},
            &limited);
        sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){vector->name, "a limit one byte short is refused",
                                                                      status == SXML_STATUS_LIMIT_EXCEEDED});
        if (status == SXML_STATUS_OK) {
            sxml_allocator_release(sxml_default_allocator(), limited.output.bytes);
        }
    }
    sxml_deflate_vectors_release(stream.bytes);
    sxml_deflate_vectors_release(expected.bytes);
}

static void sxml_deflate_vectors_check_refused(sxml_deflate_vectors_run *run, const sxml_deflate_vectors_vector *vector) {
    char path[SXML_DEFLATE_VECTORS_MAXIMUM_PATH];
    sxml_deflate_vectors_bytes stream = {0};
    (void)snprintf(path, sizeof path, "%s/tests/deflate/%s.deflate", run->root, vector->name);
    if (!sxml_deflate_vectors_read(path, &stream)) {
        sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){vector->name, "the vector reads", false});
        return;
    }
    sxml_inflate_result result = {0};
    sxml_status status = sxml_deflate_vectors_inflate(
        &(sxml_deflate_vectors_inflation){.input = sxml_deflate_vectors_view(&stream),
                                          .allocator = sxml_default_allocator(),
                                          .maximum_output_byte_count = SIZE_MAX},
        &result);
    if (strcmp(vector->kind, "malformed") == 0) {
        sxml_byte_view name = sxml_status_name(status);
        bool is_expected = name.byte_count == strlen(vector->status) &&
                           memcmp(name.bytes, vector->status, name.byte_count) == 0;
        if (!is_expected) {
            (void)fprintf(stderr, "  %s: got %.*s, expected %s\n", vector->name, (int)name.byte_count,
                          (const char *)name.bytes, vector->status);
        }
        sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){vector->name, "refused with its status", is_expected});
    } else {
        sxml_deflate_vectors_record(
            run, (sxml_deflate_vectors_check){vector->name, "noise: refused, or consumed within its bytes",
                                              status != SXML_STATUS_OK || result.consumed_byte_count <= stream.byte_count});
    }
    if (status == SXML_STATUS_OK) {
        sxml_allocator_release(sxml_default_allocator(), result.output.bytes);
    }
    sxml_deflate_vectors_release(stream.bytes);
}

/* ----------------------------------------------------------- Deflate */

/* The size of zlib's level-6 stream of a raw input (its -default vector). */
static size_t sxml_deflate_vectors_zlib_size(const sxml_deflate_vectors_run *run, const char *base_name) {
    char path[SXML_DEFLATE_VECTORS_MAXIMUM_PATH];
    (void)snprintf(path, sizeof path, "%s/tests/deflate/%s-default.deflate", run->root, base_name);
    sxml_deflate_vectors_bytes stream = {0};
    if (!sxml_deflate_vectors_read(path, &stream)) {
        return 0;
    }
    size_t byte_count = stream.byte_count;
    sxml_deflate_vectors_release(stream.bytes);
    return byte_count;
}

/* Deflates each -default vector's raw input: round trip, determinism, the
   stored bound and, for compressible inputs, the ratio to zlib. */
static void sxml_deflate_vectors_check_deflate(sxml_deflate_vectors_run *run, const sxml_deflate_vectors_vector *vector) {
    size_t name_length = strlen(vector->name);
    const char suffix[] = "-default";
    if (name_length < sizeof suffix || strcmp(vector->name + name_length - (sizeof suffix - 1), suffix) != 0) {
        return;
    }
    char base_name[256];
    (void)snprintf(base_name, sizeof base_name, "%.*s", (int)(name_length - (sizeof suffix - 1)), vector->name);
    char path[SXML_DEFLATE_VECTORS_MAXIMUM_PATH];
    (void)snprintf(path, sizeof path, "%s/tests/deflate/%s.raw", run->root, vector->name);
    sxml_deflate_vectors_bytes raw = {0};
    if (!sxml_deflate_vectors_read(path, &raw)) {
        sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){vector->name, "the raw input reads", false});
        return;
    }
    sxml_memory_view first = {0};
    sxml_memory_view second = {0};
    sxml_status status = sxml_deflate(
        &(sxml_deflate_request){.input = sxml_deflate_vectors_view(&raw), .allocator = sxml_default_allocator()}, &first);
    sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){vector->name, "deflates", status == SXML_STATUS_OK});
    if (status == SXML_STATUS_OK) {
        status = sxml_deflate(
            &(sxml_deflate_request){.input = sxml_deflate_vectors_view(&raw), .allocator = sxml_default_allocator()},
            &second);
        sxml_deflate_vectors_record(
            run, (sxml_deflate_vectors_check){
                     vector->name, "deflates to the same stream twice",
                     status == SXML_STATUS_OK && first.byte_count == second.byte_count &&
                         (first.byte_count == 0 || memcmp(first.bytes, second.bytes, first.byte_count) == 0)});
        /* A stored block costs 5 bytes per 65,535: no stream needs more. */
        size_t stored_bound = raw.byte_count + 5 * (raw.byte_count / 65535 + 1) + 8;
        sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){vector->name, "is within the stored-block bound",
                                                                      first.byte_count <= stored_bound});
        sxml_inflate_result result = {0};
        status = sxml_deflate_vectors_inflate(
            &(sxml_deflate_vectors_inflation){.input = {.bytes = first.bytes, .byte_count = first.byte_count},
                                              .allocator = sxml_default_allocator(),
                                              .maximum_output_byte_count = SIZE_MAX},
            &result);
        sxml_deflate_vectors_record(
            run, (sxml_deflate_vectors_check){vector->name, "deflate's stream inflates back to its input",
                                              status == SXML_STATUS_OK && sxml_deflate_vectors_equal(result.output, &raw) &&
                                                  result.consumed_byte_count == first.byte_count});
        if (status == SXML_STATUS_OK) {
            sxml_allocator_release(sxml_default_allocator(), result.output.bytes);
        }
        static const char *const compressible[] = {"text", "runs", "lengths", "unicode"};
        for (size_t compressible_index = 0; compressible_index < sizeof compressible / sizeof compressible[0];
             compressible_index++) {
            if (strcmp(base_name, compressible[compressible_index]) == 0) {
                size_t zlib_size = sxml_deflate_vectors_zlib_size(run, base_name);
                bool is_close = zlib_size > 0 && first.byte_count * 10 <= zlib_size * 13;
                if (!is_close) {
                    (void)fprintf(stderr, "  %s: %zu bytes, zlib %zu\n", base_name, first.byte_count, zlib_size);
                }
                sxml_deflate_vectors_record(
                    run, (sxml_deflate_vectors_check){vector->name, "within 1.3 times zlib's level-6 size", is_close});
            }
        }
        sxml_allocator_release(sxml_default_allocator(), first.bytes);
        sxml_allocator_release(sxml_default_allocator(), second.bytes);
    }
    sxml_deflate_vectors_release(raw.bytes);
}

/* ------------------------------------------------- Allocation failures */

typedef enum sxml_deflate_vectors_operation {
    SXML_DEFLATE_VECTORS_OPERATION_INFLATE,
    SXML_DEFLATE_VECTORS_OPERATION_DEFLATE,
} sxml_deflate_vectors_operation;

typedef struct sxml_deflate_vectors_failure_sweep {
    const char *name;
    sxml_deflate_vectors_operation operation;
    sxml_byte_view input;
} sxml_deflate_vectors_failure_sweep;

/* Fails allocation 1, 2, ... until the operation succeeds: each failure
   must be SXML_STATUS_OUT_OF_MEMORY with nothing left allocated. */
static void sxml_deflate_vectors_sweep(sxml_deflate_vectors_run *run, const sxml_deflate_vectors_failure_sweep *sweep) {
    for (uint64_t failing_number = 1; failing_number < 100000; failing_number++) {
        sxml_counting_allocator *counting = NULL;
        if (sxml_counting_allocator_create(sxml_default_allocator(),
                                           &(sxml_counting_allocator_configuration){
                                               .backing_allocator = sxml_default_allocator(),
                                               .failing_allocation_number = failing_number},
                                           &counting) != SXML_STATUS_OK) {
            sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){sweep->name, "makes a counting allocator", false});
            return;
        }
        sxml_allocator allocator = sxml_counting_allocator_as_allocator(counting);
        sxml_status status = SXML_STATUS_OK;
        void *output = NULL;
        if (sweep->operation == SXML_DEFLATE_VECTORS_OPERATION_INFLATE) {
            sxml_inflate_result result = {0};
            status = sxml_deflate_vectors_inflate(
                &(sxml_deflate_vectors_inflation){
                    .input = sweep->input, .allocator = allocator, .maximum_output_byte_count = SIZE_MAX},
                &result);
            output = result.output.bytes;
        } else {
            sxml_memory_view stream = {0};
            status = sxml_deflate(&(sxml_deflate_request){.input = sweep->input, .allocator = allocator}, &stream);
            output = stream.bytes;
        }
        if (status == SXML_STATUS_OK) {
            sxml_allocator_release(allocator, output);
        }
        sxml_allocation_counts counts = sxml_counting_allocator_read_counts(counting);
        bool injected = counts.failed_allocation_count > 0;
        sxml_counting_allocator_destroy(counting);
        if (status == SXML_STATUS_OK && !injected) {
            sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){sweep->name, "survives every allocation failure", true});
            return;
        }
        if ((status != SXML_STATUS_OUT_OF_MEMORY && injected) || counts.live_allocation_count != 0) {
            (void)fprintf(stderr, "  %s: allocation %llu failed: status %d, %llu live\n", sweep->name,
                          (unsigned long long)failing_number, (int)status,
                          (unsigned long long)counts.live_allocation_count);
            sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){sweep->name, "survives every allocation failure", false});
            return;
        }
    }
    sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){sweep->name, "ends its allocation sweep", false});
}

static void sxml_deflate_vectors_check_failures(sxml_deflate_vectors_run *run) {
    char path[SXML_DEFLATE_VECTORS_MAXIMUM_PATH];
    sxml_deflate_vectors_bytes stream = {0};
    sxml_deflate_vectors_bytes raw = {0};
    (void)snprintf(path, sizeof path, "%s/tests/deflate/text-default.deflate", run->root);
    bool is_read = sxml_deflate_vectors_read(path, &stream);
    (void)snprintf(path, sizeof path, "%s/tests/deflate/text-default.raw", run->root);
    is_read = sxml_deflate_vectors_read(path, &raw) && is_read;
    if (is_read) {
        sxml_deflate_vectors_sweep(run, &(sxml_deflate_vectors_failure_sweep){
                                            .name = "inflate-allocation-failures",
                                            .operation = SXML_DEFLATE_VECTORS_OPERATION_INFLATE,
                                            .input = sxml_deflate_vectors_view(&stream)});
        sxml_deflate_vectors_sweep(run, &(sxml_deflate_vectors_failure_sweep){
                                            .name = "deflate-allocation-failures",
                                            .operation = SXML_DEFLATE_VECTORS_OPERATION_DEFLATE,
                                            .input = sxml_deflate_vectors_view(&raw)});
    } else {
        sxml_deflate_vectors_record(run, (sxml_deflate_vectors_check){"text-default", "the vector reads", false});
    }
    sxml_deflate_vectors_release(stream.bytes);
    sxml_deflate_vectors_release(raw.bytes);
}

/* ---------------------------------------------------------------- Main */

int main(int argument_count, char **arguments) {
    if (argument_count != 2) {
        (void)fprintf(stderr, "usage: deflate_vectors ROOT\n");
        return 2;
    }
    sxml_deflate_vectors_run run = {.root = arguments[1]};
    char path[SXML_DEFLATE_VECTORS_MAXIMUM_PATH];
    (void)snprintf(path, sizeof path, "%s/tests/deflate/manifest.tsv", run.root);
    FILE *manifest = fopen(path, "r");
    if (manifest == NULL) {
        (void)fprintf(stderr, "deflate_vectors: cannot read %s\n", path);
        return 2;
    }
    char line[1024];
    while (fgets(line, sizeof line, manifest) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }
        char *fields[4] = {line, NULL, NULL, NULL};
        for (size_t field_index = 1; field_index < 4; field_index++) {
            char *tab = strchr(fields[field_index - 1], '\t');
            if (tab == NULL) {
                break;
            }
            *tab = '\0';
            fields[field_index] = tab + 1;
        }
        if (fields[2] == NULL) {
            continue;
        }
        const sxml_deflate_vectors_vector vector = {.name = fields[0], .kind = fields[1], .status = fields[2]};
        if (strcmp(vector.kind, "valid") == 0) {
            sxml_deflate_vectors_check_valid(&run, &vector);
            sxml_deflate_vectors_check_deflate(&run, &vector);
        } else {
            sxml_deflate_vectors_check_refused(&run, &vector);
        }
    }
    (void)fclose(manifest);
    sxml_deflate_vectors_check_failures(&run);
    (void)printf("deflate_vectors: %zu checks, %zu failed\n", run.check_count, run.failure_count);
    return run.failure_count == 0 && run.check_count > 0 ? 0 : 1;
}
