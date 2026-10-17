#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "document.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xpath.h"

/*
 * QT4 environments, resolved (glossary: qt4_environment;
 * docs/QT4_CATALOG.md, "Environments"; third_party/qt4tests/
 * catalog-schema.xsd, element "environment").
 *
 * An environment is read from its element into plain structs whose arrays
 * live in an arena and whose views point into the document the element is
 * in (or into the arena, for resolved file paths). Every file attribute is
 * resolved against the file that contains the environment and given as a
 * suite path (suite_path.h). Nothing is read or evaluated here: running a
 * test (packet 1.25) parses the sources and evaluates the select
 * expressions.
 */

/* Where a test case's environment came from. */
typedef enum sxml_qt4_environment_origin {
    /* The test case has no environment element: an empty static and
       dynamic context, with the context value absent. */
    SXML_QT4_ENVIRONMENT_ORIGIN_ABSENT,
    /* An environment element in the test case itself. */
    SXML_QT4_ENVIRONMENT_ORIGIN_INLINE,
    /* ref= naming an environment of the test-set file. */
    SXML_QT4_ENVIRONMENT_ORIGIN_TEST_SET,
    /* ref= naming an environment of catalog.xml. */
    SXML_QT4_ENVIRONMENT_ORIGIN_CATALOG,
} sxml_qt4_environment_origin;

/* A <source>: an XML document of the environment. */
typedef struct sxml_qt4_source {
    /* "." for the context value, "$name" for an external variable, empty
       for a document reached only by its URI. */
    sxml_byte_view role;
    /* The suite path of the document. */
    sxml_byte_view file_path;
    /* The URI under which fn:doc finds it; empty for none. */
    sxml_byte_view uri;
    /* "strict" or "lax" asks for schema validation; empty for none. */
    sxml_byte_view validation;
    /* declared="true": the query prolog declares the variable itself. */
    bool is_declared;
} sxml_qt4_source;

typedef struct sxml_qt4_source_view {
    const sxml_qt4_source *sources;
    size_t source_count;
} sxml_qt4_source_view;

/* A <param>: an external variable. */
typedef struct sxml_qt4_parameter {
    /* The variable's QName as written, without "$". */
    sxml_byte_view name;
    /* An XPath expression giving the value; empty for none. */
    sxml_byte_view select;
    /* The declared SequenceType as text (D51,
       sxml_xpath_parse_sequence_type); empty for none. */
    sxml_byte_view declared_type;
    /* A source document as the value; empty for none. */
    sxml_byte_view source;
    /* declared="true": the query prolog declares the variable itself. */
    bool is_declared;
} sxml_qt4_parameter;

typedef struct sxml_qt4_parameter_view {
    const sxml_qt4_parameter *parameters;
    size_t parameter_count;
} sxml_qt4_parameter_view;

/* A <namespace>: a statically known namespace. */
typedef struct sxml_qt4_namespace_declaration {
    /* Empty for the default namespace for elements and types. */
    sxml_byte_view namespace_prefix;
    sxml_byte_view namespace_uri;
} sxml_qt4_namespace_declaration;

typedef struct sxml_qt4_namespace_declaration_view {
    const sxml_qt4_namespace_declaration *namespace_declarations;
    size_t namespace_declaration_count;
} sxml_qt4_namespace_declaration_view;

/* A <resource>: a text resource reached by URI. */
typedef struct sxml_qt4_resource {
    sxml_byte_view uri;
    sxml_byte_view file_path;
    /* Empty for none. */
    sxml_byte_view media_type;
    sxml_byte_view encoding;
} sxml_qt4_resource;

typedef struct sxml_qt4_resource_view {
    const sxml_qt4_resource *resources;
    size_t resource_count;
} sxml_qt4_resource_view;

/* Texts, such as the queries of a collection. */
typedef struct sxml_qt4_text_view {
    const sxml_byte_view *texts;
    size_t text_count;
} sxml_qt4_text_view;

/* A <collection> for fn:collection. */
typedef struct sxml_qt4_collection {
    /* Empty for the default collection. */
    sxml_byte_view uri;
    sxml_qt4_source_view sources;
    sxml_qt4_resource_view resources;
    /* XQuery expressions whose results join the collection. */
    sxml_qt4_text_view queries;
} sxml_qt4_collection;

typedef struct sxml_qt4_collection_view {
    const sxml_qt4_collection *collections;
    size_t collection_count;
} sxml_qt4_collection_view;

