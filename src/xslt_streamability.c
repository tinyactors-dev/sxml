/*
 * Streamability analysis of XSLT 4.0 (packet 7.6a; see
 * src/xslt_streamability_internal.h). Section numbers are of XSLT 4.0's
 * chapter 19 "Streamability" as XSLT 3.0 numbers it (19.1 to 19.10), and
 * of XSLT 4.0 elsewhere.
 *
 * Golden file: src/xslt_accumulator.c (a pass over the stylesheet AST with
 * its own tables), and the frame machine of src/xpath_lower.c.
 */
#include "xslt_streamability_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "memory_internal.h"
#include "sxml/name.h"
#include "xpath_ast_internal.h"
#include "xslt_ast_internal.h"

/* ------------------------------------------------------------ Lattices */

/* 19.4: the posture of a construct, in the order the combined posture of
   a choice operand group needs. */
typedef enum posture {
    SXML_STREAM_POSTURE_GROUNDED,
    SXML_STREAM_POSTURE_CLIMBING,
    SXML_STREAM_POSTURE_STRIDING,
    SXML_STREAM_POSTURE_CRAWLING,
    SXML_STREAM_POSTURE_ROAMING,
} posture;

/* 19.6: the sweep, ordered from narrow to wide. */
typedef enum sweep {
    SXML_STREAM_SWEEP_MOTIONLESS,
    SXML_STREAM_SWEEP_CONSUMING,
    SXML_STREAM_SWEEP_FREE_RANGING,
} sweep;

/* 19.3: the operand usage of an operand role. NONE marks "no operand". */
typedef enum usage {
    SXML_STREAM_USAGE_NONE,
    SXML_STREAM_USAGE_ABSORPTION,
    SXML_STREAM_USAGE_INSPECTION,
    SXML_STREAM_USAGE_TRANSMISSION,
    SXML_STREAM_USAGE_NAVIGATION,
} usage;

/*
 * 19.1: a U-type, a set of fundamental item types, as bits. The atomic
 * types the analysis never tells apart share SXML_U_OTHER_ATOMIC: only node
 * kinds, the numeric types (positional predicates), xs:boolean and
 * xs:string (non-numeric predicates) and function items matter.
 */
typedef uint32_t u_type;
enum {
    SXML_U_DOCUMENT = 1U << 0U,
    SXML_U_ELEMENT = 1U << 1U,
    SXML_U_ATTRIBUTE = 1U << 2U,
    SXML_U_TEXT = 1U << 3U,
    SXML_U_COMMENT = 1U << 4U,
    SXML_U_PROCESSING_INSTRUCTION = 1U << 5U,
    SXML_U_NAMESPACE = 1U << 6U,
    SXML_U_BOOLEAN = 1U << 7U,
    SXML_U_DECIMAL = 1U << 8U,
    SXML_U_DOUBLE = 1U << 9U,
    SXML_U_FLOAT = 1U << 10U,
    SXML_U_STRING = 1U << 11U,
    SXML_U_UNTYPED_ATOMIC = 1U << 12U,
    SXML_U_OTHER_ATOMIC = 1U << 13U,
    SXML_U_FUNCTION = 1U << 14U,
};
/* Nodes that can have children are SXML_U_PARENTS (19.8.1 rule 1(b)(iii)(A));
   what the child and descendant axes reach is SXML_U_CHILDREN (19.1.1). */
enum {
    SXML_U_NODES = SXML_U_DOCUMENT | SXML_U_ELEMENT | SXML_U_ATTRIBUTE | SXML_U_TEXT | SXML_U_COMMENT |
              SXML_U_PROCESSING_INSTRUCTION | SXML_U_NAMESPACE,
    SXML_U_NUMERIC = SXML_U_DECIMAL | SXML_U_DOUBLE | SXML_U_FLOAT,
    SXML_U_ATOMIC = SXML_U_BOOLEAN | SXML_U_DECIMAL | SXML_U_DOUBLE | SXML_U_FLOAT | SXML_U_STRING | SXML_U_UNTYPED_ATOMIC |
               SXML_U_OTHER_ATOMIC,
    SXML_U_ANY = SXML_U_NODES | SXML_U_ATOMIC | SXML_U_FUNCTION,
    SXML_U_PARENTS = SXML_U_DOCUMENT | SXML_U_ELEMENT,
    SXML_U_CHILDREN = SXML_U_ELEMENT | SXML_U_TEXT | SXML_U_COMMENT | SXML_U_PROCESSING_INSTRUCTION,
};

/* True when every item type of type is in supertype (19.1, "subtype"). */
static bool is_subtype(u_type type, u_type supertype) {
    return (type & ~supertype) == 0U;
}

/* The posture, sweep and static type of a construct. An uncertain one is
   one sxml has no rule for (see the header). */
typedef struct assessment {
    posture posture;
    sweep sweep;
    u_type type;
    bool is_uncertain;
    /* For a step of a path: it can be a step of a motionless pattern
       (19.8.8.8, scanning expressions). */
    bool is_pattern_step;
} assessment;

static assessment grounded_motionless(u_type type) {
    return (assessment){.posture = SXML_STREAM_POSTURE_GROUNDED,
                        .sweep = SXML_STREAM_SWEEP_MOTIONLESS,
                        .type = type,
                        .is_uncertain = false,
                        .is_pattern_step = false};
}

static assessment roaming_free_ranging(u_type type) {
    return (assessment){.posture = SXML_STREAM_POSTURE_ROAMING,
                        .sweep = SXML_STREAM_SWEEP_FREE_RANGING,
                        .type = type,
                        .is_uncertain = false,
                        .is_pattern_step = false};
}

static assessment make_assessment(posture value_posture, sweep value_sweep, u_type type) {
    return (assessment){.posture = value_posture,
                        .sweep = value_sweep,
                        .type = type,
                        .is_uncertain = false,
                        .is_pattern_step = false};
}

static sweep wider_sweep(sweep first, sweep second) {
    if (first == SXML_STREAM_SWEEP_FREE_RANGING || second == SXML_STREAM_SWEEP_FREE_RANGING) {
        return SXML_STREAM_SWEEP_FREE_RANGING;
    }
    if (first == SXML_STREAM_SWEEP_CONSUMING || second == SXML_STREAM_SWEEP_CONSUMING) {
        return SXML_STREAM_SWEEP_CONSUMING;
    }
    return SXML_STREAM_SWEEP_MOTIONLESS;
}

/* 19.6, note: roaming and free-ranging go together. */
static assessment normalized(assessment value) {
    if (value.posture == SXML_STREAM_POSTURE_ROAMING || value.sweep == SXML_STREAM_SWEEP_FREE_RANGING) {
        value.posture = SXML_STREAM_POSTURE_ROAMING;
        value.sweep = SXML_STREAM_SWEEP_FREE_RANGING;
    }
    return value;
}

static bool is_free(assessment value) {
    return value.posture == SXML_STREAM_POSTURE_ROAMING || value.sweep == SXML_STREAM_SWEEP_FREE_RANGING;
}

static bool is_motionless(assessment value) {
    return value.sweep == SXML_STREAM_SWEEP_MOTIONLESS && !is_free(value);
}

static bool is_grounded_motionless(assessment value) {
    return value.posture == SXML_STREAM_POSTURE_GROUNDED && is_motionless(value);
}

/* 19.8, "combined posture" of a choice operand group, folded one operand
   at a time from grounded (the posture of an empty group). */
static posture combined_posture(posture combined, posture operand) {
    if (combined == SXML_STREAM_POSTURE_ROAMING || operand == SXML_STREAM_POSTURE_ROAMING) {
        return SXML_STREAM_POSTURE_ROAMING;
    }
    if (combined == SXML_STREAM_POSTURE_GROUNDED) {
        return operand;
    }
    if (operand == SXML_STREAM_POSTURE_GROUNDED || operand == combined) {
        return combined;
    }
    /* One crawling, the other striding. */
    if ((combined == SXML_STREAM_POSTURE_CRAWLING && operand == SXML_STREAM_POSTURE_STRIDING) ||
        (combined == SXML_STREAM_POSTURE_STRIDING && operand == SXML_STREAM_POSTURE_CRAWLING)) {
        return SXML_STREAM_POSTURE_CRAWLING;
    }
    return SXML_STREAM_POSTURE_ROAMING;
}

/* 19.8.1 rule 1(b): the adjusted sweep S' of an operand. */
static sweep adjusted_sweep(assessment operand, usage operand_usage) {
    if (operand.sweep == SXML_STREAM_SWEEP_FREE_RANGING || operand.posture == SXML_STREAM_POSTURE_ROAMING) {
        return SXML_STREAM_SWEEP_FREE_RANGING;
    }
    if (operand.posture == SXML_STREAM_POSTURE_GROUNDED) {
        return operand.sweep;
    }
    usage adjusted_usage = operand_usage;
    if (operand_usage == SXML_STREAM_USAGE_ABSORPTION && (operand.type & SXML_U_PARENTS) == 0U) {
        adjusted_usage = SXML_STREAM_USAGE_INSPECTION;
    }
    switch (adjusted_usage) {
    case SXML_STREAM_USAGE_ABSORPTION:
        return operand.posture == SXML_STREAM_POSTURE_CLIMBING ? SXML_STREAM_SWEEP_FREE_RANGING : SXML_STREAM_SWEEP_CONSUMING;
    case SXML_STREAM_USAGE_INSPECTION:
    case SXML_STREAM_USAGE_TRANSMISSION:
        return operand.sweep;
    case SXML_STREAM_USAGE_NAVIGATION:
    case SXML_STREAM_USAGE_NONE:
        return SXML_STREAM_SWEEP_FREE_RANGING;
    }
    return SXML_STREAM_SWEEP_FREE_RANGING;
}

/* ------------------------------------------------- The general rules */

/* How the operands of a frame combine. */
typedef enum combination {
    /* 19.8.1. */
    SXML_STREAM_COMBINATION_GENERAL,
    /* xsl:map of map entries (19.8.4.23) and map constructors
       (19.8.8.17): roaming when one is, else grounded with the widest
       sweep. */
    SXML_STREAM_COMBINATION_MAP,
    /* xsl:fork (19.8.4.20): roaming when one is not grounded, else
       grounded with the widest sweep; with at most one consuming prong,
       the general rules, so that one prong may return streamed nodes
       (as the XT40 cases si-fork-006 and others expect). */
    SXML_STREAM_COMBINATION_FORK,
} combination;

/* The operands combined so far by the general rules (19.8.1). */
typedef struct general_state {
    uint32_t operand_count;
    bool has_free_ranging;
    bool is_uncertain;
    /* Potentially consuming operands (19.8.1). */
    uint32_t consuming_count;
    assessment first_consuming;
    sweep first_adjusted_sweep;
    usage first_usage;
    bool is_first_higher_order;
    /* All potentially consuming operands so far are of one choice operand
       group (choice_group, never 0); their combined posture and widest
       sweep. */
    bool is_one_group;
    uint32_t choice_group;
    posture group_posture;
    sweep group_sweep;
    /* All of them motionless with one posture (19.8.1 rule 2(c)(ii)). */
    bool is_motionless_alike;
    posture motionless_posture;
    /* The union of the static types of the transmission operands, and of
       all operands. */
    u_type transmitted_type;
    u_type operand_type;
    /* SXML_STREAM_COMBINATION_MAP and SXML_STREAM_COMBINATION_FORK. */
    sweep widest_sweep;
    bool has_roaming;
    bool has_ungrounded;
} general_state;

static general_state empty_general_state(void) {
    general_state state;
    memset(&state, 0, sizeof(state));
    state.is_one_group = true;
    state.is_motionless_alike = true;
    state.group_posture = SXML_STREAM_POSTURE_GROUNDED;
    return state;
}

typedef struct general_operand {
    assessment value;
    usage usage;
    bool is_higher_order;
    uint32_t choice_group;
} general_operand;

static void general_add(general_state *state, general_operand operand) {
    state->operand_count += 1U;
    state->is_uncertain = state->is_uncertain || operand.value.is_uncertain;
    state->operand_type |= operand.value.type;
    if (operand.usage == SXML_STREAM_USAGE_TRANSMISSION) {
        state->transmitted_type |= operand.value.type;
    }
    state->widest_sweep = wider_sweep(state->widest_sweep, operand.value.sweep);
    state->has_roaming = state->has_roaming || is_free(operand.value);
    state->has_ungrounded = state->has_ungrounded || operand.value.posture != SXML_STREAM_POSTURE_GROUNDED;
    const sweep adjusted = adjusted_sweep(operand.value, operand.usage);
    if (adjusted == SXML_STREAM_SWEEP_FREE_RANGING) {
        state->has_free_ranging = true;
        return;
    }
    const bool is_potentially_consuming =
        adjusted == SXML_STREAM_SWEEP_CONSUMING || (operand.usage == SXML_STREAM_USAGE_TRANSMISSION &&
                                        operand.value.posture != SXML_STREAM_POSTURE_GROUNDED);
    if (!is_potentially_consuming) {
        return;
    }
    if (state->consuming_count == 0U) {
        state->first_consuming = operand.value;
        state->first_adjusted_sweep = adjusted;
        state->first_usage = operand.usage;
        state->is_first_higher_order = operand.is_higher_order;
        state->choice_group = operand.choice_group;
        state->motionless_posture = operand.value.posture;
    }
    state->consuming_count += 1U;
    if (operand.choice_group == 0U || operand.choice_group != state->choice_group) {
        state->is_one_group = false;
    }
    state->group_posture = combined_posture(state->group_posture, operand.value.posture);
    state->group_sweep = wider_sweep(state->group_sweep, adjusted);
    if (adjusted != SXML_STREAM_SWEEP_MOTIONLESS || operand.value.posture != state->motionless_posture) {
        state->is_motionless_alike = false;
    }
}

/* 19.8.1 rule 2. is_at_most_one: a built-in function whose result has
   at most one item (rule 2(d)(iii)). */
typedef struct general_finish {
    combination combination;
    bool is_at_most_one;
    u_type type;
} general_finish;

static assessment general_result(const general_state *state, general_finish finish) {
    assessment result = grounded_motionless(finish.type);
    const bool is_fork_of_one = finish.combination == SXML_STREAM_COMBINATION_FORK && state->consuming_count <= 1U;
    if (!is_fork_of_one &&
        (finish.combination == SXML_STREAM_COMBINATION_MAP || finish.combination == SXML_STREAM_COMBINATION_FORK)) {
        const bool fails = finish.combination == SXML_STREAM_COMBINATION_MAP ? state->has_roaming
                                                                 : state->has_ungrounded ||
                                                                       state->has_roaming;
        result = fails ? roaming_free_ranging(finish.type)
                       : make_assessment(SXML_STREAM_POSTURE_GROUNDED, state->widest_sweep, finish.type);
    } else if (state->operand_count == 0U) {
        result = grounded_motionless(finish.type);
    } else if (state->has_free_ranging) {
        result = roaming_free_ranging(finish.type);
    } else if (state->consuming_count > 1U) {
        if (state->is_one_group) {
            result = make_assessment(state->group_posture, state->group_sweep, finish.type);
        } else if (state->is_motionless_alike) {
            result = make_assessment(state->motionless_posture, SXML_STREAM_SWEEP_MOTIONLESS, finish.type);
        } else {
            result = roaming_free_ranging(finish.type);
        }
    } else if (state->consuming_count == 1U) {
        if (state->is_first_higher_order) {
            result = roaming_free_ranging(finish.type);
        } else if (state->first_usage == SXML_STREAM_USAGE_ABSORPTION ||
                   state->first_usage == SXML_STREAM_USAGE_INSPECTION) {
            result = make_assessment(SXML_STREAM_POSTURE_GROUNDED, SXML_STREAM_SWEEP_CONSUMING, finish.type);
        } else if (state->first_consuming.posture == SXML_STREAM_POSTURE_CRAWLING && finish.is_at_most_one) {
            result = make_assessment(SXML_STREAM_POSTURE_STRIDING, state->first_adjusted_sweep, finish.type);
        } else {
            result = make_assessment(state->first_consuming.posture, state->first_adjusted_sweep,
                                     finish.type);
        }
    }
    result = normalized(result);
    result.is_uncertain = state->is_uncertain;
    return result;
}

/* 19.8 "type-adjusted posture and sweep" of value with respect to
   usage_of_type, the type-determined usage of the required type; a
   required type of nodes keeps the value as it is (transmission), so
   that a filter function's body can be striding (19.8.5.4). */
static assessment type_adjusted(assessment value, usage usage_of_type) {
    general_state state = empty_general_state();
    general_add(&state, (general_operand){.value = value,
                                          .usage = usage_of_type == SXML_STREAM_USAGE_NAVIGATION
                                                       ? SXML_STREAM_USAGE_TRANSMISSION
                                                       : usage_of_type,
                                          .is_higher_order = false,
                                          .choice_group = 0});
    return general_result(&state, (general_finish){.combination = SXML_STREAM_COMBINATION_GENERAL,
                                                   .is_at_most_one = false,
                                                   .type = value.type});
}

/* ------------------------------------------------------------- U-types */

/* 19.1: the U-types of item type kinds of an XPath AST; a name is an
   atomic type. 0 (every kind not listed, nested choices among them) is
   U{*}. */
static const u_type item_type_u_types[SXML_AST_KIND_COUNT] = {
    [SXML_AST_KIND_ANY_ITEM_TYPE] = SXML_U_ANY,
    [SXML_AST_KIND_GNODE_TYPE] = SXML_U_NODES,
    [SXML_AST_KIND_ANY_XNODE_TYPE] = SXML_U_NODES,
    [SXML_AST_KIND_DOCUMENT_NODE_TYPE] = SXML_U_DOCUMENT,
    [SXML_AST_KIND_ELEMENT_NODE_TYPE] = SXML_U_ELEMENT,
    [SXML_AST_KIND_SCHEMA_ELEMENT_NODE_TYPE] = SXML_U_ELEMENT,
    [SXML_AST_KIND_ATTRIBUTE_NODE_TYPE] = SXML_U_ATTRIBUTE,
    [SXML_AST_KIND_SCHEMA_ATTRIBUTE_NODE_TYPE] = SXML_U_ATTRIBUTE,
    [SXML_AST_KIND_PROCESSING_INSTRUCTION_NODE_TYPE] = SXML_U_PROCESSING_INSTRUCTION,
    [SXML_AST_KIND_COMMENT_NODE_TYPE] = SXML_U_COMMENT,
    [SXML_AST_KIND_TEXT_NODE_TYPE] = SXML_U_TEXT,
    [SXML_AST_KIND_NAMESPACE_NODE_TYPE] = SXML_U_NAMESPACE,
    [SXML_AST_KIND_ANY_MAP_TYPE] = SXML_U_FUNCTION,
    [SXML_AST_KIND_ANY_ARRAY_TYPE] = SXML_U_FUNCTION,
    [SXML_AST_KIND_ANY_RECORD_TYPE] = SXML_U_FUNCTION,
    [SXML_AST_KIND_TYPED_MAP_TYPE] = SXML_U_FUNCTION,
    [SXML_AST_KIND_TYPED_ARRAY_TYPE] = SXML_U_FUNCTION,
    [SXML_AST_KIND_TYPED_RECORD_TYPE] = SXML_U_FUNCTION,
    [SXML_AST_KIND_ANY_FUNCTION_TYPE] = SXML_U_FUNCTION,
    [SXML_AST_KIND_TYPED_FUNCTION_TYPE] = SXML_U_FUNCTION,
    [SXML_AST_KIND_ENUMERATION_TYPE] = SXML_U_STRING,
    [SXML_AST_KIND_NCNAME] = SXML_U_ATOMIC,
    [SXML_AST_KIND_PREFIXED_NAME] = SXML_U_ATOMIC,
    [SXML_AST_KIND_URI_QUALIFIED_NAME] = SXML_U_ATOMIC,
};

static u_type item_type_u_type(const sxml_ast *ast, sxml_ast_node_index item_type) {
    const sxml_ast_kind kind = sxml_ast_get_node(ast, item_type)->kind;
    const u_type type = (uint32_t)kind < (uint32_t)SXML_AST_KIND_COUNT ? item_type_u_types[kind] : 0U;
    return type == 0U ? SXML_U_ANY : type;
}

/* The U-type of a SEQUENCE_TYPE node (19.1): empty-sequence() is U{};
   otherwise the item type's, the occurrence ignored. A choice of item
   types is the union of its alternatives. */
static u_type sequence_type_u_type(const sxml_ast *ast, sxml_ast_node_index sequence_type) {
    const sxml_ast_node *node = sxml_ast_get_node(ast, sequence_type);
    if (node->kind != SXML_AST_KIND_SEQUENCE_TYPE) {
        return SXML_U_ANY;
    }
    if (node->detail.occurrence == SXML_OCCURRENCE_ZERO) {
        return 0U;
    }
    const sxml_ast_node_index item_type =
        sxml_ast_find_child(ast, (sxml_ast_child_query){.parent = sequence_type,
                                                        .field = SXML_AST_FIELD_ITEM_TYPE});
    if (sxml_ast_get_node(ast, item_type)->kind != SXML_AST_KIND_CHOICE_ITEM_TYPE) {
        return item_type_u_type(ast, item_type);
    }
    u_type type = 0U;
    for (sxml_ast_node_index alternative = sxml_ast_get_node(ast, item_type)->first_child;
         alternative.value != 0; alternative = sxml_ast_get_node(ast, alternative)->next_sibling) {
        type |= item_type_u_type(ast, alternative);
    }
    return type;
}

/* A declared type: its U-type, whether it allows more than one item, and
   whether one was declared. */
typedef struct declared_type {
    u_type type;
    bool allows_many;
    bool is_declared;
    /* document-node(element(...)) (19.8.8.5, 19.8.8.6). */
    bool is_document_with_element;
} declared_type;

static declared_type undeclared_type(void) {
    return (declared_type){
        .type = SXML_U_ANY, .allows_many = true, .is_declared = false, .is_document_with_element = false};
}

static declared_type sequence_type_of(const sxml_ast *ast, sxml_ast_node_index sequence_type) {
    const sxml_ast_node *node = sxml_ast_get_node(ast, sequence_type);
    if (node->kind != SXML_AST_KIND_SEQUENCE_TYPE) {
        return undeclared_type();
    }
    const sxml_ast_node_index item_type =
        sxml_ast_find_child(ast, (sxml_ast_child_query){.parent = sequence_type,
                                                        .field = SXML_AST_FIELD_ITEM_TYPE});
    const sxml_ast_node *item_node = sxml_ast_get_node(ast, item_type);
    bool is_document_with_element = false;
    if (item_node->kind == SXML_AST_KIND_DOCUMENT_NODE_TYPE) {
        const sxml_ast_kind content_kind =
            sxml_ast_get_node(ast, sxml_ast_find_child(
                                       ast, (sxml_ast_child_query){.parent = item_type,
                                                                   .field = SXML_AST_FIELD_CONTENT}))
                ->kind;
        is_document_with_element = content_kind != SXML_AST_KIND_NONE;
    }
    return (declared_type){.type = sequence_type_u_type(ast, sequence_type),
                           .allows_many = node->detail.occurrence == SXML_OCCURRENCE_ZERO_OR_MORE ||
                                          node->detail.occurrence == SXML_OCCURRENCE_ONE_OR_MORE,
                           .is_declared = true,
                           .is_document_with_element = is_document_with_element};
}

/* 19.8, "type-determined usage" of a required type. */
static usage type_determined_usage(declared_type type) {
    if (!type.is_declared) {
        return SXML_STREAM_USAGE_NAVIGATION;
    }
    if (is_subtype(type.type, SXML_U_FUNCTION)) {
        return SXML_STREAM_USAGE_INSPECTION;
    }
    if (is_subtype(type.type, SXML_U_ATOMIC)) {
        return SXML_STREAM_USAGE_ABSORPTION;
    }
    return SXML_STREAM_USAGE_NAVIGATION;
}

/* The more restrictive of two type-determined usages (19.8.4.9). */
static usage more_restrictive_usage(usage first, usage second) {
    if (first == SXML_STREAM_USAGE_ABSORPTION || second == SXML_STREAM_USAGE_ABSORPTION) {
        return SXML_STREAM_USAGE_ABSORPTION;
    }
    if (first == SXML_STREAM_USAGE_INSPECTION || second == SXML_STREAM_USAGE_INSPECTION) {
        return SXML_STREAM_USAGE_INSPECTION;
    }
    return SXML_STREAM_USAGE_NAVIGATION;
}

/* ---------------------------------------------------- AST conveniences */

static const sxml_ast_node_index no_ast_node = {.value = 0};
static const sxml_xslt_ast_node_index no_xslt_node = {.value = 0};

static sxml_ast_node_index child_by_field(const sxml_ast *ast, sxml_ast_node_index parent,
                                          sxml_ast_field field) {
    return sxml_ast_find_child(ast, (sxml_ast_child_query){.parent = parent, .field = field});
}

static const sxml_xslt_ast_attribute *find_xslt_attribute(const sxml_xslt_ast *ast,
                                                          sxml_xslt_ast_node_index node,
                                                          sxml_xslt_ast_attribute_kind kind) {
    const sxml_xslt_ast_attribute_index index =
        sxml_xslt_ast_find_attribute(ast, (sxml_xslt_ast_attribute_query){.node = node, .kind = kind});
    return index.value == 0 ? NULL : sxml_xslt_ast_get_attribute(ast, index);
}

static bool is_true_attribute(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node,
                              sxml_xslt_ast_attribute_kind kind) {
    const sxml_xslt_ast_attribute *attribute = find_xslt_attribute(ast, node, kind);
    return attribute != NULL && attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
           attribute->value.payload.is_true;
}

/* The expression of an EXPRESSION, PATTERN, VALUE_TEMPLATE or type
   attribute; NULL when absent or of another form. */
static const sxml_xslt_ast_expression *attribute_expression(const sxml_xslt_ast *ast,
                                                            sxml_xslt_ast_node_index node,
                                                            sxml_xslt_ast_attribute_kind kind) {
    const sxml_xslt_ast_attribute *attribute = find_xslt_attribute(ast, node, kind);
    if (attribute == NULL) {
        return NULL;
    }
    switch (attribute->value.kind) {
    case SXML_XSLT_AST_VALUE_KIND_EXPRESSION:
    case SXML_XSLT_AST_VALUE_KIND_PATTERN:
    case SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE:
    case SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE:
    case SXML_XSLT_AST_VALUE_KIND_ITEM_TYPE: {
        const sxml_xslt_ast_expression *expression =
            sxml_xslt_ast_get_expression(ast, attribute->value.payload.expression);
        return expression->ast == NULL ? NULL : expression;
    }
    case SXML_XSLT_AST_VALUE_KIND_NONE:
    case SXML_XSLT_AST_VALUE_KIND_TEXT:
    case SXML_XSLT_AST_VALUE_KIND_BOOLEAN:
    case SXML_XSLT_AST_VALUE_KIND_NUMBER:
    case SXML_XSLT_AST_VALUE_KIND_KEYWORD:
    case SXML_XSLT_AST_VALUE_KIND_NAME:
    case SXML_XSLT_AST_VALUE_KIND_NAME_LIST:
    case SXML_XSLT_AST_VALUE_KIND_NAMESPACE_BINDING:
        return NULL;
    }
    return NULL;
}

/* The declared type of node's as attribute. */
static declared_type declared_type_of_node(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_expression *type =
        attribute_expression(ast, node, SXML_XSLT_AST_ATTRIBUTE_AS);
    if (type == NULL) {
        return undeclared_type();
    }
    return sequence_type_of(type->ast, type->ast->root);
}

static sxml_xslt_ast_keyword keyword_attribute(const sxml_xslt_ast *ast,
                                               sxml_xslt_ast_node_index node,
                                               sxml_xslt_ast_attribute_kind kind) {
    const sxml_xslt_ast_attribute *attribute = find_xslt_attribute(ast, node, kind);
    if (attribute == NULL || attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_KEYWORD) {
        return SXML_XSLT_AST_KEYWORD_NONE;
    }
    return attribute->value.payload.keyword;
}

static sxml_name_identifier name_attribute(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *attribute =
        find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_NAME);
    if (attribute == NULL || attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME) {
        return (sxml_name_identifier){.index = 0};
    }
    return attribute->value.payload.name;
}

static bool is_kind(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node, sxml_xslt_ast_kind kind) {
    return sxml_xslt_ast_get_node(ast, node)->kind == kind;
}

static sxml_xslt_ast_node_index node_at(uint32_t value) {
    return (sxml_xslt_ast_node_index){.value = value};
}

/* The kinds that are instructions of a sequence constructor (5.7), with
   literal result elements, text nodes and extension instructions. */
static const bool instruction_kinds[SXML_XSLT_AST_KIND_COUNT] = {
    [SXML_XSLT_AST_KIND_LITERAL_RESULT_ELEMENT] = true,
    [SXML_XSLT_AST_KIND_TEXT_NODE] = true,
    [SXML_XSLT_AST_KIND_EXTENSION_INSTRUCTION] = true,
    [SXML_XSLT_AST_KIND_UNKNOWN_INSTRUCTION] = true,
    [SXML_XSLT_AST_KIND_VARIABLE] = true,
    [SXML_XSLT_AST_KIND_ANALYZE_STRING] = true,
    [SXML_XSLT_AST_KIND_APPLY_IMPORTS] = true,
    [SXML_XSLT_AST_KIND_APPLY_TEMPLATES] = true,
    [SXML_XSLT_AST_KIND_ARRAY] = true,
    [SXML_XSLT_AST_KIND_ARRAY_MEMBER] = true,
    [SXML_XSLT_AST_KIND_ASSERT] = true,
    [SXML_XSLT_AST_KIND_ATTRIBUTE] = true,
    [SXML_XSLT_AST_KIND_BREAK] = true,
    [SXML_XSLT_AST_KIND_CALL_TEMPLATE] = true,
    [SXML_XSLT_AST_KIND_CHOOSE] = true,
    [SXML_XSLT_AST_KIND_COMMENT] = true,
    [SXML_XSLT_AST_KIND_COPY] = true,
    [SXML_XSLT_AST_KIND_COPY_OF] = true,
    [SXML_XSLT_AST_KIND_DOCUMENT] = true,
    [SXML_XSLT_AST_KIND_ELEMENT] = true,
    [SXML_XSLT_AST_KIND_EVALUATE] = true,
    [SXML_XSLT_AST_KIND_FOR_EACH] = true,
    [SXML_XSLT_AST_KIND_FOR_EACH_GROUP] = true,
    [SXML_XSLT_AST_KIND_FORK] = true,
    [SXML_XSLT_AST_KIND_IF] = true,
    [SXML_XSLT_AST_KIND_ITERATE] = true,
    [SXML_XSLT_AST_KIND_MAP] = true,
    [SXML_XSLT_AST_KIND_MAP_ENTRY] = true,
    [SXML_XSLT_AST_KIND_MERGE] = true,
    [SXML_XSLT_AST_KIND_MESSAGE] = true,
    [SXML_XSLT_AST_KIND_NAMESPACE] = true,
    [SXML_XSLT_AST_KIND_NEXT_ITERATION] = true,
    [SXML_XSLT_AST_KIND_NEXT_MATCH] = true,
    [SXML_XSLT_AST_KIND_NUMBER] = true,
    [SXML_XSLT_AST_KIND_ON_EMPTY] = true,
    [SXML_XSLT_AST_KIND_ON_NON_EMPTY] = true,
    [SXML_XSLT_AST_KIND_PERFORM_SORT] = true,
    [SXML_XSLT_AST_KIND_PROCESSING_INSTRUCTION] = true,
    [SXML_XSLT_AST_KIND_RESULT_DOCUMENT] = true,
    [SXML_XSLT_AST_KIND_SELECT] = true,
    [SXML_XSLT_AST_KIND_SEQUENCE] = true,
    [SXML_XSLT_AST_KIND_SOURCE_DOCUMENT] = true,
    [SXML_XSLT_AST_KIND_SWITCH] = true,
    [SXML_XSLT_AST_KIND_TEXT] = true,
    [SXML_XSLT_AST_KIND_TRY] = true,
    [SXML_XSLT_AST_KIND_VALUE_OF] = true,
    [SXML_XSLT_AST_KIND_WHERE_POPULATED] = true,
    [SXML_XSLT_AST_KIND_RECORD] = true,
};

static bool is_instruction_kind(sxml_xslt_ast_kind kind) {
    return (uint32_t)kind < (uint32_t)SXML_XSLT_AST_KIND_COUNT && instruction_kinds[kind];
}

/* ------------------------------------------------------------- Names */

/* What a function call's or variable reference's name resolves to. */
typedef struct resolved_name {
    bool is_resolved;
    sxml_namespace_uri_identifier namespace_uri;
    sxml_byte_view local_name;
} resolved_name;

typedef struct name_lookup {
    const sxml_xslt_ast *ast;
    const sxml_name_pool *name_pool;
} name_lookup;

/* The namespace of prefix among the statically known namespaces of
   static_context (3.6.1, 5.1.1), else of the prefixes XPath 4.0
   predeclares (fn, xs, map, array, math). */
typedef struct prefix_query {
    sxml_xslt_ast_static_context_index static_context;
    sxml_byte_view prefix;
} prefix_query;

static bool resolve_prefix(const name_lookup *lookup, prefix_query query,
                           sxml_namespace_uri_identifier *namespace_uri) {
    const sxml_byte_view prefix = query.prefix;
    const sxml_namespace_binding_view bindings =
        sxml_xslt_ast_get_namespace_bindings(lookup->ast, query.static_context);
    for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding binding = bindings.namespace_bindings[binding_index];
        sxml_byte_view binding_prefix = {.bytes = NULL, .byte_count = 0};
        if (sxml_name_pool_get_namespace_prefix(lookup->name_pool, binding.namespace_prefix,
                                                &binding_prefix) == SXML_STATUS_OK &&
            sxml_byte_view_equals(binding_prefix, prefix)) {
            *namespace_uri = binding.namespace_uri;
            return true;
        }
    }
    static const struct {
        const char *prefix;
        sxml_well_known_namespace namespace_uri;
    } predeclared[] = {{"fn", SXML_NAMESPACE_FUNCTIONS},
                       {"xs", SXML_NAMESPACE_XML_SCHEMA},
                       {"map", SXML_NAMESPACE_MAP_FUNCTIONS},
                       {"array", SXML_NAMESPACE_ARRAY_FUNCTIONS},
                       {"math", SXML_NAMESPACE_MATH_FUNCTIONS}};
    for (size_t entry_index = 0; entry_index < sizeof predeclared / sizeof predeclared[0];
         entry_index += 1) {
        const sxml_byte_view text = {.bytes = (const uint8_t *)predeclared[entry_index].prefix,
                                     .byte_count = strlen(predeclared[entry_index].prefix)};
        if (sxml_byte_view_equals(text, prefix)) {
            *namespace_uri = sxml_well_known_namespace_identifier(predeclared[entry_index].namespace_uri);
            return true;
        }
    }
    return false;
}

/* The expanded name of a Name node of an XPath AST; an unprefixed name
   takes default_namespace (the default function namespace, fn, or no
   namespace for a variable). */
typedef struct name_to_resolve {
    const sxml_ast *ast;
    sxml_ast_node_index name;
    sxml_xslt_ast_static_context_index static_context;
    sxml_well_known_namespace default_namespace;
} name_to_resolve;

