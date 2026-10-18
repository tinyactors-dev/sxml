/*
 * Compile stage SIMPLE_TYPES: simple type definitions and facets
 * (src/schema_internal.h, docs/DECISIONS.md D165, D167). A stub until
 * packet 5.5 fills it: it checks and registers nothing, so a schema
 * compiles with what READ and RESOLVE check, and its simple type
 * definitions keep registered_type SXML_TYPE_NONE.
 *
 * Packet 5.5 checks every simple type definition (XSD 1.1 Part 2, 4.1.6
 * "Constraints on Simple Type Definition Schema Components", the facets'
 * constraints of 4.3), registers it in derivation order with
 * sxml_type_registry_register_schema_type (src/types_internal.h), writes
 * primitive_type, the effective assertions and registered_type, and
 * checks the value constraints of attribute declarations and uses
 * (a-props-correct, au-props-correct).
 *
 * Golden file: src/evaluator_try.c (a part that one file owns).
 */
#include "schema_internal.h"

sxml_status sxml_schema_compile_simple_types(sxml_schema_compilation *compilation) {
    (void)compilation;
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_compile_simple_type(sxml_schema_compilation *compilation,
                                            sxml_schema_type_definition_index simple_type) {
    (void)compilation;
    (void)simple_type;
    return SXML_STATUS_OK;
}
