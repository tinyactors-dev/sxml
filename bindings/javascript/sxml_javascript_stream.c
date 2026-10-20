/*
 * The streams of the JavaScript package's processors (D294): the host
 * shim's output writer and byte reader (bindings/host/sxml_host.h,
 * "Streams") over the WebAssembly imports sxml_host.write,
 * sxml_host.read and sxml_host.release_reader, which src/index.ts
 * implements. Golden file: sxml_javascript_host.c, the resolver over the
 * import sxml_host.resolve.
 */
#include "sxml_javascript.h"

#include "sxml/status.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__wasm__)
/* Implemented by the package: hands one output event to the sink of the
   processor's current call, copying its bytes out. Returns a status: OK,
   or another to stop the serialization (the package keeps the exception
   the sink threw and rethrows it). */
__attribute__((import_module("sxml_host"), import_name("write"))) extern uint32_t
sxml_javascript_host_write(sxml_host_processor *processor, const sxml_host_output_event *event);

/* Implemented by the package: fills the front of request->buffer from the
   chunk source it delivered as request->reader_identifier. */
__attribute__((import_module("sxml_host"), import_name("read"))) extern uint32_t
sxml_javascript_host_read(sxml_host_processor *processor, sxml_host_read_request *request);

/* Implemented by the package: closes that chunk source. */
__attribute__((import_module("sxml_host"), import_name("release_reader"))) extern void
sxml_javascript_host_release_reader(sxml_host_processor *processor, uint32_t reader_identifier);

static uint32_t sxml_javascript_write(void *state, const sxml_host_output_event *event) {
    return sxml_javascript_host_write(state, event);
}

static uint32_t sxml_javascript_read(void *state, sxml_host_read_request *request) {
    return sxml_javascript_host_read(state, request);
}

static void sxml_javascript_release_reader(void *state, uint32_t reader_identifier) {
    sxml_javascript_host_release_reader(state, reader_identifier);
}
#else
/* Native builds (lint, -Werror) have no package: the output is dropped and
   a reader ends at once. */
static uint32_t sxml_javascript_write(void *state, const sxml_host_output_event *event) {
    (void)state;
    (void)event;
    return SXML_STATUS_OK;
}

static uint32_t sxml_javascript_read(void *state, sxml_host_read_request *request) {
    (void)state;
    request->byte_count = 0;
    request->is_end = 1;
    return SXML_STATUS_OK;
}

static void sxml_javascript_release_reader(void *state, uint32_t reader_identifier) {
    (void)state;
    (void)reader_identifier;
}
#endif

SXML_JAVASCRIPT_EXPORT("sxml_javascript_set_output")
uint32_t sxml_javascript_set_output(sxml_host_processor *processor, uint32_t chunk_byte_count) {
    return sxml_host_set_output(
        processor,
        &(sxml_host_output_configuration){
            .writer = {.state = processor,
                       .write = chunk_byte_count == 0 ? NULL : sxml_javascript_write},
            .chunk_byte_count = chunk_byte_count});
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_set_byte_reader")
uint32_t sxml_javascript_set_byte_reader(sxml_host_processor *processor) {
    return sxml_host_set_byte_reader(
        processor, &(sxml_host_byte_reader){.state = processor,
                                            .read = sxml_javascript_read,
                                            .release = sxml_javascript_release_reader});
}