static resolved_name resolve_name(const name_lookup *lookup, name_to_resolve request) {
    resolved_name result = {.is_resolved = false,
                            .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_NONE),
                            .local_name = {.bytes = NULL, .byte_count = 0}};
    const sxml_ast_node *node = sxml_ast_get_node(request.ast, request.name);
    if (node->kind != SXML_AST_KIND_NCNAME && node->kind != SXML_AST_KIND_PREFIXED_NAME &&
        node->kind != SXML_AST_KIND_URI_QUALIFIED_NAME) {
        return result;
    }
    result.local_name = sxml_ast_get_text(request.ast, node->payload.name.local_name);
    if (node->kind == SXML_AST_KIND_URI_QUALIFIED_NAME) {
        result.is_resolved = sxml_name_pool_find_namespace_uri(
            lookup->name_pool, sxml_ast_get_text(request.ast, node->payload.name.namespace_uri),
            &result.namespace_uri);
        return result;
    }
    if (node->kind == SXML_AST_KIND_NCNAME) {
        result.namespace_uri = sxml_well_known_namespace_identifier(request.default_namespace);
        result.is_resolved = true;
        return result;
    }
    result.is_resolved = resolve_prefix(
        lookup,
        (prefix_query){.static_context = request.static_context,
                       .prefix = sxml_ast_get_text(request.ast, node->payload.name.namespace_prefix)},
        &result.namespace_uri);
    return result;
}

/* True when name is the interned name identifier. */
static bool is_same_name(const name_lookup *lookup, resolved_name name,
                         sxml_name_identifier identifier) {
    sxml_name_parts parts;
    memset(&parts, 0, sizeof(parts));
    return name.is_resolved && identifier.index != 0 &&
           sxml_name_pool_get_name_parts(lookup->name_pool, identifier, &parts) == SXML_STATUS_OK &&
           parts.namespace_uri.index == name.namespace_uri.index &&
           sxml_byte_view_equals(parts.local_name, name.local_name);
}

/* ------------------------------------------------- Stylesheet lookups */

/* The first declaration of kind that names name; 0 for none. */
typedef struct declaration_query {
    sxml_xslt_ast_kind kind;
    sxml_name_identifier name;
} declaration_query;

static sxml_xslt_ast_node_index find_named_declaration(const sxml_xslt_ast *ast,
                                                       declaration_query query) {
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (sxml_xslt_ast_get_node(ast, declaration)->kind == query.kind &&
            name_attribute(ast, declaration).index == query.name.index) {
            return declaration;
        }
    }
    return no_xslt_node;
}

/* The parameters of a stylesheet function or template (its first
   children), counted. */
static uint32_t parameter_count(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node) {
    uint32_t count = 0;
    for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, node)->first_child;
         child.value != 0; child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        if (sxml_xslt_ast_get_node(ast, child)->kind == SXML_XSLT_AST_KIND_PARAM) {
            count += 1U;
        }
    }
    return count;
}

/* The position-th parameter (from 0) of node; 0 for none. */
typedef struct parameter_query {
    sxml_xslt_ast_node_index node;
    uint32_t position;
} parameter_query;

static sxml_xslt_ast_node_index parameter_at(const sxml_xslt_ast *ast, parameter_query query) {
    uint32_t position = 0;
    for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, query.node)->first_child;
         child.value != 0; child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        if (sxml_xslt_ast_get_node(ast, child)->kind == SXML_XSLT_AST_KIND_PARAM) {
            if (position == query.position) {
                return child;
            }
            position += 1U;
        }
    }
    return no_xslt_node;
}

/* The parameter of node named name; 0 for none. */
typedef struct named_parameter_query {
    sxml_xslt_ast_node_index node;
    sxml_name_identifier name;
} named_parameter_query;

static sxml_xslt_ast_node_index parameter_named(const sxml_xslt_ast *ast,
                                                named_parameter_query query) {
    for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, query.node)->first_child;
         child.value != 0; child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        if (sxml_xslt_ast_get_node(ast, child)->kind == SXML_XSLT_AST_KIND_PARAM &&
            name_attribute(ast, child).index == query.name.index) {
            return child;
        }
    }
    return no_xslt_node;
}

/* A stylesheet function by name and arity (10.3); 0 for none. */
typedef struct function_query {
    resolved_name name;
    uint32_t arity;
} function_query;

static sxml_xslt_ast_node_index find_function(const name_lookup *lookup, function_query query) {
    const sxml_xslt_ast *ast = lookup->ast;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (sxml_xslt_ast_get_node(ast, declaration)->kind == SXML_XSLT_AST_KIND_FUNCTION &&
            parameter_count(ast, declaration) == query.arity &&
            is_same_name(lookup, query.name, name_attribute(ast, declaration))) {
            return declaration;
        }
    }
    return no_xslt_node;
}

/* A mode token of xsl:apply-templates or of a template rule. */
typedef struct mode_reference {
    /* NAME, UNNAMED, DEFAULT (resolved before use), CURRENT or ALL. */
    sxml_xslt_ast_name_token_kind kind;
    sxml_name_identifier name;
} mode_reference;

/* The mode a token names, with #default resolved against the element's
   static context (3.6.3). */
static mode_reference resolve_mode(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node,
                                   sxml_xslt_ast_name_token token) {
    if (token.kind == SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT) {
        const sxml_xslt_ast_static_context *context = sxml_xslt_ast_get_static_context(
            ast, sxml_xslt_ast_get_node(ast, node)->static_context);
        return (mode_reference){.kind = context->default_mode.kind ==
                                                SXML_XSLT_AST_NAME_TOKEN_KIND_NAME
                                            ? SXML_XSLT_AST_NAME_TOKEN_KIND_NAME
                                            : SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED,
                                .name = context->default_mode.name};
    }
    return (mode_reference){.kind = token.kind, .name = token.name};
}

/* True when a mode declaration (6.9.1) of reference says
   streamable="yes". */
static bool is_streamable_mode(const sxml_xslt_ast *ast, mode_reference reference) {
    if (reference.kind == SXML_XSLT_AST_NAME_TOKEN_KIND_CURRENT) {
        /* 19.8.4.5, note: #current is as a streamable mode. */
        return true;
    }
    /* 6.9.1: of the declarations that set streamable, the one of
       highest import precedence (the later of equal ones) decides. */
    bool is_streamable = false;
    uint32_t deciding_precedence = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        const sxml_xslt_ast_node *entry = sxml_xslt_ast_get_node(ast, declaration);
        if (entry->kind != SXML_XSLT_AST_KIND_MODE ||
            find_xslt_attribute(ast, declaration, SXML_XSLT_AST_ATTRIBUTE_STREAMABLE) == NULL) {
            continue;
        }
        const sxml_name_identifier name = name_attribute(ast, declaration);
        const bool is_named = name.index != 0;
        const bool matches = reference.kind == SXML_XSLT_AST_NAME_TOKEN_KIND_NAME
                                 ? is_named && name.index == reference.name.index
                                 : !is_named && reference.kind == SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED;
        const uint32_t precedence = sxml_xslt_ast_get_module(ast, entry->module)->import_precedence;
        if (matches && precedence >= deciding_precedence) {
            deciding_precedence = precedence;
            is_streamable = is_true_attribute(ast, declaration, SXML_XSLT_AST_ATTRIBUTE_STREAMABLE);
        }
    }
    return is_streamable;
}

/* True when some mode is streamable. */
static bool has_streamable_mode(const sxml_xslt_ast *ast) {
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (sxml_xslt_ast_get_node(ast, declaration)->kind != SXML_XSLT_AST_KIND_MODE) {
            continue;
        }
        const sxml_name_identifier name = name_attribute(ast, declaration);
        if (is_streamable_mode(ast, (mode_reference){.kind = name.index != 0
                                                                 ? SXML_XSLT_AST_NAME_TOKEN_KIND_NAME
                                                                 : SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED,
                                                     .name = name})) {
            return true;
        }
    }
    return false;
}

/* 19.8.4.1: attribute sets named by use-attribute-sets are motionless
   when every declaration of each says streamable="yes"; any other
   makes the instruction roaming. */