/* A <collation>. */
typedef struct sxml_qt4_collation {
    sxml_byte_view uri;
    /* default="true": the default collation. */
    bool is_default;
} sxml_qt4_collation;

typedef struct sxml_qt4_collation_view {
    const sxml_qt4_collation *collations;
    size_t collation_count;
} sxml_qt4_collation_view;

/* A <schema>: schema import, which needs schema awareness. */
typedef struct sxml_qt4_schema {
    /* The target namespace; empty for none. */
    sxml_byte_view uri;
    /* The suite path of the schema document; empty for none. */
    sxml_byte_view file_path;
} sxml_qt4_schema;

typedef struct sxml_qt4_schema_view {
    const sxml_qt4_schema *schemas;
    size_t schema_count;
} sxml_qt4_schema_view;

/*
 * A <decimal-format>: a decimal format of the static context
 * (catalog-schema.xsd, element "decimal-format"; F&O 4.0, "Defining a
 * decimal format"). Each attribute the element has is one property, its
 * value as written (sxml_decimal_format_property: a marker, a marker and
 * rendition such as "%:pc", or a rendition).
 */
typedef struct sxml_qt4_decimal_format {
    /* The name's namespace URI, its prefix bound by the element's in-scope
       namespaces; empty for a name without a prefix (XSLT 4.0 and XQuery
       4.0 put unprefixed decimal-format names in no namespace). */
    sxml_byte_view namespace_uri;
    /* The name's local part; empty for the unnamed decimal format. */
    sxml_byte_view local_name;
    /* The name as written, for listings; empty for the unnamed one. */
    sxml_byte_view name;
    sxml_decimal_format_property_view properties;
} sxml_qt4_decimal_format;

typedef struct sxml_qt4_decimal_format_view {
    const sxml_qt4_decimal_format *decimal_formats;
    size_t decimal_format_count;
} sxml_qt4_decimal_format_view;

/* What the static base URI of the test is. */
typedef enum sxml_qt4_static_base_uri_kind {
    /* No <static-base-uri>: the URI of the test-set file. */
    SXML_QT4_STATIC_BASE_URI_TEST_SET_FILE,
    /* uri="#UNDEFINED": the static base URI is absent. */
    SXML_QT4_STATIC_BASE_URI_ABSENT,
    /* The URI given. */
    SXML_QT4_STATIC_BASE_URI_GIVEN,
} sxml_qt4_static_base_uri_kind;

typedef struct sxml_qt4_environment {
    sxml_qt4_environment_origin origin;
    /* The name it was referred to by; empty for an inline or absent
       environment. */
    sxml_byte_view name;
    sxml_qt4_source_view sources;
    sxml_qt4_parameter_view parameters;
    sxml_qt4_namespace_declaration_view namespace_declarations;
    sxml_qt4_resource_view resources;
    sxml_qt4_collection_view collections;
    sxml_qt4_collation_view collations;
    sxml_qt4_schema_view schemas;
    /* <context-item select>: the context value as an expression. */
    bool has_context_item;
    sxml_byte_view context_item_select;
    sxml_qt4_static_base_uri_kind static_base_uri_kind;
    /* For SXML_QT4_STATIC_BASE_URI_GIVEN. */
    sxml_byte_view static_base_uri;
    sxml_qt4_decimal_format_view decimal_formats;
    /* <function-library> and <sandpit>: XSLT/XQuery function libraries and
       writable directories (EXPath File). */
    bool has_function_library;
    bool has_sandpit;
} sxml_qt4_environment;

/* The environment of a test case with no environment element. */
sxml_qt4_environment sxml_qt4_environment_absent(void);

/* An environment element to read. */
typedef struct sxml_qt4_environment_request {
    /* The document holding the element; borrowed, and the views of the
       result point into it. */
    const sxml_qt4_document *document;
    sxml_node_index element;
    /* The suite path of that document, which file attributes are relative
       to. */
    sxml_byte_view containing_file;
    sxml_qt4_environment_origin origin;
    sxml_byte_view name;
    /* Receives the arrays and resolved paths. */
    sxml_arena *arena;
} sxml_qt4_environment_request;

/*
 * Reads the environment element request names into *environment.
 * Elements the catalog schema allows that the driver does not need
 * (description, created, modified) are skipped. Returns
 * SXML_STATUS_MALFORMED_INPUT for a decimal-format name whose prefix is
 * not bound, and the document's and the arena's failures, leaving
 * *environment untouched.
 */
SXML_NODISCARD sxml_status
sxml_qt4_environment_read(const sxml_qt4_environment_request *request,
                          sxml_qt4_environment *environment);
