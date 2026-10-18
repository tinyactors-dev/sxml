#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "schema_internal.h"

/*
 * The schema validator's state (include/sxml/schema.h;
 * src/schema_internal.h, "The validator's seams"; docs/DECISIONS.md D165,
 * D167, D173). Internal to sxml: src/schema_validate.c is the tree
 * builder, src/schema_validate_value.c assesses simple values and keeps
 * the ID table; the packets of identity constraints (5.8) and assertions
 * and type alternatives (5.9) read the open elements from here.
 *
 * Section numbers are of XSD 1.1 Part 1 unless they name Part 2.
 *
 * Events and the held-back start
 * ------------------------------
 * The validator receives the events of one tree (include/sxml/tree.h)
 * and passes them on to its result builder. An element's start is held
 * back, with its namespace bindings and attributes (glossary
 * pending_element), until its first content event or its end: xsi:type,
 * xsi:nil and the attributes decide its governing type and its
 * annotation (3.3.4.1, 3.4.4). Then the element gets a validation frame
 * (glossary validation_frame) on a stack in allocator memory (D71), and
 * its start, bindings and assessed attributes (schema normalized values,
 * annotations, defaults added) go to the result builder.
 *
 * Assessment
 * ----------
 * Every element is assessed one of three ways (glossary assessment, 5.2
 * and 3.10.1): strictly, against a governing type (from a declaration,
 * xsi:type or the configuration); laxly, when nothing governs it (its
 * attributes and children are assessed where global declarations exist,
 * and it is annotated xs:anyType); or skipped, with its subtree, when a
 * wildcard with processContents skip matched it (annotations xs:anyType
 * and xs:untypedAtomic, XDM 4.0, 3.3.1.1). After an error in an element's
 * content, its remaining children are assessed laxly and its end is not
 * checked, so one error gives one report.
 *
 * Content
 * -------
 * Children are matched by the content model cursor of their parent's
 * type (src/schema_internal.h, 5.6's matcher). Simple content is
 * collected and assessed at the element's end (cvc-type.3.1.3,
 * cvc-complex-type.2.2), and passed on as one text event of its schema
 * normalized value (XDM 4.0, 7.5.7.4) or of the default or fixed value of
 * an empty element (cvc-elt.5.1); whitespace text in content that is not
 * mixed is dropped (7.5.7.4).
 *
 * Thread safety: the thread of the processor (D35).
 */

/* How an element (or attribute) is assessed (glossary assessment). */
typedef enum sxml_schema_assessment {
    /* Against a governing type. */
    SXML_SCHEMA_ASSESSMENT_STRICT = 0,
    /* Nothing governs it: its attributes and children are assessed where
       global declarations exist; validity not known. */
    SXML_SCHEMA_ASSESSMENT_LAX,
    /* Not assessed, with its subtree (processContents skip). */
    SXML_SCHEMA_ASSESSMENT_SKIP,
} sxml_schema_assessment;

/* One open element (glossary validation_frame). */
typedef struct sxml_schema_validation_frame {
    sxml_name_identifier element_name;
    sxml_schema_assessment assessment;
    /* The governing element declaration; 0 without one. */
    sxml_schema_element_declaration_index element_declaration;
    /* STRICT: the governing type definition; 0 otherwise. */
    sxml_schema_type_definition_index type_definition;
    /* STRICT: the content type's variety, SIMPLE for a simple type. */
    sxml_content_type_variety content_variety;
    /* SIMPLE: the simple type the content is assessed against. */
    sxml_schema_type_definition_index simple_type;
    /* The content model cursor of element-only and mixed content (NULL
       model: no child element allowed); its counter values are the
       validator's counter_values from first_counter. */
    const sxml_schema_content_model *content_model;
    uint32_t content_model_state;
    size_t first_counter;
    uint32_t counter_count;
    /* xsi:nil="true" was accepted (cvc-elt.3.2). */
    bool is_nilled;
    /* An error in the content: the rest is assessed laxly. */
    bool has_content_error;
    bool has_element_children;
    /* Character children were met (also whitespace ones). */
    bool has_character_children;
    /* Where the element's collected text starts in text_bytes. */
    size_t text_start;
    /* Where the element's namespace bindings start in namespace_bindings. */
    size_t binding_start;
    /* The element was annotated as an ID (simple content of type xs:ID). */
    bool is_id;
    /* The element's number, from 1 in document order (IDs, 3.17.5.2). */
    uint64_t element_number;
    /* While recorded: its node in the recorded tree. */
    uint32_t recorded_node;
    /* Where the element's own inheritable attributes start in
       inherited_attributes and inherited_bytes (3.3.5.6). */
    size_t first_inherited;
    size_t inherited_byte_start;
    sxml_source_location location;
} sxml_schema_validation_frame;