static bool are_streamable_attribute_sets(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *attribute =
        find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS);
    if (attribute == NULL || attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
        return true;
    }
    const sxml_xslt_ast_name_token_view tokens =
        sxml_xslt_ast_get_name_tokens(ast, attribute->value.payload.name_tokens);
    for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
        const sxml_name_identifier name = tokens.name_tokens[token_index].name;
        for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
            const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
            if (sxml_xslt_ast_get_node(ast, declaration)->kind == SXML_XSLT_AST_KIND_ATTRIBUTE_SET &&
                name_attribute(ast, declaration).index == name.index &&
                !is_true_attribute(ast, declaration, SXML_XSLT_AST_ATTRIBUTE_STREAMABLE)) {
                return false;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------ Machine */

/* What a frame assesses. */
typedef enum construct_source {
    SXML_STREAM_CONSTRUCT_SOURCE_NONE,
    /* A node of an XPath AST (an expression, a value template's parts). */
    SXML_STREAM_CONSTRUCT_SOURCE_XPATH,
    /* An XSLT element: an instruction, or xsl:attribute in an attribute
       set. */
    SXML_STREAM_CONSTRUCT_SOURCE_XSLT,
    /* The sequence constructor of an XSLT element: its instruction
       children (19.8.3). */
    SXML_STREAM_CONSTRUCT_SOURCE_SEQUENCE_CONSTRUCTOR,
    /* A pattern AST (19.8.10). */
    SXML_STREAM_CONSTRUCT_SOURCE_PATTERN,
    /* An operand whose assessment is known (operands only). */
    SXML_STREAM_CONSTRUCT_SOURCE_FIXED,
} construct_source;

typedef struct construct {
    construct_source source;
    /* XSLT: the element; SEQUENCE_CONSTRUCTOR: its parent. */
    sxml_xslt_ast_node_index node;
    /* XPATH and PATTERN. */
    const sxml_ast *ast;
    sxml_ast_node_index ast_node;
    sxml_xslt_ast_static_context_index static_context;
} construct;

/* The focus a construct is assessed in: its context posture (19.5) and
   context item type (19.2). */
typedef struct focus {
    posture posture;
    u_type type;
} focus;

/* Where current-group() of a construct stands (19.8.9.4). */
typedef enum group_state {
    SXML_STREAM_GROUP_STATE_NONE,
    /* In the body of the xsl:for-each-group of group_frame. */
    SXML_STREAM_GROUP_STATE_ACTIVE,
    /* Inside an xsl:source-document within it (19.8.4.37). */
    SXML_STREAM_GROUP_STATE_POISONED,
    /* The group of an xsl:for-each-group around an xsl:source-document
       whose body is checked on its own: nodes of another document,
       grounded for this one. */
    SXML_STREAM_GROUP_STATE_OTHER_DOCUMENT,
} group_state;

/* What the containing constructs say about a construct, inherited down
   the frames. */
typedef struct scope {
    /* The declared-streamable stylesheet function whose body this is: its
       first parameter is the streaming parameter (19.8.5). */
    sxml_xslt_ast_node_index streaming_function;
    group_state group_state;
    uint32_t group_frame;
    /* No higher-order or focus-changing operand between the group body
       and here. */
    bool is_group_path_clean;
    /* The xsl:source-document frame that a current-group() or
       current-merge-group() of an outer instruction poisons. */
    uint32_t poison_frame;
    bool is_merge_poisoned;
    bool is_in_pattern;
    /* The context of the outermost XPath expression, and whether a
       higher-order operand lies between it and here (19.8.9.3). */
    focus expression_focus;
    bool is_below_higher_order;
    /* For fn:accumulator-after (19.8.9.1): an enclosing node is part of a
       sequence constructor; a preceding sibling there is consuming; the
       focus changed since the innermost instruction. */
    bool is_in_sequence_constructor;
    bool follows_consuming;
    bool has_changed_focus;
    /* The phase of the xsl:accumulator-rule being assessed, or NONE. */
    sxml_xslt_ast_keyword accumulator_phase;
    bool is_child_of_fork;
    /* Inside a higher-order operand of some construct of the function
       body: a reference to the streaming parameter there is
       free-ranging (19.8.4.30, note; 19.8.4.18, note). */
    bool is_in_higher_order;
    /* The variable of a for or quantified expression over streamed nodes
       (step_binding), and the assessment of its binding sequence. */
    resolved_name bound_variable;
    assessment bound_value;
} scope;

static scope empty_scope(void) {
    scope value;
    memset(&value, 0, sizeof(value));
    value.expression_focus = (focus){.posture = SXML_STREAM_POSTURE_ROAMING, .type = 0U};
    value.accumulator_phase = SXML_XSLT_AST_KEYWORD_NONE;
    return value;
}

/* Operand flags. */
enum {
    /* A higher-order operand (19.8). */
    SXML_STREAM_OPERAND_FLAG_HIGHER_ORDER = 1U << 0U,
    /* A controlled operand: assessed in focus, which the controlling
       operand gave (19.2). */
    SXML_STREAM_OPERAND_FLAG_CHANGES_FOCUS = 1U << 1U,
    /* Assessed in focus without being a controlled operand. */
    SXML_STREAM_OPERAND_FLAG_GIVEN_FOCUS = 1U << 2U,
    /* An instruction of a sequence constructor. */
    SXML_STREAM_OPERAND_FLAG_SEQUENCE_CONSTRUCTOR_CHILD = 1U << 3U,
    /* The body of the frame's xsl:for-each-group. */
    SXML_STREAM_OPERAND_FLAG_GROUP_BODY = 1U << 4U,
    /* Uncertain when it turns out not grounded (an operand whose usage
       sxml could not refine, 19.8.9.8). */
    SXML_STREAM_OPERAND_FLAG_UNCERTAIN_UNLESS_GROUNDED = 1U << 5U,
    /* An xsl:for-each-group child of xsl:fork (19.8.4.19). */
    SXML_STREAM_OPERAND_FLAG_CHILD_OF_FORK = 1U << 6U,
    /* The body of an xsl:source-document: poisons current-group(). */
    SXML_STREAM_OPERAND_FLAG_SOURCE_DOCUMENT_BODY = 1U << 7U,
    /* A path component after "//" (descendant-or-self::node() first). */
    SXML_STREAM_OPERAND_FLAG_AFTER_DOUBLE_SLASH = 1U << 8U,
    /* A path component that is root(.) treat as document-node()
       (19.8.8.8). */
    SXML_STREAM_OPERAND_FLAG_ROOT = 1U << 9U,
};

typedef struct operand {
    construct construct;
    /* FIXED. */
    assessment fixed;
    usage usage;
    uint32_t choice_group;
    uint32_t flags;
    focus focus;
} operand;

/* Phases every frame shares; a construct's own phases count from 0. */
enum {
    SXML_STREAM_PHASE_GENERAL = 1000,
    SXML_STREAM_PHASE_RETURN_GENERAL = 1001,
};

typedef struct frame {
    construct construct;
    focus focus;
    scope scope;
    uint32_t phase;
    bool is_done;
    assessment result;
    /* The assessment of the operand last pushed. */
    bool has_child;
    assessment child;
    /* Assessments the construct keeps between phases. */
    assessment saved;
    assessment second;
    /* The operand stack's count when the frame was pushed. */
    uint32_t operand_base;
    /* The operands of the general rules, and the next to assess. */
    uint32_t operand_first;
    uint32_t operand_count;
    uint32_t operand_next;
    general_state general;
    general_finish finish;
    uint32_t after_general_phase;
    assessment general_result;
    /* A sequence constructor: an instruction so far was consuming. */
    bool saw_consuming;
    /* The construct's own position (a child, a predicate, a pattern
       node) and counts. */
    uint32_t cursor;
    uint32_t counter;
    bool flag;
    /* A node the construct names besides its own (a function call's
       call node, a stylesheet function). */
    sxml_ast_node_index auxiliary_ast_node;
    sxml_xslt_ast_node_index auxiliary_node;
    /* An xsl:source-document whose body uses current-group() of an outer
       instruction. */
    bool is_poisoned;
    /* An axis step after "//" assessed as a descendant step (see
       step_path). */
    bool is_descendant_step;
} frame;

typedef struct machine {
    const sxml_xslt_ast *ast;
    name_lookup lookup;
    sxml_allocator allocator;
    frame *frames;
    uint32_t frame_count;
    uint32_t frame_capacity;
    uint32_t maximum_frame_count;
    operand *operands;
    uint32_t operand_count;
    uint32_t operand_capacity;
    bool is_limit_reached;
} machine;

/* Grows an array of element_size bytes per element to hold one more. */
typedef struct growth {
    void **elements;
    uint32_t *capacity;
    uint32_t count;
    size_t element_size;
    size_t element_alignment;
} growth;

static sxml_status grow(sxml_allocator allocator, const growth *request) {
    if (request->count < *request->capacity) {
        return SXML_STATUS_OK;
    }
    const uint32_t new_capacity = *request->capacity == 0U ? 64U : *request->capacity * 2U;
    if (new_capacity <= *request->capacity) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        allocator,
        (sxml_array_allocation_request){.element_count = new_capacity,
                                        .element_size = request->element_size,
                                        .element_alignment = request->element_alignment},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (request->count != 0U && *request->elements != NULL) {
        memcpy(memory, *request->elements, (size_t)request->count * request->element_size);
    }
    sxml_allocator_release(allocator, *request->elements);
    *request->elements = memory;
    *request->capacity = new_capacity;
    return SXML_STATUS_OK;
}

static sxml_status add_operand(machine *state, uint32_t frame_index, operand value) {
    void *elements = state->operands;
    const sxml_status status =
        grow(state->allocator, &(growth){.elements = &elements,
                                         .capacity = &state->operand_capacity,
                                         .count = state->operand_count,
                                         .element_size = sizeof(operand),
                                         .element_alignment = alignof(operand)});
    state->operands = elements;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->operands[state->operand_count] = value;
    state->operand_count += 1U;
    state->frames[frame_index].operand_count += 1U;
    return SXML_STATUS_OK;
}

/* Starts a frame's operand list (on top of the operand stack). */
static void begin_operands(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    current->operand_first = state->operand_count;
    current->operand_count = 0;
    current->operand_next = 0;
    current->general = empty_general_state();
}

/* Combines the listed operands, then continues at after_phase with the
   combination in general_result. */
typedef struct general_start {
    general_finish finish;
    uint32_t after_phase;
} general_start;

static void start_general(machine *state, uint32_t frame_index, general_start start) {
    frame *current = &state->frames[frame_index];
    current->finish = start.finish;
    current->after_general_phase = start.after_phase;
    current->phase = SXML_STREAM_PHASE_GENERAL;
}

static void finish_frame(frame *current, assessment result) {
    current->result = result;
    current->is_done = true;
}

/* A frame to push. */
typedef struct frame_request {
    construct construct;
    focus focus;
    scope scope;
} frame_request;

static sxml_status push_frame(machine *state, const frame_request *request) {
    if (state->frame_count >= state->maximum_frame_count) {
        state->is_limit_reached = true;
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    void *elements = state->frames;
    const sxml_status status =
        grow(state->allocator, &(growth){.elements = &elements,
                                         .capacity = &state->frame_capacity,
                                         .count = state->frame_count,
                                         .element_size = sizeof(frame),
                                         .element_alignment = alignof(frame)});
    state->frames = elements;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame *pushed = &state->frames[state->frame_count];
    memset(pushed, 0, sizeof(*pushed));
    pushed->construct = request->construct;
    pushed->focus = request->focus;
    pushed->scope = request->scope;
    pushed->operand_base = state->operand_count;
    pushed->general = empty_general_state();
    state->frame_count += 1U;
    return SXML_STATUS_OK;
}

/* The scope and focus an operand of frame_index is assessed in. */
static frame_request operand_request(const machine *state, uint32_t frame_index,
                                     const operand *value) {
    const frame *current = &state->frames[frame_index];
    frame_request request = {
        .construct = value->construct, .focus = current->focus, .scope = current->scope};
    if ((value->flags & (SXML_STREAM_OPERAND_FLAG_HIGHER_ORDER | SXML_STREAM_OPERAND_FLAG_CHANGES_FOCUS)) != 0U) {
        request.scope.is_in_higher_order = true;
    }
    if ((value->flags & SXML_STREAM_OPERAND_FLAG_HIGHER_ORDER) != 0U) {
        request.scope.is_group_path_clean = false;
        request.scope.is_below_higher_order = true;
    }
    if ((value->flags & SXML_STREAM_OPERAND_FLAG_CHANGES_FOCUS) != 0U) {
        request.focus = value->focus;
        request.scope.has_changed_focus = true;
        request.scope.is_group_path_clean = false;
        request.scope.is_below_higher_order = true;
    } else if ((value->flags & SXML_STREAM_OPERAND_FLAG_GIVEN_FOCUS) != 0U) {
        request.focus = value->focus;
    }
    if (value->construct.source == SXML_STREAM_CONSTRUCT_SOURCE_SEQUENCE_CONSTRUCTOR) {
        /* An instruction's attributes are evaluated before its content,
           so for fn:accumulator-after a consuming attribute precedes the
           instructions of the content (19.8.9.1 rule 8). */
        request.scope.follows_consuming = current->scope.follows_consuming || current->saw_consuming;
    }
    if ((value->flags & SXML_STREAM_OPERAND_FLAG_SEQUENCE_CONSTRUCTOR_CHILD) != 0U) {
        request.scope.is_in_sequence_constructor = true;
        request.scope.follows_consuming = current->scope.follows_consuming || current->saw_consuming;
        request.scope.has_changed_focus = false;
    }
    if ((value->flags & SXML_STREAM_OPERAND_FLAG_GROUP_BODY) != 0U) {
        request.scope.group_state = SXML_STREAM_GROUP_STATE_ACTIVE;
        request.scope.group_frame = frame_index;
        request.scope.is_group_path_clean = true;
    }
    if ((value->flags & SXML_STREAM_OPERAND_FLAG_SOURCE_DOCUMENT_BODY) != 0U) {
        const bool is_grounded_group =
            request.scope.group_state == SXML_STREAM_GROUP_STATE_ACTIVE &&
            is_grounded_motionless(state->frames[request.scope.group_frame].saved);
        if (request.scope.group_state != SXML_STREAM_GROUP_STATE_NONE && !is_grounded_group) {
            request.scope.group_state = SXML_STREAM_GROUP_STATE_POISONED;
        }
        request.scope.is_merge_poisoned = true;
        request.scope.poison_frame = frame_index;
        request.scope.accumulator_phase = SXML_XSLT_AST_KEYWORD_NONE;
        request.scope.is_in_sequence_constructor = false;
        request.scope.follows_consuming = false;
    }
    request.scope.is_child_of_fork = (value->flags & SXML_STREAM_OPERAND_FLAG_CHILD_OF_FORK) != 0U;
    return request;
}

/* Pushes an operand of frame_index for assessment. */
static sxml_status push_operand(machine *state, uint32_t frame_index, operand value) {
    const frame_request request = operand_request(state, frame_index, &value);
    return push_frame(state, &request);
}

/* Takes the child's assessment, once. */
static bool take_child(frame *current, assessment *child) {
    if (!current->has_child) {
        return false;
    }
    current->has_child = false;
    *child = current->child;
    return true;
}

static void add_assessed_operand(frame *current, const operand *value, assessment assessed) {
    if ((value->flags & SXML_STREAM_OPERAND_FLAG_UNCERTAIN_UNLESS_GROUNDED) != 0U &&
        assessed.posture != SXML_STREAM_POSTURE_GROUNDED) {
        assessed.is_uncertain = true;
    }
    general_add(&current->general,
                (general_operand){.value = assessed,
                                  .usage = value->usage,
                                  .is_higher_order = (value->flags & SXML_STREAM_OPERAND_FLAG_HIGHER_ORDER) != 0U,
                                  .choice_group = value->choice_group});
    if (assessed.sweep != SXML_STREAM_SWEEP_MOTIONLESS) {
        current->saw_consuming = true;
    }
}

/* One step of SXML_STREAM_PHASE_GENERAL: takes the last operand's assessment, then
   pushes the next operand or combines them. */
static sxml_status step_general(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    assessment child;
    if (take_child(current, &child)) {
        add_assessed_operand(current, &state->operands[current->operand_first + current->operand_next - 1U],
                             child);
    }
    while (current->operand_next < current->operand_count) {
        const operand value = state->operands[current->operand_first + current->operand_next];
        current->operand_next += 1U;
        if (value.construct.source == SXML_STREAM_CONSTRUCT_SOURCE_FIXED) {
            add_assessed_operand(current, &value, value.fixed);
            continue;
        }
        return push_operand(state, frame_index, value);
    }
    current->general_result = general_result(&current->general, current->finish);
    current->phase = current->after_general_phase;
    if (current->phase == SXML_STREAM_PHASE_RETURN_GENERAL) {
        finish_frame(current, current->general_result);
    }
    return SXML_STATUS_OK;
}

/* Constructs of operands. */
static construct xpath_construct(const sxml_ast *ast, sxml_ast_node_index node,
                                 sxml_xslt_ast_static_context_index static_context) {
    return (construct){.source = SXML_STREAM_CONSTRUCT_SOURCE_XPATH,
                       .node = no_xslt_node,
                       .ast = ast,
                       .ast_node = node,
                       .static_context = static_context};
}

static construct expression_construct(const sxml_xslt_ast_expression *expression) {
    return xpath_construct(expression->ast, expression->ast->root, expression->static_context);
}

static construct xslt_construct(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node) {
    return (construct){.source = SXML_STREAM_CONSTRUCT_SOURCE_XSLT,
                       .node = node,
                       .ast = NULL,
                       .ast_node = no_ast_node,
                       .static_context = sxml_xslt_ast_get_node(ast, node)->static_context};
}

static construct sequence_constructor_construct(const sxml_xslt_ast *ast,
                                                sxml_xslt_ast_node_index node) {
    construct value = xslt_construct(ast, node);
    value.source = SXML_STREAM_CONSTRUCT_SOURCE_SEQUENCE_CONSTRUCTOR;
    return value;
}

static construct pattern_construct(const sxml_xslt_ast_expression *expression) {
    construct value = expression_construct(expression);
    value.source = SXML_STREAM_CONSTRUCT_SOURCE_PATTERN;
    return value;
}

static operand plain_operand(construct value, usage operand_usage) {
    operand result;
    memset(&result, 0, sizeof(result));
    result.construct = value;
    result.usage = operand_usage;
    return result;
}

static operand fixed_operand(assessment value, usage operand_usage) {
    operand result;
    memset(&result, 0, sizeof(result));
    result.construct.source = SXML_STREAM_CONSTRUCT_SOURCE_FIXED;
    result.fixed = value;
    result.usage = operand_usage;
    return result;
}

/* "." in focus (19.8.8.13). */
static assessment context_item(focus value) {
    return make_assessment(value.posture, SXML_STREAM_SWEEP_MOTIONLESS, value.type);
}

/* --------------------------------------------------- Built-in functions */

/* The operand a call takes from the focus when an argument is left out
   (19.8.9: "Equivalent to f(.)", "f(x, /)"). */
typedef enum implicit_operand {
    SXML_STREAM_IMPLICIT_NONE,
    SXML_STREAM_IMPLICIT_CONTEXT,
    SXML_STREAM_IMPLICIT_ROOT,
} implicit_operand;

/* The static type of a call's result (19.1, FunctionCall). TRANSMITTED:
   the union of the types of the transmission arguments. */
typedef enum result_type {
    SXML_STREAM_RESULT_ANY,
    SXML_STREAM_RESULT_ATOMIC,
    SXML_STREAM_RESULT_BOOLEAN,
    SXML_STREAM_RESULT_STRING,
    SXML_STREAM_RESULT_NUMERIC,
    SXML_STREAM_RESULT_NODES,
    SXML_STREAM_RESULT_FUNCTION,
    SXML_STREAM_RESULT_TRANSMITTED,
} result_type;

/* One row of 19.8.9 "Classifying Calls to Built-In Functions": the
   function's namespace, local name and the usage of each argument (A, I,
   T, N; the arity is their count). */
typedef struct built_in {
    const char *local_name;
    const char *usages;
    sxml_well_known_namespace namespace_uri;
    implicit_operand implicit;
    usage implicit_usage;
    result_type result;
} built_in;

#define BUILT_IN(namespace_uri, local_name, usages, implicit, implicit_usage, result)               \
    {local_name, usages, namespace_uri, implicit, implicit_usage, result}

/* The table of 19.8.9, with the rows that point to a section of their
   own given the usages that section names (reverse, innermost and
   fold-right navigation, 19.8.9.9, 19.8.9.13, 19.8.9.17;
   function-lookup, 19.8.9.12). normalize-space() and string-length()
   read the context item as their one-argument forms do (F&O 4.0). */
static const built_in built_ins[] = {
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "append", "IN", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "filter", "II", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "flatten", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "fold-left", "INI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "fold-right", "INI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "for-each", "II", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "for-each-pair", "III", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "get", "IA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "head", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "insert-before", "IAN", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "join", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "put", "IIN", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "remove", "IA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "reverse", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "size", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "sort", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "sort", "IA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "sort", "IAI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "subarray", "IA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "subarray", "IAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_ARRAY_FUNCTIONS, "tail", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "abs", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "adjust-date-to-timezone", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "adjust-date-to-timezone", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "adjust-dateTime-to-timezone", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "adjust-dateTime-to-timezone", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "adjust-time-to-timezone", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "adjust-time-to-timezone", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "analyze-string", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "analyze-string", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "apply", "AI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "available-environment-variables", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "available-system-properties", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "avg", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "base-uri", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_INSPECTION, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "base-uri", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "boolean", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "ceiling", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "codepoint-equal", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "codepoints-to-string", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "collation-key", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "collation-key", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "collection", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "collection", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "compare", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "compare", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "concat", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "contains", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "contains", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "contains-token", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "contains-token", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "copy-of", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_ABSORPTION, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "copy-of", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "count", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "current-date", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "current-dateTime", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "current-output-uri", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "current-time", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "data", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_ABSORPTION, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "data", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "dateTime", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "day-from-date", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "day-from-dateTime", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "days-from-duration", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "deep-equal", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "deep-equal", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "deep-equal", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "deep-equal", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "default-collation", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "default-language", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "distinct-values", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "distinct-values", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "doc", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "doc-available", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "document", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "document", "AI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "document-uri", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_INSPECTION, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "document-uri", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "element-available", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "element-with-id", "A", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_NAVIGATION, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "element-with-id", "AN", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "empty", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "encode-for-uri", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "ends-with", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "ends-with", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "environment-variable", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "error", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "error", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "error", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "error", "AAN", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "escape-html-uri", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "exactly-one", "T", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "exists", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "false", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "filter", "NI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "floor", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "fold-left", "NAI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "for-each", "NI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "for-each-pair", "NNI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "format-date", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "format-date", "AAAAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "format-dateTime", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "format-dateTime", "AAAAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "format-integer", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "format-integer", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "format-number", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "format-number", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "format-time", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "format-time", "AAAAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "function-arity", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "function-available", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "function-available", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "function-name", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "generate-id", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_INSPECTION, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "generate-id", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "has-children", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_INSPECTION, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "has-children", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "head", "T", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "hours-from-dateTime", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "hours-from-duration", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "hours-from-time", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "id", "A", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_NAVIGATION, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "id", "AN", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "idref", "A", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_NAVIGATION, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "idref", "AN", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "implicit-timezone", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "in-scope-prefixes", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "index-of", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "index-of", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "insert-before", "TAT", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "iri-to-uri", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "json-doc", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "json-doc", "AI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "json-to-xml", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "json-to-xml", "AI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "key", "AA", SXML_STREAM_IMPLICIT_ROOT, SXML_STREAM_USAGE_NAVIGATION, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "key", "AAN", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "lang", "A", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_INSPECTION, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "lang", "AI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "load-xquery-module", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "load-xquery-module", "AI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "local-name", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_INSPECTION, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "local-name", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "local-name-from-QName", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "lower-case", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "matches", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "matches", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "max", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "max", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "min", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "min", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "minutes-from-dateTime", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "minutes-from-duration", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "minutes-from-time", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "month-from-date", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "month-from-dateTime", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "months-from-duration", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "name", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_INSPECTION, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "name", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "namespace-uri", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_INSPECTION, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "namespace-uri", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "namespace-uri-for-prefix", "AI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "namespace-uri-from-QName", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "nilled", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_INSPECTION, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "nilled", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "node-name", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_INSPECTION, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "node-name", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "normalize-space", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_ABSORPTION, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "normalize-space", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "normalize-unicode", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "normalize-unicode", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "not", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "number", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_ABSORPTION, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "number", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "one-or-more", "T", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "parse-ietf-date", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "parse-json", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "parse-json", "AI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "parse-xml", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "parse-xml-fragment", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "path", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_NAVIGATION, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "path", "N", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "prefix-from-QName", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "QName", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "random-number-generator", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "random-number-generator", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "regex-group", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "remove", "TA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "replace", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "replace", "AAAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "resolve-QName", "AI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "resolve-uri", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "resolve-uri", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "round", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "round", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "round-half-to-even", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "round-half-to-even", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "seconds-from-dateTime", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "seconds-from-duration", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "seconds-from-time", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "serialize", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "serialize", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "snapshot", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_ABSORPTION, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "snapshot", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NODES),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "sort", "N", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "sort", "NA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "sort", "NAI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "starts-with", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "starts-with", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "static-base-uri", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "stream-available", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "string", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_ABSORPTION, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "string", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "string-join", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "string-join", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "string-length", "", SXML_STREAM_IMPLICIT_CONTEXT, SXML_STREAM_USAGE_ABSORPTION, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "string-length", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "string-to-codepoints", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "subsequence", "TA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "subsequence", "TAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "substring", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "substring", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "substring-after", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "substring-after", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "substring-before", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "substring-before", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "sum", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "sum", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "system-property", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "tail", "T", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "timezone-from-date", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "timezone-from-dateTime", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "timezone-from-time", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "tokenize", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "tokenize", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "tokenize", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "trace", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "trace", "TA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "transform", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "translate", "AAA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "true", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "type-available", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "unordered", "T", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "unparsed-entity-public-id", "A", SXML_STREAM_IMPLICIT_ROOT, SXML_STREAM_USAGE_INSPECTION, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "unparsed-entity-public-id", "AI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "unparsed-entity-uri", "A", SXML_STREAM_IMPLICIT_ROOT, SXML_STREAM_USAGE_INSPECTION, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "unparsed-entity-uri", "AI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "unparsed-text", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "unparsed-text", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "unparsed-text-available", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "unparsed-text-available", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "unparsed-text-lines", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "unparsed-text-lines", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "upper-case", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "uri-collection", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "uri-collection", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "xml-to-json", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "xml-to-json", "AI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_STRING),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "year-from-date", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "year-from-dateTime", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "years-from-duration", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "zero-or-one", "T", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_MAP_FUNCTIONS, "contains", "IA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_BOOLEAN),
    BUILT_IN(SXML_NAMESPACE_MAP_FUNCTIONS, "entry", "AN", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_MAP_FUNCTIONS, "find", "IA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_MAP_FUNCTIONS, "for-each", "II", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_MAP_FUNCTIONS, "get", "IA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_MAP_FUNCTIONS, "keys", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ATOMIC),
    BUILT_IN(SXML_NAMESPACE_MAP_FUNCTIONS, "merge", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_MAP_FUNCTIONS, "merge", "II", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_MAP_FUNCTIONS, "put", "IAN", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_MAP_FUNCTIONS, "remove", "IA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_MAP_FUNCTIONS, "size", "I", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "acos", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "asin", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "atan", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "atan2", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "cos", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "exp", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "exp10", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "log", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "log10", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "pi", "", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "pow", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "sin", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "sqrt", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_MATH_FUNCTIONS, "tan", "A", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_NUMERIC),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "reverse", "N", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "innermost", "N", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "fold-right", "NAI", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_ANY),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "function-lookup", "AA", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_FUNCTION),
    BUILT_IN(SXML_NAMESPACE_FUNCTIONS, "outermost", "T", SXML_STREAM_IMPLICIT_NONE, SXML_STREAM_USAGE_NONE, SXML_STREAM_RESULT_TRANSMITTED),
};

#undef BUILT_IN

static bool text_equals(sxml_byte_view text, const char *literal) {
    const size_t byte_count = strlen(literal);
    return text.byte_count == byte_count &&
           (byte_count == 0 || memcmp(text.bytes, literal, byte_count) == 0);
}

/* The row of a built-in function by name and arity; NULL for none. */
static const built_in *find_built_in(resolved_name name, uint32_t arity) {
    if (!name.is_resolved) {
        return NULL;
    }
    for (size_t entry_index = 0; entry_index < sizeof built_ins / sizeof built_ins[0];
         entry_index += 1) {
        const built_in *entry = &built_ins[entry_index];
        if (sxml_well_known_namespace_identifier(entry->namespace_uri).index ==
                name.namespace_uri.index &&
            strlen(entry->usages) == arity && text_equals(name.local_name, entry->local_name)) {
            return entry;
        }
    }
    return NULL;
}

static bool is_fn_function(resolved_name name, const char *local_name) {
    return name.is_resolved &&
           name.namespace_uri.index ==
               sxml_well_known_namespace_identifier(SXML_NAMESPACE_FUNCTIONS).index &&
           text_equals(name.local_name, local_name);
}

static usage usage_of_letter(char letter) {
    switch (letter) {
    case 'A':
        return SXML_STREAM_USAGE_ABSORPTION;
    case 'I':
        return SXML_STREAM_USAGE_INSPECTION;
    case 'T':
        return SXML_STREAM_USAGE_TRANSMISSION;
    default:
        return SXML_STREAM_USAGE_NAVIGATION;
    }
}

/* A focus-dependent function (10.3.6, F&O 4.0 "focus-dependent"): one
   of arity that reads the focus. */
static bool is_focus_dependent_function(resolved_name name, uint32_t arity) {
    if (arity == 0 && (is_fn_function(name, "position") || is_fn_function(name, "last") ||
                       is_fn_function(name, "root"))) {
        return true;
    }
    const built_in *entry = find_built_in(name, arity);
    return entry != NULL && entry->implicit != SXML_STREAM_IMPLICIT_NONE;
}

/* ------------------------------------------------------- Syntax scans */

/* What a scan of an expression's subtree looks for. */
typedef struct scan_request {
    const sxml_ast *ast;
    sxml_ast_node_index root;
    sxml_xslt_ast_static_context_index static_context;
    /* Skip nested predicates. */
    bool skips_predicates;
    /* Skip operands with another focus-setting container (the right of
       "/" and "!", inline function bodies, predicates). */
    bool skips_controlled_operands;
    /* The streaming parameter's name, when there is one. */
    resolved_name streaming_parameter;
} scan_request;

typedef struct scan_result {
    /* A context item expression, an axis step or a call on a
       focus-dependent function (19.8.8.9 rule 4). */
    bool depends_on_focus;
    /* A call on or reference to position, last or function-lookup
       (19.8.10, "non-positional predicate"). */
    bool has_positional_call;
    /* A reference to the streaming parameter (19.8.8.16, 19.8.10). */
    bool references_streaming_parameter;
} scan_result;

static uint32_t argument_count(const sxml_ast *ast, sxml_ast_node_index arguments) {
    uint32_t count = 0;
    for (sxml_ast_node_index argument = sxml_ast_get_node(ast, arguments)->first_child;
         argument.value != 0; argument = sxml_ast_get_node(ast, argument)->next_sibling) {
        if (sxml_ast_get_node(ast, argument)->field == SXML_AST_FIELD_ARGUMENT) {
            count += 1U;
        }
    }
    return count;
}

static bool is_skipped_in_scan(const scan_request *request, sxml_ast_node_index node) {
    if (node.value == request->root.value) {
        return false;
    }
    const sxml_ast_node *entry = sxml_ast_get_node(request->ast, node);
    if (entry->kind == SXML_AST_KIND_PREDICATE &&
        (request->skips_predicates || request->skips_controlled_operands)) {
        return true;
    }
    if (!request->skips_controlled_operands) {
        return false;
    }
    const sxml_ast_kind parent_kind = sxml_ast_get_node(request->ast, entry->parent)->kind;
    return (entry->field == SXML_AST_FIELD_RIGHT &&
            (parent_kind == SXML_AST_KIND_RELATIVE_PATH_EXPRESSION ||
             parent_kind == SXML_AST_KIND_SIMPLE_MAP_EXPRESSION)) ||
           (entry->field == SXML_AST_FIELD_BODY &&
            parent_kind == SXML_AST_KIND_INLINE_FUNCTION_EXPRESSION);
}

/* A node of a scan. */
typedef struct scanned_node {
    const scan_request *request;
    sxml_ast_node_index node;
} scanned_node;

static void scan_node(const name_lookup *lookup, scanned_node scanned, scan_result *result) {
    const scan_request *request = scanned.request;
    const sxml_ast_node_index node = scanned.node;
    const sxml_ast *ast = request->ast;
    const sxml_ast_node *entry = sxml_ast_get_node(ast, node);
    if (entry->kind == SXML_AST_KIND_CONTEXT_VALUE_REFERENCE ||
        entry->kind == SXML_AST_KIND_AXIS_STEP || entry->kind == SXML_AST_KIND_UNARY_LOOKUP) {
        result->depends_on_focus = true;
    }
    if (entry->kind == SXML_AST_KIND_FUNCTION_CALL ||
        entry->kind == SXML_AST_KIND_NAMED_FUNCTION_REFERENCE) {
        const resolved_name name = resolve_name(
            lookup, (name_to_resolve){.ast = ast,
                                      .name = child_by_field(ast, node, SXML_AST_FIELD_NAME),
                                      .static_context = request->static_context,
                                      .default_namespace = SXML_NAMESPACE_FUNCTIONS});
        if (is_fn_function(name, "position") || is_fn_function(name, "last") ||
            is_fn_function(name, "function-lookup")) {
            result->has_positional_call = true;
        }
        if (entry->kind == SXML_AST_KIND_FUNCTION_CALL &&
            is_focus_dependent_function(
                name, argument_count(ast, child_by_field(ast, node, SXML_AST_FIELD_ARGUMENTS)))) {
            result->depends_on_focus = true;
        }
    }
    if (entry->kind == SXML_AST_KIND_VARIABLE_REFERENCE && request->streaming_parameter.is_resolved) {
        const resolved_name name = resolve_name(
            lookup, (name_to_resolve){.ast = ast,
                                      .name = child_by_field(ast, node, SXML_AST_FIELD_NAME),
                                      .static_context = request->static_context,
                                      .default_namespace = SXML_NAMESPACE_NONE});
        if (name.is_resolved &&
            name.namespace_uri.index == request->streaming_parameter.namespace_uri.index &&
            sxml_byte_view_equals(name.local_name, request->streaming_parameter.local_name)) {
            result->references_streaming_parameter = true;
        }
    }
}

/* Walks the subtree of request->root in pre-order, without recursion:
   down through first children, then to the next sibling of the nearest
   ancestor that has one. */
static scan_result scan_expression(const name_lookup *lookup, const scan_request *request) {
    scan_result result = {.depends_on_focus = false,
                          .has_positional_call = false,
                          .references_streaming_parameter = false};
    const sxml_ast *ast = request->ast;
    sxml_ast_node_index node = request->root;
    while (node.value != 0) {
        bool descends = false;
        if (!is_skipped_in_scan(request, node)) {
            scan_node(lookup, (scanned_node){.request = request, .node = node}, &result);
            descends = true;
        }
        const sxml_ast_node_index first_child = sxml_ast_get_node(ast, node)->first_child;
        if (descends && first_child.value != 0) {
            node = first_child;
            continue;
        }
        while (node.value != request->root.value &&
               sxml_ast_get_node(ast, node)->next_sibling.value == 0) {
            node = sxml_ast_get_node(ast, node)->parent;
        }
        node = node.value == request->root.value ? no_ast_node
                                                 : sxml_ast_get_node(ast, node)->next_sibling;
    }
    return result;
}

/* ---------------------------------------------------------- Axis steps */

typedef enum axis_class {
    SXML_STREAM_AXIS_CLASS_SELF,
    SXML_STREAM_AXIS_CLASS_ATTRIBUTE,
    SXML_STREAM_AXIS_CLASS_NAMESPACE,
    SXML_STREAM_AXIS_CLASS_CHILD,
    SXML_STREAM_AXIS_CLASS_DESCENDANT,
    SXML_STREAM_AXIS_CLASS_DESCENDANT_OR_SELF,
    SXML_STREAM_AXIS_CLASS_PARENT,
    SXML_STREAM_AXIS_CLASS_ANCESTOR,
    SXML_STREAM_AXIS_CLASS_ANCESTOR_OR_SELF,
    /* following, preceding and the sibling axes. */
    SXML_STREAM_AXIS_CLASS_SIBLINGS,
    /* Axes chapter 19 has no rule for (XPath 4.0's -or-self axes). */
    SXML_STREAM_AXIS_CLASS_OTHER,
} axis_class;

static axis_class classify_axis(sxml_ast_axis axis) {
    switch (axis) {
    case SXML_AST_AXIS_NONE:
    case SXML_AST_AXIS_CHILD:
        return SXML_STREAM_AXIS_CLASS_CHILD;
    case SXML_AST_AXIS_SELF:
        return SXML_STREAM_AXIS_CLASS_SELF;
    case SXML_AST_AXIS_ATTRIBUTE:
        return SXML_STREAM_AXIS_CLASS_ATTRIBUTE;
    case SXML_AST_AXIS_NAMESPACE:
        return SXML_STREAM_AXIS_CLASS_NAMESPACE;
    case SXML_AST_AXIS_DESCENDANT:
        return SXML_STREAM_AXIS_CLASS_DESCENDANT;
    case SXML_AST_AXIS_DESCENDANT_OR_SELF:
        return SXML_STREAM_AXIS_CLASS_DESCENDANT_OR_SELF;
    case SXML_AST_AXIS_PARENT:
        return SXML_STREAM_AXIS_CLASS_PARENT;
    case SXML_AST_AXIS_ANCESTOR:
        return SXML_STREAM_AXIS_CLASS_ANCESTOR;
    case SXML_AST_AXIS_ANCESTOR_OR_SELF:
        return SXML_STREAM_AXIS_CLASS_ANCESTOR_OR_SELF;
    case SXML_AST_AXIS_FOLLOWING:
    case SXML_AST_AXIS_FOLLOWING_SIBLING:
    case SXML_AST_AXIS_PRECEDING:
    case SXML_AST_AXIS_PRECEDING_SIBLING:
        return SXML_STREAM_AXIS_CLASS_SIBLINGS;
    case SXML_AST_AXIS_FOLLOWING_OR_SELF:
    case SXML_AST_AXIS_FOLLOWING_SIBLING_OR_SELF:
    case SXML_AST_AXIS_PRECEDING_OR_SELF:
    case SXML_AST_AXIS_PRECEDING_SIBLING_OR_SELF:
    case SXML_AST_AXIS_ITEM:
        return SXML_STREAM_AXIS_CLASS_OTHER;
    }
    return SXML_STREAM_AXIS_CLASS_OTHER;
}

/* 19.1.1: the node kinds an axis reaches from context items of type
   context_type. */
static u_type reachable_kinds(axis_class axis, u_type context_type) {
    const u_type nodes = context_type & SXML_U_NODES;
    switch (axis) {
    case SXML_STREAM_AXIS_CLASS_SELF:
        return nodes;
    case SXML_STREAM_AXIS_CLASS_ATTRIBUTE:
        return (nodes & SXML_U_ELEMENT) != 0U ? SXML_U_ATTRIBUTE : 0U;
    case SXML_STREAM_AXIS_CLASS_NAMESPACE:
        return (nodes & SXML_U_ELEMENT) != 0U ? SXML_U_NAMESPACE : 0U;
    case SXML_STREAM_AXIS_CLASS_CHILD:
    case SXML_STREAM_AXIS_CLASS_DESCENDANT:
        return (nodes & SXML_U_PARENTS) != 0U ? SXML_U_CHILDREN : 0U;
    case SXML_STREAM_AXIS_CLASS_DESCENDANT_OR_SELF:
        return ((nodes & SXML_U_PARENTS) != 0U ? SXML_U_CHILDREN : 0U) | nodes;
    case SXML_STREAM_AXIS_CLASS_PARENT:
    case SXML_STREAM_AXIS_CLASS_ANCESTOR:
        return nodes == SXML_U_DOCUMENT || nodes == 0U ? 0U : SXML_U_PARENTS;
    case SXML_STREAM_AXIS_CLASS_ANCESTOR_OR_SELF:
        return (nodes == SXML_U_DOCUMENT || nodes == 0U ? 0U : SXML_U_PARENTS) | nodes;
    case SXML_STREAM_AXIS_CLASS_SIBLINGS:
    case SXML_STREAM_AXIS_CLASS_OTHER:
        return nodes == SXML_U_DOCUMENT || nodes == 0U ? 0U : SXML_U_CHILDREN;
    }
    return SXML_U_NODES;
}

/* The principal node kind of an axis (XPath 4.0, 4.6.4.3). */
static u_type principal_kind(axis_class axis) {
    switch (axis) {
    case SXML_STREAM_AXIS_CLASS_ATTRIBUTE:
        return SXML_U_ATTRIBUTE;
    case SXML_STREAM_AXIS_CLASS_NAMESPACE:
        return SXML_U_NAMESPACE;
    case SXML_STREAM_AXIS_CLASS_SELF:
    case SXML_STREAM_AXIS_CLASS_CHILD:
    case SXML_STREAM_AXIS_CLASS_DESCENDANT:
    case SXML_STREAM_AXIS_CLASS_DESCENDANT_OR_SELF:
    case SXML_STREAM_AXIS_CLASS_PARENT:
    case SXML_STREAM_AXIS_CLASS_ANCESTOR:
    case SXML_STREAM_AXIS_CLASS_ANCESTOR_OR_SELF:
    case SXML_STREAM_AXIS_CLASS_SIBLINGS:
    case SXML_STREAM_AXIS_CLASS_OTHER:
        return SXML_U_ELEMENT;
    }
    return SXML_U_ELEMENT;
}

/* T(NT) of 19.1.1 for the node test of an axis step: a name test is the
   axis's principal node kind, a kind test its U-type. */
static u_type node_test_kinds(const sxml_ast *ast, sxml_ast_node_index test, axis_class axis) {
    const sxml_ast_node *node = sxml_ast_get_node(ast, test);
    if (node->kind == SXML_AST_KIND_NCNAME || node->kind == SXML_AST_KIND_PREFIXED_NAME ||
        node->kind == SXML_AST_KIND_URI_QUALIFIED_NAME || node->kind == SXML_AST_KIND_WILDCARD) {
        return principal_kind(axis);
    }
    if (node->kind == SXML_AST_KIND_UNION_NODE_TEST) {
        u_type type = 0U;
        for (sxml_ast_node_index member = node->first_child; member.value != 0;
             member = sxml_ast_get_node(ast, member)->next_sibling) {
            const sxml_ast_kind member_kind = sxml_ast_get_node(ast, member)->kind;
            type |= member_kind == SXML_AST_KIND_NCNAME || member_kind == SXML_AST_KIND_PREFIXED_NAME ||
                            member_kind == SXML_AST_KIND_URI_QUALIFIED_NAME ||
                            member_kind == SXML_AST_KIND_WILDCARD
                        ? principal_kind(axis)
                        : item_type_u_type(ast, member) & SXML_U_NODES;
        }
        return type;
    }
    if (node->kind == SXML_AST_KIND_NONE) {
        return SXML_U_NODES;
    }
    return item_type_u_type(ast, test) & SXML_U_NODES;
}

/* The axis an AXIS_STEP node uses: the default axis of 4.6.5 when none
   is written (attribute for attribute() and schema-attribute() tests,
   namespace for namespace-node()), parent for "..". */
static axis_class axis_of_step(const sxml_ast *ast, sxml_ast_node_index step) {
    const sxml_ast_node *node = sxml_ast_get_node(ast, step);
    if (node->detail.axis != SXML_AST_AXIS_NONE) {
        return classify_axis(node->detail.axis);
    }
    const sxml_ast_kind test_kind =
        sxml_ast_get_node(ast, child_by_field(ast, step, SXML_AST_FIELD_TEST))->kind;
    if (test_kind == SXML_AST_KIND_ATTRIBUTE_NODE_TYPE ||
        test_kind == SXML_AST_KIND_SCHEMA_ATTRIBUTE_NODE_TYPE) {
        return SXML_STREAM_AXIS_CLASS_ATTRIBUTE;
    }
    if (test_kind == SXML_AST_KIND_NAMESPACE_NODE_TYPE) {
        return SXML_STREAM_AXIS_CLASS_NAMESPACE;
    }
    return SXML_STREAM_AXIS_CLASS_CHILD;
}

/* The axes of ForwardAxisP (5.5.2): a step of a motionless pattern. */
static bool is_pattern_axis(axis_class axis) {
    return axis == SXML_STREAM_AXIS_CLASS_CHILD || axis == SXML_STREAM_AXIS_CLASS_DESCENDANT ||
           axis == SXML_STREAM_AXIS_CLASS_ATTRIBUTE || axis == SXML_STREAM_AXIS_CLASS_SELF ||
           axis == SXML_STREAM_AXIS_CLASS_DESCENDANT_OR_SELF || axis == SXML_STREAM_AXIS_CLASS_NAMESPACE;
}

/* An axis step to assess without its predicates. */
typedef struct bare_step {
    focus focus;
    axis_class axis;
    u_type test_kinds;
} bare_step;

/* 19.8.8.9 rules 1, 2, 3 and 6 (the table). */
static assessment assess_bare_step(bare_step step) {
    const u_type type = reachable_kinds(step.axis, step.focus.type) & step.test_kinds;
    if (step.focus.posture == SXML_STREAM_POSTURE_GROUNDED) {
        return grounded_motionless(type);
    }
    if (step.focus.posture == SXML_STREAM_POSTURE_ROAMING) {
        return roaming_free_ranging(type);
    }
    if (type == 0U) {
        return grounded_motionless(0U);
    }
    const bool selects_elements = (type & SXML_U_ELEMENT) != 0U;
    const posture context = step.focus.posture;
    switch (step.axis) {
    case SXML_STREAM_AXIS_CLASS_PARENT:
    case SXML_STREAM_AXIS_CLASS_ANCESTOR:
    case SXML_STREAM_AXIS_CLASS_ANCESTOR_OR_SELF:
        return make_assessment(SXML_STREAM_POSTURE_CLIMBING, SXML_STREAM_SWEEP_MOTIONLESS, type);
    case SXML_STREAM_AXIS_CLASS_ATTRIBUTE:
    case SXML_STREAM_AXIS_CLASS_NAMESPACE:
        return make_assessment(SXML_STREAM_POSTURE_STRIDING, SXML_STREAM_SWEEP_MOTIONLESS, type);
    case SXML_STREAM_AXIS_CLASS_SELF:
        if (context == SXML_STREAM_POSTURE_CRAWLING && !selects_elements) {
            return make_assessment(SXML_STREAM_POSTURE_STRIDING, SXML_STREAM_SWEEP_MOTIONLESS, type);
        }
        return make_assessment(context, SXML_STREAM_SWEEP_MOTIONLESS, type);
    case SXML_STREAM_AXIS_CLASS_CHILD:
        return context == SXML_STREAM_POSTURE_STRIDING
                   ? make_assessment(SXML_STREAM_POSTURE_STRIDING, SXML_STREAM_SWEEP_CONSUMING, type)
                   : roaming_free_ranging(type);
    case SXML_STREAM_AXIS_CLASS_DESCENDANT:
    case SXML_STREAM_AXIS_CLASS_DESCENDANT_OR_SELF:
        if (context != SXML_STREAM_POSTURE_STRIDING) {
            return roaming_free_ranging(type);
        }
        return make_assessment(selects_elements ? SXML_STREAM_POSTURE_CRAWLING : SXML_STREAM_POSTURE_STRIDING,
                               SXML_STREAM_SWEEP_CONSUMING, type);
    case SXML_STREAM_AXIS_CLASS_SIBLINGS:
    case SXML_STREAM_AXIS_CLASS_OTHER:
        return roaming_free_ranging(type);
    }
    return roaming_free_ranging(type);
}

/* 19.8.9.18: root(X) of X's assessment; "/" is root(.) treat as
   document-node() (19.8.8.8). */
static assessment assess_root(assessment argument) {
    if (argument.type == SXML_U_DOCUMENT && argument.posture == SXML_STREAM_POSTURE_STRIDING) {
        assessment result = argument;
        result.is_pattern_step = true;
        return result;
    }
    /* head((X)/ancestor-or-self::node()): the step's posture, which head
       transmits. */
    assessment step = assess_bare_step(
        (bare_step){.focus = {.posture = argument.posture, .type = argument.type},
                    .axis = SXML_STREAM_AXIS_CLASS_ANCESTOR_OR_SELF,
                    .test_kinds = SXML_U_NODES});
    step.sweep = wider_sweep(step.sweep, argument.sweep);
    step = normalized(step);
    step.type = SXML_U_DOCUMENT;
    step.is_uncertain = argument.is_uncertain;
    step.is_pattern_step = true;
    return step;
}

/* ------------------------------------------------------ XPath constructs */

/* How an XPath node kind is assessed (19.8.8). UNCERTAIN, 0, for kinds
   chapter 19 has no rule for. */
typedef enum xpath_rule {
    SXML_STREAM_XPATH_RULE_UNCERTAIN = 0,
    /* One operand passed through: the root, (E), { E }. */
    SXML_STREAM_XPATH_RULE_PASS,
    /* The general rules with the usages of general_child_usage. */
    SXML_STREAM_XPATH_RULE_GENERAL,
    SXML_STREAM_XPATH_RULE_LITERAL,
    SXML_STREAM_XPATH_RULE_CONTEXT_ITEM,
    SXML_STREAM_XPATH_RULE_VARIABLE,
    SXML_STREAM_XPATH_RULE_AXIS_STEP,
    SXML_STREAM_XPATH_RULE_PATH,
    SXML_STREAM_XPATH_RULE_SIMPLE_MAP,
    SXML_STREAM_XPATH_RULE_FILTER,
    SXML_STREAM_XPATH_RULE_UNION,
    SXML_STREAM_XPATH_RULE_FUNCTION_CALL,
    SXML_STREAM_XPATH_RULE_DYNAMIC_CALL,
    SXML_STREAM_XPATH_RULE_FLWOR,
    SXML_STREAM_XPATH_RULE_QUANTIFIED,
    SXML_STREAM_XPATH_RULE_INLINE_FUNCTION,
    SXML_STREAM_XPATH_RULE_NAMED_FUNCTION_REFERENCE,
    SXML_STREAM_XPATH_RULE_MAP_CONSTRUCTOR,
    SXML_STREAM_XPATH_RULE_IF,
    SXML_STREAM_XPATH_RULE_INSTANCE_OF,
    SXML_STREAM_XPATH_RULE_TREAT,
    SXML_STREAM_XPATH_RULE_LOOKUP,
    SXML_STREAM_XPATH_RULE_UNARY_LOOKUP,
} xpath_rule;

static const uint8_t xpath_rules[SXML_AST_KIND_COUNT] = {
    [SXML_AST_KIND_XPATH] = SXML_STREAM_XPATH_RULE_PASS,
    [SXML_AST_KIND_PARENTHESIZED_EXPRESSION] = SXML_STREAM_XPATH_RULE_PASS,
    [SXML_AST_KIND_ENCLOSED_EXPRESSION] = SXML_STREAM_XPATH_RULE_PASS,
    [SXML_AST_KIND_SEQUENCE_EXPRESSION] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_OR_EXPRESSION] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_AND_EXPRESSION] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_COMPARISON_EXPRESSION] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_OTHERWISE_EXPRESSION] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_STRING_CONCATENATION_EXPRESSION] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_RANGE_EXPRESSION] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_ADDITIVE_EXPRESSION] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_MULTIPLICATIVE_EXPRESSION] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_CASTABLE_EXPRESSION] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_CAST_EXPRESSION] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_UNARY_EXPRESSION] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_STRING_TEMPLATE] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_MAP_CONSTRUCTOR_ENTRY] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_SQUARE_ARRAY_CONSTRUCTOR] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_CURLY_ARRAY_CONSTRUCTOR] = SXML_STREAM_XPATH_RULE_GENERAL,
    [SXML_AST_KIND_INTEGER_LITERAL] = SXML_STREAM_XPATH_RULE_LITERAL,
    [SXML_AST_KIND_HEX_INTEGER_LITERAL] = SXML_STREAM_XPATH_RULE_LITERAL,
    [SXML_AST_KIND_BINARY_INTEGER_LITERAL] = SXML_STREAM_XPATH_RULE_LITERAL,
    [SXML_AST_KIND_DECIMAL_LITERAL] = SXML_STREAM_XPATH_RULE_LITERAL,
    [SXML_AST_KIND_DOUBLE_LITERAL] = SXML_STREAM_XPATH_RULE_LITERAL,
    [SXML_AST_KIND_STRING_LITERAL] = SXML_STREAM_XPATH_RULE_LITERAL,
    [SXML_AST_KIND_QNAME_LITERAL] = SXML_STREAM_XPATH_RULE_LITERAL,
    [SXML_AST_KIND_BOOLEAN_CONSTANT] = SXML_STREAM_XPATH_RULE_LITERAL,
    [SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART] = SXML_STREAM_XPATH_RULE_LITERAL,
    [SXML_AST_KIND_CONTEXT_VALUE_REFERENCE] = SXML_STREAM_XPATH_RULE_CONTEXT_ITEM,
    [SXML_AST_KIND_VARIABLE_REFERENCE] = SXML_STREAM_XPATH_RULE_VARIABLE,
    [SXML_AST_KIND_AXIS_STEP] = SXML_STREAM_XPATH_RULE_AXIS_STEP,
    [SXML_AST_KIND_RELATIVE_PATH_EXPRESSION] = SXML_STREAM_XPATH_RULE_PATH,
    [SXML_AST_KIND_ABSOLUTE_PATH_EXPRESSION] = SXML_STREAM_XPATH_RULE_PATH,
    [SXML_AST_KIND_SIMPLE_MAP_EXPRESSION] = SXML_STREAM_XPATH_RULE_SIMPLE_MAP,
    [SXML_AST_KIND_FILTER_EXPRESSION] = SXML_STREAM_XPATH_RULE_FILTER,
    [SXML_AST_KIND_UNION_EXPRESSION] = SXML_STREAM_XPATH_RULE_UNION,
    [SXML_AST_KIND_INTERSECT_EXCEPT_EXPRESSION] = SXML_STREAM_XPATH_RULE_UNION,
    [SXML_AST_KIND_FUNCTION_CALL] = SXML_STREAM_XPATH_RULE_FUNCTION_CALL,
    [SXML_AST_KIND_ARROW_EXPRESSION] = SXML_STREAM_XPATH_RULE_FUNCTION_CALL,
    [SXML_AST_KIND_DYNAMIC_FUNCTION_CALL] = SXML_STREAM_XPATH_RULE_DYNAMIC_CALL,
    [SXML_AST_KIND_FLWOR_EXPRESSION] = SXML_STREAM_XPATH_RULE_FLWOR,
    [SXML_AST_KIND_QUANTIFIED_EXPRESSION] = SXML_STREAM_XPATH_RULE_QUANTIFIED,
    [SXML_AST_KIND_INLINE_FUNCTION_EXPRESSION] = SXML_STREAM_XPATH_RULE_INLINE_FUNCTION,
    [SXML_AST_KIND_NAMED_FUNCTION_REFERENCE] = SXML_STREAM_XPATH_RULE_NAMED_FUNCTION_REFERENCE,
    [SXML_AST_KIND_MAP_CONSTRUCTOR] = SXML_STREAM_XPATH_RULE_MAP_CONSTRUCTOR,
    [SXML_AST_KIND_IF_EXPRESSION] = SXML_STREAM_XPATH_RULE_IF,
    [SXML_AST_KIND_INSTANCE_OF_EXPRESSION] = SXML_STREAM_XPATH_RULE_INSTANCE_OF,
    [SXML_AST_KIND_TREAT_EXPRESSION] = SXML_STREAM_XPATH_RULE_TREAT,
    [SXML_AST_KIND_LOOKUP_EXPRESSION] = SXML_STREAM_XPATH_RULE_LOOKUP,
    [SXML_AST_KIND_UNARY_LOOKUP] = SXML_STREAM_XPATH_RULE_UNARY_LOOKUP,
};

static xpath_rule rule_of_xpath_kind(sxml_ast_kind kind) {
    return (uint32_t)kind < (uint32_t)SXML_AST_KIND_COUNT ? (xpath_rule)xpath_rules[kind]
                                                         : SXML_STREAM_XPATH_RULE_UNCERTAIN;
}

/* 19.8.8, the proformas of the operand roles table, for the kinds of
   SXML_STREAM_XPATH_RULE_GENERAL: the usage of a child in role field; NONE for a
   child that is no operand (a type). */
static usage general_child_usage(const sxml_ast_node *parent, sxml_ast_field field) {
    const sxml_ast_kind kind = parent->kind;
    if (kind == SXML_AST_KIND_SEQUENCE_EXPRESSION) {
        return field == SXML_AST_FIELD_ITEM ? SXML_STREAM_USAGE_TRANSMISSION : SXML_STREAM_USAGE_NONE;
    }
    if (kind == SXML_AST_KIND_OTHERWISE_EXPRESSION) {
        return field == SXML_AST_FIELD_LEFT || field == SXML_AST_FIELD_RIGHT ? SXML_STREAM_USAGE_TRANSMISSION
                                                                             : SXML_STREAM_USAGE_NONE;
    }
    if (kind == SXML_AST_KIND_OR_EXPRESSION || kind == SXML_AST_KIND_AND_EXPRESSION) {
        return field == SXML_AST_FIELD_LEFT || field == SXML_AST_FIELD_RIGHT ? SXML_STREAM_USAGE_INSPECTION
                                                                             : SXML_STREAM_USAGE_NONE;
    }
    if (kind == SXML_AST_KIND_COMPARISON_EXPRESSION) {
        const sxml_ast_operator comparison = parent->detail.operator;
        const bool is_node_comparison =
            comparison == SXML_AST_OPERATOR_IS || comparison == SXML_AST_OPERATOR_IS_NOT ||
            comparison == SXML_AST_OPERATOR_PRECEDES || comparison == SXML_AST_OPERATOR_FOLLOWS ||
            comparison == SXML_AST_OPERATOR_PRECEDES_OR_IS ||
            comparison == SXML_AST_OPERATOR_FOLLOWS_OR_IS;
        if (field != SXML_AST_FIELD_LEFT && field != SXML_AST_FIELD_RIGHT) {
            return SXML_STREAM_USAGE_NONE;
        }
        return is_node_comparison ? SXML_STREAM_USAGE_INSPECTION : SXML_STREAM_USAGE_ABSORPTION;
    }
    if (kind == SXML_AST_KIND_STRING_CONCATENATION_EXPRESSION ||
        kind == SXML_AST_KIND_RANGE_EXPRESSION || kind == SXML_AST_KIND_ADDITIVE_EXPRESSION ||
        kind == SXML_AST_KIND_MULTIPLICATIVE_EXPRESSION) {
        return field == SXML_AST_FIELD_LEFT || field == SXML_AST_FIELD_RIGHT ? SXML_STREAM_USAGE_ABSORPTION
                                                                             : SXML_STREAM_USAGE_NONE;
    }
    if (kind == SXML_AST_KIND_CASTABLE_EXPRESSION || kind == SXML_AST_KIND_CAST_EXPRESSION) {
        return field == SXML_AST_FIELD_VALUE ? SXML_STREAM_USAGE_ABSORPTION : SXML_STREAM_USAGE_NONE;
    }
    if (kind == SXML_AST_KIND_UNARY_EXPRESSION) {
        return field == SXML_AST_FIELD_OPERAND ? SXML_STREAM_USAGE_ABSORPTION : SXML_STREAM_USAGE_NONE;
    }
    if (kind == SXML_AST_KIND_STRING_TEMPLATE) {
        /* 19.8.7: value templates absorb their expressions. */
        return field == SXML_AST_FIELD_PART ? SXML_STREAM_USAGE_ABSORPTION : SXML_STREAM_USAGE_NONE;
    }
    if (kind == SXML_AST_KIND_MAP_CONSTRUCTOR_ENTRY) {
        /* 19.8.8.17 through 19.8.4.24: key A, value N. */
        if (field == SXML_AST_FIELD_KEY) {
            return SXML_STREAM_USAGE_ABSORPTION;
        }
        return field == SXML_AST_FIELD_VALUE ? SXML_STREAM_USAGE_NAVIGATION : SXML_STREAM_USAGE_NONE;
    }
    if (kind == SXML_AST_KIND_SQUARE_ARRAY_CONSTRUCTOR) {
        return field == SXML_AST_FIELD_MEMBER ? SXML_STREAM_USAGE_NAVIGATION : SXML_STREAM_USAGE_NONE;
    }
    if (kind == SXML_AST_KIND_CURLY_ARRAY_CONSTRUCTOR) {
        return field == SXML_AST_FIELD_CONTENT ? SXML_STREAM_USAGE_NAVIGATION : SXML_STREAM_USAGE_NONE;
    }
    return SXML_STREAM_USAGE_NONE;
}

/* 19.1, the static types of the kinds of SXML_STREAM_XPATH_RULE_GENERAL;
   TRANSMITTED for the union of the transmission operands. */
static result_type general_result_type(sxml_ast_kind kind) {
    if (kind == SXML_AST_KIND_SEQUENCE_EXPRESSION || kind == SXML_AST_KIND_OTHERWISE_EXPRESSION) {
        return SXML_STREAM_RESULT_TRANSMITTED;
    }
    if (kind == SXML_AST_KIND_OR_EXPRESSION || kind == SXML_AST_KIND_AND_EXPRESSION ||
        kind == SXML_AST_KIND_COMPARISON_EXPRESSION || kind == SXML_AST_KIND_CASTABLE_EXPRESSION) {
        return SXML_STREAM_RESULT_BOOLEAN;
    }
    if (kind == SXML_AST_KIND_STRING_CONCATENATION_EXPRESSION ||
        kind == SXML_AST_KIND_STRING_TEMPLATE) {
        return SXML_STREAM_RESULT_STRING;
    }
    if (kind == SXML_AST_KIND_RANGE_EXPRESSION || kind == SXML_AST_KIND_UNARY_EXPRESSION) {
        return SXML_STREAM_RESULT_NUMERIC;
    }
    if (kind == SXML_AST_KIND_MAP_CONSTRUCTOR_ENTRY ||
        kind == SXML_AST_KIND_SQUARE_ARRAY_CONSTRUCTOR ||
        kind == SXML_AST_KIND_CURLY_ARRAY_CONSTRUCTOR) {
        return SXML_STREAM_RESULT_FUNCTION;
    }
    return SXML_STREAM_RESULT_ATOMIC;
}

static u_type type_of_result(result_type type, u_type transmitted) {
    switch (type) {
    case SXML_STREAM_RESULT_ANY:
        return SXML_U_ANY;
    case SXML_STREAM_RESULT_ATOMIC:
        return SXML_U_ATOMIC;
    case SXML_STREAM_RESULT_BOOLEAN:
        return SXML_U_BOOLEAN;
    case SXML_STREAM_RESULT_STRING:
        return SXML_U_STRING;
    case SXML_STREAM_RESULT_NUMERIC:
        return SXML_U_NUMERIC;
    case SXML_STREAM_RESULT_NODES:
        return SXML_U_NODES;
    case SXML_STREAM_RESULT_FUNCTION:
        return SXML_U_FUNCTION;
    case SXML_STREAM_RESULT_TRANSMITTED:
        return transmitted;
    }
    return SXML_U_ANY;
}

