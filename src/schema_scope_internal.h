#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/schema.h"
#include "sxml/status.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

#include "function_library_internal.h"

/*
 * The in-scope schema definitions of a static context (XPath 4.0, section
 * 2.2.1; D174): what XPath, XQuery and XSLT ask of the compiled schemas
 * of a static context (include/sxml/xpath.h, in_scope_schemas), and the
 * one way schema errors become errors of those languages. Internal to
 * sxml; src/schema_scope.c.
 *
 * Lookups go through the schemas in order and stop at the first that
 * has the name (the built-in types are every schema's, so a caller that
 * looks for a built-in type looks in the type registry first).
 *
 * Thread safety: the thread of the processor (D35).
 */

/* A global declaration found in the in-scope schemas, and its schema. */
typedef struct sxml_schema_scope_element {
    const sxml_schema *schema;
    sxml_element_declaration_description description;
} sxml_schema_scope_element;

typedef struct sxml_schema_scope_attribute {
    const sxml_schema *schema;
    sxml_attribute_declaration_description description;
} sxml_schema_scope_attribute;

/* A type found in the in-scope schemas, and its schema. */
typedef struct sxml_schema_scope_type {
    const sxml_schema *schema;
    sxml_type_identifier type;
} sxml_schema_scope_type;

/* The first schema of schemas with a global element declaration named
   element_name. Returns false, leaving *found untouched, when none has. */
bool sxml_schema_scope_find_element(sxml_schema_view schemas, sxml_name_identifier element_name,
                                    sxml_schema_scope_element *found);

/* As sxml_schema_scope_find_element for attribute declarations. */
bool sxml_schema_scope_find_attribute(sxml_schema_view schemas,
                                      sxml_name_identifier attribute_name,
                                      sxml_schema_scope_attribute *found);

/* As sxml_schema_scope_find_element for type definitions. */
bool sxml_schema_scope_find_type(sxml_schema_view schemas, sxml_name_identifier type_name,
                                 sxml_schema_scope_type *found);

/* The first schema of schemas that registered type (a type annotation),
   so that validating against it finds it; NULL when none did. */
const sxml_schema *sxml_schema_scope_find_type_owner(sxml_schema_view schemas,
                                                     sxml_type_identifier type);

/*
 * The constructor function of a simple type of the in-scope schemas
 * (XPath 4.0, section 4.5.1.3, "Constructor Functions"): when
 * query->arity is 1 and query->function_name names an atomic, list or
 * union type of schemas that is not built-in (the function library makes
 * those), *entry becomes the CONSTRUCTOR entry the function library makes
 * for a built-in type: xs:anyAtomicType? to T? (T's item type * for a
 * list type), its signature registered in registry. Otherwise *entry is
 * left untouched. Returns the registry's failures.
 */
typedef struct sxml_schema_scope_constructor_request {
    sxml_schema_view schemas;
    sxml_type_registry *type_registry;
    const sxml_function_query *query;
} sxml_schema_scope_constructor_request;

SXML_NODISCARD sxml_status sxml_schema_scope_find_constructor(
    const sxml_schema_scope_constructor_request *request, sxml_function_entry *entry);

/*
 * What fn:element-to-map's schema-based conversion reads of a complex
 * type a schema of schemas registered (F&O 4.0, 14.6.6): whether it has
 * {attribute uses} or an {attribute wildcard}, and for element-only
 * content the element declaration terms of its {particle}, expanded
 * through model groups: whether open content or a wildcard term is there,
 * how many names there are, and whether one name may occur more than once
 * or every name at most once (maxOccurs multiplied down the levels and
 * summed over the terms of a name; unbounded is infinite). child_name is
 * the name when there is one. is_found is false when no schema of schemas
 * registered type as a complex type. Returns the allocator's failures.
 */
typedef struct sxml_schema_scope_content_request {
    sxml_schema_view schemas;
    sxml_type_identifier type;
    sxml_allocator allocator;
} sxml_schema_scope_content_request;