/* An attribute the open elements' descendants may inherit (3.3.5.6: one
   attributed to an attribute use, or governed by a declaration, whose
   {inheritable} is true); its value, as it appeared, is in
   inherited_bytes. */
typedef struct sxml_schema_inherited_attribute {
    sxml_name_identifier attribute_name;
    sxml_namespace_prefix_identifier namespace_prefix;
    size_t value_offset;
    size_t value_byte_count;
} sxml_schema_inherited_attribute;

/* An attribute of the held-back start; its value is in attribute_bytes. */
typedef struct sxml_schema_pending_attribute {
    sxml_name_identifier attribute_name;
    sxml_namespace_prefix_identifier namespace_prefix;
    size_t value_offset;
    size_t value_byte_count;
    sxml_source_location location;
} sxml_schema_pending_attribute;

/* The held-back start of an element (glossary pending_element) and what
   its parent's content decided for it. */
typedef struct sxml_schema_pending_element {
    bool is_pending;
    sxml_element_start_event event;
    sxml_schema_assessment assessment;
    /* The declaration the parent's content model (or the global
       declarations) gave; 0 when none. */
    sxml_schema_element_declaration_index element_declaration;
    /* The validation root's governing type from the configuration. */
    sxml_schema_type_definition_index governing_type;
    /* Strict without a declaration (the validation root in strict mode, a
       strict wildcard): an error unless xsi:type gives a type; the
       constraint to report. Empty when not so. */
    sxml_byte_view missing_declaration_constraint;
    /* A wildcard matched the element and a declaration of the parent's
       content model has its name: the element's governing type must be
       that declaration's (Element Declarations Consistent, XSD 1.1,
       3.4.4.2 clause 5). 0 when not so. */
    sxml_schema_element_declaration_index consistent_declaration;
    size_t binding_start;
} sxml_schema_pending_element;

/* The IDs and IDREFs of one validation root (glossary id_table). */
struct sxml_schema_id_table {
    sxml_allocator allocator;
    /* The values, back to back. */
    sxml_schema_scratch_array value_bytes;
    /* sxml_schema_id_entry: one per ID. */
    sxml_schema_scratch_array identifiers;
    /* Open addressing over identifiers (index + 1; 0 free), a power of
       two slots, at most half in use. */
    uint32_t *slots;
    size_t slot_count;
    /* sxml_schema_idref_entry: one per IDREF, checked at the end. */
    sxml_schema_scratch_array references;
};

/* A check that needs an element's recorded subtree, run when the
   outermost recorded element ends (glossary recorded_subtree). */
typedef struct sxml_schema_deferred_check {
    /* The element's node in the recorded tree. */
    uint32_t node_index;
    /* Its declaration's identity constraints (3.11.4), when it has any. */
    sxml_schema_element_declaration_index element_declaration;
    /* Its complex type's assertions (3.13.4.1), when it has any. */
    sxml_schema_type_definition_index type_definition;
} sxml_schema_deferred_check;

/*
 * The subtree recorder (src/schema_validate_record.c; glossary
 * recorded_subtree): while an element whose declaration has identity
 * constraints or whose type has assertions is open, the annotated events
 * the validator sends go to an arena tree as well, one tree for the
 * outermost such element, whose inner ones are nodes of it. The tree is
 * finished when that element ends; then the checks deferred for it and
 * its descendants run, in the order the elements ended, and the tree is
 * released.
 */
typedef struct sxml_schema_subtree_recorder {
    /* NULL when nothing is recorded. */
    sxml_arena_tree *arena_tree;
    /* The frame index of the outermost recorded element. */
    size_t root_frame;
    /* Nodes the tree has so far (namespace bindings make none, adjacent
       texts one), and whether the last node is a text. */
    uint32_t node_count;
    bool ends_in_text;
    /* The declaration the next start_element records. */
    sxml_schema_element_declaration_index next_declaration;
    /* uint32_t per node: its governing element declaration (0 for other
       nodes and laxly assessed elements). */
    sxml_schema_scratch_array element_declarations;
    /* sxml_schema_deferred_check, in the order the elements ended. */
    sxml_schema_scratch_array checks;
} sxml_schema_subtree_recorder;

