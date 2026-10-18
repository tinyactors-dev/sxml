/*
 * Assertions and type alternatives: compile stage ASSERTIONS and their
 * evaluation during validation (src/schema_internal.h, docs/DECISIONS.md
 * D165, D167). A stub until packet 5.9 fills it: the stage compiles
 * nothing; an empty type table selects the declared type, and any other
 * type table, or a type with assertions, is
 * SXML_STATUS_UNSUPPORTED_FEATURE when validation meets it, the one
 * rejection point of each feature (D162).
 *
 * Packet 5.9 compiles the tests with include/sxml/xpath.h in their
 * namespace context and evaluates them: type alternatives on the element
 * with its attributes and inherited attributes (XSD 1.1 Part 1, 3.12.4),
 * assertions on the recorded subtree or with $value bound (3.13.4; Part 2,
 * 4.3.13).
 *
 * Golden file: src/evaluator_try.c (a part that one file owns).
 */
#include "schema_internal.h"

sxml_status sxml_schema_compile_assertions(sxml_schema_compilation *compilation) {
    (void)compilation;
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_select_type_alternative(
    sxml_schema_validator *validator, const sxml_schema_type_alternative_request *request,
    sxml_schema_type_definition_index *selected_type) {
    (void)validator;
    const sxml_schema_element_declaration *declaration = sxml_schema_get_element_declaration(
        &request->schema->components, request->element_declaration);
    /* Without a type table the declared type governs (3.3.4.1). */
    if (declaration->type_alternatives.entry_count == 0) {
        *selected_type = declaration->type_definition;
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

sxml_status sxml_schema_check_assertions(sxml_schema_validator *validator,
                                         const sxml_schema_assertion_request *request) {
    (void)validator;
    const sxml_schema_type_definition *definition = sxml_schema_get_type_definition(
        &request->schema->components, request->type_definition);
    if (definition->assertions.entry_count == 0) {
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}
