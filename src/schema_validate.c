/*
 * The schema validator: a tree builder that assesses the events it
 * receives and passes them on, annotated, to its result builder
 * (include/sxml/schema.h; src/schema_validate_internal.h; docs/DECISIONS.md
 * D165, D167, D173).
 *
 * Section numbers are of XSD 1.1 Part 1 (W3C Recommendation, 5 April
 * 2012): 3.3.4 Element Declaration Validation Rules (cvc-elt,
 * cvc-assess-elt), 3.4.4 Complex Type Definition Validation Rules
 * (cvc-complex-type), 3.2.4 and 3.5.4 (cvc-attribute, cvc-au,
 * cvc-assess-attr), 3.10.4 (cvc-wildcard), 3.17.4 (cvc-id) and 5.2
 * Assessing Schema-Validity; XDM 4.0, 3.3 and 7.5 for what the result
 * builder receives (annotations, nilled, is-id, is-idrefs, schema
 * normalized values, defaults).
 *
 * Golden files: src/xslt_whitespace.c (a filtering tree builder with one
 * entry per open element on a stack in allocator memory, D71) and
 * src/arena_tree.c (a builder's lifecycle and event checks).
 */
#include "schema_validate_internal.h"

#include "cast_internal.h"
#include "processor_internal.h"
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"

#include <stdalign.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view no_text = {.bytes = NULL, .byte_count = 0};

/* ---------------------------------------------------------- The result */

static bool has_result_builder(const sxml_schema_validator *validator) {
    return validator->configuration.result_builder.operations != NULL;
}

static sxml_tree_builder result_of(const sxml_schema_validator *validator) {
    return validator->configuration.result_builder;
}

/* The builders the annotated events of the open elements go to: the
   result builder and, while a subtree is recorded, the recorder
   (src/schema_validate_record.c). */
typedef struct event_targets {
    sxml_tree_builder builders[2];
    size_t builder_count;
} event_targets;

static event_targets targets_of(sxml_schema_validator *validator) {
    event_targets targets = {.builder_count = 0};
    if (has_result_builder(validator)) {
        targets.builders[targets.builder_count] = result_of(validator);
        targets.builder_count += 1;
    }
    if (validator->recorder.arena_tree != NULL) {
        targets.builders[targets.builder_count] = sxml_schema_recorder_as_tree_builder(validator);
        targets.builder_count += 1;
    }
    return targets;
}

static bool is_recording(const sxml_schema_validator *validator) {
    return validator->recorder.arena_tree != NULL;
}

/* ----------------------------------------------------------- Components */

static const sxml_schema_type_definition *type_at(const sxml_schema_validator *validator,
                                                  sxml_schema_type_definition_index index) {
    return sxml_schema_get_type_definition(validator->components, index);
}

static const sxml_schema_element_declaration *
element_at(const sxml_schema_validator *validator, sxml_schema_element_declaration_index index) {
    return sxml_schema_get_element_declaration(validator->components, index);
}

static const sxml_schema_attribute_declaration *
attribute_at(const sxml_schema_validator *validator,
             sxml_schema_attribute_declaration_index index) {
    return sxml_schema_get_attribute_declaration(validator->components, index);
}

/* The global component of a name in a symbol space; 0 when none. */
static uint32_t find_global(const sxml_schema_validator *validator, sxml_schema_name_key key) {
    uint32_t component = 0;
    if (!sxml_schema_components_find_name(validator->components, key, &component)) {
        return 0;
    }
    return component;
}

/* The annotation a type definition gives; xs:anyType for one that did not
   register (compilation reported it). */
static sxml_type_identifier annotation_of(const sxml_schema_validator *validator,
                                          sxml_schema_type_definition_index type) {
    const sxml_type_identifier registered = type_at(validator, type)->registered_type;
    if (registered.index == SXML_TYPE_NONE) {
        return (sxml_type_identifier){.index = SXML_TYPE_XS_ANY_TYPE};
    }
    return registered;
}

static bool registered_derives(const sxml_schema_validator *validator, sxml_type_identifier type,
                               sxml_builtin_type base_type) {
    return type.index != SXML_TYPE_NONE &&
           sxml_type_registry_is_derived_from(
               validator->type_registry,
               (sxml_type_pair){.derived_type = type, .base_type = {.index = (uint32_t)base_type}});
}

/* What a simple type says of its elements before their value is known:
   is-id for an atomic type derived from xs:ID, is-idrefs for xs:IDREF and
   lists of it (XDM 4.0, 7.5.2.4). */
typedef struct identity_marks {
    bool is_id;
    bool is_idrefs;
} identity_marks;

static identity_marks marks_of(const sxml_schema_validator *validator,
                               sxml_schema_type_definition_index simple_type) {
    const sxml_schema_type_definition *definition = type_at(validator, simple_type);
    if (definition->variety == SXML_TYPE_VARIETY_ATOMIC) {
        return (identity_marks){
            .is_id = registered_derives(validator, definition->registered_type, SXML_TYPE_XS_ID),
            .is_idrefs =
                registered_derives(validator, definition->registered_type, SXML_TYPE_XS_IDREF)};
    }
    if (definition->variety == SXML_TYPE_VARIETY_LIST) {
        const sxml_schema_type_definition *item_type = type_at(validator, definition->item_type);
        return (identity_marks){
            .is_id = false,
            .is_idrefs =
                registered_derives(validator, definition->registered_type, SXML_TYPE_XS_IDREFS) ||
                registered_derives(validator, item_type->registered_type, SXML_TYPE_XS_IDREF)};
    }
    return (identity_marks){.is_id = false, .is_idrefs = false};
}

/* ------------------------------------------------------------- Reports */

/* A report at a location. */
typedef struct located_error {
    sxml_byte_view constraint_name;
    sxml_byte_view description;
    sxml_source_location location;
} located_error;

static sxml_status report_at(sxml_schema_validator *validator, located_error error) {
    return sxml_schema_validator_report_error(
        validator, &(sxml_schema_validation_error){.constraint_name = error.constraint_name,
                                                   .description = error.description,
                                                   .location = error.location});
}

/* --------------------------------------------------------------- Frames */

static sxml_schema_validation_frame *top_frame(const sxml_schema_validator *validator) {
    if (validator->frames.entry_count == 0) {
        return NULL;
    }
    return sxml_schema_scratch_array_at(&validator->frames, validator->frames.entry_count - 1);
}

/* The namespace bindings from index first on, as a view. */
static sxml_namespace_binding_view bindings_from(const sxml_schema_validator *validator,
                                                 size_t first) {
    const size_t count = validator->namespace_bindings.entry_count;
    if (first >= count) {
        return (sxml_namespace_binding_view){.namespace_bindings = NULL,
                                             .namespace_binding_count = 0};
    }
    return (sxml_namespace_binding_view){
        .namespace_bindings = sxml_schema_scratch_array_at(&validator->namespace_bindings, first),
        .namespace_binding_count = count - first};
}

/* A prefix bound to namespace_uri in scope, innermost first; 0 when none
   (namespace fixup of the result builder then makes one). */
static sxml_namespace_prefix_identifier prefix_for(const sxml_schema_validator *validator,
                                                   sxml_namespace_uri_identifier namespace_uri) {
    for (size_t binding_index = validator->namespace_bindings.entry_count; binding_index > 0;
         binding_index -= 1) {
        const sxml_namespace_binding *binding =
            sxml_schema_scratch_array_at(&validator->namespace_bindings, binding_index - 1);
        if (binding->namespace_uri.index == namespace_uri.index &&
            binding->namespace_prefix.index != 0) {
            return binding->namespace_prefix;
        }
    }
    return (sxml_namespace_prefix_identifier){.index = 0};
}

/* ------------------------------------------------------- Value constraints */

/* The effective value constraint of an attribute use (3.5.1: the use's,
   else its declaration's). */
static sxml_schema_value_constraint use_value_constraint(const sxml_schema_validator *validator,
                                                         const sxml_schema_attribute_use *use) {
    if (use->value_constraint.variety != SXML_SCHEMA_VALUE_CONSTRAINT_NONE) {
        return use->value_constraint;
    }
    return attribute_at(validator, use->attribute_declaration)->value_constraint;
}

/* Two actual values to compare: a value and a value constraint's lexical
   form cast to the same type. */
typedef struct fixed_comparison {
    sxml_schema_type_definition_index simple_type;
    sxml_sequence value;
    sxml_schema_value_constraint value_constraint;
} fixed_comparison;

/* Whether the values are equal. */
typedef struct fixed_outcome {
    bool is_equal;
} fixed_outcome;

/* True when the value equals the fixed value (3.2.4.1 clause 3, 3.5.4.2,
   cvc-elt.5.2.2.2.2: equal in the value space, item by item). A fixed
   value that does not cast is never equal. */
static sxml_status equals_fixed_value(sxml_schema_validator *validator,
                                      const fixed_comparison *comparison, fixed_outcome *outcome) {
    outcome->is_equal = false;
    const sxml_schema_components *components = validator->components;
    const sxml_schema_type_definition *definition = type_at(validator, comparison->simple_type);
    const sxml_schema_namespace_context *context = sxml_schema_get_namespace_context(
        components, comparison->value_constraint.namespace_context);
    const sxml_byte_view lexical_form =
        sxml_schema_get_text(components, comparison->value_constraint.lexical_form);
    if (definition->registered_type.index == SXML_TYPE_NONE) {
        outcome->is_equal = true;
        return SXML_STATUS_OK;
    }
    sxml_error_record ignored;
    sxml_sequence fixed_value = sxml_sequence_empty();
    sxml_status status = sxml_cast_make_value_from_text(
        validator->item_heap,
        &(sxml_text_cast_request){
            .text = lexical_form,
            .target_type = sxml_schema_cast_target_of(definition->registered_type),
            .namespace_bindings =
                sxml_schema_get_namespace_bindings(components, context->namespace_bindings),
            .error_record = &ignored},
        &fixed_value);
    if (status != SXML_STATUS_OK) {
        return status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR ||
                       status == SXML_STATUS_STATIC_ERROR || status == SXML_STATUS_INVALID_ARGUMENT
                   ? SXML_STATUS_OK
                   : status;
    }
    sxml_sequence_iterator first = sxml_sequence_iterator_start(comparison->value);
    sxml_sequence_iterator second = sxml_sequence_iterator_start(fixed_value);
    bool equal = true;
    while (status == SXML_STATUS_OK && equal) {
        sxml_iteration_step first_step = {.has_item = false};
        sxml_iteration_step second_step = {.has_item = false};
        status = sxml_sequence_iterator_next(&first, NULL, &first_step);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_iterator_next(&second, NULL, &second_step);
        }
        if (status != SXML_STATUS_OK || (!first_step.has_item && !second_step.has_item)) {
            break;
        }
        equal = first_step.has_item && second_step.has_item &&
                sxml_atomic_item_is_atomic_equal(first_step.item, second_step.item);
    }
    sxml_sequence_release(fixed_value);
    outcome->is_equal = equal;
    return status;
}