struct sxml_schema_validator {
    /* The configuration, kept: schema and states by reference. */
    sxml_schema_validator_configuration configuration;
    const sxml_schema_components *components;
    sxml_allocator allocator;
    /* The allocator of sxml_schema_validator_create; the null allocator
       after initialize. */
    sxml_allocator creating_allocator;
    sxml_item_heap *item_heap;
    sxml_name_pool *name_pool;
    const sxml_type_registry *type_registry;
    /* Names the validator compares with, found once (0: never interned,
       so no event can carry them). */
    sxml_name_identifier xsi_type_name;
    sxml_name_identifier xsi_nil_name;
    /* sxml_schema_validation_frame. */
    sxml_schema_scratch_array frames;
    sxml_schema_pending_element pending;
    /* sxml_schema_pending_attribute of the held-back start, and their
       values. */
    sxml_schema_scratch_array pending_attributes;
    sxml_schema_scratch_array attribute_bytes;
    /* sxml_schema_inherited_attribute: the inheritable attributes of the
       open elements, outermost first, and their values (type
       alternatives, 3.12.4 clause 1.1.3). */
    sxml_schema_scratch_array inherited_attributes;
    sxml_schema_scratch_array inherited_bytes;
    /* uint8_t per attribute use of the element's type: 1 once an
       attribute matched it (cvc-complex-type.4, defaults). */
    sxml_schema_scratch_array attribute_uses_met;
    /* sxml_namespace_binding: the bindings of the open elements and the
       held-back one, outermost first. */
    sxml_schema_scratch_array namespace_bindings;
    /* sxml_namespace_binding: the in-scope bindings, innermost first,
       made for a cast (src/cast.c takes the first binding of a prefix). */
    sxml_schema_scratch_array cast_bindings;
    /* uint32_t: particles still to visit when a content model's
       declarations are searched (an explicit stack, D71). */
    sxml_schema_scratch_array particle_stack;
    /* uint64_t: the content model counters of the open elements. */
    sxml_schema_scratch_array counter_values;
    /* Collected simple content of the open elements, and scratch text. */
    sxml_schema_scratch_array text_bytes;
    sxml_schema_scratch_array value_bytes;
    /* The document's base URI (its start event's base URI, else its
       document URI), the base URI of the parentless copies made for
       assertions and type alternatives (fn:base-uri, XDM 4.0, 5.2). */
    sxml_schema_scratch_array base_uri_bytes;
    /* The names of the document's unparsed entities, each followed by a
       zero byte (ENTITY values, Part 2, 3.3.20). */
    sxml_schema_scratch_array entity_names;
    struct sxml_schema_id_table id_table;
    sxml_schema_subtree_recorder recorder;
    /* The tree so far: a document was started; the root has ended. */
    bool is_document;
    bool has_root;
    bool is_complete;
    /* The validation root was assessed laxly without a type. */
    bool is_root_not_known;
    /* Elements started so far (validation frames' numbers), and the
       number of the element whose attributes are being assessed (0 for a
       parentless attribute). */
    uint64_t element_count;
    uint64_t attribute_element_number;
    size_t error_count;
};

/* The type a value of a registered simple type is cast to: the type
   itself, but xs:untypedAtomic for the special types xs:anySimpleType and
   xs:anyAtomicType, which take every string as it is (Part 2, 3.2.1,
   3.2.2) and which no cast targets. */
static inline sxml_type_identifier sxml_schema_cast_target_of(sxml_type_identifier registered) {
    if (registered.index == (uint32_t)SXML_TYPE_XS_ANY_SIMPLE_TYPE ||
        registered.index == (uint32_t)SXML_TYPE_XS_ANY_ATOMIC_TYPE) {
        return (sxml_type_identifier){.index = SXML_TYPE_XS_UNTYPED_ATOMIC};
    }
    return registered;
}

/* --------------------------------------------- src/schema_validate_value.c */

/* A simple value to assess (3.16.4 String Valid; Part 2, 4.1.4). */
typedef struct sxml_schema_value_request {
    /* A simple type definition of the schema. */
    sxml_schema_type_definition_index simple_type;
    /* The value as it appears (attribute value, element content);
       borrowed. */
    sxml_byte_view text;
    sxml_source_location location;
    /* The element an ID of the value identifies (sxml_schema_id_request);
       0 for none. */
    uint64_t element_number;
} sxml_schema_value_request;

/* What assessing a simple value gave. */
typedef struct sxml_schema_value_outcome {
    bool is_valid;
    /* The schema normalized value (Part 2, 4.3.6 whiteSpace): in
       validator->value_bytes or the request's text; valid until the next
       value is assessed. */
    sxml_byte_view normalized_value;
    /* The actual value, owned by the caller once is_valid (an
       xs:untypedAtomic for xs:anySimpleType and xs:anyAtomicType). */
    sxml_sequence value;
    /* An item of the value is an xs:ID, or derived; the value is one
       such item (XDM 4.0, 7.5.2.4 is-id). */
    bool is_id;
    /* An item of the value is an xs:IDREF, or derived (is-idrefs). */
    bool is_idrefs;
} sxml_schema_value_outcome;

