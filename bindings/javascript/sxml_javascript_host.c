/*
 * The resolver of the JavaScript package's processors (D238): the host
 * shim's resolver interface over the WebAssembly import sxml_host.resolve,
 * which src/index.ts implements ("resolve"). Golden file: the import this
 * replaces in the JavaScript shim, now bindings/host/sxml_host.c.
 */
#include "sxml_javascript.h"

#include "sxml/processor.h"
#include "sxml/status.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__wasm__)
/* Implemented by the package: asked with the processor's address, which it
   maps to its processor state. Returns a status: SXML_STATUS_OK, or
   another to stop the evaluation (the package keeps the exception it
   caught and rethrows it). */
__attribute__((import_module("sxml_host"), import_name("resolve"))) extern uint32_t
sxml_javascript_host_resolve(sxml_host_processor *processor,
                             const sxml_host_resource_request *request,
                             sxml_host_resource *resource);

static uint32_t sxml_javascript_resolve(void *state, const sxml_host_resource_request *request,
                                        sxml_host_resource *resource) {
    (void)state;
    return sxml_javascript_host_resolve(request->processor, request, resource);
}

sxml_host_resolver sxml_javascript_resolver(void) {
    return (sxml_host_resolver){.state = NULL, .resolve = sxml_javascript_resolve};
}
#else
/* Native builds (lint, -Werror) have no package: nothing is available. */
sxml_host_resolver sxml_javascript_resolver(void) {
    return (sxml_host_resolver){.state = NULL, .resolve = NULL};
}
#endif