/* Lists every child of node with a usage as an operand. */
static sxml_status add_child_operands(machine *state, uint32_t frame_index) {
    const frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const sxml_ast_node *parent = sxml_ast_get_node(ast, current->construct.ast_node);
    const sxml_xslt_ast_static_context_index static_context = current->construct.static_context;
    for (sxml_ast_node_index child = parent->first_child; child.value != 0;
         child = sxml_ast_get_node(ast, child)->next_sibling) {
        const usage child_usage = general_child_usage(parent, sxml_ast_get_node(ast, child)->field);
        if (child_usage == SXML_STREAM_USAGE_NONE) {
            continue;
        }
        const sxml_status status = add_operand(
            state, frame_index,
            plain_operand(xpath_construct(ast, child, static_context), child_usage));
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

static general_start return_general(result_type type) {
    return (general_start){
        .finish = {.combination = SXML_STREAM_COMBINATION_GENERAL, .is_at_most_one = false, .type = 0U},
        .after_phase = (uint32_t)type + 2000U};
}

/* Phases at or above 2000 finish a general combination with the result
   type phase - 2000 (a result_type). */
static bool finishes_general(frame *current) {
    if (current->phase < 2000U) {
        return false;
    }
    assessment result = current->general_result;
    result.type =
        type_of_result((result_type)(current->phase - 2000U), current->general.transmitted_type);
    finish_frame(current, result);
    return true;
}

/* The static type of a predicate (19.1: an arithmetic expression as a
   predicate is numeric). */
static u_type predicate_type(const sxml_ast *ast, sxml_ast_node_index expression, u_type type) {
    sxml_ast_node_index node = expression;
    while (sxml_ast_get_node(ast, node)->kind == SXML_AST_KIND_PARENTHESIZED_EXPRESSION) {
        node = child_by_field(ast, node, SXML_AST_FIELD_EXPRESSION);
    }
    const sxml_ast_kind kind = sxml_ast_get_node(ast, node)->kind;
    if (kind == SXML_AST_KIND_ADDITIVE_EXPRESSION || kind == SXML_AST_KIND_MULTIPLICATIVE_EXPRESSION ||
        kind == SXML_AST_KIND_UNARY_EXPRESSION) {
        return SXML_U_NUMERIC;
    }
    return type;
}

/* The streaming parameter's name of scope's function, if any. */
static resolved_name streaming_parameter_name(const machine *state, const scope *value) {
    resolved_name result = {.is_resolved = false,
                            .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_NONE),
                            .local_name = {.bytes = NULL, .byte_count = 0}};
    if (value->streaming_function.value == 0) {
        return result;
    }
    const sxml_xslt_ast_node_index parameter = parameter_at(
        state->ast, (parameter_query){.node = value->streaming_function, .position = 0});
    sxml_name_parts parts;
    memset(&parts, 0, sizeof(parts));
    if (parameter.value == 0 ||
        sxml_name_pool_get_name_parts(state->lookup.name_pool, name_attribute(state->ast, parameter),
                                      &parts) != SXML_STATUS_OK) {
        return result;
    }
    result.is_resolved = true;
    result.namespace_uri = parts.namespace_uri;
    result.local_name = parts.local_name;
    return result;
}

/* A predicate expression's scan for 19.8.8.9 rule 4 and 19.8.10. */
typedef struct predicate_scan {
    sxml_ast_node_index expression;
    bool skips_controlled_operands;
} predicate_scan;

static scan_result scan_predicate(const machine *state, const frame *current, predicate_scan scan) {
    return scan_expression(&state->lookup,
                           &(scan_request){.ast = current->construct.ast,
                                           .root = scan.expression,
                                           .static_context = current->construct.static_context,
                                           .skips_predicates = true,
                                           .skips_controlled_operands = scan.skips_controlled_operands,
                                           .streaming_parameter =
                                               streaming_parameter_name(state, &current->scope)});
}

static sxml_ast_node_index predicate_expression(const sxml_ast *ast, sxml_ast_node_index predicate) {
    return child_by_field(ast, predicate, SXML_AST_FIELD_EXPRESSION);
}

/* The next PREDICATE child of node after from (0: the first). */
static sxml_ast_node_index next_predicate(const sxml_ast *ast, sxml_ast_node_index node,
                                          sxml_ast_node_index from) {
    sxml_ast_node_index child = from.value == 0 ? sxml_ast_get_node(ast, node)->first_child
                                                : sxml_ast_get_node(ast, from)->next_sibling;
    while (child.value != 0 && sxml_ast_get_node(ast, child)->field != SXML_AST_FIELD_PREDICATE) {
        child = sxml_ast_get_node(ast, child)->next_sibling;
    }
    return child;
}

static operand focused_operand(construct value, focus given) {
    operand result = plain_operand(value, SXML_STREAM_USAGE_TRANSMISSION);
    result.flags = SXML_STREAM_OPERAND_FLAG_CHANGES_FOCUS;
    result.focus = given;
    return result;
}

static focus striding_focus(u_type type) {
    return (focus){.posture = SXML_STREAM_POSTURE_STRIDING, .type = type};
}

static focus focus_of(assessment value) {
    return (focus){.posture = value.posture, .type = value.type};
}

/* ---------------------------------------------------- XPath: the steps */

static assessment literal_assessment(sxml_ast_kind kind) {
    if (kind == SXML_AST_KIND_STRING_LITERAL || kind == SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART) {
        return grounded_motionless(SXML_U_STRING);
    }
    if (kind == SXML_AST_KIND_DOUBLE_LITERAL) {
        return grounded_motionless(SXML_U_DOUBLE);
    }
    if (kind == SXML_AST_KIND_BOOLEAN_CONSTANT) {
        return grounded_motionless(SXML_U_BOOLEAN);
    }
    if (kind == SXML_AST_KIND_QNAME_LITERAL) {
        return grounded_motionless(SXML_U_OTHER_ATOMIC);
    }
    return grounded_motionless(SXML_U_DECIMAL);
}

/* The XSLT element an expression AST belongs to; 0 for none. */
static sxml_xslt_ast_node_index owner_of_expression(const sxml_xslt_ast *ast, const sxml_ast *expression) {
    for (uint32_t index = 1; index < ast->expression_count; index += 1U) {
        const sxml_xslt_ast_expression *entry =
            sxml_xslt_ast_get_expression(ast, (sxml_xslt_ast_expression_index){.value = index});
        if (entry->ast == expression) {
            return entry->node;
        }
    }
    return no_xslt_node;
}

/* The declared type of the xsl:variable or xsl:param named name in
   scope at node (9.9: a preceding sibling of node or of an ancestor, else
   a global one); $value in an accumulator rule has the accumulator's
   type (18.2.4). U{*} when none is declared or found (19.1, VarRef). */
static u_type variable_type(const machine *state, const frame *current, resolved_name name) {
    const sxml_xslt_ast *ast = state->ast;
    sxml_xslt_ast_node_index node = owner_of_expression(ast, current->construct.ast);
    for (sxml_xslt_ast_node_index ancestor = node; ancestor.value != 0;
         ancestor = sxml_xslt_ast_get_node(ast, ancestor)->parent) {
        if (is_kind(ast, ancestor, SXML_XSLT_AST_KIND_ACCUMULATOR_RULE) &&
            name.namespace_uri.index == SXML_NAMESPACE_NONE && text_equals(name.local_name, "value")) {
            return declared_type_of_node(ast, sxml_xslt_ast_get_node(ast, ancestor)->parent).type;
        }
    }
    while (node.value != 0) {
        const sxml_xslt_ast_node_index parent = sxml_xslt_ast_get_node(ast, node)->parent;
        for (sxml_xslt_ast_node_index sibling =
                 parent.value == 0 ? no_xslt_node : sxml_xslt_ast_get_node(ast, parent)->first_child;
             sibling.value != 0 && sibling.value != node.value;
             sibling = sxml_xslt_ast_get_node(ast, sibling)->next_sibling) {
            if ((is_kind(ast, sibling, SXML_XSLT_AST_KIND_VARIABLE) ||
                 is_kind(ast, sibling, SXML_XSLT_AST_KIND_PARAM)) &&
                is_same_name(&state->lookup, name, name_attribute(ast, sibling))) {
                return declared_type_of_node(ast, sibling).type;
            }
        }
        node = parent;
    }
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if ((is_kind(ast, declaration, SXML_XSLT_AST_KIND_VARIABLE) ||
             is_kind(ast, declaration, SXML_XSLT_AST_KIND_PARAM)) &&
            is_same_name(&state->lookup, name, name_attribute(ast, declaration))) {
            return declared_type_of_node(ast, declaration).type;
        }
    }
    return SXML_U_ANY;
}

/* 19.8.8.12 with 19.8.5: a reference to the streaming parameter takes
   its posture and sweep from the function's category; every other
   variable reference is grounded and motionless. */
static assessment variable_assessment(const machine *state, const frame *current) {
    const resolved_name parameter = streaming_parameter_name(state, &current->scope);
    const sxml_ast *ast = current->construct.ast;
    const resolved_name name = resolve_name(
        &state->lookup,
        (name_to_resolve){.ast = ast,
                          .name = child_by_field(ast, current->construct.ast_node, SXML_AST_FIELD_NAME),
                          .static_context = current->construct.static_context,
                          .default_namespace = SXML_NAMESPACE_NONE});
    const resolved_name bound = current->scope.bound_variable;
    if (bound.is_resolved && name.is_resolved && name.namespace_uri.index == bound.namespace_uri.index &&
        sxml_byte_view_equals(name.local_name, bound.local_name)) {
        /* A variable bound to each streamed item in turn: as ".". */
        return make_assessment(current->scope.bound_value.posture, SXML_STREAM_SWEEP_MOTIONLESS,
                               current->scope.bound_value.type);
    }
    if (!parameter.is_resolved || !name.is_resolved ||
        name.namespace_uri.index != parameter.namespace_uri.index ||
        !sxml_byte_view_equals(name.local_name, parameter.local_name)) {
        return grounded_motionless(variable_type(state, current, name));
    }
    if (current->scope.is_in_higher_order) {
        return roaming_free_ranging(SXML_U_ANY);
    }
    const sxml_xslt_ast_node_index function = current->scope.streaming_function;
    const declared_type type = declared_type_of_node(
        state->ast, parameter_at(state->ast, (parameter_query){.node = function, .position = 0}));
    const sxml_xslt_ast_keyword category =
        keyword_attribute(state->ast, function, SXML_XSLT_AST_ATTRIBUTE_STREAMABILITY);
    if (category == SXML_XSLT_AST_KEYWORD_ABSORBING) {
        /* 19.8.5.2. */
        return make_assessment(SXML_STREAM_POSTURE_STRIDING,
                               type.allows_many ? SXML_STREAM_SWEEP_CONSUMING : SXML_STREAM_SWEEP_MOTIONLESS, type.type);
    }
    if (category == SXML_XSLT_AST_KEYWORD_ASCENT) {
        /* 19.8.5.7. */
        return make_assessment(SXML_STREAM_POSTURE_CLIMBING, SXML_STREAM_SWEEP_MOTIONLESS, type.type);
    }
    /* inspection, filter, shallow-descent, deep-descent. */
    return make_assessment(SXML_STREAM_POSTURE_STRIDING, SXML_STREAM_SWEEP_MOTIONLESS, type.type);
}

/* 19.8.8.9. phase 0 assesses the bare step; then each predicate in
   turn, in the focus the bare step gives (striding when that is
   roaming, so the step can still be judged as a pattern step). */