/* ------------------------------------------------------------ Attributes */

static sxml_status send_attribute(sxml_schema_validator *validator,
                                  const sxml_attribute_event *event) {
    const event_targets targets = targets_of(validator);
    sxml_status status = SXML_STATUS_OK;
    for (size_t target = 0; status == SXML_STATUS_OK && target < targets.builder_count;
         target += 1) {
        status = targets.builders[target].operations->add_attribute(targets.builders[target].state,
                                                                    event);
    }
    return status;
}

/* An attribute to assess against a declaration. */
typedef struct declared_attribute {
    const sxml_schema_pending_attribute *attribute;
    sxml_byte_view value;
    sxml_schema_attribute_declaration_index declaration;
    /* The attribute use it matched; 0 for a global declaration. */
    sxml_schema_attribute_use_index use;
} declared_attribute;

/* Attribute Locally Valid (3.2.4.1) and Attribute Locally Valid (Use)
   (3.5.4.2): the value against the declaration's type and the fixed
   value; then the annotated attribute goes on. */
static sxml_status assess_declared_attribute(sxml_schema_validator *validator,
                                             const declared_attribute *declared) {
    const sxml_schema_attribute_declaration *declaration =
        attribute_at(validator, declared->declaration);
    const sxml_schema_type_definition_index simple_type = declaration->type_definition;
    sxml_schema_value_outcome outcome;
    sxml_status status = sxml_schema_validate_simple_value(
        validator,
        &(sxml_schema_value_request){.simple_type = simple_type,
                                     .text = declared->value,
                                     .location = declared->attribute->location,
                                     .element_number = validator->attribute_element_number},
        &outcome);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schema_value_constraint value_constraint = declaration->value_constraint;
    if (declared->use.value != 0) {
        value_constraint = use_value_constraint(
            validator, sxml_schema_get_attribute_use(validator->components, declared->use));
    }
    /* The normalized value is the validator's scratch text until the next
       value: send the attribute before the fixed value is cast. */
    status = send_attribute(
        validator,
        &(sxml_attribute_event){.attribute_name = declared->attribute->attribute_name,
                                .namespace_prefix = declared->attribute->namespace_prefix,
                                .attribute_value = outcome.normalized_value,
                                .type_annotation = annotation_of(validator, simple_type),
                                .is_id = outcome.is_id,
                                .is_idrefs = outcome.is_idrefs,
                                .location = declared->attribute->location});
    if (status == SXML_STATUS_OK && outcome.is_valid &&
        value_constraint.variety == SXML_SCHEMA_VALUE_CONSTRAINT_FIXED) {
        fixed_outcome fixed = {.is_equal = false};
        status = equals_fixed_value(validator,
                                    &(fixed_comparison){.simple_type = simple_type,
                                                        .value = outcome.value,
                                                        .value_constraint = value_constraint},
                                    &fixed);
        if (status == SXML_STATUS_OK && !fixed.is_equal) {
            status =
                report_at(validator,
                          (located_error){
                              .constraint_name = declared->use.value != 0 ? TEXT("cvc-au")
                                                                          : TEXT("cvc-attribute.4"),
                              .description = TEXT("An attribute's value is not its fixed value"),
                              .location = declared->attribute->location});
        }
    }
    sxml_sequence_release(outcome.value);
    return status;
}

/* Sends an attribute that is not assessed (lax without a declaration, a
   skip wildcard, a skipped element): xs:untypedAtomic (XDM 4.0, 3.3.1.1). */
static sxml_status send_unassessed_attribute(sxml_schema_validator *validator,
                                             const sxml_schema_pending_attribute *attribute,
                                             sxml_byte_view value) {
    return send_attribute(validator, &(sxml_attribute_event){
                                         .attribute_name = attribute->attribute_name,
                                         .namespace_prefix = attribute->namespace_prefix,
                                         .attribute_value = value,
                                         .type_annotation = {.index = SXML_TYPE_XS_UNTYPED_ATOMIC},
                                         .is_id = false,
                                         .is_idrefs = false,
                                         .location = attribute->location});
}

static sxml_byte_view pending_value(const sxml_schema_validator *validator,
                                    const sxml_schema_pending_attribute *attribute) {
    if (attribute->value_byte_count == 0) {
        return no_text;
    }
    return (sxml_byte_view){.bytes = validator->attribute_bytes.bytes + attribute->value_offset,
                            .byte_count = attribute->value_byte_count};
}

/* An attribute laxly: by its global declaration where there is one. */
static sxml_status assess_lax_attribute(sxml_schema_validator *validator,
                                        const sxml_schema_pending_attribute *attribute) {
    const sxml_byte_view value = pending_value(validator, attribute);
    const uint32_t declaration = find_global(
        validator,
        (sxml_schema_name_key){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION,
                               .name = attribute->attribute_name});
    if (declaration == 0) {
        return send_unassessed_attribute(validator, attribute, value);
    }
    return assess_declared_attribute(validator,
                                     &(declared_attribute){.attribute = attribute,
                                                           .value = value,
                                                           .declaration = {.value = declaration},
                                                           .use = {.value = 0}});
}

/* The attribute use of a type whose declaration has the name; 0 when
   none. Its position in the type's list goes to *position. */
typedef struct use_lookup {
    sxml_schema_index_view uses;
    sxml_name_identifier attribute_name;
} use_lookup;

static size_t find_use(const sxml_schema_validator *validator, use_lookup lookup) {
    for (size_t position = 0; position < lookup.uses.index_count; position += 1) {
        const sxml_schema_attribute_use *use = sxml_schema_get_attribute_use(
            validator->components,
            (sxml_schema_attribute_use_index){.value = lookup.uses.indexes[position]});
        if (attribute_at(validator, use->attribute_declaration)->attribute_name.index ==
            lookup.attribute_name.index) {
            return position + 1U;
        }
    }
    return 0;
}

/* One attribute of an element of a complex type (cvc-complex-type.3):
   an attribute use of its name, else the attribute wildcard. */
typedef struct complex_attribute {
    const sxml_schema_type_definition *type_definition;
    const sxml_schema_pending_attribute *attribute;
} complex_attribute;

static sxml_status assess_complex_attribute(sxml_schema_validator *validator,
                                            complex_attribute request) {
    const sxml_schema_pending_attribute *attribute = request.attribute;
    const sxml_byte_view value = pending_value(validator, attribute);
    const sxml_schema_index_view uses =
        sxml_schema_get_list(validator->components, request.type_definition->attribute_uses);
    const size_t position = find_use(
        validator, (use_lookup){.uses = uses, .attribute_name = attribute->attribute_name});
    if (position != 0) {
        uint8_t *met = sxml_schema_scratch_array_at(&validator->attribute_uses_met, position - 1U);
        *met = 1;
        const sxml_schema_attribute_use_index use = {.value = uses.indexes[position - 1U]};
        return assess_declared_attribute(
            validator, &(declared_attribute){
                           .attribute = attribute,
                           .value = value,
                           .declaration = sxml_schema_get_attribute_use(validator->components, use)
                                              ->attribute_declaration,
                           .use = use});
    }
    const sxml_schema_wildcard_index wildcard = request.type_definition->attribute_wildcard;
    if (wildcard.value == 0 ||
        !sxml_schema_wildcard_allows_name(
            validator->components,
            &(sxml_schema_wildcard_test){.wildcard = wildcard,
                                         .name = attribute->attribute_name,
                                         .symbol_space =
                                             SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION})) {
        /* 3.4.4.2, clause 3: no use and no wildcard for it. */
        sxml_status status = report_at(
            validator, (located_error){.constraint_name = TEXT("cvc-complex-type.3.2.2"),
                                       .description = TEXT("The attribute is not allowed"),
                                       .location = attribute->location});
        if (status == SXML_STATUS_OK) {
            status = send_unassessed_attribute(validator, attribute, value);
        }
        return status;
    }
    /* 3.10.4.? via 3.4.4.2, clause 4 and cvc-assess-attr: the wildcard's
       process contents. */
    switch (sxml_schema_get_wildcard(validator->components, wildcard)->process_contents) {
    case SXML_SCHEMA_PROCESS_CONTENTS_SKIP:
        return send_unassessed_attribute(validator, attribute, value);
    case SXML_SCHEMA_PROCESS_CONTENTS_LAX:
        return assess_lax_attribute(validator, attribute);
    case SXML_SCHEMA_PROCESS_CONTENTS_STRICT:
        break;
    }
    const uint32_t declaration = find_global(
        validator,
        (sxml_schema_name_key){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION,
                               .name = attribute->attribute_name});
    if (declaration == 0) {
        sxml_status status = report_at(
            validator,
            (located_error){.constraint_name = TEXT("cvc-assess-attr"),
                            .description =
                                TEXT("A strict attribute wildcard matched an attribute without a "
                                     "global declaration"),
                            .location = attribute->location});
        if (status == SXML_STATUS_OK) {
            status = send_unassessed_attribute(validator, attribute, value);
        }
        return status;
    }
    return assess_declared_attribute(validator,
                                     &(declared_attribute){.attribute = attribute,
                                                           .value = value,
                                                           .declaration = {.value = declaration},
                                                           .use = {.value = 0}});
}

/* The attribute uses of a complex type that no attribute matched:
   required ones are cvc-complex-type.4, those with a default or fixed
   value add the attribute (3.4.5, the PSVI's default attributes). */
typedef struct missing_uses {
    const sxml_schema_type_definition *type_definition;
    sxml_source_location location;
} missing_uses;

