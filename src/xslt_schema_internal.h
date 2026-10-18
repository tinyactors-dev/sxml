#pragma once

#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/xpath.h"

#include "xslt_ast_internal.h"

/*
 * The schema imports of a stylesheet (glossary schema_import; XSLT 4.0,
 * section 3.13; D179): its xsl:import-schema declarations compiled into
 * one schema that becomes, after the host's schemas, the in-scope schema
 * definitions of every module. Module xslt_schema, defined in
 * src/xslt_schema.c. Internal to sxml.
 *
 * The synthetic schema document of 3.13 is the list of schema documents
 * one sxml_schema_create compiles: an inline xs:schema element as a NODE
 * document, a schema-location as a RESOLVED document (resolved against
 * the declaration's static base URI, its namespace checked), a namespace
 * alone as a RESOLVED document asked for by namespace. Of two
 * declarations of one namespace only those of the highest import
 * precedence count; a namespace a host schema defines (or the XSD
 * namespace) is taken from there and not read again; a location or a
 * namespace the resolver does not deliver is no error by itself (3.13:
 * names it would have given are then missing). The processor keeps the
 * compiled schema (sxml_processor_keep_schema, as XQuery's import schema,
 * D174); a stylesheet with neither host schemas nor imports gets the
 * processor's schema of the built-in components, so that lax validation,
 * xsi:type and validation by a built-in type always have a schema.
 *
 * Errors: XTSE0215 for an inline schema with a schema-location, or with a
 * namespace other than its targetNamespace; XTSE0220 for a synthetic
 * schema document that breaks XSD 1.1 Part 1's constraints (its
 * constraint name and description in the error's description), at the
 * first declaration.
 *
 * Thread safety: the thread of the processor (D35).
 */

typedef struct sxml_xslt_schema_request {
    /* Sealed; its in-scope schemas are set. */
    sxml_xslt_ast *ast;
    /* Its resolver reads the schema documents; it keeps the schema. */
    sxml_processor *processor;
    /* The host's schemas (sxml_xslt_stylesheet_configuration's
       static_context.in_scope_schemas), in scope first; borrowed, they
       must outlive the stylesheet. */
    sxml_schema_view host_schemas;
    sxml_error_record *error_record;
} sxml_xslt_schema_request;

/*
 * Compiles request->ast's schema imports and sets its in-scope schemas
 * (sxml_xslt_ast_set_in_scope_schemas). Returns SXML_STATUS_STATIC_ERROR
 * with the error record filled (above), and the failures of the resolver,
 * the schema compilation and the allocator.
 */
SXML_NODISCARD sxml_status sxml_xslt_import_schemas(const sxml_xslt_schema_request *request);