static sxml_status step_axis_step(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const sxml_ast_node_index node = current->construct.ast_node;
    const axis_class axis =
        current->is_descendant_step ? SXML_STREAM_AXIS_CLASS_DESCENDANT : axis_of_step(ast, node);
    if (current->phase == 0U) {
        const assessment base = assess_bare_step(
            (bare_step){.focus = current->focus,
                        .axis = axis,
                        .test_kinds = node_test_kinds(
                            ast, child_by_field(ast, node, SXML_AST_FIELD_TEST), axis)});
        const sxml_ast_node_index first = next_predicate(ast, node, no_ast_node);
        if (first.value == 0 || current->focus.posture == SXML_STREAM_POSTURE_GROUNDED ||
            current->focus.posture == SXML_STREAM_POSTURE_ROAMING || base.type == 0U) {
            assessment result = base;
            result.is_pattern_step = first.value == 0 && is_pattern_axis(axis) &&
                                     current->focus.posture != SXML_STREAM_POSTURE_ROAMING;
            finish_frame(current, result);
            return SXML_STATUS_OK;
        }
        current->saved = base;
        current->second = grounded_motionless(0U);
        current->flag = true;
        current->counter = 0U;
        current->cursor = first.value;
        current->phase = 1U;
        const focus predicate_focus = {.posture = base.posture == SXML_STREAM_POSTURE_ROAMING ? SXML_STREAM_POSTURE_STRIDING
                                                                                  : base.posture,
                                       .type = base.type};
        return push_operand(
            state, frame_index,
            focused_operand(xpath_construct(ast, predicate_expression(ast, first),
                                            current->construct.static_context),
                            predicate_focus));
    }
    assessment child;
    if (!take_child(current, &child)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_ast_node_index predicate = {.value = current->cursor};
    const sxml_ast_node_index expression = predicate_expression(ast, predicate);
    if (child.sweep != SXML_STREAM_SWEEP_MOTIONLESS || is_free(child)) {
        current->flag = false;
    }
    current->second.is_uncertain = current->second.is_uncertain || child.is_uncertain;
    const u_type type = predicate_type(ast, expression, child.type);
    const scan_result focus_scan = scan_predicate(state, current, (predicate_scan){.expression = expression, .skips_controlled_operands = true});
    const scan_result positional_scan = scan_predicate(state, current, (predicate_scan){.expression = expression, .skips_controlled_operands = false});
    if (type != 0U && is_subtype(type, SXML_U_NUMERIC) && !focus_scan.depends_on_focus) {
        current->counter |= 1U;
    }
    if ((type & SXML_U_NUMERIC) != 0U || positional_scan.has_positional_call) {
        current->counter |= 2U;
    }
    const sxml_ast_node_index next = next_predicate(ast, node, predicate);
    if (next.value != 0) {
        current->cursor = next.value;
        const assessment base = current->saved;
        const focus predicate_focus = {.posture = base.posture == SXML_STREAM_POSTURE_ROAMING ? SXML_STREAM_POSTURE_STRIDING
                                                                                  : base.posture,
                                       .type = base.type};
        return push_operand(
            state, frame_index,
            focused_operand(xpath_construct(ast, predicate_expression(ast, next),
                                            current->construct.static_context),
                            predicate_focus));
    }
    const assessment base = current->saved;
    assessment result = base;
    /* Rule 4: a numeric predicate that does not depend on the focus
       selects one node. */
    const bool selects_one = current->focus.posture == SXML_STREAM_POSTURE_STRIDING &&
                             (axis == SXML_STREAM_AXIS_CLASS_DESCENDANT ||
                              axis == SXML_STREAM_AXIS_CLASS_DESCENDANT_OR_SELF) &&
                             (current->counter & 1U) != 0U;
    if (base.posture != SXML_STREAM_POSTURE_ROAMING && selects_one) {
        result = make_assessment(SXML_STREAM_POSTURE_STRIDING, SXML_STREAM_SWEEP_CONSUMING, base.type);
    } else if (base.posture == SXML_STREAM_POSTURE_ROAMING || !current->flag) {
        result = roaming_free_ranging(base.type);
    }
    result.is_pattern_step = is_pattern_axis(axis) && current->flag && (current->counter & 2U) == 0U;
    result.is_uncertain = current->second.is_uncertain;
    finish_frame(current, result);
    return SXML_STATUS_OK;
}

/* Lists the components of a path from left to right (19.8.8.8): "/"
   and "//" expand to root(.) treat as document-node() and
   descendant-or-self::node(), and a/b/c is (a/b)/c. Components are
   added right to left, then reversed. */
static sxml_status list_path_components(machine *state, uint32_t frame_index) {
    const frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const sxml_xslt_ast_static_context_index static_context = current->construct.static_context;
    const uint32_t first = state->operand_count;
    sxml_ast_node_index node = current->construct.ast_node;
    bool has_root = false;
    uint32_t leftmost_flags = 0U;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && node.value != 0) {
        const sxml_ast_node *entry = sxml_ast_get_node(ast, node);
        if (entry->kind == SXML_AST_KIND_RELATIVE_PATH_EXPRESSION) {
            operand component = plain_operand(
                xpath_construct(ast, child_by_field(ast, node, SXML_AST_FIELD_RIGHT), static_context),
                SXML_STREAM_USAGE_TRANSMISSION);
            component.flags = entry->detail.operator == SXML_AST_OPERATOR_DOUBLE_SLASH
                                  ? SXML_STREAM_OPERAND_FLAG_AFTER_DOUBLE_SLASH
                                  : 0U;
            status = add_operand(state, frame_index, component);
            node = child_by_field(ast, node, SXML_AST_FIELD_LEFT);
            continue;
        }
        if (entry->kind == SXML_AST_KIND_ABSOLUTE_PATH_EXPRESSION && !has_root) {
            has_root = true;
            leftmost_flags = entry->detail.operator == SXML_AST_OPERATOR_DOUBLE_SLASH
                                 ? SXML_STREAM_OPERAND_FLAG_AFTER_DOUBLE_SLASH
                                 : 0U;
            node = child_by_field(ast, node, SXML_AST_FIELD_PATH);
            continue;
        }
        operand component =
            plain_operand(xpath_construct(ast, node, static_context), SXML_STREAM_USAGE_TRANSMISSION);
        component.flags = leftmost_flags;
        status = add_operand(state, frame_index, component);
        break;
    }
    if (status == SXML_STATUS_OK && has_root) {
        operand root = fixed_operand(grounded_motionless(SXML_U_DOCUMENT), SXML_STREAM_USAGE_TRANSMISSION);
        root.flags = SXML_STREAM_OPERAND_FLAG_ROOT;
        status = add_operand(state, frame_index, root);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (uint32_t lower_index = first, upper_index = state->operand_count; lower_index + 1U < upper_index;
         lower_index += 1U, upper_index -= 1U) {
        const operand swapped = state->operands[lower_index];
        state->operands[lower_index] = state->operands[upper_index - 1U];
        state->operands[upper_index - 1U] = swapped;
    }
    return SXML_STATUS_OK;
}

/* The call node and the arrow's left operand of a FUNCTION_CALL or
   ARROW_EXPRESSION frame (19.8.8.14: X => F(Y) is F(X, Y)). */
typedef struct call_shape {
    sxml_ast_node_index call;
    sxml_ast_node_index arrow_value;
    sxml_ast_node_index arguments;
    uint32_t arity;
    bool has_placeholder;
    bool has_keyword_argument;
} call_shape;

static call_shape shape_of_call(const sxml_ast *ast, sxml_ast_node_index node) {
    call_shape shape = {.call = node,
                        .arrow_value = no_ast_node,
                        .arguments = no_ast_node,
                        .arity = 0,
                        .has_placeholder = false,
                        .has_keyword_argument = false};
    if (sxml_ast_get_node(ast, node)->kind == SXML_AST_KIND_ARROW_EXPRESSION) {
        shape.arrow_value = child_by_field(ast, node, SXML_AST_FIELD_VALUE);
        shape.call = child_by_field(ast, node, SXML_AST_FIELD_TARGET);
        shape.arity = 1U;
    }
    shape.arguments = child_by_field(ast, shape.call, SXML_AST_FIELD_ARGUMENTS);
    for (sxml_ast_node_index argument = sxml_ast_get_node(ast, shape.arguments)->first_child;
         argument.value != 0; argument = sxml_ast_get_node(ast, argument)->next_sibling) {
        const sxml_ast_node *entry = sxml_ast_get_node(ast, argument);
        if (entry->kind == SXML_AST_KIND_KEYWORD_ARGUMENT) {
            shape.has_keyword_argument = true;
            shape.arity += 1U;
        } else if (entry->field == SXML_AST_FIELD_ARGUMENT) {
            shape.has_placeholder =
                shape.has_placeholder || entry->kind == SXML_AST_KIND_ARGUMENT_PLACEHOLDER;
            shape.arity += 1U;
        }
    }
    return shape;
}

/* A predicate whose value is never numeric (its expression a
   comparison, a boolean or string function, a path...) and that calls
   no position(), last() or function-lookup(): a non-positional predicate
   by its syntax alone. */
static bool is_plainly_non_positional(const machine *state, const frame *current,
                                      sxml_ast_node_index expression) {
    const sxml_ast *ast = current->construct.ast;
    sxml_ast_node_index node = expression;
    while (sxml_ast_get_node(ast, node)->kind == SXML_AST_KIND_PARENTHESIZED_EXPRESSION) {
        node = child_by_field(ast, node, SXML_AST_FIELD_EXPRESSION);
    }
    const sxml_ast_kind kind = sxml_ast_get_node(ast, node)->kind;
    bool is_non_numeric =
        kind == SXML_AST_KIND_COMPARISON_EXPRESSION || kind == SXML_AST_KIND_OR_EXPRESSION ||
        kind == SXML_AST_KIND_AND_EXPRESSION || kind == SXML_AST_KIND_AXIS_STEP ||
        kind == SXML_AST_KIND_RELATIVE_PATH_EXPRESSION ||
        kind == SXML_AST_KIND_ABSOLUTE_PATH_EXPRESSION || kind == SXML_AST_KIND_STRING_LITERAL ||
        kind == SXML_AST_KIND_QUANTIFIED_EXPRESSION || kind == SXML_AST_KIND_INSTANCE_OF_EXPRESSION ||
        kind == SXML_AST_KIND_CASTABLE_EXPRESSION || kind == SXML_AST_KIND_BOOLEAN_CONSTANT;
    if (kind == SXML_AST_KIND_FUNCTION_CALL) {
        const call_shape shape = shape_of_call(ast, node);
        const built_in *entry = find_built_in(
            resolve_name(&state->lookup,
                         (name_to_resolve){.ast = ast,
                                           .name = child_by_field(ast, node, SXML_AST_FIELD_NAME),
                                           .static_context = current->construct.static_context,
                                           .default_namespace = SXML_NAMESPACE_FUNCTIONS}),
            shape.arity);
        is_non_numeric = entry != NULL && (entry->result == SXML_STREAM_RESULT_BOOLEAN ||
                                           entry->result == SXML_STREAM_RESULT_STRING ||
                                           entry->result == SXML_STREAM_RESULT_NODES);
    }
    return is_non_numeric && !scan_predicate(state, current, (predicate_scan){.expression = expression, .skips_controlled_operands = false}).has_positional_call;
}

/* An axis step on the child axis whose predicates are plainly
   non-positional. */
static bool is_descendant_rewritable(const machine *state, const frame *current,
                                     sxml_ast_node_index step) {
    const sxml_ast *ast = current->construct.ast;
    if (sxml_ast_get_node(ast, step)->kind != SXML_AST_KIND_AXIS_STEP ||
        axis_of_step(ast, step) != SXML_STREAM_AXIS_CLASS_CHILD) {
        return false;
    }
    for (sxml_ast_node_index predicate = next_predicate(ast, step, no_ast_node); predicate.value != 0;
         predicate = next_predicate(ast, step, predicate)) {
        if (!is_plainly_non_positional(state, current, predicate_expression(ast, predicate))) {
            return false;
        }
    }
    return true;
}

/* Composes the path so far (saved) with the next component's
   assessment: the right posture, the wider sweep, and the scanning
   expression rule when the posture is roaming (19.8.8.8). */
static void compose_path(frame *current, assessment right) {
    const assessment left = current->saved;
    current->flag = current->flag && right.is_pattern_step;
    assessment composed = make_assessment(right.posture, wider_sweep(left.sweep, right.sweep),
                                          right.type);
    if (composed.posture == SXML_STREAM_POSTURE_ROAMING && current->flag && !is_free(left)) {
        composed.posture = (right.type & SXML_U_ELEMENT) != 0U ? SXML_STREAM_POSTURE_CRAWLING : SXML_STREAM_POSTURE_STRIDING;
        composed.sweep = SXML_STREAM_SWEEP_CONSUMING;
    }
    composed = normalized(composed);
    composed.is_uncertain = left.is_uncertain || right.is_uncertain;
    current->saved = composed;
}

static sxml_status step_path(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    if (current->phase == 0U) {
        begin_operands(state, frame_index);
        const sxml_status status = list_path_components(state, frame_index);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        current = &state->frames[frame_index];
        current->cursor = 0U;
        current->flag = true;
        current->phase = 1U;
    }
    assessment child;
    if (take_child(current, &child)) {
        if (current->cursor == 1U) {
            current->saved = child;
            current->flag = child.is_pattern_step;
        } else {
            compose_path(current, child);
        }
    }
    while (current->cursor < current->operand_count) {
        const operand component = state->operands[current->operand_first + current->cursor];
        const bool is_first = current->cursor == 0U;
        current->cursor += 1U;
        if ((component.flags & SXML_STREAM_OPERAND_FLAG_ROOT) != 0U) {
            current->saved = assess_root(context_item(current->focus));
            current->flag = true;
            continue;
        }
        if (is_first) {
            return push_operand(state, frame_index,
                                plain_operand(component.construct, SXML_STREAM_USAGE_TRANSMISSION));
        }
        if ((component.flags & SXML_STREAM_OPERAND_FLAG_AFTER_DOUBLE_SLASH) != 0U &&
            is_descendant_rewritable(state, current, component.construct.ast_node)) {
            /* E//child::T[P] is E/descendant::T[P] when no predicate is
               positional (XPath 4.0, 4.6.7: the same nodes), the rewrite
               that makes .//T crawling rather than roaming. */
            const sxml_status status = push_operand(
                state, frame_index, focused_operand(component.construct, focus_of(current->saved)));
            if (status == SXML_STATUS_OK) {
                state->frames[state->frame_count - 1U].is_descendant_step = true;
            }
            return status;
        }
        if ((component.flags & SXML_STREAM_OPERAND_FLAG_AFTER_DOUBLE_SLASH) != 0U) {
            const assessment step = assess_bare_step((bare_step){
                .focus = focus_of(current->saved),
                .axis = SXML_STREAM_AXIS_CLASS_DESCENDANT_OR_SELF,
                .test_kinds = SXML_U_NODES});
            assessment pattern_step = step;
            pattern_step.is_pattern_step = true;
            compose_path(current, pattern_step);
        }
        return push_operand(state, frame_index,
                            focused_operand(component.construct, focus_of(current->saved)));
    }
    assessment result = current->saved;
    result.is_pattern_step = false;
    finish_frame(current, result);
    return SXML_STATUS_OK;
}

/* 19.8.8.7. */
static sxml_status step_simple_map(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const sxml_ast_node_index node = current->construct.ast_node;
    assessment child;
    if (current->phase == 0U) {
        current->phase = 1U;
        return push_operand(state, frame_index,
                            plain_operand(xpath_construct(ast, child_by_field(ast, node, SXML_AST_FIELD_LEFT),
                                                          current->construct.static_context),
                                          SXML_STREAM_USAGE_TRANSMISSION));
    }
    if (!take_child(current, &child)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (current->phase == 1U) {
        current->saved = child;
        current->phase = 2U;
        return push_operand(
            state, frame_index,
            focused_operand(xpath_construct(ast, child_by_field(ast, node, SXML_AST_FIELD_RIGHT),
                                            current->construct.static_context),
                            focus_of(child)));
    }
    assessment result =
        normalized(make_assessment(child.posture, wider_sweep(current->saved.sweep, child.sweep),
                                   child.type));
    result.is_uncertain = child.is_uncertain || current->saved.is_uncertain;
    finish_frame(current, result);
    return SXML_STATUS_OK;
}

/* 19.8.8.10. */
static sxml_status step_filter(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const sxml_ast_node_index node = current->construct.ast_node;
    const sxml_ast_node_index expression =
        predicate_expression(ast, child_by_field(ast, node, SXML_AST_FIELD_PREDICATE));
    assessment child;
    if (current->phase == 0U) {
        current->phase = 1U;
        return push_operand(state, frame_index,
                            plain_operand(xpath_construct(ast, child_by_field(ast, node, SXML_AST_FIELD_VALUE),
                                                          current->construct.static_context),
                                          SXML_STREAM_USAGE_TRANSMISSION));
    }
    if (!take_child(current, &child)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (current->phase == 1U) {
        current->saved = child;
        current->phase = 2U;
        return push_operand(state, frame_index,
                            focused_operand(xpath_construct(ast, expression,
                                                            current->construct.static_context),
                                            focus_of(child)));
    }
    const assessment base = current->saved;
    const u_type type = predicate_type(ast, expression, child.type);
    const scan_result focus_scan = scan_predicate(state, current, (predicate_scan){.expression = expression, .skips_controlled_operands = true});
    assessment result = roaming_free_ranging(base.type);
    if (base.posture == SXML_STREAM_POSTURE_CRAWLING && type != 0U && is_subtype(type, SXML_U_NUMERIC) &&
        !focus_scan.depends_on_focus) {
        result = make_assessment(SXML_STREAM_POSTURE_STRIDING, base.sweep, base.type);
    } else if (child.sweep == SXML_STREAM_SWEEP_MOTIONLESS && !is_free(child)) {
        result = base;
    }
    result.is_pattern_step = false;
    result.is_uncertain = base.is_uncertain || child.is_uncertain;
    finish_frame(current, result);
    return SXML_STATUS_OK;
}

/* 19.8.8.4. */
static sxml_status step_union(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const sxml_ast_node_index node = current->construct.ast_node;
    assessment child;
    if (current->phase == 0U || current->phase == 1U) {
        if (current->phase == 1U) {
            if (!take_child(current, &child)) {
                return SXML_STATUS_INVALID_ARGUMENT;
            }
            current->saved = child;
        }
        const sxml_ast_field field = current->phase == 0U ? SXML_AST_FIELD_LEFT : SXML_AST_FIELD_RIGHT;
        current->phase += 1U;
        return push_operand(state, frame_index,
                            plain_operand(xpath_construct(ast, child_by_field(ast, node, field),
                                                          current->construct.static_context),
                                          SXML_STREAM_USAGE_TRANSMISSION));
    }
    if (!take_child(current, &child)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const assessment left = current->saved;
    const assessment right = child;
    const sxml_ast_operator operator_kind = sxml_ast_get_node(ast, node)->detail.operator;
    const u_type type = operator_kind == SXML_AST_OPERATOR_INTERSECT ? left.type & right.type
                        : operator_kind == SXML_AST_OPERATOR_EXCEPT ? left.type
                                                                     : left.type | right.type;
    const bool is_left_down = left.posture == SXML_STREAM_POSTURE_STRIDING || left.posture == SXML_STREAM_POSTURE_CRAWLING;
    const bool is_right_down = right.posture == SXML_STREAM_POSTURE_STRIDING || right.posture == SXML_STREAM_POSTURE_CRAWLING;
    assessment result = roaming_free_ranging(type);
    if (is_free(left) || is_free(right)) {
        /* Rule 1. */
    } else if (left.posture == SXML_STREAM_POSTURE_GROUNDED && left.sweep == SXML_STREAM_SWEEP_MOTIONLESS) {
        result = make_assessment(right.posture, right.sweep, type);
    } else if ((right.posture == SXML_STREAM_POSTURE_GROUNDED &&
                right.sweep == SXML_STREAM_SWEEP_MOTIONLESS) ||
               (operator_kind != SXML_AST_OPERATOR_UNION && is_motionless(right))) {
        /* Rule 2; and E intersect F and E except F select among E's
           nodes, so with F motionless they have E's posture (sxml
           relaxes the last rule of 19.8.8.4 here, as the XT40 case
           si-group-055 expects). */
        result = make_assessment(left.posture, left.sweep, type);
    } else if (left.posture == SXML_STREAM_POSTURE_CLIMBING && right.posture == SXML_STREAM_POSTURE_CLIMBING) {
        result = make_assessment(SXML_STREAM_POSTURE_CLIMBING, wider_sweep(left.sweep, right.sweep), type);
    } else if (is_left_down && is_right_down) {
        result = make_assessment(SXML_STREAM_POSTURE_CRAWLING, wider_sweep(left.sweep, right.sweep), type);
    }
    result.is_uncertain = left.is_uncertain || right.is_uncertain;
    finish_frame(current, result);
    return SXML_STATUS_OK;
}

/* The position-th argument (from 0) of a call, the arrow's left operand
   first; 0 for a placeholder or none. */
static sxml_ast_node_index argument_at(const sxml_ast *ast, const call_shape *shape,
                                       uint32_t position) {
    uint32_t index = 0;
    if (shape->arrow_value.value != 0) {
        if (position == 0U) {
            return shape->arrow_value;
        }
        index = 1U;
    }
    for (sxml_ast_node_index argument = sxml_ast_get_node(ast, shape->arguments)->first_child;
         argument.value != 0; argument = sxml_ast_get_node(ast, argument)->next_sibling) {
        const sxml_ast_node *entry = sxml_ast_get_node(ast, argument);
        if (entry->field != SXML_AST_FIELD_ARGUMENT && entry->kind != SXML_AST_KIND_KEYWORD_ARGUMENT) {
            continue;
        }
        if (index == position) {
            return entry->kind == SXML_AST_KIND_ARGUMENT_PLACEHOLDER ||
                           entry->kind == SXML_AST_KIND_KEYWORD_ARGUMENT
                       ? no_ast_node
                       : argument;
        }
        index += 1U;
    }
    return no_ast_node;
}

/* The declared type of the position-th parameter of an inline function
   expression with a signature; undeclared otherwise. */
static declared_type inline_parameter_type(const sxml_ast *ast, sxml_ast_node_index function,
                                           uint32_t position) {
    const sxml_ast_node *entry = sxml_ast_get_node(ast, function);
    if (entry->kind != SXML_AST_KIND_INLINE_FUNCTION_EXPRESSION ||
        (entry->flags & SXML_AST_FLAG_HAS_SIGNATURE) == 0U) {
        return undeclared_type();
    }
    uint32_t index = 0;
    for (sxml_ast_node_index parameter = entry->first_child; parameter.value != 0;
         parameter = sxml_ast_get_node(ast, parameter)->next_sibling) {
        if (sxml_ast_get_node(ast, parameter)->field != SXML_AST_FIELD_PARAMETER) {
            continue;
        }
        if (index == position) {
            const sxml_ast_node_index declaration = child_by_field(ast, parameter, SXML_AST_FIELD_TYPE);
            if (declaration.value == 0) {
                return undeclared_type();
            }
            return sequence_type_of(ast, child_by_field(ast, declaration, SXML_AST_FIELD_TYPE));
        }
        index += 1U;
    }
    return undeclared_type();
}

/* Phases of FUNCTION_CALL frames. */
enum {
    SXML_STREAM_CALL_PHASE_START = 0,
    /* A one-argument special function's argument is assessed. */
    SXML_STREAM_CALL_PHASE_SPECIAL_ARGUMENT = 1,
    /* A stylesheet function's first argument is assessed. */
    SXML_STREAM_CALL_PHASE_FIRST_ARGUMENT = 2,
    /* The general combination of a stylesheet function call is done. */
    SXML_STREAM_CALL_PHASE_STYLESHEET_COMBINED = 3,
    /* The general combination of fn:outermost is done. */
    SXML_STREAM_CALL_PHASE_OUTERMOST_COMBINED = 4,
    /* A call sxml cannot classify is combined. */
    SXML_STREAM_CALL_PHASE_UNKNOWN_COMBINED = 5,
};

/* Lists the arguments of a call with the usages of usages (letters) or
   usage_of_others, and an implicit operand. */
typedef struct call_operands {
    const call_shape *shape;
    const char *usages;
    usage usage_of_others;
    uint32_t first_position;
    uint32_t operand_flags;
} call_operands;

static sxml_status add_call_operands(machine *state, uint32_t frame_index,
                                     const call_operands *request) {
    const frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    for (uint32_t position = request->first_position; position < request->shape->arity;
         position += 1U) {
        const sxml_ast_node_index argument = argument_at(ast, request->shape, position);
        if (argument.value == 0) {
            continue;
        }
        const usage argument_usage =
            request->usages != NULL ? usage_of_letter(request->usages[position])
                                    : request->usage_of_others;
        operand value = plain_operand(
            xpath_construct(ast, argument, current->construct.static_context), argument_usage);
        value.flags = request->operand_flags;
        const sxml_status status = add_operand(state, frame_index, value);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* 19.8.9.1, the sweep of accumulator-after with the assessed name
   argument, rule by rule. */
static sweep accumulator_after_sweep(const frame *current, assessment name) {
    if (name.sweep != SXML_STREAM_SWEEP_MOTIONLESS || is_free(name)) {
        return SXML_STREAM_SWEEP_FREE_RANGING;
    }
    if (current->focus.posture == SXML_STREAM_POSTURE_GROUNDED ||
        (current->focus.type & SXML_U_PARENTS) == 0U) {
        return SXML_STREAM_SWEEP_MOTIONLESS;
    }
    if (current->scope.accumulator_phase == SXML_XSLT_AST_KEYWORD_START) {
        return SXML_STREAM_SWEEP_FREE_RANGING;
    }
    if (current->scope.accumulator_phase == SXML_XSLT_AST_KEYWORD_END) {
        return SXML_STREAM_SWEEP_MOTIONLESS;
    }
    if (!current->scope.is_in_sequence_constructor || current->scope.has_changed_focus) {
        return SXML_STREAM_SWEEP_FREE_RANGING;
    }
    return current->scope.follows_consuming ? SXML_STREAM_SWEEP_MOTIONLESS
                                            : SXML_STREAM_SWEEP_CONSUMING;
}

static assessment accumulator_after_assessment(const frame *current, assessment name) {
    assessment result = normalized(make_assessment(
        SXML_STREAM_POSTURE_GROUNDED, accumulator_after_sweep(current, name), SXML_U_ANY));
    result.is_uncertain = name.is_uncertain;
    return result;
}

/* Special functions of 19.8.9 that take no operand assessment: the
   assessment, or false when the function is not one. */
typedef struct focus_call {
    const frame *frame;
    resolved_name name;
    uint32_t arity;
} focus_call;

static bool assess_focus_function(machine *state, focus_call call, assessment *result) {
    const frame *current = call.frame;
    const resolved_name name = call.name;
    const uint32_t arity = call.arity;
    if (is_fn_function(name, "position") || is_fn_function(name, "current-grouping-key") ||
        is_fn_function(name, "current-merge-key")) {
        /* 19.8.9.16, 19.8.9.5, 19.8.9.7. */
        *result = grounded_motionless(is_fn_function(name, "position") ? SXML_U_NUMERIC : SXML_U_ATOMIC);
        return true;
    }
    if (is_fn_function(name, "last") && arity == 0U) {
        /* 19.8.9.14. */
        const posture context = current->focus.posture;
        *result = context == SXML_STREAM_POSTURE_STRIDING || context == SXML_STREAM_POSTURE_CRAWLING ||
                          context == SXML_STREAM_POSTURE_ROAMING
                      ? roaming_free_ranging(SXML_U_NUMERIC)
                      : grounded_motionless(SXML_U_NUMERIC);
        return true;
    }
    if (is_fn_function(name, "current") && arity == 0U) {
        /* 19.8.9.3. */
        const focus outer = current->scope.expression_focus;
        if (!current->scope.is_in_pattern && outer.posture == SXML_STREAM_POSTURE_GROUNDED) {
            *result = grounded_motionless(outer.type);
        } else if (current->scope.is_in_pattern || current->scope.is_below_higher_order) {
            *result = make_assessment(SXML_STREAM_POSTURE_CLIMBING, SXML_STREAM_SWEEP_MOTIONLESS, outer.type);
        } else {
            *result = normalized(make_assessment(outer.posture, SXML_STREAM_SWEEP_MOTIONLESS, outer.type));
        }
        return true;
    }
    if (is_fn_function(name, "current-group") && arity == 0U) {
        /* 19.8.9.4; 19.8.4.37 for one inside an xsl:source-document. */
        if (current->scope.group_state == SXML_STREAM_GROUP_STATE_ACTIVE &&
            (current->scope.is_group_path_clean ||
             is_grounded_motionless(state->frames[current->scope.group_frame].saved))) {
            *result = state->frames[current->scope.group_frame].saved;
            result->is_pattern_step = false;
        } else if (current->scope.group_state == SXML_STREAM_GROUP_STATE_OTHER_DOCUMENT) {
            *result = grounded_motionless(SXML_U_ANY);
        } else {
            if (current->scope.group_state == SXML_STREAM_GROUP_STATE_POISONED) {
                state->frames[current->scope.poison_frame].is_poisoned = true;
            }
            *result = roaming_free_ranging(SXML_U_ANY);
        }
        return true;
    }
    if (is_fn_function(name, "current-merge-group")) {
        /* 19.8.9.6; 19.8.4.37. */
        if (current->scope.is_merge_poisoned && current->scope.poison_frame != 0U) {
            state->frames[current->scope.poison_frame].is_poisoned = true;
            *result = roaming_free_ranging(SXML_U_ANY);
        } else {
            *result = grounded_motionless(SXML_U_ANY);
        }
        return true;
    }
    if (is_fn_function(name, "root") && arity == 0U) {
        /* 19.8.9.18. */
        *result = assess_root(context_item(current->focus));
        result->is_pattern_step = false;
        return true;
    }
    return false;
}

/* The usages of fn:for-each, fn:filter, fn:fold-left and
   fn:for-each-pair refined by the function argument's signature
   (19.8.9.8, 19.8.9.10, 19.8.9.11): the sequence arguments'
   type-determined usages when the function is an inline function with
   declared parameter types; else navigation, which sxml cannot refine,
   so such an argument is uncertain unless grounded. */
typedef struct named_call {
    const call_shape *shape;
    resolved_name name;
    sxml_xslt_ast_node_index function;
} named_call;

static sxml_status add_higher_order_call_operands(machine *state, uint32_t frame_index,
                                                  named_call call) {
    const call_shape *shape = call.shape;
    const resolved_name name = call.name;
    const frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const bool is_fold = is_fn_function(name, "fold-left");
    const bool is_pair = is_fn_function(name, "for-each-pair");
    const uint32_t function_position = is_fold || is_pair ? 2U : 1U;
    const sxml_ast_node_index function = argument_at(ast, shape, function_position);
    for (uint32_t position = 0; position < shape->arity; position += 1U) {
        const sxml_ast_node_index argument = argument_at(ast, shape, position);
        if (argument.value == 0) {
            continue;
        }
        usage argument_usage = SXML_STREAM_USAGE_INSPECTION;
        uint32_t flags = 0;
        const bool is_sequence = position == 0U || (is_pair && position == 1U);
        if (is_fold && position == 1U) {
            argument_usage = SXML_STREAM_USAGE_ABSORPTION;
        } else if (is_sequence) {
            const uint32_t parameter = is_fold ? 1U : position;
            const declared_type type = inline_parameter_type(ast, function, parameter);
            argument_usage = type_determined_usage(type);
            if (!type.is_declared) {
                flags = SXML_STREAM_OPERAND_FLAG_UNCERTAIN_UNLESS_GROUNDED;
            }
            if (is_fn_function(name, "filter") && argument_usage == SXML_STREAM_USAGE_NAVIGATION) {
                /* 19.8.9 table: fn:filter(N, I). */
                argument_usage = SXML_STREAM_USAGE_NAVIGATION;
            }
        }
        operand value = plain_operand(
            xpath_construct(ast, argument, current->construct.static_context), argument_usage);
        value.flags = flags;
        const sxml_status status = add_operand(state, frame_index, value);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

static bool is_higher_order_built_in(resolved_name name, uint32_t arity) {
    return (arity == 2U && (is_fn_function(name, "for-each") || is_fn_function(name, "filter"))) ||
           (arity == 3U && (is_fn_function(name, "fold-left") || is_fn_function(name, "for-each-pair")));
}

/* Starts a stylesheet function call (19.8.5): the first argument is
   assessed on its own, since several categories look at it. */
static sxml_status start_stylesheet_call(machine *state, uint32_t frame_index, named_call call) {
    const call_shape *shape = call.shape;
    const sxml_xslt_ast_node_index function = call.function;
    frame *current = &state->frames[frame_index];
    current->auxiliary_node = function;
    const declared_type declared_result = declared_type_of_node(state->ast, function);
    if (shape->arity == 0U) {
        /* 19.8.5: calls to zero-arity functions are grounded and
           motionless. */
        finish_frame(current, grounded_motionless(declared_result.type));
        return SXML_STATUS_OK;
    }
    const sxml_ast_node_index first = argument_at(current->construct.ast, shape, 0U);
    if (first.value == 0) {
        /* A partial application with the first argument a placeholder
           (19.8.8.14): the others as for an unclassified function. */
        current->saved = grounded_motionless(0U);
        current->phase = SXML_STREAM_CALL_PHASE_FIRST_ARGUMENT;
        current->has_child = true;
        current->child = grounded_motionless(0U);
        return SXML_STATUS_OK;
    }
    current->phase = SXML_STREAM_CALL_PHASE_FIRST_ARGUMENT;
    return push_operand(state, frame_index,
                        plain_operand(xpath_construct(current->construct.ast, first,
                                                      current->construct.static_context),
                                      SXML_STREAM_USAGE_TRANSMISSION));
}

/* The type-determined usage of a stylesheet function's parameter. */
static usage parameter_usage(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index function,
                             uint32_t position) {
    return type_determined_usage(declared_type_of_node(
        ast, parameter_at(ast, (parameter_query){.node = function, .position = position})));
}

static sxml_xslt_ast_keyword function_category(const sxml_xslt_ast *ast,
                                               sxml_xslt_ast_node_index function) {
    const sxml_xslt_ast_keyword category =
        keyword_attribute(ast, function, SXML_XSLT_AST_ATTRIBUTE_STREAMABILITY);
    return category == SXML_XSLT_AST_KEYWORD_NONE ? SXML_XSLT_AST_KEYWORD_UNCLASSIFIED : category;
}

/* With the first argument assessed: lists it (except for the descent
   categories) and the others with their usages, and combines them. */
static sxml_status continue_stylesheet_call(machine *state, uint32_t frame_index,
                                            const call_shape *shape) {
    frame *current = &state->frames[frame_index];
    const sxml_xslt_ast_node_index function = current->auxiliary_node;
    const sxml_xslt_ast_keyword category = function_category(state->ast, function);
    begin_operands(state, frame_index);
    sxml_status status = SXML_STATUS_OK;
    if (category != SXML_XSLT_AST_KEYWORD_SHALLOW_DESCENT &&
        category != SXML_XSLT_AST_KEYWORD_DEEP_DESCENT) {
        usage first_usage = parameter_usage(state->ast, function, 0U);
        if (category == SXML_XSLT_AST_KEYWORD_ABSORBING) {
            first_usage = SXML_STREAM_USAGE_ABSORPTION;
        } else if (category == SXML_XSLT_AST_KEYWORD_INSPECTION ||
                   category == SXML_XSLT_AST_KEYWORD_ASCENT) {
            first_usage = SXML_STREAM_USAGE_INSPECTION;
        } else if (category == SXML_XSLT_AST_KEYWORD_FILTER) {
            first_usage = SXML_STREAM_USAGE_TRANSMISSION;
        }
        status = add_operand(state, frame_index, fixed_operand(current->saved, first_usage));
    }
    for (uint32_t position = 1; status == SXML_STATUS_OK && position < shape->arity;
         position += 1U) {
        const sxml_ast_node_index argument = argument_at(state->frames[frame_index].construct.ast,
                                                         shape, position);
        if (argument.value == 0) {
            continue;
        }
        status = add_operand(
            state, frame_index,
            plain_operand(xpath_construct(state->frames[frame_index].construct.ast, argument,
                                          state->frames[frame_index].construct.static_context),
                          parameter_usage(state->ast, function, position)));
    }
    if (status == SXML_STATUS_OK) {
        start_general(state, frame_index,
                      (general_start){.finish = {.combination = SXML_STREAM_COMBINATION_GENERAL,
                                                 .is_at_most_one = false,
                                                 .type = SXML_U_ANY},
                                      .after_phase = SXML_STREAM_CALL_PHASE_STYLESHEET_COMBINED});
    }
    return status;
}

/* The categories' rules for function calls (19.8.5.1 to 19.8.5.7). */
static assessment finish_stylesheet_call(const machine *state, const frame *current) {
    const sxml_xslt_ast_node_index function = current->auxiliary_node;
    const sxml_xslt_ast_keyword category = function_category(state->ast, function);
    const assessment first = current->saved;
    const assessment others = current->general_result;
    const declared_type declared_result = declared_type_of_node(state->ast, function);
    assessment result = others;
    result.type = declared_result.type;
    if (category == SXML_XSLT_AST_KEYWORD_ABSORBING) {
        if (first.posture == SXML_STREAM_POSTURE_CRAWLING) {
            result = roaming_free_ranging(declared_result.type);
        }
    } else if (category == SXML_XSLT_AST_KEYWORD_ASCENT) {
        if (is_free(others) || others.sweep != SXML_STREAM_SWEEP_MOTIONLESS) {
            result = roaming_free_ranging(declared_result.type);
        } else if (first.posture == SXML_STREAM_POSTURE_GROUNDED) {
            result = grounded_motionless(declared_result.type);
        } else {
            result = make_assessment(SXML_STREAM_POSTURE_CLIMBING, SXML_STREAM_SWEEP_MOTIONLESS, declared_result.type);
        }
    } else if (category == SXML_XSLT_AST_KEYWORD_SHALLOW_DESCENT ||
               category == SXML_XSLT_AST_KEYWORD_DEEP_DESCENT) {
        const declared_type first_type = declared_type_of_node(
            state->ast, parameter_at(state->ast, (parameter_query){.node = function, .position = 0}));
        const assessment adjusted = type_adjusted(first, type_determined_usage(first_type));
        const bool is_consuming_twice =
            adjusted.sweep == SXML_STREAM_SWEEP_CONSUMING && others.sweep == SXML_STREAM_SWEEP_CONSUMING;
        if ((adjusted.posture != SXML_STREAM_POSTURE_STRIDING && adjusted.posture != SXML_STREAM_POSTURE_GROUNDED) ||
            others.posture != SXML_STREAM_POSTURE_GROUNDED || is_consuming_twice || is_free(adjusted) ||
            is_free(others)) {
            result = roaming_free_ranging(declared_result.type);
        } else if (adjusted.posture == SXML_STREAM_POSTURE_GROUNDED) {
            result = make_assessment(SXML_STREAM_POSTURE_GROUNDED, wider_sweep(adjusted.sweep, others.sweep),
                                     declared_result.type);
        } else {
            const bool keeps_sweep =
                (first_type.type & SXML_U_PARENTS) == 0U || (first.type & SXML_U_PARENTS) == 0U;
            result = make_assessment(category == SXML_XSLT_AST_KEYWORD_SHALLOW_DESCENT
                                         ? adjusted.posture
                                         : SXML_STREAM_POSTURE_CRAWLING,
                                     keeps_sweep ? adjusted.sweep : SXML_STREAM_SWEEP_CONSUMING,
                                     declared_result.type);
        }
    }
    result.is_uncertain = first.is_uncertain || others.is_uncertain;
    return result;
}

/* 19.8.8.14 and 19.8.9. */
static sxml_status step_function_call(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const call_shape shape = shape_of_call(ast, current->construct.ast_node);
    const resolved_name name = resolve_name(
        &state->lookup, (name_to_resolve){.ast = ast,
                                          .name = child_by_field(ast, shape.call, SXML_AST_FIELD_NAME),
                                          .static_context = current->construct.static_context,
                                          .default_namespace = SXML_NAMESPACE_FUNCTIONS});
    assessment child;
    switch (current->phase) {
    case SXML_STREAM_CALL_PHASE_SPECIAL_ARGUMENT: {
        if (!take_child(current, &child)) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        assessment result;
        if (is_fn_function(name, "accumulator-after")) {
            result = accumulator_after_assessment(current, child);
        } else if (is_fn_function(name, "accumulator-before")) {
            /* 19.8.9.2. */
            result = child.sweep == SXML_STREAM_SWEEP_MOTIONLESS && !is_free(child) ? grounded_motionless(SXML_U_ANY)
                                                                         : roaming_free_ranging(SXML_U_ANY);
            result.is_uncertain = child.is_uncertain;
        } else {
            result = assess_root(child);
            result.is_pattern_step = false;
        }
        finish_frame(current, result);
        return SXML_STATUS_OK;
    }
    case SXML_STREAM_CALL_PHASE_FIRST_ARGUMENT:
        if (!take_child(current, &child)) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        current->saved = child;
        return continue_stylesheet_call(state, frame_index, &shape);
    case SXML_STREAM_CALL_PHASE_STYLESHEET_COMBINED:
        finish_frame(current, finish_stylesheet_call(state, current));
        return SXML_STATUS_OK;
    case SXML_STREAM_CALL_PHASE_OUTERMOST_COMBINED: {
        /* 19.8.9.15. */
        assessment result = current->general_result;
        if (result.posture == SXML_STREAM_POSTURE_CRAWLING) {
            result.posture = SXML_STREAM_POSTURE_STRIDING;
        }
        result.type = current->general.transmitted_type;
        finish_frame(current, result);
        return SXML_STATUS_OK;
    }
    case SXML_STREAM_CALL_PHASE_UNKNOWN_COMBINED: {
        assessment result = current->general_result;
        result.type = SXML_U_ANY;
        finish_frame(current, result);
        return SXML_STATUS_OK;
    }
    default:
        break;
    }
    if (shape.has_keyword_argument || sxml_ast_get_node(ast, shape.call)->kind != SXML_AST_KIND_FUNCTION_CALL) {
        /* Keyword arguments and dynamic arrow targets: uncertain unless
           every argument is grounded. */
        begin_operands(state, frame_index);
        const sxml_status status = add_call_operands(
            state, frame_index,
            &(call_operands){.shape = &shape,
                             .usages = NULL,
                             .usage_of_others = SXML_STREAM_USAGE_NAVIGATION,
                             .first_position = 0,
                             .operand_flags = SXML_STREAM_OPERAND_FLAG_UNCERTAIN_UNLESS_GROUNDED});
        start_general(state, frame_index,
                      (general_start){.finish = {.combination = SXML_STREAM_COMBINATION_GENERAL,
                                                 .is_at_most_one = false,
                                                 .type = SXML_U_ANY},
                                      .after_phase = SXML_STREAM_CALL_PHASE_UNKNOWN_COMBINED});
        return status;
    }
    assessment special;
    if (assess_focus_function(state, (focus_call){.frame = current, .name = name, .arity = shape.arity},
                              &special)) {
        finish_frame(current, special);
        return SXML_STATUS_OK;
    }
    if ((is_fn_function(name, "accumulator-after") || is_fn_function(name, "accumulator-before") ||
         is_fn_function(name, "root")) &&
        shape.arity == 1U && argument_at(ast, &shape, 0U).value != 0) {
        current->phase = SXML_STREAM_CALL_PHASE_SPECIAL_ARGUMENT;
        return push_operand(state, frame_index,
                            plain_operand(xpath_construct(ast, argument_at(ast, &shape, 0U),
                                                          current->construct.static_context),
                                          SXML_STREAM_USAGE_TRANSMISSION));
    }
    if (shape.has_placeholder && is_focus_dependent_function(name, shape.arity) &&
        current->focus.posture != SXML_STREAM_POSTURE_GROUNDED) {
        /* 19.8.8.14, partial function applications. */
        finish_frame(current, roaming_free_ranging(SXML_U_FUNCTION));
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_node_index function =
        find_function(&state->lookup, (function_query){.name = name, .arity = shape.arity});
    if (function.value != 0) {
        return start_stylesheet_call(state, frame_index,
                                     (named_call){.shape = &shape, .name = name, .function = function});
    }
    begin_operands(state, frame_index);
    sxml_status status = SXML_STATUS_OK;
    if (is_higher_order_built_in(name, shape.arity)) {
        status = add_higher_order_call_operands(
            state, frame_index, (named_call){.shape = &shape, .name = name, .function = no_xslt_node});
        start_general(state, frame_index, return_general(SXML_STREAM_RESULT_ANY));
        return status;
    }
    if (name.is_resolved && name.namespace_uri.index ==
                                sxml_well_known_namespace_identifier(SXML_NAMESPACE_XML_SCHEMA).index &&
        shape.arity == 1U) {
        /* A constructor function: one operand, absorbed. */
        status = add_call_operands(state, frame_index,
                                   &(call_operands){.shape = &shape,
                                                    .usages = "A",
                                                    .usage_of_others = SXML_STREAM_USAGE_ABSORPTION,
                                                    .first_position = 0,
                                                    .operand_flags = 0});
        start_general(state, frame_index, return_general(SXML_STREAM_RESULT_ATOMIC));
        return status;
    }
    const built_in *entry = find_built_in(name, shape.arity);
    if (entry == NULL) {
        /* An extension function or one 19.8.9 does not list: uncertain
           unless every argument is grounded. */
        status = add_call_operands(
            state, frame_index,
            &(call_operands){.shape = &shape,
                             .usages = NULL,
                             .usage_of_others = SXML_STREAM_USAGE_NAVIGATION,
                             .first_position = 0,
                             .operand_flags = SXML_STREAM_OPERAND_FLAG_UNCERTAIN_UNLESS_GROUNDED});
        start_general(state, frame_index,
                      (general_start){.finish = {.combination = SXML_STREAM_COMBINATION_GENERAL,
                                                 .is_at_most_one = false,
                                                 .type = SXML_U_ANY},
                                      .after_phase = SXML_STREAM_CALL_PHASE_UNKNOWN_COMBINED});
        return status;
    }
    status = add_call_operands(state, frame_index,
                               &(call_operands){.shape = &shape,
                                                .usages = entry->usages,
                                                .usage_of_others = SXML_STREAM_USAGE_NAVIGATION,
                                                .first_position = 0,
                                                .operand_flags = 0});
    if (status == SXML_STATUS_OK && entry->implicit != SXML_STREAM_IMPLICIT_NONE) {
        const assessment implicit = entry->implicit == SXML_STREAM_IMPLICIT_CONTEXT
                                        ? context_item(state->frames[frame_index].focus)
                                        : assess_root(context_item(state->frames[frame_index].focus));
        status = add_operand(state, frame_index, fixed_operand(implicit, entry->implicit_usage));
    }
    if (is_fn_function(name, "outermost")) {
        start_general(state, frame_index,
                      (general_start){.finish = {.combination = SXML_STREAM_COMBINATION_GENERAL,
                                                 .is_at_most_one = false,
                                                 .type = SXML_U_ANY},
                                      .after_phase = SXML_STREAM_CALL_PHASE_OUTERMOST_COMBINED});
        return status;
    }
    general_start start = return_general(entry->result);
    /* 19.8.1 rule 2(d)(iii). */
    start.finish.is_at_most_one = is_fn_function(name, "head") ||
                                  is_fn_function(name, "exactly-one") ||
                                  is_fn_function(name, "zero-or-one");
    start_general(state, frame_index, start);
    return status;
}

/* Adds the child of node in field as an operand, if there is one. */
typedef struct field_operand {
    sxml_ast_field field;
    usage usage;
    uint32_t flags;
    uint32_t choice_group;
} field_operand;

static sxml_status add_field_operand(machine *state, uint32_t frame_index, field_operand request) {
    const frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const sxml_ast_node_index child = child_by_field(ast, current->construct.ast_node, request.field);
    if (child.value == 0) {
        return SXML_STATUS_OK;
    }
    operand value = plain_operand(xpath_construct(ast, child, current->construct.static_context),
                                  request.usage);
    value.flags = request.flags;
    value.choice_group = request.choice_group;
    return add_operand(state, frame_index, value);
}

/* 19.8.8.11: the function I; the arguments by the signature of an
   inline function, else navigation. */
static sxml_status start_dynamic_call(machine *state, uint32_t frame_index) {
    begin_operands(state, frame_index);
    const frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const sxml_ast_node_index node = current->construct.ast_node;
    sxml_status status = add_field_operand(
        state, frame_index,
        (field_operand){.field = SXML_AST_FIELD_FUNCTION, .usage = SXML_STREAM_USAGE_INSPECTION, .flags = 0, .choice_group = 0});
    const sxml_ast_node_index function = child_by_field(ast, node, SXML_AST_FIELD_FUNCTION);
    uint32_t position = 0;
    for (sxml_ast_node_index argument =
             sxml_ast_get_node(ast, child_by_field(ast, node, SXML_AST_FIELD_ARGUMENTS))->first_child;
         status == SXML_STATUS_OK && argument.value != 0;
         argument = sxml_ast_get_node(ast, argument)->next_sibling) {
        const sxml_ast_node *entry = sxml_ast_get_node(ast, argument);
        if (entry->field != SXML_AST_FIELD_ARGUMENT) {
            continue;
        }
        if (entry->kind != SXML_AST_KIND_ARGUMENT_PLACEHOLDER) {
            /* Without a signature (a variable may hold a map, 19.8.8.11,
               note), uncertain unless grounded. */
            const declared_type type = inline_parameter_type(ast, function, position);
            operand value = plain_operand(
                xpath_construct(ast, argument, state->frames[frame_index].construct.static_context),
                type_determined_usage(type));
            value.flags = type.is_declared ? 0U : SXML_STREAM_OPERAND_FLAG_UNCERTAIN_UNLESS_GROUNDED;
            status = add_operand(state, frame_index, value);
        }
        position += 1U;
    }
    start_general(state, frame_index, return_general(SXML_STREAM_RESULT_ANY));
    return status;
}

/* 19.8.8.1 and the LetExpr row of 19.8.8: every binding's expression is
   navigated, and is higher-order after a for binding; the return
   expression is transmitted, higher-order after one. */
static sxml_status start_flwor(machine *state, uint32_t frame_index) {
    begin_operands(state, frame_index);
    const frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const sxml_xslt_ast_static_context_index static_context = current->construct.static_context;
    bool follows_for = false;
    bool is_uncertain = false;
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index clause = sxml_ast_get_node(ast, current->construct.ast_node)->first_child;
         status == SXML_STATUS_OK && clause.value != 0;
         clause = sxml_ast_get_node(ast, clause)->next_sibling) {
        const sxml_ast_node *clause_entry = sxml_ast_get_node(ast, clause);
        if (clause_entry->kind == SXML_AST_KIND_RETURN_CLAUSE) {
            operand value = plain_operand(
                xpath_construct(ast, child_by_field(ast, clause, SXML_AST_FIELD_VALUE), static_context),
                SXML_STREAM_USAGE_TRANSMISSION);
            value.flags = follows_for ? SXML_STREAM_OPERAND_FLAG_HIGHER_ORDER : 0U;
            status = add_operand(state, frame_index, value);
            continue;
        }
        const bool is_for = clause_entry->kind == SXML_AST_KIND_FOR_CLAUSE;
        if (!is_for && clause_entry->kind != SXML_AST_KIND_LET_CLAUSE) {
            is_uncertain = true;
            continue;
        }
        for (sxml_ast_node_index binding = clause_entry->first_child;
             status == SXML_STATUS_OK && binding.value != 0;
             binding = sxml_ast_get_node(ast, binding)->next_sibling) {
            const sxml_ast_node_index value_node = child_by_field(
                ast, binding, is_for ? SXML_AST_FIELD_SEQUENCE : SXML_AST_FIELD_VALUE);
            if (value_node.value == 0) {
                continue;
            }
            operand value = plain_operand(xpath_construct(ast, value_node, static_context),
                                          SXML_STREAM_USAGE_NAVIGATION);
            /* Binding a variable to streamed nodes is not
               guaranteed-streamable, but a processor may stream it by
               tracing the variable (19.8.8.1, note): uncertain. */
            value.flags = (follows_for ? SXML_STREAM_OPERAND_FLAG_HIGHER_ORDER : 0U) |
                          SXML_STREAM_OPERAND_FLAG_UNCERTAIN_UNLESS_GROUNDED;
            status = add_operand(state, frame_index, value);
            follows_for = follows_for || is_for;
        }
    }
    state->frames[frame_index].general.is_uncertain = is_uncertain;
    start_general(state, frame_index, return_general(SXML_STREAM_RESULT_TRANSMITTED));
    return status;
}

/* 19.8.8.2. */
static sxml_status start_quantified(machine *state, uint32_t frame_index) {
    begin_operands(state, frame_index);
    const frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const sxml_xslt_ast_static_context_index static_context = current->construct.static_context;
    bool is_first = true;
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index binding = sxml_ast_get_node(ast, current->construct.ast_node)->first_child;
         status == SXML_STATUS_OK && binding.value != 0;
         binding = sxml_ast_get_node(ast, binding)->next_sibling) {
        if (sxml_ast_get_node(ast, binding)->field != SXML_AST_FIELD_BINDING) {
            continue;
        }
        operand value = plain_operand(
            xpath_construct(ast, child_by_field(ast, binding, SXML_AST_FIELD_SEQUENCE), static_context),
            SXML_STREAM_USAGE_NAVIGATION);
        /* As for a for binding (19.8.8.2, note). */
        value.flags = (is_first ? 0U : SXML_STREAM_OPERAND_FLAG_HIGHER_ORDER) | SXML_STREAM_OPERAND_FLAG_UNCERTAIN_UNLESS_GROUNDED;
        status = add_operand(state, frame_index, value);
        is_first = false;
    }
    if (status == SXML_STATUS_OK) {
        status = add_field_operand(state, frame_index,
                                   (field_operand){.field = SXML_AST_FIELD_CONDITION,
                                                   .usage = SXML_STREAM_USAGE_INSPECTION,
                                                   .flags = SXML_STREAM_OPERAND_FLAG_HIGHER_ORDER,
                                                   .choice_group = 0});
    }
    start_general(state, frame_index, return_general(SXML_STREAM_RESULT_BOOLEAN));
    return status;
}

/* The one binding of "for $x in S return R" or "some|every $x in S
   satisfies C", or 0 when the expression has more clauses or bindings, a
   positional variable or a let clause. */
static sxml_ast_node_index single_binding(const sxml_ast *ast, sxml_ast_node_index node) {
    const sxml_ast_node *entry = sxml_ast_get_node(ast, node);
    sxml_ast_node_index binding = no_ast_node;
    uint32_t binding_count = 0;
    for (sxml_ast_node_index child = entry->first_child; child.value != 0;
         child = sxml_ast_get_node(ast, child)->next_sibling) {
        const sxml_ast_node *child_entry = sxml_ast_get_node(ast, child);
        if (entry->kind == SXML_AST_KIND_QUANTIFIED_EXPRESSION) {
            if (child_entry->field == SXML_AST_FIELD_BINDING) {
                binding = child;
                binding_count += 1U;
            }
            continue;
        }
        if (child_entry->kind == SXML_AST_KIND_RETURN_CLAUSE) {
            continue;
        }
        if (child_entry->kind != SXML_AST_KIND_FOR_CLAUSE) {
            return no_ast_node;
        }
        for (sxml_ast_node_index for_binding = child_entry->first_child; for_binding.value != 0;
             for_binding = sxml_ast_get_node(ast, for_binding)->next_sibling) {
            if (sxml_ast_get_node(ast, for_binding)->kind != SXML_AST_KIND_FOR_ITEM_BINDING ||
                child_by_field(ast, for_binding, SXML_AST_FIELD_POSITION).value != 0) {
                return no_ast_node;
            }
            binding = for_binding;
            binding_count += 1U;
        }
    }
    return binding_count == 1U ? binding : no_ast_node;
}

/*
 * 19.8.8.1 and 19.8.8.2 make a binding to streamed nodes roaming: the
 * analysis is syntactic, and a processor may do more. sxml does what the
 * XT40 cases expect (streamable-100 accepted, -114 and -129 rejected):
 * with one binding of a sequence S that is not grounded, "for $x in S
 * return R" is assessed as S ! R and "some $x in S satisfies C" as
 * exists(S ! C) (inspected), with $x as ".". Other shapes, and grounded
 * sequences, take the general rules (start_flwor, start_quantified).
 */
static sxml_status step_binding(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const sxml_ast_node_index node = current->construct.ast_node;
    const bool is_quantified =
        sxml_ast_get_node(ast, node)->kind == SXML_AST_KIND_QUANTIFIED_EXPRESSION;
    const sxml_ast_node_index binding = single_binding(ast, node);
    assessment child = grounded_motionless(0U);
    if (current->phase == 0U && binding.value != 0) {
        current->phase = 1U;
        return push_operand(state, frame_index,
                            plain_operand(xpath_construct(ast, child_by_field(ast, binding, SXML_AST_FIELD_SEQUENCE),
                                                          current->construct.static_context),
                                          SXML_STREAM_USAGE_TRANSMISSION));
    }
    if (current->phase == 0U || (current->phase == 1U && take_child(current, &child) &&
                                 child.posture == SXML_STREAM_POSTURE_GROUNDED)) {
        return is_quantified ? start_quantified(state, frame_index) : start_flwor(state, frame_index);
    }
    if (current->phase == 1U) {
        current->saved = child;
        current->phase = 2U;
        sxml_ast_node_index body = child_by_field(ast, node, SXML_AST_FIELD_CONDITION);
        if (!is_quantified) {
            for (sxml_ast_node_index clause = sxml_ast_get_node(ast, node)->first_child; clause.value != 0;
                 clause = sxml_ast_get_node(ast, clause)->next_sibling) {
                if (sxml_ast_get_node(ast, clause)->kind == SXML_AST_KIND_RETURN_CLAUSE) {
                    body = child_by_field(ast, clause, SXML_AST_FIELD_VALUE);
                }
            }
        }
        const sxml_status status = push_operand(
            state, frame_index,
            focused_operand(xpath_construct(ast, body, current->construct.static_context), focus_of(child)));
        if (status == SXML_STATUS_OK) {
            frame *pushed = &state->frames[state->frame_count - 1U];
            const sxml_ast_node_index variable =
                child_by_field(ast, child_by_field(ast, binding, SXML_AST_FIELD_VARIABLE), SXML_AST_FIELD_NAME);
            pushed->scope.bound_variable = resolve_name(
                &state->lookup, (name_to_resolve){.ast = ast,
                                                  .name = variable,
                                                  .static_context = current->construct.static_context,
                                                  .default_namespace = SXML_NAMESPACE_NONE});
            pushed->scope.bound_value = child;
        }
        return status;
    }
    if (!take_child(current, &child)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const assessment sequence = current->saved;
    assessment mapped = normalized(
        make_assessment(child.posture, wider_sweep(sequence.sweep, child.sweep), child.type));
    mapped.is_uncertain = sequence.is_uncertain || child.is_uncertain;
    if (is_quantified) {
        mapped = type_adjusted(mapped, SXML_STREAM_USAGE_INSPECTION);
        mapped.type = SXML_U_BOOLEAN;
    }
    finish_frame(current, mapped);
    return SXML_STATUS_OK;
}

/* 19.8.8.15. */
static assessment named_function_reference_assessment(const machine *state, const frame *current) {
    const sxml_ast *ast = current->construct.ast;
    const sxml_ast_node_index node = current->construct.ast_node;
    const resolved_name name = resolve_name(
        &state->lookup, (name_to_resolve){.ast = ast,
                                          .name = child_by_field(ast, node, SXML_AST_FIELD_NAME),
                                          .static_context = current->construct.static_context,
                                          .default_namespace = SXML_NAMESPACE_FUNCTIONS});
    const sxml_byte_view arity_text = sxml_ast_get_text(
        ast, sxml_ast_get_node(ast, child_by_field(ast, node, SXML_AST_FIELD_ARITY))->payload.text);
    uint32_t arity = 0;
    for (size_t byte_index = 0; byte_index < arity_text.byte_count && arity < 1000U; byte_index += 1) {
        const uint8_t digit = arity_text.bytes[byte_index];
        if (digit >= '0' && digit <= '9') {
            arity = arity * 10U + (uint32_t)(digit - '0');
        }
    }
    if (is_focus_dependent_function(name, arity) && current->focus.posture != SXML_STREAM_POSTURE_GROUNDED) {
        return roaming_free_ranging(SXML_U_FUNCTION);
    }
    return grounded_motionless(SXML_U_FUNCTION);
}

/* 19.8.8.18: the base inspected, a computed key absorbed. */
static sxml_status add_lookup_key(machine *state, uint32_t frame_index, sxml_ast_node_index lookup) {
    const frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    const sxml_ast_node_index key = child_by_field(ast, lookup, SXML_AST_FIELD_KEY);
    const sxml_ast_kind kind = sxml_ast_get_node(ast, key)->kind;
    if (kind == SXML_AST_KIND_PARENTHESIZED_EXPRESSION || kind == SXML_AST_KIND_VARIABLE_REFERENCE ||
        kind == SXML_AST_KIND_CONTEXT_VALUE_REFERENCE) {
        return add_operand(state, frame_index,
                           plain_operand(xpath_construct(ast, key, current->construct.static_context),
                                         SXML_STREAM_USAGE_ABSORPTION));
    }
    return SXML_STATUS_OK;
}

static sxml_status step_xpath(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    if (current->phase == SXML_STREAM_PHASE_GENERAL) {
        return step_general(state, frame_index);
    }
    if (finishes_general(current)) {
        return SXML_STATUS_OK;
    }
    const sxml_ast *ast = current->construct.ast;
    const sxml_ast_node_index node = current->construct.ast_node;
    const sxml_ast_node *entry = sxml_ast_get_node(ast, node);
    const xpath_rule rule = rule_of_xpath_kind(entry->kind);
    sxml_status status = SXML_STATUS_OK;
    assessment child;
    switch (rule) {
    case SXML_STREAM_XPATH_RULE_UNCERTAIN: {
        assessment result = grounded_motionless(SXML_U_ANY);
        result.is_uncertain = true;
        finish_frame(current, result);
        return SXML_STATUS_OK;
    }
    case SXML_STREAM_XPATH_RULE_PASS: {
        if (current->phase == 1U) {
            if (!take_child(current, &child)) {
                return SXML_STATUS_INVALID_ARGUMENT;
            }
            child.is_pattern_step = false;
            finish_frame(current, child);
            return SXML_STATUS_OK;
        }
        if (entry->kind == SXML_AST_KIND_XPATH) {
            /* The outermost expression: its focus for fn:current()
               (19.8.9.3). */
            current->scope.expression_focus = current->focus;
            current->scope.is_below_higher_order = false;
        }
        const sxml_ast_node_index inner = child_by_field(
            ast, node, entry->kind == SXML_AST_KIND_XPATH ? SXML_AST_FIELD_BODY : SXML_AST_FIELD_EXPRESSION);
        if (inner.value == 0) {
            finish_frame(current, grounded_motionless(0U));
            return SXML_STATUS_OK;
        }
        current->phase = 1U;
        return push_operand(state, frame_index,
                            plain_operand(xpath_construct(ast, inner, current->construct.static_context),
                                          SXML_STREAM_USAGE_TRANSMISSION));
    }
    case SXML_STREAM_XPATH_RULE_GENERAL:
        begin_operands(state, frame_index);
        status = add_child_operands(state, frame_index);
        start_general(state, frame_index, return_general(general_result_type(entry->kind)));
        return status;
    case SXML_STREAM_XPATH_RULE_LITERAL:
        finish_frame(current, literal_assessment(entry->kind));
        return SXML_STATUS_OK;
    case SXML_STREAM_XPATH_RULE_CONTEXT_ITEM: {
        /* "." is self::node() for a node, a step of a pattern
           (19.8.8.8: ./descendant::a/b scans as descendant::a/b). */
        assessment result = context_item(current->focus);
        result.is_pattern_step = true;
        finish_frame(current, result);
        return SXML_STATUS_OK;
    }
    case SXML_STREAM_XPATH_RULE_VARIABLE:
        finish_frame(current, variable_assessment(state, current));
        return SXML_STATUS_OK;
    case SXML_STREAM_XPATH_RULE_AXIS_STEP:
        return step_axis_step(state, frame_index);
    case SXML_STREAM_XPATH_RULE_PATH:
        return step_path(state, frame_index);
    case SXML_STREAM_XPATH_RULE_SIMPLE_MAP:
        return step_simple_map(state, frame_index);
    case SXML_STREAM_XPATH_RULE_FILTER:
        return step_filter(state, frame_index);
    case SXML_STREAM_XPATH_RULE_UNION:
        return step_union(state, frame_index);
    case SXML_STREAM_XPATH_RULE_FUNCTION_CALL:
        return step_function_call(state, frame_index);
    case SXML_STREAM_XPATH_RULE_DYNAMIC_CALL:
        return start_dynamic_call(state, frame_index);
    case SXML_STREAM_XPATH_RULE_FLWOR:
    case SXML_STREAM_XPATH_RULE_QUANTIFIED:
        return step_binding(state, frame_index);
    case SXML_STREAM_XPATH_RULE_INLINE_FUNCTION: {
        /* 19.8.8.16. */
        const scan_result scan = scan_expression(
            &state->lookup,
            &(scan_request){.ast = ast,
                            .root = node,
                            .static_context = current->construct.static_context,
                            .skips_predicates = false,
                            .skips_controlled_operands = false,
                            .streaming_parameter = streaming_parameter_name(state, &current->scope)});
        finish_frame(current, scan.references_streaming_parameter ? roaming_free_ranging(SXML_U_FUNCTION)
                                                                   : grounded_motionless(SXML_U_FUNCTION));
        return SXML_STATUS_OK;
    }
    case SXML_STREAM_XPATH_RULE_NAMED_FUNCTION_REFERENCE:
        finish_frame(current, named_function_reference_assessment(state, current));
        return SXML_STATUS_OK;
    case SXML_STREAM_XPATH_RULE_MAP_CONSTRUCTOR:
        /* 19.8.8.17: as xsl:map of xsl:map-entry instructions. */
        begin_operands(state, frame_index);
        for (sxml_ast_node_index member = entry->first_child;
             status == SXML_STATUS_OK && member.value != 0;
             member = sxml_ast_get_node(ast, member)->next_sibling) {
            status = add_operand(
                state, frame_index,
                plain_operand(xpath_construct(ast, member, state->frames[frame_index].construct.static_context),
                              SXML_STREAM_USAGE_TRANSMISSION));
        }
        start_general(state, frame_index,
                      (general_start){.finish = {.combination = SXML_STREAM_COMBINATION_MAP,
                                                 .is_at_most_one = false,
                                                 .type = SXML_U_FUNCTION},
                                      .after_phase = SXML_STREAM_PHASE_RETURN_GENERAL});
        return status;
    case SXML_STREAM_XPATH_RULE_IF:
        /* 19.8.8.3. */
        begin_operands(state, frame_index);
        status = add_field_operand(state, frame_index,
                                   (field_operand){.field = SXML_AST_FIELD_CONDITION,
                                                   .usage = SXML_STREAM_USAGE_INSPECTION,
                                                   .flags = 0,
                                                   .choice_group = 0});
        if (status == SXML_STATUS_OK) {
            status = add_field_operand(state, frame_index,
                                       (field_operand){.field = SXML_AST_FIELD_THEN,
                                                       .usage = SXML_STREAM_USAGE_TRANSMISSION,
                                                       .flags = 0,
                                                       .choice_group = 1});
        }
        if (status == SXML_STATUS_OK) {
            status = add_field_operand(state, frame_index,
                                       (field_operand){.field = SXML_AST_FIELD_ELSE,
                                                       .usage = SXML_STREAM_USAGE_TRANSMISSION,
                                                       .flags = 0,
                                                       .choice_group = 1});
        }
        start_general(state, frame_index, return_general(SXML_STREAM_RESULT_TRANSMITTED));
        return status;
    case SXML_STREAM_XPATH_RULE_INSTANCE_OF: {
        /* 19.8.8.5. */
        const declared_type type =
            sequence_type_of(ast, child_by_field(ast, node, SXML_AST_FIELD_TYPE));
        begin_operands(state, frame_index);
        status = add_field_operand(
            state, frame_index,
            (field_operand){.field = SXML_AST_FIELD_VALUE,
                            .usage = type.is_document_with_element ? SXML_STREAM_USAGE_ABSORPTION : SXML_STREAM_USAGE_INSPECTION,
                            .flags = 0,
                            .choice_group = 0});
        start_general(state, frame_index, return_general(SXML_STREAM_RESULT_BOOLEAN));
        return status;
    }
    case SXML_STREAM_XPATH_RULE_TREAT: {
        /* 19.8.8.6. */
        const declared_type type =
            sequence_type_of(ast, child_by_field(ast, node, SXML_AST_FIELD_TYPE));
        if (current->phase == 1U) {
            assessment result = current->general_result;
            result.type = type.type;
            finish_frame(current, result);
            return SXML_STATUS_OK;
        }
        if (type.is_document_with_element) {
            finish_frame(current, roaming_free_ranging(type.type));
            return SXML_STATUS_OK;
        }
        begin_operands(state, frame_index);
        status = add_field_operand(state, frame_index,
                                   (field_operand){.field = SXML_AST_FIELD_VALUE,
                                                   .usage = SXML_STREAM_USAGE_TRANSMISSION,
                                                   .flags = 0,
                                                   .choice_group = 0});
        start_general(state, frame_index,
                      (general_start){.finish = {.combination = SXML_STREAM_COMBINATION_GENERAL,
                                                 .is_at_most_one = false,
                                                 .type = type.type},
                                      .after_phase = 1U});
        return status;
    }
    case SXML_STREAM_XPATH_RULE_LOOKUP:
        begin_operands(state, frame_index);
        status = add_field_operand(state, frame_index,
                                   (field_operand){.field = SXML_AST_FIELD_VALUE,
                                                   .usage = SXML_STREAM_USAGE_INSPECTION,
                                                   .flags = 0,
                                                   .choice_group = 0});
        if (status == SXML_STATUS_OK) {
            status = add_lookup_key(state, frame_index, child_by_field(ast, node, SXML_AST_FIELD_LOOKUP));
        }
        start_general(state, frame_index, return_general(SXML_STREAM_RESULT_ANY));
        return status;
    case SXML_STREAM_XPATH_RULE_UNARY_LOOKUP:
        begin_operands(state, frame_index);
        status = add_operand(state, frame_index,
                             fixed_operand(context_item(current->focus), SXML_STREAM_USAGE_INSPECTION));
        if (status == SXML_STATUS_OK) {
            status = add_lookup_key(state, frame_index, node);
        }
        start_general(state, frame_index, return_general(SXML_STREAM_RESULT_ANY));
        return status;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* ------------------------------------------------------- XSLT constructs */

/* How an XSLT instruction is assessed (19.8.4). */
typedef enum xslt_rule {
    SXML_STREAM_XSLT_RULE_UNCERTAIN = 0,
    SXML_STREAM_XSLT_RULE_GENERAL,
    SXML_STREAM_XSLT_RULE_GROUNDED,
    SXML_STREAM_XSLT_RULE_APPLY_TEMPLATES,
    SXML_STREAM_XSLT_RULE_FOR_EACH,
    SXML_STREAM_XSLT_RULE_FOR_EACH_GROUP,
    SXML_STREAM_XSLT_RULE_ITERATE,
    SXML_STREAM_XSLT_RULE_FORK,
    SXML_STREAM_XSLT_RULE_MAP,
    SXML_STREAM_XSLT_RULE_MERGE,
    SXML_STREAM_XSLT_RULE_SOURCE_DOCUMENT,
    SXML_STREAM_XSLT_RULE_COPY,
    SXML_STREAM_XSLT_RULE_PERFORM_SORT,
} xslt_rule;

static const uint8_t xslt_rules[SXML_XSLT_AST_KIND_COUNT] = {
    [SXML_XSLT_AST_KIND_LITERAL_RESULT_ELEMENT] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_EXTENSION_INSTRUCTION] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_UNKNOWN_INSTRUCTION] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_VARIABLE] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_ANALYZE_STRING] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_APPLY_IMPORTS] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_APPLY_TEMPLATES] = SXML_STREAM_XSLT_RULE_APPLY_TEMPLATES,
    [SXML_XSLT_AST_KIND_ARRAY] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_ARRAY_MEMBER] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_ASSERT] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_ATTRIBUTE] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_BREAK] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_CALL_TEMPLATE] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_CHOOSE] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_COMMENT] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_COPY] = SXML_STREAM_XSLT_RULE_COPY,
    [SXML_XSLT_AST_KIND_COPY_OF] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_DOCUMENT] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_ELEMENT] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_EVALUATE] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_FALLBACK] = SXML_STREAM_XSLT_RULE_GROUNDED,
    [SXML_XSLT_AST_KIND_FOR_EACH] = SXML_STREAM_XSLT_RULE_FOR_EACH,
    [SXML_XSLT_AST_KIND_FOR_EACH_GROUP] = SXML_STREAM_XSLT_RULE_FOR_EACH_GROUP,
    [SXML_XSLT_AST_KIND_FORK] = SXML_STREAM_XSLT_RULE_FORK,
    [SXML_XSLT_AST_KIND_IF] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_ITERATE] = SXML_STREAM_XSLT_RULE_ITERATE,
    [SXML_XSLT_AST_KIND_MAP] = SXML_STREAM_XSLT_RULE_MAP,
    [SXML_XSLT_AST_KIND_MAP_ENTRY] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_MERGE] = SXML_STREAM_XSLT_RULE_MERGE,
    [SXML_XSLT_AST_KIND_MESSAGE] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_NAMESPACE] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_NEXT_ITERATION] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_NEXT_MATCH] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_NUMBER] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_ON_EMPTY] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_ON_NON_EMPTY] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_PERFORM_SORT] = SXML_STREAM_XSLT_RULE_PERFORM_SORT,
    [SXML_XSLT_AST_KIND_PROCESSING_INSTRUCTION] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_RESULT_DOCUMENT] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_SELECT] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_SEQUENCE] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_SOURCE_DOCUMENT] = SXML_STREAM_XSLT_RULE_SOURCE_DOCUMENT,
    [SXML_XSLT_AST_KIND_SWITCH] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_TEXT] = SXML_STREAM_XSLT_RULE_GROUNDED,
    [SXML_XSLT_AST_KIND_TRY] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_VALUE_OF] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_WHERE_POPULATED] = SXML_STREAM_XSLT_RULE_GENERAL,
    [SXML_XSLT_AST_KIND_RECORD] = SXML_STREAM_XSLT_RULE_GENERAL,
};

