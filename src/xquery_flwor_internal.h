#pragma once

#include <stddef.h>
#include <stdint.h>

#include "function_library_internal.h"
#include "ir_internal.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "xpath_lower_internal.h"

/*
 * Group by and window clauses (XQuery 4.0, sections 4.13.4 and 4.13.8;
 * docs/DECISIONS.md D55, D126; packet 3.8). Internal to sxml.
 *
 * The FLWOR lowering of src/xquery_lower.c (packet 3.7) calls these at
 * three points; src/xquery_lower_window.c lowers, src/xquery_flwor.c holds
 * the two internal native rows (in no function table) the lowered code
 * calls.
 *
 * group by is a tuple stream clause. Its grouping specifications are
 * bindings of the segment before it: one with a value is a let (4.13.8:
 * "group by $g := E" is "let $g := E group by $g"); after the last one,
 * sxml_xquery_bind_grouping_keys rebinds each grouping variable to its
 * grouping key, the value coerced to xs:anyAtomicType? (atomized, XPTY0004
 * for more than one item), so the tuple arrays hold the keys. The tuple
 * arrays of a group by pack every variable, constants too, since a
 * non-grouping variable becomes the concatenation of its values in the
 * group. sxml_xquery_lower_group_by then calls the group-by row over the
 * tuple arrays: it returns one tuple array per group, in the order of the
 * groups' first tuples, with the same layout: each key member the key of
 * the group's first tuple, every other member the concatenation of its
 * values (4.13.8).
 *
 * A window clause is a clause of a segment, like a for clause (4.13.4.3):
 * a MAP over the window arrays the window row returns for the binding
 * sequence, whose body binds the window variables from the window array
 * (glossary `window_array`) and lowers the rest of the segment. The start
 * and end conditions are condition closures (glossary
 * `condition_closure`): fn($s, $sp, $spr, $sn) and fn($s, $sp, $spr, $sn,
 * $e, $ep, $epr, $en) returning fn:boolean of the condition, each
 * parameter bound to its variable's name when the clause declares it.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* One variable of a tuple array's layout (glossary `tuple_layout`): its
   name and either its member number (a packed variable) or its atom (a
   constant or global, which is not packed). */
typedef struct sxml_xquery_tuple_member {
    sxml_name_identifier variable_name;
    /* From 1; 0 when the variable is not packed. */
    uint32_t member_number;
    /* The variable's atom when it is not packed. */
    sxml_ir_atom atom;
} sxml_xquery_tuple_member;

typedef struct sxml_xquery_tuple_layout {
    const sxml_xquery_tuple_member *members;
    size_t member_count;
} sxml_xquery_tuple_layout;

/* ---------------------------------------------------------------- group by */

/* The end of a group by clause's grouping specifications in a segment. */
typedef struct sxml_xquery_grouping_keys {
    /* The GROUP_BY_CLAUSE. */
    sxml_ast_node_index clause;
    /* The segment's block, where the coercions are bound. */
    sxml_xpath_lowering_block *block;
    /* The FLWOR's own variables are the scope entries from here on. */
    size_t scope_mark;
} sxml_xquery_grouping_keys;

/*
 * Rebinds each grouping variable to its grouping key (see above). A
 * grouping variable that is not a variable of the FLWOR is XQST0094
 * (4.13.8). Fails as lowering does.
 */
SXML_NODISCARD sxml_status sxml_xquery_bind_grouping_keys(
    sxml_xpath_lowerer *lowerer, sxml_xquery_grouping_keys request);

/* A group by over tuple arrays. */
typedef struct sxml_xquery_grouping {
    sxml_ast_node_index clause;
    /* The layout of the tuple arrays (every variable packed). */
    sxml_xquery_tuple_layout layout;
    /* Where the call is bound. */
    sxml_xpath_lowering_block *block;
} sxml_xquery_grouping;

/*
 * Binds the group-by call over *tuples (in: the pre-grouping tuple arrays,
 * out: the post-grouping ones, same layout) in request->block. A collation
 * sxml does not know is XQST0076; relative URIs are resolved against the
 * static base URI (4.13.8). Fails as lowering does.
 */
