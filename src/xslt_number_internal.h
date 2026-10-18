#pragma once

#include "function_library_internal.h"

/*
 * The internal native row of xsl:number (XSLT 4.0, section 12;
 * docs/DECISIONS.md D119, D153; packet 4.11). Internal to sxml.
 *
 * Like the rows of xsl:for-each-group (src/xslt_group_internal.h) it is in
 * no function table: src/xslt_lower_number.c calls it by a STATIC_CALL
 * whose target is the row, with arguments of its parameter types. It
 * formats a place marker (glossary `place_marker`) by the number to string
 * conversion attributes (12.4) over the picture engine of
 * src/format_internal.h, the one fn:format-integer uses (D119):
 *
 *     format-place-marker($numbers as xs:integer*, $format as xs:string,
 *                         $grouping-separator as xs:string?,
 *                         $grouping-size as xs:string?,
 *                         $ordinal as xs:string?, $lang as xs:string?,
 *                         $letter-value as xs:string?) as xs:string
 *
 * The attributes absent are the empty sequence ($format is "1" then).
 * XTDE0030 for a grouping-size that is not an xs:integer, a lang that is
 * neither an xs:language nor "", and a letter-value other than
 * "alphabetic" and "traditional"; lang and letter-value change nothing
 * else (English only, D97). Defined in src/function_library_format.c.
 */
extern const sxml_native_function sxml_xslt_format_place_marker_function;

/*
 * The internal row of xsl:analyze-string (XSLT 4.0, 17.1), called the same
 * way:
 *
 *     analyze-segments($value as xs:string, $pattern as xs:string,
 *                      $flags as xs:string) as item()*
 *
 * the segments of $value in order, each matching one as the
 * fn:matching-segment-record fn:matching-segments gives (F&O 4.0, 6.3.5)
 * and each non-empty non-matching one as an xs:string, taken from the
 * search itself, so the segments cost time linear in $value. FORX0002 and
 * FORX0001 as fn:matching-segments. Defined in
 * src/function_library_regex.c.
 */
extern const sxml_native_function sxml_xslt_analyze_segments_function;