static xslt_rule rule_of_xslt_kind(sxml_xslt_ast_kind kind) {
    return (uint32_t)kind < (uint32_t)SXML_XSLT_AST_KIND_COUNT ? (xslt_rule)xslt_rules[kind]
                                                              : SXML_STREAM_XSLT_RULE_UNCERTAIN;
}


/* The usage of an expression attribute of an instruction (19.8.4); NONE
   for one that is no operand. Value templates absorb (19.8.7). */
static usage xslt_attribute_usage(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node,
                                  const sxml_xslt_ast_attribute *attribute) {
    const sxml_xslt_ast_kind kind = sxml_xslt_ast_get_node(ast, node)->kind;
    if (attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE) {
        return SXML_STREAM_USAGE_ABSORPTION;
    }
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_EXPRESSION) {
        return SXML_STREAM_USAGE_NONE;
    }
    const sxml_xslt_ast_attribute_kind attribute_kind = attribute->kind;
    if (attribute_kind == SXML_XSLT_AST_ATTRIBUTE_TEST) {
        return kind == SXML_XSLT_AST_KIND_SWITCH ? SXML_STREAM_USAGE_ABSORPTION : SXML_STREAM_USAGE_INSPECTION;
    }
    if (attribute_kind == SXML_XSLT_AST_ATTRIBUTE_NAMESPACE_CONTEXT) {
        return SXML_STREAM_USAGE_INSPECTION;
    }
    if (kind == SXML_XSLT_AST_KIND_IF && (attribute_kind == SXML_XSLT_AST_ATTRIBUTE_THEN ||
                                          attribute_kind == SXML_XSLT_AST_ATTRIBUTE_ELSE)) {
        /* Added with the content (add_instruction_operands). */
        return SXML_STREAM_USAGE_NONE;
    }
    if (attribute_kind == SXML_XSLT_AST_ATTRIBUTE_CONTEXT_ITEM ||
        attribute_kind == SXML_XSLT_AST_ATTRIBUTE_WITH_PARAMS ||
        attribute_kind == SXML_XSLT_AST_ATTRIBUTE_RECORD_FIELD) {
        return SXML_STREAM_USAGE_NAVIGATION;
    }
    if (attribute_kind != SXML_XSLT_AST_ATTRIBUTE_SELECT) {
        return SXML_STREAM_USAGE_ABSORPTION;
    }
    if (kind == SXML_XSLT_AST_KIND_SEQUENCE || kind == SXML_XSLT_AST_KIND_ON_EMPTY ||
        kind == SXML_XSLT_AST_KIND_ON_NON_EMPTY || kind == SXML_XSLT_AST_KIND_BREAK ||
        kind == SXML_XSLT_AST_KIND_TRY) {
        return SXML_STREAM_USAGE_TRANSMISSION;
    }
    if (kind == SXML_XSLT_AST_KIND_MAP_ENTRY || kind == SXML_XSLT_AST_KIND_NUMBER ||
        kind == SXML_XSLT_AST_KIND_ARRAY || kind == SXML_XSLT_AST_KIND_ARRAY_MEMBER) {
        return SXML_STREAM_USAGE_NAVIGATION;
    }
    if (kind == SXML_XSLT_AST_KIND_VARIABLE) {
        /* 19.8.4.41. */
        return type_determined_usage(declared_type_of_node(ast, node));
    }
    return SXML_STREAM_USAGE_ABSORPTION;
}

/* The usage of an instruction's sequence constructor; NONE for one
   without. */
static usage xslt_content_usage(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_kind kind = sxml_xslt_ast_get_node(ast, node)->kind;
    if (kind == SXML_XSLT_AST_KIND_SEQUENCE || kind == SXML_XSLT_AST_KIND_ON_EMPTY ||
        kind == SXML_XSLT_AST_KIND_ON_NON_EMPTY || kind == SXML_XSLT_AST_KIND_BREAK ||
        kind == SXML_XSLT_AST_KIND_IF || kind == SXML_XSLT_AST_KIND_TRY ||
        kind == SXML_XSLT_AST_KIND_WHERE_POPULATED) {
        return SXML_STREAM_USAGE_TRANSMISSION;
    }
    if (kind == SXML_XSLT_AST_KIND_MAP_ENTRY || kind == SXML_XSLT_AST_KIND_ARRAY ||
        kind == SXML_XSLT_AST_KIND_ARRAY_MEMBER || kind == SXML_XSLT_AST_KIND_RECORD) {
        return SXML_STREAM_USAGE_NAVIGATION;
    }
    if (kind == SXML_XSLT_AST_KIND_VARIABLE) {
        const declared_type type = declared_type_of_node(ast, node);
        return type.is_declared ? type_determined_usage(type) : SXML_STREAM_USAGE_ABSORPTION;
    }
    if (kind == SXML_XSLT_AST_KIND_LITERAL_RESULT_ELEMENT || kind == SXML_XSLT_AST_KIND_ELEMENT ||
        kind == SXML_XSLT_AST_KIND_ATTRIBUTE || kind == SXML_XSLT_AST_KIND_COMMENT ||
        kind == SXML_XSLT_AST_KIND_PROCESSING_INSTRUCTION || kind == SXML_XSLT_AST_KIND_NAMESPACE ||
        kind == SXML_XSLT_AST_KIND_MESSAGE || kind == SXML_XSLT_AST_KIND_ASSERT ||
        kind == SXML_XSLT_AST_KIND_DOCUMENT || kind == SXML_XSLT_AST_KIND_RESULT_DOCUMENT ||
        kind == SXML_XSLT_AST_KIND_VALUE_OF) {
        return SXML_STREAM_USAGE_ABSORPTION;
    }
    return SXML_STREAM_USAGE_NONE;
}

/* True when node has a child of kind. */
static bool has_child_of_kind(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node,
                              sxml_xslt_ast_kind kind) {
    for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, node)->first_child;
         child.value != 0; child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        if (is_kind(ast, child, kind)) {
            return true;
        }
    }
    return false;
}

/* Adds an operand for each expression attribute of node with a usage. */
static sxml_status add_attribute_operands(machine *state, uint32_t frame_index,
                                          sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast *ast = state->ast;
    const sxml_xslt_ast_attribute_list list = sxml_xslt_ast_get_node(ast, node)->attributes;
    for (uint32_t offset = 0; offset < list.attribute_count; offset += 1U) {
        const sxml_xslt_ast_attribute *attribute = sxml_xslt_ast_get_attribute(
            ast, (sxml_xslt_ast_attribute_index){.value = list.first_attribute_index + offset});
        const usage attribute_usage = xslt_attribute_usage(ast, node, attribute);
        if (attribute_usage == SXML_STREAM_USAGE_NONE) {
            continue;
        }
        const sxml_xslt_ast_expression *expression =
            sxml_xslt_ast_get_expression(ast, attribute->value.payload.expression);
        if (expression->ast == NULL) {
            continue;
        }
        const sxml_status status = add_operand(
            state, frame_index, plain_operand(expression_construct(expression), attribute_usage));
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* An operand for the value of a with-param or param: its select
   expression, else its sequence constructor. */
static operand value_operand(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node, usage value_usage) {
    const sxml_xslt_ast_expression *select =
        attribute_expression(ast, node, SXML_XSLT_AST_ATTRIBUTE_SELECT);
    return select != NULL ? plain_operand(expression_construct(select), value_usage)
                          : plain_operand(sequence_constructor_construct(ast, node), value_usage);
}

/* The xsl:iterate that an xsl:next-iteration belongs to; 0 for none. */
static sxml_xslt_ast_node_index enclosing_iterate(const sxml_xslt_ast *ast,
                                                  sxml_xslt_ast_node_index node) {
    sxml_xslt_ast_node_index ancestor = sxml_xslt_ast_get_node(ast, node)->parent;
    while (ancestor.value != 0 && !is_kind(ast, ancestor, SXML_XSLT_AST_KIND_ITERATE)) {
        ancestor = sxml_xslt_ast_get_node(ast, ancestor)->parent;
    }
    return ancestor;
}

/* The named template xsl:call-template names; 0 for none. */
static sxml_xslt_ast_node_index called_template(const sxml_xslt_ast *ast,
                                                sxml_xslt_ast_node_index node) {
    return find_named_declaration(
        ast, (declaration_query){.kind = SXML_XSLT_AST_KIND_TEMPLATE, .name = name_attribute(ast, node)});
}

/* The with-param children of node, each with the type-determined usage
   of its as attribute, or of the target's parameter (19.8.4.9,
   19.8.4.28), the more restrictive. */
static sxml_status add_with_param_operands(machine *state, uint32_t frame_index,
                                           sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast *ast = state->ast;
    sxml_xslt_ast_node_index target = no_xslt_node;
    if (is_kind(ast, node, SXML_XSLT_AST_KIND_CALL_TEMPLATE)) {
        target = called_template(ast, node);
    } else if (is_kind(ast, node, SXML_XSLT_AST_KIND_NEXT_ITERATION)) {
        target = enclosing_iterate(ast, node);
    }
    for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, node)->first_child;
         child.value != 0; child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        if (!is_kind(ast, child, SXML_XSLT_AST_KIND_WITH_PARAM)) {
            continue;
        }
        usage value_usage = type_determined_usage(declared_type_of_node(ast, child));
        if (target.value != 0) {
            const sxml_xslt_ast_node_index parameter = parameter_named(
                ast, (named_parameter_query){.node = target, .name = name_attribute(ast, child)});
            if (parameter.value != 0) {
                value_usage = more_restrictive_usage(
                    value_usage, type_determined_usage(declared_type_of_node(ast, parameter)));
            }
        }
        const sxml_status status =
            add_operand(state, frame_index, value_operand(ast, child, value_usage));
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* The xsl:sort children of node: their value templates absorbed, and
   their sort keys absorbed in sort_focus, higher-order operands. */
typedef struct sort_request {
    sxml_xslt_ast_node_index node;
    focus focus;
} sort_request;

static sxml_status add_sort_operands(machine *state, uint32_t frame_index, sort_request request) {
    const sxml_xslt_ast_node_index node = request.node;
    const focus sort_focus = request.focus;
    const sxml_xslt_ast *ast = state->ast;
    sxml_status status = SXML_STATUS_OK;
    for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, node)->first_child;
         status == SXML_STATUS_OK && child.value != 0;
         child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        if (!is_kind(ast, child, SXML_XSLT_AST_KIND_SORT)) {
            continue;
        }
        const sxml_xslt_ast_attribute_list list = sxml_xslt_ast_get_node(ast, child)->attributes;
        for (uint32_t offset = 0; status == SXML_STATUS_OK && offset < list.attribute_count;
             offset += 1U) {
            const sxml_xslt_ast_attribute *attribute = sxml_xslt_ast_get_attribute(
                ast, (sxml_xslt_ast_attribute_index){.value = list.first_attribute_index + offset});
            if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE) {
                continue;
            }
            const sxml_xslt_ast_expression *expression =
                sxml_xslt_ast_get_expression(ast, attribute->value.payload.expression);
            if (expression->ast != NULL) {
                status = add_operand(state, frame_index,
                                     plain_operand(expression_construct(expression), SXML_STREAM_USAGE_ABSORPTION));
            }
        }
        if (status == SXML_STATUS_OK) {
            operand key = value_operand(ast, child, SXML_STREAM_USAGE_ABSORPTION);
            key.flags = SXML_STREAM_OPERAND_FLAG_CHANGES_FOCUS | SXML_STREAM_OPERAND_FLAG_HIGHER_ORDER;
            key.focus = sort_focus;
            status = add_operand(state, frame_index, key);
        }
    }
    return status;
}

/* use-attribute-sets (19.8.4.1, 19.8.4.12, 19.8.4.15). */
static sxml_status add_attribute_set_operand(machine *state, uint32_t frame_index,
                                             sxml_xslt_ast_node_index node) {
    if (are_streamable_attribute_sets(state->ast, node)) {
        return SXML_STATUS_OK;
    }
    return add_operand(state, frame_index,
                       fixed_operand(roaming_free_ranging(SXML_U_ANY), SXML_STREAM_USAGE_INSPECTION));
}

/* Children of node of kind, each one's sequence constructor (or select)
   an operand with usage in a choice group. */
typedef struct branch_operands {
    sxml_xslt_ast_node_index node;
    usage usage;
    uint32_t choice_group;
    uint32_t flags;
    focus focus;
} branch_operands;

static sxml_status add_branch_operand(machine *state, uint32_t frame_index,
                                      const branch_operands *request) {
    operand value = value_operand(state->ast, request->node, request->usage);
    value.choice_group = request->choice_group;
    value.flags = request->flags;
    value.focus = request->focus;
    return add_operand(state, frame_index, value);
}

/* The operands of an instruction that follows the general rules
   (19.8.4). */
static sxml_status add_instruction_operands(machine *state, uint32_t frame_index) {
    const sxml_xslt_ast *ast = state->ast;
    const sxml_xslt_ast_node_index node = state->frames[frame_index].construct.node;
    const sxml_xslt_ast_kind kind = sxml_xslt_ast_get_node(ast, node)->kind;
    const focus outer = state->frames[frame_index].focus;
    sxml_status status = add_attribute_operands(state, frame_index, node);
    const usage content_usage = xslt_content_usage(ast, node);
    if (status == SXML_STATUS_OK && content_usage != SXML_STREAM_USAGE_NONE) {
        operand content = plain_operand(sequence_constructor_construct(ast, node), content_usage);
        /* xsl:if's content and its then and else attributes (4.0) are a
           choice operand group (19.8.4.21 with 8.1). */
        content.choice_group = kind == SXML_XSLT_AST_KIND_IF ? 1U : 0U;
        status = add_operand(state, frame_index, content);
    }
    if (status == SXML_STATUS_OK && kind == SXML_XSLT_AST_KIND_IF) {
        static const sxml_xslt_ast_attribute_kind branches[2] = {SXML_XSLT_AST_ATTRIBUTE_THEN,
                                                                 SXML_XSLT_AST_ATTRIBUTE_ELSE};
        for (size_t branch_index = 0; status == SXML_STATUS_OK && branch_index < 2U;
             branch_index += 1) {
            const sxml_xslt_ast_expression *branch =
                attribute_expression(ast, node, branches[branch_index]);
            if (branch != NULL) {
                operand value = plain_operand(expression_construct(branch), SXML_STREAM_USAGE_TRANSMISSION);
                value.choice_group = 1U;
                status = add_operand(state, frame_index, value);
            }
        }
    }
    if (status == SXML_STATUS_OK &&
        (kind == SXML_XSLT_AST_KIND_LITERAL_RESULT_ELEMENT || kind == SXML_XSLT_AST_KIND_ELEMENT)) {
        status = add_attribute_set_operand(state, frame_index, node);
    }
    if (status == SXML_STATUS_OK &&
        (kind == SXML_XSLT_AST_KIND_APPLY_IMPORTS || kind == SXML_XSLT_AST_KIND_NEXT_MATCH)) {
        /* 19.8.4.4: an implicit ".", absorbed. */
        status = add_operand(state, frame_index, fixed_operand(context_item(outer), SXML_STREAM_USAGE_ABSORPTION));
    }
    if (status == SXML_STATUS_OK && kind == SXML_XSLT_AST_KIND_CALL_TEMPLATE) {
        /* 19.8.4.9: an implicit "." unless the template prohibits a
           context item. */
        const sxml_xslt_ast_node_index target = called_template(ast, node);
        sxml_xslt_ast_node_index context_item_node = no_xslt_node;
        for (sxml_xslt_ast_node_index child =
                 target.value == 0 ? no_xslt_node : sxml_xslt_ast_get_node(ast, target)->first_child;
             child.value != 0; child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
            if (is_kind(ast, child, SXML_XSLT_AST_KIND_CONTEXT_ITEM)) {
                context_item_node = child;
            }
        }
        if (keyword_attribute(ast, context_item_node, SXML_XSLT_AST_ATTRIBUTE_USE) !=
            SXML_XSLT_AST_KEYWORD_ABSENT) {
            status = add_operand(
                state, frame_index,
                fixed_operand(context_item(outer),
                              type_determined_usage(declared_type_of_node(ast, context_item_node))));
        }
    }
    if (status == SXML_STATUS_OK &&
        (kind == SXML_XSLT_AST_KIND_APPLY_IMPORTS || kind == SXML_XSLT_AST_KIND_NEXT_MATCH ||
         kind == SXML_XSLT_AST_KIND_CALL_TEMPLATE || kind == SXML_XSLT_AST_KIND_NEXT_ITERATION ||
         kind == SXML_XSLT_AST_KIND_EVALUATE)) {
        status = add_with_param_operands(state, frame_index, node);
    }
    if (status == SXML_STATUS_OK && kind == SXML_XSLT_AST_KIND_NUMBER &&
        find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_VALUE) == NULL &&
        find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_SELECT) == NULL) {
        /* 19.8.4.30: select defaults to ".", navigated. */
        status = add_operand(state, frame_index, fixed_operand(context_item(outer), SXML_STREAM_USAGE_NAVIGATION));
    }
    if (status == SXML_STATUS_OK && kind == SXML_XSLT_AST_KIND_SELECT &&
        sxml_xslt_ast_get_node(ast, node)->content.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION) {
        const sxml_xslt_ast_expression *expression = sxml_xslt_ast_get_expression(
            ast, sxml_xslt_ast_get_node(ast, node)->content.payload.expression);
        if (expression->ast != NULL) {
            status = add_operand(state, frame_index,
                                 plain_operand(expression_construct(expression), SXML_STREAM_USAGE_TRANSMISSION));
        }
    }
    for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, node)->first_child;
         status == SXML_STATUS_OK && child.value != 0;
         child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        const sxml_xslt_ast_kind child_kind = sxml_xslt_ast_get_node(ast, child)->kind;
        if (child_kind == SXML_XSLT_AST_KIND_WHEN || child_kind == SXML_XSLT_AST_KIND_OTHERWISE) {
            /* 19.8.4.10: tests inspected (xsl:switch's compared,
               absorbed), branches a choice operand group. */
            const sxml_xslt_ast_expression *test =
                attribute_expression(ast, child, SXML_XSLT_AST_ATTRIBUTE_TEST);
            if (test != NULL) {
                status = add_operand(state, frame_index,
                                     plain_operand(expression_construct(test),
                                                   kind == SXML_XSLT_AST_KIND_SWITCH ? SXML_STREAM_USAGE_ABSORPTION
                                                                                     : SXML_STREAM_USAGE_INSPECTION));
            }
            if (status == SXML_STATUS_OK) {
                status = add_branch_operand(state, frame_index,
                                            &(branch_operands){.node = child,
                                                               .usage = SXML_STREAM_USAGE_TRANSMISSION,
                                                               .choice_group = 1,
                                                               .flags = 0,
                                                               .focus = outer});
            }
        } else if (child_kind == SXML_XSLT_AST_KIND_CATCH) {
            /* 19.8.4.39. */
            status = add_branch_operand(state, frame_index,
                                        &(branch_operands){.node = child,
                                                           .usage = SXML_STREAM_USAGE_TRANSMISSION,
                                                           .choice_group = 1,
                                                           .flags = 0,
                                                           .focus = outer});
        } else if (child_kind == SXML_XSLT_AST_KIND_MATCHING_SUBSTRING ||
                   child_kind == SXML_XSLT_AST_KIND_NON_MATCHING_SUBSTRING) {
            /* 19.8.4.3: navigated, in a grounded focus of strings. */
            status = add_branch_operand(
                state, frame_index,
                &(branch_operands){.node = child,
                                   .usage = SXML_STREAM_USAGE_NAVIGATION,
                                   .choice_group = 0,
                                   .flags = SXML_STREAM_OPERAND_FLAG_GIVEN_FOCUS | SXML_STREAM_OPERAND_FLAG_HIGHER_ORDER,
                                   .focus = {.posture = SXML_STREAM_POSTURE_GROUNDED, .type = SXML_U_STRING}});
        } else if (child_kind == SXML_XSLT_AST_KIND_FALLBACK &&
                   (kind == SXML_XSLT_AST_KIND_EXTENSION_INSTRUCTION ||
                    kind == SXML_XSLT_AST_KIND_UNKNOWN_INSTRUCTION)) {
            /* 19.8.4.2: sxml knows no extension instruction, so it
               performs fallback. */
            status = add_operand(state, frame_index,
                                 plain_operand(sequence_constructor_construct(ast, child),
                                               SXML_STREAM_USAGE_TRANSMISSION));
        }
    }
    return status;
}

/* Pushes the expression of node's attribute, or finishes the phase with
   fallback when there is none. */
typedef struct attribute_push {
    sxml_xslt_ast_attribute_kind kind;
    uint32_t next_phase;
    assessment fallback;
} attribute_push;