static sxml_status add_missing_uses(sxml_schema_validator *validator, missing_uses missing) {
    const sxml_schema_index_view uses =
        sxml_schema_get_list(validator->components, missing.type_definition->attribute_uses);
    sxml_status status = SXML_STATUS_OK;
    for (size_t position = 0; status == SXML_STATUS_OK && position < uses.index_count;
         position += 1) {
        if (*(const uint8_t *)sxml_schema_scratch_array_at(&validator->attribute_uses_met,
                                                           position) != 0) {
            continue;
        }
        const sxml_schema_attribute_use *use = sxml_schema_get_attribute_use(
            validator->components,
            (sxml_schema_attribute_use_index){.value = uses.indexes[position]});
        if (use->use_kind == SXML_SCHEMA_ATTRIBUTE_USE_REQUIRED) {
            status = report_at(
                validator, (located_error){.constraint_name = TEXT("cvc-complex-type.4"),
                                           .description = TEXT("A required attribute is missing"),
                                           .location = missing.location});
            continue;
        }
        const sxml_schema_value_constraint value_constraint = use_value_constraint(validator, use);
        if (value_constraint.variety == SXML_SCHEMA_VALUE_CONSTRAINT_NONE) {
            continue;
        }
        const sxml_schema_attribute_declaration *declaration =
            attribute_at(validator, use->attribute_declaration);
        sxml_name_parts parts = {.namespace_uri = {.index = 0}, .local_name = no_text};
        status = sxml_name_pool_get_name_parts(validator->name_pool, declaration->attribute_name,
                                               &parts);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const sxml_schema_pending_attribute attribute = {
            .attribute_name = declaration->attribute_name,
            .namespace_prefix = parts.namespace_uri.index == SXML_NAMESPACE_NONE
                                    ? (sxml_namespace_prefix_identifier){.index = 0}
                                    : prefix_for(validator, parts.namespace_uri),
            .value_offset = 0,
            .value_byte_count = 0,
            .location = missing.location};
        status = assess_declared_attribute(
            validator,
            &(declared_attribute){
                .attribute = &attribute,
                .value = sxml_schema_get_text(validator->components, value_constraint.lexical_form),
                .declaration = use->attribute_declaration,
                .use = {.value = uses.indexes[position]}});
    }
    return status;
}

/* True for the four attributes of the xsi: namespace that every element
   may have (3.4.4.2, clause 3; 3.2.7: attribute declarations 1 to 4). */
static uint32_t xsi_declaration_of(const sxml_schema_validator *validator,
                                   sxml_name_identifier attribute_name) {
    const uint32_t declaration = find_global(
        validator,
        (sxml_schema_name_key){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION,
                               .name = attribute_name});
    return declaration >= 1U && declaration <= 4U ? declaration : 0;
}

/* Assesses and sends the attributes of the element of frame. */
static sxml_status assess_attributes(sxml_schema_validator *validator,
                                     const sxml_schema_validation_frame *frame) {
    const sxml_schema_type_definition *type_definition =
        frame->assessment == SXML_SCHEMA_ASSESSMENT_STRICT
            ? type_at(validator, frame->type_definition)
            : NULL;
    const bool is_complex =
        type_definition != NULL && type_definition->variety == SXML_TYPE_VARIETY_COMPLEX;
    sxml_status status = SXML_STATUS_OK;
    if (is_complex) {
        const size_t use_count = type_definition->attribute_uses.entry_count;
        validator->attribute_uses_met.entry_count = 0;
        const uint8_t not_met = 0;
        for (size_t use_index = 0; status == SXML_STATUS_OK && use_index < use_count;
             use_index += 1) {
            status = sxml_schema_scratch_array_append(validator->allocator,
                                                      &validator->attribute_uses_met, &not_met);
        }
    }
    const size_t attribute_count = validator->pending_attributes.entry_count;
    for (size_t attribute_index = 0; status == SXML_STATUS_OK && attribute_index < attribute_count;
         attribute_index += 1) {
        const sxml_schema_pending_attribute *attribute =
            sxml_schema_scratch_array_at(&validator->pending_attributes, attribute_index);
        const sxml_byte_view value = pending_value(validator, attribute);
        const uint32_t xsi_declaration = xsi_declaration_of(validator, attribute->attribute_name);
        /* An attribute use of an xsi: attribute's name governs it (an
           xsi:type use="required", say); otherwise its built-in
           declaration does. */
        const bool has_use =
            is_complex &&
            find_use(validator,
                     (use_lookup){.uses = sxml_schema_get_list(validator->components,
                                                               type_definition->attribute_uses),
                                  .attribute_name = attribute->attribute_name}) != 0;
        if (frame->assessment == SXML_SCHEMA_ASSESSMENT_SKIP) {
            status = send_unassessed_attribute(validator, attribute, value);
        } else if (xsi_declaration != 0 && !has_use) {
            status = assess_declared_attribute(
                validator, &(declared_attribute){.attribute = attribute,
                                                 .value = value,
                                                 .declaration = {.value = xsi_declaration},
                                                 .use = {.value = 0}});
        } else if (is_complex) {
            status = assess_complex_attribute(
                validator,
                (complex_attribute){.type_definition = type_definition, .attribute = attribute});
        } else if (type_definition != NULL) {
            /* 3.4.4.1 cvc-type.3.1.1: a simple type allows no attributes
               but the xsi: ones. */
            status = report_at(
                validator,
                (located_error){.constraint_name = TEXT("cvc-type.3.1.1"),
                                .description = TEXT("An element of a simple type has an attribute"),
                                .location = attribute->location});
            if (status == SXML_STATUS_OK) {
                status = send_unassessed_attribute(validator, attribute, value);
            }
        } else {
            status = assess_lax_attribute(validator, attribute);
        }
    }
    if (status == SXML_STATUS_OK && is_complex) {
        status = add_missing_uses(validator, (missing_uses){.type_definition = type_definition,
                                                            .location = frame->location});
    }
    return status;
}

/* ------------------------------------------------------------- xsi:type */

/* The pending attribute of a name; NULL when absent. */
static const sxml_schema_pending_attribute *find_pending(const sxml_schema_validator *validator,
                                                         sxml_name_identifier attribute_name) {
    if (attribute_name.index == 0) {
        return NULL;
    }
    for (size_t attribute_index = 0; attribute_index < validator->pending_attributes.entry_count;
         attribute_index += 1) {
        const sxml_schema_pending_attribute *attribute =
            sxml_schema_scratch_array_at(&validator->pending_attributes, attribute_index);
        if (attribute->attribute_name.index == attribute_name.index) {
            return attribute;
        }
    }
    return NULL;
}

/* What xsi:type names (cvc-elt.4.1, 4.2): its type definition, or 0 when
   it is not a QName or names no type definition (reported). */
static sxml_status resolve_xsi_type(sxml_schema_validator *validator,
                                    const sxml_schema_pending_attribute *attribute,
                                    sxml_schema_type_definition_index *type) {
    type->value = 0;
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    sxml_status status = sxml_schema_validator_get_cast_bindings(validator, &bindings);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_error_record ignored;
    sxml_sequence value = sxml_sequence_empty();
    status = sxml_cast_make_value_from_text(
        validator->item_heap,
        &(sxml_text_cast_request){.text = pending_value(validator, attribute),
                                  .target_type = {.index = SXML_TYPE_XS_QNAME},
                                  .namespace_bindings = bindings,
                                  .error_record = &ignored},
        &value);
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR) {
        return report_at(validator,
                         (located_error){.constraint_name = TEXT("cvc-elt.4.1"),
                                         .description = TEXT("The xsi:type value is not a QName"),
                                         .location = attribute->location});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item item = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    sxml_atomic_datum datum;
    status = sxml_sequence_get_item(
        value, (sxml_item_request){.item_index = 0, .error_record = NULL}, &item);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(item, &datum);
    }
    const sxml_name_identifier type_name = status == SXML_STATUS_OK
                                               ? datum.datum_details.qname.expanded_name
                                               : (sxml_name_identifier){.index = 0};
    sxml_sequence_release(value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    type->value = find_global(
        validator, (sxml_schema_name_key){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                          .name = type_name});
    if (type->value == 0) {
        return report_at(
            validator,
            (located_error){.constraint_name = TEXT("cvc-elt.4.2"),
                            .description = TEXT("The xsi:type value names no type definition"),
                            .location = attribute->location});
    }
    return SXML_STATUS_OK;
}

/* The local type definition of xsi:type replaces the declared one when it
   is validly derived from it (cvc-elt.4.3, 3.3.4.1). */
typedef struct type_choice {
    sxml_schema_element_declaration_index element_declaration;
    /* In/out: the type so far; 0 when nothing governs yet. */
    sxml_schema_type_definition_index *type;
    const sxml_schema_pending_attribute *xsi_type;
} type_choice;

static sxml_status apply_xsi_type(sxml_schema_validator *validator, type_choice choice) {
    sxml_schema_type_definition_index local_type = {.value = 0};
    sxml_status status = resolve_xsi_type(validator, choice.xsi_type, &local_type);
    if (status != SXML_STATUS_OK || local_type.value == 0) {
        return status;
    }
    if (choice.type->value == 0) {
        *choice.type = local_type;
        return SXML_STATUS_OK;
    }
    /* Blocked: the declaration's {disallowed substitutions} and the
       declared type's {prohibited substitutions} (3.3.4.1, clause 4.3;
       3.4.6.5). */
    sxml_schema_derivation_set blocked = type_at(validator, *choice.type)->prohibited_substitutions;
    if (choice.element_declaration.value != 0) {
        blocked |= element_at(validator, choice.element_declaration)->disallowed_substitutions;
    }
    blocked &= SXML_SCHEMA_DERIVATION_EXTENSION | SXML_SCHEMA_DERIVATION_RESTRICTION;
    sxml_schema_derivation_outcome outcome = {.is_derived = false};
    status = sxml_schema_check_derivation(
        validator->components,
        &(sxml_schema_derivation_request){
            .derived_type = local_type, .base_type = *choice.type, .blocked_derivations = blocked},
        &outcome);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!outcome.is_derived) {
        return report_at(
            validator,
            (located_error){.constraint_name = TEXT("cvc-elt.4.3"),
                            .description = TEXT("The xsi:type type is not validly derived from "
                                                "the declared type"),
                            .location = choice.xsi_type->location});
    }
    *choice.type = local_type;
    return SXML_STATUS_OK;
}

/* xsi:nil (cvc-elt.3.1, 3.2.2): true makes the element nilled when its
   declaration is nillable. */
typedef struct nil_check {
    sxml_schema_validation_frame *frame;
    const sxml_schema_pending_attribute *xsi_nil;
} nil_check;

