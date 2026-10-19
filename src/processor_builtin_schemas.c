/*
 * The optional feature sxml_builtin_schemas_feature (include/sxml/schema.h;
 * packet health-3, docs/DECISIONS.md D225): the processor's built-in
 * schemas for XPath's functions, made by sxml_processor_get_builtin_schema
 * and sxml_processor_get_function_namespace_schema (src/processor.c).
 * XPath's function library reaches them only through this table, so the
 * schema compiler is linked only into a program that names the feature
 * (or links XQuery, XSLT or the schema API, which call the compiler
 * themselves).
 *
 * Golden file: the feature part of src/schema_from_dtd.c (D211).
 */
#include "sxml/schema.h"

#include "optional_feature_internal.h"
#include "processor_internal.h"

static const sxml_builtin_schemas_feature_operations sxml_builtin_schemas_operations = {
    .get_builtin_schema = sxml_processor_get_builtin_schema,
    .get_function_namespace_schema = sxml_processor_get_function_namespace_schema,
};

const sxml_optional_feature_definition sxml_builtin_schemas_feature_definition = {
    .feature_kind = SXML_OPTIONAL_FEATURE_KIND_BUILTIN_SCHEMAS,
    .feature_name = SXML_BYTE_VIEW_INITIALIZER("builtin-schemas"),
    .operations = &sxml_builtin_schemas_operations,
    .finalize_state = NULL,
    .library_functions = NULL,
};

sxml_optional_feature sxml_builtin_schemas_feature(void) {
    return (sxml_optional_feature){.definition = &sxml_builtin_schemas_feature_definition};
}
