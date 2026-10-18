/*
 * Compile stage COMPLEX_TYPES: complex type definitions, their content
 * types and attribute uses, element declarations and substitution groups
 * (src/schema_internal.h, docs/DECISIONS.md D165, D167). A stub until
 * packet 5.6 fills it: the stage checks and registers nothing, so a
 * schema compiles with what the earlier stages check, and derivation
 * questions are not answered yet.
 *
 * Packet 5.6 derives every complex type's properties from its base and
 * explicit content (XSD 1.1 Part 1, 3.4.2), checks the derivation
 * (cos-ct-extends, derivation-ok-restriction), the particles and the
 * element declarations (e-props-correct, cos-equiv-derived-ok-rec),
 * compiles the content models (src/schema_content_model.c) and registers
 * each type with sxml_type_registry_register_schema_type.
 *
 * Golden file: src/evaluator_try.c (a part that one file owns).
 */
#include "schema_internal.h"

sxml_status sxml_schema_compile_complex_types(sxml_schema_compilation *compilation) {
    (void)compilation;
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_check_derivation(const sxml_schema_components *components,
                                         const sxml_schema_derivation_request *request,
                                         bool *is_derived) {
    (void)components;
    (void)request;
    (void)is_derived;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}