static sxml_status apply_xsi_nil(sxml_schema_validator *validator, nil_check check) {
    sxml_schema_validation_frame *frame = check.frame;
    const sxml_schema_element_declaration *declaration =
        element_at(validator, frame->element_declaration);
    if (frame->element_declaration.value == 0) {
        return SXML_STATUS_OK;
    }
    if (!declaration->is_nillable) {
        return report_at(validator,
                         (located_error){.constraint_name = TEXT("cvc-elt.3.1"),
                                         .description = TEXT("xsi:nil on an element that is not "
                                                             "nillable"),
                                         .location = check.xsi_nil->location});
    }
    sxml_error_record ignored;
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = sxml_cast_make_value_from_text(
        validator->item_heap,
        &(sxml_text_cast_request){
            .text = pending_value(validator, check.xsi_nil),
            .target_type = {.index = SXML_TYPE_XS_BOOLEAN},
            .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
            .error_record = &ignored},
        &value);
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR) {
        /* The attribute's own assessment reports the value. */
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item item = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    sxml_atomic_datum datum;
    status = sxml_sequence_get_item(
        value, (sxml_item_request){.item_index = 0, .error_record = NULL}, &item);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(item, &datum);
    }
    const bool is_true = status == SXML_STATUS_OK && datum.datum_details.boolean;
    sxml_sequence_release(value);
    if (status != SXML_STATUS_OK || !is_true) {
        return status;
    }
    frame->is_nilled = true;
    if (declaration->value_constraint.variety == SXML_SCHEMA_VALUE_CONSTRAINT_FIXED) {
        return report_at(validator,
                         (located_error){.constraint_name = TEXT("cvc-elt.3.2.2"),
                                         .description = TEXT("A nilled element has a fixed value"),
                                         .location = check.xsi_nil->location});
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ The held-back start */

/* Gives frame its type's content: variety, simple type, content model
   and counters. */
static sxml_status set_content(sxml_schema_validator *validator,
                               sxml_schema_validation_frame *frame) {
    const sxml_schema_type_definition *definition = type_at(validator, frame->type_definition);
    if (definition->variety != SXML_TYPE_VARIETY_COMPLEX) {
        frame->content_variety = SXML_CONTENT_TYPE_VARIETY_SIMPLE;
        frame->simple_type = frame->type_definition;
        return SXML_STATUS_OK;
    }
    frame->content_variety = definition->content_type_variety;
    frame->simple_type = definition->simple_content_type;
    frame->content_model = definition->content_model;
    if (frame->content_model == NULL) {
        return SXML_STATUS_OK;
    }
    frame->counter_count = sxml_schema_content_model_get_counter_count(frame->content_model);
    const uint64_t zero = 0;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t counter = 0; status == SXML_STATUS_OK && counter < frame->counter_count;
         counter += 1) {
        status = sxml_schema_scratch_array_append(validator->allocator, &validator->counter_values,
                                                  &zero);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schema_content_model_cursor cursor = {
        .content_model = frame->content_model,
        .state = 0,
        .counter_values =
            sxml_schema_scratch_array_at(&validator->counter_values, frame->first_counter),
        .counter_count = frame->counter_count};
    sxml_schema_content_model_start(&cursor);
    frame->content_model_state = cursor.state;
    return SXML_STATUS_OK;
}

/* The governing type of the held-back element (3.3.4.1, 3.3.4.6): its
   declaration's type (after type alternatives), else the configured
   governing type, replaced by xsi:type. */
static sxml_status choose_type(sxml_schema_validator *validator,
                               sxml_schema_validation_frame *frame) {
    const sxml_schema_pending_element *pending = &validator->pending;
    sxml_schema_type_definition_index type = pending->governing_type;
    sxml_status status = SXML_STATUS_OK;
    if (frame->element_declaration.value != 0) {
        const sxml_schema_element_declaration *declaration =
            element_at(validator, frame->element_declaration);
        if (declaration->is_abstract) {
            /* cvc-elt.2: an abstract declaration governs no element. */
            status = report_at(validator,
                               (located_error){.constraint_name = TEXT("cvc-elt.2"),
                                               .description = TEXT("The element's declaration is "
                                                                   "abstract"),
                                               .location = frame->location});
        }
        if (status == SXML_STATUS_OK) {
            /* 3.3.4.1: the type table selects the type (packet 5.9). */
            status = sxml_schema_select_type(validator, frame->element_declaration, &type);
        }
    }
    const sxml_schema_pending_attribute *xsi_type =
        find_pending(validator, validator->xsi_type_name);
    if (status == SXML_STATUS_OK && xsi_type != NULL) {
        status = apply_xsi_type(validator,
                                (type_choice){.element_declaration = frame->element_declaration,
                                              .type = &type,
                                              .xsi_type = xsi_type});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (type.value == 0) {
        frame->assessment = SXML_SCHEMA_ASSESSMENT_LAX;
        if (pending->missing_declaration_constraint.byte_count != 0) {
            status = report_at(
                validator,
                (located_error){.constraint_name = pending->missing_declaration_constraint,
                                .description = TEXT("No declaration governs the element"),
                                .location = frame->location});
        }
        return status;
    }
    frame->assessment = SXML_SCHEMA_ASSESSMENT_STRICT;
    frame->type_definition = type;
    const sxml_schema_type_definition *definition = type_at(validator, type);
    if (definition->variety == SXML_TYPE_VARIETY_COMPLEX && definition->is_abstract) {
        /* cvc-type.2: no element is of an abstract type. */
        status = report_at(validator,
                           (located_error){.constraint_name = TEXT("cvc-type.2"),
                                           .description = TEXT("The element's type is abstract"),
                                           .location = frame->location});
    }
    if (status == SXML_STATUS_OK) {
        status = set_content(validator, frame);
    }
    return status;
}

/* Sends the start of the element of frame and its namespace bindings: its
   own to the result builder, every one in scope to the recorder when the
   element is the root of the recorded tree. */
static sxml_status send_start(sxml_schema_validator *validator,
                              const sxml_schema_validation_frame *frame) {
    sxml_element_start_event event = validator->pending.event;
    event.type_annotation = frame->assessment == SXML_SCHEMA_ASSESSMENT_STRICT
                                ? annotation_of(validator, frame->type_definition)
                                : (sxml_type_identifier){.index = SXML_TYPE_XS_ANY_TYPE};
    const identity_marks marks = frame->assessment == SXML_SCHEMA_ASSESSMENT_STRICT &&
                                         !frame->is_nilled &&
                                         frame->content_variety == SXML_CONTENT_TYPE_VARIETY_SIMPLE
                                     ? marks_of(validator, frame->simple_type)
                                     : (identity_marks){.is_id = false, .is_idrefs = false};
    event.is_id = marks.is_id;
    event.is_idrefs = marks.is_idrefs;
    event.is_nilled = frame->is_nilled;
    const event_targets targets = targets_of(validator);
    const bool is_recorded_root =
        is_recording(validator) && validator->recorder.root_frame == validator->frames.entry_count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t target = 0; status == SXML_STATUS_OK && target < targets.builder_count;
         target += 1) {
        const sxml_tree_builder builder = targets.builders[target];
        status = builder.operations->start_element(builder.state, &event);
        if (status == SXML_STATUS_OK && is_recorded_root && target + 1U == targets.builder_count) {
            status = sxml_schema_send_in_scope_bindings(validator, builder);
            continue;
        }
        const sxml_namespace_binding_view bindings = bindings_from(validator, frame->binding_start);
        for (size_t binding_index = 0;
             status == SXML_STATUS_OK && binding_index < bindings.namespace_binding_count;
             binding_index += 1) {
            status = builder.operations->add_namespace_binding(
                builder.state, &bindings.namespace_bindings[binding_index]);
        }
    }
    return status;
}

/* Whether an element needs its subtree recorded (src/schema_internal.h,
   the validator's seams): its declaration has identity constraints, or
   its type has assertions. */
static bool needs_subtree(const sxml_schema_validator *validator,
                          const sxml_schema_validation_frame *frame) {
    if (frame->assessment != SXML_SCHEMA_ASSESSMENT_STRICT) {
        return false;
    }
    const sxml_schema_type_definition *definition = type_at(validator, frame->type_definition);
    return element_at(validator, frame->element_declaration)->identity_constraints.entry_count !=
               0 ||
           (definition->variety == SXML_TYPE_VARIETY_COMPLEX &&
            definition->assertions.entry_count != 0);
}

/* Element Declarations Consistent at validation (XSD 1.1, 3.4.4.2
   clause 5): an element a strict or lax wildcard matched whose name a
   declaration of the parent's content model has must be governed by a type
   derived from that declaration's type (xs:anyType when assessed laxly
   without one); a skipped element has no governing type to check. */
static sxml_status check_consistent_type(sxml_schema_validator *validator,
                                         const sxml_schema_validation_frame *frame) {
    if (frame->assessment == SXML_SCHEMA_ASSESSMENT_SKIP) {
        return SXML_STATUS_OK;
    }
    const sxml_schema_type_definition_index governing_type =
        frame->assessment == SXML_SCHEMA_ASSESSMENT_STRICT
            ? frame->type_definition
            : (sxml_schema_type_definition_index){.value = SXML_TYPE_XS_ANY_TYPE};
    const sxml_schema_element_declaration *declaration =
        element_at(validator, validator->pending.consistent_declaration);
    sxml_schema_derivation_outcome outcome = {.is_derived = false};
    const sxml_status status = sxml_schema_check_derivation(
        validator->components,
        &(sxml_schema_derivation_request){.derived_type = governing_type,
                                          .base_type = declaration->type_definition,
                                          .blocked_derivations = SXML_SCHEMA_DERIVATION_NONE},
        &outcome);
    if (status != SXML_STATUS_OK || outcome.is_derived) {
        return status;
    }
    return report_at(validator,
                     (located_error){.constraint_name = TEXT("cvc-complex-type.5"),
                                     .description = TEXT("An element a wildcard matched is not of "
                                                         "a type derived from the content model's "
                                                         "declaration of its name"),
                                     .location = frame->location});
}

/* Ends the attribute phase of the held-back element: its type, xsi:nil,
   its start and attributes go on, and its frame is pushed. */
static sxml_status flush_pending(sxml_schema_validator *validator) {
    sxml_schema_pending_element *pending = &validator->pending;
    if (!pending->is_pending) {
        return SXML_STATUS_OK;
    }
    pending->is_pending = false;
    sxml_schema_validation_frame frame = {.element_name = pending->event.element_name,
                                          .assessment = pending->assessment,
                                          .element_declaration = pending->element_declaration,
                                          .type_definition = {.value = 0},
                                          .content_variety = SXML_CONTENT_TYPE_VARIETY_MIXED,
                                          .simple_type = {.value = 0},
                                          .content_model = NULL,
                                          .content_model_state = 0,
                                          .first_counter = validator->counter_values.entry_count,
                                          .counter_count = 0,
                                          .is_nilled = false,
                                          .has_content_error = false,
                                          .has_element_children = false,
                                          .has_character_children = false,
                                          .text_start = validator->text_bytes.entry_count,
                                          .binding_start = pending->binding_start,
                                          .is_id = false,
                                          .element_number = validator->element_count + 1U,
                                          .location = pending->event.location};
    validator->element_count += 1;
    sxml_status status = SXML_STATUS_OK;
    if (frame.assessment != SXML_SCHEMA_ASSESSMENT_SKIP) {
        status = choose_type(validator, &frame);
    }
    const sxml_schema_pending_attribute *xsi_nil = find_pending(validator, validator->xsi_nil_name);
    if (status == SXML_STATUS_OK && xsi_nil != NULL &&
        frame.assessment == SXML_SCHEMA_ASSESSMENT_STRICT) {
        status = apply_xsi_nil(validator, (nil_check){.frame = &frame, .xsi_nil = xsi_nil});
    }
    if (status == SXML_STATUS_OK && pending->consistent_declaration.value != 0) {
        status = check_consistent_type(validator, &frame);
    }
    if (validator->frames.entry_count == 0 && frame.assessment != SXML_SCHEMA_ASSESSMENT_STRICT) {
        validator->is_root_not_known = true;
    }
    if (status == SXML_STATUS_OK && !is_recording(validator) && needs_subtree(validator, &frame)) {
        status = sxml_schema_recorder_start(validator);
    }
    if (status == SXML_STATUS_OK && is_recording(validator)) {
        frame.recorded_node = validator->recorder.node_count;
        validator->recorder.next_declaration =
            frame.assessment == SXML_SCHEMA_ASSESSMENT_STRICT
                ? frame.element_declaration
                : (sxml_schema_element_declaration_index){.value = 0};
    }
    if (status == SXML_STATUS_OK) {
        status = send_start(validator, &frame);
    }
    if (status == SXML_STATUS_OK) {
        validator->attribute_element_number = frame.element_number;
        status = assess_attributes(validator, &frame);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_scratch_array_append(validator->allocator, &validator->frames, &frame);
    }
    validator->pending_attributes.entry_count = 0;
    validator->attribute_bytes.entry_count = 0;
    return status;
}

/* ------------------------------------------------------------- Children */

/* What an element child is assessed with. */
typedef struct child_assessment {
    sxml_schema_assessment assessment;
    sxml_schema_element_declaration_index element_declaration;
    sxml_byte_view missing_declaration_constraint;
    /* As sxml_schema_pending_element.consistent_declaration. */
    sxml_schema_element_declaration_index consistent_declaration;
} child_assessment;

/* Laxly: by the global declaration of its name where there is one
   (3.10.4.? via cvc-assess-elt, lax). */
static child_assessment lax_child(const sxml_schema_validator *validator,
                                  sxml_name_identifier element_name) {
    const uint32_t declaration = find_global(
        validator,
        (sxml_schema_name_key){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                               .name = element_name});
    return (child_assessment){.assessment = declaration != 0 ? SXML_SCHEMA_ASSESSMENT_STRICT
                                                             : SXML_SCHEMA_ASSESSMENT_LAX,
                              .element_declaration = {.value = declaration},
                              .missing_declaration_constraint = no_text};
}

/* An error in the parent's content: reported once, then the rest of the
   content is assessed laxly. */
typedef struct content_error {
    sxml_schema_validation_frame *frame;
    sxml_byte_view constraint_name;
    sxml_byte_view description;
    sxml_source_location location;
} content_error;

static sxml_status report_content_error(sxml_schema_validator *validator, content_error error) {
    error.frame->has_content_error = true;
    return report_at(validator, (located_error){.constraint_name = error.constraint_name,
                                                .description = error.description,
                                                .location = error.location});
}

/* A child matched a wildcard (3.10.4.1, 3.4.4.2 clause 2.4): its process
   contents decide. */
static child_assessment wildcard_child(const sxml_schema_validator *validator,
                                       sxml_schema_wildcard_index wildcard,
                                       sxml_name_identifier element_name) {
    switch (sxml_schema_get_wildcard(validator->components, wildcard)->process_contents) {
    case SXML_SCHEMA_PROCESS_CONTENTS_SKIP:
        return (child_assessment){.assessment = SXML_SCHEMA_ASSESSMENT_SKIP,
                                  .element_declaration = {.value = 0},
                                  .missing_declaration_constraint = no_text};
    case SXML_SCHEMA_PROCESS_CONTENTS_LAX:
        return lax_child(validator, element_name);
    case SXML_SCHEMA_PROCESS_CONTENTS_STRICT:
        break;
    }
    child_assessment child = lax_child(validator, element_name);
    if (child.element_declaration.value == 0) {
        /* Strict: without a global declaration only xsi:type may govern
           it (cvc-assess-elt.1.1.1). */
        child.missing_declaration_constraint = TEXT("cvc-complex-type.2.4");
    }
    return child;
}

/* A name to look for among the element particles of a type's content. */
typedef struct particle_search {
    sxml_schema_type_definition_index type_definition;
    sxml_name_identifier element_name;
} particle_search;

/* Writes the declaration of an element particle of the content of the
   type or a base type (or a member of its substitution group) that has
   the name to
   *declaration; 0 when none. The particle tree is walked with an explicit
   stack (D71); it has no cycles (mg-props-correct.2). */
static sxml_status find_particle_declaration(sxml_schema_validator *validator,
                                             particle_search search,
                                             sxml_schema_element_declaration_index *declaration) {
    const sxml_schema_components *components = validator->components;
    sxml_schema_scratch_array *stack = &validator->particle_stack;
    declaration->value = 0;
    stack->entry_count = 0;
    /* The type's particle and its base types' (XSD 1.1, 3.8.6.3: the
       declarations of the type hierarchy count). */
    sxml_status status = SXML_STATUS_OK;
    sxml_schema_type_definition_index type = search.type_definition;
    for (uint32_t depth = 0; status == SXML_STATUS_OK && depth < UINT16_MAX; depth += 1) {
        const sxml_schema_type_definition *definition = type_at(validator, type);
        if (definition->variety != SXML_TYPE_VARIETY_COMPLEX ||
            type.value == SXML_TYPE_XS_ANY_TYPE) {
            break;
        }
        if (definition->particle.value != 0) {
            status = sxml_schema_scratch_array_append(validator->allocator, stack,
                                                      &definition->particle.value);
        }
        type = definition->base_type;
    }
    while (status == SXML_STATUS_OK && stack->entry_count > 0 && declaration->value == 0) {
        stack->entry_count -= 1;
        const uint32_t particle_index =
            *(const uint32_t *)sxml_schema_scratch_array_at(stack, stack->entry_count);
        const sxml_schema_particle *particle = sxml_schema_get_particle(
            components, (sxml_schema_particle_index){.value = particle_index});
        if (particle->term_kind == SXML_SCHEMA_TERM_MODEL_GROUP) {
            const sxml_schema_index_view particles = sxml_schema_get_list(
                components,
                sxml_schema_get_model_group(
                    components, (sxml_schema_model_group_index){.value = particle->term})
                    ->particles);
            for (size_t entry = 0; status == SXML_STATUS_OK && entry < particles.index_count;
                 entry += 1) {
                status = sxml_schema_scratch_array_append(validator->allocator, stack,
                                                          &particles.indexes[entry]);
            }
            continue;
        }
        if (particle->term_kind != SXML_SCHEMA_TERM_ELEMENT_DECLARATION) {
            continue;
        }
        const sxml_schema_element_declaration_index candidate = {.value = particle->term};
        const sxml_schema_element_declaration *element = element_at(validator, candidate);
        if (element->element_name.index == search.element_name.index) {
            *declaration = candidate;
            break;
        }
        const sxml_schema_index_view members =
            sxml_schema_get_list(components, element->substitution_group_members);
        for (size_t entry = 0; entry < members.index_count; entry += 1) {
            const sxml_schema_element_declaration_index member = {.value = members.indexes[entry]};
            if (element_at(validator, member)->element_name.index == search.element_name.index) {
                *declaration = member;
                break;
            }
        }
    }
    return status;
}

/* The child of a strictly assessed parent: through the content model. */
typedef struct child_request {
    sxml_schema_validation_frame *parent;
    sxml_name_identifier element_name;
    sxml_source_location location;
} child_request;

static sxml_status match_child(sxml_schema_validator *validator, child_request request,
                               child_assessment *child) {
    sxml_schema_validation_frame *parent = request.parent;
    *child = lax_child(validator, request.element_name);
    if (parent->is_nilled) {
        return report_content_error(
            validator, (content_error){.frame = parent,
                                       .constraint_name = TEXT("cvc-elt.3.2.1"),
                                       .description = TEXT("A nilled element has content"),
                                       .location = request.location});
    }
    switch (parent->content_variety) {
    case SXML_CONTENT_TYPE_VARIETY_EMPTY:
        return report_content_error(
            validator,
            (content_error){.frame = parent,
                            .constraint_name = TEXT("cvc-complex-type.2.1"),
                            .description = TEXT("An element of empty content has a child element"),
                            .location = request.location});
    case SXML_CONTENT_TYPE_VARIETY_SIMPLE:
        return report_content_error(
            validator,
            (content_error){.frame = parent,
                            .constraint_name =
                                type_at(validator, parent->type_definition)->variety ==
                                        SXML_TYPE_VARIETY_COMPLEX
                                    ? TEXT("cvc-complex-type.2.2")
                                    : TEXT("cvc-type.3.1.2"),
                            .description = TEXT("An element of simple content has a child element"),
                            .location = request.location});
    case SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY:
    case SXML_CONTENT_TYPE_VARIETY_MIXED:
        break;
    }
    sxml_schema_content_model_match match = {.match_kind = SXML_SCHEMA_MATCH_NONE};
    if (parent->content_model != NULL) {
        sxml_schema_content_model_cursor cursor = {
            .content_model = parent->content_model,
            .state = parent->content_model_state,
            .counter_values =
                sxml_schema_scratch_array_at(&validator->counter_values, parent->first_counter),
            .counter_count = parent->counter_count};
        const sxml_status status = sxml_schema_content_model_step(
            validator->components,
            &(sxml_schema_content_model_step_request){.cursor = &cursor,
                                                      .element_name = request.element_name},
            &match);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        parent->content_model_state = cursor.state;
    }
    switch (match.match_kind) {
    case SXML_SCHEMA_MATCH_NONE:
        /* 3.4.4.2, clause 2.4: the child does not fit the content model. */
        return report_content_error(
            validator, (content_error){.frame = parent,
                                       .constraint_name = TEXT("cvc-complex-type.2.4"),
                                       .description = TEXT("The element is not allowed here"),
                                       .location = request.location});
    case SXML_SCHEMA_MATCH_ELEMENT_DECLARATION:
        *child = (child_assessment){.assessment = SXML_SCHEMA_ASSESSMENT_STRICT,
                                    .element_declaration = match.element_declaration,
                                    .missing_declaration_constraint = no_text};
        return SXML_STATUS_OK;
    case SXML_SCHEMA_MATCH_WILDCARD:
        *child = wildcard_child(validator, match.wildcard, request.element_name);
        return find_particle_declaration(
            validator,
            (particle_search){.type_definition = parent->type_definition,
                              .element_name = request.element_name},
            &child->consistent_declaration);
    }
    return SXML_STATUS_OK;
}

/* The validation root (5.2): the configured governing type, else the
   global declaration of its name; strict mode needs one (cvc-elt.1). */
static child_assessment root_assessment(const sxml_schema_validator *validator,
                                        sxml_name_identifier element_name) {
    if (validator->pending.governing_type.value != 0) {
        return (child_assessment){.assessment = SXML_SCHEMA_ASSESSMENT_STRICT,
                                  .element_declaration = {.value = 0},
                                  .missing_declaration_constraint = no_text};
    }
    child_assessment root = lax_child(validator, element_name);
    if (root.element_declaration.value == 0 &&
        validator->configuration.validation_mode == SXML_VALIDATION_MODE_STRICT) {
        root.missing_declaration_constraint = TEXT("cvc-elt.1");
    }
    return root;
}

/* ------------------------------------------------------------ Element ends */

static sxml_status send_text_event(sxml_schema_validator *validator, const sxml_text_event *event) {
    const event_targets targets = targets_of(validator);
    sxml_status status = SXML_STATUS_OK;
    for (size_t target = 0; status == SXML_STATUS_OK && target < targets.builder_count;
         target += 1) {
        status =
            targets.builders[target].operations->add_text(targets.builders[target].state, event);
    }
    return status;
}

static sxml_status send_text(sxml_schema_validator *validator, sxml_byte_view text,
                             sxml_source_location location) {
    if (text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    return send_text_event(
        validator,
        &(sxml_text_event){.text = text, .location = location, .is_cdata_section = false});
}

static sxml_byte_view collected_text(const sxml_schema_validator *validator,
                                     const sxml_schema_validation_frame *frame) {
    const size_t byte_count = validator->text_bytes.entry_count - frame->text_start;
    if (byte_count == 0) {
        return no_text;
    }
    return (sxml_byte_view){.bytes = validator->text_bytes.bytes + frame->text_start,
                            .byte_count = byte_count};
}

/* The element is empty: no character or element children (cvc-elt.5.1,
   5.2.1: its default or fixed value applies). */
static bool is_empty_element(const sxml_schema_validation_frame *frame) {
    return !frame->has_character_children && !frame->has_element_children;
}

/* The number of the parent of the element that ends (the top frame); 0
   for the validation root. */
static uint64_t parent_number_of(const sxml_schema_validator *validator) {
    if (validator->frames.entry_count < 2) {
        return 0;
    }
    const sxml_schema_validation_frame *parent =
        sxml_schema_scratch_array_at(&validator->frames, validator->frames.entry_count - 2);
    return parent->element_number;
}

/* Simple content at the end (cvc-type.3.1.3, cvc-complex-type.2.2): the
   value, or the value constraint of an empty element, against the simple
   type; fixed values compared; the schema normalized value goes on as
   the element's text (XDM 4.0, 7.5.7.4). */
static sxml_status end_simple_content(sxml_schema_validator *validator,
                                      const sxml_schema_validation_frame *frame) {
    const sxml_schema_value_constraint value_constraint =
        element_at(validator, frame->element_declaration)->value_constraint;
    const bool takes_value_constraint =
        is_empty_element(frame) && value_constraint.variety != SXML_SCHEMA_VALUE_CONSTRAINT_NONE;
    const sxml_byte_view text =
        takes_value_constraint
            ? sxml_schema_get_text(validator->components, value_constraint.lexical_form)
            : collected_text(validator, frame);
    sxml_schema_value_outcome outcome;
    sxml_status status = sxml_schema_validate_simple_value(
        validator,
        &(sxml_schema_value_request){.simple_type = frame->simple_type,
                                     .text = text,
                                     .location = frame->location,
                                     .element_number = parent_number_of(validator)},
        &outcome);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = send_text(validator, outcome.normalized_value, frame->location);
    if (status == SXML_STATUS_OK && outcome.is_valid && !takes_value_constraint &&
        value_constraint.variety == SXML_SCHEMA_VALUE_CONSTRAINT_FIXED) {
        fixed_outcome fixed = {.is_equal = false};
        status = equals_fixed_value(validator,
                                    &(fixed_comparison){.simple_type = frame->simple_type,
                                                        .value = outcome.value,
                                                        .value_constraint = value_constraint},
                                    &fixed);
        if (status == SXML_STATUS_OK && !fixed.is_equal) {
            status = report_at(
                validator,
                (located_error){.constraint_name = TEXT("cvc-elt.5.2.2.2.2"),
                                .description = TEXT("The element's value is not its fixed value"),
                                .location = frame->location});
        }
    }
    sxml_sequence_release(outcome.value);
    return status;
}

/* Mixed content at the end: the value constraint of an empty element is
   its text; a fixed value is compared as a string (cvc-elt.5.2.2.1,
   5.2.2.2.1). */
static sxml_status end_mixed_content(sxml_schema_validator *validator,
                                     const sxml_schema_validation_frame *frame) {
    const sxml_schema_value_constraint value_constraint =
        element_at(validator, frame->element_declaration)->value_constraint;
    if (value_constraint.variety == SXML_SCHEMA_VALUE_CONSTRAINT_NONE) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view lexical_form =
        sxml_schema_get_text(validator->components, value_constraint.lexical_form);
    if (is_empty_element(frame)) {
        return send_text(validator, lexical_form, frame->location);
    }
    if (value_constraint.variety != SXML_SCHEMA_VALUE_CONSTRAINT_FIXED) {
        return SXML_STATUS_OK;
    }
    if (frame->has_element_children) {
        return report_at(validator,
                         (located_error){.constraint_name = TEXT("cvc-elt.5.2.2.1"),
                                         .description = TEXT("An element with a fixed value has "
                                                             "element children"),
                                         .location = frame->location});
    }
    if (!sxml_byte_view_equals(collected_text(validator, frame), lexical_form)) {
        return report_at(validator,
                         (located_error){.constraint_name = TEXT("cvc-elt.5.2.2.2.1"),
                                         .description = TEXT("The element's content is not its "
                                                             "fixed value"),
                                         .location = frame->location});
    }
    return SXML_STATUS_OK;
}

/* The checks at the end of a strictly assessed element. */
static sxml_status end_strict_element(sxml_schema_validator *validator,
                                      const sxml_schema_validation_frame *frame) {
    sxml_status status = SXML_STATUS_OK;
    if (!frame->has_content_error && !frame->is_nilled) {
        switch (frame->content_variety) {
        case SXML_CONTENT_TYPE_VARIETY_SIMPLE:
            status = end_simple_content(validator, frame);
            break;
        case SXML_CONTENT_TYPE_VARIETY_MIXED:
            status = end_mixed_content(validator, frame);
            break;
        case SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY:
        case SXML_CONTENT_TYPE_VARIETY_EMPTY:
            break;
        }
        if (status == SXML_STATUS_OK && frame->content_model != NULL) {
            const sxml_schema_content_model_cursor cursor = {
                .content_model = frame->content_model,
                .state = frame->content_model_state,
                .counter_values =
                    sxml_schema_scratch_array_at(&validator->counter_values, frame->first_counter),
                .counter_count = frame->counter_count};
            if (!sxml_schema_content_model_can_end(&cursor)) {
                /* 3.4.4.2, clause 2.4: the content is not complete. */
                status = report_at(validator,
                                   (located_error){.constraint_name = TEXT("cvc-complex-type.2.4"),
                                                   .description = TEXT("The element's content is "
                                                                       "incomplete"),
                                                   .location = frame->location});
            }
        }
    }
    if (status == SXML_STATUS_OK && needs_subtree(validator, frame)) {
        /* 3.13.4.1 (packet 5.9) and 3.11.4 (packet 5.8) read the
           element's recorded subtree: they run once the outermost
           recorded element has ended. */
        const sxml_schema_type_definition *definition = type_at(validator, frame->type_definition);
        const bool has_assertions = definition->variety == SXML_TYPE_VARIETY_COMPLEX &&
                                    definition->assertions.entry_count != 0;
        const bool has_identity_constraints =
            element_at(validator, frame->element_declaration)->identity_constraints.entry_count !=
            0;
        status = sxml_schema_recorder_defer_check(
            validator,
            &(sxml_schema_deferred_check){
                .node_index = frame->recorded_node,
                .element_declaration = has_identity_constraints
                                           ? frame->element_declaration
                                           : (sxml_schema_element_declaration_index){.value = 0},
                .type_definition = has_assertions
                                       ? frame->type_definition
                                       : (sxml_schema_type_definition_index){.value = 0}});
    }
    return status;
}

/* --------------------------------------------------------- Builder events */

static sxml_status forward_start_document(sxml_schema_validator *validator,
                                          const sxml_document_start_event *event) {
    if (!has_result_builder(validator)) {
        return SXML_STATUS_OK;
    }
    const sxml_tree_builder result = result_of(validator);
    return result.operations->start_document(result.state, event);
}

static sxml_status validator_start_document(void *state, const sxml_document_start_event *event) {
    sxml_schema_validator *validator = state;
    if (validator->is_document || validator->has_root || validator->is_complete) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    validator->is_document = true;
    return forward_start_document(validator, event);
}

/* The validation root has ended: cvc-id over its IDREFs. */
static sxml_status end_validation_root(sxml_schema_validator *validator) {
    const sxml_status status = sxml_schema_id_table_check_idrefs(&validator->id_table, validator);
    if (!validator->is_document) {
        validator->is_complete = true;
    }
    return status;
}

static sxml_status validator_end_document(void *state) {
    sxml_schema_validator *validator = state;
    if (!validator->is_document || validator->is_complete || validator->frames.entry_count != 0 ||
        validator->pending.is_pending) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    validator->is_complete = true;
    if (!has_result_builder(validator)) {
        return SXML_STATUS_OK;
    }
    const sxml_tree_builder result = result_of(validator);
    return result.operations->end_document(result.state);
}

static sxml_status validator_start_element(void *state, const sxml_element_start_event *event) {
    sxml_schema_validator *validator = state;
    if (validator->is_complete) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_status status = flush_pending(validator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schema_validation_frame *parent = top_frame(validator);
    child_assessment child = {.assessment = SXML_SCHEMA_ASSESSMENT_LAX,
                              .element_declaration = {.value = 0},
                              .missing_declaration_constraint = no_text};
    if (parent == NULL) {
        if (validator->has_root) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        validator->has_root = true;
        child = root_assessment(validator, event->element_name);
    } else {
        parent->has_element_children = true;
        if (parent->assessment == SXML_SCHEMA_ASSESSMENT_SKIP) {
            child.assessment = SXML_SCHEMA_ASSESSMENT_SKIP;
        } else if (parent->assessment == SXML_SCHEMA_ASSESSMENT_LAX || parent->has_content_error) {
            child = lax_child(validator, event->element_name);
        } else {
            status = match_child(validator,
                                 (child_request){.parent = parent,
                                                 .element_name = event->element_name,
                                                 .location = event->location},
                                 &child);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    validator->pending = (sxml_schema_pending_element){
        .is_pending = true,
        .event = *event,
        .assessment = child.assessment,
        .element_declaration = child.element_declaration,
        .governing_type = parent == NULL ? validator->pending.governing_type
                                         : (sxml_schema_type_definition_index){.value = 0},
        .missing_declaration_constraint = child.missing_declaration_constraint,
        .consistent_declaration = child.consistent_declaration,
        .binding_start = validator->namespace_bindings.entry_count};
    validator->pending.event.location = event->location;
    validator->pending_attributes.entry_count = 0;
    validator->attribute_bytes.entry_count = 0;
    return SXML_STATUS_OK;
}

static sxml_status
validator_add_namespace_binding(void *state, const sxml_namespace_binding *namespace_binding) {
    sxml_schema_validator *validator = state;
    if (validator->pending.is_pending) {
        return sxml_schema_scratch_array_append(validator->allocator,
                                                &validator->namespace_bindings, namespace_binding);
    }
    if (validator->frames.entry_count != 0 || validator->is_document || validator->is_complete) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* A parentless namespace node passes on (include/sxml/schema.h). */
    validator->is_complete = true;
    if (!has_result_builder(validator)) {
        return SXML_STATUS_OK;
    }
    const sxml_tree_builder result = result_of(validator);
    return result.operations->add_namespace_binding(result.state, namespace_binding);
}

/* A parentless attribute is the validation root (5.2): the configured
   governing type, else its global declaration; strict mode needs one. */
static sxml_status validate_root_attribute(sxml_schema_validator *validator,
                                           const sxml_attribute_event *event) {
    validator->has_root = true;
    validator->attribute_element_number = 0;
    const sxml_schema_pending_attribute attribute = {.attribute_name = event->attribute_name,
                                                     .namespace_prefix = event->namespace_prefix,
                                                     .value_offset = 0,
                                                     .value_byte_count = 0,
                                                     .location = event->location};
    sxml_status status = SXML_STATUS_OK;
    const sxml_schema_type_definition_index governing_type = validator->pending.governing_type;
    if (governing_type.value != 0) {
        sxml_schema_value_outcome outcome;
        status = sxml_schema_validate_simple_value(
            validator,
            &(sxml_schema_value_request){.simple_type = governing_type,
                                         .text = event->attribute_value,
                                         .location = event->location,
                                         .element_number = 0},
            &outcome);
        if (status == SXML_STATUS_OK) {
            status = send_attribute(
                validator,
                &(sxml_attribute_event){.attribute_name = event->attribute_name,
                                        .namespace_prefix = event->namespace_prefix,
                                        .attribute_value = outcome.normalized_value,
                                        .type_annotation = annotation_of(validator, governing_type),
                                        .is_id = outcome.is_id,
                                        .is_idrefs = outcome.is_idrefs,
                                        .location = event->location});
            sxml_sequence_release(outcome.value);
        }
    } else {
        const uint32_t declaration = find_global(
            validator,
            (sxml_schema_name_key){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION,
                                   .name = event->attribute_name});
        if (declaration != 0) {
            status = assess_declared_attribute(
                validator, &(declared_attribute){.attribute = &attribute,
                                                 .value = event->attribute_value,
                                                 .declaration = {.value = declaration},
                                                 .use = {.value = 0}});
        } else {
            validator->is_root_not_known = true;
            if (validator->configuration.validation_mode == SXML_VALIDATION_MODE_STRICT) {
                status = report_at(
                    validator,
                    (located_error){.constraint_name = TEXT("cvc-assess-attr.1"),
                                    .description = TEXT("No declaration governs the attribute"),
                                    .location = event->location});
            }
            if (status == SXML_STATUS_OK) {
                status = send_unassessed_attribute(validator, &attribute, event->attribute_value);
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        status = end_validation_root(validator);
    }
    return status;
}

static sxml_status validator_add_attribute(void *state, const sxml_attribute_event *event) {
    sxml_schema_validator *validator = state;
    if (!validator->pending.is_pending) {
        if (validator->frames.entry_count != 0 || validator->is_document || validator->has_root ||
            validator->is_complete) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        return validate_root_attribute(validator, event);
    }
    const sxml_schema_pending_attribute attribute = {
        .attribute_name = event->attribute_name,
        .namespace_prefix = event->namespace_prefix,
        .value_offset = validator->attribute_bytes.entry_count,
        .value_byte_count = event->attribute_value.byte_count,
        .location = event->location};
    sxml_status status = sxml_schema_scratch_array_append_bytes(
        validator->allocator, &validator->attribute_bytes, event->attribute_value);
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_scratch_array_append(validator->allocator,
                                                  &validator->pending_attributes, &attribute);
    }
    return status;
}

static sxml_status validator_end_element(void *state) {
    sxml_schema_validator *validator = state;
    sxml_status status = flush_pending(validator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_schema_validation_frame *top = top_frame(validator);
    if (top == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_schema_validation_frame frame = *top;
    if (frame.assessment == SXML_SCHEMA_ASSESSMENT_STRICT) {
        status = end_strict_element(validator, &frame);
    }
    const event_targets targets = targets_of(validator);
    for (size_t target = 0; status == SXML_STATUS_OK && target < targets.builder_count;
         target += 1) {
        status = targets.builders[target].operations->end_element(targets.builders[target].state);
    }
    validator->frames.entry_count -= 1;
    validator->namespace_bindings.entry_count = frame.binding_start;
    validator->counter_values.entry_count = frame.first_counter;
    validator->text_bytes.entry_count = frame.text_start;
    if (status == SXML_STATUS_OK && is_recording(validator) &&
        validator->recorder.root_frame == validator->frames.entry_count) {
        /* The outermost recorded element has ended. */
        status = sxml_schema_recorder_finish(validator);
    }
    if (status == SXML_STATUS_OK && validator->frames.entry_count == 0) {
        status = end_validation_root(validator);
    }
    return status;
}

static bool is_whitespace(sxml_byte_view text) {
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
            return false;
        }
    }
    return true;
}

/* Character children of a strictly assessed element (cvc-elt.3.2.1,
   cvc-complex-type.2.1 to 2.3). Writes to outcome->keeps whether the text goes
   on as it is. */
typedef struct text_request {
    sxml_schema_validation_frame *frame;
    const sxml_text_event *event;
} text_request;

/* Whether the text goes on. */
typedef struct text_outcome {
    bool keeps;
} text_outcome;

static sxml_status assess_text(sxml_schema_validator *validator, text_request request,
                               text_outcome *outcome) {
    sxml_schema_validation_frame *frame = request.frame;
    const sxml_byte_view text = request.event->text;
    outcome->keeps = true;
    if (frame->has_content_error) {
        return SXML_STATUS_OK;
    }
    if (frame->is_nilled) {
        return report_content_error(
            validator, (content_error){.frame = frame,
                                       .constraint_name = TEXT("cvc-elt.3.2.1"),
                                       .description = TEXT("A nilled element has content"),
                                       .location = request.event->location});
    }
    switch (frame->content_variety) {
    case SXML_CONTENT_TYPE_VARIETY_MIXED: {
        const sxml_schema_value_constraint value_constraint =
            element_at(validator, frame->element_declaration)->value_constraint;
        if (value_constraint.variety == SXML_SCHEMA_VALUE_CONSTRAINT_FIXED) {
            return sxml_schema_scratch_array_append_bytes(validator->allocator,
                                                          &validator->text_bytes, text);
        }
        return SXML_STATUS_OK;
    }
    case SXML_CONTENT_TYPE_VARIETY_SIMPLE:
        /* Collected, and sent at the end as its schema normalized value. */
        outcome->keeps = false;
        return sxml_schema_scratch_array_append_bytes(validator->allocator, &validator->text_bytes,
                                                      text);
    case SXML_CONTENT_TYPE_VARIETY_EMPTY:
        outcome->keeps = false;
        return report_content_error(
            validator,
            (content_error){.frame = frame,
                            .constraint_name = TEXT("cvc-complex-type.2.1"),
                            .description = TEXT("An element of empty content has character "
                                                "children"),
                            .location = request.event->location});
    case SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY:
        if (is_whitespace(text)) {
            /* XDM 4.0, 7.5.7.4: whitespace in content that is not mixed
               makes no text node. */
            outcome->keeps = false;
            return SXML_STATUS_OK;
        }
        return report_content_error(
            validator, (content_error){.frame = frame,
                                       .constraint_name = TEXT("cvc-complex-type.2.3"),
                                       .description = TEXT("Character children in element-only "
                                                           "content"),
                                       .location = request.event->location});
    }
    return SXML_STATUS_OK;
}

static sxml_status validator_add_text(void *state, const sxml_text_event *event) {
    sxml_schema_validator *validator = state;
    sxml_status status = flush_pending(validator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schema_validation_frame *frame = top_frame(validator);
    text_outcome outcome = {.keeps = true};
    if (frame != NULL && event->text.byte_count != 0) {
        frame->has_character_children = true;
        if (frame->assessment == SXML_SCHEMA_ASSESSMENT_STRICT) {
            status =
                assess_text(validator, (text_request){.frame = frame, .event = event}, &outcome);
        }
    } else if (frame == NULL && !validator->is_document) {
        /* A parentless text passes on (include/sxml/schema.h). */
        validator->is_complete = true;
    }
    if (status != SXML_STATUS_OK || !outcome.keeps) {
        return status;
    }
    return send_text_event(validator, event);
}

static sxml_status validator_add_comment(void *state, const sxml_comment_event *event) {
    sxml_schema_validator *validator = state;
    const sxml_status status = flush_pending(validator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (validator->frames.entry_count == 0 && !validator->is_document) {
        validator->is_complete = true;
    }
    const event_targets targets = targets_of(validator);
    sxml_status sent = SXML_STATUS_OK;
    for (size_t target = 0; sent == SXML_STATUS_OK && target < targets.builder_count; target += 1) {
        sent =
            targets.builders[target].operations->add_comment(targets.builders[target].state, event);
    }
    return sent;
}

static sxml_status
validator_add_processing_instruction(void *state, const sxml_processing_instruction_event *event) {
    sxml_schema_validator *validator = state;
    const sxml_status status = flush_pending(validator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (validator->frames.entry_count == 0 && !validator->is_document) {
        validator->is_complete = true;
    }
    const event_targets targets = targets_of(validator);
    sxml_status sent = SXML_STATUS_OK;
    for (size_t target = 0; sent == SXML_STATUS_OK && target < targets.builder_count; target += 1) {
        sent = targets.builders[target].operations->add_processing_instruction(
            targets.builders[target].state, event);
    }
    return sent;
}

static sxml_status validator_add_unparsed_entity(void *state, const sxml_unparsed_entity *entity) {
    sxml_schema_validator *validator = state;
    const uint8_t end = 0;
    sxml_status status = sxml_schema_scratch_array_append_bytes(
        validator->allocator, &validator->entity_names, entity->entity_name);
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_scratch_array_append_bytes(
            validator->allocator, &validator->entity_names,
            (sxml_byte_view){.bytes = &end, .byte_count = 1});
    }
    if (status != SXML_STATUS_OK || !has_result_builder(validator)) {
        return status;
    }
    const sxml_tree_builder result = result_of(validator);
    return result.operations->add_unparsed_entity(result.state, entity);
}

static const sxml_tree_builder_operations validator_operations = {
    .start_document = validator_start_document,
    .end_document = validator_end_document,
    .start_element = validator_start_element,
    .add_namespace_binding = validator_add_namespace_binding,
    .add_attribute = validator_add_attribute,
    .end_element = validator_end_element,
    .add_text = validator_add_text,
    .add_comment = validator_add_comment,
    .add_processing_instruction = validator_add_processing_instruction,
    .add_unparsed_entity = validator_add_unparsed_entity,
};

/* ------------------------------------------------------------- Lifecycle */

/* The type definition of the schema whose registered type is type; 0 when
   none (built-in types are definitions 1 to 54, D167). */
static uint32_t definition_of_registered(const sxml_schema_components *components,
                                         sxml_type_identifier type) {
    if (type.index != SXML_TYPE_NONE && type.index < SXML_TYPE_FIRST_REGISTERED) {
        return type.index;
    }
    const uint32_t count = components->tables[SXML_SCHEMA_TABLE_TYPE_DEFINITION].entry_count;
    for (uint32_t index = 1; index < count; index += 1) {
        if (sxml_schema_get_type_definition(components,
                                            (sxml_schema_type_definition_index){.value = index})
                ->registered_type.index == type.index) {
            return index;
        }
    }
    return 0;
}

sxml_status
sxml_schema_validator_memory_requirements(const sxml_schema_validator_configuration *configuration,
                                          sxml_memory_requirements *requirements) {
    if (configuration == NULL || configuration->schema == NULL || requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (configuration->governing_type.index != SXML_TYPE_NONE &&
        definition_of_registered(&configuration->schema->components,
                                 configuration->governing_type) == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_schema_validator),
                                               .alignment = alignof(sxml_schema_validator)};
    return SXML_STATUS_OK;
}

static sxml_name_identifier find_xsi_name(const sxml_name_pool *name_pool,
                                          sxml_byte_view local_name) {
    sxml_name_identifier name = {.index = 0};
    (void)sxml_name_pool_find_name(
        name_pool,
        &(sxml_name_request){.namespace_uri = sxml_well_known_namespace_identifier(
                                 SXML_NAMESPACE_XML_SCHEMA_INSTANCE),
                             .local_name = local_name},
        &name);
    return name;
}

static sxml_schema_scratch_array scratch_of(size_t entry_byte_count) {
    return (sxml_schema_scratch_array){
        .bytes = NULL, .entry_byte_count = entry_byte_count, .entry_count = 0, .entry_capacity = 0};
}

sxml_status
sxml_schema_validator_initialize(sxml_memory_view memory,
                                 const sxml_schema_validator_configuration *configuration,
                                 sxml_schema_validator **validator) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_schema_validator_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (validator == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_schema *schema = configuration->schema;
    sxml_processor *processor = schema->processor;
    sxml_schema_validator *placed = (sxml_schema_validator *)memory.bytes;
    *placed = (sxml_schema_validator){
        .configuration = *configuration,
        .components = &schema->components,
        .allocator = processor->allocator,
        .creating_allocator = {.state = NULL, .operations = NULL},
        .item_heap = processor->item_heap,
        .name_pool = processor->name_pool,
        .type_registry = processor->type_registry,
        .xsi_type_name = find_xsi_name(processor->name_pool, TEXT("type")),
        .xsi_nil_name = find_xsi_name(processor->name_pool, TEXT("nil")),
        .frames = scratch_of(sizeof(sxml_schema_validation_frame)),
        .pending = {.is_pending = false,
                    .governing_type = {.value = definition_of_registered(
                                           &schema->components, configuration->governing_type)}},
        .pending_attributes = scratch_of(sizeof(sxml_schema_pending_attribute)),
        .attribute_bytes = scratch_of(1),
        .attribute_uses_met = scratch_of(1),
        .namespace_bindings = scratch_of(sizeof(sxml_namespace_binding)),
        .cast_bindings = scratch_of(sizeof(sxml_namespace_binding)),
        .particle_stack = scratch_of(sizeof(uint32_t)),
        .counter_values = scratch_of(sizeof(uint64_t)),
        .text_bytes = scratch_of(1),
        .value_bytes = scratch_of(1),
        .entity_names = scratch_of(1),
        .is_document = false,
        .has_root = false,
        .is_complete = false,
        .is_root_not_known = false,
        .error_count = 0};
    sxml_schema_id_table_initialize(&placed->id_table, processor->allocator);
    sxml_schema_recorder_initialize(&placed->recorder, processor->allocator);
    *validator = placed;
    return SXML_STATUS_OK;
}

void sxml_schema_validator_finalize(sxml_schema_validator *validator) {
    if (validator == NULL) {
        return;
    }
    const sxml_allocator allocator = validator->allocator;
    sxml_schema_scratch_array_release(allocator, &validator->frames);
    sxml_schema_scratch_array_release(allocator, &validator->pending_attributes);
    sxml_schema_scratch_array_release(allocator, &validator->attribute_bytes);
    sxml_schema_scratch_array_release(allocator, &validator->attribute_uses_met);
    sxml_schema_scratch_array_release(allocator, &validator->namespace_bindings);
    sxml_schema_scratch_array_release(allocator, &validator->cast_bindings);
    sxml_schema_scratch_array_release(allocator, &validator->particle_stack);
    sxml_schema_scratch_array_release(allocator, &validator->counter_values);
    sxml_schema_scratch_array_release(allocator, &validator->text_bytes);
    sxml_schema_scratch_array_release(allocator, &validator->value_bytes);
    sxml_schema_scratch_array_release(allocator, &validator->entity_names);
    sxml_schema_id_table_release(&validator->id_table);
    sxml_schema_recorder_release(&validator->recorder, allocator);
}

sxml_status sxml_schema_validator_create(sxml_allocator allocator,
                                         const sxml_schema_validator_configuration *configuration,
                                         sxml_schema_validator **validator) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_schema_validator_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (validator == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    void *memory = NULL;
    status =
        sxml_allocator_allocate(allocator,
                                (sxml_allocation_request){.byte_count = requirements.byte_count,
                                                          .alignment = requirements.alignment},
                                &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schema_validator *placed = NULL;
    status = sxml_schema_validator_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count}, configuration,
        &placed);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    placed->creating_allocator = allocator;
    *validator = placed;
    return SXML_STATUS_OK;
}

void sxml_schema_validator_destroy(sxml_schema_validator *validator) {
    if (validator == NULL) {
        return;
    }
    const sxml_allocator allocator = validator->creating_allocator;
    sxml_schema_validator_finalize(validator);
    sxml_allocator_release(allocator, validator);
}

sxml_tree_builder sxml_schema_validator_as_tree_builder(sxml_schema_validator *validator) {
    return (sxml_tree_builder){.state = validator, .operations = &validator_operations};
}

sxml_status sxml_schema_validator_get_outcome(const sxml_schema_validator *validator,
                                              sxml_validation_outcome *outcome) {
    if (validator == NULL || outcome == NULL || !validator->is_complete) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_validity validity = SXML_VALIDITY_VALID;
    if (validator->error_count != 0) {
        validity = SXML_VALIDITY_INVALID;
    } else if (validator->is_root_not_known) {
        validity = SXML_VALIDITY_NOT_KNOWN;
    }
    *outcome =
        (sxml_validation_outcome){.validity = validity, .error_count = validator->error_count};
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ Validating a node */

/* A copy of the node through a validator into a new arena tree. */
typedef struct node_validation {
    sxml_schema_validator *validator;
    sxml_arena_tree *arena_tree;
} node_validation;

static sxml_status copy_through_validator(const sxml_schema *schema,
                                          const sxml_schema_validation_request *request,
                                          node_validation *validation) {
    sxml_processor *processor = schema->processor;
    sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    sxml_status status = sxml_item_get_node(request->node, &node);
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* As sxml_processor_parse_document's trees: destroyed with the last
       reference (D39). */
    status = sxml_arena_tree_create(
        processor->allocator,
        &(sxml_arena_tree_configuration){
            .allocator = processor->allocator,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = processor->item_heap,
            .maximum_node_count = processor->limits.maximum_tree_node_count,
            .maximum_depth = 0,
            .maximum_text_byte_count = processor->limits.maximum_tree_text_byte_count,
            .keeps_locations = false,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = true,
            .error_record = NULL},
        &validation->arena_tree);
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_validator_create(
            processor->allocator,
            &(sxml_schema_validator_configuration){
                .schema = schema,
                .validation_mode = request->validation_mode,
                .governing_type = request->governing_type,
                .result_builder = sxml_arena_tree_as_tree_builder(validation->arena_tree),
                .report_sink = request->report_sink,
                .error_record = request->error_record},
            &validation->validator);
    }
    if (status == SXML_STATUS_OK) {
        /* XSD assesses the infoset, not earlier annotations
           (include/sxml/schema.h). */
        status = sxml_tree_builder_copy_node(
            sxml_schema_validator_as_tree_builder(validation->validator),
            &(sxml_copy_request){.node = node,
                                 .preserves_namespaces = true,
                                 .inherits_namespaces = true,
                                 .preserves_type_annotations = false});
    }
    return status;
}

sxml_status sxml_schema_validate_node(const sxml_schema *schema,
                                      const sxml_schema_validation_request *request,
                                      sxml_item *validated_node) {
    if (schema == NULL || request == NULL || validated_node == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    node_validation validation = {.validator = NULL, .arena_tree = NULL};
    sxml_status status = copy_through_validator(schema, request, &validation);
    sxml_validation_outcome outcome = {.validity = SXML_VALIDITY_INVALID, .error_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_validator_get_outcome(validation.validator, &outcome);
    }
    if (status == SXML_STATUS_OK && outcome.validity == SXML_VALIDITY_INVALID) {
        /* The error record holds the first error (the report sink saw
           them all). */
        status = SXML_STATUS_DYNAMIC_ERROR;
    }
    sxml_schema_validator_destroy(validation.validator);
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(validation.arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(validation.arena_tree);
        return status;
    }
    *validated_node = sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}
