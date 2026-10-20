/*
 * fn:load-xquery-module (F&O 4.0, 18.2): one library part, `xquery_module`,
 * serving every language (D101), so XPath, XQuery and XSLT find the same
 * row (D272: every build has every language; XSLT 4.0, 5.3.1 puts the
 * F&O functions in a stylesheet's static context). The body, which
 * compiles the module with the XQuery front end and evaluates it as a unit
 * evaluation, is sxml_xquery_load_module_body in src/xquery_load_module.c.
 *
 * Golden file: src/function_library_exslt.c (the rows of a small part).
 */
#include "function_library_core_internal.h"
#include "function_library_internal.h"
#include "function_library_parts_internal.h"
#include "xquery_load_module_internal.h"

static const sxml_function_parameter load_parameters[] = {
    SXML_REQUIRED_PARAMETER("module-uri", "xs:string"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?"),
};

static const sxml_native_function xquery_module_functions[] = {
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT |
                   SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC |
                   SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("load-xquery-module"),
     .parameters = {load_parameters, sizeof(load_parameters) / sizeof(load_parameters[0])},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("fn:load-xquery-module-record"),
     .call = NULL,
     .resumable_body = &sxml_xquery_load_module_body},
};

const sxml_native_function_view sxml_function_library_xquery_module =
    SXML_LIBRARY_PART(xquery_module_functions);
