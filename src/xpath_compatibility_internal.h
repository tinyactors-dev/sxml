#ifndef SXML_XPATH_COMPATIBILITY_INTERNAL_H
#define SXML_XPATH_COMPATIBILITY_INTERNAL_H

/*
 * The conversions of XPath 1.0 compatibility mode (XPath 4.0, section
 * 2.2.1; glossary `xpath_1_compatibility_mode`; packet 7.4, D197) as
 * internal native rows in no function table. The mode is a flag of the
 * static context (sxml_xpath_static_context): src/xpath_lower.c reads it
 * and calls these rows as ordinary STATIC_CALLs, so the evaluator does not
 * know the mode.
 *
 * - argument rows, for the arguments of static calls to built-in
 *   functions (3.4.1): the first item, then fn:string or fn:number for a
 *   parameter of type xs:string or xs:double; the optional forms keep the
 *   empty sequence, which is an instance of xs:string? and xs:double?;
 * - the arithmetic operand row (4.9): the empty sequence becomes NaN,
 *   the first atomized item is kept, and an xs:boolean, xs:string,
 *   xs:decimal, xs:float or xs:untypedAtomic becomes an xs:double by
 *   fn:number;
 * - the general comparison row (4.11.2, the rules for the mode): its
 *   arguments are the two operands, not atomized, and the operator as the
 *   string "eq", "ne", "lt", "le", "gt" or "ge".
 *
 * Golden file: src/xslt_simple_content.c (an internal row in no function
 * table).
 */
#include "function_library_internal.h"

extern const sxml_native_function sxml_xpath_compatible_first_item_function;
extern const sxml_native_function sxml_xpath_compatible_string_function;
extern const sxml_native_function sxml_xpath_compatible_optional_string_function;
extern const sxml_native_function sxml_xpath_compatible_double_function;
extern const sxml_native_function sxml_xpath_compatible_optional_double_function;
extern const sxml_native_function sxml_xpath_compatible_operand_function;
extern const sxml_native_function sxml_xpath_compatible_comparison_function;

#endif
