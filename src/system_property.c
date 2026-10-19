/*
 * The system properties (src/system_property_internal.h, D163): one table
 * for fn:system-properties (F&O 4.0, 17.1.9) and the XSLT fn:system-property
 * (XSLT 4.0, 24.2.1). sxml's XPath and XQuery are schema-aware and read
 * typed data (D174), XSLT not yet; sxml has no XInclude (D97, delayed by
 * D162) and streams XSLT's streamable constructs (D219). Dynamic XSLT, the EXPath modules, DTD
 * validation and invisible XML are optional features (D189, D211, D215). XSLT has
 * backwards-compatible behaviour (packet 7.4).
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

/* An F&O property that is true when a function is known where it is
   asked. */
#define SXML_KNOWN_FUNCTION_PROPERTY(function_text, namespace_value, local_text, arity_value) \
    {.function_name = SXML_BYTE_VIEW_INITIALIZER(function_text),                          \
     .xslt_name = SXML_BYTE_VIEW_INITIALIZER(""),                                         \
     .value_kind = SXML_SYSTEM_PROPERTY_VALUE_KNOWN_FUNCTION,                             \
     .value = {.datum_kind = SXML_DATUM_KIND_BOOLEAN},                                    \
     .value_type = SXML_TYPE_XS_BOOLEAN,                                                  \
     .known_function = {.function_namespace = (namespace_value),                          \
                        .local_name = SXML_BYTE_VIEW_INITIALIZER(local_text),             \
                        .arity = (arity_value)}}

/* An F&O property that is true when the processor where it is asked names
   an optional feature. */
#define SXML_OPTIONAL_FEATURE_PROPERTY(function_text, feature_kind_value)                \
    {.function_name = SXML_BYTE_VIEW_INITIALIZER(function_text),                          \
     .xslt_name = SXML_BYTE_VIEW_INITIALIZER(""),                                         \
     .value_kind = SXML_SYSTEM_PROPERTY_VALUE_OPTIONAL_FEATURE,                           \
     .value = {.datum_kind = SXML_DATUM_KIND_BOOLEAN},                                    \
     .value_type = SXML_TYPE_XS_BOOLEAN,                                                  \
     .optional_feature_kind = (feature_kind_value)}

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
    /* XQuery, XPath (D174) and XSLT (D179) are schema-aware. */
    SXML_BOOLEAN_PROPERTY("schema-aware", "", true),
    SXML_BOOLEAN_PROPERTY("", "is-schema-aware", true),
    SXML_BOOLEAN_PROPERTY("", "supports-serialization", true),
    /* XSLT 1.0 behaviour (3.8.1; packet 7.4, D197). */
    SXML_BOOLEAN_PROPERTY("", "supports-backwards-compatibility", true),
    SXML_BOOLEAN_PROPERTY("", "supports-namespace-axis", true),
    /* Streaming (18, 19; packets 7.6a and 7.6b, D212, D219). */
    SXML_BOOLEAN_PROPERTY("", "supports-streaming", true),
    SXML_BOOLEAN_PROPERTY("", "supports-dynamic-evaluation", true),
    SXML_BOOLEAN_PROPERTY("", "supports-higher-order-functions", true),
    /* F&O only (17.1.9). */
    SXML_BOOLEAN_PROPERTY("accepts-typed-data", "", true),
    SXML_BOOLEAN_PROPERTY("supports-xinclude", "", false),
    /* fn:doc and fn:parse-xml validate with dtd-validation true, and the
       tree has is-id and is-idrefs from the DTD (packets 7.14b, 7.14c;
       D199, D210), when the processor names the optional feature
       sxml_dtd_validation_feature (D211). */
    SXML_OPTIONAL_FEATURE_PROPERTY("supports-dtd-validation",
                                   SXML_OPTIONAL_FEATURE_KIND_DTD_VALIDATION),
    SXML_OPTIONAL_FEATURE_PROPERTY("supports-dtd-attribute-typing",
                                   SXML_OPTIONAL_FEATURE_KIND_DTD_VALIDATION),
    /* fn:invisible-xml, when the processor names the optional feature
       sxml_invisible_xml_feature (packet 7.18, D215). */
    SXML_OPTIONAL_FEATURE_PROPERTY("supports-invisible-xml",
                                   SXML_OPTIONAL_FEATURE_KIND_INVISIBLE_XML),
    SXML_KNOWN_FUNCTION_PROPERTY("supports-dynamic-xquery", SXML_NAMESPACE_FUNCTIONS,
                                 "load-xquery-module", 1),
    /* The optional feature sxml_xslt_transform_feature (D203). */
    SXML_KNOWN_FUNCTION_PROPERTY("supports-dynamic-xslt", SXML_NAMESPACE_FUNCTIONS, "transform",
                                 1),
    /* The optional feature sxml_expath_binary_feature (D198). */
    SXML_KNOWN_FUNCTION_PROPERTY("supports-binary-library", SXML_NAMESPACE_EXPATH_BINARY, "hex",
                                 1),
    /* The optional feature sxml_expath_file_feature (D200). */
    SXML_KNOWN_FUNCTION_PROPERTY("supports-file-library", SXML_NAMESPACE_EXPATH_FILE, "exists",
                                 1),
};

sxml_system_property_view sxml_system_properties(void) {
    return (sxml_system_property_view){
        .properties = sxml_system_property_table,
        .property_count = sizeof(sxml_system_property_table) / sizeof(sxml_system_property_table[0])};
}
