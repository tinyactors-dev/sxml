#pragma once

/*
 * The system properties (glossary system_property): one table for the
 * capabilities and facts both fn:system-properties (F&O 4.0, 17.1.9; a
 * map keyed by no-namespace names, typed values) and the XSLT function
 * fn:system-property (XSLT 4.0, 24.2.1; names in the XSLT namespace,
 * string values) report, so the two never disagree (D163). Internal to
 * sxml; defined in src/system_property.c.
 */
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"

#include <stddef.h>

/* What a property's value is: given, or computed by the caller. */
typedef enum sxml_system_property_value_kind {
    SXML_SYSTEM_PROPERTY_VALUE_GIVEN,
    /* sxml_version_text(), an xs:string. */
    SXML_SYSTEM_PROPERTY_VALUE_PRODUCT_VERSION,
    /* Whether known_function is in the function table where the property
       is asked, an xs:boolean: fn:load-xquery-module#1 for
       supports-dynamic-xquery, a function of an optional feature for
       supports-binary-library (F&O 4.0, 17.1.9; D198). */
    SXML_SYSTEM_PROPERTY_VALUE_KNOWN_FUNCTION,
} sxml_system_property_value_kind;

/* A function to look for, by name and arity. */
typedef struct sxml_system_property_function {
    sxml_well_known_namespace function_namespace;
    sxml_byte_view local_name;
    size_t arity;
} sxml_system_property_function;

typedef struct sxml_system_property {
    /* The no-namespace name of fn:system-properties; empty when F&O has
       none. */
    sxml_byte_view function_name;
    /* The local name in the XSLT namespace of fn:system-property; empty
       when XSLT has none. */
    sxml_byte_view xslt_name;
    /* The value for GIVEN, of value_type. */
    sxml_atomic_datum value;
    sxml_system_property_value_kind value_kind;
    sxml_builtin_type value_type;
    /* For KNOWN_FUNCTION. */
    sxml_system_property_function known_function;
} sxml_system_property;

typedef struct sxml_system_property_view {
    const sxml_system_property *properties;
    size_t property_count;
} sxml_system_property_view;

/* Every system property. */
sxml_system_property_view sxml_system_properties(void);
