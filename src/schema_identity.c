/*
 * Identity constraints: compile stage IDENTITY_CONSTRAINTS and their
 * check during validation (src/schema_internal.h, docs/DECISIONS.md
 * D165, D167). A stub until packet 5.8 fills it: the stage compiles
 * nothing, and validating an element whose declaration has identity
 * constraints is SXML_STATUS_UNSUPPORTED_FEATURE, the one rejection point
 * of the feature (D162).
 *
 * Packet 5.8 checks the selector and field XPath subsets (XSD 1.1 Part 1,
 * 3.11.6: c-selector-xpath, c-fields-xpath) and compiles them, then
 * evaluates them on the recorded subtree of the declaring element and
 * compares key sequences (3.11.4, cvc-identity-constraint).
 *
 * Golden file: src/evaluator_try.c (a part that one file owns).
 */
#include "schema_internal.h"

sxml_status sxml_schema_compile_identity_constraints(sxml_schema_compilation *compilation) {
    (void)compilation;
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_check_identity_constraints(
    sxml_schema_validator *validator, const sxml_schema_identity_request *request) {
    (void)validator;
    const sxml_schema_element_declaration *declaration = sxml_schema_get_element_declaration(
        &request->schema->components, request->element_declaration);
    if (declaration->identity_constraints.entry_count == 0) {
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}