static sxml_status push_attribute(machine *state, uint32_t frame_index, attribute_push request) {
    frame *current = &state->frames[frame_index];
    current->phase = request.next_phase;
    const sxml_xslt_ast_expression *expression =
        attribute_expression(state->ast, current->construct.node, request.kind);
    if (expression == NULL) {
        current->has_child = true;
        current->child = request.fallback;
        return SXML_STATUS_OK;
    }
    return push_operand(state, frame_index,
                        plain_operand(expression_construct(expression), SXML_STREAM_USAGE_TRANSMISSION));
}

/* 19.8.4.5. */
static sxml_status step_apply_templates(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_xslt_ast *ast = state->ast;
    const sxml_xslt_ast_node_index node = current->construct.node;
    if (current->phase == 0U) {
        /* Without select, child::node(). */
        return push_attribute(
            state, frame_index,
            (attribute_push){.kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                             .next_phase = 1U,
                             .fallback = assess_bare_step((bare_step){.focus = current->focus,
                                                                      .axis = SXML_STREAM_AXIS_CLASS_CHILD,
                                                                      .test_kinds = SXML_U_NODES})});
    }
    assessment selected;
    if (!take_child(current, &selected)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const bool has_sort = has_child_of_kind(ast, node, SXML_XSLT_AST_KIND_SORT);
    if (selected.posture != SXML_STREAM_POSTURE_GROUNDED) {
        mode_reference mode = {.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT,
                               .name = {.index = 0}};
        const sxml_xslt_ast_attribute *attribute =
            find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_MODE);
        if (attribute != NULL && attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
            const sxml_xslt_ast_name_token_view tokens =
                sxml_xslt_ast_get_name_tokens(ast, attribute->value.payload.name_tokens);
            if (tokens.name_token_count > 0) {
                mode = resolve_mode(ast, node, tokens.name_tokens[0]);
            }
        } else {
            mode = resolve_mode(ast, node,
                                (sxml_xslt_ast_name_token){.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT,
                                                           .name = {.index = 0},
                                                           .namespace_uri = {.index = 0},
                                                           .has_arity = false,
                                                           .arity = 0});
        }
        if (has_sort || !is_streamable_mode(ast, mode) || selected.posture == SXML_STREAM_POSTURE_CLIMBING ||
            selected.posture == SXML_STREAM_POSTURE_CRAWLING) {
            assessment result = roaming_free_ranging(SXML_U_ANY);
            result.is_uncertain = selected.is_uncertain;
            finish_frame(current, result);
            return SXML_STATUS_OK;
        }
    }
    begin_operands(state, frame_index);
    sxml_status status =
        add_operand(state, frame_index, fixed_operand(selected, SXML_STREAM_USAGE_ABSORPTION));
    if (status == SXML_STATUS_OK) {
        status = add_with_param_operands(state, frame_index, node);
    }
    if (status == SXML_STATUS_OK && selected.posture == SXML_STREAM_POSTURE_GROUNDED) {
        status = add_sort_operands(state, frame_index, (sort_request){.node = node, .focus = (focus){.posture = SXML_STREAM_POSTURE_GROUNDED, .type = selected.type}});
    }
    start_general(state, frame_index, return_general(SXML_STREAM_RESULT_ANY));
    return status;
}

/* The phases of xsl:for-each, xsl:for-each-group and xsl:iterate
   (19.8.4.18, 19.8.4.19, 19.8.4.22). */
enum {
    SXML_STREAM_LOOP_PHASE_SELECT = 0,
    SXML_STREAM_LOOP_PHASE_SELECTED = 1,
    /* xsl:for-each-group: the grouping key; xsl:iterate: each parameter
       and xsl:on-completion. */
    SXML_STREAM_LOOP_PHASE_CHECKED = 2,
    SXML_STREAM_LOOP_PHASE_BODY = 3,
};

/* The grounded case: the general rules, the body a higher-order operand
   with a grounded focus. */
static sxml_status start_grounded_loop(machine *state, uint32_t frame_index, assessment selected) {
    const sxml_xslt_ast *ast = state->ast;
    const sxml_xslt_ast_node_index node = state->frames[frame_index].construct.node;
    const sxml_xslt_ast_kind kind = sxml_xslt_ast_get_node(ast, node)->kind;
    const focus grounded = {.posture = SXML_STREAM_POSTURE_GROUNDED, .type = selected.type};
    begin_operands(state, frame_index);
    sxml_status status =
        add_operand(state, frame_index, fixed_operand(selected, SXML_STREAM_USAGE_INSPECTION));
    if (status == SXML_STATUS_OK) {
        status = add_sort_operands(state, frame_index, (sort_request){.node = node, .focus = grounded});
    }
    if (status == SXML_STATUS_OK && kind == SXML_XSLT_AST_KIND_FOR_EACH_GROUP) {
        const sxml_xslt_ast_attribute_list list = sxml_xslt_ast_get_node(ast, node)->attributes;
        for (uint32_t offset = 0; status == SXML_STATUS_OK && offset < list.attribute_count;
             offset += 1U) {
            const sxml_xslt_ast_attribute *attribute = sxml_xslt_ast_get_attribute(
                ast, (sxml_xslt_ast_attribute_index){.value = list.first_attribute_index + offset});
            const bool is_key = attribute->kind == SXML_XSLT_AST_ATTRIBUTE_GROUP_BY ||
                                attribute->kind == SXML_XSLT_AST_ATTRIBUTE_GROUP_ADJACENT;
            const bool is_template = attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE;
            if (!is_key && !is_template) {
                continue;
            }
            const sxml_xslt_ast_expression *expression =
                sxml_xslt_ast_get_expression(ast, attribute->value.payload.expression);
            if (expression->ast == NULL) {
                continue;
            }
            operand value = plain_operand(expression_construct(expression), SXML_STREAM_USAGE_ABSORPTION);
            if (is_key) {
                value.flags = SXML_STREAM_OPERAND_FLAG_CHANGES_FOCUS;
                value.focus = grounded;
            }
            status = add_operand(state, frame_index, value);
        }
    }
    if (status == SXML_STATUS_OK && kind == SXML_XSLT_AST_KIND_ITERATE) {
        for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, node)->first_child;
             status == SXML_STATUS_OK && child.value != 0;
             child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
            if (is_kind(ast, child, SXML_XSLT_AST_KIND_PARAM)) {
                status = add_operand(state, frame_index, value_operand(ast, child, SXML_STREAM_USAGE_NAVIGATION));
            } else if (is_kind(ast, child, SXML_XSLT_AST_KIND_ON_COMPLETION)) {
                operand completion = value_operand(ast, child, SXML_STREAM_USAGE_TRANSMISSION);
                completion.flags = SXML_STREAM_OPERAND_FLAG_GIVEN_FOCUS;
                completion.focus = (focus){.posture = SXML_STREAM_POSTURE_ROAMING, .type = 0U};
                status = add_operand(state, frame_index, completion);
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        operand body = plain_operand(sequence_constructor_construct(ast, node), SXML_STREAM_USAGE_TRANSMISSION);
        body.flags = SXML_STREAM_OPERAND_FLAG_CHANGES_FOCUS;
        if (kind != SXML_XSLT_AST_KIND_ITERATE) {
            body.flags |= SXML_STREAM_OPERAND_FLAG_HIGHER_ORDER;
        }
        if (kind == SXML_XSLT_AST_KIND_FOR_EACH_GROUP) {
            body.flags |= SXML_STREAM_OPERAND_FLAG_GROUP_BODY;
        }
        body.focus = grounded;
        status = add_operand(state, frame_index, body);
    }
    start_general(state, frame_index, return_general(SXML_STREAM_RESULT_ANY));
    return status;
}

/* The next child of node at or after from (0: the first) that the loop
   checks before its body: xsl:param and xsl:on-completion of
   xsl:iterate. */
static sxml_xslt_ast_node_index next_checked_child(const sxml_xslt_ast *ast,
                                                   sxml_xslt_ast_node_index node, uint32_t from) {
    sxml_xslt_ast_node_index child =
        from == 0U ? sxml_xslt_ast_get_node(ast, node)->first_child
                   : sxml_xslt_ast_get_node(ast, node_at(from))->next_sibling;
    while (child.value != 0 && !is_kind(ast, child, SXML_XSLT_AST_KIND_PARAM) &&
           !is_kind(ast, child, SXML_XSLT_AST_KIND_ON_COMPLETION)) {
        child = sxml_xslt_ast_get_node(ast, child)->next_sibling;
    }
    return child;
}

static void finish_roaming(frame *current, bool is_uncertain) {
    assessment result = roaming_free_ranging(SXML_U_ANY);
    result.is_uncertain = is_uncertain;
    finish_frame(current, result);
}

static sxml_status step_loop(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_xslt_ast *ast = state->ast;
    const sxml_xslt_ast_node_index node = current->construct.node;
    const sxml_xslt_ast_kind kind = sxml_xslt_ast_get_node(ast, node)->kind;
    if (current->phase == SXML_STREAM_LOOP_PHASE_SELECT) {
        return push_attribute(state, frame_index,
                              (attribute_push){.kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                               .next_phase = SXML_STREAM_LOOP_PHASE_SELECTED,
                                               .fallback = grounded_motionless(0U)});
    }
    assessment child;
    if (!take_child(current, &child)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (current->phase == SXML_STREAM_LOOP_PHASE_SELECTED) {
        current->saved = child;
        if (child.posture == SXML_STREAM_POSTURE_GROUNDED) {
            /* current-group() of a grounded population is grounded and
               motionless: reading it is the select expression's sweep,
               counted once (as the XT40 cases si-group-A* expect). */
            current->saved = grounded_motionless(child.type);
            return start_grounded_loop(state, frame_index, child);
        }
        const bool has_sort = has_child_of_kind(ast, node, SXML_XSLT_AST_KIND_SORT);
        const bool has_group_by = find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_GROUP_BY) != NULL;
        if ((kind == SXML_XSLT_AST_KIND_FOR_EACH && has_sort) ||
            (kind == SXML_XSLT_AST_KIND_FOR_EACH_GROUP && !current->scope.is_child_of_fork &&
             (has_sort || has_group_by))) {
            finish_roaming(current, child.is_uncertain);
            return SXML_STATUS_OK;
        }
        current->second = grounded_motionless(0U);
        current->cursor = 0U;
        current->phase = SXML_STREAM_LOOP_PHASE_CHECKED;
        if (kind == SXML_XSLT_AST_KIND_FOR_EACH_GROUP) {
            const sxml_xslt_ast_expression *key =
                attribute_expression(ast, node, SXML_XSLT_AST_ATTRIBUTE_GROUP_BY);
            if (key == NULL) {
                key = attribute_expression(ast, node, SXML_XSLT_AST_ATTRIBUTE_GROUP_ADJACENT);
            }
            if (key != NULL) {
                return push_operand(state, frame_index,
                                    focused_operand(expression_construct(key), focus_of(child)));
            }
            current->has_child = true;
            current->child = grounded_motionless(0U);
            return SXML_STATUS_OK;
        }
        current->has_child = true;
        current->child = grounded_motionless(0U);
        return SXML_STATUS_OK;
    }
    if (current->phase == SXML_STREAM_LOOP_PHASE_CHECKED) {
        current->second.is_uncertain = current->second.is_uncertain || child.is_uncertain;
        /* The grouping key must be motionless; xsl:iterate's parameters
           and xsl:on-completion grounded and motionless. */
        const bool fails = kind == SXML_XSLT_AST_KIND_ITERATE
                               ? child.posture != SXML_STREAM_POSTURE_GROUNDED || child.sweep != SXML_STREAM_SWEEP_MOTIONLESS
                               : child.sweep != SXML_STREAM_SWEEP_MOTIONLESS || is_free(child);
        if (fails) {
            finish_roaming(current, current->second.is_uncertain || current->saved.is_uncertain);
            return SXML_STATUS_OK;
        }
        if (kind == SXML_XSLT_AST_KIND_FOR_EACH_GROUP && current->cursor == 0U) {
            /* group-starting-with and group-ending-with must be motionless
               patterns to group streamed nodes (14.5 with 19.8.10). */
            current->cursor = 1U;
            const sxml_xslt_ast_expression *pattern =
                attribute_expression(ast, node, SXML_XSLT_AST_ATTRIBUTE_GROUP_STARTING_WITH);
            if (pattern == NULL) {
                pattern = attribute_expression(ast, node, SXML_XSLT_AST_ATTRIBUTE_GROUP_ENDING_WITH);
            }
            if (pattern != NULL) {
                operand value = plain_operand(pattern_construct(pattern), SXML_STREAM_USAGE_INSPECTION);
                value.flags = SXML_STREAM_OPERAND_FLAG_GIVEN_FOCUS;
                value.focus = striding_focus(SXML_U_ANY);
                return push_operand(state, frame_index, value);
            }
        }
        if (kind == SXML_XSLT_AST_KIND_ITERATE) {
            const sxml_xslt_ast_node_index next = next_checked_child(ast, node, current->cursor);
            if (next.value != 0) {
                current->cursor = next.value;
                operand value = value_operand(ast, next, SXML_STREAM_USAGE_TRANSMISSION);
                if (is_kind(ast, next, SXML_XSLT_AST_KIND_ON_COMPLETION)) {
                    value.flags = SXML_STREAM_OPERAND_FLAG_GIVEN_FOCUS;
                    value.focus = (focus){.posture = SXML_STREAM_POSTURE_ROAMING, .type = 0U};
                }
                return push_operand(state, frame_index, value);
            }
        }
        current->phase = SXML_STREAM_LOOP_PHASE_BODY;
        operand body = focused_operand(sequence_constructor_construct(ast, node), focus_of(current->saved));
        if (kind == SXML_XSLT_AST_KIND_FOR_EACH_GROUP) {
            body.flags |= SXML_STREAM_OPERAND_FLAG_GROUP_BODY;
        }
        return push_operand(state, frame_index, body);
    }
    const assessment selected = current->saved;
    assessment result = normalized(make_assessment(child.posture, wider_sweep(selected.sweep, child.sweep),
                                                   child.type));
    if (selected.posture == SXML_STREAM_POSTURE_CRAWLING && child.sweep == SXML_STREAM_SWEEP_CONSUMING) {
        result = roaming_free_ranging(child.type);
    }
    result.is_uncertain = selected.is_uncertain || child.is_uncertain || current->second.is_uncertain;
    finish_frame(current, result);
    return SXML_STATUS_OK;
}

/* 19.8.4.20. */
static sxml_status step_fork(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_xslt_ast *ast = state->ast;
    const sxml_xslt_ast_node_index node = current->construct.node;
    if (current->phase == 1U) {
        assessment child;
        if (!take_child(current, &child)) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        finish_frame(current, child);
        return SXML_STATUS_OK;
    }
    for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, node)->first_child;
         child.value != 0; child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        if (is_kind(ast, child, SXML_XSLT_AST_KIND_FOR_EACH_GROUP)) {
            current->phase = 1U;
            operand group = plain_operand(xslt_construct(ast, child), SXML_STREAM_USAGE_TRANSMISSION);
            group.flags = SXML_STREAM_OPERAND_FLAG_CHILD_OF_FORK;
            return push_operand(state, frame_index, group);
        }
    }
    begin_operands(state, frame_index);
    sxml_status status = SXML_STATUS_OK;
    for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, node)->first_child;
         status == SXML_STATUS_OK && child.value != 0;
         child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        if (is_kind(ast, child, SXML_XSLT_AST_KIND_SEQUENCE)) {
            status = add_operand(state, frame_index,
                                 plain_operand(xslt_construct(ast, child), SXML_STREAM_USAGE_TRANSMISSION));
        }
    }
    start_general(state, frame_index,
                  (general_start){.finish = {.combination = SXML_STREAM_COMBINATION_FORK,
                                             .is_at_most_one = false,
                                             .type = SXML_U_ANY},
                                  .after_phase = SXML_STREAM_PHASE_RETURN_GENERAL});
    return status;
}

/* 19.8.4.23. */
static sxml_status start_map(machine *state, uint32_t frame_index) {
    const sxml_xslt_ast *ast = state->ast;
    const sxml_xslt_ast_node_index node = state->frames[frame_index].construct.node;
    bool has_only_entries = true;
    for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, node)->first_child;
         child.value != 0; child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        if (!is_kind(ast, child, SXML_XSLT_AST_KIND_MAP_ENTRY) &&
            !is_kind(ast, child, SXML_XSLT_AST_KIND_FALLBACK)) {
            has_only_entries = false;
        }
    }
    /* With duplicates (4.0) the entries are combined in order: no
       parallel evaluation (as the XT40 cases si-map-010 to -012 expect). */
    if (find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_DUPLICATES) != NULL) {
        has_only_entries = false;
    }
    begin_operands(state, frame_index);
    sxml_status status = SXML_STATUS_OK;
    if (!has_only_entries) {
        status = add_operand(state, frame_index,
                             plain_operand(sequence_constructor_construct(ast, node), SXML_STREAM_USAGE_TRANSMISSION));
        start_general(state, frame_index, return_general(SXML_STREAM_RESULT_FUNCTION));
        return status;
    }
    for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, node)->first_child;
         status == SXML_STATUS_OK && child.value != 0;
         child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        if (is_kind(ast, child, SXML_XSLT_AST_KIND_MAP_ENTRY)) {
            status = add_operand(state, frame_index,
                                 plain_operand(xslt_construct(ast, child), SXML_STREAM_USAGE_TRANSMISSION));
        }
    }
    start_general(state, frame_index,
                  (general_start){.finish = {.combination = SXML_STREAM_COMBINATION_MAP,
                                             .is_at_most_one = false,
                                             .type = SXML_U_FUNCTION},
                                  .after_phase = SXML_STREAM_PHASE_RETURN_GENERAL});
    return status;
}

/* 19.8.4.25: grounded and motionless when every xsl:merge-source's
   for-each-item and for-each-source are, or its select is when it has
   neither; else roaming. */
static sxml_status step_merge(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_xslt_ast *ast = state->ast;
    assessment child;
    if (take_child(current, &child)) {
        current->second.is_uncertain = current->second.is_uncertain || child.is_uncertain;
        if (child.posture != SXML_STREAM_POSTURE_GROUNDED || child.sweep != SXML_STREAM_SWEEP_MOTIONLESS) {
            current->flag = true;
        }
    }
    /* cursor: the merge source; counter: the next of its three
       attributes. */
    while (true) {
        sxml_xslt_ast_node_index source =
            current->cursor == 0U ? sxml_xslt_ast_get_node(ast, current->construct.node)->first_child
                                  : node_at(current->cursor);
        while (source.value != 0 && !is_kind(ast, source, SXML_XSLT_AST_KIND_MERGE_SOURCE)) {
            source = sxml_xslt_ast_get_node(ast, source)->next_sibling;
            current->counter = 0U;
        }
        if (source.value == 0) {
            break;
        }
        current->cursor = source.value;
        const bool has_for_each =
            find_xslt_attribute(ast, source, SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_ITEM) != NULL ||
            find_xslt_attribute(ast, source, SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_SOURCE) != NULL;
        static const sxml_xslt_ast_attribute_kind checked[3] = {
            SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_ITEM, SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_SOURCE,
            SXML_XSLT_AST_ATTRIBUTE_SELECT};
        while (current->counter < 3U) {
            const sxml_xslt_ast_attribute_kind kind = checked[current->counter];
            current->counter += 1U;
            if (kind == SXML_XSLT_AST_ATTRIBUTE_SELECT && has_for_each) {
                continue;
            }
            const sxml_xslt_ast_expression *expression = attribute_expression(ast, source, kind);
            if (expression != NULL) {
                return push_operand(state, frame_index,
                                    plain_operand(expression_construct(expression), SXML_STREAM_USAGE_TRANSMISSION));
            }
        }
        current->counter = 0U;
        current->cursor = sxml_xslt_ast_get_node(ast, source)->next_sibling.value;
        if (current->cursor == 0U) {
            break;
        }
    }
    assessment result = current->flag ? roaming_free_ranging(SXML_U_ANY) : grounded_motionless(SXML_U_ANY);
    result.is_uncertain = current->second.is_uncertain;
    finish_frame(current, result);
    return SXML_STATUS_OK;
}

/* 19.8.4.37: roaming when the body uses current-group() or
   current-merge-group() of an enclosing instruction, else grounded with
   the sweep of href. The body's own streamability is checked where the
   instruction is declared streamable (check_source_document). */
static sxml_status step_source_document(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_xslt_ast *ast = state->ast;
    const sxml_xslt_ast_node_index node = current->construct.node;
    if (current->phase == 0U) {
        return push_attribute(state, frame_index,
                              (attribute_push){.kind = SXML_XSLT_AST_ATTRIBUTE_HREF,
                                               .next_phase = 1U,
                                               .fallback = grounded_motionless(SXML_U_STRING)});
    }
    assessment child;
    if (!take_child(current, &child)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (current->phase == 1U) {
        current->saved = child;
        current->phase = 2U;
        operand body = plain_operand(sequence_constructor_construct(ast, node), SXML_STREAM_USAGE_TRANSMISSION);
        body.flags = SXML_STREAM_OPERAND_FLAG_GIVEN_FOCUS | SXML_STREAM_OPERAND_FLAG_SOURCE_DOCUMENT_BODY;
        body.focus = (focus){.posture = is_true_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_STREAMABLE)
                                            ? SXML_STREAM_POSTURE_STRIDING
                                            : SXML_STREAM_POSTURE_GROUNDED,
                             .type = SXML_U_DOCUMENT};
        return push_operand(state, frame_index, body);
    }
    const assessment href = current->saved;
    assessment result = current->is_poisoned
                            ? roaming_free_ranging(SXML_U_ANY)
                            : normalized(make_assessment(SXML_STREAM_POSTURE_GROUNDED, href.sweep, SXML_U_ANY));
    result.is_uncertain = href.is_uncertain;
    finish_frame(current, result);
    return SXML_STATUS_OK;
}

/* 19.8.4.12, with a streamed select assessed as xsl:for-each over the
   selection with an xsl:copy of each item: the content in the focus of
   the selection, the sweeps of both. 19.8.4.12 makes the content a
   higher-order operand, so that nested xsl:copy select="*" would be
   roaming; the XT40 cases si-copy-* expect them streamable. */
static sxml_status step_copy(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_xslt_ast *ast = state->ast;
    const sxml_xslt_ast_node_index node = current->construct.node;
    if (current->phase == 0U) {
        return push_attribute(state, frame_index,
                              (attribute_push){.kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                               .next_phase = 1U,
                                               .fallback = context_item(current->focus)});
    }
    assessment child;
    if (!take_child(current, &child)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const bool has_select = find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_SELECT) != NULL;
    if (current->phase == 2U) {
        const assessment selected = current->saved;
        general_state copy = empty_general_state();
        general_add(&copy, (general_operand){.value = context_item(focus_of(selected)),
                                             .usage = SXML_STREAM_USAGE_INSPECTION,
                                             .is_higher_order = false,
                                             .choice_group = 0});
        general_add(&copy, (general_operand){.value = child,
                                             .usage = SXML_STREAM_USAGE_ABSORPTION,
                                             .is_higher_order = false,
                                             .choice_group = 0});
        if (!are_streamable_attribute_sets(ast, node)) {
            general_add(&copy, (general_operand){.value = roaming_free_ranging(SXML_U_ANY),
                                                 .usage = SXML_STREAM_USAGE_INSPECTION,
                                                 .is_higher_order = false,
                                                 .choice_group = 0});
        }
        const assessment each = general_result(&copy, (general_finish){.combination = SXML_STREAM_COMBINATION_GENERAL,
                                                                       .is_at_most_one = false,
                                                                       .type = SXML_U_ANY});
        assessment result =
            selected.posture == SXML_STREAM_POSTURE_CRAWLING && each.sweep == SXML_STREAM_SWEEP_CONSUMING
                ? roaming_free_ranging(SXML_U_ANY)
                : normalized(make_assessment(each.posture, wider_sweep(selected.sweep, each.sweep), SXML_U_ANY));
        result.is_uncertain = selected.is_uncertain || each.is_uncertain;
        finish_frame(current, result);
        return SXML_STATUS_OK;
    }
    const assessment selected = child;
    if (has_select && selected.posture != SXML_STREAM_POSTURE_GROUNDED) {
        current->saved = selected;
        current->phase = 2U;
        return push_operand(state, frame_index,
                            focused_operand(sequence_constructor_construct(ast, node), focus_of(selected)));
    }
    begin_operands(state, frame_index);
    sxml_status status = add_operand(state, frame_index, fixed_operand(selected, SXML_STREAM_USAGE_INSPECTION));
    if (status == SXML_STATUS_OK) {
        operand content = plain_operand(sequence_constructor_construct(ast, node), SXML_STREAM_USAGE_ABSORPTION);
        if (has_select) {
            content.flags = SXML_STREAM_OPERAND_FLAG_CHANGES_FOCUS | SXML_STREAM_OPERAND_FLAG_HIGHER_ORDER;
            content.focus = focus_of(selected);
        }
        status = add_operand(state, frame_index, content);
    }
    if (status == SXML_STATUS_OK) {
        status = add_attribute_set_operand(state, frame_index, node);
    }
    start_general(state, frame_index, return_general(SXML_STREAM_RESULT_ANY));
    return status;
}

/* 19.8.4.33. */
static sxml_status step_perform_sort(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_xslt_ast *ast = state->ast;
    const sxml_xslt_ast_node_index node = current->construct.node;
    if (current->phase == 0U) {
        return push_attribute(state, frame_index,
                              (attribute_push){.kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                               .next_phase = 1U,
                                               .fallback = grounded_motionless(0U)});
    }
    assessment selected;
    if (!take_child(current, &selected)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    begin_operands(state, frame_index);
    sxml_status status = add_operand(state, frame_index, fixed_operand(selected, SXML_STREAM_USAGE_NAVIGATION));
    if (status == SXML_STATUS_OK) {
        status = add_sort_operands(state, frame_index, (sort_request){.node = node, .focus = focus_of(selected)});
    }
    if (status == SXML_STATUS_OK && find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_SELECT) == NULL) {
        status = add_operand(state, frame_index,
                             plain_operand(sequence_constructor_construct(ast, node), SXML_STREAM_USAGE_NAVIGATION));
    }
    start_general(state, frame_index, return_general(SXML_STREAM_RESULT_ANY));
    return status;
}