typedef struct sxml_schema_scope_content {
    bool is_found;
    bool has_attributes;
    bool has_wildcards;
    size_t child_name_count;
    bool has_repeated_name;
    bool has_names_at_most_once;
    sxml_name_identifier child_name;
} sxml_schema_scope_content;

SXML_NODISCARD sxml_status sxml_schema_scope_describe_content(
    const sxml_schema_scope_content_request *request, sxml_schema_scope_content *content);

/* True when a document schema was assembled from has the target
   namespace namespace_uri (SXML_NAMESPACE_NONE for none). */
bool sxml_schema_scope_defines_namespace(const sxml_schema *schema,
                                         sxml_namespace_uri_identifier namespace_uri);

/*
 * Whether the processor's resource resolver delivers a schema document
 * (XQuery's "import schema" and XSLT's xsl:import-schema may ignore a
 * location hint that leads nowhere, and a namespace no document is found
 * for: XQuery 4.0, 5.11; XSLT 4.0, 3.13): request->document is RESOLVED,
 * its document_uri resolved against request->base_uri (empty: asked by
 * its target namespace alone). Asks once with kind SCHEMA_DOCUMENT and
 * releases what is delivered; is_delivered is false when nothing is or
 * the processor has no resolver. A document asked by the xml: or fn:
 * namespace alone is always delivered: the schema assembly knows those
 * namespaces' documents (src/schema_assemble.c). Returns the resolver's
 * and the URI resolution's failures.
 */
typedef struct sxml_schema_scope_probe {
    sxml_processor *processor;
    sxml_schema_document document;
    sxml_byte_view base_uri;
} sxml_schema_scope_probe;

typedef struct sxml_schema_scope_delivery {
    bool is_delivered;
} sxml_schema_scope_delivery;

SXML_NODISCARD sxml_status sxml_schema_scope_probe_document(const sxml_schema_scope_probe *request,
                                                            sxml_schema_scope_delivery *delivery);

/*
 * The members of the substitution group of a global element declaration
 * (XSD 1.1 Part 1, 3.3.6.2, transitively), the head first, that an
 * element may stand for (XPath 4.0, 3.2.7.3, schema-element(N)): not
 * abstract, and none when the head's {disallowed substitutions} has
 * substitution. member_index 0 is the head itself.
 */
typedef struct sxml_schema_scope_member_request {
    sxml_schema_scope_element head;
    size_t member_index;
} sxml_schema_scope_member_request;

/* The number of member_index values (the head and its members, abstract
   ones included: sxml_schema_scope_get_member skips them). */
size_t sxml_schema_scope_member_count(const sxml_schema_scope_element *head);

/* Writes member request->member_index to *member and returns true; false
   when that member is abstract or blocked. */
bool sxml_schema_scope_get_member(const sxml_schema_scope_member_request *request,
                                  sxml_element_declaration_description *member);

/*
 * An error a schema raised (its code a constraint name in no namespace,
 * include/sxml/schema.h) raised again as an error of the language
 * around it: error_code in the err namespace, its description the
 * constraint name, ": " and the schema's description, its location the
 * schema's unless location is given (line_number not 0). Keeps the text
 * in the item heap (sxml_item_heap_raise_error).
 */
typedef struct sxml_schema_scope_error {
    sxml_item_heap *item_heap;
    /* For the description's text while it is raised. */
    sxml_allocator allocator;
    /* The status the schema returned, which has an error record. */
    sxml_status error_status;
    /* ASCII, such as "XQST0059"; static. */
    sxml_byte_view error_code;
    /* The schema's error. */
    const sxml_error_record *schema_error;
    /* Receives the language's error; may be NULL. */
    sxml_error_record *error_record;
    sxml_source_location location;
} sxml_schema_scope_error;

/* Returns request->error_status, or the heap's failure. */
SXML_NODISCARD sxml_status sxml_schema_scope_raise(const sxml_schema_scope_error *request);

/* The schema documents of the fn: namespace that every processor knows
   (F&O 4.0, appendix D: D.1 for fn:analyze-string, D.2 for
   fn:json-to-xml, D.3 for fn:csv-to-xml); src/schema_assemble.c reads
   them for an import of the namespace the resolver does not deliver.
   document_index is below SXML_FUNCTION_NAMESPACE_DOCUMENT_COUNT. */
