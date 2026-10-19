#pragma once

/*
 * The DTD of a document as XSD 1.1 schema documents (packet 7.14b,
 * docs/DECISIONS.md D199), written by src/dtd.c from the parser's DTD
 * tables (src/xml_internal.h) for the DTD validator of include/sxml/xml.h:
 * DTD validation is schema validation against components built from the
 * DTD, with no second content-model engine (D183).
 */
#include "sxml/status.h"
#include "sxml/view.h"
#include "xml_internal.h"

#include <stddef.h>

/* A namespace prefix and the namespace it is bound to, by which the names
   of the DTD, written as qualified names, are resolved. */
typedef struct sxml_dtd_prefix_binding {
    /* Empty for the default namespace. */
    sxml_byte_view namespace_prefix;
    sxml_byte_view namespace_uri;
} sxml_dtd_prefix_binding;

typedef struct sxml_dtd_binding_view {
    const sxml_dtd_prefix_binding *bindings;
    size_t binding_count;
} sxml_dtd_binding_view;

/* The schema documents to write. */
typedef struct sxml_dtd_schema_request {
    const sxml_xml_dtd *dtd;
    /* The first binding of a prefix wins. */
    sxml_dtd_binding_view bindings;
    /* The documents' texts are appended here. */
    sxml_xml_buffer *output;
    /* Receives one sxml_byte_range per document, its text in output; the
       first document is the one of no namespace, which holds the
       notations. */
    sxml_xml_array *documents;
} sxml_dtd_schema_request;

/*
 * Writes the schema documents of request->dtd: one per namespace that a
 * declared element type or a prefixed attribute name resolves to, and one
 * for no namespace. A name whose prefix has no binding is left out, so an
 * element or attribute of that name is not declared. Returns the
 * allocator's failures and SXML_STATUS_ARITHMETIC_OVERFLOW.
 */
SXML_NODISCARD sxml_status sxml_dtd_write_schemas(const sxml_dtd_schema_request *request);
