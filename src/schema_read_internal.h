#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "schema_internal.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/view.h"

/*
 * The read stage's two halves (src/schema_internal.h, READ; D167, D170).
 * Internal to sxml.
 *
 * src/schema_assemble.c is the schema assembly (glossary schema_assembly;
 * XSD 1.1 Part 1, 4.2): the work list of schema documents. It parses each
 * document into an arena tree, reads it once (by absolute URI and target
 * namespace), gives a document included without a targetNamespace its
 * includer's (glossary chameleon_inclusion), and runs
 * sxml_schema_read_document on it; xs:include, xs:import, xs:redefine and
 * xs:override elements the reader meets come back through
 * sxml_schema_assembly_add_reference and join the list. When the list is
 * empty, the children of every xs:redefine and xs:override are read, in
 * the order the elements were met, so the components they replace exist
 * (glossary redefinition). The trees live until READ returns.
 *
 * src/schema_read.c reads one schema document's elements into components
 * "as read" (src/schema_internal.h): one table of schema element roles
 * (glossary schema_element_role) gives every element's attributes and
 * child slots (glossary child_slot) as the schema for schema documents
 * (Part 1, appendix A) allows, and the component it is read into; the
 * walk is a loop over the tree's nodes in document order (D71: no
 * recursion), and the lists of a component (particles, attribute uses,
 * facets, ...) are written once its children are read.
 */

/* What one call of sxml_schema_read_document reads. */
typedef enum sxml_schema_read_kind {
    /* The xs:schema element and everything in it, except the children of
       xs:redefine and xs:override, which are only referred to. */
    SXML_SCHEMA_READ_KIND_DOCUMENT = 0,
    /* The children of one xs:redefine element (Part 1, 4.2.4): each
       replaces the component of its name, and a type's base, a group's or
       attribute group's reference to its own name, is the original. */
    SXML_SCHEMA_READ_KIND_REDEFINE,
    /* The children of one xs:override element (Part 1, 4.2.5): each
       replaces the component of its name; one whose name has no component
       is not read. */
    SXML_SCHEMA_READ_KIND_OVERRIDE,
} sxml_schema_read_kind;

/*
 * What a schema document's xs:schema element and xs:defaultOpenContent say
 * for every component in it (Part 1, 3.17.2). The DOCUMENT read writes
 * them; the REDEFINE and OVERRIDE reads of the same document use them.
 */
typedef struct sxml_schema_document_defaults {
    bool is_read;
    /* elementFormDefault and attributeFormDefault are "qualified". */
    bool qualifies_elements;
    bool qualifies_attributes;
    /* blockDefault and finalDefault. */
    sxml_schema_derivation_set block_default;
    sxml_schema_derivation_set final_default;
    /* defaultAttributes, a reference to an attribute group; 0 when
       absent. */
    sxml_schema_reference_index default_attributes;
    /* xpathDefaultNamespace of xs:schema, resolved; ##defaultNamespace
       is resolved on each host element instead (3.13.2: its in-scope
       default namespace). */
    sxml_namespace_uri_identifier xpath_default_namespace;
    bool xpath_default_is_default_namespace;
    /* xs:defaultOpenContent: its mode (NONE when absent), wildcard and
       appliesToEmpty. */
    sxml_schema_open_content_mode open_content_mode;
    sxml_schema_wildcard_index open_content_wildcard;
    bool open_content_applies_to_empty;
} sxml_schema_document_defaults;

/* The schema assembly (src/schema_assemble.c). */
typedef struct sxml_schema_assembly sxml_schema_assembly;

/* One read of a schema document's elements. Borrowed for the call. */
typedef struct sxml_schema_read_request {
    sxml_schema_read_kind read_kind;
    sxml_schema_document_index document;
    /* The document's tree, its xs:schema element, and the element to read:
       the xs:schema element, or an xs:redefine or xs:override child of
       it. */
    sxml_tree *tree;
    sxml_node_index schema_element;
    sxml_node_index start_element;
    /* The target namespace of the document's components: its
       targetNamespace, or for a chameleon inclusion the includer's. */
    sxml_namespace_uri_identifier target_namespace;
    /* The document has no targetNamespace and takes the includer's: its
       unqualified references are in target_namespace too (Part 1, F.1). */
    bool is_chameleon;
    /* The document's absolute URI, the base URI of its elements before
       xml:base; may be empty. */
    sxml_byte_view document_uri;
    /* In/out: the document's defaults, written by the DOCUMENT read. */
    sxml_schema_document_defaults *defaults;
    /* REDEFINE and OVERRIDE: the document the element refers to. */
    sxml_schema_document_index redefined_document;
} sxml_schema_read_request;

