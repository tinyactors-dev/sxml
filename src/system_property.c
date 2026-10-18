/*
 * The system properties (src/system_property_internal.h, D163): one table
 * for fn:system-properties (F&O 4.0, 17.1.9) and the XSLT fn:system-property
 * (XSLT 4.0, 24.2.1). sxml is not schema-aware, reads no typed data, has
 * neither XInclude, DTD validation, invisible XML, streaming, dynamic XSLT,
 * xsl:evaluate nor the EXPath modules, and no backwards-compatible
 * behaviour (D97, delayed by D162).
 *
 * Golden file: src/function_library_json.c (the table it held).
 */
#include "system_property_internal.h"

#define SXML_TEXT_PROPERTY(function_text, xslt_text, value_text)                         \
    {.function_name = SXML_BYTE_VIEW_INITIALIZER(function_text),                          \
     .xslt_name = SXML_BYTE_VIEW_INITIALIZER(xslt_text),                                  \
     .value_kind = SXML_SYSTEM_PROPERTY_VALUE_GIVEN,                                      \
     .value = {.datum_kind = SXML_DATUM_KIND_STRING,                                      \
               .datum_details = {.string = {.utf8 = SXML_BYTE_VIEW_INITIALIZER(value_text), \
                                            .codepoint_count = sizeof(value_text) - 1,    \
                                            .is_ascii = true}}},                          \
     .value_type = SXML_TYPE_XS_STRING}

#define SXML_DECIMAL_PROPERTY(function_text, xslt_text, value_text)                  \
    {.function_name = SXML_BYTE_VIEW_INITIALIZER(function_text),                      \
     .xslt_name = SXML_BYTE_VIEW_INITIALIZER(xslt_text),                              \
     .value_kind = SXML_SYSTEM_PROPERTY_VALUE_GIVEN,                                  \
     .value = {.datum_kind = SXML_DATUM_KIND_DECIMAL,                                 \
               .datum_details = {.decimal_text = SXML_BYTE_VIEW_INITIALIZER(value_text)}}, \
     .value_type = SXML_TYPE_XS_DECIMAL}

#define SXML_BOOLEAN_PROPERTY(function_text, xslt_text, truth)                           \
    {.function_name = SXML_BYTE_VIEW_INITIALIZER(function_text),                          \
     .xslt_name = SXML_BYTE_VIEW_INITIALIZER(xslt_text),                                  \
     .value_kind = SXML_SYSTEM_PROPERTY_VALUE_GIVEN,                                      \
     .value = {.datum_kind = SXML_DATUM_KIND_BOOLEAN, .datum_details = {.boolean = (truth)}}, \
     .value_type = SXML_TYPE_XS_BOOLEAN}

static const sxml_system_property sxml_system_property_table[] = {
    SXML_DECIMAL_PROPERTY("xpath-version", "xpath-version", "4.0"),
    SXML_DECIMAL_PROPERTY("xsd-version", "xsd-version", "1.1"),
    SXML_TEXT_PROPERTY("product-name", "product-name", "sxml"),
    {.function_name = SXML_BYTE_VIEW_INITIALIZER("product-version"),
     .xslt_name = SXML_BYTE_VIEW_INITIALIZER("product-version"),
     .value_kind = SXML_SYSTEM_PROPERTY_VALUE_PRODUCT_VERSION,
     .value = {.datum_kind = SXML_DATUM_KIND_STRING},
     .value_type = SXML_TYPE_XS_STRING},
    /* XSLT only (24.2.1). */
    SXML_DECIMAL_PROPERTY("", "version", "4.0"),
    SXML_TEXT_PROPERTY("", "vendor", "sxml"),
    SXML_TEXT_PROPERTY("", "vendor-url", ""),
    SXML_BOOLEAN_PROPERTY("schema-aware", "is-schema-aware", false),
    SXML_BOOLEAN_PROPERTY("", "supports-serialization", true),
    SXML_BOOLEAN_PROPERTY("", "supports-backwards-compatibility", false),
    SXML_BOOLEAN_PROPERTY("", "supports-namespace-axis", true),
    SXML_BOOLEAN_PROPERTY("", "supports-streaming", false),
    SXML_BOOLEAN_PROPERTY("", "supports-dynamic-evaluation", false),
    SXML_BOOLEAN_PROPERTY("", "supports-higher-order-functions", true),
    /* F&O only (17.1.9). */
    SXML_BOOLEAN_PROPERTY("accepts-typed-data", "", false),
    SXML_BOOLEAN_PROPERTY("supports-xinclude", "", false),
    SXML_BOOLEAN_PROPERTY("supports-dtd-validation", "", false),
    SXML_BOOLEAN_PROPERTY("supports-dtd-attribute-typing", "", false),
    SXML_BOOLEAN_PROPERTY("supports-invisible-xml", "", false),
    {.function_name = SXML_BYTE_VIEW_INITIALIZER("supports-dynamic-xquery"),
     .xslt_name = SXML_BYTE_VIEW_INITIALIZER(""),
     .value_kind = SXML_SYSTEM_PROPERTY_VALUE_DYNAMIC_XQUERY,
     .value = {.datum_kind = SXML_DATUM_KIND_BOOLEAN},
     .value_type = SXML_TYPE_XS_BOOLEAN},
    SXML_BOOLEAN_PROPERTY("supports-dynamic-xslt", "", false),
    SXML_BOOLEAN_PROPERTY("supports-binary-library", "", false),
    SXML_BOOLEAN_PROPERTY("supports-file-library", "", false),
};

sxml_system_property_view sxml_system_properties(void) {
    return (sxml_system_property_view){
        .properties = sxml_system_property_table,
        .property_count = sizeof(sxml_system_property_table) / sizeof(sxml_system_property_table[0])};
}
