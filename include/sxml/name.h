#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * The name pool: interned namespace URIs, prefixes and expanded names
 * (docs/DECISIONS.md, D31).
 *
 * Every name that sxml compares often (element and attribute names, type
 * names, QName items, function names) is interned once into a name pool and
 * then handled as a 32-bit identifier, so name comparison is integer
 * comparison and a node or QName item stores no text.
 *
 * - A namespace URI is interned into an sxml_namespace_uri_identifier;
 *   identifier 0 is "no namespace" (the absent namespace URI, which XDM 4.0
 *   section 5.6 distinguishes from no name at all).
 * - A prefix is interned into an sxml_namespace_prefix_identifier;
 *   identifier 0 is "no prefix". Prefixes are never part of a name's
 *   identity (XDM 4.0, section 5.6): they are kept beside names, for output.
 * - An expanded name (namespace URI plus local name) is interned into an
 *   sxml_name_identifier.
 *
 * Identifiers are dense small integers, valid only with the pool that issued
 * them. The namespaces in sxml_well_known_namespace have fixed identifiers
 * in every pool. Interned text is never removed: it lives until the pool is
 * finalized, so the views the pool hands out stay valid until then.
 *
 * One name pool serves one processor: its type registry, its item heap and
 * every tree they handle. Mixing identifiers of different pools is a contract
 * violation that sxml does not detect.
 *
 * Thread safety: a name pool, and everything that holds its identifiers, is
 * used by one thread at a time (docs/DECISIONS.md, D35).
 */

/* An interned namespace URI. index 0 is "no namespace". */
typedef struct sxml_namespace_uri_identifier {
    uint32_t index;
} sxml_namespace_uri_identifier;

/* An interned namespace prefix. index 0 is "no prefix". */
typedef struct sxml_namespace_prefix_identifier {
    uint32_t index;
} sxml_namespace_prefix_identifier;

/*
 * An interned expanded name. Two names are the same name exactly when their
 * identifiers from the same pool are equal. index 0 is never issued; it
 * means "no name" where a name is optional (an anonymous function item, an
 * anonymous type, the node name of a text node).
 */
typedef struct sxml_name_identifier {
    uint32_t index;
} sxml_name_identifier;

/*
 * Namespaces with a fixed namespace URI identifier in every pool. The value
 * of each constant is the index of its sxml_namespace_uri_identifier. The
 * list only grows; values never change.
 */
typedef enum sxml_well_known_namespace {
    /* No namespace. */
    SXML_NAMESPACE_NONE = 0,
    /* http://www.w3.org/XML/1998/namespace, bound to "xml". */
    SXML_NAMESPACE_XML = 1,
    /* http://www.w3.org/2000/xmlns/, the namespace of xmlns attributes. */
    SXML_NAMESPACE_XMLNS = 2,
    /* http://www.w3.org/2001/XMLSchema, "xs": built-in types. */
    SXML_NAMESPACE_XML_SCHEMA = 3,
    /* http://www.w3.org/2001/XMLSchema-instance, "xsi". */
    SXML_NAMESPACE_XML_SCHEMA_INSTANCE = 4,
    /* http://www.w3.org/2005/xpath-functions, "fn". */
    SXML_NAMESPACE_FUNCTIONS = 5,
    /* http://www.w3.org/2005/xpath-functions/map, "map". */
    SXML_NAMESPACE_MAP_FUNCTIONS = 6,
    /* http://www.w3.org/2005/xpath-functions/array, "array". */
    SXML_NAMESPACE_ARRAY_FUNCTIONS = 7,
    /* http://www.w3.org/2005/xpath-functions/math, "math". */
    SXML_NAMESPACE_MATH_FUNCTIONS = 8,
    /* http://www.w3.org/2005/xqt-errors, "err" (SXML_ERROR_NAMESPACE_URI). */
    SXML_NAMESPACE_ERRORS = 9,
    /* http://www.w3.org/2005/xquery-local-functions, "local". */
    SXML_NAMESPACE_LOCAL_FUNCTIONS = 10,
    /* http://www.w3.org/2010/xslt-xquery-serialization, "output". */
    SXML_NAMESPACE_SERIALIZATION = 11,
    /* http://www.w3.org/1999/XSL/Transform, "xsl". */
    SXML_NAMESPACE_XSLT = 12,
    /* http://expath.org/ns/binary, "bin": the EXPath Binary functions and
       their errors (EXPath Binary 4.0, section 1.5; D191). */
    SXML_NAMESPACE_EXPATH_BINARY = 13,
    /* http://expath.org/ns/file, "file": the EXPath File functions and
       their errors (EXPath File 4.0, section 1.7; D200). */
    SXML_NAMESPACE_EXPATH_FILE = 14,
    /* One more than the largest well-known namespace. */
    SXML_NAMESPACE_WELL_KNOWN_COUNT = 15,
} sxml_well_known_namespace;

/*
 * The identifier of a well-known namespace. A function so that the
 * conversion from the enumeration is written once.
 */
static inline sxml_namespace_uri_identifier sxml_well_known_namespace_identifier(
    sxml_well_known_namespace well_known_namespace) {
    return (sxml_namespace_uri_identifier){
        .index = (uint32_t)well_known_namespace};
}

/* An expanded name to intern: an interned namespace URI and a local name. */
typedef struct sxml_name_request {
    sxml_namespace_uri_identifier namespace_uri;
    /* The local name as UTF-8; must not be empty. The pool copies it. The
       pool does not check NCName syntax: callers that take names from
       untrusted input check it first. */
    sxml_byte_view local_name;
} sxml_name_request;