/*
 * Assesses request->text against request->simple_type: the cast engine
 * (src/cast.c, whiteSpace, the lexical mapping and every facet) with the
 * in-scope namespaces, then the ENTITY rule (Part 2, 3.3.20), the
 * assertion seam (5.9), and the ID table (cvc-id). An invalid value is
 * reported (cvc-datatype-valid.1.2.1, or the facet's constraint name)
 * and gives is_valid false, as does a false assertion
 * (cvc-assertions-valid). Returns the report's
 * status, the assertion seam's and the heap's failures.
 */
SXML_NODISCARD sxml_status sxml_schema_validate_simple_value(
    sxml_schema_validator *validator, const sxml_schema_value_request *request,
    sxml_schema_value_outcome *outcome);

/* The in-scope namespace bindings of the open elements (and the held-back
   one), innermost first, in validator->cast_bindings. */
SXML_NODISCARD sxml_status sxml_schema_validator_get_cast_bindings(
    sxml_schema_validator *validator, sxml_namespace_binding_view *bindings);

/* One validation error, as sxml_schema_validator_report takes it. */
typedef struct sxml_schema_validation_error {
    /* The constraint name, static text. */
    sxml_byte_view constraint_name;
    /* Static text (the error record keeps it). */
    sxml_byte_view description;
    sxml_source_location location;
} sxml_schema_validation_error;

/* Reports a validation error (sxml_schema_validator_report) and returns
   its status. */
SXML_NODISCARD sxml_status sxml_schema_validator_report_error(
    sxml_schema_validator *validator, const sxml_schema_validation_error *error);

/* Appends bytes to a scratch array of bytes (entry_byte_count 1),
   doubling its capacity. Returns the allocator's failures and
   SXML_STATUS_LIMIT_EXCEEDED on size overflow. */
SXML_NODISCARD sxml_status sxml_schema_scratch_array_append_bytes(sxml_allocator allocator,
                                                                  sxml_schema_scratch_array *array,
                                                                  sxml_byte_view bytes);

/* -------------------------------------------- src/schema_validate_record.c */

/* Makes an idle recorder whose memory comes from allocator. */
void sxml_schema_recorder_initialize(sxml_schema_subtree_recorder *recorder,
                                     sxml_allocator allocator);

/* Releases the recorder's tree and memory; it is then idle. */
void sxml_schema_recorder_release(sxml_schema_subtree_recorder *recorder, sxml_allocator allocator);

/* A new arena tree for a parentless element, with the processor's tree
   limits, kept by the caller, who destroys it (D39): the recorded
   subtree, the element copy of type alternatives, the copy assertions
   evaluate on. Returns the arena tree's failures. */
SXML_NODISCARD sxml_status sxml_schema_validator_create_tree(
    const sxml_schema_validator *validator, sxml_arena_tree **arena_tree);

/* Starts recording at the element the next frame is (frames.entry_count):
   a new arena tree with the processor's limits. Returns the arena tree's
   and the allocator's failures. */
SXML_NODISCARD sxml_status sxml_schema_recorder_start(sxml_schema_validator *validator);

/* The builder that records events into the recorder's tree, counting its
   nodes. Valid while recording. */
sxml_tree_builder sxml_schema_recorder_as_tree_builder(sxml_schema_validator *validator);

/* Sends the in-scope namespace bindings of the held-back element to
   builder, each prefix once (its innermost binding), as the bindings of
   a parentless element (the root of a recorded tree, an element copy). */
SXML_NODISCARD sxml_status sxml_schema_send_in_scope_bindings(sxml_schema_validator *validator,
                                                              sxml_tree_builder builder);

/* Queues a check for the element of frame (its recorded node). */
SXML_NODISCARD sxml_status sxml_schema_recorder_defer_check(
    sxml_schema_validator *validator, const sxml_schema_deferred_check *check);

/* Finishes the tree once the outermost recorded element has ended, runs
   the deferred checks through the seams of 5.8 and 5.9 and releases the
   tree. Returns the seams' and the tree's failures. */
SXML_NODISCARD sxml_status sxml_schema_recorder_finish(sxml_schema_validator *validator);

/* The type alternative the declaration's type table selects for the
   held-back element (3.12.4), through the seam of 5.9, which receives a
   parentless copy of the element with its attributes as they appear and
   its in-scope namespaces, and the attributes it inherits (3.3.5.6) that
   it does not have itself; the declared type when the table is empty. */
SXML_NODISCARD sxml_status sxml_schema_select_type(
    sxml_schema_validator *validator, sxml_schema_element_declaration_index element_declaration,
    sxml_schema_type_definition_index *selected_type);

/* Makes an empty ID table whose memory comes from allocator. */
void sxml_schema_id_table_initialize(sxml_schema_id_table *id_table, sxml_allocator allocator);

/* Releases the ID table's memory; the table is then empty. */
void sxml_schema_id_table_release(sxml_schema_id_table *id_table);