SXML_NODISCARD sxml_status sxml_xquery_lower_group_by(
    sxml_xpath_lowerer *lowerer, const sxml_xquery_grouping *request,
    sxml_ir_atom *tuples);

/* ------------------------------------------------------------- Windows */

/* A clause of a segment and where the rest of the segment goes. */
typedef struct sxml_xquery_segment_rest {
    /* The FLWOR's frame. */
    sxml_xpath_lowering_frame *flwor_frame;
    /* A clause without bindings. */
    sxml_ast_node_index clause;
    sxml_xpath_lowering_block *block;
    sxml_ir_expression_index *rest;
} sxml_xquery_segment_rest;

/*
 * Defined in src/xquery_lower.c: pushes the frame that lowers the segment
 * after request->clause in request->block, its result to *request->rest.
 */
SXML_NODISCARD sxml_status sxml_xquery_push_segment_rest(
    sxml_xpath_lowering_machine *machine, const sxml_xquery_segment_rest *request);

/*
 * Pushes the frame of the window clause request->clause, lowered in
 * request->block: its MAP, with the rest of the segment inside, goes to
 * *request->rest. XQST0103 when two of its variables have one name
 * (4.13.4).
 */
SXML_NODISCARD sxml_status sxml_xquery_push_window_clause(
    sxml_xpath_lowering_machine *machine, const sxml_xquery_segment_rest *request);

/* --------------------------------------------- The native rows (xquery_flwor.c) */

/*
 * group-by($tuples as array(*)*, $keys as xs:integer*, $collations as
 * xs:string*) as array(*)*: $keys are member numbers (from 1), each with
 * the collation URI at the same place in $collations ("" for the default
 * collation). Key members are empty or one atomic item; two keys are
 * equal when both are empty or they are atomic-equal, strings, xs:anyURI
 * and xs:untypedAtomic compared under the collation (4.13.8, fn:deep-equal
 * on single atomic items).
 */
extern const sxml_native_function sxml_xquery_group_by_function;

/* Bits of the window row's $kind. */
typedef enum sxml_xquery_window_kind {
    /* Sliding (absent: tumbling). */
    SXML_XQUERY_WINDOW_SLIDING = 1U << 0U,
    /* The clause has an end condition. */
    SXML_XQUERY_WINDOW_HAS_END = 1U << 1U,
    /* "only end". */
    SXML_XQUERY_WINDOW_ONLY_END = 1U << 2U,
} sxml_xquery_window_kind;

/* Members of a window array. */
typedef enum sxml_xquery_window_member {
    SXML_XQUERY_WINDOW_MEMBER_WINDOW = 1,
    SXML_XQUERY_WINDOW_MEMBER_START = 2,
    SXML_XQUERY_WINDOW_MEMBER_START_POSITION = 3,
    SXML_XQUERY_WINDOW_MEMBER_START_PREVIOUS = 4,
    SXML_XQUERY_WINDOW_MEMBER_START_NEXT = 5,
    SXML_XQUERY_WINDOW_MEMBER_END = 6,
    SXML_XQUERY_WINDOW_MEMBER_END_POSITION = 7,
    SXML_XQUERY_WINDOW_MEMBER_END_PREVIOUS = 8,
    SXML_XQUERY_WINDOW_MEMBER_END_NEXT = 9,
} sxml_xquery_window_member;

/* Parameters of the start and end condition closures. */
#define SXML_XQUERY_WINDOW_START_PARAMETER_COUNT 4U
#define SXML_XQUERY_WINDOW_END_PARAMETER_COUNT 8U

/*
 * window($input as item()*, $start as fn(*)?, $end as fn(*)?, $kind as
 * xs:integer) as array(*)*: the window arrays of $input in the order of
 * their start items (4.13.4.1, 4.13.4.2). An absent $start or $end is a
 * condition that is always true; $kind holds sxml_xquery_window_kind bits.
 * Each condition's value is an xs:boolean.
 */
extern const sxml_native_function sxml_xquery_window_function;
