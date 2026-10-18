#pragma once

#include "function_library_internal.h"

/*
 * The internal native row of xsl:for-each-group (XSLT 4.0, section 14;
 * docs/DECISIONS.md D137, D152; packet 4.9). Internal to sxml.
 *
 * Like group by's rows (src/xquery_flwor_internal.h) it is in no function
 * table: src/xslt_lower_group.c calls it by a STATIC_CALL whose target is
 * the row. Lowering computes, for each item of the population in
 * population order, one grouping pair (glossary `grouping_pair`): the
 * array [item, value], where value is the item's grouping keys (atomized,
 * xs:untypedAtomic cast to xs:string; for group-by without composite, the
 * distinct ones) or, for a pattern, whether the item matches it. The row
 * returns the groups in order of first appearance (14.3), each a group
 * array (glossary `group_array`): [current group, current grouping key],
 * the key that of the group's initial item (empty for the patterns).
 *
 *     for-each-group($pairs as array(*)*, $method as xs:integer,
 *                    $collation as xs:string) as array(*)*
 *
 * $method is a sxml_xslt_grouping_method; $collation is the collation
 * URI ("" for the default collation) under which strings, xs:anyURI and
 * xs:untypedAtomic keys are compared. group-by is the XQuery group-by row
 * over one tuple [item, key] per key (composite keys: one member per key
 * item, padded with empty members to the longest), so grouping keys are
 * equal exactly when XQuery's are (D137): atomic-equal, strings under the
 * collation. group-adjacent compares each item's key with the previous
 * item's the same way; a key that is not one atomic item is XTTE1100
 * unless composite. The pattern methods start a group at every matching
 * item, or after it.
 */

/* How the row forms groups (14.1); values of its $method. */
typedef enum sxml_xslt_grouping_method {
    SXML_XSLT_GROUPING_METHOD_BY = 0,
    SXML_XSLT_GROUPING_METHOD_BY_COMPOSITE = 1,
    SXML_XSLT_GROUPING_METHOD_ADJACENT = 2,
    SXML_XSLT_GROUPING_METHOD_ADJACENT_COMPOSITE = 3,
    SXML_XSLT_GROUPING_METHOD_STARTING_WITH = 4,
    SXML_XSLT_GROUPING_METHOD_ENDING_WITH = 5,
} sxml_xslt_grouping_method;

extern const sxml_native_function sxml_xslt_for_each_group_function;