enum { SXML_FUNCTION_NAMESPACE_DOCUMENT_COUNT = 3 };

sxml_byte_view sxml_schema_function_namespace_document(size_t document_index);

/* ------------------------------------------------------------ Validate */

/* The $mode of the validate row. */
typedef enum sxml_schema_validate_mode {
    SXML_SCHEMA_VALIDATE_MODE_STRICT = 0,
    SXML_SCHEMA_VALIDATE_MODE_LAX = 1,
    /* "validate type T": $type names T. */
    SXML_SCHEMA_VALIDATE_MODE_TYPE = 2,
    /* Added to one of the above: XSLT's rules (below). */
    SXML_SCHEMA_VALIDATE_MODE_XSLT_RULES = 4,
} sxml_schema_validate_mode;

/*
 * validate($node as item()*, $mode as xs:integer, $type as xs:QName?) as
 * node(), the internal native row (in no function table) XQuery's validate
 * expression lowers to (XQuery 4.0, 4.25; D174; src/function_library_schema.c):
 * $node must be one document or element node (XQTY0030), a document with
 * one element child and no text children (XQDY0061); its validated copy
 * (sxml_schema_validate_node) against the in-scope schema that declares
 * the validation root's name (strict: XQDY0084 when none does; lax: the
 * first schema then) or, for $mode TYPE, that defines $type. An invalid
 * node is XQDY0027 with the constraint name in the description.
 *
 * validate-items($node, $mode, $type) as item()*, a row of its own (D180),
 * takes SXML_SCHEMA_VALIDATE_MODE_XSLT_RULES added to $mode and has the
 * rules of XSLT's [xsl:]validation and [xsl:]type attributes (XSLT 4.0,
 * 25; D179):
 * every item of $node is validated in turn, documents, elements and
 * attributes (each a validation root, its in-scope schema found as
 * above), other items kept as they are; a document whose children are
 * not one element, no text and any comments and processing instructions
 * is XTTE1550; strict validation without a declaration XTTE1512, an
 * invalid node XTTE1510 (strict), XTTE1515 (lax) or XTTE1540 (by type),
 * a document that breaks an ID/IDREF constraint XTTE1555, an attribute by
 * a complex type XTTE1535.
 */
extern const sxml_native_function sxml_schema_validate_function;

/* validate-items($node, $mode, $type) as item()*, XSLT's rules (above). */
extern const sxml_native_function sxml_schema_validate_items_function;

/*
 * The xsd-validation option of fn:parse-xml and fn:doc (F&O 4.0, 17.2.1;
 * D174), xsd_validation its value: "skip" leaves *document_node as it is;
 * "strict", "lax" and "type Q{uri}local" replace it, owned, by its
 * validated copy against the in-scope schemas of call's static context
 * (FODC0008 for another value or a type not in the static context,
 * FODC0014 for invalid content or, strict, an element without a
 * declaration); on failure *document_node is released.
 */
SXML_NODISCARD sxml_status sxml_schema_scope_validate_document(const sxml_function_call *call,
                                                               sxml_byte_view xsd_validation,
                                                               sxml_item *document_node);

/*
 * Replaces *node, a document or element node owned by the caller, with
 * its validated copy (strict) against the schema of the fn: namespace
 * (sxml_processor_get_function_namespace_schema): what fn:json-to-xml
 * with validate=true and fn:analyze-string return. An in-scope schema
 * of the namespace is tried first, so that its declarations match. An
 * invalid node is error_code with the constraint name in the
 * description; on failure *node is released. Without an evaluation
 * environment (no processor) *node stays as it is, or, is_required,
 * error_code is raised.
 */
typedef struct sxml_function_namespace_validation {
    sxml_byte_view error_code;
    bool is_required;
    sxml_item *node;
} sxml_function_namespace_validation;

SXML_NODISCARD sxml_status sxml_schema_scope_validate_by_function_namespace(
    const sxml_function_call *call, const sxml_function_namespace_validation *validation);
