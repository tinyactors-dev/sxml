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
