/*
 * The schema validator (include/sxml/schema.h; src/schema_internal.h;
 * docs/DECISIONS.md D165, D167). A stub until packet 5.7 fills it: every
 * operation returns SXML_STATUS_UNSUPPORTED_FEATURE (the null builder for
 * sxml_schema_validator_as_tree_builder), so a host learns that
 * validation is not available rather than getting an untyped tree.
 *
 * Packet 5.7 makes the validator a tree builder filter (XSD 1.1 Part 1,
 * 3.3.4 "Element Declaration Validation Rules", 3.4.4, 3.2.4, 5.2) over
 * the content model matcher, the recorded subtrees and the ID table of
 * src/schema_internal.h, and sxml_schema_validate_node over
 * sxml_tree_builder_copy_node and an arena tree.
 *
 * Golden file: src/evaluator_try.c (a part that one file owns).
 */
#include "schema_internal.h"

sxml_status sxml_schema_validator_memory_requirements(
    const sxml_schema_validator_configuration *configuration,
    sxml_memory_requirements *requirements) {
    (void)configuration;
    (void)requirements;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

sxml_status sxml_schema_validator_initialize(
    sxml_memory_view memory, const sxml_schema_validator_configuration *configuration,
    sxml_schema_validator **validator) {
    (void)memory;
    (void)configuration;
    (void)validator;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

void sxml_schema_validator_finalize(sxml_schema_validator *validator) {
    (void)validator;
}

sxml_status sxml_schema_validator_create(
    sxml_allocator allocator, const sxml_schema_validator_configuration *configuration,
    sxml_schema_validator **validator) {
    (void)allocator;
    (void)configuration;
    (void)validator;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

void sxml_schema_validator_destroy(sxml_schema_validator *validator) {
    (void)validator;
}

sxml_tree_builder sxml_schema_validator_as_tree_builder(sxml_schema_validator *validator) {
    (void)validator;
    return (sxml_tree_builder){.state = NULL, .operations = NULL};
}

sxml_status sxml_schema_validator_get_outcome(const sxml_schema_validator *validator,
                                              sxml_validation_outcome *outcome) {
    (void)validator;
    (void)outcome;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

sxml_status sxml_schema_validate_node(const sxml_schema *schema,
                                      const sxml_schema_validation_request *request,
                                      sxml_item *validated_node) {
    (void)schema;
    (void)request;
    (void)validated_node;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

sxml_status sxml_schema_validator_report(sxml_schema_validator *validator,
                                         const sxml_schema_report *report) {
    (void)validator;
    (void)report;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

sxml_status sxml_schema_id_table_add_id(sxml_schema_id_table *id_table,
                                        const sxml_schema_id_request *request,
                                        bool *is_duplicate) {
    (void)id_table;
    (void)request;
    (void)is_duplicate;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

sxml_status sxml_schema_id_table_add_idref(sxml_schema_id_table *id_table,
                                           const sxml_schema_id_request *request) {
    (void)id_table;
    (void)request;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

sxml_status sxml_schema_id_table_check_idrefs(sxml_schema_id_table *id_table,
                                              sxml_schema_validator *validator) {
    (void)id_table;
    (void)validator;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}