/* The parts of an interned name. local_name is owned by the pool. */
typedef struct sxml_name_parts {
    sxml_namespace_uri_identifier namespace_uri;
    sxml_byte_view local_name;
} sxml_name_parts;

typedef struct sxml_name_pool sxml_name_pool;

typedef struct sxml_name_pool_configuration {
    /* Provides memory for interned text and the lookup tables. Must not be
       the null allocator; must outlive the pool. */
    sxml_allocator allocator;
    /* Most bytes the pool may allocate in total, a bound on work for
       untrusted input; 0 means no bound. Reaching it gives
       SXML_STATUS_LIMIT_EXCEEDED. */
    size_t maximum_byte_count;
} sxml_name_pool_configuration;

/*
 * Writes the memory requirements of a name pool to *requirements. They do
 * not include the memory the pool allocates as it grows. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid configuration.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_name_pool_memory_requirements(
    const sxml_name_pool_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a name pool in memory and writes its address to *name_pool. The
 * well-known namespaces are present from the start; they need no
 * allocation. memory must stay valid, unmoved, until sxml_name_pool_finalize.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, or
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_name_pool_initialize(
    sxml_memory_view memory, const sxml_name_pool_configuration *configuration,
    sxml_name_pool **name_pool);

/*
 * Releases everything the pool allocated. Every view and identifier it
 * issued becomes invalid. Does nothing when name_pool is NULL.
 */
SXML_EXPORT void sxml_name_pool_finalize(sxml_name_pool *name_pool);

/*
 * Allocates a name pool through allocator and initializes it. allocator is
 * stored and must outlive the pool. Returns the failures of
 * sxml_name_pool_initialize and sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_name_pool_create(
    sxml_allocator allocator, const sxml_name_pool_configuration *configuration,
    sxml_name_pool **name_pool);

/*
 * Finalizes a name pool made by sxml_name_pool_create and releases its
 * memory. Does nothing when name_pool is NULL.
 */
SXML_EXPORT void sxml_name_pool_destroy(sxml_name_pool *name_pool);

/*
 * Interns namespace_uri and writes its identifier to *identifier. The same
 * text always gives the same identifier; the empty text gives
 * SXML_NAMESPACE_NONE. namespace_uri must be well-formed UTF-8 (the pool
 * does not check); it is copied.
 *
 * Returns SXML_STATUS_OUT_OF_MEMORY or SXML_STATUS_LIMIT_EXCEEDED when the
 * pool cannot grow. Average O(byte_count).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_name_pool_intern_namespace_uri(
    sxml_name_pool *name_pool, sxml_byte_view namespace_uri,
    sxml_namespace_uri_identifier *identifier);

/*
 * Interns a namespace prefix as for sxml_name_pool_intern_namespace_uri; the
 * empty text gives identifier 0, "no prefix".
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_name_pool_intern_namespace_prefix(
    sxml_name_pool *name_pool, sxml_byte_view namespace_prefix,
    sxml_namespace_prefix_identifier *identifier);

/*
 * Interns the expanded name request describes and writes its identifier to
 * *identifier. Returns SXML_STATUS_INVALID_ARGUMENT for an empty local name
 * or a namespace URI identifier the pool did not issue, and the failures of
 * sxml_name_pool_intern_namespace_uri.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_name_pool_intern_name(
    sxml_name_pool *name_pool, const sxml_name_request *request,
    sxml_name_identifier *identifier);

/*
 * Looks a name up without interning it: writes its identifier to
 * *identifier and returns true when the name was interned before; returns
 * false, leaving *identifier untouched, otherwise. A name test against a
 * name never interned can match no node, so lookups on untrusted input
 * need not grow the pool. Never allocates.
 */
SXML_EXPORT bool sxml_name_pool_find_name(const sxml_name_pool *name_pool,
                                          const sxml_name_request *request,
                                          sxml_name_identifier *identifier);

/*
 * Looks a namespace URI up without interning it, as sxml_name_pool_find_name
 * does for names: writes its identifier to *identifier and returns true when
 * it was interned before (the empty text gives SXML_NAMESPACE_NONE); returns
 * false, leaving *identifier untouched, otherwise. Together with
 * sxml_name_pool_find_name it lets a name from untrusted input be looked up
 * without growing the pool. Never allocates.
 */
SXML_EXPORT bool sxml_name_pool_find_namespace_uri(
    const sxml_name_pool *name_pool, sxml_byte_view namespace_uri,
    sxml_namespace_uri_identifier *identifier);

/*
 * Writes the text of a namespace URI identifier to *namespace_uri (empty for
 * SXML_NAMESPACE_NONE). The view is owned by the pool. Returns
 * SXML_STATUS_INDEX_OUT_OF_RANGE for an identifier the pool did not issue.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_name_pool_get_namespace_uri(
    const sxml_name_pool *name_pool, sxml_namespace_uri_identifier identifier,
    sxml_byte_view *namespace_uri);

/*
 * Writes the text of a prefix identifier to *namespace_prefix (empty for
 * identifier 0), as for sxml_name_pool_get_namespace_uri.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_name_pool_get_namespace_prefix(
    const sxml_name_pool *name_pool,
    sxml_namespace_prefix_identifier identifier,
    sxml_byte_view *namespace_prefix);

/*
 * Writes the parts of a name identifier to *parts. local_name is owned by
 * the pool. Returns SXML_STATUS_INDEX_OUT_OF_RANGE for identifier 0 or an
 * identifier the pool did not issue. O(1).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_name_pool_get_name_parts(
    const sxml_name_pool *name_pool, sxml_name_identifier identifier,
    sxml_name_parts *parts);