/*
 * Reads request's elements into components, reporting what breaks the
 * XML representation constraints (src-*, the schema for schema
 * documents) through sxml_schema_compilation_report, and passes each
 * xs:include, xs:import, xs:redefine and xs:override to assembly. Returns
 * the failures of the components, the tree, the name pool and the
 * assembly, and the report's status when it stops the compilation.
 * src/schema_read.c.
 */
SXML_NODISCARD sxml_status sxml_schema_read_document(sxml_schema_assembly *assembly,
                                                     const sxml_schema_read_request *request);

/* How a document refers to another one (Part 1, 4.2.3 to 4.2.6). */
typedef enum sxml_schema_document_relation {
    SXML_SCHEMA_DOCUMENT_RELATION_INCLUDE = 0,
    SXML_SCHEMA_DOCUMENT_RELATION_IMPORT,
    SXML_SCHEMA_DOCUMENT_RELATION_REDEFINE,
    SXML_SCHEMA_DOCUMENT_RELATION_OVERRIDE,
} sxml_schema_document_relation;

/* An xs:include, xs:import, xs:redefine or xs:override met. Views are
   borrowed for the call. */
typedef struct sxml_schema_document_reference {
    sxml_schema_document_relation relation;
    /* The referring document's read. */
    const sxml_schema_read_request *referrer;
    /* The element, for the later read of a redefine's or override's
       children, and where its errors are reported. */
    sxml_node_index element;
    sxml_schema_position position;
    /* schemaLocation, whitespace-trimmed; empty when absent (xs:import
       may have none). */
    sxml_byte_view schema_location;
    /* The base URI of the element, which schema_location is resolved
       against. */
    sxml_byte_view base_uri;
    /* xs:import: its namespace attribute (SXML_NAMESPACE_NONE when
       absent). */
    sxml_namespace_uri_identifier imported_namespace;
} sxml_schema_document_reference;

/*
 * Adds the document reference refers to to the assembly's work list (or
 * finds it read already), and, for xs:redefine and xs:override, the read
 * of the element's children. Reports src-include, src-import,
 * src-redefine and src-override. Returns the failures of the resolver, the
 * parser and the allocator, and the report's status when it stops the
 * compilation. src/schema_assemble.c.
 */
SXML_NODISCARD sxml_status sxml_schema_assembly_add_reference(
    sxml_schema_assembly *assembly, const sxml_schema_document_reference *reference);

/* A component a redefine's or override's child replaces in the document
   it refers to. */
typedef struct sxml_schema_redefinition_key {
    sxml_schema_name_key name;
    sxml_schema_document_index redefined_document;
} sxml_schema_redefinition_key;

/* What noting a redefinition found. */
typedef struct sxml_schema_redefinition_note {
    /* No xs:redefine or xs:override child replaced the component before. */
    bool is_first;
} sxml_schema_redefinition_note;

/*
 * Notes that a child of xs:redefine or xs:override replaces the global
 * component key names in key's document, and writes to note whether one
 * did before (a component redefined twice in one document: src-redefine,
 * src-override; a chain of redefinitions of other documents is not).
 * Returns the
 * allocator's failures. src/schema_assemble.c.
 */
SXML_NODISCARD sxml_status sxml_schema_assembly_note_redefinition(
    sxml_schema_assembly *assembly, sxml_schema_redefinition_key key,
    sxml_schema_redefinition_note *note);

/* The compilation the assembly reads for. src/schema_assemble.c. */
sxml_schema_compilation *sxml_schema_assembly_get_compilation(const sxml_schema_assembly *assembly);
