#pragma once

/*
 * Internal parts of the type registry (include/sxml/types.h,
 * docs/DECISIONS.md D32 and D58): the static descriptions of the built-in
 * types, shared by src/types.c and by modules that need a built-in
 * description without a registry.
 */
#include "sxml/types.h"

/*
 * The static description of a built-in type, for builtin_type from 1 up to
 * SXML_TYPE_BUILTIN_COUNT - 1. type_name is 0 in it: names are interned per
 * name pool, so the registry fills them in when it describes a type. NULL
 * for SXML_TYPE_NONE and values outside that range.
 */
const sxml_type_description *sxml_type_builtin_description(
    sxml_builtin_type builtin_type);

/*
 * The local name of a built-in schema type in the XML Schema namespace
 * ("integer" for SXML_TYPE_XS_INTEGER); empty for the generic item types,
 * which have no name, and for values outside the built-in range.
 */
sxml_byte_view sxml_type_builtin_local_name(sxml_builtin_type builtin_type);

/*
 * Registers the schema type description describes and writes its new
 * identifier to *type (D165). Only schema compilation (src/schema*.c)
 * calls it, after checking XSD 1.1's constraints on the definition:
 * applicable facets and valid facet values (Part 2, 4.3), derivation by
 * restriction, list or union from a base that is not final for it, no
 * circular definitions (Part 1, 3.4.6 and 3.16.6). The registry checks
 * only what it relies on:
 * - variety is SXML_TYPE_VARIETY_COMPLEX, ATOMIC, LIST or UNION;
 * - base_type, list_item_type, member_types and simple_content_type are
 *   identifiers the registry issued (so base types are registered first;
 *   schema compilation registers in derivation order);
 * - an ATOMIC type's base is atomic; its primitive_type is computed from
 *   the base (the description's is ignored);
 * - facets only on ATOMIC, LIST and UNION types.
 * type_name is kept for describing but not entered into the name index
 * (sxml_type_registry_find_type does not find it); is_builtin is ignored.
 * Views (member types, facets and their values) are copied. Schema types
 * are never interned: every call gives a new identifier.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for a description that breaks the
 * list above, SXML_STATUS_CAPACITY_EXCEEDED for a registry without an
 * allocator, SXML_STATUS_LIMIT_EXCEEDED at maximum_registered_type_count,
 * and the failures of the allocator; *type is then untouched.
 */
SXML_NODISCARD sxml_status sxml_type_registry_register_schema_type(
    sxml_type_registry *registry, const sxml_type_description *description,
    sxml_type_identifier *type);