static sxml_status step_xslt(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    if (current->phase == SXML_STREAM_PHASE_GENERAL) {
        return step_general(state, frame_index);
    }
    if (finishes_general(current)) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_node *entry = sxml_xslt_ast_get_node(state->ast, current->construct.node);
    const sxml_xslt_ast_kind kind = entry->kind;
    if (sxml_xslt_ast_get_static_context(state->ast, entry->static_context)->version < 2.0) {
        /* An instruction with XSLT 1.0 behaviour (3.8.1) is roaming and
           free-ranging, as the XT40 case streamable-141 expects. */
        finish_frame(current, roaming_free_ranging(SXML_U_ANY));
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    switch (rule_of_xslt_kind(kind)) {
    case SXML_STREAM_XSLT_RULE_UNCERTAIN: {
        assessment result = grounded_motionless(SXML_U_ANY);
        result.is_uncertain = true;
        finish_frame(current, result);
        return SXML_STATUS_OK;
    }
    case SXML_STREAM_XSLT_RULE_GROUNDED:
        finish_frame(current, grounded_motionless(SXML_U_ANY));
        return SXML_STATUS_OK;
    case SXML_STREAM_XSLT_RULE_GENERAL:
        begin_operands(state, frame_index);
        status = add_instruction_operands(state, frame_index);
        start_general(state, frame_index, return_general(SXML_STREAM_RESULT_ANY));
        return status;
    case SXML_STREAM_XSLT_RULE_APPLY_TEMPLATES:
        return step_apply_templates(state, frame_index);
    case SXML_STREAM_XSLT_RULE_FOR_EACH:
    case SXML_STREAM_XSLT_RULE_FOR_EACH_GROUP:
    case SXML_STREAM_XSLT_RULE_ITERATE:
        return step_loop(state, frame_index);
    case SXML_STREAM_XSLT_RULE_FORK:
        return step_fork(state, frame_index);
    case SXML_STREAM_XSLT_RULE_MAP:
        return start_map(state, frame_index);
    case SXML_STREAM_XSLT_RULE_MERGE:
        return step_merge(state, frame_index);
    case SXML_STREAM_XSLT_RULE_SOURCE_DOCUMENT:
        return step_source_document(state, frame_index);
    case SXML_STREAM_XSLT_RULE_COPY:
        return step_copy(state, frame_index);
    case SXML_STREAM_XSLT_RULE_PERFORM_SORT:
        return step_perform_sort(state, frame_index);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* 19.8.3: the instructions transmitted, text value templates absorbed. */
static sxml_status step_sequence_constructor(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    if (current->phase == SXML_STREAM_PHASE_GENERAL) {
        return step_general(state, frame_index);
    }
    if (finishes_general(current)) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast *ast = state->ast;
    begin_operands(state, frame_index);
    sxml_status status = SXML_STATUS_OK;
    for (sxml_xslt_ast_node_index child =
             sxml_xslt_ast_get_node(ast, current->construct.node)->first_child;
         status == SXML_STATUS_OK && child.value != 0;
         child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        const sxml_xslt_ast_node *entry = sxml_xslt_ast_get_node(ast, child);
        operand value = plain_operand(xslt_construct(ast, child), SXML_STREAM_USAGE_TRANSMISSION);
        if (entry->kind == SXML_XSLT_AST_KIND_TEXT_NODE) {
            if (entry->content.kind != SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE) {
                continue;
            }
            const sxml_xslt_ast_expression *expression =
                sxml_xslt_ast_get_expression(ast, entry->content.payload.expression);
            if (expression->ast == NULL) {
                continue;
            }
            value = plain_operand(expression_construct(expression), SXML_STREAM_USAGE_ABSORPTION);
        } else if (!is_instruction_kind(entry->kind)) {
            continue;
        }
        value.flags = SXML_STREAM_OPERAND_FLAG_SEQUENCE_CONSTRUCTOR_CHILD;
        status = add_operand(state, frame_index, value);
    }
    start_general(state, frame_index, return_general(SXML_STREAM_RESULT_ANY));
    return status;
}

/* ------------------------------------------------------------- Patterns */

static bool is_pattern_kind(sxml_ast_kind kind) {
    return kind >= SXML_AST_KIND_PATTERN && kind <= SXML_AST_KIND_SEQUENCE_PATTERN;
}

/* The U-type of the items a step of a pattern matches (19.2: the match
   type is that of the pattern's equivalent expression). */
static u_type pattern_step_type(const sxml_ast *ast, sxml_ast_node_index step) {
    const sxml_ast_node *entry = sxml_ast_get_node(ast, step);
    if (entry->kind == SXML_AST_KIND_AXIS_STEP_PATTERN) {
        const axis_class axis = axis_of_step(ast, step);
        return reachable_kinds(axis, SXML_U_ANY) &
               node_test_kinds(ast, child_by_field(ast, step, SXML_AST_FIELD_TEST), axis);
    }
    if (entry->kind == SXML_AST_KIND_SIMPLE_PATTERN) {
        const sxml_ast_kind item_kind =
            sxml_ast_get_node(ast, child_by_field(ast, step, SXML_AST_FIELD_ITEM))->kind;
        if (item_kind == SXML_AST_KIND_ATOMIC_PATTERN) {
            return SXML_U_ATOMIC;
        }
        if (item_kind == SXML_AST_KIND_MAP_PATTERN || item_kind == SXML_AST_KIND_RECORD_PATTERN ||
            item_kind == SXML_AST_KIND_ARRAY_PATTERN) {
            return SXML_U_FUNCTION;
        }
        return SXML_U_ANY;
    }
    if (entry->kind == SXML_AST_KIND_FUNCTION_CALL_PATTERN ||
        entry->kind == SXML_AST_KIND_FILTER_PATTERN) {
        return SXML_U_NODES;
    }
    return SXML_U_ANY;
}

/* The match type of a pattern (19.2), with a small fixed work list; a
   pattern deeper than it is U{*}. */
static u_type pattern_match_type(const sxml_ast *ast) {
    enum { SXML_STREAM_WORK_CAPACITY = 32 };
    sxml_ast_node_index work[SXML_STREAM_WORK_CAPACITY];
    uint32_t work_count = 0;
    u_type type = 0U;
    work[work_count] = ast->root;
    work_count += 1U;
    while (work_count > 0U) {
        work_count -= 1U;
        const sxml_ast_node_index node = work[work_count];
        const sxml_ast_node *entry = sxml_ast_get_node(ast, node);
        sxml_ast_node_index next[2] = {no_ast_node, no_ast_node};
        if (entry->kind == SXML_AST_KIND_PATTERN || entry->kind == SXML_AST_KIND_PARENTHESIZED_PATTERN) {
            next[0] = child_by_field(ast, node, SXML_AST_FIELD_PATTERN);
        } else if (entry->kind == SXML_AST_KIND_UNION_PATTERN) {
            next[0] = child_by_field(ast, node, SXML_AST_FIELD_LEFT);
            next[1] = child_by_field(ast, node, SXML_AST_FIELD_RIGHT);
        } else if (entry->kind == SXML_AST_KIND_INTERSECT_EXCEPT_PATTERN) {
            next[0] = child_by_field(ast, node, SXML_AST_FIELD_LEFT);
        } else if (entry->kind == SXML_AST_KIND_RELATIVE_PATH_PATTERN) {
            next[0] = child_by_field(ast, node, SXML_AST_FIELD_RIGHT);
        } else if (entry->kind == SXML_AST_KIND_ABSOLUTE_PATH_PATTERN ||
                   entry->kind == SXML_AST_KIND_ROOTED_PATH_PATTERN) {
            next[0] = child_by_field(ast, node, SXML_AST_FIELD_PATH);
            if (next[0].value == 0) {
                type |= entry->kind == SXML_AST_KIND_ABSOLUTE_PATH_PATTERN ? SXML_U_DOCUMENT : SXML_U_ANY;
            }
        } else {
            type |= pattern_step_type(ast, node);
        }
        for (uint32_t next_index = 0; next_index < 2U; next_index += 1U) {
            if (next[next_index].value == 0) {
                continue;
            }
            if (work_count == SXML_STREAM_WORK_CAPACITY) {
                return SXML_U_ANY;
            }
            work[work_count] = next[next_index];
            work_count += 1U;
        }
    }
    return type == 0U ? SXML_U_ANY : type;
}

/* 19.8.10: a pattern is motionless when it has no RootedPath, no
   reference to the streaming parameter, and every top-level predicate is
   motionless (in a striding focus of its step's type) and
   non-positional. phase 0 checks the first two; cursor walks the AST's
   nodes for top-level predicates (their parents are pattern kinds). */
static sxml_status step_pattern(machine *state, uint32_t frame_index) {
    frame *current = &state->frames[frame_index];
    const sxml_ast *ast = current->construct.ast;
    if (current->phase == 0U) {
        for (uint32_t index = 1; index < ast->node_count; index += 1U) {
            const sxml_ast_kind kind = sxml_ast_get_node(ast, (sxml_ast_node_index){.value = index})->kind;
            if (kind == SXML_AST_KIND_ROOTED_PATH_PATTERN || kind == SXML_AST_KIND_FUNCTION_CALL_PATTERN) {
                finish_frame(current, roaming_free_ranging(SXML_U_BOOLEAN));
                return SXML_STATUS_OK;
            }
        }
        const scan_result scan = scan_expression(
            &state->lookup,
            &(scan_request){.ast = ast,
                            .root = ast->root,
                            .static_context = current->construct.static_context,
                            .skips_predicates = false,
                            .skips_controlled_operands = false,
                            .streaming_parameter = streaming_parameter_name(state, &current->scope)});
        if (scan.references_streaming_parameter) {
            finish_frame(current, roaming_free_ranging(SXML_U_BOOLEAN));
            return SXML_STATUS_OK;
        }
        current->scope.is_in_pattern = true;
        /* current() in a predicate is the matched item (19.1.2). */
        current->scope.expression_focus = (focus){.posture = SXML_STREAM_POSTURE_STRIDING,
                                                  .type = pattern_match_type(ast)};
        current->cursor = 1U;
        current->phase = 1U;
        current->second = grounded_motionless(SXML_U_BOOLEAN);
    }
    assessment child;
    if (take_child(current, &child)) {
        const sxml_ast_node_index expression =
            predicate_expression(ast, (sxml_ast_node_index){.value = current->cursor - 1U});
        const scan_result scan = scan_predicate(state, current, (predicate_scan){.expression = expression, .skips_controlled_operands = false});
        current->second.is_uncertain = current->second.is_uncertain || child.is_uncertain;
        if (child.sweep != SXML_STREAM_SWEEP_MOTIONLESS || is_free(child) ||
            (predicate_type(ast, expression, child.type) & SXML_U_NUMERIC) != 0U ||
            scan.has_positional_call) {
            assessment result = roaming_free_ranging(SXML_U_BOOLEAN);
            result.is_uncertain = current->second.is_uncertain;
            finish_frame(current, result);
            return SXML_STATUS_OK;
        }
    }
    while (current->cursor < ast->node_count) {
        const sxml_ast_node_index node = {.value = current->cursor};
        current->cursor += 1U;
        const sxml_ast_node *entry = sxml_ast_get_node(ast, node);
        if (entry->kind != SXML_AST_KIND_PREDICATE ||
            !is_pattern_kind(sxml_ast_get_node(ast, entry->parent)->kind)) {
            continue;
        }
        const focus predicate_focus = {.posture = SXML_STREAM_POSTURE_STRIDING,
                                       .type = pattern_step_type(ast, entry->parent)};
        operand value = focused_operand(
            xpath_construct(ast, predicate_expression(ast, node), current->construct.static_context),
            predicate_focus);
        return push_operand(state, frame_index, value);
    }
    finish_frame(current, current->second);
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------- The loop */

static sxml_status step_frame(machine *state, uint32_t frame_index) {
    switch (state->frames[frame_index].construct.source) {
    case SXML_STREAM_CONSTRUCT_SOURCE_XPATH:
        return step_xpath(state, frame_index);
    case SXML_STREAM_CONSTRUCT_SOURCE_XSLT:
        return step_xslt(state, frame_index);
    case SXML_STREAM_CONSTRUCT_SOURCE_SEQUENCE_CONSTRUCTOR:
        return step_sequence_constructor(state, frame_index);
    case SXML_STREAM_CONSTRUCT_SOURCE_PATTERN:
        return step_pattern(state, frame_index);
    case SXML_STREAM_CONSTRUCT_SOURCE_NONE:
    case SXML_STREAM_CONSTRUCT_SOURCE_FIXED:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Assesses one construct, running the machine until its frame has its
   result. A construct that needs more frames than the limit is
   uncertain. */
static sxml_status assess(machine *state, const frame_request *request, assessment *result) {
    *result = grounded_motionless(SXML_U_ANY);
    result->is_uncertain = true;
    state->frame_count = 0;
    state->operand_count = 0;
    state->is_limit_reached = false;
    sxml_status status = push_frame(state, request);
    while (status == SXML_STATUS_OK && state->frame_count > 0U) {
        const uint32_t top = state->frame_count - 1U;
        status = step_frame(state, top);
        if (status != SXML_STATUS_OK || state->frame_count != top + 1U ||
            !state->frames[top].is_done) {
            continue;
        }
        const assessment done = state->frames[top].result;
        state->operand_count = state->frames[top].operand_base;
        state->frame_count = top;
        if (top == 0U) {
            *result = done;
        } else {
            state->frames[top - 1U].has_child = true;
            state->frames[top - 1U].child = done;
        }
    }
    if (status == SXML_STATUS_LIMIT_EXCEEDED && state->is_limit_reached) {
        state->frame_count = 0;
        state->operand_count = 0;
        *result = grounded_motionless(SXML_U_ANY);
        result->is_uncertain = true;
        return SXML_STATUS_OK;
    }
    return status;
}

/* ------------------------------------------------------------ Verdicts */

/* Whether a declared-streamable construct is guaranteed-streamable, and
   why not. */
typedef struct verdict {
    bool is_guaranteed;
    bool is_uncertain;
    const char *reason;
} verdict;

static verdict guaranteed(void) {
    return (verdict){.is_guaranteed = true, .is_uncertain = false, .reason = NULL};
}

/* Records a failed condition (the first one counts). */
typedef struct condition {
    bool holds;
    bool is_uncertain;
    const char *reason;
} condition;

static void require(verdict *outcome, condition checked) {
    outcome->is_uncertain = outcome->is_uncertain || checked.is_uncertain;
    if (!checked.holds && outcome->is_guaranteed) {
        outcome->is_guaranteed = false;
        outcome->reason = checked.reason;
    }
}


static sxml_status assess_pattern(machine *state, const sxml_xslt_ast_expression *pattern,
                                  assessment *result) {
    scope pattern_scope = empty_scope();
    pattern_scope.is_in_pattern = true;
    return assess(state,
                  &(frame_request){.construct = pattern_construct(pattern),
                                   .focus = striding_focus(SXML_U_ANY),
                                   .scope = pattern_scope},
                  result);
}


/* 6.6.4 "Streamable Templates". */
static sxml_status check_template_rule(machine *state, sxml_xslt_ast_node_index node,
                                       verdict *outcome) {
    const sxml_xslt_ast *ast = state->ast;
    const sxml_xslt_ast_expression *match =
        attribute_expression(ast, node, SXML_XSLT_AST_ATTRIBUTE_MATCH);
    if (match == NULL) {
        return SXML_STATUS_OK;
    }
    assessment pattern;
    sxml_status status = assess_pattern(state, match, &pattern);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    require(outcome, (condition){.holds = is_motionless(pattern),
                                 .is_uncertain = pattern.is_uncertain,
                                 .reason = "a template rule of a streamable mode is not guaranteed-streamable (XSLT 4.0, 6.6.4): its match pattern is not motionless"});
    const focus body_focus = striding_focus(pattern_match_type(match->ast));
    assessment body;
    status = assess(state,
                    &(frame_request){.construct = sequence_constructor_construct(ast, node),
                                     .focus = body_focus,
                                     .scope = empty_scope()},
                    &body);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const assessment adjusted =
        type_adjusted(body, type_determined_usage(declared_type_of_node(ast, node)));
    require(outcome, (condition){.holds = body.sweep != SXML_STREAM_SWEEP_FREE_RANGING &&
                                          adjusted.posture == SXML_STREAM_POSTURE_GROUNDED,
                                 .is_uncertain = body.is_uncertain,
                                 .reason = body.sweep == SXML_STREAM_SWEEP_FREE_RANGING
                                               ? "a template rule of a streamable mode is not "
                                                 "guaranteed-streamable (XSLT 4.0, 6.6.4): its body "
                                                 "is free-ranging"
                                               : "a template rule of a streamable mode is not "
                                                 "guaranteed-streamable (XSLT 4.0, 6.6.4): its body "
                                                 "is not grounded"});
    for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, node)->first_child;
         status == SXML_STATUS_OK && child.value != 0;
         child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        if (!is_kind(ast, child, SXML_XSLT_AST_KIND_PARAM)) {
            continue;
        }
        assessment initializer;
        status = assess(state,
                        &(frame_request){.construct = value_operand(ast, child, SXML_STREAM_USAGE_TRANSMISSION).construct,
                                         .focus = body_focus,
                                         .scope = empty_scope()},
                        &initializer);
        if (status == SXML_STATUS_OK) {
            require(outcome, (condition){.holds = is_motionless(initializer),
                                         .is_uncertain = initializer.is_uncertain,
                                         .reason = "a template rule of a streamable mode is not guaranteed-streamable (XSLT 4.0, 6.6.4): a parameter's default value is not motionless"});
        }
    }
    return status;
}

/* True when a template rule is applicable to a mode declared
   streamable="yes" (6.6.4): its mode list names one, #all and some mode
   is, or it is inside such an xsl:mode (6.9.6). */
static bool is_in_streamable_mode(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_node_index parent = sxml_xslt_ast_get_node(ast, node)->parent;
    if (is_kind(ast, parent, SXML_XSLT_AST_KIND_MODE)) {
        return is_true_attribute(ast, parent, SXML_XSLT_AST_ATTRIBUTE_STREAMABLE);
    }
    const sxml_xslt_ast_attribute *attribute =
        find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_MODE);
    if (attribute == NULL || attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
        return is_streamable_mode(
            ast, resolve_mode(ast, node,
                              (sxml_xslt_ast_name_token){.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT,
                                                         .name = {.index = 0},
                                                         .namespace_uri = {.index = 0},
                                                         .has_arity = false,
                                                         .arity = 0}));
    }
    const sxml_xslt_ast_name_token_view tokens =
        sxml_xslt_ast_get_name_tokens(ast, attribute->value.payload.name_tokens);
    for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
        const sxml_xslt_ast_name_token token = tokens.name_tokens[token_index];
        if (token.kind == SXML_XSLT_AST_NAME_TOKEN_KIND_ALL ? has_streamable_mode(ast)
                                                           : token.kind != SXML_XSLT_AST_NAME_TOKEN_KIND_CURRENT &&
                                                                 is_streamable_mode(ast, resolve_mode(ast, node, token))) {
            return true;
        }
    }
    return false;
}

/* 18.1: the body, in a striding focus of a document node, is grounded
   and not free-ranging. */
static sxml_status check_source_document(machine *state, sxml_xslt_ast_node_index node,
                                         verdict *outcome) {
    scope body_scope = empty_scope();
    for (sxml_xslt_ast_node_index ancestor = sxml_xslt_ast_get_node(state->ast, node)->parent;
         ancestor.value != 0; ancestor = sxml_xslt_ast_get_node(state->ast, ancestor)->parent) {
        if (is_kind(state->ast, ancestor, SXML_XSLT_AST_KIND_FOR_EACH_GROUP) ||
            is_kind(state->ast, ancestor, SXML_XSLT_AST_KIND_MERGE)) {
            body_scope.group_state = SXML_STREAM_GROUP_STATE_OTHER_DOCUMENT;
        }
    }
    assessment body;
    const sxml_status status =
        assess(state,
               &(frame_request){.construct = sequence_constructor_construct(state->ast, node),
                                .focus = striding_focus(SXML_U_DOCUMENT),
                                .scope = body_scope},
               &body);
    if (status == SXML_STATUS_OK) {
        require(outcome, (condition){.holds = body.posture == SXML_STREAM_POSTURE_GROUNDED &&
                                              body.sweep != SXML_STREAM_SWEEP_FREE_RANGING,
                                     .is_uncertain = body.is_uncertain,
                                     .reason = "xsl:source-document with streamable=\"yes\" is not guaranteed-streamable (XSLT 4.0, 18.1): its content is not grounded"});
    }
    return status;
}

/* 19.8.5: the body's type-adjusted posture and sweep by the category. */
static condition function_body_condition(sxml_xslt_ast_keyword category, assessment adjusted) {
    const bool is_motionless_body = adjusted.sweep == SXML_STREAM_SWEEP_MOTIONLESS;
    const bool is_consuming_at_most = adjusted.sweep != SXML_STREAM_SWEEP_FREE_RANGING;
    bool holds = true;
    if (category == SXML_XSLT_AST_KEYWORD_ABSORBING) {
        holds = adjusted.posture == SXML_STREAM_POSTURE_GROUNDED && is_consuming_at_most;
    } else if (category == SXML_XSLT_AST_KEYWORD_INSPECTION) {
        holds = adjusted.posture == SXML_STREAM_POSTURE_GROUNDED && is_motionless_body;
    } else if (category == SXML_XSLT_AST_KEYWORD_FILTER) {
        holds = adjusted.posture == SXML_STREAM_POSTURE_STRIDING && is_motionless_body;
    } else if (category == SXML_XSLT_AST_KEYWORD_SHALLOW_DESCENT) {
        holds = adjusted.posture == SXML_STREAM_POSTURE_STRIDING && is_consuming_at_most;
    } else if (category == SXML_XSLT_AST_KEYWORD_DEEP_DESCENT) {
        holds = adjusted.posture == SXML_STREAM_POSTURE_CRAWLING && is_consuming_at_most;
    } else if (category == SXML_XSLT_AST_KEYWORD_ASCENT) {
        holds = (adjusted.posture == SXML_STREAM_POSTURE_CLIMBING || adjusted.posture == SXML_STREAM_POSTURE_GROUNDED) &&
                is_motionless_body;
    }
    return (condition){.holds = holds,
                       .is_uncertain = adjusted.is_uncertain,
                       .reason = "a stylesheet function is not guaranteed-streamable (XSLT 4.0, 19.8.5): its body does not meet the rules of its streamability category"};
}

static sxml_status check_function(machine *state, sxml_xslt_ast_node_index node, verdict *outcome) {
    const sxml_xslt_ast *ast = state->ast;
    const sxml_xslt_ast_keyword category = function_category(ast, node);
    if (category == SXML_XSLT_AST_KEYWORD_UNCLASSIFIED || parameter_count(ast, node) == 0U) {
        return SXML_STATUS_OK;
    }
    if (category != SXML_XSLT_AST_KEYWORD_ABSORBING) {
        /* 19.8.5.3 to 19.8.5.7, the rules for the signature. */
        const declared_type streaming = declared_type_of_node(
            ast, parameter_at(ast, (parameter_query){.node = node, .position = 0}));
        require(outcome, (condition){.holds = !streaming.allows_many || (streaming.type & SXML_U_NODES) == 0U,
                                     .is_uncertain = false,
                                     .reason = "a stylesheet function is not guaranteed-streamable (XSLT 4.0, 19.8.5): its streaming parameter allows more than one node"});
    }
    scope body_scope = empty_scope();
    body_scope.streaming_function = node;
    assessment body;
    const sxml_status status =
        assess(state,
               &(frame_request){.construct = sequence_constructor_construct(ast, node),
                                .focus = {.posture = SXML_STREAM_POSTURE_ROAMING, .type = 0U},
                                .scope = body_scope},
               &body);
    if (status == SXML_STATUS_OK) {
        require(outcome,
                function_body_condition(category, type_adjusted(body, type_determined_usage(
                                                                          declared_type_of_node(ast, node)))));
    }
    return status;
}

/* 10.2.3 and 19.8.6. */
static sxml_status check_attribute_set(machine *state, sxml_xslt_ast_node_index node,
                                       verdict *outcome) {
    const sxml_xslt_ast *ast = state->ast;
    require(outcome, (condition){.holds = are_streamable_attribute_sets(ast, node),
                                 .is_uncertain = false,
                                 .reason = "xsl:attribute-set with streamable=\"yes\" is not guaranteed-streamable (XSLT 4.0, 10.2.3): it uses an attribute set that is not declared streamable"});
    sxml_status status = SXML_STATUS_OK;
    for (sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, node)->first_child;
         status == SXML_STATUS_OK && child.value != 0;
         child = sxml_xslt_ast_get_node(ast, child)->next_sibling) {
        if (!is_kind(ast, child, SXML_XSLT_AST_KIND_ATTRIBUTE)) {
            continue;
        }
        assessment attribute;
        status = assess(state,
                        &(frame_request){.construct = xslt_construct(ast, child),
                                         .focus = striding_focus(SXML_U_ELEMENT),
                                         .scope = empty_scope()},
                        &attribute);
        if (status == SXML_STATUS_OK) {
            require(outcome, (condition){.holds = is_motionless(attribute),
                                         .is_uncertain = attribute.is_uncertain,
                                         .reason = "xsl:attribute-set with streamable=\"yes\" is not guaranteed-streamable (XSLT 4.0, 10.2.3): one of its xsl:attribute instructions is not motionless"});
        }
    }
    return status;
}

/* 18.2.9 "Streamability of Accumulators". */
static sxml_status check_accumulator(machine *state, sxml_xslt_ast_node_index node, verdict *outcome) {
    const sxml_xslt_ast *ast = state->ast;
    sxml_status status = SXML_STATUS_OK;
    /* The values are converted to the accumulator's declared type (19.1),
       so they are type-adjusted by it. */
    const usage value_usage = type_determined_usage(declared_type_of_node(ast, node));
    const sxml_xslt_ast_expression *initial =
        attribute_expression(ast, node, SXML_XSLT_AST_ATTRIBUTE_INITIAL_VALUE);
    if (initial != NULL) {
        assessment value;
        status = assess(state, &(frame_request){.construct = expression_construct(initial),
                                                .focus = striding_focus(SXML_U_DOCUMENT),
                                                .scope = empty_scope()},
                        &value);
        if (status == SXML_STATUS_OK) {
            value = type_adjusted(value, value_usage);
            require(outcome, (condition){.holds = is_grounded_motionless(value),
                                         .is_uncertain = value.is_uncertain,
                                         .reason = "xsl:accumulator with streamable=\"yes\" is not guaranteed-streamable (XSLT 4.0, 18.2.9): its initial value is not grounded and motionless"});
        }
    }
    for (sxml_xslt_ast_node_index accumulator_rule = sxml_xslt_ast_get_node(ast, node)->first_child;
         status == SXML_STATUS_OK && accumulator_rule.value != 0;
         accumulator_rule = sxml_xslt_ast_get_node(ast, accumulator_rule)->next_sibling) {
        if (!is_kind(ast, accumulator_rule, SXML_XSLT_AST_KIND_ACCUMULATOR_RULE)) {
            continue;
        }
        const sxml_xslt_ast_expression *match =
            attribute_expression(ast, accumulator_rule, SXML_XSLT_AST_ATTRIBUTE_MATCH);
        u_type match_type = SXML_U_ANY;
        if (match != NULL) {
            assessment pattern;
            status = assess_pattern(state, match, &pattern);
            if (status != SXML_STATUS_OK) {
                break;
            }
            require(outcome, (condition){.holds = is_motionless(pattern),
                                         .is_uncertain = pattern.is_uncertain,
                                         .reason = "xsl:accumulator with streamable=\"yes\" is not guaranteed-streamable (XSLT 4.0, 18.2.9): the match pattern of a accumulator_rule is not motionless"});
            match_type = pattern_match_type(match->ast);
        }
        scope rule_scope = empty_scope();
        const sxml_xslt_ast_keyword phase = keyword_attribute(ast, accumulator_rule, SXML_XSLT_AST_ATTRIBUTE_PHASE);
        rule_scope.accumulator_phase = phase == SXML_XSLT_AST_KEYWORD_END ? SXML_XSLT_AST_KEYWORD_END
                                                                         : SXML_XSLT_AST_KEYWORD_START;
        /* A capturing accumulator_rule (19.9, 4.0) reads a snapshot of the node:
           grounded. */
        const bool captures = is_true_attribute(ast, accumulator_rule, SXML_XSLT_AST_ATTRIBUTE_CAPTURE);
        assessment value;
        status = assess(state,
                        &(frame_request){.construct = value_operand(ast, accumulator_rule, SXML_STREAM_USAGE_TRANSMISSION).construct,
                                         .focus = {.posture = captures ? SXML_STREAM_POSTURE_GROUNDED : SXML_STREAM_POSTURE_STRIDING,
                                                   .type = match_type},
                                         .scope = rule_scope},
                        &value);
        if (status == SXML_STATUS_OK) {
            value = type_adjusted(value, value_usage);
            require(outcome, (condition){.holds = is_grounded_motionless(value),
                                         .is_uncertain = value.is_uncertain,
                                         .reason = "xsl:accumulator with streamable=\"yes\" is not guaranteed-streamable (XSLT 4.0, 18.2.9): the value of a accumulator_rule is not grounded and motionless"});
        }
    }
    return status;
}

/* 15.4 "Streamable Merging". */
static sxml_status check_merge_source(machine *state, sxml_xslt_ast_node_index node,
                                      verdict *outcome) {
    const sxml_xslt_ast *ast = state->ast;
    require(outcome,
            (condition){.holds = find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_SOURCE) != NULL,
                        .is_uncertain = false,
                        .reason = "xsl:merge-source with streamable=\"yes\" is not guaranteed-streamable (XSLT 4.0, 15.4): it has no for-each-source attribute"});
    require(outcome,
            (condition){.holds = !is_true_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_SORT_BEFORE_MERGE),
                        .is_uncertain = false,
                        .reason = "xsl:merge-source with streamable=\"yes\" is not guaranteed-streamable (XSLT 4.0, 15.4): it sorts before merging"});
    const sxml_xslt_ast_expression *select =
        attribute_expression(ast, node, SXML_XSLT_AST_ATTRIBUTE_SELECT);
    if (select == NULL) {
        return SXML_STATUS_OK;
    }
    assessment value;
    const sxml_status status = assess(state, &(frame_request){.construct = expression_construct(select),
                                                              .focus = striding_focus(SXML_U_DOCUMENT),
                                                              .scope = empty_scope()},
                                      &value);
    if (status == SXML_STATUS_OK) {
        require(outcome,
                (condition){.holds = (value.posture == SXML_STREAM_POSTURE_STRIDING ||
                                      value.posture == SXML_STREAM_POSTURE_GROUNDED) &&
                                     value.sweep != SXML_STREAM_SWEEP_FREE_RANGING,
                            .is_uncertain = value.is_uncertain,
                            .reason = "xsl:merge-source with streamable=\"yes\" is not guaranteed-streamable (XSLT 4.0, 15.4): its select expression is not striding or grounded"});
    }
    return status;
}

/* Writes XTSE3430 for node, with the verdict's reason (a literal), into
   the error record. */
typedef struct error_report {
    sxml_xslt_ast_node_index node;
    verdict outcome;
} error_report;

static sxml_status report_not_streamable(const sxml_xslt_streamability_request *request,
                                         const error_report *report) {
    if (request->error_record == NULL) {
        return SXML_STATUS_STATIC_ERROR;
    }
    const char *reason = report->outcome.reason != NULL
                             ? report->outcome.reason
                             : "a construct declared streamable is not guaranteed-streamable "
                               "(XSLT 4.0, 19.10)";
    *request->error_record = (sxml_error_record){
        .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("XTSE3430"),
                 .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
        .value = NULL,
        .diagnostic = {.description = {.bytes = (const uint8_t *)reason, .byte_count = strlen(reason)},
                       .location = sxml_xslt_ast_get_source_location(request->ast, report->node)}};
    return SXML_STATUS_STATIC_ERROR;
}

/* The checks of one node: whether it is declared streamable, and its
   verdict. */
typedef struct node_check {
    verdict outcome;
    bool is_declared;
} node_check;

static sxml_status check_node(machine *state, sxml_xslt_ast_node_index node, node_check *check) {
    verdict *outcome = &check->outcome;
    bool *is_declared = &check->is_declared;
    const sxml_xslt_ast *ast = state->ast;
    *outcome = guaranteed();
    *is_declared = true;
    const sxml_xslt_ast_kind kind = sxml_xslt_ast_get_node(ast, node)->kind;
    if (kind == SXML_XSLT_AST_KIND_TEMPLATE &&
        find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_MATCH) != NULL &&
        is_in_streamable_mode(ast, node)) {
        return check_template_rule(state, node, outcome);
    }
    if (kind == SXML_XSLT_AST_KIND_FUNCTION &&
        function_category(ast, node) != SXML_XSLT_AST_KEYWORD_UNCLASSIFIED) {
        return check_function(state, node, outcome);
    }
    if (is_true_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_STREAMABLE)) {
        if (kind == SXML_XSLT_AST_KIND_SOURCE_DOCUMENT) {
            return check_source_document(state, node, outcome);
        }
        if (kind == SXML_XSLT_AST_KIND_ATTRIBUTE_SET) {
            return check_attribute_set(state, node, outcome);
        }
        if (kind == SXML_XSLT_AST_KIND_ACCUMULATOR) {
            return check_accumulator(state, node, outcome);
        }
        if (kind == SXML_XSLT_AST_KIND_MERGE_SOURCE) {
            return check_merge_source(state, node, outcome);
        }
    }
    *is_declared = false;
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_check_streamability(const sxml_xslt_streamability_request *request) {
    if (request == NULL || request->ast == NULL || !request->ast->is_sealed ||
        request->name_pool == NULL || sxml_allocator_is_null(request->allocator)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    machine state = {.ast = request->ast,
                     .lookup = {.ast = request->ast, .name_pool = request->name_pool},
                     .allocator = request->allocator,
                     .frames = NULL,
                     .frame_count = 0,
                     .frame_capacity = 0,
                     .maximum_frame_count = request->maximum_frame_count == 0U
                                                ? 100000U
                                                : request->maximum_frame_count,
                     .operands = NULL,
                     .operand_count = 0,
                     .operand_capacity = 0,
                     .is_limit_reached = false};
    sxml_status status = SXML_STATUS_OK;
    sxml_xslt_ast_node_index failed = no_xslt_node;
    verdict failure = guaranteed();
    for (uint32_t index = 1; status == SXML_STATUS_OK && failed.value == 0 &&
                             index < request->ast->node_count;
         index += 1U) {
        node_check check = {.outcome = guaranteed(), .is_declared = false};
        status = check_node(&state, node_at(index), &check);
        if (status == SXML_STATUS_OK && check.is_declared && !check.outcome.is_guaranteed &&
            !check.outcome.is_uncertain) {
            failed = node_at(index);
            failure = check.outcome;
        }
    }
    sxml_allocator_release(request->allocator, state.frames);
    sxml_allocator_release(request->allocator, state.operands);
    if (status != SXML_STATUS_OK || failed.value == 0) {
        return status;
    }
    return report_not_streamable(request, &(error_report){.node = failed, .outcome = failure});
}

/* ------------------------------------------------- Sweeps for lowering */

sxml_status sxml_xslt_assess_sweep(const sxml_xslt_sweep_query *query,
                                   sxml_xslt_stream_sweep *found) {
    *found = SXML_XSLT_STREAM_SWEEP_UNCERTAIN;
    if (query == NULL || query->ast == NULL || !query->ast->is_sealed ||
        query->name_pool == NULL || sxml_allocator_is_null(query->allocator)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    machine state = {.ast = query->ast,
                     .lookup = {.ast = query->ast, .name_pool = query->name_pool},
                     .allocator = query->allocator,
                     .frames = NULL,
                     .frame_count = 0,
                     .frame_capacity = 0,
                     .maximum_frame_count = 100000U,
                     .operands = NULL,
                     .operand_count = 0,
                     .operand_capacity = 0,
                     .is_limit_reached = false};
    /* The stacks are made before the machine runs. */
    void *operands = NULL;
    sxml_status status = grow(query->allocator, &(growth){.elements = &operands,
                                                         .capacity = &state.operand_capacity,
                                                         .count = 0,
                                                         .element_size = sizeof(operand),
                                                         .element_alignment = alignof(operand)});
    state.operands = operands;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    construct assessed = query->assesses_content
                             ? sequence_constructor_construct(query->ast, query->instruction)
                             : xslt_construct(query->ast, query->instruction);
    if (query->expression_ast != NULL) {
        assessed = xpath_construct(query->expression_ast, query->expression_node,
                                   query->static_context);
    }
    const u_type context_type = query->is_document_focus ? SXML_U_DOCUMENT : SXML_U_ELEMENT;
    scope start_scope = empty_scope();
    if (query->follows_consuming) {
        start_scope.is_in_sequence_constructor = true;
        start_scope.follows_consuming = true;
    }
    assessment result;
    status = assess(&state,
                    &(frame_request){.construct = assessed,
                                     .focus = striding_focus(context_type),
                                     .scope = start_scope},
                    &result);
    sxml_allocator_release(query->allocator, state.frames);
    sxml_allocator_release(query->allocator, state.operands);
    if (status != SXML_STATUS_OK || result.is_uncertain) {
        return status;
    }
    if (is_free(result)) {
        *found = SXML_XSLT_STREAM_SWEEP_FREE_RANGING;
    } else if (result.sweep == SXML_STREAM_SWEEP_CONSUMING ||
               (query->requires_grounded && result.posture != SXML_STREAM_POSTURE_GROUNDED)) {
        *found = SXML_XSLT_STREAM_SWEEP_CONSUMING;
    } else {
        *found = SXML_XSLT_STREAM_SWEEP_MOTIONLESS;
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------ Accumulators for lowering */

sxml_status sxml_xslt_assess_accumulators(const sxml_xslt_accumulator_query *query,
                                          bool *is_guaranteed) {
    *is_guaranteed = false;
    if (query == NULL || query->ast == NULL || !query->ast->is_sealed ||
        query->name_pool == NULL || sxml_allocator_is_null(query->allocator)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_xslt_ast *ast = query->ast;
    machine state = {.ast = ast,
                     .lookup = {.ast = ast, .name_pool = query->name_pool},
                     .allocator = query->allocator,
                     .frames = NULL,
                     .frame_count = 0,
                     .frame_capacity = 0,
                     .maximum_frame_count = 100000U,
                     .operands = NULL,
                     .operand_count = 0,
                     .operand_capacity = 0,
                     .is_limit_reached = false};
    verdict outcome = guaranteed();
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t index = 1; status == SXML_STATUS_OK && outcome.is_guaranteed &&
                             !outcome.is_uncertain && index < ast->node_count;
         index += 1U) {
        const sxml_xslt_ast_node_index node = node_at(index);
        if (is_kind(ast, node, SXML_XSLT_AST_KIND_ACCUMULATOR) &&
            is_true_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_STREAMABLE)) {
            status = check_accumulator(&state, node, &outcome);
        }
    }
    sxml_allocator_release(query->allocator, state.frames);
    sxml_allocator_release(query->allocator, state.operands);
    *is_guaranteed = status == SXML_STATUS_OK && outcome.is_guaranteed && !outcome.is_uncertain;
    return status;
}

/* ------------------------------------------------- Modes for lowering */

/* The mode an xsl:apply-templates uses (its mode attribute, #default
   resolved); false for #current, #all or a list, which name no one mode
   before it runs. */
static bool instruction_mode(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node,
                             mode_reference *mode) {
    sxml_xslt_ast_name_token token = {.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT,
                                      .name = {.index = 0},
                                      .namespace_uri = {.index = 0},
                                      .has_arity = false,
                                      .arity = 0};
    const sxml_xslt_ast_attribute *attribute =
        find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_MODE);
    if (attribute != NULL && attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
        const sxml_xslt_ast_name_token_view tokens =
            sxml_xslt_ast_get_name_tokens(ast, attribute->value.payload.name_tokens);
        if (tokens.name_token_count != 1) {
            return false;
        }
        token = tokens.name_tokens[0];
    }
    *mode = resolve_mode(ast, node, token);
    return mode->kind == SXML_XSLT_AST_NAME_TOKEN_KIND_NAME ||
           mode->kind == SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED;
}

/* True when a template rule is applicable to mode (6.6.4, 6.9.6): its
   mode list names it or is #all, or it is inside that xsl:mode. */
static bool is_applicable_to_mode(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node,
                                  mode_reference mode) {
    const sxml_xslt_ast_node_index parent = sxml_xslt_ast_get_node(ast, node)->parent;
    if (is_kind(ast, parent, SXML_XSLT_AST_KIND_MODE)) {
        const sxml_name_identifier name = name_attribute(ast, parent);
        return mode.kind == SXML_XSLT_AST_NAME_TOKEN_KIND_NAME ? name.index == mode.name.index
                                                               : name.index == 0;
    }
    const sxml_xslt_ast_attribute *attribute =
        find_xslt_attribute(ast, node, SXML_XSLT_AST_ATTRIBUTE_MODE);
    sxml_xslt_ast_name_token default_token = {.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT,
                                              .name = {.index = 0},
                                              .namespace_uri = {.index = 0},
                                              .has_arity = false,
                                              .arity = 0};
    sxml_xslt_ast_name_token_view tokens = {.name_tokens = &default_token, .name_token_count = 1};
    if (attribute != NULL && attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
        tokens = sxml_xslt_ast_get_name_tokens(ast, attribute->value.payload.name_tokens);
    }
    for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
        const sxml_xslt_ast_name_token token = tokens.name_tokens[token_index];
        if (token.kind == SXML_XSLT_AST_NAME_TOKEN_KIND_ALL) {
            return true;
        }
        const mode_reference named = resolve_mode(ast, node, token);
        if (named.kind == mode.kind &&
            (named.kind != SXML_XSLT_AST_NAME_TOKEN_KIND_NAME ||
             named.name.index == mode.name.index)) {
            return true;
        }
    }
    return false;
}

bool sxml_xslt_applies_in_mode(const sxml_xslt_ast *ast, sxml_xslt_mode_pair pair) {
    mode_reference mode = {.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED, .name = {.index = 0}};
    return ast != NULL && is_kind(ast, pair.template_rule, SXML_XSLT_AST_KIND_TEMPLATE) &&
           find_xslt_attribute(ast, pair.template_rule, SXML_XSLT_AST_ATTRIBUTE_MATCH) != NULL &&
           instruction_mode(ast, pair.instruction, &mode) &&
           is_applicable_to_mode(ast, pair.template_rule, mode);
}

sxml_status sxml_xslt_assess_mode(const sxml_xslt_mode_query *query, bool *is_guaranteed) {
    *is_guaranteed = false;
    if (query == NULL || query->ast == NULL || !query->ast->is_sealed ||
        query->name_pool == NULL || sxml_allocator_is_null(query->allocator)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_xslt_ast *ast = query->ast;
    mode_reference mode = {.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED, .name = {.index = 0}};
    if (query->instruction.value == 0) {
        /* A mode by name (D228); #default against the principal module
           (2.4.3). */
        const sxml_xslt_ast_name_token token = {
            .kind = query->is_default_mode ? SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT
                    : query->mode_name.index != 0 ? SXML_XSLT_AST_NAME_TOKEN_KIND_NAME
                                                  : SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED,
            .name = query->mode_name,
            .namespace_uri = {.index = 0},
            .has_arity = false,
            .arity = 0};
        mode = resolve_mode(
            ast, sxml_xslt_ast_get_module(ast, (sxml_xslt_ast_module_index){.value = 1})->root,
            token);
    } else if (!instruction_mode(ast, query->instruction, &mode)) {
        return SXML_STATUS_OK;
    }
    if (!is_streamable_mode(ast, mode)) {
        return SXML_STATUS_OK;
    }
    machine state = {.ast = ast,
                     .lookup = {.ast = ast, .name_pool = query->name_pool},
                     .allocator = query->allocator,
                     .frames = NULL,
                     .frame_count = 0,
                     .frame_capacity = 0,
                     .maximum_frame_count = 100000U,
                     .operands = NULL,
                     .operand_count = 0,
                     .operand_capacity = 0,
                     .is_limit_reached = false};
    /* Every template rule of the mode is guaranteed-streamable (6.6.4),
       none uncertain. */
    verdict outcome = guaranteed();
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t index = 1; status == SXML_STATUS_OK && outcome.is_guaranteed &&
                             !outcome.is_uncertain && index < ast->node_count;
         index += 1U) {
        const sxml_xslt_ast_node_index template_rule = node_at(index);
        if (is_kind(ast, template_rule, SXML_XSLT_AST_KIND_TEMPLATE) &&
            find_xslt_attribute(ast, template_rule, SXML_XSLT_AST_ATTRIBUTE_MATCH) != NULL &&
            is_applicable_to_mode(ast, template_rule, mode)) {
            status = check_template_rule(&state, template_rule, &outcome);
        }
    }
    sxml_allocator_release(query->allocator, state.frames);
    sxml_allocator_release(query->allocator, state.operands);
    *is_guaranteed = status == SXML_STATUS_OK && outcome.is_guaranteed && !outcome.is_uncertain;
    return status;
}
